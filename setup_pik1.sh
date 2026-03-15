#!/bin/bash
# setup_pik1.sh -- configure a single USB CDC ACM gadget via configfs.
# Creates /dev/ttyGS0 on the host (Pi) side.
# Idempotent: if the gadget is already bound to a UDC, exits immediately.

set -e

GADGET_DIR="/sys/kernel/config/usb_gadget/pik1"

VENDOR_ID="0x1d6b"
PRODUCT_ID="0x0104"
MANUFACTURER="Creality"
PRODUCT="K1 Bridge"
SERIALNUMBER="pik1"
LANG="0x409"

log() { echo "setup_pik1: $*" >&2; }

# If the gadget already exists and is bound to a UDC, nothing to do.
if [ -d "$GADGET_DIR" ]; then
    UDC="$(cat "$GADGET_DIR/UDC" 2>/dev/null || true)"
    if [ -n "$UDC" ]; then
        log "gadget already bound to $UDC -- skipping"
        exit 0
    fi
fi

log "loading libcomposite"
modprobe libcomposite

if ! mountpoint -q /sys/kernel/config; then
    log "mounting configfs"
    mount -t configfs none /sys/kernel/config
fi

log "creating gadget at $GADGET_DIR"
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
echo "CDC ACM bridge" > "$GADGET_DIR/configs/c.1/strings/$LANG/configuration"

mkdir -p "$GADGET_DIR/functions/acm.0"
ln -s "$GADGET_DIR/functions/acm.0" "$GADGET_DIR/configs/c.1/acm.0"

UDC="$(ls /sys/class/udc/ 2>/dev/null | head -1)"
if [ -z "$UDC" ]; then
    log "ERROR: no UDC found in /sys/class/udc/ -- is dwc2 loaded?"
    exit 1
fi

log "binding gadget to UDC: $UDC"
echo "$UDC" > "$GADGET_DIR/UDC"

sleep 0.2
if [ -c /dev/ttyGS0 ]; then
    log "ttyGS0 ready"
else
    log "WARNING: /dev/ttyGS0 not present after bind -- check dmesg"
fi
