#include "fd.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <sys/epoll.h>

bool pik_epoll_set(int epfd, int fd, uint32_t events, void *ptr) {
    struct epoll_event ev = { .events = events, .data.ptr = ptr };
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev) == 0)
        return true;
    if (errno != ENOENT)
        return false;
    return epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) == 0;
}

void pik_epoll_del(int epfd, int fd) {
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
}

bool pik_fd_set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}
