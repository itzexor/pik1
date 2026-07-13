#!/bin/bash
set -euo pipefail

readonly GADGET_NAME="pik1"
readonly VENDOR_ID="0x1d6b"
readonly PRODUCT_ID="0x0104"
readonly MANUFACTURER="PiK1"
readonly PRODUCT="PiK1 Bridge"
readonly SERIALNUMBER="123456"
readonly USB_LANG="0x409"
readonly ACM_COUNT=3

readonly CONFIGFS_DIR="/sys/kernel/config"
readonly GADGET_DIR="$CONFIGFS_DIR/usb_gadget/$GADGET_NAME"
readonly CONFIG_DIR="$GADGET_DIR/configs/c.1"

log() { echo "setup_otg: $*" >&2; }

write_file() {
    local path="$1"
    local value="$2"

    printf '%s\n' "$value" > "$path"
}

gadget_has_acm_functions() {
    local idx

    for ((idx = 0; idx < ACM_COUNT; idx++)); do
        [ -d "$GADGET_DIR/functions/acm.$idx" ] || return 1
        [ -e "$CONFIG_DIR/acm.$idx" ] || return 1
    done
}

first_udc() {
    local udc

    for udc in /sys/class/udc/*; do
        [ -e "$udc" ] || return 1
        basename "$udc"
        return 0
    done

    return 1
}

ensure_configfs() {
    if ! mountpoint -q "$CONFIGFS_DIR"; then
        log "mounting configfs"
        mount -t configfs none "$CONFIGFS_DIR"
    fi
}

ensure_gadget_unbound_or_ready() {
    local udc=""

    [ -d "$GADGET_DIR" ] || return 0
    udc="$(cat "$GADGET_DIR/UDC" 2>/dev/null || true)"
    [ -n "$udc" ] || return 0

    if gadget_has_acm_functions; then
        log "gadget already bound to $udc -- skipping"
        exit 0
    fi

    log "ERROR: gadget already bound to $udc but is missing one or more ACM functions"
    log "ERROR: expected acm.0 through acm.$((ACM_COUNT - 1))"
    log "ERROR: unbind or remove the stale gadget before starting pik1"
    exit 1
}

configure_acm_functions() {
    local idx

    for ((idx = 0; idx < ACM_COUNT; idx++)); do
        mkdir -p "$GADGET_DIR/functions/acm.$idx"
        ln -sf "$GADGET_DIR/functions/acm.$idx" "$CONFIG_DIR/acm.$idx"
    done
}

log "loading libcomposite"
modprobe libcomposite

ensure_configfs
ensure_gadget_unbound_or_ready

log "creating gadget at $GADGET_DIR"
mkdir -p "$GADGET_DIR"

write_file "$GADGET_DIR/idVendor" "$VENDOR_ID"
write_file "$GADGET_DIR/idProduct" "$PRODUCT_ID"
write_file "$GADGET_DIR/bcdUSB" "0x0200"
write_file "$GADGET_DIR/bcdDevice" "0x0100"

mkdir -p "$GADGET_DIR/strings/$USB_LANG"
write_file "$GADGET_DIR/strings/$USB_LANG/manufacturer" "$MANUFACTURER"
write_file "$GADGET_DIR/strings/$USB_LANG/product" "$PRODUCT"
write_file "$GADGET_DIR/strings/$USB_LANG/serialnumber" "$SERIALNUMBER"

mkdir -p "$CONFIG_DIR"
write_file "$CONFIG_DIR/MaxPower" "250"

mkdir -p "$CONFIG_DIR/strings/$USB_LANG"
write_file "$CONFIG_DIR/strings/$USB_LANG/configuration" "CDC ACM bridge (${ACM_COUNT}-port)"

configure_acm_functions

UDC="$(first_udc || true)"
if [ -z "$UDC" ]; then
    log "ERROR: no UDC found in /sys/class/udc/ -- is dwc2 loaded?"
    exit 1
fi

log "binding gadget to UDC: $UDC"
write_file "$GADGET_DIR/UDC" "$UDC"

log "gadget setup complete"
