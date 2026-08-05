#pragma once

/* Product identity shared by the host lookup path, FunctionFS gadget
 * descriptors, install templates, and user-facing logs/docs. */

#define PIK1_RELEASE_VERSION "0.13.0"
#define PIK1_PROTOCOL_VERSION 13u

#define PIK1_USB_VID "1d6b"
#define PIK1_USB_PID "51c1"
#define PIK1_USB_VIDPID PIK1_USB_VID ":" PIK1_USB_PID
#define PIK1_USB_ID_VENDOR "0x" PIK1_USB_VID
#define PIK1_USB_ID_PRODUCT "0x" PIK1_USB_PID
#define PIK1_USB_BCD_USB "0x0200"
#define PIK1_USB_BCD_DEVICE "0x0100"
#define PIK1_USB_LANG "0x409"
#define PIK1_USB_LANG_ID 0x0409u

#define PIK1_USB_MANUFACTURER "PiK1"
#define PIK1_USB_PRODUCT "PiK1 Bridge"
#define PIK1_USB_SERIAL "123456"
#define PIK1_USB_CONFIGURATION "PiK1 bulk bridge"
#define PIK1_USB_INTERFACE "PiK1 Bulk"

#define PIK1_FFS_NAME "pik"
#define PIK1_FFS_MOUNT "/run/pik1-ffs"
