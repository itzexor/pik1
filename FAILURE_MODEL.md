# Failure Model

PiK1 is a transport bridge, not a printer safety controller. Thermal, motion,
heater, watchdog, and emergency-stop safety remain the responsibility of
Klipper, MCU firmware, printer configuration, and physical protections.

This document defines the failure contract for the current architecture: one
USB bulk connection, one sequenced link, and three logical services carried by
that link.

## Core policy

When transport or protocol state is ambiguous, fail the shared session,
discard its queued state, and reconnect from a new generation.

PiK1 must:

- preserve payload bytes and ordering;
- reject malformed or integrity-failed frames;
- bound all queues, histories, retries, and timeouts;
- never replay bytes across a session boundary;
- never keep a logical service alive after the underlying link fails;
- expose communication loss to Klipper and local clients;
- avoid binding the TCP listener more broadly than configured.

PiK1 does not validate G-code, reconstruct application messages, decide
whether a print should continue, or provide a safety interlock.

## Shared-link lifecycle

The Pi FunctionFS backend and the K1 usbfs backend are opposite ends of one
byte stream. A transport open creates a fresh local link session and sends a
HELLO on control channel 0. Serial and tunnel endpoints do not start until a
compatible HELLO is received.

Any of these conditions fails the entire session:

- USB disconnect, endpoint disable, unrecoverable URB/AIO error, or impossible
  completion length;
- receive-buffer overflow or a frame that cannot be recovered within the
  retransmit budget;
- session ID change after synchronization;
- sequence data outside the retained history window;
- invalid type/channel combinations or malformed service frames;
- overflow of a reliable link, service, serial, or active-connection queue;
- liveness timeout.

Teardown closes or cancels transport work, closes PTYs and TCP connections,
clears all service queues and retransmit history, and resets control state.
Reconnect uses a new session ID and bounded backoff.

No bytes, close events, stream generations, or ACKs from the old session may
affect the new one.

## Framing and retransmission

Each sequenced frame contains type, channel, session ID, sequence number,
payload, and CRC32, then uses COBS framing with a zero delimiter.

The receiver delivers frames exactly once and in order. A damaged frame or
sequence gap starts a bounded recovery window:

1. The receiver drops data at and beyond the gap and sends a NAK for the next
   expected sequence.
2. The sender retransmits byte-identical encoded frames from fixed-size
   history.
3. Duplicates already delivered are discarded by sequence number.
4. If the gap is not healed in time, or the requested frame has aged out, the
   session fails.

NAKs are link-control frames outside the sequenced service stream. They carry
the session being repaired and are ignored unless that session matches the
sender's live transmit session.

For a short, bounded interval before the first valid HELLO, stale bytes from a
previous USB generation may be discarded. After that grace period, invalid
bring-up traffic fails the session.

## Scheduling and backpressure

Outbound service frames enter bounded queues with strict priority:

1. control;
2. serial mux;
3. TCP tunnel.

The scheduler admits only a shallow amount to the encoded link ring. Each USB
transport keeps only one transmit operation in flight. This bounds the amount
of tunnel data that can sit ahead of a new MCU or control frame.

Queue overflow is never treated as successful delivery. Reliable overflow
fails the session. Producers are paused at high-water marks and resumed below
low-water marks so overflow should occur only when flow-control assumptions
have already failed.

USB completion handling consumes only the number of bytes the kernel reports
as completed. A valid short write leaves the remainder at the head of the link
ring for the next operation. Zero, negative, or over-length successful
completion results are treated as transport failures.

The event loop also checks USB completion queues on a short deadline so
progress does not depend solely on an epoll notification.

## Serial service

MCU UART input is not forwarded until the expected Klipper framing marker has
been observed. MCU silence returns the channel to its resynchronizing state
and tells the PTY side to flush its endpoint. A UART error closes the local
descriptor, resets channel state, and also flushes the peer before reopening.

Data for a configured but closed MCU, a write failure, or an active-channel
buffer overflow fails the shared session.

On the Pi, a PTY may exist while Klipper has no slave reader. In that specific
state, MCU bytes may be dropped with a rate-limited log rather than buffered
indefinitely. Once a PTY reader is present, writes are reliable and bounded;
overflow fails the session. A later reader receives only new bytes.

## TCP tunnel service

The tunnel is optional. Receiving tunnel frames when it is disabled, receiving
OPEN in listener mode, invalid connection IDs, duplicate active OPENs, or
active-connection output overflow fails the session.

Each connection slot carries a nonzero generation byte. Frames for an older
closed generation are ignored; they must never close or write to a reused
slot. Fatal local TCP I/O closes that connection and sends CLOSE to the peer.

Per-connection PAUSE/RESUME protects local socket output, while class
high-water backpressure pauses all local TCP input. Closing the shared link
closes every tunnel connection without sending stale CLOSE frames.

The listener defaults to loopback. Wildcard binds are permitted only when
explicitly configured and are logged because the tunnel adds no
authentication.

## Control commands

Only one outbound command may await an ACK in a daemon. A second local command
is rejected as busy. Only one received reboot/restart/poweroff action may be
scheduled; another action is rejected and cannot overwrite the first.

The receiver sends an ACK before executing a destructive peer action, then
waits briefly so the ACK can leave the transport. ACK timeout is reported to
the initiator and does not imply that an unacknowledged action ran.

## Review checklist

For changes to transport or protocol code, verify:

- every length is validated before copy, allocation, or consume;
- partial reads and writes preserve the unprocessed remainder;
- no event tag is interpreted as the wrong component;
- all queues and retry loops have fixed bounds;
- service priority is decided before transport admission;
- link teardown clears every service and generation;
- stale frames cannot cross a new HELLO/session boundary;
- active reliable endpoints never silently drop on overflow;
- TCP binds remain explicit;
- logs identify failures without dumping payload contents.

The native suite covers framing, sequence recovery, stale-session handling,
control validation, queue behavior, fragmented local commands, serial mux
state, and tunnel stream rules. USB hardware behavior must also be exercised
on both a K1 usbfs host and a Pi FunctionFS gadget before release.
