#!/bin/bash
set -euo pipefail

modprobe libcomposite

GADGETDIR="g1"

cd /sys/kernel/config/usb_gadget/
if [ -d /sys/kernel/config/usb_gadget/$GADGETDIR ]; then
    exit 0
fi

mkdir -p $GADGETDIR
cd $GADGETDIR

echo 0x1d6b > idVendor  # Linux Foundation
echo 0x0104 > idProduct # Multifunction Composite Gadget
echo 0x0100 > bcdDevice # v1.0.0
echo 0x0200 > bcdUSB    # USB2

mkdir -p strings/0x409
cat /sys/firmware/devicetree/base/serial-number > strings/0x409/serialnumber
echo "Raspberry Pi" > strings/0x409/manufacturer
echo "Multi-Serial Gadget" > strings/0x409/product

# TODO: set max power

mkdir -p configs/c.1/strings/0x409
echo "Config 1: Multi-Serial" > configs/c.1/strings/0x409/configuration
mkdir -p functions/acm.usb0     #ACM0 on the K1 is visible as GS0 on the Pi
mkdir -p functions/acm.usb1     #ACM1 on the K1 is visible as GS1 on the Pi
mkdir -p functions/acm.usb2     #ACM2 on the K1 is visible as GS2 on the Pi
ln -s functions/acm.usb0 configs/c.1/
ln -s functions/acm.usb1 configs/c.1/
ln -s functions/acm.usb2 configs/c.1/

# Attach the gadget to the USB Device Controller with the correct UDC
UDC=$(ls /sys/class/udc | head -n1)
echo "$UDC" > UDC