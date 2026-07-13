// pik1d daemon supervisor: USB discovery, child management, mux session loop

#include "control.h"
#include "serialmux.h"
#include "fd.h"
#include "logging.h"
#include "util.h"
#include "usb_discovery.h"
#include "version.h"

#include <errno.h>
#include <fcntl.h>
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
#include <poll.h>
#include <sys/signalfd.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define RETRY_MIN_MS 1000
#define RETRY_MAX_MS 30000
#define MAX_EVENTS 32
#ifndef LOCAL_CONTROL_DIR
#define LOCAL_CONTROL_DIR "/run/pik1"
#endif
#ifndef LOCAL_CONTROL_SOCK
#define LOCAL_CONTROL_SOCK LOCAL_CONTROL_DIR "/control.sock"
#endif
#ifndef PEER_INITIATED_MARKER
#define PEER_INITIATED_MARKER LOCAL_CONTROL_DIR "/peer-initiated"
#endif
#define TCP_STATUS_FD_ENV "PIK1_TCP_STATUS_FD"
#define CONTROL_SOCK_ENV "PIK1_CONTROL_SOCK"
#define COMMAND_ACK_TIMEOUT_MS 3000
#define LOCAL_CONTROL_WRITE_TIMEOUT_MS 1000
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

static const char *g_uart_name;
static uint32_t g_link_flags;
static pik_control_action_t g_remote_action;
static bool g_remote_action_pending;
static int64_t g_remote_action_at_ms;
static bool g_signal_command_pending;
static bool g_signal_command_done;
static uint32_t g_signal_request_id;
static int64_t g_signal_deadline_ms;
static char **g_argv;

static const char *local_control_sock_path(void) {
    const char *p = getenv(CONTROL_SOCK_ENV);
    return (p && *p) ? p : LOCAL_CONTROL_SOCK;
}

static int connect_local_control(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", local_control_sock_path());
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int control_client_main(const char *cmd) {
    int fd = connect_local_control();
    if (fd < 0) {
        fprintf(stderr, "pik1d: connect %s: %s\n", local_control_sock_path(), strerror(errno));
        return 1;
    }
    dprintf(fd, "%s\n", cmd);

    char buf[192];
    size_t len = 0;
    while (len < sizeof(buf) - 1) {
        ssize_t n = read(fd, buf + len, sizeof(buf) - 1 - len);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            fprintf(stderr, "pik1d: read control response: %s\n", strerror(errno));
            close(fd);
            return 1;
        }
        if (n == 0) break;
        len += (size_t)n;
    }
    buf[len] = '\0';
    fputs(buf, stdout);
    close(fd);
    return strncmp(buf, "OK", 2) == 0 ? 0 : 1;
}

static bool parse_control_action(const char *cmd, pik_control_action_t *action) {
    if (strcmp(cmd, "restart-peer") == 0) {
        *action = PIK_CONTROL_ACTION_RESTART_PEER;
        return true;
    }
    if (strcmp(cmd, "reboot-peer") == 0) {
        *action = PIK_CONTROL_ACTION_REBOOT_PEER;
        return true;
    }
    if (strcmp(cmd, "poweroff-peer") == 0) {
        *action = PIK_CONTROL_ACTION_POWEROFF_PEER;
        return true;
    }
    if (strcmp(cmd, "status-peer") == 0) {
        *action = PIK_CONTROL_ACTION_STATUS;
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
    const char *sock_path = local_control_sock_path();
    g_local.epfd = epfd;
    if (!getenv(CONTROL_SOCK_ENV) &&
        mkdir(LOCAL_CONTROL_DIR, 0700) < 0 && errno != EEXIST) {
        LOG("mkdir %s: %s", LOCAL_CONTROL_DIR, strerror(errno));
        return false;
    }
    g_local.listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (g_local.listen_fd < 0) {
        LOG("local control socket: %s", strerror(errno));
        return false;
    }

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", sock_path);
    unlink(sock_path);
    if (bind(g_local.listen_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        LOG("bind %s: %s", sock_path, strerror(errno));
        close(g_local.listen_fd);
        g_local.listen_fd = -1;
        return false;
    }
    if (chmod(sock_path, 0600) < 0) {
        LOG("chmod %s: %s", sock_path, strerror(errno));
        close(g_local.listen_fd);
        g_local.listen_fd = -1;
        unlink(sock_path);
        return false;
    }
    if (listen(g_local.listen_fd, 4) < 0) {
        LOG("listen %s: %s", sock_path, strerror(errno));
        close(g_local.listen_fd);
        g_local.listen_fd = -1;
        unlink(sock_path);
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
        unlink(local_control_sock_path());
    }
}

static bool local_control_write(int fd, const void *buf, size_t len) {
    const char *p = buf;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, p + off, len - off);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd = { .fd = fd, .events = POLLOUT };
            int r = poll(&pfd, 1, LOCAL_CONTROL_WRITE_TIMEOUT_MS);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) {
                LOG("local control write timeout after %zu/%zu bytes", off, len);
                return false;
            }
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                LOG("local control write failed after %zu/%zu bytes", off, len);
                return false;
            }
            continue;
        }
        if (n <= 0) {
            LOG("local control write: %s", n == 0 ? "EOF" : strerror(errno));
            return false;
        }
        off += (size_t)n;
    }
    return true;
}

static void local_control_reply_and_close(const char *msg) {
    if (g_local.client_fd >= 0 &&
        !local_control_write(g_local.client_fd, msg, strlen(msg)))
        LOG("local control response was not fully delivered");
    local_control_close_client();
}

static void local_control_reply_payload_and_close(const uint8_t *payload,
                                                  size_t payload_len) {
    char msg[160];
    int n = snprintf(msg, sizeof(msg), "OK %.*s\n",
                     (int)payload_len, (const char *)payload);
    if (n < 0 || (size_t)n >= sizeof(msg)) {
        local_control_reply_and_close("ERR response too large\n");
        return;
    }
    local_control_reply_and_close(msg);
}

static void local_control_reply_error_status(pik_control_ack_status_t status,
                                             const uint8_t *payload,
                                             size_t payload_len) {
    char msg[160];
    if (!payload)
        payload_len = 0;
    snprintf(msg, sizeof(msg), "ERR peer %s%s%.*s\n",
             pik_control_ack_status_name(status),
             payload_len ? " " : "", (int)payload_len,
             payload ? (const char *)payload : "");
    local_control_reply_and_close(msg);
}

static void local_control_accept(void) {
    int fd = accept4(g_local.listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) return;
    if (g_local.client_fd >= 0) {
        if (!local_control_write(fd, "ERR busy\n", strlen("ERR busy\n")))
            LOG("local control busy response was not fully delivered");
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
    if (g_local.command_pending || g_signal_command_pending) {
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
    pik_control_ack_status_t status;
    const uint8_t *payload;
    size_t payload_len;
    while (pik_control_take_ack(&request_id, &status, &payload, &payload_len)) {
        if (g_local.command_pending && request_id == g_local.request_id) {
            if (status == PIK_CONTROL_ACK_OK && payload_len) {
                local_control_reply_payload_and_close(payload, payload_len);
            } else {
                if (status == PIK_CONTROL_ACK_OK)
                    local_control_reply_and_close("OK\n");
                else
                    local_control_reply_error_status(status, payload, payload_len);
            }
        } else if (g_signal_command_pending && request_id == g_signal_request_id) {
            LOG("restart command ack status=%s", pik_control_ack_status_name(status));
            g_signal_command_pending = false;
            g_signal_command_done = true;
        } else {
            LOG("command ack request=%u status=%s",
                request_id, pik_control_ack_status_name(status));
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

static void append_link_names(char *buf, size_t cap, uint32_t flags) {
    if (flags == 0) {
        snprintf(buf, cap, "none");
    } else if ((flags & (PIK_CONTROL_LINK_SERIAL | PIK_CONTROL_LINK_TCP)) ==
               (PIK_CONTROL_LINK_SERIAL | PIK_CONTROL_LINK_TCP)) {
        snprintf(buf, cap, "serial,tcp");
    } else if (flags & PIK_CONTROL_LINK_SERIAL) {
        snprintf(buf, cap, "serial");
    } else if (flags & PIK_CONTROL_LINK_TCP) {
        snprintf(buf, cap, "tcp");
    } else {
        snprintf(buf, cap, "unknown");
    }
}

static void set_link_flags(uint32_t flags) {
    if (g_link_flags == flags) return;
    g_link_flags = flags;
    if (pik_control_ready() && !pik_control_send_link_state(g_link_flags))
        LOG("failed to send link state update");
}

static void set_link_flag(uint32_t flag, bool up) {
    uint32_t flags = up ? (g_link_flags | flag) : (g_link_flags & ~flag);
    set_link_flags(flags);
}

static void on_control_command(pik_control_action_t action, uint32_t request_id, void *ctx) {
    (void)ctx;
    LOG("received command %s request=%u", pik_control_action_name(action), request_id);
    if (action == PIK_CONTROL_ACTION_STATUS) {
        char links[24];
        char peer_links[24];
        uint32_t peer_flags;
        append_link_names(links, sizeof(links), g_link_flags);
        if (pik_control_peer_link_state(&peer_flags))
            append_link_names(peer_links, sizeof(peer_links), peer_flags);
        else
            snprintf(peer_links, sizeof(peer_links), "unknown");

        char status[128];
        int n = snprintf(status, sizeof(status),
                         "uart=%s release=%s proto=%u feat=0x%08x links=%s peer=%s",
                         g_uart_name, PIK1_RELEASE_VERSION, PIK1_PROTOCOL_VERSION,
                         PIK1_FEATURE_FLAGS, links, peer_links);
        if (n < 0) {
            if (!pik_control_send_ack(request_id, PIK_CONTROL_ACK_INTERNAL_ERROR, NULL, 0))
                LOG("failed to send status error ACK request=%u", request_id);
            return;
        }
        size_t len = (size_t)n < sizeof(status) ? (size_t)n : sizeof(status) - 1;
        if (!pik_control_send_ack(request_id, PIK_CONTROL_ACK_OK, (const uint8_t *)status, len))
            LOG("failed to send status ACK request=%u", request_id);
        return;
    }

    if (!pik_control_send_ack(request_id, PIK_CONTROL_ACK_OK, NULL, 0)) {
        LOG("failed to send command ACK request=%u", request_id);
        return;
    }
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
    int     epfd;
    int64_t restart_at_ms;
    int     backoff_ms;
    int     status_fd;
    bool    intentional_stop;
    bool    waiting_for_tunnel;
} g_child = {
    .epfd = -1,
    .pid = -1,
    .status_fd = -1,
    .backoff_ms = RETRY_MIN_MS,
};
static int g_child_status_tag;

static void child_schedule_restart(int64_t now) {
    g_child.restart_at_ms = now + pik_backoff_next(&g_child.backoff_ms, RETRY_MAX_MS);
}

static void child_close_status_pipe(void) {
    if (g_child.status_fd < 0) return;
    pik_epoll_del(g_child.epfd, g_child.status_fd);
    close(g_child.status_fd);
    g_child.status_fd = -1;
}

static void child_status_read(void) {
    char buf[32];
    while (g_child.status_fd >= 0) {
        ssize_t n = read(g_child.status_fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            LOG("child: status pipe read: %s", strerror(errno));
            child_close_status_pipe();
            set_link_flag(PIK_CONTROL_LINK_TCP, false);
            return;
        }
        if (n == 0) {
            child_close_status_pipe();
            set_link_flag(PIK_CONTROL_LINK_TCP, false);
            return;
        }
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == 'U')
                set_link_flag(PIK_CONTROL_LINK_TCP, true);
            else if (buf[i] == 'D')
                set_link_flag(PIK_CONTROL_LINK_TCP, false);
        }
    }
}

static bool child_make_status_pipe(int fds[2]) {
    if (pipe(fds) < 0) {
        LOG("child: status pipe: %s", strerror(errno));
        return false;
    }

    int flags = fcntl(fds[0], F_GETFL, 0);
    if (flags < 0 || fcntl(fds[0], F_SETFL, flags | O_NONBLOCK) < 0) {
        LOG("child: status pipe nonblock: %s", strerror(errno));
        close(fds[0]);
        close(fds[1]);
        return false;
    }

    flags = fcntl(fds[0], F_GETFD, 0);
    if (flags < 0 || fcntl(fds[0], F_SETFD, flags | FD_CLOEXEC) < 0) {
        LOG("child: status pipe cloexec: %s", strerror(errno));
        close(fds[0]);
        close(fds[1]);
        return false;
    }

    return true;
}

static void child_spawn(const char *tunnel_dev, int64_t now) {
    if (!g_child.enabled || g_child.pid >= 0) return;

    snprintf(g_child.tunnel_dev, sizeof(g_child.tunnel_dev), "%s", tunnel_dev);
    g_child.argv[1] = g_child.tunnel_dev;

    int status_pipe[2] = { -1, -1 };
    if (!child_make_status_pipe(status_pipe)) {
        child_schedule_restart(now);
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        LOG("child: fork: %s", strerror(errno));
        close(status_pipe[0]);
        close(status_pipe[1]);
        child_schedule_restart(now);
        return;
    }
    if (pid == 0) {
        sigset_t all;
        sigfillset(&all);
        sigprocmask(SIG_UNBLOCK, &all, NULL);
        close(status_pipe[0]);
        char fd_env[16];
        snprintf(fd_env, sizeof(fd_env), "%d", status_pipe[1]);
        setenv(TCP_STATUS_FD_ENV, fd_env, 1);
        execvp(g_child.argv[0], g_child.argv);
        LOG("child: execvp %s: %s", g_child.argv[0], strerror(errno));
        _exit(127);
    }

    close(status_pipe[1]);
    g_child.status_fd = status_pipe[0];
    pik_epoll_set(g_child.epfd, g_child.status_fd, EPOLLIN, &g_child_status_tag);
    g_child.pid = pid;
    g_child.restart_at_ms = 0;
    g_child.backoff_ms = RETRY_MIN_MS;
    g_child.intentional_stop = false;
    g_child.waiting_for_tunnel = false;
    set_link_flag(PIK_CONTROL_LINK_TCP, false);
    LOG("child: spawned %s pid=%d (TCP link state pending)", g_child.argv[0], pid);
}

static void child_stop(void) {
    if (g_child.pid <= 0) {
        g_child.pid = -1;
        g_child.restart_at_ms = 0;
        child_close_status_pipe();
        set_link_flag(PIK_CONTROL_LINK_TCP, false);
        return;
    }

    pid_t pid = g_child.pid;
    g_child.intentional_stop = true;
    kill(pid, SIGTERM);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
    g_child.pid = -1;
    g_child.restart_at_ms = 0;
    child_close_status_pipe();
    set_link_flag(PIK_CONTROL_LINK_TCP, false);
    g_child.intentional_stop = false;
}

static void child_handle_sigchld(int64_t now, bool session_active) {
    if (g_child.pid <= 0) return;

    int status = 0;
    pid_t r = waitpid(g_child.pid, &status, WNOHANG);
    if (r <= 0) return;

    g_child.pid = -1;
    child_close_status_pipe();
    set_link_flag(PIK_CONTROL_LINK_TCP, false);
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

/* Tear down everything tied to the current USB control session. */
static void session_teardown(void) {
    child_stop();
    serialmux_cleanup();
    pik_control_cleanup();
}

/* A peer-commanded reboot/poweroff must not be relayed back by our own
 * shutdown hooks: the Pi's pik1-peer-* units are gated on this marker via
 * ConditionPathExists. Best-effort; /run is tmpfs so it clears on boot. */
static void mark_peer_initiated(void) {
    int fd = open(PEER_INITIATED_MARKER, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        LOG("marker %s: %s", PEER_INITIATED_MARKER, strerror(errno));
        return;
    }
    close(fd);
}

static void execute_remote_action(pik_control_action_t action) {
    if (action == PIK_CONTROL_ACTION_REBOOT_PEER ||
        action == PIK_CONTROL_ACTION_POWEROFF_PEER)
        mark_peer_initiated();
    session_teardown();
    local_control_cleanup();

    switch (action) {
    case PIK_CONTROL_ACTION_RESTART_PEER:
        LOG("executing restart-peer");
        execv(g_argv[0], g_argv);
        LOG("execv %s: %s", g_argv[0], strerror(errno));
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
        "  %s --control status-peer|restart-peer|reboot-peer|poweroff-peer\n"
        "  %s --usb VID:PID  mcu:N:DEV:BAUD [...] [listen:BIND_ADDR:PORT]\n"
        "  %s --usb VID:PID  pty:N:SYMLINK  [...] [forward:TARGET_HOST:PORT]\n",
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
    pik_log_set_timestamps(is_mcu);

    serialmux_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    bool seen_ch[UINT8_MAX + 1] = { false };
    while (argi < argc) {
        char *spec = argv[argi];
        if (strncmp(spec, "listen:", 7) == 0 ||
            strncmp(spec, "forward:", 8) == 0)
            break;
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
    char *tcp_mode = NULL;
    pik_control_tcp_role_t tcp_role = PIK_CONTROL_TCP_NONE;

    if (argi < argc &&
        (strncmp(argv[argi], "listen:", 7) == 0 ||
         strncmp(argv[argi], "forward:", 8) == 0)) {
        const char *tcp_spec = argv[argi];
        const char *hostport = NULL;
        if (strncmp(tcp_spec, "listen:", 7) == 0) {
            tcp_mode = "listen";
            tcp_role = PIK_CONTROL_TCP_LISTEN;
            hostport = tcp_spec + 7;
        } else {
            tcp_mode = "forward";
            tcp_role = PIK_CONTROL_TCP_FORWARD;
            hostport = tcp_spec + 8;
        }

        char port_str[16], extra;
        if (sscanf(hostport, "%63[^:]:%15[^:]%c",
                   tcp_addr, port_str, &extra) != 2 ||
            !pik_parse_port(port_str, &tcp_port))
            DIE("bad tcp spec: %s", argv[argi]);
        argi++;

        snprintf(addr_port, sizeof(addr_port), "%s:%d", tcp_addr, tcp_port);
        int i = 0;
        snprintf(tb_path, sizeof(tb_path), "%s/tcpbridge", self_dir);
        g_child.argv[i++] = (access(tb_path, X_OK) == 0) ? tb_path : "tcpbridge";
        g_child.argv[i++] = g_child.tunnel_dev;
        g_child.argv[i++] = tcp_mode;
        g_child.argv[i++] = addr_port;
        g_child.argv[i] = NULL;
        g_child.enabled = true;
        has_tcp = true;
    }

    if (argi != argc) DIE("unexpected argument: %s", argv[argi]);

    g_uart_name = is_mcu ? "mcu" : "pty";

    if (has_tcp)
        LOG("uart=%s release=%s protocol=%u channels=%d tcp=%s:%s:%d control=usb[0] serial=usb[1] tunnel=usb[2]",
            g_uart_name, PIK1_RELEASE_VERSION, PIK1_PROTOCOL_VERSION,
            cfg.n_channels, tcp_mode, tcp_addr, tcp_port);
    else
        LOG("uart=%s release=%s protocol=%u channels=%d control=usb[0] serial=usb[1]",
            g_uart_name, PIK1_RELEASE_VERSION, PIK1_PROTOCOL_VERSION,
            cfg.n_channels);

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
    g_child.epfd = epfd;

    static int sig_tag;
    pik_epoll_set(epfd, sig_fd, EPOLLIN, &sig_tag);
    pik_control_init(epfd, is_mcu ? PIK_CONTROL_ROLE_MCU : PIK_CONTROL_ROLE_PTY,
                     on_control_command, NULL);
    {
        uint8_t channel_ids[MAX_CHANNELS];
        for (int i = 0; i < cfg.n_channels; i++)
            channel_ids[i] = cfg.channels[i].ch_id;
        pik_control_set_config(channel_ids, (size_t)cfg.n_channels, tcp_role);
    }
    if (!local_control_start(epfd))
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
                    g_link_flags = 0;
                    session_active = true;
                    session_confirmed = false;
                    usb_retry_at = 0;
                    if (pik_control_handshake_failures() == 0)
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
                set_link_flag(PIK_CONTROL_LINK_SERIAL, true);
                LOG("data session started: serial=%s%s%s",
                    serial_dev, has_tcp ? " tunnel=" : "",
                    has_tcp ? tunnel_dev : "");
                if (has_tcp)
                    child_spawn(tunnel_dev, now);
            } else {
                LOG("serial mux failed to start");
                set_link_flag(PIK_CONTROL_LINK_SERIAL, false);
                session_active = false;
                session_confirmed = false;
                session_teardown();
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
                        if (g_local.command_pending) {
                            /* single outstanding outbound command; see control.h */
                            LOG("ignoring SIGUSR1 while a local command is pending");
                            g_signal_command_done = true;
                        } else if (!is_mcu && !g_signal_command_pending && !g_signal_command_done &&
                            pik_control_send_command(PIK_CONTROL_ACTION_RESTART_PEER,
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
                    /* quiet after the first failure of a dead-peer handshake
                     * loop; control logs a periodic summary instead */
                    if (pik_control_handshake_failures() <= 1) {
                        LOG("control link failed, restarting session");
                        last_usb_state = USB_WAIT_UNKNOWN;
                    }
                    set_link_flags(0);
                    session_active = false;
                    session_confirmed = false;
                    session_teardown();
                    usb_retry_at = now + pik_backoff_next(&usb_backoff_ms, RETRY_MAX_MS);
                }
                continue;
            }

            if (ptr == &g_child_status_tag) {
                child_status_read();
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
                LOG("serial mux failed, restarting session");
                set_link_flags(0);
                session_active = false;
                session_confirmed = false;
                session_teardown();
                usb_retry_at = now + pik_backoff_next(&usb_backoff_ms, RETRY_MAX_MS);
                last_usb_state = USB_WAIT_UNKNOWN;
            }
        }

        if (g_signal_command_done) shutdown = true;
        if (shutdown) break;

        now = pik_now_ms();
        local_control_check_ack(now);

        if (session_active && !pik_control_tick(now)) {
            if (pik_control_handshake_failures() <= 1) {
                LOG("control link failed, restarting session");
                last_usb_state = USB_WAIT_UNKNOWN;
            }
            set_link_flags(0);
            session_active = false;
            session_confirmed = false;
            session_teardown();
            usb_retry_at = now + pik_backoff_next(&usb_backoff_ms, RETRY_MAX_MS);
        }

        if (session_confirmed && !serialmux_tick(now)) {
            LOG("serial mux failed, restarting session");
            set_link_flags(0);
            session_active = false;
            session_confirmed = false;
            session_teardown();
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

    session_teardown();
    local_control_cleanup();
    close(sig_fd);
    close(epfd);
    return 0;
}
