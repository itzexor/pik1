#pragma once

#include <stdbool.h>
#include <stdint.h>

bool pik_epoll_set(int epfd, int fd, uint32_t events, void *ptr);
void pik_epoll_del(int epfd, int fd);
bool pik_fd_set_nonblock(int fd);
