#pragma once

#include "link.h"

#include <stdbool.h>
#include <stdint.h>

enum {
    PIK_USB_FS_MAX_PACKET = 64u,
    PIK_USB_HS_MAX_PACKET = 512u,
    PIK_USB_TX_CHUNK = PIK_USB_HS_MAX_PACKET,
};

#define PIK_CONFIGFS_GADGET "/sys/kernel/config/usb_gadget/pik1"

bool pik_usb_host_start(pik_link_t *lk, int epfd, int64_t now);
bool pik_usb_host_owns_event(const void *ptr);
bool pik_usb_host_dispatch(void *ptr, uint32_t events, int64_t now);
bool pik_usb_host_tick(int64_t now);
int64_t pik_usb_host_deadline(void);
void pik_usb_host_cleanup(void);

bool pik_usb_gadget_prepare(void);
bool pik_usb_gadget_start(pik_link_t *lk, int epfd, int64_t now);
bool pik_usb_gadget_owns_event(const void *ptr);
bool pik_usb_gadget_dispatch(void *ptr, uint32_t events, int64_t now);
bool pik_usb_gadget_tick(int64_t now);
int64_t pik_usb_gadget_deadline(void);
void pik_usb_gadget_cleanup(void);
