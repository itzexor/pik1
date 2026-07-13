#pragma once

#include <stdint.h>

void pik_epoll_set(int epfd, int fd, uint32_t events, void *ptr);
void pik_epoll_del(int epfd, int fd);
void pik_fd_set_nonblock(int fd);
