#pragma once

enum {
    PIK_USB_FS_MAX_PACKET = 64u,
    PIK_USB_HS_MAX_PACKET = 512u,
    /* Keep transport writes to one high-speed bulk packet. */
    PIK_USB_TX_CHUNK = PIK_USB_HS_MAX_PACKET,
};

#define PIK_CONFIGFS_GADGET "/sys/kernel/config/usb_gadget/pik1"
