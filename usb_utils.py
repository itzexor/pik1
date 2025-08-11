import subprocess
import logging
import time
from pathlib import Path
import os, stat, termios

# Constants

USB_VID = "0x1d6b"  # Linux Foundation
USB_PID = "0x0104"  # Multifunction Composite Gadget

GADGET = "g1"
CFG_LABEL = "Config 1: Multi-Serial"

USBRESET_BIN = "/opt/bin/usbreset"
DETECT_TIMEOUT = 30  # seconds
DEVICE_DELAY = 1

class DeviceException(Exception):
    pass


def wait_for_usb(shutdown_event):
    """
    Block until USB gadget with specified VID:PID appears via lsusb.
    """
    elapsed = 0
    while not shutdown_event.is_set():
        rc = subprocess.run(["lsusb", "-d", f"{USB_VID}:{USB_PID}"], stdout=subprocess.DEVNULL).returncode
        if rc == 0:
            logging.info(f"USB gadget {USB_VID}:{USB_PID} detected")
            return
        if elapsed > DETECT_TIMEOUT:
            raise DeviceException(f"Timeout waiting for USB gadget {USB_VID}:{USB_PID}")
        time.sleep(1)
        elapsed += 1


def usbreset_device():
    """
    Reset the USB gadget using the usbreset binary.
    """
    subprocess.run([USBRESET_BIN, f"{USB_VID}:{USB_PID}"], check=True)


def detect_gadget_ttys(host_mode=False):
    """
    Discover and return up to three character-device paths under /dev/tty*
    whose parent USB device has the given VID:PID. If any non-character
    device is found and removed, returns None.
    """
    from pathlib import Path

    matches = []
    error = False

    if host_mode:
        matches = [str(p) for p in Path('/dev').glob('ttyGS*')]
    else:
        matches = [str(p) for p in Path('/dev').glob('ttyACM*')]
        for tty in matches:
            mode = os.stat(tty).st_mode
            if not stat.S_ISCHR(mode):
                try:
                    os.remove(tty)
                    logging.warning(f"Removed non-character device {tty}")
                except Exception as e:
                    logging.warning(f"Failed to remove {tty}: {e}")
                error = True

    if error:
        return None

    if len(matches) < 3:
        msg = f"Found only {len(matches)} gadget ports, expected 3"
        logging.error(msg)
        raise DeviceException(msg)

    return sorted(matches)[:3]


def open_serial(dev, baud, nonblocking=False):
    """
    Open and configure a serial file descriptor with given baud.
    """
    flags = os.O_RDWR | os.O_NOCTTY
    if nonblocking:
        flags |= os.O_NONBLOCK
    fd = os.open(dev, flags)
    attrs = termios.tcgetattr(fd)
    # raw input, raw output
    attrs[0] = 0
    attrs[1] = 0
    # control flags: 8N1, enable receiver, local
    attrs[2] = termios.CREAD | termios.CLOCAL | termios.CS8
    # local flags raw
    attrs[3] = 0
    # set baud
    b = getattr(termios, f"B{baud}", None)
    if b is None:
        raise ValueError(f"Unsupported baud rate: {baud}")
    attrs[4] = b
    attrs[5] = b
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    termios.tcflush(fd, termios.TCIOFLUSH)
    return fd

def setup_usb_gadget():
    """
    Configure the USB composite gadget via ConfigFS (modprobe + directory writes).
    """

    # 1) Ensure libcomposite is loaded (raises if it fails)
    subprocess.run(["modprobe", "libcomposite"], check=True)

    cfgfs = Path("/sys/kernel/config/usb_gadget")
    gadget_dir = cfgfs / GADGET

    # 2) If gadget already exists, do nothing (match your shell behavior)
    if gadget_dir.exists():
        return

    # 3) Create gadget root
    gadget_dir.mkdir(parents=True, exist_ok=True)

    # 4) Device IDs and versions
    (gadget_dir / "idVendor").write_text(USB_VID)
    (gadget_dir / "idProduct").write_text(USB_PID)
    (gadget_dir / "bcdDevice").write_text("0x0100")  # v1.0.0
    (gadget_dir / "bcdUSB").write_text("0x0200")     # USB2

    # 5) Strings (serial/manufacturer/product)
    s_en = gadget_dir / "strings" / "0x409"
    s_en.mkdir(parents=True, exist_ok=True)

    serial = "0000000000000000"
    try:
        # Strip possible NULs from DT strings
        raw = Path("/sys/firmware/devicetree/base/serial-number").read_text()
        serial = raw.strip("\x00\r\n") or serial
    except FileNotFoundError:
        pass

    (s_en / "serialnumber").write_text(serial)
    (s_en / "manufacturer").write_text("Raspberry Pi")
    (s_en / "product").write_text("Multi-Serial Gadget")

    # 6) Config and label
    cfg_dir = gadget_dir / "configs" / "c.1"
    cfg_dir.mkdir(parents=True, exist_ok=True)
    cfg_str = cfg_dir / "strings" / "0x409"
    cfg_str.mkdir(parents=True, exist_ok=True)
    (cfg_str / "configuration").write_text(CFG_LABEL)

    # Optional TODO (same as your shell): set MaxPower if desired
    # (cfg_dir / "MaxPower").write_text("250")  # units: 2mA (250 => 500mA)

    # 7) Functions: three CDC ACM ports
    func_dir = gadget_dir / "functions"
    for i in range(3):
        (func_dir / f"acm.usb{i}").mkdir(parents=True, exist_ok=True)

    # 8) Link functions into the config (use absolute targets to be foolproof)
    for i in range(3):
        link_path = cfg_dir / f"acm.usb{i}"
        target = func_dir / f"acm.usb{i}"
        try:
            # If a stale/bad link exists, remove it
            if link_path.exists() or link_path.is_symlink():
                link_path.unlink()
            link_path.symlink_to(target)
        except FileExistsError:
            # Harmless if already correctly linked
            pass

    # 9) Bind to UDC
    udc_root = Path("/sys/class/udc")
    udcs = [p.name for p in udc_root.iterdir()]
    if not udcs:
        raise RuntimeError("No UDC found (is dwc2 loaded and in device mode?)")
    (gadget_dir / "UDC").write_text(udcs[0])