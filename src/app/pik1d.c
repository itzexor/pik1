// pik1d daemon lifecycle: single-link session management

#include "app_config.h"
#include "control.h"
#include "daemon_control.h"
#include "local_control.h"
#include "pik_proto.h"
#include "serialmux.h"
#include "session.h"
#include "tunnel.h"
#include "usb_bulk.h"
#include "usb_gadget_configfs.h"
#include "usb_gadget_ffs.h"
#include "fd.h"
#include "logging.h"
#include "product.h"
#include "util.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <unistd.h>

#define RETRY_MIN_MS 1000
#define RETRY_MAX_MS 30000
#define MAX_EVENTS 32

#define LOG(...) pik_log("pik1", __VA_ARGS__)
#define DIE(...) pik_die("pik1", __VA_ARGS__)

typedef struct {
    app_mode_t mode;
    bool has_tcp;
    int transport_backoff_ms;
} app_state_t;

static app_state_t g_app = {
    .transport_backoff_ms = RETRY_MIN_MS,
};

static bool transport_prepare(void) {
    if (g_app.mode == APP_MODE_K1) return true;
    return pik_ffs_prepare_gadget();
}

static bool transport_start(int epfd, int64_t now) {
    pik_link_t *lk = pik_session_link();
    if (g_app.mode == APP_MODE_K1)
        return pik_usb_bulk_start(lk, epfd, now);
    return pik_ffs_start(lk, epfd, now);
}

static bool transport_owns_event(const void *ptr) {
    return g_app.mode == APP_MODE_K1 ? pik_usb_bulk_owns_event(ptr)
                                     : pik_ffs_owns_event(ptr);
}

static bool transport_dispatch(void *ptr, uint32_t events, int64_t now) {
    return g_app.mode == APP_MODE_K1 ? pik_usb_bulk_dispatch(ptr, events, now)
                                     : pik_ffs_dispatch(ptr, events, now);
}

static bool transport_tick(int64_t now) {
    return g_app.mode == APP_MODE_K1 ? pik_usb_bulk_tick(now)
                                     : pik_ffs_tick(now);
}

static int64_t transport_deadline(void) {
    return g_app.mode == APP_MODE_K1 ? pik_usb_bulk_deadline()
                                     : pik_ffs_deadline();
}

static void transport_cleanup(void) {
    if (g_app.mode == APP_MODE_K1)
        pik_usb_bulk_cleanup();
    else
        pik_ffs_cleanup();
}

/* Start logical services only after the shared-link handshake completes. */
static void on_control_ready(void) {
    int64_t now = pik_now_ms();
    uint32_t services = PIK_CONTROL_SERVICE_SERIAL;
    g_app.transport_backoff_ms = RETRY_MIN_MS;
    serialmux_start(now);
    tunnel_start(now);
    if (tunnel_active())
        services |= PIK_CONTROL_SERVICE_TUNNEL;
    pik_daemon_set_service_flags(services);
    LOG("services started");
}

static void session_teardown(void) {
    transport_cleanup();
    tunnel_cleanup();
    serialmux_cleanup();
    pik_control_cleanup();
    pik_session_cleanup();
    pik_daemon_set_service_flags(0);
}

static void execute_remote_action(pik_control_action_t action, char **argv) {
    if (action == PIK_CONTROL_ACTION_REBOOT_PEER ||
        action == PIK_CONTROL_ACTION_POWEROFF_PEER)
        pik_mark_peer_initiated();
    session_teardown();
    pik_local_control_cleanup();

    switch (action) {
    case PIK_CONTROL_ACTION_RESTART_PEER:
        LOG("executing restart-peer");
        execv(argv[0], argv);
        LOG("execv %s: %s", argv[0], strerror(errno));
        _exit(127);
    case PIK_CONTROL_ACTION_REBOOT_PEER:
        LOG("executing reboot-peer");
        execl("/sbin/reboot", "reboot", (char *)NULL);
        LOG("execl /sbin/reboot: %s", strerror(errno));
        _exit(127);
    case PIK_CONTROL_ACTION_POWEROFF_PEER:
        LOG("executing poweroff-peer");
        execl("/sbin/poweroff", "poweroff", (char *)NULL);
        LOG("execl /sbin/poweroff: %s", strerror(errno));
        _exit(127);
    default:
        _exit(1);
    }
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("pik1d %s protocol=%u features=0x%08x\n",
               PIK1_RELEASE_VERSION, PIK1_PROTOCOL_VERSION, PIK1_FEATURE_FLAGS);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "--control") == 0)
        return pik_local_control_client_main(argv[2]);

    if (argc < 3) pik_app_config_usage(argv[0]);

    app_config_t cfg;
    pik_app_config_parse(argc, argv, &cfg);

    g_app.mode = cfg.mode;
    const char *transport_name = cfg.mode == APP_MODE_K1 ? "usb-bulk" : "ffs";
    pik_daemon_control_init(cfg.uart_name);
    g_app.has_tcp = cfg.has_tcp;
    pik_log_set_timestamps(cfg.mode == APP_MODE_K1);

    if (cfg.has_tcp)
        LOG("uart=%s release=%s protocol=%u channels=%d tcp=%s:%s:%d link=%s",
            cfg.uart_name, PIK1_RELEASE_VERSION, PIK1_PROTOCOL_VERSION,
            cfg.mux.n_channels, cfg.tcp_mode_name, cfg.tcp_addr, cfg.tcp_port,
            transport_name);
    else
        LOG("uart=%s release=%s protocol=%u channels=%d link=%s",
            cfg.uart_name, PIK1_RELEASE_VERSION, PIK1_PROTOCOL_VERSION,
            cfg.mux.n_channels, transport_name);

    if (!transport_prepare())
        DIE("failed to prepare %s", transport_name);

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGUSR1);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    signal(SIGPIPE, SIG_IGN);

    int sig_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sig_fd < 0) DIE("signalfd: %s", strerror(errno));

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) DIE("epoll_create1: %s", strerror(errno));

    static int sig_tag;
    if (!pik_epoll_set(epfd, sig_fd, EPOLLIN, &sig_tag))
        DIE("epoll add signal fd: %s", strerror(errno));
    pik_session_init();
    pik_control_init(cfg.control_role, pik_daemon_on_control_command,
                     on_control_ready);
    {
        uint8_t channel_ids[MAX_CHANNELS];
        for (int i = 0; i < cfg.mux.n_channels; i++)
            channel_ids[i] = cfg.mux.channels[i].ch_id;
        pik_control_set_config(channel_ids, (size_t)cfg.mux.n_channels, cfg.tcp_role);
    }
    if (!pik_local_control_start(epfd))
        DIE("failed to start local control socket");
    serialmux_init(&cfg.mux, epfd);
    tunnel_init(epfd, cfg.tunnel_mode, cfg.tcp_addr, cfg.tcp_port);

    bool shutdown = false;
    int exit_status = 0;
    bool session_active = false;
    int64_t transport_retry_at = pik_now_ms();
    bool usb_bulk_waiting_logged = false;

    while (!shutdown) {
        int64_t now = pik_now_ms();

        if (!session_active && now >= transport_retry_at) {
            if (transport_start(epfd, now) && pik_control_on_link_open()) {
                usb_bulk_waiting_logged = false;
                pik_daemon_set_service_flags(0);
                session_active = true;
                transport_retry_at = 0;
                if (pik_control_handshake_failures() == 0)
                    LOG("session started: %s", transport_name);
            } else {
                if (cfg.mode == APP_MODE_K1) {
                    if (!usb_bulk_waiting_logged) {
                        LOG("waiting for USB bulk device");
                        usb_bulk_waiting_logged = true;
                    }
                } else {
                    usb_bulk_waiting_logged = false;
                }
                session_teardown();
                transport_retry_at =
                    now + pik_backoff_next(&g_app.transport_backoff_ms, RETRY_MAX_MS);
            }
        }

        int64_t dl = session_active ? pik_control_deadline() : transport_retry_at;
        if (session_active) {
            int64_t sd = pik_session_deadline();
            if (sd < dl) dl = sd;
            int64_t md = serialmux_deadline(now);
            if (md < dl) dl = md;
            int64_t td = tunnel_deadline();
            if (td < dl) dl = td;
            int64_t xd = transport_deadline();
            if (xd < dl) dl = xd;
        }
        int64_t ld = pik_daemon_control_deadline();
        if (ld < dl) dl = ld;

        int timeout = 5000;
        if (dl != INT64_MAX) {
            int64_t wait_ms = dl - now;
            timeout = wait_ms <= 0 ? 0 : (wait_ms < 5000 ? (int)wait_ms : 5000);
        }

        struct epoll_event evs[MAX_EVENTS];
        int n = epoll_wait(epfd, evs, MAX_EVENTS, timeout);
        if (n < 0) {
            if (errno == EINTR) continue;
            LOG("epoll_wait: %s", strerror(errno));
            exit_status = 1;
            break;
        }

        now = pik_now_ms();
        bool session_failed = false;
        for (int i = 0; i < n; i++) {
            void *ptr = evs[i].data.ptr;
            uint32_t ev = evs[i].events;

            if (ptr == &sig_tag) {
                struct signalfd_siginfo si;
                while (read(sig_fd, &si, sizeof(si)) == (ssize_t)sizeof(si)) {
                    if (si.ssi_signo == SIGTERM) {
                        shutdown = true;
                    } else if (si.ssi_signo == SIGUSR1) {
                        pik_daemon_request_restart_peer(cfg.mode == APP_MODE_PI, now);
                    }
                }
                continue;
            }

            if (pik_local_control_owns_listen(ptr)) {
                pik_local_control_accept();
                continue;
            }

            if (pik_local_control_owns_client(ptr)) {
                pik_local_control_read(now, pik_daemon_signal_pending());
                continue;
            }

            if (!session_active) continue;

            if (transport_owns_event(ptr)) {
                if (!transport_dispatch(ptr, ev, now))
                    session_failed = true;
            } else if (serialmux_owns_event(ptr)) {
                if (!serialmux_dispatch(ptr, ev, now))
                    session_failed = true;
            } else if (tunnel_owns_event(ptr)) {
                if (!tunnel_dispatch(ptr, ev))
                    session_failed = true;
            }
        }

        if (pik_daemon_signal_done()) shutdown = true;
        if (shutdown) break;

        now = pik_now_ms();
        pik_daemon_check_acks(now);

        if (session_active && !session_failed) {
            if (!pik_control_tick(now) ||
                !pik_session_tick(now) ||
                !transport_tick(now) ||
                !serialmux_tick(now) ||
                !tunnel_tick(now))
                session_failed = true;
        }

        if (session_active && !session_failed && g_app.has_tcp)
            pik_daemon_set_service_flag(PIK_CONTROL_SERVICE_TUNNEL,
                                        tunnel_active());

        if (session_active && session_failed) {
            /* Control logs periodic summaries for a persistent handshake loop. */
            if (pik_control_handshake_failures() <= 1)
                LOG("session failed, restarting");
            session_active = false;
            session_teardown();
            transport_retry_at =
                now + pik_backoff_next(&g_app.transport_backoff_ms, RETRY_MAX_MS);
        }

        pik_control_action_t remote_action;
        if (pik_daemon_remote_action_due(now, &remote_action))
            execute_remote_action(remote_action, argv);
    }

    session_teardown();
    pik_local_control_cleanup();
    close(sig_fd);
    close(epfd);
    return exit_status;
}
