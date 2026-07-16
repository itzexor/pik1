#pragma once

#include "link.h"

#include <stdbool.h>
#include <stdint.h>

bool pik_usb_bulk_start(pik_link_t *lk, int epfd, int64_t now);
bool pik_usb_bulk_owns_event(const void *ptr);
bool pik_usb_bulk_dispatch(void *ptr, uint32_t events, int64_t now);
bool pik_usb_bulk_tick(int64_t now);
int64_t pik_usb_bulk_deadline(void);
void pik_usb_bulk_cleanup(void);
