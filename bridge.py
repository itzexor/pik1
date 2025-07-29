import selectors
import os
import logging
from usb_utils import open_serial

def run_mcu_bridges(mcu_tty, nozzle_tty, gadget_tty0, gadget_tty1, baud, shutdown_event):
    logging.info(f"Starting bridges: '{mcu_tty}'<->'{gadget_tty0}', '{nozzle_tty}'<->'{gadget_tty1}'")
    fd_mcu = open_serial(mcu_tty, baud, nonblocking=True)
    fd_nozzle = open_serial(nozzle_tty, baud, nonblocking=True)
    fd_g0 = open_serial(gadget_tty0, baud, nonblocking=True)
    fd_g1 = open_serial(gadget_tty1, baud, nonblocking=True)

    sel = selectors.DefaultSelector()
    sel.register(fd_mcu, selectors.EVENT_READ, fd_g0)
    sel.register(fd_g0, selectors.EVENT_READ, fd_mcu)
    sel.register(fd_nozzle, selectors.EVENT_READ, fd_g1)
    sel.register(fd_g1, selectors.EVENT_READ, fd_nozzle)

    try:
        while not shutdown_event.is_set():
            for key, _ in sel.select(timeout=1.0):
                src, dst = key.fd, key.data
                try:
                    data = os.read(src, 4096)
                    if data:
                        os.write(dst, data)
                except OSError as e:
                    logging.error(f"Bridge error on fd {src}: {e}")
                    if shutdown_event.is_set():
                        break
    finally:
        sel.close()
        for fd in (fd_mcu, fd_nozzle, fd_g0, fd_g1):
            try: os.close(fd)
            except: pass
