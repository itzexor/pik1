#!/bin/bash
set -e

GADGET_NAME="pik1"
VENDOR_ID="0x1d6b"
PRODUCT_ID="0x0104"
MANUFACTURER="PiK1"
PRODUCT="PiK1 Bridge"
SERIALNUMBER="123456"
LANG="0x409"

GADGET_DIR="/sys/kernel/config/usb_gadget/$GADGET_NAME"

log() { echo "setup_pik1: $*" >&2; }

if [ -d "$GADGET_DIR" ]; then
    UDC="$(cat "$GADGET_DIR/UDC" 2>/dev/null || true)"
    if [ -n "$UDC" ]; then
        log "gadget already bound to $UDC -- skipping"
        exit 0
    fi
fi

modprobe libcomposite

if ! mountpoint -q /sys/kernel/config; then
    log "mounting configfs"
    mount -t configfs none /sys/kernel/config
fi

mkdir -p "$GADGET_DIR"

echo "$VENDOR_ID"  > "$GADGET_DIR/idVendor"
echo "$PRODUCT_ID" > "$GADGET_DIR/idProduct"
echo "0x0200"      > "$GADGET_DIR/bcdUSB"
echo "0x0100"      > "$GADGET_DIR/bcdDevice"

mkdir -p "$GADGET_DIR/strings/$LANG"
echo "$MANUFACTURER" > "$GADGET_DIR/strings/$LANG/manufacturer"
echo "$PRODUCT"      > "$GADGET_DIR/strings/$LANG/product"
echo "$SERIALNUMBER" > "$GADGET_DIR/strings/$LANG/serialnumber"

mkdir -p "$GADGET_DIR/configs/c.1"
echo 250 > "$GADGET_DIR/configs/c.1/MaxPower"

mkdir -p "$GADGET_DIR/configs/c.1/strings/$LANG"
echo "CDC ACM bridge (2-port)" > "$GADGET_DIR/configs/c.1/strings/$LANG/configuration"

mkdir -p "$GADGET_DIR/functions/acm.0"
ln -sf "$GADGET_DIR/functions/acm.0" "$GADGET_DIR/configs/c.1/acm.0"

mkdir -p "$GADGET_DIR/functions/acm.1"
ln -sf "$GADGET_DIR/functions/acm.1" "$GADGET_DIR/configs/c.1/acm.1"

UDC="$(ls /sys/class/udc/ 2>/dev/null | head -1)"
if [ -z "$UDC" ]; then
    log "ERROR: no UDC found in /sys/class/udc/ -- is dwc2 loaded?"
    exit 1
fi

echo "$UDC" > "$GADGET_DIR/UDC"
