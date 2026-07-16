#pragma once

#include "link.h"

#include <stdbool.h>
#include <stdint.h>

bool pik_ffs_start(pik_link_t *lk, int epfd, int64_t now);
bool pik_ffs_owns_event(const void *ptr);
bool pik_ffs_dispatch(void *ptr, uint32_t events, int64_t now);
bool pik_ffs_tick(int64_t now);
int64_t pik_ffs_deadline(void);
void pik_ffs_cleanup(void);
