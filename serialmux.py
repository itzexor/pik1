#!/usr/bin/env python3

from __future__ import annotations

import argparse
import errno
import fcntl
import os
import pty
import selectors
import socket
import struct
import sys
import termios
import time
import zlib
from typing import Callable, Dict, List, Optional, Tuple

# -- Frame protocol -------------------------------------------------------------
#
#  [ 0xAA 0x55 ][ type:1 ][ channel:1 ][ length:2 LE ][ payload:N ][ crc32:4 LE ]
#
MAGIC    = b'\xAA\x55'
HDR_FMT  = '<2sBBH'               # magic(2) type(1) channel(1) length(2)
HDR_SIZE = struct.calcsize(HDR_FMT)   # 6
CRC_SIZE = 4

# Control frames
F_DATA   = 0x01   # raw serial / passthrough bytes
F_FLUSH  = 0x02   # exporter->host: MCU resetting, discard + go WAITING
F_READY  = 0x03   # exporter->host: MCU booted, go ACTIVE
F_HELLO  = 0x05   # link handshake
F_ACK    = 0x06   # link handshake reply
# TCP tunnel frames  (payload always prefixed with conn_id: 2B LE)
F_TCONN  = 0x10   # new TCP connection opened
F_TDATA  = 0x11   # TCP payload
F_TCLOSE = 0x12   # TCP connection closed
# Keepalive
F_PING   = 0x20
F_PONG   = 0x21

MAX_PAYLOAD    = 16 * 1024
MAX_TICK       = 1.0           # upper bound on select() sleep when idle

# MCU watchdog: how long UART can be silent before we assume a reset.
# This must be:
#   > identify handshake window: MCU sends one sync frame then waits silently
#     for Klipper to respond. That round trip can take several seconds.
#   < bootloader dwell time: the GD32 on the K1 sits in its bootloader for
#     ~14-15 seconds before jumping to Klipper firmware. 5s is comfortably
#     inside that window so genuine resets are always caught.
# During normal printing the MCU is never silent for even 1s, so 5s cannot
# false-fire during normal operation.
RESET_SILENCE  = 5.0

# Klipper firmware sync byte -- seeing this after silence means MCU is booted
KLIPPER_SYNC   = 0x7E

# Keepalive
KA_INTERVAL    = 3.0            # send PING after this many seconds idle TX
KA_TIMEOUT     = 10.0           # declare link dead after this many seconds no RX

# Backpressure
LINK_HIGH_WATER = 512 * 1024    # pause channel reads when link TX queue exceeds this
LINK_LOW_WATER  = 256 * 1024
CONN_HIGH_WATER = 256 * 1024    # per TCP connection outbound buffer cap

SendFn = Callable[[int, int, bytes], None]   # (ftype, channel_id, payload)


# -- Logging --------------------------------------------------------------------

def _log(msg: str) -> None:
    print(f'{time.strftime("%H:%M:%S")} {msg}', file=sys.stderr, flush=True)



# -- USB sysfs discovery --------------------------------------------------------

def find_acm_by_usb_id(vid: str, pid: str) -> Optional[str]:
    """
    Scan sysfs for a ttyACM* device whose USB parent matches vid:pid.
    Returns the first match as '/dev/ttyACMN', or None if not found.
    Walks up to 8 levels of the sysfs tree looking for idVendor/idProduct.
    """
    base = '/sys/class/tty'
    try:
        entries = sorted(os.listdir(base))
    except OSError:
        return None
    for name in entries:
        if not name.startswith('ttyACM'):
            continue
        path = os.path.join(base, name)
        try:
            path = os.path.realpath(path)
        except OSError:
            pass
        cur = path
        for _ in range(8):
            vendor_f = os.path.join(cur, 'idVendor')
            product_f = os.path.join(cur, 'idProduct')
            if os.path.isfile(vendor_f) and os.path.isfile(product_f):
                try:
                    v = open(vendor_f).read().strip()
                    p = open(product_f).read().strip()
                    if v == vid and p == pid:
                        return f'/dev/{name}'
                except OSError:
                    pass
                break
            parent = os.path.dirname(cur)
            if parent == cur:
                break
            cur = parent
    return None


def wait_for_acm(vid: str, pid: str) -> str:
    """
    Block until a ttyACM* matching vid:pid appears in sysfs.
    Logs once on first miss, then every 10s. Returns the device path.
    """
    logged = False
    last_log = 0.0
    while True:
        dev = find_acm_by_usb_id(vid, pid)
        if dev is not None:
            return dev
        now = time.monotonic()
        if not logged or (now - last_log) >= 10.0:
            _log(f'USB {vid}:{pid} -- waiting for device to appear...')
            logged = True
            last_log = now
        time.sleep(0.5)

# -- TTY helpers ----------------------------------------------------------------

def open_serial_fd(dev: str, baud: int) -> int:
    fd = os.open(dev, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attrs        = termios.tcgetattr(fd)
    attrs[0]     = 0                                          # iflag: no processing
    attrs[1]     = 0                                          # oflag
    attrs[2]     = termios.CREAD | termios.CLOCAL | termios.CS8  # cflag
    attrs[3]     = 0                                          # lflag
    attrs[6][termios.VMIN]  = 0
    attrs[6][termios.VTIME] = 0
    baud_attr = getattr(termios, f'B{baud}', None)
    if baud_attr is None:
        raise ValueError(f'Unsupported baud rate: {baud}')
    attrs[4] = baud_attr  # ispeed
    attrs[5] = baud_attr  # ospeed
    termios.tcsetattr(fd, termios.TCSAFLUSH, attrs)
    return fd


def open_pty_raw(baud: int) -> Tuple[int, int]:
    """Return (master_fd, slave_fd) with slave in raw mode."""
    master_fd, slave_fd = pty.openpty()
    attrs        = termios.tcgetattr(slave_fd)
    attrs[0]     = 0
    attrs[1]     = 0
    attrs[2]     = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3]     = 0
    baud_attr    = getattr(termios, f'B{baud}', termios.B230400)
    attrs[4]     = baud_attr
    attrs[5]     = baud_attr
    attrs[6][termios.VMIN]  = 0
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(slave_fd, termios.TCSANOW, attrs)
    fcntl.fcntl(master_fd, fcntl.F_SETFL,
                fcntl.fcntl(master_fd, fcntl.F_GETFL) | os.O_NONBLOCK)
    return master_fd, slave_fd


# -- Frame builder --------------------------------------------------------------

def build_frame(ftype: int, channel: int, payload: bytes = b'') -> bytes:
    hdr = struct.pack(HDR_FMT, MAGIC, ftype, channel, len(payload))
    crc = struct.pack('<I', zlib.crc32(payload) & 0xFFFFFFFF)
    return hdr + payload + crc


# -- Streaming frame parser -----------------------------------------------------

class FrameParser:
    """
    Feed arbitrary byte chunks; fires on_frame(ftype, channel, payload) for
    every CRC-valid complete frame. Re-syncs automatically on corruption.
    """

    def __init__(self, on_frame: Callable[[int, int, bytes], None]):
        self._cb  = on_frame
        self._buf = bytearray()

    def feed(self, data: bytes) -> None:
        self._buf += data
        while True:
            i = self._buf.find(MAGIC)
            if i < 0:
                # No magic present; keep a tail in case magic straddles the next read
                keep = HDR_SIZE + CRC_SIZE
                if len(self._buf) > keep:
                    del self._buf[:-keep]
                break
            if i > 0:
                del self._buf[:i]
            if len(self._buf) < HDR_SIZE:
                break
            _, ftype, channel, length = struct.unpack(HDR_FMT, self._buf[:HDR_SIZE])
            if length > MAX_PAYLOAD:
                del self._buf[:2]   # skip magic, rescan
                continue
            total = HDR_SIZE + length + CRC_SIZE
            if len(self._buf) < total:
                break
            payload  = bytes(self._buf[HDR_SIZE:HDR_SIZE + length])
            crc_rx,  = struct.unpack('<I', self._buf[HDR_SIZE + length:total])
            if (zlib.crc32(payload) & 0xFFFFFFFF) != crc_rx:
                del self._buf[:2]
                continue
            del self._buf[:total]
            try:
                self._cb(ftype, channel, payload)
            except Exception as e:
                _log(f'Frame callback error: {e}')


# -- Link TX queue --------------------------------------------------------------

class LinkTxQueue:
    """
    Outbound frame queue for the CDC link.
    Frames are appended to a bytearray and drained with a single os.write().
    Partial writes are handled by advancing an offset into the buffer.
    """

    def __init__(self):
        self._buf: bytearray = bytearray()
        self.queued_bytes = 0

    def enqueue(self, frame: bytes) -> None:
        self._buf += frame
        self.queued_bytes += len(frame)

    def empty(self) -> bool:
        return not self._buf

    def drain_to_fd(self, fd: int) -> int:
        """Drain as much as possible with one write. Returns bytes written."""
        if not self._buf:
            return 0
        try:
            written = os.write(fd, self._buf)
        except BlockingIOError:
            return 0
        except OSError as e:
            if e.errno in (errno.EAGAIN, errno.EWOULDBLOCK):
                return 0
            raise
        del self._buf[:written]
        self.queued_bytes -= written
        return written


# -- Channel base class ---------------------------------------------------------

class Channel:
    def __init__(self, channel_id: int):
        self.channel_id = channel_id

    def on_frame(self, ftype: int, payload: bytes) -> None:
        raise NotImplementedError

    def on_link_connect(self) -> None:
        pass

    def on_link_disconnect(self) -> None:
        pass

    def tick(self, now: float) -> None:
        pass

    def next_deadline(self, now: float) -> Optional[float]:
        """Return the next absolute time this channel needs tick() called,
        or None if the channel has no pending timers."""
        return None

    def close(self) -> None:
        pass

    def pause_source_reads(self) -> None:
        """Called by Daemon when the link TX queue is above high-water mark."""
        pass

    def resume_source_reads(self) -> None:
        """Called by Daemon when the link TX queue drains below low-water mark."""
        pass


# -- MCU channel -- exporter side ------------------------------------------------

class McuChannel(Channel):
    """
    Bridges a physical UART (ttyS*) to the mux link.

    State machine:
        INIT      -- startup; no data observed yet
        ACTIVE    -- MCU running Klipper firmware; data flows freely
        RESETTING -- UART went silent; discarding bytes until 0x7E confirms reboot

    The watchdog runs in tick() -- no extra thread needed.
    UART reopen is also handled in tick() on failure.
    """

    ST_INIT      = 'INIT'
    ST_ACTIVE    = 'ACTIVE'
    ST_RESETTING = 'RESETTING'

    def __init__(self, channel_id: int, device: str, baud: int,
                 send: SendFn, sel: selectors.BaseSelector):
        super().__init__(channel_id)
        self._dev     = device
        self._baud    = baud
        self._send    = send
        self._sel     = sel
        self._state   = self.ST_INIT
        self._last_rx = 0.0
        self._link_up = False
        self._fd: Optional[int]   = None
        self._txbuf   = bytearray()
        self._reopen_at = 0.0
        self._bp_paused   = False   # link TX queue above high-water
        self._uart_in_sel = False   # whether _fd is currently in the selector

        self._open_uart()

    # -- UART lifecycle ---------------------------------------------------------

    def _open_uart(self) -> None:
        try:
            fd = open_serial_fd(self._dev, self._baud)
        except OSError as e:
            _log(f'MCU ch{self.channel_id}: cannot open {self._dev}: {e} -- retry in 2s')
            self._reopen_at = time.monotonic() + 2.0
            return
        self._fd = fd
        self._uart_in_sel = False
        self._update_uart_interest()
        _log(f'MCU ch{self.channel_id}: opened {self._dev} @ {self._baud}')

    def _close_uart(self) -> None:
        if self._fd is None:
            return
        self._uart_in_sel = False
        try:
            self._sel.unregister(self._fd)
        except Exception:
            pass
        try:
            os.close(self._fd)
        except Exception:
            pass
        self._fd = None

    # -- Selector callback ------------------------------------------------------

    def _on_uart_event(self, key, mask) -> None:
        if mask & selectors.EVENT_READ:
            self._uart_read()
        if mask & selectors.EVENT_WRITE:
            self._uart_drain()

    def _uart_read(self) -> None:
        try:
            data = os.read(self._fd, 4096)
        except BlockingIOError:
            return
        except OSError as e:
            _log(f'MCU ch{self.channel_id}: UART read error: {e}')
            self._close_uart()
            self._transition(self.ST_RESETTING)   # sends FLUSH so host goes WAITING
            self._reopen_at = time.monotonic() + 1.0
            return

        if not data:
            return

        self._last_rx = time.monotonic()

        if self._state in (self.ST_INIT, self.ST_RESETTING):
            if KLIPPER_SYNC in data:
                idx = data.index(KLIPPER_SYNC)
                _log(f'MCU ch{self.channel_id}: 0x7E seen at offset {idx} -> ACTIVE')
                self._transition(self.ST_ACTIVE)
                if self._link_up:
                    self._send(F_DATA, self.channel_id, data[idx:])
            # else: bootloader noise -- discard silently
        else:
            if self._link_up:
                self._send(F_DATA, self.channel_id, data)

    def _uart_drain(self) -> None:
        if not self._txbuf or self._fd is None:
            return
        try:
            n = os.write(self._fd, self._txbuf)
            del self._txbuf[:n]
        except BlockingIOError:
            pass
        except OSError as e:
            _log(f'MCU ch{self.channel_id}: UART write error: {e}')
            self._txbuf.clear()
        self._update_uart_interest()

    def _update_uart_interest(self) -> None:
        if self._fd is None:
            return
        events = (selectors.EVENT_READ if not self._bp_paused else 0)
        if self._txbuf:
            events |= selectors.EVENT_WRITE
        if events == 0:
            if self._uart_in_sel:
                try:
                    self._sel.unregister(self._fd)
                except Exception:
                    pass
                self._uart_in_sel = False
        elif self._uart_in_sel:
            try:
                self._sel.modify(self._fd, events, self._on_uart_event)
            except Exception:
                pass
        else:
            try:
                self._sel.register(self._fd, events, self._on_uart_event)
                self._uart_in_sel = True
            except Exception:
                pass

    # -- State machine ----------------------------------------------------------

    def _transition(self, new_state: str) -> None:
        if new_state == self._state:
            return
        _log(f'MCU ch{self.channel_id}: {self._state} -> {new_state}')
        self._state = new_state
        if new_state == self.ST_RESETTING:
            self._last_rx = 0.0
            self._txbuf.clear()
            self._update_uart_interest()
            if self._link_up:
                self._send(F_FLUSH, self.channel_id, b'')
        elif new_state == self.ST_ACTIVE:
            if self._link_up:
                self._send(F_READY, self.channel_id, b'')

    # -- Channel interface ------------------------------------------------------

    def on_frame(self, ftype: int, payload: bytes) -> None:
        """DATA frame from host (Klipper -> MCU direction)."""
        if ftype == F_DATA:
            if self._fd is None:
                return
            if self._state != self.ST_ACTIVE:
                # Don't write Klipper protocol data to the bootloader
                return
            self._txbuf += payload
            self._uart_drain()

    def on_link_connect(self) -> None:
        self._link_up = True
        # Re-broadcast current state so the host always knows where we are
        if self._state == self.ST_ACTIVE:
            self._send(F_READY, self.channel_id, b'')
        else:
            self._send(F_FLUSH, self.channel_id, b'')

    def on_link_disconnect(self) -> None:
        self._link_up = False
        self._txbuf.clear()
        self._update_uart_interest()

    def pause_source_reads(self) -> None:
        if self._bp_paused:
            return
        self._bp_paused = True
        self._update_uart_interest()

    def resume_source_reads(self) -> None:
        if not self._bp_paused:
            return
        self._bp_paused = False
        self._update_uart_interest()

    def tick(self, now: float) -> None:
        # Reopen UART if it previously failed
        if self._fd is None and now >= self._reopen_at:
            self._open_uart()
            return

        # Watchdog: silence after active period -> MCU has reset
        if (self._state == self.ST_ACTIVE
                and self._last_rx > 0
                and (now - self._last_rx) > RESET_SILENCE):
            self._transition(self.ST_RESETTING)

    def next_deadline(self, now: float) -> Optional[float]:
        if self._fd is None:
            return self._reopen_at
        if self._state == self.ST_ACTIVE and self._last_rx > 0:
            return self._last_rx + RESET_SILENCE
        return None

    def close(self) -> None:
        self._close_uart()


# -- PTY channel -- host side ----------------------------------------------------

class PtyChannel(Channel):
    """
    Presents a PTY slave (via a stable symlink) to Klipper and bridges it
    to the mux link.

    States:
        WAITING -- PTY closed, symlink absent; Klipper is in reconnect loop
        ACTIVE  -- PTY open, symlink present; data flows freely

    On FLUSH or link disconnect the PTY is torn down entirely so Klipper
    gets an immediate EIO on its held fd and enters its reconnect loop.
    On READY a fresh PTY is created and the symlink restored so Klipper
    can reopen it cleanly.

    This means Klipper always experiences a clean open/close cycle around
    every MCU reset or bridge outage rather than accumulating serial
    timeouts against an open-but-unresponsive port.
    """

    def __init__(self, channel_id: int, symlink: str, baud: int,
                 send: SendFn, sel: selectors.BaseSelector):
        super().__init__(channel_id)
        self._symlink   = symlink
        self._baud      = baud
        self._send      = send
        self._sel       = sel
        self._master_fd: Optional[int] = None
        self._slave_fd:  Optional[int] = None
        self._txbuf     = bytearray()
        self._bp_paused    = False
        self._master_in_sel = False

        # Start closed -- PTY opens on first READY
        self._remove_symlink()

    # -- PTY lifecycle ------------------------------------------------------------

    def _open_pty(self) -> None:
        """Create a new PTY and point the symlink at its slave end."""
        if self._master_fd is not None:
            return  # already open
        master_fd, slave_fd = open_pty_raw(self._baud)
        slave_path = os.ttyname(slave_fd)
        try:
            self._remove_symlink()
            os.symlink(slave_path, self._symlink)
        except OSError as e:
            _log(f'PTY ch{self.channel_id}: open failed: {e}')
            try:
                os.close(master_fd)
            except Exception:
                pass
            try:
                os.close(slave_fd)
            except Exception:
                pass
            return
        self._master_fd = master_fd
        self._slave_fd  = slave_fd
        self._master_in_sel = False
        self._update_master_interest()
        _log(f'PTY ch{self.channel_id}: opened {slave_path} -> {self._symlink}')

    def _close_pty(self) -> None:
        """Tear down the PTY so Klipper gets EIO and enters its reconnect loop."""
        if self._master_fd is None:
            return  # already closed
        self._master_in_sel = False
        try:
            self._sel.unregister(self._master_fd)
        except Exception:
            pass
        for fd in (self._master_fd, self._slave_fd):
            try:
                os.close(fd)
            except Exception:
                pass
        self._master_fd = None
        self._slave_fd  = None
        self._txbuf.clear()
        self._remove_symlink()
        _log(f'PTY ch{self.channel_id}: closed')

    def _remove_symlink(self) -> None:
        try:
            os.unlink(self._symlink)
        except OSError:
            pass

    # -- I/O ----------------------------------------------------------------------

    def _on_master_event(self, key, mask) -> None:
        if mask & selectors.EVENT_READ:
            try:
                data = os.read(self._master_fd, 4096)
            except (BlockingIOError, OSError):
                data = None
            if data:
                self._send(F_DATA, self.channel_id, data)
        if mask & selectors.EVENT_WRITE:
            self._pty_drain()

    def _pty_drain(self) -> None:
        if not self._txbuf or self._master_fd is None:
            return
        try:
            n = os.write(self._master_fd, self._txbuf)
            del self._txbuf[:n]
        except BlockingIOError:
            pass
        except OSError as e:
            _log(f'PTY ch{self.channel_id}: write error: {e}')
            self._txbuf.clear()
        self._update_master_interest()

    def _update_master_interest(self) -> None:
        if self._master_fd is None:
            return
        events = (selectors.EVENT_READ if not self._bp_paused else 0)
        if self._txbuf:
            events |= selectors.EVENT_WRITE
        if events == 0:
            if self._master_in_sel:
                try:
                    self._sel.unregister(self._master_fd)
                except Exception:
                    pass
                self._master_in_sel = False
        elif self._master_in_sel:
            try:
                self._sel.modify(self._master_fd, events, self._on_master_event)
            except Exception:
                pass
        else:
            try:
                self._sel.register(self._master_fd, events, self._on_master_event)
                self._master_in_sel = True
            except Exception:
                pass

    # -- Channel interface --------------------------------------------------------

    def on_frame(self, ftype: int, payload: bytes) -> None:
        if ftype == F_FLUSH:
            _log(f'PTY ch{self.channel_id}: FLUSH -> WAITING')
            self._close_pty()

        elif ftype == F_READY:
            _log(f'PTY ch{self.channel_id}: READY -> ACTIVE')
            self._open_pty()

        elif ftype == F_DATA:
            if self._master_fd is not None:
                self._txbuf += payload
                self._pty_drain()
            # Discard while closed -- Klipper will reopen cleanly after READY

    def on_link_connect(self) -> None:
        # Stay closed until exporter confirms MCU state via READY
        self._close_pty()

    def on_link_disconnect(self) -> None:
        _log(f'PTY ch{self.channel_id}: link down')
        self._close_pty()

    def pause_source_reads(self) -> None:
        if self._bp_paused:
            return
        self._bp_paused = True
        self._update_master_interest()

    def resume_source_reads(self) -> None:
        if not self._bp_paused:
            return
        self._bp_paused = False
        self._update_master_interest()

    def close(self) -> None:
        self._close_pty()


# -- TCP tunnel helpers ---------------------------------------------------------

def _pack_cid(cid: int) -> bytes:
    return struct.pack('<H', cid)

def _unpack_cid(payload: bytes) -> Tuple[Optional[int], bytes]:
    if len(payload) < 2:
        return None, b''
    return struct.unpack_from('<H', payload, 0)[0], payload[2:]


class _TcpConn:
    """State for one tunnelled TCP connection."""
    __slots__ = ('sock', 'txbuf', 'connecting', 'in_sel')
    def __init__(self, sock: socket.socket, connecting: bool = False):
        self.sock       = sock
        self.txbuf      = bytearray()
        self.connecting = connecting
        self.in_sel     = False


# -- TCP source channel -- exporter side ----------------------------------------

class TcpSourceChannel(Channel):
    """
    Listens for TCP connections (e.g., from the stock screen) and tunnels
    each one to the host via TCONN / TDATA / TCLOSE frames.

    conn_id is 2 bytes LE, allocated sequentially with wrap-around.
    All connections are torn down when the link goes down.
    """

    def __init__(self, channel_id: int, bind_addr: str, bind_port: int,
                 send: SendFn, sel: selectors.BaseSelector):
        super().__init__(channel_id)
        self._send     = send
        self._sel      = sel
        self._link_up  = False
        self._bp_paused = False
        self._next_cid = 0
        self._conns:   Dict[int, _TcpConn]       = {}
        self._by_sock: Dict[socket.socket, int]  = {}

        self._server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._server.bind((bind_addr, bind_port))
        self._server.listen(32)
        self._server.setblocking(False)
        self._sel.register(self._server, selectors.EVENT_READ, self._on_accept)
        _log(f'TCP src ch{channel_id}: listening {bind_addr}:{bind_port}')

    def _alloc_cid(self) -> Optional[int]:
        for _ in range(65536):
            cid = self._next_cid
            self._next_cid = (self._next_cid + 1) % 65536
            if cid not in self._conns:
                return cid
        return None

    def _on_accept(self, key, mask) -> None:
        try:
            sock, addr = self._server.accept()
        except OSError as e:
            # BlockingIOError (EAGAIN): no connection pending -- normal
            # OSError(EMFILE/ENFILE): too many open files -- log and back off
            if e.errno not in (errno.EAGAIN, errno.EWOULDBLOCK):
                _log(f'TCP src ch{self.channel_id}: accept error: {e}')
            return
        sock.setblocking(False)
        try:
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        except OSError:
            pass

        if not self._link_up:
            sock.close()
            return

        cid = self._alloc_cid()
        if cid is None:
            _log(f'TCP src ch{self.channel_id}: conn_id pool exhausted')
            sock.close()
            return

        self._conns[cid]    = _TcpConn(sock)
        self._by_sock[sock] = cid
        self._update_sock_interest(cid)
        _log(f'TCP src ch{self.channel_id}: accepted {addr} cid={cid}')
        self._send(F_TCONN, self.channel_id, _pack_cid(cid))

    def _on_tcp_event(self, key, mask) -> None:
        sock = key.fileobj
        cid  = self._by_sock.get(sock)
        if cid is None:
            return
        conn = self._conns.get(cid)
        if conn is None:
            return

        if mask & selectors.EVENT_READ:
            try:
                data = sock.recv(65536)
            except BlockingIOError:
                data = None
            except OSError:
                data = b''
            if data is None:
                pass  # would block
            elif data:
                off = 0
                while off < len(data):
                    chunk = data[off:off + MAX_PAYLOAD - 2]
                    self._send(F_TDATA, self.channel_id, _pack_cid(cid) + chunk)
                    off += len(chunk)
            else:
                self._close_cid(cid, notify=True)
                return

        if mask & selectors.EVENT_WRITE:
            if conn.txbuf:
                try:
                    n = sock.send(conn.txbuf)
                    del conn.txbuf[:n]
                except (BlockingIOError, ConnectionResetError, BrokenPipeError) as e:
                    if not isinstance(e, BlockingIOError):
                        self._close_cid(cid, notify=True)
                        return
            self._update_sock_interest(cid)

    def _update_sock_interest(self, cid: int) -> None:
        conn = self._conns.get(cid)
        if conn is None:
            return
        events = (selectors.EVENT_READ if not self._bp_paused else 0)
        if conn.txbuf:
            events |= selectors.EVENT_WRITE
        if events == 0:
            if conn.in_sel:
                try:
                    self._sel.unregister(conn.sock)
                except Exception:
                    pass
                conn.in_sel = False
        elif conn.in_sel:
            try:
                self._sel.modify(conn.sock, events, self._on_tcp_event)
            except Exception:
                pass
        else:
            try:
                self._sel.register(conn.sock, events, self._on_tcp_event)
                conn.in_sel = True
            except Exception:
                pass

    def _close_cid(self, cid: int, notify: bool = False) -> None:
        conn = self._conns.pop(cid, None)
        if conn is None:
            return
        self._by_sock.pop(conn.sock, None)
        try:
            self._sel.unregister(conn.sock)
        except Exception:
            pass
        try:
            conn.sock.close()
        except Exception:
            pass
        if notify and self._link_up:
            self._send(F_TCLOSE, self.channel_id, _pack_cid(cid))
        _log(f'TCP src ch{self.channel_id}: closed cid={cid}')

    def on_frame(self, ftype: int, payload: bytes) -> None:
        cid, data = _unpack_cid(payload)
        if cid is None:
            return
        if ftype == F_TDATA:
            conn = self._conns.get(cid)
            if conn and data:
                conn.txbuf += data
                if len(conn.txbuf) > CONN_HIGH_WATER:
                    _log(f'TCP src ch{self.channel_id}: cid={cid} high-water -- closing')
                    self._close_cid(cid, notify=True)
                    return
                self._update_sock_interest(cid)
        elif ftype == F_TCLOSE:
            self._close_cid(cid, notify=False)

    def on_link_connect(self) -> None:
        self._link_up = True

    def on_link_disconnect(self) -> None:
        self._link_up = False
        for cid in list(self._conns):
            self._close_cid(cid, notify=False)

    def pause_source_reads(self) -> None:
        if self._bp_paused:
            return
        self._bp_paused = True
        for cid in list(self._conns):
            self._update_sock_interest(cid)

    def resume_source_reads(self) -> None:
        if not self._bp_paused:
            return
        self._bp_paused = False
        for cid in list(self._conns):
            self._update_sock_interest(cid)

    def close(self) -> None:
        self.on_link_disconnect()
        try:
            self._sel.unregister(self._server)
        except Exception:
            pass
        try:
            self._server.close()
        except Exception:
            pass


# -- TCP destination channel -- host side ---------------------------------------

class TcpDestChannel(Channel):
    """
    Receives TCONN / TDATA / TCLOSE frames from the exporter and connects
    each to a local TCP service (e.g., Moonraker on localhost:7125).
    """

    def __init__(self, channel_id: int, dest_addr: str, dest_port: int,
                 send: SendFn, sel: selectors.BaseSelector):
        super().__init__(channel_id)
        self._send      = send
        self._sel       = sel
        self._dest_addr = dest_addr
        self._dest_port = dest_port
        self._bp_paused = False
        self._conns:   Dict[int, _TcpConn]       = {}
        self._by_sock: Dict[socket.socket, int]  = {}

    def on_frame(self, ftype: int, payload: bytes) -> None:
        cid, data = _unpack_cid(payload)
        if cid is None:
            return

        if ftype == F_TCONN:
            if cid not in self._conns:
                self._open_conn(cid)

        elif ftype == F_TDATA:
            conn = self._conns.get(cid)
            if conn and data:
                conn.txbuf += data
                if len(conn.txbuf) > CONN_HIGH_WATER:
                    _log(f'TCP dst ch{self.channel_id}: cid={cid} high-water -- closing')
                    self._close_cid(cid, notify=True)
                    return
                self._update_sock_interest(cid)

        elif ftype == F_TCLOSE:
            self._close_cid(cid, notify=False)

    def _open_conn(self, cid: int) -> None:
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.setblocking(False)
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            err = sock.connect_ex((self._dest_addr, self._dest_port))
        except OSError as e:
            _log(f'TCP dst ch{self.channel_id}: cannot open cid={cid}: {e}')
            try:
                sock.close()
            except Exception:
                pass
            self._send(F_TCLOSE, self.channel_id, _pack_cid(cid))
            return

        # EINPROGRESS/EALREADY/EWOULDBLOCK: async connect in progress (normal)
        # 0: immediate success (unusual but valid for loopback)
        # anything else (e.g. ECONNREFUSED): immediate failure
        if err not in (0, errno.EINPROGRESS, errno.EALREADY, errno.EWOULDBLOCK):
            _log(f'TCP dst ch{self.channel_id}: connect cid={cid} failed immediately: {errno.errorcode.get(err, err)}')
            try:
                sock.close()
            except Exception:
                pass
            self._send(F_TCLOSE, self.channel_id, _pack_cid(cid))
            return

        connecting = (err != 0)   # err==0 means already connected
        self._conns[cid]    = _TcpConn(sock, connecting=connecting)
        self._by_sock[sock] = cid
        self._update_sock_interest(cid)
        _log(f'TCP dst ch{self.channel_id}: connecting cid={cid} -> '
             f'{self._dest_addr}:{self._dest_port}')

    def _on_tcp_event(self, key, mask) -> None:
        sock = key.fileobj
        cid  = self._by_sock.get(sock)
        if cid is None:
            return
        conn = self._conns.get(cid)
        if conn is None:
            return

        # Finalise async connect
        if conn.connecting and (mask & selectors.EVENT_WRITE):
            err = sock.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR)
            if err != 0:
                _log(f'TCP dst ch{self.channel_id}: connect failed cid={cid} errno={err}')
                self._close_cid(cid, notify=True)
                return
            conn.connecting = False
            _log(f'TCP dst ch{self.channel_id}: connected cid={cid}')

        if (mask & selectors.EVENT_WRITE) and not conn.connecting:
            if conn.txbuf:
                try:
                    n = sock.send(conn.txbuf)
                    del conn.txbuf[:n]
                except (BlockingIOError, ConnectionResetError, BrokenPipeError) as e:
                    if not isinstance(e, BlockingIOError):
                        self._close_cid(cid, notify=True)
                        return
            self._update_sock_interest(cid)

        if mask & selectors.EVENT_READ:
            try:
                data = sock.recv(65536)
            except BlockingIOError:
                data = None
            except OSError:
                data = b''
            if data is None:
                pass
            elif data:
                off = 0
                while off < len(data):
                    chunk = data[off:off + MAX_PAYLOAD - 2]
                    self._send(F_TDATA, self.channel_id, _pack_cid(cid) + chunk)
                    off += len(chunk)
            else:
                self._close_cid(cid, notify=True)

    def _update_sock_interest(self, cid: int) -> None:
        conn = self._conns.get(cid)
        if conn is None:
            return
        events = (selectors.EVENT_READ if not self._bp_paused else 0)
        if conn.txbuf or conn.connecting:
            events |= selectors.EVENT_WRITE
        if events == 0:
            if conn.in_sel:
                try:
                    self._sel.unregister(conn.sock)
                except Exception:
                    pass
                conn.in_sel = False
        elif conn.in_sel:
            try:
                self._sel.modify(conn.sock, events, self._on_tcp_event)
            except Exception:
                pass
        else:
            try:
                self._sel.register(conn.sock, events, self._on_tcp_event)
                conn.in_sel = True
            except Exception:
                pass

    def _close_cid(self, cid: int, notify: bool = False) -> None:
        conn = self._conns.pop(cid, None)
        if conn is None:
            return
        self._by_sock.pop(conn.sock, None)
        try:
            self._sel.unregister(conn.sock)
        except Exception:
            pass
        try:
            conn.sock.close()
        except Exception:
            pass
        if notify:
            self._send(F_TCLOSE, self.channel_id, _pack_cid(cid))
        _log(f'TCP dst ch{self.channel_id}: closed cid={cid}')

    def on_link_disconnect(self) -> None:
        for cid in list(self._conns):
            self._close_cid(cid, notify=False)

    def pause_source_reads(self) -> None:
        if self._bp_paused:
            return
        self._bp_paused = True
        for cid in list(self._conns):
            self._update_sock_interest(cid)

    def resume_source_reads(self) -> None:
        if not self._bp_paused:
            return
        self._bp_paused = False
        for cid in list(self._conns):
            self._update_sock_interest(cid)

    def close(self) -> None:
        self.on_link_disconnect()


# -- Main daemon ----------------------------------------------------------------

class Daemon:
    """
    Owns the selector, the CDC link fd, all channels, and the main loop.
    All I/O -- link, UARTs, PTYs, TCP sockets -- runs in this single thread.
    """

    def __init__(self, mode: str, link_dev: Optional[str],
                 sel: selectors.BaseSelector,
                 channels: List[Channel],
                 usb_id: Optional[Tuple[str, str]] = None):
        self._mode     = mode
        self._link_dev = link_dev   # explicit path, or None when usb_id is used
        self._usb_id   = usb_id     # (vid, pid) for sysfs discovery, or None
        self._sel      = sel
        self._channels: Dict[int, Channel] = {ch.channel_id: ch for ch in channels}
        self._txq      = LinkTxQueue()
        self._parser   = FrameParser(self._on_frame)
        self._link_fd: Optional[int] = None
        self._link_up  = False
        self._last_rx  = 0.0
        self._last_tx  = 0.0

        self._disconnected  = True
        self._reopen_at     = 0.0
        self._reopen_delay  = 0.5
        self._link_bp_paused = False

        self._open_link()

    # -- Link fd management -----------------------------------------------------

    def _resolve_link_dev(self) -> Optional[str]:
        """
        Return the link device path, or None if not yet available.
        Always non-blocking -- callers must handle None and schedule a retry.
        The initial blocking wait is done in main() before the daemon starts.
        """
        if self._usb_id is not None:
            return find_acm_by_usb_id(*self._usb_id)
        return self._link_dev

    def _open_link(self) -> None:
        dev = self._resolve_link_dev()
        if dev is None:
            _log('Link: no device available')
            self._schedule_reopen()
            return
        try:
            fd = open_serial_fd(dev, 115200)
        except OSError as e:
            _log(f'Link: cannot open {dev}: {e}')
            self._schedule_reopen()
            return

        self._link_fd      = fd
        self._disconnected = False
        self._reopen_delay = 0.5
        self._link_bp_paused = False
        self._parser       = FrameParser(self._on_frame)
        self._txq          = LinkTxQueue()
        self._last_rx      = time.monotonic()
        self._last_tx      = time.monotonic()
        self._sel.register(fd, selectors.EVENT_READ, self._on_link_event)
        _log(f'Link: opened {dev}')
        # Initiate handshake immediately; _on_link_up() fires on ACK/HELLO from peer
        self._enqueue(build_frame(F_HELLO, 0))

    def _close_link(self, reason: str) -> None:
        if self._disconnected:
            return
        _log(f'Link: down -- {reason}')
        self._disconnected = True
        self._link_up      = False
        if self._link_fd is not None:
            try:
                self._sel.unregister(self._link_fd)
            except Exception:
                pass
            try:
                os.close(self._link_fd)
            except Exception:
                pass
            self._link_fd = None
        self._txq = LinkTxQueue()
        if self._link_bp_paused:
            self._link_bp_paused = False
            for ch in self._channels.values():
                try:
                    ch.resume_source_reads()
                except Exception:
                    pass
        for ch in self._channels.values():
            try:
                ch.on_link_disconnect()
            except Exception as e:
                _log(f'on_link_disconnect error: {e}')
        self._schedule_reopen()

    def _schedule_reopen(self) -> None:
        self._reopen_at    = time.monotonic() + self._reopen_delay
        self._reopen_delay = min(self._reopen_delay * 2.0, 8.0)

    # -- Link I/O ---------------------------------------------------------------

    def _on_link_event(self, key, mask) -> None:
        # Guard against stale events from a select() call that completed before
        # _close_link unregistered this fd (both events land in the same batch).
        if self._link_fd is None:
            return
        if mask & selectors.EVENT_READ:
            try:
                data = os.read(self._link_fd, 65536)
            except BlockingIOError:
                data = None
            except OSError as e:
                self._close_link(f'read error: {e}')
                return
            if data:
                self._last_rx = time.monotonic()
                self._parser.feed(data)
            # NOTE: 0-byte read on a non-blocking TTY (VMIN=0) means no data
            # available -- it is NOT EOF. Real disconnect on a TTY raises
            # OSError(EIO), which is caught above. Treating 0 as EOF causes
            # a reconnect storm on ttyGS* gadget ports, which report readable
            # with 0 bytes while the USB host is still establishing the link.

        if mask & selectors.EVENT_WRITE:
            try:
                self._txq.drain_to_fd(self._link_fd)
            except OSError as e:
                self._close_link(f'write error: {e}')
                return
            if self._link_bp_paused and self._txq.queued_bytes <= LINK_LOW_WATER:
                self._link_bp_paused = False
                for ch in self._channels.values():
                    try:
                        ch.resume_source_reads()
                    except Exception as e:
                        _log(f'resume_source_reads error: {e}')
            if self._txq.empty():
                try:
                    self._sel.modify(self._link_fd, selectors.EVENT_READ,
                                     self._on_link_event)
                except Exception:
                    pass

    def _enqueue(self, frame: bytes) -> None:
        if self._disconnected or self._link_fd is None:
            return
        self._txq.enqueue(frame)
        self._last_tx = time.monotonic()
        try:
            key = self._sel.get_key(self._link_fd)
            if not (key.events & selectors.EVENT_WRITE):
                self._sel.modify(self._link_fd,
                                 selectors.EVENT_READ | selectors.EVENT_WRITE,
                                 self._on_link_event)
        except Exception:
            pass

    def send(self, ftype: int, channel: int, payload: bytes) -> None:
        """Public send called by channels."""
        if self._disconnected:
            return
        # Hysteresis backpressure on bulk data frames only.
        # Control frames (FLUSH, READY, TCONN, TCLOSE, PING, etc.) always pass through.
        # When the TX queue hits HIGH_WATER we pause source reads on all channels so
        # data is held in the kernel buffer instead of being consumed and dropped here.
        # Reads are resumed when the queue drains below LOW_WATER in _on_link_event.
        if ftype in (F_DATA, F_TDATA):
            if not self._link_bp_paused and self._txq.queued_bytes >= LINK_HIGH_WATER:
                self._link_bp_paused = True
                for ch in self._channels.values():
                    try:
                        ch.pause_source_reads()
                    except Exception as e:
                        _log(f'pause_source_reads error: {e}')
        self._enqueue(build_frame(ftype, channel, payload))

    # -- Frame dispatch ---------------------------------------------------------

    def _on_frame(self, ftype: int, channel: int, payload: bytes) -> None:
        if ftype == F_PING:
            self._enqueue(build_frame(F_PONG, 0))
            return
        if ftype == F_PONG:
            return
        if ftype in (F_HELLO, F_ACK):
            if ftype == F_HELLO:
                # Always ACK a HELLO -- the peer may have reconnected
                self._enqueue(build_frame(F_ACK, 0))
                if self._link_up:
                    # Already linked: peer reconnected, re-broadcast channel
                    # states so it can resync without a full handshake cycle
                    _log(f'Link: peer reconnected, rebroadcasting channel states')
                    for ch in self._channels.values():
                        try:
                            ch.on_link_connect()
                        except Exception as e:
                            _log(f'on_link_connect error: {e}')
                    return
            # ACK or first HELLO: complete the handshake
            _log(f'Link: received {"HELLO" if ftype == F_HELLO else "ACK"} from peer')
            self._on_link_up()
            return

        ch = self._channels.get(channel)
        if ch is None:
            return
        try:
            ch.on_frame(ftype, payload)
        except Exception as e:
            _log(f'Channel {channel} on_frame error: {e}')

    def _on_link_up(self) -> None:
        if self._link_up:
            return
        self._link_up = True
        _log(f'Link: handshake complete ({self._mode})')
        for ch in self._channels.values():
            try:
                ch.on_link_connect()
            except Exception as e:
                _log(f'on_link_connect error: {e}')

    # -- Keepalive --------------------------------------------------------------

    def _tick_keepalive(self, now: float) -> None:
        if self._disconnected:
            return
        if self._last_rx > 0 and (now - self._last_rx) > KA_TIMEOUT:
            self._close_link('keepalive timeout')
            return
        if ((now - self._last_tx) >= KA_INTERVAL
                and self._txq.empty()
                and self._link_up):
            self._enqueue(build_frame(F_PING, 0))

    # -- Main loop --------------------------------------------------------------

    def _next_timeout(self, now: float) -> float:
        """
        Compute how long select() can sleep before a timer needs servicing.
        Returns a value in [0, MAX_TICK] seconds.
        """
        deadline = now + MAX_TICK

        if self._disconnected:
            deadline = min(deadline, self._reopen_at)
        elif self._link_up:
            if self._last_tx > 0:
                deadline = min(deadline, self._last_tx + KA_INTERVAL)
            if self._last_rx > 0:
                deadline = min(deadline, self._last_rx + KA_TIMEOUT)

        for ch in self._channels.values():
            d = ch.next_deadline(now)
            if d is not None:
                deadline = min(deadline, d)

        return max(0.0, deadline - now)

    def run(self) -> None:
        _log(f'serialmux {self._mode} started')
        while True:
            now = time.monotonic()

            if self._disconnected and now >= self._reopen_at:
                self._open_link()

            self._tick_keepalive(now)

            for ch in self._channels.values():
                try:
                    ch.tick(now)
                except Exception as e:
                    _log(f'Channel tick error: {e}')

            timeout = self._next_timeout(now)
            events = self._sel.select(timeout=timeout)
            for key, mask in events:
                try:
                    key.data(key, mask)
                except Exception as e:
                    _log(f'Selector callback error: {e}')


# -- Config parsing and validation ---------------------------------------------

class ConfigError(Exception):
    pass


def _validate_int(value: str, name: str, min_val: int = None, max_val: int = None) -> int:
    try:
        n = int(value)
    except ValueError:
        raise ConfigError(f'{name} must be an integer, got: {value!r}')
    if min_val is not None and n < min_val:
        raise ConfigError(f'{name} must be >= {min_val}, got: {n}')
    if max_val is not None and n > max_val:
        raise ConfigError(f'{name} must be <= {max_val}, got: {n}')
    return n


def _validate_baud(value: str, spec: str, valid_bauds: set) -> None:
    baud = _validate_int(value, f'baud in {spec!r}', min_val=1)
    if baud not in valid_bauds:
        raise ConfigError(
            f'Baud rate {baud} in {spec!r} is not a standard value.\n'
            f'  Valid rates: {sorted(valid_bauds)}'
        )


def _validate_channel_specs(mode: str, specs: List[str]) -> None:
    """
    Validate all channel specs before any file descriptors are opened.
    Raises ConfigError with a clear message on the first problem found.
    """
    seen_ids = {}
    valid_bauds = {1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200,
                   230400, 460800, 921600}

    for spec in specs:
        parts = spec.split(':')

        if len(parts) < 3:
            raise ConfigError(
                f'Channel spec {spec!r} is too short.\n'
                f'  exporter mcu format: mcu:<id>:<device>:<baud>\n'
                f'  exporter tcp format: tcp:<id>:<bind_addr>:<port>\n'
                f'  host mcu format:     mcu:<id>:<pty_symlink>[:<baud>]\n'
                f'  host tcp format:     tcp:<id>:<dest_addr>:<port>'
            )

        kind = parts[0]
        if kind not in ('mcu', 'tcp'):
            raise ConfigError(
                f'Unknown channel type {kind!r} in {spec!r} -- must be "mcu" or "tcp"'
            )

        cid = _validate_int(parts[1], f'channel id in {spec!r}', min_val=0, max_val=255)

        if cid in seen_ids:
            raise ConfigError(
                f'Duplicate channel id {cid} -- already used by {seen_ids[cid]!r}'
            )
        seen_ids[cid] = spec

        if kind == 'mcu':
            if mode == 'exporter':
                if len(parts) != 4:
                    raise ConfigError(
                        f'Exporter mcu spec {spec!r} must have exactly 4 parts: '
                        f'mcu:<id>:<device>:<baud>'
                    )
                dev = parts[2]
                if not dev.startswith('/dev/'):
                    raise ConfigError(
                        f'Device {dev!r} in {spec!r} does not look like a device path'
                    )
                _validate_baud(parts[3], spec, valid_bauds)
            else:
                if len(parts) not in (3, 4):
                    raise ConfigError(
                        f'Host mcu spec {spec!r} must have 3 or 4 parts: '
                        f'mcu:<id>:<pty_symlink>[:<baud>]'
                    )
                if len(parts) == 4:
                    _validate_baud(parts[3], spec, valid_bauds)

        elif kind == 'tcp':
            if len(parts) != 4:
                raise ConfigError(
                    f'TCP spec {spec!r} must have exactly 4 parts: '
                    f'tcp:<id>:<addr>:<port>'
                )
            _validate_int(parts[3], f'port in {spec!r}', min_val=1, max_val=65535)


def build_channels(mode: str, specs: List[str],
                   send: SendFn,
                   sel: selectors.BaseSelector) -> List[Channel]:
    channels: List[Channel] = []
    for spec in specs:
        parts = spec.split(':')
        kind  = parts[0]
        cid   = int(parts[1])

        if kind == 'mcu':
            if mode == 'exporter':
                channels.append(McuChannel(cid, parts[2], int(parts[3]), send, sel))
            else:
                baud = int(parts[3]) if len(parts) > 3 else 230400
                channels.append(PtyChannel(cid, parts[2], baud, send, sel))
        elif kind == 'tcp':
            if mode == 'exporter':
                channels.append(TcpSourceChannel(cid, parts[2], int(parts[3]), send, sel))
            else:
                channels.append(TcpDestChannel(cid, parts[2], int(parts[3]), send, sel))

    return channels


DESCRIPTION = '''\
serialmux -- Klipper serial + TCP multiplexer over a USB CDC gadget link.
Single-threaded, selector-based. No dependencies beyond the Python stdlib.

Multiplexes two Klipper MCU serial ports and an optional TCP tunnel over a
single USB CDC ACM serial link between two machines, using a simple framing
protocol with CRC32 integrity checking. Handles MCU resets cleanly by
detecting firmware boot (0x7E sync byte) and signalling the host to hold
Klipper off until the MCU is ready.

MODES
  exporter   Run on the machine physically wired to the MCUs (e.g. K1 board).
             Reads from UART devices and writes to the CDC gadget port.

  host       Run on the machine running Klipper (e.g. Raspberry Pi).
             Exposes PTY devices that Klipper connects to as serial ports.

LINK DEVICE
  Specify either a direct device path or a USB VID:PID to discover via sysfs:

  --usb VID:PID   (recommended for exporter)
    Discover the CDC ACM device by USB vendor and product ID. The daemon
    waits at startup and after each disconnect for the device to reappear,
    so no external USB-wait loop is needed in the init script.
    Example: --usb 1d6b:0104

  link_dev (positional argument)
    Explicit device path. Use this on the host side where the gadget serial
    device is always known ahead of time (e.g. /dev/ttyGS0).

CHANNEL SPECS
  One or more channel specs follow the link device. Each has the form:
  <type>:<id>:<addr>:<param>

  mcu channels (serial bridge):
    exporter:  mcu:<id>:<uart_device>:<baud>
               e.g.  mcu:0:/dev/ttyS7:230400
    host:      mcu:<id>:<pty_symlink>[:<baud>]
               e.g.  mcu:0:/tmp/klipper_mcu
               The PTY symlink is what you put in printer.cfg as the serial path.
               Baud defaults to 230400 if omitted.

  tcp channels (TCP tunnel, e.g. for a Moonraker screen on the exporter machine):
    exporter:  tcp:<id>:<bind_addr>:<port>
               e.g.  tcp:2:0.0.0.0:7125
               Accepts TCP connections and tunnels them to the host.
    host:      tcp:<id>:<dest_addr>:<port>
               e.g.  tcp:2:127.0.0.1:7125
               Receives tunnelled connections and forwards to a local service.

  Channel ids must be unique integers 0-255 and must match between exporter
  and host (e.g. mcu:0 on exporter must be mcu:0 on host).

EXAMPLES
  Exporter (K1 board) -- discover ACM by USB ID:
    python3 serialmux.py exporter --usb 1d6b:0104 \\
        mcu:0:/dev/ttyS7:230400 \\
        mcu:1:/dev/ttyS1:230400 \\
        tcp:2:0.0.0.0:7125

  Host (Raspberry Pi) -- explicit gadget device:
    python3 serialmux.py host /dev/ttyGS0 \\
        mcu:0:/tmp/klipper_mcu \\
        mcu:1:/tmp/klipper_toolhead \\
        tcp:2:127.0.0.1:7125

  printer.cfg:
    [mcu]
    serial: /tmp/klipper_mcu

    [mcu nozzle_mcu]
    serial: /tmp/klipper_toolhead
'''



def _validate_usb_id(value: str) -> Tuple[str, str]:
    """Parse and validate a VID:PID string, return (vid, pid) in lowercase."""
    parts = value.lower().split(':')
    if len(parts) != 2:
        raise ConfigError(f'USB ID must be VID:PID, got: {value!r}')
    for part, name in zip(parts, ('VID', 'PID')):
        if not part or not all(c in '0123456789abcdef' for c in part):
            raise ConfigError(
                f'{name} in {value!r} must be a hex string (e.g. 1d6b:0104)'
            )
    return parts[0], parts[1]


def main() -> None:
    ap = argparse.ArgumentParser(
        prog='serialmux.py',
        description=DESCRIPTION,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument(
        'mode',
        choices=['exporter', 'host'],
        help='exporter: runs on the MCU machine | host: runs on the Klipper machine',
    )
    ap.add_argument(
        'link_dev',
        nargs='?',
        default=None,
        help=(
            'Explicit CDC gadget device path (e.g. /dev/ttyGS0). '
            'Use this on the host side. '
            'Mutually exclusive with --usb.'
        ),
    )
    ap.add_argument(
        '--usb',
        metavar='VID:PID',
        default=None,
        help=(
            'Discover the CDC ACM device by USB ID (e.g. 1d6b:0104). '
            'The daemon waits for the device to appear and rediscovers it '
            'after each disconnect. Recommended for exporter side. '
            'Mutually exclusive with link_dev.'
        ),
    )
    ap.add_argument(
        'channels',
        nargs='+',
        metavar='channel_spec',
        help=(
            'One or more channel specs. See description above for full format.\n'
            '  mcu:<id>:<device>:<baud>    (exporter MCU channel)\n'
            '  mcu:<id>:<pty_symlink>      (host MCU channel)\n'
            '  tcp:<id>:<addr>:<port>      (TCP tunnel channel)'
        ),
    )
    args = ap.parse_args()

    # Exactly one of link_dev or --usb must be given
    if args.link_dev and args.usb:
        ap.error('link_dev and --usb are mutually exclusive')
    if not args.link_dev and not args.usb:
        ap.error('one of link_dev or --usb is required')

    usb_id: Optional[Tuple[str, str]] = None
    if args.usb:
        try:
            usb_id = _validate_usb_id(args.usb)
        except ConfigError as e:
            ap.error(str(e))

    try:
        _validate_channel_specs(args.mode, args.channels)
    except ConfigError as e:
        ap.error(str(e))

    sel = selectors.DefaultSelector()

    daemon_ref: List[Optional[Daemon]] = [None]

    def send(ftype: int, channel: int, payload: bytes) -> None:
        if daemon_ref[0] is not None:
            daemon_ref[0].send(ftype, channel, payload)

    # If using USB discovery, block here until the device appears once.
    # After this point _resolve_link_dev is always non-blocking, so a
    # USB disconnect mid-run causes a scheduled retry rather than freezing
    # the main loop.
    if usb_id is not None:
        vid, pid = usb_id
        dev = find_acm_by_usb_id(vid, pid)
        if dev is None:
            wait_for_acm(vid, pid)
            _log(f'USB {vid}:{pid} -- found, starting daemon')

    channels = build_channels(args.mode, args.channels, send, sel)
    daemon   = Daemon(args.mode, args.link_dev, sel, channels, usb_id=usb_id)
    daemon_ref[0] = daemon

    try:
        daemon.run()
    except KeyboardInterrupt:
        _log('Shutting down')


if __name__ == '__main__':
    main()
