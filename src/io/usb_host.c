#include "usb.h"

#include "fd.h"
#include "logging.h"
#include "product.h"
#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/usb/ch9.h>
#include <linux/usbdevice_fs.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define LOG(...) pik_log("usb", __VA_ARGS__)

#define USBFS_ROOT "/dev/bus/usb"
#define SYS_USB_DEVICES "/sys/bus/usb/devices"
#define USB_IO_CHUNK PIK_USB_HS_MAX_PACKET
/* usbfs epoll is the primary completion signal; also reap on a bounded
 * interval as a progress sanity check. */
#define USB_POLL_MS 10
#define USB_EPOLL_EVENTS (EPOLLIN | EPOLLOUT | EPOLLPRI)
/* Four RX URBs keep the device side fed; one TX URB deliberately limits host
 * side buffering so MCU/control frames can overtake tunnel backlog quickly. */
#define RX_URBS 4u
#define TX_URBS 1u

typedef struct {
    struct usbdevfs_urb urb;
    uint8_t buf[USB_IO_CHUNK];
    uint32_t len;
    bool submitted;
} bulk_urb_t;

static struct {
    pik_link_t *lk;
    int epfd;
    int fd;
    int iface;
    uint8_t ep_in;
    uint8_t ep_out;
    uint16_t out_maxpacket;
    bool zero_packet;
    bulk_urb_t rx[RX_URBS];
    bulk_urb_t tx[TX_URBS];
} g_usb = {
    .epfd = -1,
    .fd = -1,
    .iface = -1,
};

static int g_usb_tag;

static bool parse_vidpid(const char *s, uint16_t *vid, uint16_t *pid) {
    unsigned v, p;
    char extra;
    if (sscanf(s, "%x:%x%c", &v, &p, &extra) != 2) return false;
    if (v > 0xffffu || p > 0xffffu) return false;
    *vid = (uint16_t)v;
    *pid = (uint16_t)p;
    return true;
}

static bool read_text_file(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    ssize_t n = read(fd, buf, cap - 1);
    close(fd);
    if (n <= 0) return false;
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) n--;
    buf[n] = '\0';
    return true;
}

static bool read_hex_u16(const char *path, uint16_t *out) {
    char buf[32];
    char *end = NULL;
    if (!read_text_file(path, buf, sizeof(buf))) return false;
    errno = 0;
    unsigned long v = strtoul(buf, &end, 16);
    if (errno || !end || *end || v > 0xfffful) return false;
    *out = (uint16_t)v;
    return true;
}

static bool read_dec_uint(const char *path, unsigned *out) {
    char buf[32];
    char *end = NULL;
    if (!read_text_file(path, buf, sizeof(buf))) return false;
    errno = 0;
    unsigned long v = strtoul(buf, &end, 10);
    if (errno || !end || *end || v > 999ul) return false;
    *out = (unsigned)v;
    return true;
}

static bool read_descriptors(const char *sys_name, uint8_t *buf, size_t cap, size_t *len) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s/descriptors", SYS_USB_DEVICES, sys_name);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;

    size_t off = 0;
    while (off < cap) {
        ssize_t n = read(fd, buf + off, cap - off);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            close(fd);
            return false;
        }
        if (n == 0) break;
        off += (size_t)n;
    }
    close(fd);
    *len = off;
    return off > 0;
}

static uint16_t get_u16le(const uint8_t *p) {
    return (uint16_t)p[0] | (uint16_t)(p[1] << 8);
}

static bool find_vendor_bulk_iface(const uint8_t *d, size_t len,
                                   int *iface, uint8_t *ep_in, uint8_t *ep_out,
                                   uint16_t *out_maxpacket) {
    int cur_iface = -1;
    bool vendor_iface = false;
    uint8_t in = 0, out = 0;
    uint16_t out_mps = 0;

    for (size_t off = 0; off + 2 <= len;) {
        uint8_t blen = d[off];
        uint8_t dtype = d[off + 1];
        if (blen < 2 || off + blen > len) break;

        if (dtype == USB_DT_INTERFACE && blen >= USB_DT_INTERFACE_SIZE) {
            if (vendor_iface && in && out) {
                *iface = cur_iface;
                *ep_in = in;
                *ep_out = out;
                *out_maxpacket = out_mps;
                return true;
            }
            cur_iface = d[off + 2];
            vendor_iface = d[off + 5] == 0xff;
            in = out = 0;
            out_mps = 0;
        } else if (dtype == USB_DT_ENDPOINT && blen >= USB_DT_ENDPOINT_SIZE &&
                   vendor_iface) {
            uint8_t attrs = d[off + 3] & USB_ENDPOINT_XFERTYPE_MASK;
            uint8_t addr = d[off + 2];
            if (attrs == USB_ENDPOINT_XFER_BULK) {
                if (addr & USB_DIR_IN)
                    in = addr;
                else {
                    out = addr;
                    out_mps = get_u16le(d + off + 4) & 0x07ffu;
                }
            }
        }
        off += blen;
    }

    if (vendor_iface && in && out) {
        *iface = cur_iface;
        *ep_in = in;
        *ep_out = out;
        *out_maxpacket = out_mps;
        return true;
    }
    return false;
}

static bool open_matching_device(const char *vidpid, int *fd_out,
                                 int *iface, uint8_t *ep_in, uint8_t *ep_out,
                                 uint16_t *out_maxpacket) {
    uint16_t want_vid, want_pid;
    if (!parse_vidpid(vidpid, &want_vid, &want_pid)) {
        LOG("bad VID:PID: %s", vidpid);
        return false;
    }

    DIR *d = opendir(SYS_USB_DEVICES);
    if (!d) {
        LOG("opendir %s: %s", SYS_USB_DEVICES, strerror(errno));
        return false;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char path[512];
        uint16_t vid, pid;
        snprintf(path, sizeof(path), "%s/%s/idVendor", SYS_USB_DEVICES, ent->d_name);
        if (!read_hex_u16(path, &vid) || vid != want_vid) continue;
        snprintf(path, sizeof(path), "%s/%s/idProduct", SYS_USB_DEVICES, ent->d_name);
        if (!read_hex_u16(path, &pid) || pid != want_pid) continue;

        uint8_t desc[4096];
        size_t desc_len = 0;
        int found_iface = -1;
        uint8_t found_in = 0, found_out = 0;
        uint16_t found_out_mps = 0;
        if (!read_descriptors(ent->d_name, desc, sizeof(desc), &desc_len) ||
            !find_vendor_bulk_iface(desc, desc_len, &found_iface, &found_in,
                                    &found_out, &found_out_mps))
            continue;

        unsigned busnum, devnum;
        snprintf(path, sizeof(path), "%s/%s/busnum", SYS_USB_DEVICES, ent->d_name);
        if (!read_dec_uint(path, &busnum)) continue;
        snprintf(path, sizeof(path), "%s/%s/devnum", SYS_USB_DEVICES, ent->d_name);
        if (!read_dec_uint(path, &devnum)) continue;

        char devpath[128];
        snprintf(devpath, sizeof(devpath), USBFS_ROOT "/%03u/%03u", busnum, devnum);
        int fd = open(devpath, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            LOG("open %s: %s", devpath, strerror(errno));
            continue;
        }

        *fd_out = fd;
        *iface = found_iface;
        *ep_in = found_in;
        *ep_out = found_out;
        *out_maxpacket = found_out_mps;
        closedir(d);
        LOG("opened %s iface=%d in=0x%02x out=0x%02x out_mps=%u", devpath,
            found_iface, found_in, found_out, found_out_mps);
        return true;
    }

    closedir(d);
    return false;
}

static bool clear_halt(uint8_t ep) {
    unsigned int e = ep;
    if (ioctl(g_usb.fd, USBDEVFS_CLEAR_HALT, &e) < 0) {
        LOG("clear halt ep=0x%02x: %s", ep, strerror(errno));
        return false;
    }
    return true;
}

static const char *urb_status_name(int status) {
    switch (-status) {
    case 0: return "OK";
    case EPROTO: return "EPROTO";
    case EILSEQ: return "EILSEQ";
    case ETIMEDOUT: return "ETIMEDOUT";
    case ECONNRESET: return "ECONNRESET";
    case ENOENT: return "ENOENT";
    case ENODEV: return "ENODEV";
    case ESHUTDOWN: return "ESHUTDOWN";
    case EPIPE: return "EPIPE";
    default: return "error";
    }
}

static void fail_usb(void) {
    pik_link_fail(g_usb.lk);
}

static bool submit_rx(bulk_urb_t *u) {
    memset(&u->urb, 0, sizeof(u->urb));
    u->urb.type = USBDEVFS_URB_TYPE_BULK;
    u->urb.endpoint = g_usb.ep_in;
    u->urb.buffer = u->buf;
    u->urb.buffer_length = (int)sizeof(u->buf);
    u->urb.usercontext = u;
    if (ioctl(g_usb.fd, USBDEVFS_SUBMITURB, &u->urb) < 0) {
        LOG("submit RX URB ep=0x%02x len=%zu: %s",
            g_usb.ep_in, sizeof(u->buf), strerror(errno));
        if (errno == ENODEV || errno == ESHUTDOWN || errno == EPIPE)
            fail_usb();
        return false;
    }
    u->len = 0;
    u->submitted = true;
    return true;
}

static bool submit_tx(bulk_urb_t *u, const uint8_t *p, uint32_t len) {
    if (len > sizeof(u->buf)) len = sizeof(u->buf);
    if (len > PIK_USB_TX_CHUNK) len = PIK_USB_TX_CHUNK;
    memcpy(u->buf, p, len);
    memset(&u->urb, 0, sizeof(u->urb));
    u->urb.type = USBDEVFS_URB_TYPE_BULK;
    u->urb.endpoint = g_usb.ep_out;
    if (g_usb.zero_packet && g_usb.out_maxpacket &&
        (len % g_usb.out_maxpacket) == 0)
        u->urb.flags |= USBDEVFS_URB_ZERO_PACKET;
    u->urb.buffer = u->buf;
    u->urb.buffer_length = (int)len;
    u->urb.usercontext = u;
    if (ioctl(g_usb.fd, USBDEVFS_SUBMITURB, &u->urb) < 0) {
        LOG("submit TX URB ep=0x%02x len=%u: %s",
            g_usb.ep_out, len, strerror(errno));
        if (errno == ENODEV || errno == ESHUTDOWN || errno == EPIPE)
            fail_usb();
        return false;
    }
    u->len = len;
    u->submitted = true;
    return true;
}

static bool drain_tx(void) {
    if (!pik_link_is_open(g_usb.lk)) return false;
    for (unsigned i = 0; i < TX_URBS; i++) {
        if (g_usb.tx[i].submitted) continue;
        uint32_t len = 0;
        const uint8_t *p = pik_link_tx_peek(g_usb.lk, &len);
        if (!p || !len) return true;
        if (!submit_tx(&g_usb.tx[i], p, len))
            return false;
    }
    return true;
}

static bool handle_urb_done(struct usbdevfs_urb *urb, int64_t now) {
    bulk_urb_t *u = urb->usercontext;
    u->submitted = false;

    if (urb->status == -EPIPE) {
        (void)clear_halt(urb->endpoint);
        fail_usb();
        return false;
    }
    if (urb->status < 0) {
        LOG("URB ep=0x%02x status=%d (%s) actual=%d requested=%d",
            urb->endpoint, urb->status, urb_status_name(urb->status),
            urb->actual_length, urb->buffer_length);
        fail_usb();
        return false;
    }

    if (urb->endpoint & USB_DIR_IN) {
        if (urb->actual_length < 0 ||
            (size_t)urb->actual_length > sizeof(u->buf)) {
            LOG("invalid RX completion: actual=%d capacity=%zu",
                urb->actual_length, sizeof(u->buf));
            fail_usb();
            return false;
        }
        if (urb->actual_length > 0 &&
            !pik_link_feed(g_usb.lk, u->buf, (size_t)urb->actual_length, now))
            return false;
        if (pik_link_is_open(g_usb.lk) && !submit_rx(u))
            return false;
    } else {
        if (urb->actual_length <= 0 ||
            (uint32_t)urb->actual_length > u->len) {
            LOG("invalid TX completion: actual=%d requested=%u",
                urb->actual_length, u->len);
            fail_usb();
            return false;
        }
        pik_link_tx_consume(g_usb.lk, (uint32_t)urb->actual_length, now);
        u->len = 0;
    }

    return drain_tx();
}

static bool reap_completed(int64_t now) {
    while (g_usb.fd >= 0) {
        struct usbdevfs_urb *urb = NULL;
        if (ioctl(g_usb.fd, USBDEVFS_REAPURBNDELAY, &urb) < 0) {
            if (errno == EAGAIN)
                return drain_tx();
            if (errno == EINTR) continue;
            LOG("reap URB: %s", strerror(errno));
            fail_usb();
            return false;
        }
        if (urb) {
            if (!handle_urb_done(urb, now))
                return false;
        }
    }
    return false;
}

bool pik_usb_host_start(pik_link_t *lk, int epfd, int64_t now) {
    pik_usb_host_cleanup();
    g_usb.lk = lk;
    g_usb.epfd = epfd;

    if (!open_matching_device(PIK1_USB_VIDPID, &g_usb.fd, &g_usb.iface,
                              &g_usb.ep_in, &g_usb.ep_out,
                              &g_usb.out_maxpacket))
        return false;

    if (ioctl(g_usb.fd, USBDEVFS_CLAIMINTERFACE, &g_usb.iface) < 0) {
        LOG("claim interface %d: %s", g_usb.iface, strerror(errno));
        pik_usb_host_cleanup();
        return false;
    }

    unsigned int caps = 0;
    if (ioctl(g_usb.fd, USBDEVFS_GET_CAPABILITIES, &caps) == 0)
        g_usb.zero_packet = (caps & USBDEVFS_CAP_ZERO_PACKET) != 0;
    else
        LOG("get usbfs capabilities: %s", strerror(errno));
    LOG("usbfs caps=0x%x zero_packet=%s", caps,
        g_usb.zero_packet ? "yes" : "no");

    if (!pik_epoll_set(epfd, g_usb.fd, USB_EPOLL_EVENTS, &g_usb_tag)) {
        LOG("epoll add usbfs fd: %s", strerror(errno));
        pik_usb_host_cleanup();
        return false;
    }
    pik_link_begin(lk, now);
    for (unsigned i = 0; i < RX_URBS; i++) {
        if (!submit_rx(&g_usb.rx[i])) {
            pik_usb_host_cleanup();
            return false;
        }
    }
    LOG("bulk transport ready: vidpid=%s", PIK1_USB_VIDPID);
    return true;
}

bool pik_usb_host_owns_event(const void *ptr) {
    return ptr == &g_usb_tag;
}

bool pik_usb_host_dispatch(void *ptr, uint32_t events, int64_t now) {
    (void)ptr;
    if (events & (EPOLLERR | EPOLLHUP)) {
        LOG("usbfs event failure: events=0x%x", events);
        fail_usb();
        return false;
    }
    return reap_completed(now);
}

bool pik_usb_host_tick(int64_t now) {
    if (g_usb.fd < 0 || !pik_link_is_open(g_usb.lk)) return true;
    return reap_completed(now);
}

int64_t pik_usb_host_deadline(void) {
    if (g_usb.fd < 0 || !pik_link_is_open(g_usb.lk)) return INT64_MAX;
    return pik_now_ms() + USB_POLL_MS;
}

void pik_usb_host_cleanup(void) {
    if (g_usb.fd >= 0) {
        for (unsigned i = 0; i < RX_URBS; i++) {
            if (g_usb.rx[i].submitted)
                ioctl(g_usb.fd, USBDEVFS_DISCARDURB, &g_usb.rx[i].urb);
            g_usb.rx[i].submitted = false;
            g_usb.rx[i].len = 0;
        }
        for (unsigned i = 0; i < TX_URBS; i++) {
            if (g_usb.tx[i].submitted)
                ioctl(g_usb.fd, USBDEVFS_DISCARDURB, &g_usb.tx[i].urb);
            g_usb.tx[i].submitted = false;
            g_usb.tx[i].len = 0;
        }
        if (g_usb.epfd >= 0)
            pik_epoll_del(g_usb.epfd, g_usb.fd);
        if (g_usb.iface >= 0)
            ioctl(g_usb.fd, USBDEVFS_RELEASEINTERFACE, &g_usb.iface);
        close(g_usb.fd);
    }
    g_usb.lk = NULL;
    g_usb.epfd = -1;
    g_usb.fd = -1;
    g_usb.iface = -1;
    g_usb.ep_in = 0;
    g_usb.ep_out = 0;
    g_usb.out_maxpacket = 0;
    g_usb.zero_packet = false;
}
