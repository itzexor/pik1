import selectors
import socket
import os
import logging
import termios
import time
from usb_utils import open_serial
from struct import pack, unpack

READ_SIZE = 4096
MAGIC = b"\x7E"

# Precomputed CRC16 CCITT-FALSE table
CRC16_TABLE = []
for byte in range(256):
    crc = byte << 8
    for _ in range(8):
        if crc & 0x8000:
            crc = ((crc << 1) ^ 0x1021)
        else:
            crc <<= 1
        crc &= 0xFFFF
    CRC16_TABLE.append(crc)

def crc16_ccitt_false(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc = ((crc << 8) ^ CRC16_TABLE[((crc >> 8) ^ byte) & 0xFF]) & 0xFFFF
    return crc

def send_frame(fd, sid, cmd, payload=b""):
    hdr = pack(">BBH", sid, cmd, len(payload))
    crc = crc16_ccitt_false(hdr + payload)
    frame = MAGIC + hdr + payload + pack(">H", crc)
    os.write(fd, frame)
    termios.tcdrain(fd)

def read_exact(fd, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = os.read(fd, n - len(buf))
        if not chunk:
            raise RuntimeError("Serial read error or disconnect")
        buf.extend(chunk)
    return bytes(buf)


def read_frame(fd):
    sync = read_exact(fd, 1)
    while sync != MAGIC:
        sync = read_exact(fd, 1)

    hdr = read_exact(fd, 4)
    sid, cmd, length = unpack(">BBH", hdr)
    payload = read_exact(fd, length) if length else b""
    crc_recv = unpack(">H", read_exact(fd, 2))[0]
    crc_calc = crc16_ccitt_false(hdr + payload)
    if crc_recv != crc_calc:
        raise ValueError(f"CRC mismatch: got 0x{crc_recv:04X}, expected 0x{crc_calc:04X}")
    return sid, cmd, payload


def _cleanup(sel, fd_ser, conns, server=None):
    try: sel.close()
    except: pass
    try: os.close(fd_ser)
    except: pass
    if server:
        try: server.close()
        except: pass
    for sock in conns.values():
        try: sock.close()
        except: pass
    conns.clear()


def run_tunnel_source(dev, baud, ip, port, shutdown_event):
    logging.info(f'Starting tunnel source loop: {ip}:{port} -> {dev}@{baud}')
    backoff = 1
    while not shutdown_event.is_set():
        fd_ser = None
        conns = {}
        try:
            server = socket.create_server((ip, port))
            server.setblocking(False)
            fd_ser = open_serial(dev, baud)
            sel = selectors.DefaultSelector()
            sel.register(server, selectors.EVENT_READ, 'accept')
            sel.register(fd_ser, selectors.EVENT_READ, 'serial')

            next_id = 1
            while not shutdown_event.is_set():
                for key, _ in sel.select(timeout=1.0):
                    tag = key.data
                    if tag == 'accept':
                        conn, _ = server.accept()
                        conn.setblocking(False)
                        sid = next_id
                        next_id = (sid % 255) + 1
                        conns[sid] = conn
                        sel.register(conn, selectors.EVENT_READ, sid)
                        send_frame(fd_ser, sid, 0)
                    elif tag == 'serial':
                        try:
                            sid, cmd, data = read_frame(fd_ser)
                        except Exception as e:
                            logging.error(f"Tunnel source fatal error: {e}")
                            raise
                        sock = conns.get(sid)
                        if cmd == 1 and sock:
                            sock.sendall(data)
                        elif cmd == 2 and sock:
                            sel.unregister(sock)
                            sock.close()
                            conns.pop(sid, None)
                    else:
                        sock = key.fileobj
                        sid = tag
                        try:
                            data = sock.recv(READ_SIZE)
                        except:
                            data = b""
                        if data:
                            send_frame(fd_ser, sid, 1, data)
                        else:
                            send_frame(fd_ser, sid, 2)
                            sel.unregister(sock)
                            sock.close()
                            conns.pop(sid, None)
            break  # shutdown_event was triggered
        except Exception as e:
            logging.warning(f"Tunnel source restarting after error: {e}")
            _cleanup(sel, fd_ser, conns, server)
            time.sleep(min(backoff, 60))
            backoff *= 2
        else:
            backoff = 1  # reset on success


def run_tunnel_dest(dev, baud, ip, port, shutdown_event):
    logging.info(f'Starting tunnel dest loop: {dev}@{baud} -> {ip}:{port}')
    backoff = 1
    while not shutdown_event.is_set():
        fd_ser = None
        conns = {}
        try:
            fd_ser = open_serial(dev, baud)
            sel = selectors.DefaultSelector()
            sel.register(fd_ser, selectors.EVENT_READ, 'serial')

            backoff = 1 # reset after connection

            while not shutdown_event.is_set():
                for key, _ in sel.select(timeout=1.0):
                    tag = key.data
                    if tag == 'serial':
                        try:
                            sid, cmd, data = read_frame(fd_ser)
                        except Exception as e:
                            logging.error(f"Tunnel dest fatal error: {e}")
                            raise
                        if cmd == 0:
                            sock = socket.socket()
                            sock.setblocking(False)
                            try: sock.connect((ip, port))
                            except BlockingIOError: pass
                            conns[sid] = sock
                            sel.register(sock, selectors.EVENT_READ, sid)
                        elif cmd == 1 and sid in conns:
                            conns[sid].sendall(data)
                        elif cmd == 2 and sid in conns:
                            sel.unregister(conns[sid])
                            conns[sid].close()
                            conns.pop(sid, None)
                    else:
                        sock = key.fileobj
                        sid = tag
                        try:
                            data = sock.recv(READ_SIZE)
                        except:
                            data = b""
                        if data:
                            send_frame(fd_ser, sid, 1, data)
                        else:
                            send_frame(fd_ser, sid, 2)
                            sel.unregister(sock)
                            sock.close()
                            conns.pop(sid, None)
        except Exception as e:
            logging.warning(f"Tunnel dest restarting after error: {e}")
            _cleanup(sel, fd_ser, conns)
            time.sleep(min(backoff, 60))
            backoff *= 2