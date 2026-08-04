#include "usb.h"

#include "fd.h"
#include "logging.h"
#include "product.h"
#include "util.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/aio_abi.h>
#include <linux/usb/ch9.h>
#include <linux/usb/functionfs.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <sys/syscall.h>
#include <unistd.h>

#define LOG(...) pik_log("ffs", __VA_ARGS__)

/* FunctionFS AIO completions signal through eventfd; also drain on a bounded
 * interval as a progress sanity check. */
#define FFS_RX_CHUNK 16384u
#define FFS_POLL_MS 10
/* Multiple OUT reads keep the gadget ready for host bursts. IN stays single
 * slot to match the one-packet TX chunk and avoid hiding priority decisions
 * inside the gadget queue. */
#define FFS_RX_SLOTS 4u
#define FFS_TX_SLOTS 1u

typedef enum {
    FFS_AIO_RX,
    FFS_AIO_TX,
} ffs_aio_dir_t;

typedef struct {
    struct iocb cb;
    ffs_aio_dir_t dir;
    bool busy;
    uint32_t len;
    uint8_t buf[FFS_RX_CHUNK];
} ffs_aio_req_t;

static struct {
    pik_link_t *lk;
    int epfd;
    int ep0;
    int out_fd;
    int in_fd;
    int event_fd;
    aio_context_t aio;
    bool enabled;
    ffs_aio_req_t rx[FFS_RX_SLOTS];
    ffs_aio_req_t tx[FFS_TX_SLOTS];
} g_ffs = {
    .epfd = -1,
    .ep0 = -1,
    .out_fd = -1,
    .in_fd = -1,
    .event_fd = -1,
};

static int g_ep0_tag;
static int g_aio_tag;

static int aio_setup(unsigned nr, aio_context_t *ctx) {
    return (int)syscall(SYS_io_setup, nr, ctx);
}

static int aio_destroy(aio_context_t ctx) {
    return (int)syscall(SYS_io_destroy, ctx);
}

static int aio_submit(aio_context_t ctx, long nr, struct iocb **iocbpp) {
    return (int)syscall(SYS_io_submit, ctx, nr, iocbpp);
}

static int aio_getevents(aio_context_t ctx, long min_nr, long nr,
                         struct io_event *events) {
    return (int)syscall(SYS_io_getevents, ctx, min_nr, nr, events, NULL);
}

static void put_u16le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static bool write_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = buf;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) {
            errno = EIO;
            return false;
        }
        p += (size_t)n;
        len -= (size_t)n;
    }
    return true;
}

static bool read_discard(int fd, size_t len) {
    uint8_t buf[256];
    while (len) {
        size_t want = len < sizeof(buf) ? len : sizeof(buf);
        ssize_t n = read(fd, buf, want);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) {
            errno = EIO;
            return false;
        }
        len -= (size_t)n;
    }
    return true;
}

static void iface_desc(uint8_t *p) {
    p[0] = USB_DT_INTERFACE_SIZE;
    p[1] = USB_DT_INTERFACE;
    p[2] = 0;       /* bInterfaceNumber: rewritten by FunctionFS */
    p[3] = 0;       /* bAlternateSetting */
    p[4] = 2;       /* bNumEndpoints */
    p[5] = 0xff;    /* vendor class */
    p[6] = 0x50;
    p[7] = 0x31;
    p[8] = 1;       /* iInterface */
}

static void ep_desc(uint8_t *p, uint8_t addr, uint16_t max_packet) {
    p[0] = USB_DT_ENDPOINT_SIZE;
    p[1] = USB_DT_ENDPOINT;
    p[2] = addr;
    p[3] = USB_ENDPOINT_XFER_BULK;
    put_u16le(p + 4, max_packet);
    p[6] = 0;
}

static bool write_descriptors(int ep0) {
    enum {
        IFACE_LEN = USB_DT_INTERFACE_SIZE,
        EP_LEN = USB_DT_ENDPOINT_SIZE,
        DESC_COUNT = 3,
        BODY_LEN = (IFACE_LEN + EP_LEN + EP_LEN) * 2,
        HEAD_LEN = 12 + 4 + 4,
        TOTAL_LEN = HEAD_LEN + BODY_LEN,
    };
    uint8_t d[TOTAL_LEN];
    uint8_t *p = d;

    pik_put_u32le(p, FUNCTIONFS_DESCRIPTORS_MAGIC_V2); p += 4;
    pik_put_u32le(p, sizeof(d)); p += 4;
    pik_put_u32le(p, FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC); p += 4;
    pik_put_u32le(p, DESC_COUNT); p += 4;
    pik_put_u32le(p, DESC_COUNT); p += 4;

    iface_desc(p); p += IFACE_LEN;
    ep_desc(p, 0x01, PIK_USB_FS_MAX_PACKET); p += EP_LEN;  /* OUT */
    ep_desc(p, 0x82, PIK_USB_FS_MAX_PACKET); p += EP_LEN;  /* IN */

    iface_desc(p); p += IFACE_LEN;
    ep_desc(p, 0x01, PIK_USB_HS_MAX_PACKET); p += EP_LEN;  /* OUT */
    ep_desc(p, 0x82, PIK_USB_HS_MAX_PACKET);               /* IN */

    return write_all(ep0, d, sizeof(d));
}

static bool write_strings(int ep0) {
    enum {
        HEAD_LEN = 16,
        LANG_LEN = 2,
        TOTAL_LEN = HEAD_LEN + LANG_LEN + sizeof(PIK1_USB_INTERFACE),
    };
    uint8_t s[TOTAL_LEN];
    uint8_t *p = s;

    pik_put_u32le(p, FUNCTIONFS_STRINGS_MAGIC); p += 4;
    pik_put_u32le(p, sizeof(s)); p += 4;
    pik_put_u32le(p, 1); p += 4;
    pik_put_u32le(p, 1); p += 4;
    put_u16le(p, PIK1_USB_LANG_ID); p += 2;
    memcpy(p, PIK1_USB_INTERFACE, sizeof(PIK1_USB_INTERFACE));

    return write_all(ep0, s, sizeof(s));
}

static bool first_udc(char *out, size_t cap) {
    DIR *d = opendir("/sys/class/udc");
    if (!d) return false;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        snprintf(out, cap, "%s", ent->d_name);
        closedir(d);
        return true;
    }
    closedir(d);
    return false;
}

static bool bind_udc(void) {
    char path[256];
    char current[256] = "";
    char udc[256];
    snprintf(path, sizeof(path), "%s/UDC", PIK_CONFIGFS_GADGET);

    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        LOG("open %s: %s", path, strerror(errno));
        return false;
    }

    ssize_t n = read(fd, current, sizeof(current) - 1);
    if (n > 0) {
        current[n] = '\0';
        if (current[0] && current[0] != '\n') {
            close(fd);
            return true;
        }
    }

    if (!first_udc(udc, sizeof(udc))) {
        LOG("no UDC found");
        close(fd);
        return false;
    }
    if (lseek(fd, 0, SEEK_SET) < 0 || !write_all(fd, udc, strlen(udc))) {
        LOG("bind UDC %s: %s", udc, strerror(errno));
        close(fd);
        return false;
    }
    close(fd);
    LOG("bound gadget to UDC: %s", udc);
    return true;
}

static void aio_req_init(void) {
    for (unsigned i = 0; i < FFS_RX_SLOTS; i++) {
        memset(&g_ffs.rx[i], 0, sizeof(g_ffs.rx[i]));
        g_ffs.rx[i].dir = FFS_AIO_RX;
    }
    for (unsigned i = 0; i < FFS_TX_SLOTS; i++) {
        memset(&g_ffs.tx[i], 0, sizeof(g_ffs.tx[i]));
        g_ffs.tx[i].dir = FFS_AIO_TX;
    }
}

static bool aio_submit_req(ffs_aio_req_t *req, int fd,
                           uint16_t opcode, uint32_t len) {
    memset(&req->cb, 0, sizeof(req->cb));
    req->cb.aio_data = (uint64_t)(uintptr_t)req;
    req->cb.aio_lio_opcode = opcode;
    req->cb.aio_fildes = (uint32_t)fd;
    req->cb.aio_buf = (uint64_t)(uintptr_t)req->buf;
    req->cb.aio_nbytes = len;
    req->cb.aio_offset = 0;
    req->cb.aio_flags = IOCB_FLAG_RESFD;
    req->cb.aio_resfd = (uint32_t)g_ffs.event_fd;

    struct iocb *cb = &req->cb;
    int r;
    do {
        r = aio_submit(g_ffs.aio, 1, &cb);
    } while (r < 0 && errno == EINTR);
    if (r != 1) {
        LOG("io_submit %s: %s",
            req->dir == FFS_AIO_RX ? "OUT" : "IN",
            r < 0 ? strerror(errno) : "short submit");
        pik_link_fail(g_ffs.lk);
        return false;
    }

    req->busy = true;
    req->len = len;
    return true;
}

static bool submit_rx_reads(void) {
    if (!g_ffs.enabled || g_ffs.out_fd < 0) return true;
    for (unsigned i = 0; i < FFS_RX_SLOTS; i++) {
        if (!g_ffs.rx[i].busy &&
            !aio_submit_req(&g_ffs.rx[i], g_ffs.out_fd,
                            IOCB_CMD_PREAD, FFS_RX_CHUNK))
            return false;
    }
    return true;
}

static bool pump_tx(void) {
    if (!g_ffs.enabled || g_ffs.in_fd < 0 || g_ffs.tx[0].busy)
        return true;

    uint32_t len = 0;
    const uint8_t *p = pik_link_tx_peek(g_ffs.lk, &len);
    if (!p || !len) return true;
    if (len > PIK_USB_TX_CHUNK) len = PIK_USB_TX_CHUNK;

    ffs_aio_req_t *req = &g_ffs.tx[0];
    memcpy(req->buf, p, len);
    return aio_submit_req(req, g_ffs.in_fd, IOCB_CMD_PWRITE, len);
}

static void drain_eventfd(void) {
    if (g_ffs.event_fd < 0) return;
    uint64_t v;
    while (read(g_ffs.event_fd, &v, sizeof(v)) == (ssize_t)sizeof(v)) {
    }
}

static bool handle_rx_complete(ffs_aio_req_t *req, int64_t res, int64_t now) {
    req->busy = false;
    if (res < 0) {
        LOG("OUT aio: %s", strerror((int)-res));
        pik_link_fail(g_ffs.lk);
        return false;
    }
    if ((uint64_t)res > req->len) {
        LOG("OUT aio: invalid completion result=%lld requested=%u",
            (long long)res, req->len);
        pik_link_fail(g_ffs.lk);
        return false;
    }
    if (res > 0) {
        if (!pik_link_feed(g_ffs.lk, req->buf, (size_t)res, now))
            return false;
    }
    return submit_rx_reads();
}

static bool handle_tx_complete(ffs_aio_req_t *req, int64_t res, int64_t now) {
    req->busy = false;
    if (res < 0) {
        LOG("IN aio: %s", strerror((int)-res));
        pik_link_fail(g_ffs.lk);
        return false;
    }
    if (res <= 0 || (uint64_t)res > req->len) {
        LOG("IN aio: invalid completion result=%lld requested=%u",
            (long long)res, req->len);
        pik_link_fail(g_ffs.lk);
        return false;
    }
    pik_link_tx_consume(g_ffs.lk, (uint32_t)res, now);
    return pump_tx();
}

static bool process_aio(int64_t now) {
    drain_eventfd();

    while (g_ffs.aio) {
        struct io_event events[8];
        int n;
        do {
            n = aio_getevents(g_ffs.aio, 0, 8, events);
        } while (n < 0 && errno == EINTR);
        if (n < 0) {
            LOG("io_getevents: %s", strerror(errno));
            pik_link_fail(g_ffs.lk);
            return false;
        }
        if (n == 0) break;

        for (int i = 0; i < n; i++) {
            ffs_aio_req_t *req = (ffs_aio_req_t *)(uintptr_t)events[i].data;
            if (!req) continue;
            if (events[i].res2 != 0) {
                LOG("aio completion secondary result=%lld",
                    (long long)events[i].res2);
                pik_link_fail(g_ffs.lk);
                return false;
            }
            bool ok = req->dir == FFS_AIO_RX
                ? handle_rx_complete(req, events[i].res, now)
                : handle_tx_complete(req, events[i].res, now);
            if (!ok) return false;
        }
    }
    return pump_tx();
}

static bool handle_setup(const struct usb_ctrlrequest *setup) {
    uint16_t len = (uint16_t)setup->wLength;
    if ((setup->bRequestType & USB_DIR_IN) != 0)
        return write_all(g_ffs.ep0, "", 0);
    if (len && !read_discard(g_ffs.ep0, len))
        return false;
    return write_all(g_ffs.ep0, "", 0);
}

static bool handle_ep0(void) {
    struct usb_functionfs_event ev[8];
    while (g_ffs.ep0 >= 0) {
        ssize_t n = read(g_ffs.ep0, ev, sizeof(ev));
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) return true;
            LOG("ep0 read: %s", strerror(errno));
            pik_link_fail(g_ffs.lk);
            return false;
        }
        if (n == 0) return true;
        int count = (int)(n / (ssize_t)sizeof(ev[0]));
        for (int i = 0; i < count; i++) {
            switch (ev[i].type) {
            case FUNCTIONFS_ENABLE:
                g_ffs.enabled = true;
                LOG("endpoints enabled");
                if (!submit_rx_reads() || !pump_tx())
                    return false;
                break;
            case FUNCTIONFS_DISABLE:
                g_ffs.enabled = false;
                pik_link_fail(g_ffs.lk);
                LOG("endpoints disabled");
                break;
            case FUNCTIONFS_UNBIND:
                g_ffs.enabled = false;
                pik_link_fail(g_ffs.lk);
                LOG("function unbound");
                break;
            case FUNCTIONFS_SETUP:
                if (!handle_setup(&ev[i].u.setup)) {
                    LOG("setup request failed: %s", strerror(errno));
                    pik_link_fail(g_ffs.lk);
                    return false;
                }
                break;
            default:
                break;
            }
        }
    }
    return true;
}

bool pik_usb_gadget_start(pik_link_t *lk, int epfd, int64_t now) {
    char path[256];

    pik_usb_gadget_cleanup();
    g_ffs.lk = lk;
    g_ffs.epfd = epfd;
    aio_req_init();

    snprintf(path, sizeof(path), "%s/ep0", PIK1_FFS_MOUNT);
    g_ffs.ep0 = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (g_ffs.ep0 < 0) {
        LOG("open %s: %s", path, strerror(errno));
        return false;
    }
    if (!write_descriptors(g_ffs.ep0) || !write_strings(g_ffs.ep0)) {
        LOG("write FunctionFS descriptors: %s", strerror(errno));
        pik_usb_gadget_cleanup();
        return false;
    }

    snprintf(path, sizeof(path), "%s/ep1", PIK1_FFS_MOUNT);
    g_ffs.out_fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (g_ffs.out_fd < 0) {
        LOG("open %s: %s", path, strerror(errno));
        pik_usb_gadget_cleanup();
        return false;
    }
    snprintf(path, sizeof(path), "%s/ep2", PIK1_FFS_MOUNT);
    g_ffs.in_fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (g_ffs.in_fd < 0) {
        LOG("open %s: %s", path, strerror(errno));
        pik_usb_gadget_cleanup();
        return false;
    }

    g_ffs.event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (g_ffs.event_fd < 0) {
        LOG("eventfd: %s", strerror(errno));
        pik_usb_gadget_cleanup();
        return false;
    }
    if (aio_setup(FFS_RX_SLOTS + FFS_TX_SLOTS, &g_ffs.aio) < 0) {
        LOG("io_setup: %s", strerror(errno));
        pik_usb_gadget_cleanup();
        return false;
    }

    if (!pik_epoll_set(epfd, g_ffs.ep0, EPOLLIN, &g_ep0_tag) ||
        !pik_epoll_set(epfd, g_ffs.event_fd, EPOLLIN, &g_aio_tag)) {
        LOG("epoll add FunctionFS fd: %s", strerror(errno));
        pik_usb_gadget_cleanup();
        return false;
    }
    pik_link_begin(lk, now);
    if (!bind_udc()) {
        pik_usb_gadget_cleanup();
        return false;
    }
    LOG("FunctionFS transport ready at %s", PIK1_FFS_MOUNT);
    return true;
}

bool pik_usb_gadget_owns_event(const void *ptr) {
    return ptr == &g_ep0_tag || ptr == &g_aio_tag;
}

bool pik_usb_gadget_dispatch(void *ptr, uint32_t events, int64_t now) {
    if (events & (EPOLLERR | EPOLLHUP)) {
        const char *which = ptr == &g_ep0_tag ? "ep0" :
                            ptr == &g_aio_tag ? "aio" : "unknown";
        LOG("%s event failure: events=0x%x", which, events);
        pik_link_fail(g_ffs.lk);
        return false;
    }
    if (ptr == &g_ep0_tag)
        return handle_ep0();
    if (ptr == &g_aio_tag)
        return process_aio(now);
    return true;
}

bool pik_usb_gadget_tick(int64_t now) {
    if (!g_ffs.enabled || !pik_link_is_open(g_ffs.lk)) return true;
    return process_aio(now);
}

int64_t pik_usb_gadget_deadline(void) {
    if (!g_ffs.enabled || !pik_link_is_open(g_ffs.lk)) return INT64_MAX;
    return pik_now_ms() + FFS_POLL_MS;
}

void pik_usb_gadget_cleanup(void) {
    if (g_ffs.ep0 >= 0 || g_ffs.out_fd >= 0 || g_ffs.in_fd >= 0)
        LOG("cleaning up FunctionFS transport");
    if (g_ffs.epfd >= 0) {
        if (g_ffs.ep0 >= 0) pik_epoll_del(g_ffs.epfd, g_ffs.ep0);
        if (g_ffs.event_fd >= 0) pik_epoll_del(g_ffs.epfd, g_ffs.event_fd);
    }
    if (g_ffs.aio) {
        (void)aio_destroy(g_ffs.aio);
        g_ffs.aio = 0;
    }
    if (g_ffs.ep0 >= 0) close(g_ffs.ep0);
    if (g_ffs.out_fd >= 0) close(g_ffs.out_fd);
    if (g_ffs.in_fd >= 0) close(g_ffs.in_fd);
    if (g_ffs.event_fd >= 0) close(g_ffs.event_fd);
    g_ffs.lk = NULL;
    g_ffs.epfd = -1;
    g_ffs.ep0 = -1;
    g_ffs.out_fd = -1;
    g_ffs.in_fd = -1;
    g_ffs.event_fd = -1;
    g_ffs.enabled = false;
    aio_req_init();
}
