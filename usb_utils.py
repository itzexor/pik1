import subprocess
import logging
import time
from pathlib import Path
import os, stat, termios

# Constants
USB_VID = "1d6b"
USB_PID = "0104"
USBRESET_BIN = "/opt/bin/usbreset"
DETECT_TIMEOUT = 30  # seconds

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
    subprocess.run(['modprobe', 'libcomposite'], check=True)
    gadget_dir = Path('/sys/kernel/config/usb_gadget/g1')
    if gadget_dir.exists():
        return

    gadget_dir.mkdir(parents=True)
    (gadget_dir / 'idVendor').write_text(USB_VID)
    (gadget_dir / 'idProduct').write_text(USB_PID)
    (gadget_dir / 'bcdDevice').write_text('0x0100')
    (gadget_dir / 'bcdUSB').write_text('0x0200')

    # Strings
    strings_dir = gadget_dir / 'strings' / '0x409'
    strings_dir.mkdir(parents=True)
    serial = Path('/sys/firmware/devicetree/base/serial-number').read_text().strip()
    (strings_dir / 'serialnumber').write_text(serial)
    (strings_dir / 'manufacturer').write_text('Raspberry Pi')
    (strings_dir / 'product').write_text('Multi-Serial Gadget')

    # Configuration
    cfg_str = gadget_dir / 'configs' / 'c.1' / 'strings' / '0x409'
    cfg_str.mkdir(parents=True)
    (cfg_str / 'configuration').write_text('Config 1: Multi-Serial')

    # Functions
    for i in range(3):
        func = gadget_dir / 'functions' / f'acm.usb{i}'
        func.mkdir(exist_ok=True)
        link = gadget_dir / 'configs' / 'c.1' / f'acm.usb{i}'
        try:
            link.symlink_to(func.relative_to(gadget_dir))
        except FileExistsError:
            pass

    # Bind UDC
    udc = Path('/sys/class/udc').read_text().splitlines()[0]
    (gadget_dir / 'UDC').write_text(udc)
