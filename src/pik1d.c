// src/pik1d.c - daemon supervisor: USB discovery, child management, mux session loop

#include "control.h"
#include "serialmux.h"
#include "fd.h"
#include "logging.h"
#include "util.h"
#include "usb_discovery.h"
#include "version.h"

#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define RETRY_MIN_MS 1000
#define RETRY_MAX_MS 30000
#define MAX_EVENTS 32
#define LOCAL_CONTROL_SOCK "/run/pik1/control.sock"
#define COMMAND_ACK_TIMEOUT_MS 3000
#define REMOTE_ACTION_DELAY_MS 250

#define LOG(...) pik_log("pik1", __VA_ARGS__)
#define DIE(...) pik_die("pik1", __VA_ARGS__)

// ── local control client/server ───────────────────────────────────────────────
typedef struct {
    int listen_fd;
    int client_fd;
    int epfd;
    bool command_pending;
    uint32_t request_id;
    int64_t deadline_ms;
} local_control_t;

static local_control_t g_local = {
    .listen_fd = -1,
    .client_fd = -1,
    .epfd = -1,
};
static int g_local_listen_tag;

static pik_control_action_t g_remote_action;
static bool g_remote_action_pending;
static int64_t g_remote_action_at_ms;
static bool g_signal_command_pending;
static bool g_signal_command_done;
static uint32_t g_signal_request_id;
static int64_t g_signal_deadline_ms;
static char **g_argv;

static int connect_local_control(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", LOCAL_CONTROL_SOCK);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int control_client_main(const char *cmd) {
    int fd = connect_local_control();
    if (fd < 0) {
        fprintf(stderr, "pik1d: connect %s: %s\n", LOCAL_CONTROL_SOCK, strerror(errno));
        return 1;
    }
    dprintf(fd, "%s\n", cmd);

    char buf[128];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n < 0) {
        fprintf(stderr, "pik1d: read control response: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    buf[n > 0 ? n : 0] = '\0';
    fputs(buf, stdout);
    close(fd);
    return strncmp(buf, "OK", 2) == 0 ? 0 : 1;
}

static bool parse_control_action(const char *cmd, pik_control_action_t *action) {
    if (strcmp(cmd, "restart-peer") == 0) {
        *action = PIK_CONTROL_ACTION_RESTART_EXPORTER;
        return true;
    }
    if (strcmp(cmd, "reboot-peer") == 0) {
        *action = PIK_CONTROL_ACTION_REBOOT_EXPORTER;
        return true;
    }
    if (strcmp(cmd, "poweroff-peer") == 0) {
        *action = PIK_CONTROL_ACTION_POWEROFF_EXPORTER;
        return true;
    }
    return false;
}

static void local_control_close_client(void) {
    if (g_local.client_fd >= 0) {
        pik_epoll_del(g_local.epfd, g_local.client_fd);
        close(g_local.client_fd);
        g_local.client_fd = -1;
    }
    g_local.command_pending = false;
}

static bool local_control_start(int epfd) {
    g_local.epfd = epfd;
    g_local.listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (g_local.listen_fd < 0) {
        LOG("local control socket: %s", strerror(errno));
        return false;
    }

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", LOCAL_CONTROL_SOCK);
    unlink(LOCAL_CONTROL_SOCK);
    if (bind(g_local.listen_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        LOG("bind %s: %s", LOCAL_CONTROL_SOCK, strerror(errno));
        close(g_local.listen_fd);
        g_local.listen_fd = -1;
        return false;
    }
    if (chmod(LOCAL_CONTROL_SOCK, 0600) < 0) {
        LOG("chmod %s: %s", LOCAL_CONTROL_SOCK, strerror(errno));
        close(g_local.listen_fd);
        g_local.listen_fd = -1;
        unlink(LOCAL_CONTROL_SOCK);
        return false;
    }
    if (listen(g_local.listen_fd, 4) < 0) {
        LOG("listen %s: %s", LOCAL_CONTROL_SOCK, strerror(errno));
        close(g_local.listen_fd);
        g_local.listen_fd = -1;
        unlink(LOCAL_CONTROL_SOCK);
        return false;
    }
    pik_epoll_set(epfd, g_local.listen_fd, EPOLLIN, &g_local_listen_tag);
    return true;
}

static void local_control_cleanup(void) {
    local_control_close_client();
    if (g_local.listen_fd >= 0) {
        pik_epoll_del(g_local.epfd, g_local.listen_fd);
        close(g_local.listen_fd);
        g_local.listen_fd = -1;
        unlink(LOCAL_CONTROL_SOCK);
    }
}

static void local_control_write_response(int fd, const char *msg) {
    size_t off = 0;
    size_t len = strlen(msg);
    while (off < len) {
        ssize_t n = write(fd, msg + off, len - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        off += (size_t)n;
    }
}

static void local_control_reply_and_close(const char *msg) {
    if (g_local.client_fd >= 0)
        local_control_write_response(g_local.client_fd, msg);
    local_control_close_client();
}

static void local_control_accept(void) {
    int fd = accept4(g_local.listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) return;
    if (g_local.client_fd >= 0) {
        local_control_write_response(fd, "ERR busy\n");
        close(fd);
        return;
    }
    g_local.client_fd = fd;
    pik_epoll_set(g_local.epfd, fd, EPOLLIN, &g_local);
}

static void local_control_read(int64_t now) {
    char buf[64];
    ssize_t n = read(g_local.client_fd, buf, sizeof(buf) - 1);
    if (n <= 0) {
        local_control_close_client();
        return;
    }
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) n--;
    buf[n] = '\0';

    pik_control_action_t action;
    if (!parse_control_action(buf, &action)) {
        local_control_reply_and_close("ERR unknown command\n");
        return;
    }
    if (g_local.command_pending) {
        local_control_reply_and_close("ERR busy\n");
        return;
    }
    if (!pik_control_send_command(action, &g_local.request_id)) {
        local_control_reply_and_close("ERR peer not ready\n");
        return;
    }
    g_local.command_pending = true;
    g_local.deadline_ms = now + COMMAND_ACK_TIMEOUT_MS;
}

static void local_control_check_ack(int64_t now) {
    uint32_t request_id;
    uint8_t status;
    while (pik_control_take_ack(&request_id, &status)) {
        if (g_local.command_pending && request_id == g_local.request_id) {
            local_control_reply_and_close(status == 0 ? "OK\n" : "ERR peer command failed\n");
        } else if (g_signal_command_pending && request_id == g_signal_request_id) {
            LOG("restart command ack status=%u", status);
            g_signal_command_pending = false;
            g_signal_command_done = true;
        } else {
            LOG("command ack request=%u status=%u", request_id, status);
        }
    }
    if (g_local.command_pending && now >= g_local.deadline_ms)
        local_control_reply_and_close("ERR peer ack timeout\n");
    if (g_signal_command_pending && now >= g_signal_deadline_ms) {
        LOG("restart command ack timeout");
        g_signal_command_pending = false;
        g_signal_command_done = true;
    }
}

static int64_t local_control_deadline(void) {
    int64_t dl = g_local.command_pending ? g_local.deadline_ms : INT64_MAX;
    if (g_signal_command_pending && g_signal_deadline_ms < dl)
        dl = g_signal_deadline_ms;
    return dl;
}

static void on_control_command(pik_control_action_t action, uint32_t request_id, void *ctx) {
    (void)ctx;
    LOG("received command %s request=%u", pik_control_action_name(action), request_id);
    pik_control_send_ack(request_id, 0);
    g_remote_action = action;
    g_remote_action_pending = true;
    g_remote_action_at_ms = pik_now_ms() + REMOTE_ACTION_DELAY_MS;
}

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
        LOG("child: execvp %s: %s", g_child.argv[0], strerror(errno));
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

static void execute_remote_action(pik_control_action_t action) {
    child_stop();
    serialmux_cleanup();
    pik_control_cleanup();
    local_control_cleanup();

    switch (action) {
    case PIK_CONTROL_ACTION_RESTART_EXPORTER:
        LOG("executing restart-exporter");
        execv(g_argv[0], g_argv);
        LOG("execv %s: %s", g_argv[0], strerror(errno));
        _exit(127);
    case PIK_CONTROL_ACTION_REBOOT_EXPORTER:
        LOG("executing reboot-exporter");
        execl("/sbin/reboot", "reboot", (char *)NULL);
        LOG("execl /sbin/reboot: %s", strerror(errno));
        _exit(127);
    case PIK_CONTROL_ACTION_POWEROFF_EXPORTER:
        LOG("executing poweroff-exporter");
        execl("/sbin/poweroff", "poweroff", (char *)NULL);
        LOG("execl /sbin/poweroff: %s", strerror(errno));
        _exit(127);
    default:
        _exit(1);
    }
}

// ── USB session supervision ──────────────────────────────────────────────────
typedef enum {
    USB_WAIT_UNKNOWN = -1,
    USB_WAIT_NONE = 0,
    USB_WAIT_CONTROL_ONLY = 1,
    USB_WAIT_SERIAL_ONLY = 2,
    USB_WAIT_READY = 3,
} usb_wait_state_t;

static usb_wait_state_t usb_resolve(const char *vidpid, bool has_tcp,
                                    char *control_dev, size_t control_cap,
                                    char *serial_dev, size_t serial_cap,
                                    char *tunnel_dev, size_t tunnel_cap) {
    const char *control = usb_find_serial_dev(vidpid, 0);
    if (!control) return USB_WAIT_NONE;
    snprintf(control_dev, control_cap, "%s", control);

    const char *serial = usb_find_serial_dev(vidpid, 1);
    if (!serial) return USB_WAIT_CONTROL_ONLY;
    snprintf(serial_dev, serial_cap, "%s", serial);

    if (!has_tcp) return USB_WAIT_READY;

    const char *tunnel = usb_find_serial_dev(vidpid, 2);
    if (!tunnel) return USB_WAIT_SERIAL_ONLY;
    snprintf(tunnel_dev, tunnel_cap, "%s", tunnel);
    return USB_WAIT_READY;
}

static void usb_log_wait_state(usb_wait_state_t state, bool has_tcp) {
    switch (state) {
    case USB_WAIT_NONE:
        LOG("waiting for USB endpoint 0");
        break;
    case USB_WAIT_CONTROL_ONLY:
        LOG("waiting for USB endpoint 1");
        break;
    case USB_WAIT_SERIAL_ONLY:
        LOG("waiting for USB endpoint 2");
        break;
    case USB_WAIT_READY:
        LOG("USB endpoints ready%s", has_tcp ? " (0,1,2)" : " (0,1)");
        break;
    default:
        break;
    }
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage:\n"
        "  %s --version\n"
        "  %s --control restart-peer|reboot-peer|poweroff-peer\n"
        "  %s --usb VID:PID  mcu:N:DEV:BAUD [...] [tcp:ADDR:PORT]\n"
        "  %s --usb VID:PID  pty:N:SYMLINK  [...] [tcp:HOST:PORT]\n",
        prog, prog, prog, prog);
    exit(1);
}

int main(int argc, char **argv) {
    g_argv = argv;

    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("pik1d %s protocol=%u features=0x%08x\n",
               PIK1_RELEASE_VERSION, PIK1_PROTOCOL_VERSION, PIK1_FEATURE_FLAGS);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "--control") == 0)
        return control_client_main(argv[2]);

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
        LOG("mode=%s release=%s protocol=%u channels=%d tcp=%s:%d control=usb[0] serial=usb[1] tunnel=usb[2]",
            is_mcu ? "exporter" : "host", PIK1_RELEASE_VERSION,
            PIK1_PROTOCOL_VERSION, cfg.n_channels, tcp_addr, tcp_port);
    else
        LOG("mode=%s release=%s protocol=%u channels=%d control=usb[0] serial=usb[1]",
            is_mcu ? "exporter" : "host", PIK1_RELEASE_VERSION,
            PIK1_PROTOCOL_VERSION, cfg.n_channels);

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGUSR1);
    sigprocmask(SIG_BLOCK, &mask, NULL);

    int sig_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sig_fd < 0) DIE("signalfd: %s", strerror(errno));

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) DIE("epoll_create1: %s", strerror(errno));

    static int sig_tag;
    pik_epoll_set(epfd, sig_fd, EPOLLIN, &sig_tag);
    pik_control_init(epfd, is_mcu ? PIK_CONTROL_ROLE_EXPORTER : PIK_CONTROL_ROLE_HOST,
                     on_control_command, NULL);
    if (!is_mcu && !local_control_start(epfd))
        DIE("failed to start local control socket");
    serialmux_init(&cfg, epfd);

    bool shutdown = false;
    bool session_active = false;
    bool session_confirmed = false;
    int usb_backoff_ms = RETRY_MIN_MS;
    int64_t usb_retry_at = pik_now_ms();
    usb_wait_state_t last_usb_state = USB_WAIT_UNKNOWN;
    char control_dev[64] = "";
    char serial_dev[64] = "";
    char tunnel_dev[64] = "";

    while (!shutdown) {
        int64_t now = pik_now_ms();

        if (!session_active && now >= usb_retry_at) {
            usb_wait_state_t state = usb_resolve(vidpid, has_tcp,
                                                 control_dev, sizeof(control_dev),
                                                 serial_dev, sizeof(serial_dev),
                                                 tunnel_dev, sizeof(tunnel_dev));
            if (state != last_usb_state) {
                usb_log_wait_state(state, has_tcp);
                last_usb_state = state;
            }

            if (state == USB_WAIT_READY) {
                if (pik_control_start(control_dev, now)) {
                    session_active = true;
                    session_confirmed = false;
                    usb_retry_at = 0;
                    LOG("control session started: control=%s serial=%s%s%s",
                        control_dev, serial_dev, has_tcp ? " tunnel=" : "",
                        has_tcp ? tunnel_dev : "");
                } else {
                    pik_control_cleanup();
                    serialmux_cleanup();
                    usb_retry_at = now + pik_backoff_next(&usb_backoff_ms, RETRY_MAX_MS);
                }
            } else {
                usb_retry_at = now + pik_backoff_next(&usb_backoff_ms, RETRY_MAX_MS);
            }
        }

        if (session_active && pik_control_ready() && !session_confirmed) {
            if (serialmux_start(serial_dev, now)) {
                session_confirmed = true;
                usb_backoff_ms = RETRY_MIN_MS;
                LOG("data session started: serial=%s%s%s",
                    serial_dev, has_tcp ? " tunnel=" : "",
                    has_tcp ? tunnel_dev : "");
                if (has_tcp)
                    child_spawn(tunnel_dev, now);
            } else {
                LOG("serial mux failed to start");
                session_active = false;
                session_confirmed = false;
                child_stop();
                serialmux_cleanup();
                pik_control_cleanup();
                usb_retry_at = now + pik_backoff_next(&usb_backoff_ms, RETRY_MAX_MS);
                last_usb_state = USB_WAIT_UNKNOWN;
            }
        }

        int64_t dl = session_active ? pik_control_deadline() : usb_retry_at;
        if (session_confirmed) {
            int64_t sd = serialmux_deadline(now);
            if (sd < dl) dl = sd;
        }
        int64_t cd = child_deadline();
        if (cd < dl) dl = cd;
        int64_t ld = local_control_deadline();
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
                    } else if (si.ssi_signo == SIGUSR1) {
                        if (!is_mcu && !g_signal_command_pending && !g_signal_command_done &&
                            pik_control_send_command(PIK_CONTROL_ACTION_RESTART_EXPORTER,
                                                     &g_signal_request_id)) {
                            g_signal_command_pending = true;
                            g_signal_deadline_ms = now + COMMAND_ACK_TIMEOUT_MS;
                        } else {
                            g_signal_command_done = true;
                        }
                    } else if (si.ssi_signo == SIGCHLD) {
                        child_handle_sigchld(now, session_active);
                    }
                }
                continue;
            }

            if (pik_control_owns_event(ptr)) {
                if (session_active && !pik_control_dispatch(ptr, ev, now)) {
                    LOG("control session failed, restarting USB link");
                    session_active = false;
                    session_confirmed = false;
                    child_stop();
                    serialmux_cleanup();
                    pik_control_cleanup();
                    usb_retry_at = now + pik_backoff_next(&usb_backoff_ms, RETRY_MAX_MS);
                    last_usb_state = USB_WAIT_UNKNOWN;
                }
                continue;
            }

            if (ptr == &g_local_listen_tag) {
                local_control_accept();
                continue;
            }

            if (ptr == &g_local) {
                local_control_read(now);
                continue;
            }

            if (session_confirmed && !serialmux_dispatch(ptr, ev, now)) {
                LOG("session failed, restarting USB link");
                session_active = false;
                session_confirmed = false;
                child_stop();
                serialmux_cleanup();
                pik_control_cleanup();
                usb_retry_at = now + pik_backoff_next(&usb_backoff_ms, RETRY_MAX_MS);
                last_usb_state = USB_WAIT_UNKNOWN;
            }
        }

        if (g_signal_command_done) shutdown = true;
        if (shutdown) break;

        now = pik_now_ms();
        local_control_check_ack(now);

        if (session_active && !pik_control_tick(now)) {
            LOG("control session failed, restarting USB link");
            session_active = false;
            session_confirmed = false;
            child_stop();
            serialmux_cleanup();
            pik_control_cleanup();
            usb_retry_at = now + pik_backoff_next(&usb_backoff_ms, RETRY_MAX_MS);
            last_usb_state = USB_WAIT_UNKNOWN;
        }

        if (session_confirmed && !serialmux_tick(now)) {
            LOG("session failed, restarting USB link");
            session_active = false;
            session_confirmed = false;
            child_stop();
            serialmux_cleanup();
            pik_control_cleanup();
            usb_retry_at = now + pik_backoff_next(&usb_backoff_ms, RETRY_MAX_MS);
            last_usb_state = USB_WAIT_UNKNOWN;
        }

        if (session_active && has_tcp &&
            g_child.pid < 0 && g_child.restart_at_ms && now >= g_child.restart_at_ms) {
            const char *td = usb_find_serial_dev(vidpid, 2);
            if (td) {
                snprintf(tunnel_dev, sizeof(tunnel_dev), "%s", td);
                child_spawn(tunnel_dev, now);
            } else {
                if (!g_child.waiting_for_tunnel) {
                    LOG("child: waiting for tunnel device 2");
                    g_child.waiting_for_tunnel = true;
                }
                child_schedule_restart(now);
            }
        }

        if (g_remote_action_pending && now >= g_remote_action_at_ms)
            execute_remote_action(g_remote_action);
    }

    child_stop();
    serialmux_cleanup();
    pik_control_cleanup();
    local_control_cleanup();
    close(sig_fd);
    close(epfd);
    return 0;
}
