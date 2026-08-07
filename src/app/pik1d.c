// pik1d daemon lifecycle: single-link session management

#include "control.h"
#include "commands.h"
#include "pik_proto.h"
#include "serialmux.h"
#include "session.h"
#include "tunnel.h"
#include "usb.h"
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

typedef enum {
    APP_MODE_K1,
    APP_MODE_PI,
} app_mode_t;

typedef struct {
    app_mode_t mode;
    serialmux_config_t mux;
    char tcp_addr[64];
    int tcp_port;
    tunnel_mode_t tunnel_mode;
    pik_control_tcp_role_t tcp_role;
} app_config_t;

/* The listed commands are exactly what --control accepts: both come from the
 * control action vocabulary. */
#define ACTION_USAGE_LINE(sym, value, name) "  " name "\n"

static _Noreturn void usage(const char *prog) {
    fprintf(stderr,
        "Usage:\n"
        "  %s --version\n"
        "  %s --control COMMAND\n"
        "  %s --usb  mcu:N:DEV:BAUD [...] [listen:BIND_ADDR:PORT]\n"
        "  %s --ffs  pty:N:SYMLINK  [...] [forward:TARGET_HOST:PORT]\n"
        "\n"
        "Control commands:\n"
        PIK_CONTROL_ACTION_LIST(ACTION_USAGE_LINE),
        prog, prog, prog, prog);
    exit(1);
}

#undef ACTION_USAGE_LINE

static void parse_mcu_channel(const char *spec, ch_spec_t *s) {
    char ch_str[16], baud_str[16], extra;
    if (sscanf(spec, "mcu:%15[^:]:%127[^:]:%15[^:]%c",
               ch_str, s->dev, baud_str, &extra) != 3)
        DIE("bad mcu spec: %s", spec);
    if (!pik_parse_uint8(ch_str, &s->ch_id))
        DIE("bad mcu channel id: %s", spec);
    if (!pik_parse_positive_int(baud_str, &s->baud))
        DIE("bad mcu baud: %s", spec);
    s->type = CH_MCU;
}

static void parse_pty_channel(const char *spec, ch_spec_t *s) {
    char ch_str[16], extra;
    if (sscanf(spec, "pty:%15[^:]:%127s%c", ch_str, s->path, &extra) != 2)
        DIE("bad pty spec: %s", spec);
    if (!pik_parse_uint8(ch_str, &s->ch_id))
        DIE("bad pty channel id: %s", spec);
    s->type = CH_PTY;
}

static void parse_tcp_spec(app_config_t *cfg, const char *spec) {
    const char *hostport;
    if (strncmp(spec, "listen:", 7) == 0) {
        cfg->tcp_role = PIK_CONTROL_TCP_LISTEN;
        cfg->tunnel_mode = TUNNEL_MODE_LISTEN;
        hostport = spec + 7;
    } else if (strncmp(spec, "forward:", 8) == 0) {
        cfg->tcp_role = PIK_CONTROL_TCP_FORWARD;
        cfg->tunnel_mode = TUNNEL_MODE_FORWARD;
        hostport = spec + 8;
    } else {
        DIE("bad tcp spec: %s", spec);
    }

    char port_str[16], extra;
    if (sscanf(hostport, "%63[^:]:%15[^:]%c",
               cfg->tcp_addr, port_str, &extra) != 2 ||
        !pik_parse_port(port_str, &cfg->tcp_port))
        DIE("bad tcp spec: %s", spec);
}

static void parse_args(int argc, char **argv, app_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->tunnel_mode = TUNNEL_MODE_NONE;
    cfg->tcp_role = PIK_CONTROL_TCP_NONE;
    if (argc < 2)
        usage(argv[0]);

    int argi = 1;
    if (strcmp(argv[argi], "--usb") == 0) {
        cfg->mode = APP_MODE_K1;
    } else if (strcmp(argv[argi], "--ffs") == 0) {
        cfg->mode = APP_MODE_PI;
    } else {
        usage(argv[0]);
    }
    argi++;

    bool k1_mode = cfg->mode == APP_MODE_K1;
    if (argi >= argc) usage(argv[0]);
    if (k1_mode && strncmp(argv[argi], "mcu:", 4) != 0)
        DIE("--usb is only valid on the K1/MCU side");
    if (!k1_mode && strncmp(argv[argi], "pty:", 4) != 0)
        DIE("--ffs is only valid on the Pi/PTY side");

    bool seen_ch[UINT8_MAX + 1] = { false };
    while (argi < argc) {
        char *spec = argv[argi];
        if (strncmp(spec, "listen:", 7) == 0 ||
            strncmp(spec, "forward:", 8) == 0)
            break;
        if (cfg->mux.n_channels >= MAX_CHANNELS)
            DIE("too many channels (max %d): %s", MAX_CHANNELS, spec);
        argi++;

        ch_spec_t *s = &cfg->mux.channels[cfg->mux.n_channels];
        memset(s, 0, sizeof(*s));
        if (k1_mode)
            parse_mcu_channel(spec, s);
        else
            parse_pty_channel(spec, s);

        if (!pik_mux_cli_valid(s->ch_id))
            DIE("channel id out of range (valid %u..%u): %s",
                PIK_MUX_CLI_BASE, PIK_MUX_CLI_LAST, spec);
        if (seen_ch[s->ch_id])
            DIE("duplicate channel id: %u", s->ch_id);
        seen_ch[s->ch_id] = true;
        cfg->mux.n_channels++;
    }
    if (!cfg->mux.n_channels) usage(argv[0]);

    if (argi < argc &&
        (strncmp(argv[argi], "listen:", 7) == 0 ||
         strncmp(argv[argi], "forward:", 8) == 0)) {
        parse_tcp_spec(cfg, argv[argi]);
        argi++;
    }

    if (argi != argc) DIE("unexpected argument: %s", argv[argi]);
}

typedef struct {
    const char *name;
    const char *wait_message;
    bool (*prepare)(void);
    bool (*start)(pik_link_t *link, int epfd, int64_t now);
    bool (*owns_event)(const void *ptr);
    bool (*dispatch)(void *ptr, uint32_t events, int64_t now);
    bool (*tick)(int64_t now);
    int64_t (*deadline)(void);
    void (*cleanup)(void);
} transport_ops_t;

static const transport_ops_t usb_host_ops = {
    .name = "usb-bulk",
    .wait_message = "waiting for USB bulk device",
    .start = pik_usb_host_start,
    .owns_event = pik_usb_host_owns_event,
    .dispatch = pik_usb_host_dispatch,
    .tick = pik_usb_host_tick,
    .deadline = pik_usb_host_deadline,
    .cleanup = pik_usb_host_cleanup,
};

static const transport_ops_t usb_gadget_ops = {
    .name = "ffs",
    .prepare = pik_usb_gadget_prepare,
    .start = pik_usb_gadget_start,
    .owns_event = pik_usb_gadget_owns_event,
    .dispatch = pik_usb_gadget_dispatch,
    .tick = pik_usb_gadget_tick,
    .deadline = pik_usb_gadget_deadline,
    .cleanup = pik_usb_gadget_cleanup,
};

typedef struct {
    const transport_ops_t *transport;
    int transport_backoff_ms;
} app_state_t;

static app_state_t g_app = {
    .transport_backoff_ms = RETRY_MIN_MS,
};

/* Start logical services only after the shared-link handshake completes. */
static void on_control_ready(void) {
    int64_t now = pik_now_ms();
    uint32_t services = PIK_CONTROL_SERVICE_SERIAL;
    g_app.transport_backoff_ms = RETRY_MIN_MS;
    serialmux_start(now);
    tunnel_start(now);
    if (tunnel_active())
        services |= PIK_CONTROL_SERVICE_TUNNEL;
    pik_commands_set_service_flags(services);
    LOG("services started");
}

static void session_teardown(void) {
    g_app.transport->cleanup();
    tunnel_cleanup();
    serialmux_cleanup();
    pik_control_cleanup();
    pik_session_cleanup();
    pik_commands_set_service_flags(0);
}

static bool start_process(const char *path, char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        sigset_t mask;
        sigemptyset(&mask);
        sigprocmask(SIG_SETMASK, &mask, NULL);
        signal(SIGCHLD, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        execvp(path, argv);
        _exit(127);
    }
    return true;
}

static bool start_utility(const char *name) {
    char path[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1u);
    if (n < 0)
        return false;
    path[n] = '\0';

    char *base = strrchr(path, '/');
    if (!base) {
        errno = ENOENT;
        return false;
    }
    base++;
    size_t remaining = sizeof(path) - (size_t)(base - path);
    int written = snprintf(base, remaining, "scripts/%s", name);
    if (written < 0 || (size_t)written >= remaining) {
        errno = ENAMETOOLONG;
        return false;
    }
    if (access(path, X_OK) < 0)
        return false;

    char *const argv[] = { path, NULL };
    return start_process(path, argv);
}

static void execute_remote_action(pik_control_action_t action, char **argv) {
    if (action == PIK_CONTROL_ACTION_RESTART_WIFI) {
        LOG("executing restart-wifi");
        if (!start_utility("restart-wifi.sh"))
            LOG("start restart-wifi.sh: %s", strerror(errno));
        return;
    }
    if (action == PIK_CONTROL_ACTION_RESTART_KLIPPER) {
        LOG("executing restart-klipper");
        char *const systemctl_argv[] = {
            "systemctl", "restart", "klipper.service", NULL
        };
        if (!start_process("systemctl", systemctl_argv))
            LOG("start systemctl: %s", strerror(errno));
        return;
    }

    if (action == PIK_CONTROL_ACTION_REBOOT ||
        action == PIK_CONTROL_ACTION_POWEROFF)
        pik_commands_mark_peer_initiated();
    session_teardown();
    pik_commands_cleanup();

    switch (action) {
    case PIK_CONTROL_ACTION_RESTART_PIK1:
        LOG("executing restart-pik1");
        execv(argv[0], argv);
        LOG("execv %s: %s", argv[0], strerror(errno));
        _exit(127);
    case PIK_CONTROL_ACTION_REBOOT:
        LOG("executing reboot");
        execl("/sbin/reboot", "reboot", (char *)NULL);
        LOG("execl /sbin/reboot: %s", strerror(errno));
        _exit(127);
    case PIK_CONTROL_ACTION_POWEROFF:
        LOG("executing poweroff");
        execl("/sbin/poweroff", "poweroff", (char *)NULL);
        LOG("execl /sbin/poweroff: %s", strerror(errno));
        _exit(127);
    default:
        _exit(1);
    }
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("pik1d %s protocol=%u\n",
               PIK1_RELEASE_VERSION, PIK1_PROTOCOL_VERSION);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "--control") == 0)
        return pik_commands_client_main(argv[2]);

    if (argc < 3) usage(argv[0]);

    app_config_t cfg;
    parse_args(argc, argv, &cfg);

    bool k1_mode = cfg.mode == APP_MODE_K1;
    g_app.transport = k1_mode ? &usb_host_ops : &usb_gadget_ops;
    const char *side_name = k1_mode ? "mcu" : "pty";
    pik_control_role_t control_role =
        k1_mode ? PIK_CONTROL_ROLE_MCU : PIK_CONTROL_ROLE_PTY;
    pik_commands_init(side_name, !k1_mode);
    pik_log_set_timestamps(k1_mode);

    if (cfg.tunnel_mode != TUNNEL_MODE_NONE)
        LOG("uart=%s release=%s protocol=%u channels=%d tcp=%s:%s:%d link=%s",
            side_name, PIK1_RELEASE_VERSION, PIK1_PROTOCOL_VERSION,
            cfg.mux.n_channels,
            cfg.tunnel_mode == TUNNEL_MODE_LISTEN ? "listen" : "forward",
            cfg.tcp_addr, cfg.tcp_port,
            g_app.transport->name);
    else
        LOG("uart=%s release=%s protocol=%u channels=%d link=%s",
            side_name, PIK1_RELEASE_VERSION, PIK1_PROTOCOL_VERSION,
            cfg.mux.n_channels, g_app.transport->name);

    if (g_app.transport->prepare && !g_app.transport->prepare())
        DIE("failed to prepare %s", g_app.transport->name);

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGUSR1);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);

    int sig_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sig_fd < 0) DIE("signalfd: %s", strerror(errno));

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) DIE("epoll_create1: %s", strerror(errno));

    static int sig_tag;
    if (!pik_epoll_set(epfd, sig_fd, EPOLLIN, &sig_tag))
        DIE("epoll add signal fd: %s", strerror(errno));
    pik_session_init();
    pik_control_init(control_role, pik_commands_on_command,
                     on_control_ready);
    {
        uint8_t channel_ids[MAX_CHANNELS];
        for (int i = 0; i < cfg.mux.n_channels; i++)
            channel_ids[i] = cfg.mux.channels[i].ch_id;
        pik_control_set_config(channel_ids, (size_t)cfg.mux.n_channels, cfg.tcp_role);
    }
    if (!pik_commands_start(epfd))
        DIE("failed to start local control socket");
    serialmux_init(&cfg.mux, epfd);
    tunnel_init(epfd, cfg.tunnel_mode, cfg.tcp_addr, cfg.tcp_port);

    bool shutdown = false;
    int exit_status = 0;
    bool session_active = false;
    int64_t transport_retry_at = pik_now_ms();
    bool transport_waiting_logged = false;

    while (!shutdown) {
        int64_t now = pik_now_ms();

        if (!session_active && now >= transport_retry_at) {
            if (g_app.transport->start(pik_session_link(), epfd, now) &&
                pik_control_on_link_open()) {
                transport_waiting_logged = false;
                pik_commands_set_service_flags(0);
                session_active = true;
                transport_retry_at = 0;
                if (pik_control_handshake_failures() == 0)
                    LOG("session started: %s", g_app.transport->name);
            } else {
                if (g_app.transport->wait_message) {
                    if (!transport_waiting_logged) {
                        LOG("%s", g_app.transport->wait_message);
                        transport_waiting_logged = true;
                    }
                } else {
                    transport_waiting_logged = false;
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
            int64_t xd = g_app.transport->deadline();
            if (xd < dl) dl = xd;
        }
        int64_t ld = pik_commands_deadline();
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
                        pik_commands_request_restart_pik1(cfg.mode == APP_MODE_PI,
                                                          now);
                    }
                }
                continue;
            }

            if (pik_commands_owns_event(ptr)) {
                pik_commands_dispatch(ptr, now);
                continue;
            }

            if (!session_active) continue;

            if (g_app.transport->owns_event(ptr)) {
                if (!g_app.transport->dispatch(ptr, ev, now))
                    session_failed = true;
            } else if (serialmux_owns_event(ptr)) {
                if (!serialmux_dispatch(ptr, ev, now))
                    session_failed = true;
            } else if (tunnel_owns_event(ptr)) {
                if (!tunnel_dispatch(ptr, ev))
                    session_failed = true;
            }
        }

        if (pik_commands_signal_done()) shutdown = true;
        if (shutdown) break;

        now = pik_now_ms();
        pik_commands_check_acks(now);

        if (session_active && !session_failed) {
            if (!pik_control_tick(now) ||
                !pik_session_tick(now) ||
                !g_app.transport->tick(now) ||
                !serialmux_tick(now) ||
                !tunnel_tick(now))
                session_failed = true;
        }

        if (session_active && !session_failed)
            pik_commands_set_service_flag(PIK_CONTROL_SERVICE_TUNNEL,
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
        if (pik_commands_action_due(now, &remote_action))
            execute_remote_action(remote_action, argv);
    }

    session_teardown();
    pik_commands_cleanup();
    close(sig_fd);
    close(epfd);
    return exit_status;
}
