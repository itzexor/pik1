// src/pik1d.c - daemon supervisor: USB discovery, child management, mux session loop

#include "serialmux.h"
#include "fd.h"
#include "util.h"
#include "usb_discovery.h"

#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <unistd.h>

#define RETRY_MIN_MS 1000
#define RETRY_MAX_MS 30000
#define MAX_EVENTS 32

static void log_msg(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fputs("[pik1] ", stderr);
    vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr);
}
#define LOG(...) log_msg(__VA_ARGS__)
#define DIE(...) do { log_msg(__VA_ARGS__); exit(1); } while (0)

// ── tcpbridge child management ────────────────────────────────────────────────
static struct {
    bool    enabled;
    pid_t   pid;
    char   *argv[8];
    char    tunnel_dev[64];
    int64_t restart_at_ms;
    int     backoff_ms;
    bool    intentional_stop;
    bool    waiting_for_tunnel;
} g_child = {
    .pid = -1,
    .backoff_ms = RETRY_MIN_MS,
};

static void child_schedule_restart(int64_t now) {
    g_child.restart_at_ms = now + pik_backoff_next(&g_child.backoff_ms, RETRY_MAX_MS);
}

static void child_spawn(const char *tunnel_dev, int64_t now) {
    if (!g_child.enabled || g_child.pid >= 0) return;

    snprintf(g_child.tunnel_dev, sizeof(g_child.tunnel_dev), "%s", tunnel_dev);
    g_child.argv[1] = g_child.tunnel_dev;

    pid_t pid = fork();
    if (pid < 0) {
        LOG("child: fork: %s", strerror(errno));
        child_schedule_restart(now);
        return;
    }
    if (pid == 0) {
        sigset_t all;
        sigfillset(&all);
        sigprocmask(SIG_UNBLOCK, &all, NULL);
        execvp(g_child.argv[0], g_child.argv);
        fprintf(stderr, "child: execvp %s: %s\n", g_child.argv[0], strerror(errno));
        _exit(127);
    }

    g_child.pid = pid;
    g_child.restart_at_ms = 0;
    g_child.backoff_ms = RETRY_MIN_MS;
    g_child.intentional_stop = false;
    g_child.waiting_for_tunnel = false;
    LOG("child: spawned %s pid=%d", g_child.argv[0], pid);
}

static void child_stop(void) {
    if (g_child.pid <= 0) {
        g_child.pid = -1;
        g_child.restart_at_ms = 0;
        return;
    }

    pid_t pid = g_child.pid;
    g_child.intentional_stop = true;
    kill(pid, SIGTERM);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
    g_child.pid = -1;
    g_child.restart_at_ms = 0;
    g_child.intentional_stop = false;
}

static void child_handle_sigchld(int64_t now, bool session_active) {
    if (g_child.pid <= 0) return;

    int status = 0;
    pid_t r = waitpid(g_child.pid, &status, WNOHANG);
    if (r <= 0) return;

    g_child.pid = -1;
    if (g_child.intentional_stop) {
        g_child.restart_at_ms = 0;
        return;
    }

    if (session_active) {
        LOG("child: exited status=%d, restart in %dms", status, g_child.backoff_ms);
        child_schedule_restart(now);
    } else {
        LOG("child: exited status=%d", status);
        g_child.restart_at_ms = 0;
    }
}

static int64_t child_deadline(void) {
    if (!g_child.enabled || g_child.pid >= 0 || g_child.restart_at_ms == 0)
        return INT64_MAX;
    return g_child.restart_at_ms;
}

// ── USB session supervision ──────────────────────────────────────────────────
typedef enum {
    USB_WAIT_UNKNOWN = -1,
    USB_WAIT_NONE = 0,
    USB_WAIT_MAIN_ONLY = 1,
    USB_WAIT_READY = 2,
} usb_wait_state_t;

static usb_wait_state_t usb_resolve(const char *vidpid, bool has_tcp,
                                    char *main_dev, size_t main_cap,
                                    char *tunnel_dev, size_t tunnel_cap) {
    const char *main = usb_find_serial_dev(vidpid, 0);
    if (!main) return USB_WAIT_NONE;
    snprintf(main_dev, main_cap, "%s", main);

    if (!has_tcp) return USB_WAIT_READY;

    const char *tunnel = usb_find_serial_dev(vidpid, 1);
    if (!tunnel) return USB_WAIT_MAIN_ONLY;
    snprintf(tunnel_dev, tunnel_cap, "%s", tunnel);
    return USB_WAIT_READY;
}

static void usb_log_wait_state(usb_wait_state_t state, bool has_tcp) {
    switch (state) {
    case USB_WAIT_NONE:
        LOG("waiting for USB endpoint 0");
        break;
    case USB_WAIT_MAIN_ONLY:
        LOG("waiting for USB endpoint 1");
        break;
    case USB_WAIT_READY:
        LOG("USB endpoints ready%s", has_tcp ? " (0,1)" : " (0)");
        break;
    default:
        break;
    }
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage:\n"
        "  %s --usb VID:PID  mcu:N:DEV:BAUD [...] [tcp:ADDR:PORT]\n"
        "  %s --usb VID:PID  pty:N:SYMLINK  [...] [tcp:HOST:PORT]\n",
        prog, prog);
    exit(1);
}

int main(int argc, char **argv) {
    if (argc < 4) usage(argv[0]);

    char self_dir[512] = "";
    {
        char tmp[512];
        strncpy(tmp, argv[0], sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        strncpy(self_dir, dirname(tmp), sizeof(self_dir) - 1);
    }

    int argi = 1;
    if (strcmp(argv[argi], "--usb") != 0 || argi + 1 >= argc) usage(argv[0]);
    const char *vidpid = argv[argi + 1];
    argi += 2;

    if (argi >= argc) usage(argv[0]);
    bool is_mcu = strncmp(argv[argi], "mcu:", 4) == 0;
    bool is_pty = strncmp(argv[argi], "pty:", 4) == 0;
    if (!is_mcu && !is_pty) usage(argv[0]);

    serialmux_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    bool seen_ch[UINT8_MAX + 1] = { false };
    while (argi < argc) {
        char *spec = argv[argi];
        if (strncmp(spec, "tcp:", 4) == 0) break;
        if (cfg.n_channels >= MAX_CHANNELS)
            DIE("too many channels (max %d): %s", MAX_CHANNELS, spec);
        argi++;

        ch_spec_t *s = &cfg.channels[cfg.n_channels];
        memset(s, 0, sizeof(*s));

        if (is_mcu) {
            char ch_str[16], baud_str[16], extra;
            if (sscanf(spec, "mcu:%15[^:]:%127[^:]:%15[^:]%c",
                       ch_str, s->dev, baud_str, &extra) != 3)
                DIE("bad mcu spec: %s", spec);
            if (!pik_parse_uint8(ch_str, &s->ch_id))
                DIE("bad mcu channel id: %s", spec);
            if (!pik_parse_positive_int(baud_str, &s->baud))
                DIE("bad mcu baud: %s", spec);
            s->type = CH_MCU;
        } else {
            char ch_str[16], extra;
            if (sscanf(spec, "pty:%15[^:]:%127s%c", ch_str, s->path, &extra) != 2)
                DIE("bad pty spec: %s", spec);
            if (!pik_parse_uint8(ch_str, &s->ch_id))
                DIE("bad pty channel id: %s", spec);
            s->type = CH_PTY;
        }
        if (seen_ch[s->ch_id])
            DIE("duplicate channel id: %u", s->ch_id);
        seen_ch[s->ch_id] = true;
        cfg.n_channels++;
    }
    if (!cfg.n_channels) usage(argv[0]);

    bool has_tcp = false;
    static char tb_path[576], addr_port[128];
    char tcp_addr[64] = "";
    int tcp_port = 0;

    if (argi < argc && strncmp(argv[argi], "tcp:", 4) == 0) {
        char port_str[16], extra;
        if (sscanf(argv[argi] + 4, "%63[^:]:%15[^:]%c",
                   tcp_addr, port_str, &extra) != 2 ||
            !pik_parse_port(port_str, &tcp_port))
            DIE("bad tcp spec: %s", argv[argi]);
        argi++;

        snprintf(addr_port, sizeof(addr_port), "%s:%d", tcp_addr, tcp_port);
        int i = 0;
        snprintf(tb_path, sizeof(tb_path), "%s/tcpbridge", self_dir);
        g_child.argv[i++] = (access(tb_path, X_OK) == 0) ? tb_path : "tcpbridge";
        g_child.argv[i++] = g_child.tunnel_dev;
        g_child.argv[i++] = is_mcu ? "listen" : "forward";
        g_child.argv[i++] = addr_port;
        g_child.argv[i] = NULL;
        g_child.enabled = true;
        has_tcp = true;
    }

    if (argi != argc) DIE("unexpected argument: %s", argv[argi]);

    if (has_tcp)
        LOG("mode=%s channels=%d tcp=%s:%d tunnel=usb[1]",
            is_mcu ? "exporter" : "host", cfg.n_channels, tcp_addr, tcp_port);
    else
        LOG("mode=%s channels=%d", is_mcu ? "exporter" : "host", cfg.n_channels);

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigaddset(&mask, SIGTERM);
    sigprocmask(SIG_BLOCK, &mask, NULL);

    int sig_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sig_fd < 0) DIE("signalfd: %s", strerror(errno));

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) DIE("epoll_create1: %s", strerror(errno));

    static int sig_tag;
    pik_epoll_set(epfd, sig_fd, EPOLLIN, &sig_tag);
    serialmux_init(&cfg, epfd);

    bool shutdown = false;
    bool session_active = false;
    bool session_confirmed = false;
    int usb_backoff_ms = RETRY_MIN_MS;
    int64_t usb_retry_at = pik_now_ms();
    usb_wait_state_t last_usb_state = USB_WAIT_UNKNOWN;
    char main_dev[64] = "";
    char tunnel_dev[64] = "";

    while (!shutdown) {
        int64_t now = pik_now_ms();

        if (!session_active && now >= usb_retry_at) {
            usb_wait_state_t state = usb_resolve(vidpid, has_tcp,
                                                 main_dev, sizeof(main_dev),
                                                 tunnel_dev, sizeof(tunnel_dev));
            if (state != last_usb_state) {
                usb_log_wait_state(state, has_tcp);
                last_usb_state = state;
            }

            if (state == USB_WAIT_READY) {
                if (serialmux_start(main_dev, now)) {
                    session_active = true;
                    session_confirmed = false;
                    usb_retry_at = 0;
                    LOG("session started: main=%s%s%s",
                        main_dev, has_tcp ? " tunnel=" : "",
                        has_tcp ? tunnel_dev : "");
                    if (has_tcp)
                        child_spawn(tunnel_dev, now);
                } else {
                    serialmux_cleanup();
                    usb_retry_at = now + pik_backoff_next(&usb_backoff_ms, RETRY_MAX_MS);
                }
            } else {
                usb_retry_at = now + pik_backoff_next(&usb_backoff_ms, RETRY_MAX_MS);
            }
        }

        int64_t dl = session_active ? serialmux_deadline(now) : usb_retry_at;
        int64_t cd = child_deadline();
        if (cd < dl) dl = cd;

        int timeout = 5000;
        if (dl != INT64_MAX) {
            int64_t wait_ms = dl - now;
            timeout = wait_ms <= 0 ? 0 : (wait_ms < 5000 ? (int)wait_ms : 5000);
        }

        struct epoll_event evs[MAX_EVENTS];
        int n = epoll_wait(epfd, evs, MAX_EVENTS, timeout);
        if (n < 0) {
            if (errno == EINTR) continue;
            DIE("epoll_wait: %s", strerror(errno));
        }

        now = pik_now_ms();
        for (int i = 0; i < n; i++) {
            void *ptr = evs[i].data.ptr;
            uint32_t ev = evs[i].events;

            if (ptr == &sig_tag) {
                struct signalfd_siginfo si;
                while (read(sig_fd, &si, sizeof(si)) == (ssize_t)sizeof(si)) {
                    if (si.ssi_signo == SIGTERM) {
                        shutdown = true;
                    } else if (si.ssi_signo == SIGCHLD) {
                        child_handle_sigchld(now, session_active);
                    }
                }
                continue;
            }

            if (session_active && !serialmux_dispatch(ptr, ev, now)) {
                LOG("session failed, restarting USB link");
                session_active = false;
                session_confirmed = false;
                child_stop();
                serialmux_cleanup();
                usb_retry_at = now + pik_backoff_next(&usb_backoff_ms, RETRY_MAX_MS);
                last_usb_state = USB_WAIT_UNKNOWN;
            }
        }

        if (shutdown) break;

        now = pik_now_ms();
        if (session_active && !serialmux_tick(now)) {
            LOG("session failed, restarting USB link");
            session_active = false;
            session_confirmed = false;
            child_stop();
            serialmux_cleanup();
            usb_retry_at = now + pik_backoff_next(&usb_backoff_ms, RETRY_MAX_MS);
            last_usb_state = USB_WAIT_UNKNOWN;
        }

        if (session_active && !session_confirmed && serialmux_link_up()) {
            session_confirmed = true;
            usb_backoff_ms = RETRY_MIN_MS;
        }

        if (session_active && has_tcp &&
            g_child.pid < 0 && g_child.restart_at_ms && now >= g_child.restart_at_ms) {
            const char *td = usb_find_serial_dev(vidpid, 1);
            if (td) {
                snprintf(tunnel_dev, sizeof(tunnel_dev), "%s", td);
                child_spawn(tunnel_dev, now);
            } else {
                if (!g_child.waiting_for_tunnel) {
                    LOG("child: waiting for tunnel device 1");
                    g_child.waiting_for_tunnel = true;
                }
                child_schedule_restart(now);
            }
        }
    }

    child_stop();
    serialmux_cleanup();
    close(sig_fd);
    close(epfd);
    return 0;
}
