#include "fd.h"

#include <fcntl.h>
#include <stddef.h>
#include <sys/epoll.h>

void pik_epoll_set(int epfd, int fd, uint32_t events, void *ptr) {
    struct epoll_event ev = { .events = events, .data.ptr = ptr };
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev) == -1)
        epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
}

void pik_epoll_del(int epfd, int fd) {
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
}

void pik_fd_set_nonblock(int fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}
