#include "local_control.h"

#include "fd.h"
#include "logging.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define LOCAL_CONTROL_WRITE_TIMEOUT_MS 1000
#define LOG(...) pik_log("pik1", __VA_ARGS__)

typedef struct {
    int listen_fd;
    int client_fd;
    int epfd;
    bool command_pending;
    uint32_t request_id;
    int64_t deadline_ms;
    char input[64];
    size_t input_len;
} local_control_t;

static local_control_t g_local = {
    .listen_fd = -1,
    .client_fd = -1,
    .epfd = -1,
};
static int g_local_listen_tag;

const char *pik_local_control_sock_path(void) {
    const char *p = getenv(PIK_CONTROL_SOCK_ENV);
    return (p && *p) ? p : PIK_LOCAL_CONTROL_SOCK;
}

static int connect_local_control(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", pik_local_control_sock_path());
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int pik_local_control_client_main(const char *cmd) {
    int fd = connect_local_control();
    if (fd < 0) {
        fprintf(stderr, "pik1d: connect %s: %s\n",
                pik_local_control_sock_path(), strerror(errno));
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

bool pik_parse_control_action(const char *cmd, pik_control_action_t *action) {
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
    g_local.input_len = 0;
}

bool pik_local_control_start(int epfd) {
    const char *sock_path = pik_local_control_sock_path();
    g_local.epfd = epfd;
    if (!getenv(PIK_CONTROL_SOCK_ENV) &&
        mkdir(PIK_LOCAL_CONTROL_DIR, 0700) < 0 && errno != EEXIST) {
        LOG("mkdir %s: %s", PIK_LOCAL_CONTROL_DIR, strerror(errno));
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
    if (!pik_epoll_set(epfd, g_local.listen_fd, EPOLLIN,
                       &g_local_listen_tag)) {
        LOG("epoll add local control listener: %s", strerror(errno));
        close(g_local.listen_fd);
        g_local.listen_fd = -1;
        unlink(sock_path);
        return false;
    }
    return true;
}

void pik_local_control_cleanup(void) {
    local_control_close_client();
    if (g_local.listen_fd >= 0) {
        pik_epoll_del(g_local.epfd, g_local.listen_fd);
        close(g_local.listen_fd);
        g_local.listen_fd = -1;
        unlink(pik_local_control_sock_path());
    }
}

bool pik_local_control_owns_listen(const void *ptr) {
    return ptr == &g_local_listen_tag;
}

bool pik_local_control_owns_client(const void *ptr) {
    return ptr == &g_local;
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

void pik_local_control_accept(void) {
    int fd = accept4(g_local.listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) return;
    if (g_local.client_fd >= 0) {
        if (!local_control_write(fd, "ERR busy\n", strlen("ERR busy\n")))
            LOG("local control busy response was not fully delivered");
        close(fd);
        return;
    }
    g_local.client_fd = fd;
    g_local.input_len = 0;
    if (!pik_epoll_set(g_local.epfd, fd, EPOLLIN, &g_local)) {
        LOG("epoll add local control client: %s", strerror(errno));
        close(fd);
        g_local.client_fd = -1;
    }
}

static void local_control_process_command(int64_t now, bool command_busy) {
    pik_control_action_t action;
    if (!pik_parse_control_action(g_local.input, &action)) {
        local_control_reply_and_close("ERR unknown command\n");
        return;
    }
    if (g_local.command_pending || command_busy) {
        local_control_reply_and_close("ERR busy\n");
        return;
    }
    if (!pik_control_send_command(action, &g_local.request_id)) {
        local_control_reply_and_close("ERR peer not ready\n");
        return;
    }
    g_local.command_pending = true;
    g_local.deadline_ms = now + PIK_COMMAND_ACK_TIMEOUT_MS;
}

void pik_local_control_read(int64_t now, bool command_busy) {
    while (true) {
        if (g_local.input_len == sizeof(g_local.input) - 1u) {
            local_control_reply_and_close("ERR command too long\n");
            return;
        }

        ssize_t n = read(g_local.client_fd,
                         g_local.input + g_local.input_len,
                         sizeof(g_local.input) - 1u - g_local.input_len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        if (n <= 0) {
            local_control_close_client();
            return;
        }
        g_local.input_len += (size_t)n;

        char *newline = memchr(g_local.input, '\n', g_local.input_len);
        if (!newline)
            continue;
        if ((size_t)(newline - g_local.input) + 1u != g_local.input_len) {
            local_control_reply_and_close("ERR one command per connection\n");
            return;
        }

        size_t len = (size_t)(newline - g_local.input);
        if (len && g_local.input[len - 1u] == '\r')
            len--;
        g_local.input[len] = '\0';
        local_control_process_command(now, command_busy);
        return;
    }
}

bool pik_local_control_pending(void) {
    return g_local.command_pending;
}

uint32_t pik_local_control_request_id(void) {
    return g_local.request_id;
}

void pik_local_control_complete(pik_control_ack_status_t status,
                                const uint8_t *payload, size_t payload_len) {
    if (status == PIK_CONTROL_ACK_OK && payload_len) {
        local_control_reply_payload_and_close(payload, payload_len);
    } else if (status == PIK_CONTROL_ACK_OK) {
        local_control_reply_and_close("OK\n");
    } else {
        local_control_reply_error_status(status, payload, payload_len);
    }
}

void pik_local_control_check_timeout(int64_t now) {
    if (g_local.command_pending && now >= g_local.deadline_ms)
        local_control_reply_and_close("ERR peer ack timeout\n");
}

int64_t pik_local_control_deadline(void) {
    return g_local.command_pending ? g_local.deadline_ms : INT64_MAX;
}

void pik_mark_peer_initiated(void) {
    int fd = open(PIK_PEER_INITIATED_MARKER, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        LOG("marker %s: %s", PIK_PEER_INITIATED_MARKER, strerror(errno));
        return;
    }
    close(fd);
}
