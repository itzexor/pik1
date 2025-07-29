#!/usr/bin/env python3
import argparse
import logging
import signal
import threading
import sys
import time

from usb_utils import setup_usb_gadget, wait_for_usb, usbreset_device, detect_gadget_ttys, DeviceException
from bridge import run_mcu_bridges
import tunnel

# Default constants
DEFAULT_IP = '127.0.0.1'
DEFAULT_PORT = 7125
DEFAULT_BRIDGE_BAUD = 230400
DEFAULT_TUNNEL_BAUD = 115200
MAX_PREPARE_RETRIES = 6
PREPARE_RETRY_DELAY = 10


def make_shutdown_event():
    return threading.Event()


def setup_logging(log_path):
    fmt = logging.Formatter('%(asctime)s pik1: %(message)s', '%Y-%m-%d %H:%M:%S')
    handler = logging.FileHandler(log_path)
    handler.setFormatter(fmt)
    root = logging.getLogger()
    root.setLevel(logging.INFO)
    root.handlers.clear()
    root.addHandler(handler)
    console = logging.StreamHandler(sys.stdout)
    console.setFormatter(fmt)
    root.addHandler(console)


def prepare_devices(init_gadget, shutdown_event):
    if init_gadget:
        setup_usb_gadget()
        return detect_gadget_ttys(init_gadget)
    else:
        retries = 0
        while retries < MAX_PREPARE_RETRIES:
            # the first detection cycle will cleanup stale files
            wait_for_usb(shutdown_event)
            detect_gadget_ttys(init_gadget)
            # setup after reset
            usbreset_device()
            wait_for_usb(shutdown_event)
            ttys = detect_gadget_ttys(init_gadget)
            if ttys:
                return ttys
            time.sleep(PREPARE_RETRY_DELAY)
            retries += 1
        raise DeviceException("Unable to prepare devices: max retries exceeded")


def handle_pi(args, shutdown_event):
    prepare_devices(args.init_gadget, shutdown_event)
    shutdown_event.set()
    logging.info("pi mode complete: gadget configured, exiting.")


def handle_k1(args, shutdown_event):
    g0, g1, _ = prepare_devices(args.init_gadget, shutdown_event)
    run_mcu_bridges(
        args.mcu_tty, args.nozzle_tty,
        g0, g1, args.baud,
        shutdown_event
    )

def handle_tunnel(args, shutdown_event):
    _, _, g2 = prepare_devices(args.init_gadget, shutdown_event)
    if args.tunnel_role == 'source':
        tunnel.run_tunnel_source(g2, args.baud, args.ip, args.port, shutdown_event)
    else:
        tunnel.run_tunnel_dest(g2, args.baud, args.ip, args.port, shutdown_event)


def main():
    parser = argparse.ArgumentParser(
        prog="pik1.py",
        description="PiK1: Pass MCUs & Moonraker TCP socket between Raspberry Pi and Creality K1-series using USB OTG serial gadgets"
    )
    subparsers = parser.add_subparsers(dest='mode', required=True)

    pi_p = subparsers.add_parser('pi', help='Bridge MCUs to Pi - Pi side (only necessary if not using any tunnel as this will only perform gadget setup and exit)')
    pi_p.set_defaults(func=handle_pi, init_gadget=True)

    k1_p = subparsers.add_parser('k1', help='Bridge MCUs to Pi - K1 side')
    k1_p.add_argument('--mcu-tty', required=True)
    k1_p.add_argument('--nozzle-tty', required=True)
    k1_p.add_argument('--baud', type=int, default=DEFAULT_BRIDGE_BAUD)
    k1_p.set_defaults(func=handle_k1, init_gadget=False)

    pi_tun = subparsers.add_parser('pi-tunnel', help='Tunnel Pi Moonraker to K1 screen - Pi side')
    pi_tun.add_argument('--baud', type=int, default=DEFAULT_TUNNEL_BAUD)
    pi_tun.add_argument('--ip', default=DEFAULT_IP)
    pi_tun.add_argument('--port', type=int, default=DEFAULT_PORT)
    pi_tun.set_defaults(func=handle_tunnel, init_gadget=True, tunnel_role='dest')

    k1_tun = subparsers.add_parser('k1-tunnel', help='Tunnel Pi Moonraker to K1 screen - K1 side')
    k1_tun.add_argument('--baud', type=int, default=DEFAULT_TUNNEL_BAUD)
    k1_tun.add_argument('--ip', default=DEFAULT_IP)
    k1_tun.add_argument('--port', type=int, default=DEFAULT_PORT)
    k1_tun.set_defaults(func=handle_tunnel, init_gadget=False, tunnel_role='source')

    pir_tun = subparsers.add_parser('pi-tunnel-reverse', help='Tunnel K1 Moonraker to Pi screen - Pi side')
    pir_tun.add_argument('--baud', type=int, default=DEFAULT_TUNNEL_BAUD)
    pir_tun.add_argument('--ip', default=DEFAULT_IP)
    pir_tun.add_argument('--port', type=int, default=DEFAULT_PORT)
    pir_tun.set_defaults(func=handle_tunnel, init_gadget=True, tunnel_role='source')

    k1r_tun = subparsers.add_parser('k1-tunnel-reverse', help='Tunnel K1 Moonraker to Pi screen - K1 side')
    k1r_tun.add_argument('--baud', type=int, default=DEFAULT_TUNNEL_BAUD)
    k1r_tun.add_argument('--ip', default=DEFAULT_IP)
    k1r_tun.add_argument('--port', type=int, default=DEFAULT_PORT)
    k1r_tun.set_defaults(func=handle_tunnel, init_gadget=False, tunnel_role='dest')

    parser.add_argument('--log', default='/tmp/pik1.log')

    args = parser.parse_args()
    setup_logging(args.log)

    shutdown_event = make_shutdown_event()
    signal.signal(signal.SIGINT, lambda s,f: shutdown_event.set())
    signal.signal(signal.SIGTERM, lambda s,f: shutdown_event.set())

    try:
        args.func(args, shutdown_event)
        shutdown_event.wait()
    except DeviceException as e:
        logging.error(e)
        sys.exit(1)
    except Exception as e:
        logging.error(f"Execution failed in mode {args.mode}: {e}")
        sys.exit(1)

if __name__ == '__main__':
    main()
