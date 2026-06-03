#pragma once

#include "frame.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

#if defined(__GNUC__)
#define TEST_UNUSED __attribute__((unused))
#else
#define TEST_UNUSED
#endif

static TEST_UNUSED int test_set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static TEST_UNUSED bool test_wait_fd(int fd, bool writeable, int timeout_ms) {
    fd_set rfds, wfds;
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    if (writeable) FD_SET(fd, &wfds);
    else FD_SET(fd, &rfds);
    struct timeval tv = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    int r;
    do {
        r = select(fd + 1, writeable ? NULL : &rfds, writeable ? &wfds : NULL,
                   NULL, &tv);
    } while (r < 0 && errno == EINTR);
    return r > 0;
}

static TEST_UNUSED bool test_write_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = buf;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, p + off, len - off);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!test_wait_fd(fd, true, 1000)) return false;
            continue;
        }
        if (n <= 0) return false;
        off += (size_t)n;
    }
    return true;
}

static TEST_UNUSED bool test_read_delimited_frame(int fd, uint8_t *buf, size_t cap,
                                                  size_t *len, int timeout_ms) {
    *len = 0;
    while (*len < cap) {
        if (!test_wait_fd(fd, false, timeout_ms)) return false;
        ssize_t n = read(fd, buf + *len, 1);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (n <= 0) return false;
        *len += (size_t)n;
        if (buf[*len - 1] == 0) return true;
    }
    return false;
}

static TEST_UNUSED bool test_epoll_dispatch_one(int epfd,
                                                bool (*dispatch)(void *, uint32_t, int64_t),
                                                int timeout_ms) {
    struct epoll_event ev;
    int n;
    do {
        n = epoll_wait(epfd, &ev, 1, timeout_ms);
    } while (n < 0 && errno == EINTR);
    if (n <= 0) return false;
    return dispatch(ev.data.ptr, ev.events, pik_now_ms());
}

static TEST_UNUSED bool test_encode_frame(const uint8_t *header, size_t header_len,
                                          const uint8_t *payload, size_t payload_len,
                                          uint8_t *enc, size_t enc_cap, size_t *enc_len) {
    uint8_t dec[8192];
    return pik_frame_encode(header, header_len, payload, payload_len,
                            dec, sizeof(dec), enc, enc_cap, enc_len) == PIK_FRAME_OK;
}
