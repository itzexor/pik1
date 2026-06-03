#!/usr/bin/env python3
import os
import pty
import selectors
import socket
import subprocess
import sys
import time


TCPBRIDGE = os.environ.get("TCPBRIDGE", "build/tcpbridge")


def fail(msg):
    print(f"test_tcpbridge_integration: {msg}", file=sys.stderr)
    sys.exit(1)


def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def connect_retry(port, timeout=3.0):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=0.2)
        except OSError as e:
            last = e
            time.sleep(0.02)
    raise last


class PtyRelay:
    def __init__(self):
        self.m1, self.s1 = pty.openpty()
        self.m2, self.s2 = pty.openpty()
        self.path1 = os.ttyname(self.s1)
        self.path2 = os.ttyname(self.s2)
        os.set_blocking(self.m1, False)
        os.set_blocking(self.m2, False)
        self.sel = selectors.DefaultSelector()
        self.sel.register(self.m1, selectors.EVENT_READ, self.m2)
        self.sel.register(self.m2, selectors.EVENT_READ, self.m1)
        self.running = True

    def pump_until(self, deadline):
        while self.running and time.monotonic() < deadline:
            for key, _ in self.sel.select(0.02):
                src = key.fd
                dst = key.data
                try:
                    data = os.read(src, 4096)
                except BlockingIOError:
                    continue
                except OSError:
                    self.running = False
                    return
                if not data:
                    self.running = False
                    return
                os.write(dst, data)

    def close(self):
        self.running = False
        for fd in (self.m1, self.s1, self.m2, self.s2):
            try:
                os.close(fd)
            except OSError:
                pass


def terminate(proc):
    if proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=1)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=1)


def test_data_roundtrip():
    relay = PtyRelay()
    target_port = free_port()
    listen_port = free_port()
    target = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    target.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    target.bind(("127.0.0.1", target_port))
    target.listen(1)
    target.setblocking(False)

    procs = [
        subprocess.Popen(
            [TCPBRIDGE, relay.path1, "listen", f"127.0.0.1:{listen_port}"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
        ),
        subprocess.Popen(
            [TCPBRIDGE, relay.path2, "forward", f"127.0.0.1:{target_port}"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
        ),
    ]

    client = accepted = None
    try:
        client = connect_retry(listen_port)
        client.settimeout(1)
        client.sendall(b"hello-target")

        deadline = time.monotonic() + 3
        while time.monotonic() < deadline and accepted is None:
            relay.pump_until(time.monotonic() + 0.03)
            try:
                accepted, _ = target.accept()
            except BlockingIOError:
                pass
        if accepted is None:
            fail("target did not receive forwarded connection")
        accepted.settimeout(1)

        deadline = time.monotonic() + 3
        data = b""
        while time.monotonic() < deadline and len(data) < len(b"hello-target"):
            relay.pump_until(time.monotonic() + 0.03)
            try:
                data += accepted.recv(64)
            except socket.timeout:
                pass
        if data != b"hello-target":
            fail(f"target received {data!r}")

        accepted.sendall(b"hello-client")
        deadline = time.monotonic() + 3
        data = b""
        while time.monotonic() < deadline and len(data) < len(b"hello-client"):
            relay.pump_until(time.monotonic() + 0.03)
            try:
                data += client.recv(64)
            except socket.timeout:
                pass
        if data != b"hello-client":
            fail(f"client received {data!r}")
    finally:
        if client:
            client.close()
        if accepted:
            accepted.close()
        target.close()
        for p in procs:
            terminate(p)
        relay.close()


def test_wildcard_warning():
    port = free_port()
    proc = subprocess.Popen(
        [TCPBRIDGE, "/tmp/pik1-no-such-tty", "listen", f"0.0.0.0:{port}"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        deadline = time.monotonic() + 2
        stderr = ""
        while time.monotonic() < deadline:
            time.sleep(0.05)
            if proc.stderr:
                chunk = proc.stderr.read(0)
                stderr += chunk
            if proc.poll() is not None:
                break
            # Give the process enough time to bind and log before termination.
            if time.monotonic() + 1 < deadline:
                continue
            break
    finally:
        terminate(proc)
    if proc.stderr:
        try:
            stderr += proc.stderr.read()
        except Exception:
            pass
    if "unauthenticated TCP tunnel is exposed on all interfaces" not in stderr:
        fail("wildcard bind warning was not logged")


def main():
    test_data_roundtrip()
    test_wildcard_warning()
    print("test_tcpbridge_integration: ok")


if __name__ == "__main__":
    main()
