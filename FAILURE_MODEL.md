# Failure Model

This document defines the expected failure behavior for PiK1's serial, control,
and TCP bridge transports. It is a design contract for implementation and code
review, not a user-facing safety certification.

PiK1 is transport plumbing between host/UI/network components and the existing
printer control stack. It is not a safety-critical controller and must not be
relied on for thermal safety, motion safety, heater limits, emergency stop
behavior, watchdog behavior, or printer state validation.

Machine safety is expected to remain in Klipper, Klipper MCU firmware, printer
configuration limits, MCU watchdog behavior, heater safety checks, and physical
or electrical protections. PiK1 must not mask failures in ways that prevent
those layers, or the operator, from noticing unreliable communication.

When state is ambiguous, PiK1 should fail closed, loud, bounded, and recoverable.

## Non-Goals

PiK1 does not:

- validate G-code semantics;
- enforce printer kinematics or heater safety;
- decide whether motion, heating, or a print should continue;
- retry printer-control traffic for correctness;
- reconstruct lost traffic;
- preserve sessions across ambiguous reconnects;
- provide a safety interlock.

Any behavior that appears to do one of these things is suspect unless it is
explicitly designed, reviewed, and tested.

## Failure Policy

Prefer:

- close the affected session;
- drop the affected logical stream;
- discard stale queued data;
- force a clean reconnect;
- reject malformed data;
- log the failure;
- bound memory growth;
- time out blocked operations.

Avoid:

- silent recovery from impossible states;
- indefinite buffering;
- replaying old traffic;
- guessing stream ownership;
- preserving stale sessions;
- keeping a connection superficially alive while useful traffic is broken;
- hiding repeated failures behind automatic retries;
- continuing after protocol desynchronization.

## Required Invariants

These invariants should hold unless a narrow exception is documented and tested:

1. PiK1 never generates printer-control commands on its own.
2. PiK1 never semantically modifies printer-control payloads.
3. PiK1 never replays printer-control payloads after reconnect.
4. PiK1 never accepts frames with invalid CRC or integrity checks.
5. PiK1 never allocates unbounded memory from untrusted input.
6. PiK1 never allows one dead stream to cause unbounded global buffering.
7. PiK1 never silently preserves stale session state across reconnect.
8. PiK1 fails closed on impossible parser state.
9. PiK1 logs protocol-level failures.
10. PiK1 does not mask Klipper/MCU communication failure.
11. PiK1 has bounded reconnect behavior.
12. PiK1 has explicit behavior for unknown, duplicate, stale, and closed stream IDs.
13. PiK1 treats disconnect/reconnect as a new generation.
14. PiK1 does not bind network services more broadly than intended.
15. PiK1 is not required for printer thermal or motion safety.

## Failure Classes

### Disconnects

Expected causes include USB disconnect, serial reset, TCP disconnect, process
restart, firmware restart, cable instability, and power instability.

On disconnect, PiK1 should:

- close affected file descriptors or sockets;
- mark associated logical streams dead;
- discard pending buffered data for dead streams;
- log the disconnect reason if known;
- avoid reusing stale stream/session state;
- allow clean reconnect.

Suspect behavior:

- continuing to write to a dead endpoint;
- preserving old stream state across reconnect;
- replaying buffered traffic after reconnect;
- treating reconnect as the same session without a generation marker;
- silently dropping traffic while presenting the connection as healthy.

### Partial Reads And Writes

Partial reads and writes are normal transport behavior.

PiK1 should:

- assemble full frames across short reads;
- handle short writes explicitly;
- preserve unwritten suffix bytes;
- maintain bounded per-stream output buffers where needed;
- close or time out persistent inability to write.

Suspect behavior:

- assuming one read equals one frame;
- assuming one write sends all bytes;
- dropping unwritten suffix bytes;
- mixing bytes between logical streams;
- allowing output queues to grow without limit;
- blocking the whole bridge forever on one slow endpoint.

### Framing And Integrity

Invalid framing, invalid length, bad magic, bad CRC, malformed packet, unknown
type, impossible command, or impossible state should fail closed.

PiK1 should:

- reject the frame;
- log the failure;
- close or reset the affected connection/session;
- only resynchronize if the behavior is simple, bounded, and tested.

Suspect behavior:

- accepting frames with bad CRC;
- ignoring invalid lengths;
- scanning indefinitely for magic bytes;
- continuing after impossible parser state;
- allowing malformed packets to allocate large buffers.

### Length And Resource Bounds

All untrusted sizes must be validated before allocation or buffering.

PiK1 should enforce:

- maximum frame size;
- maximum payload size;
- maximum stream count;
- maximum per-stream queue size;
- maximum total buffered bytes;
- valid command/type ranges;
- valid stream/session ID ranges;
- bounded reconnect rate.

Suspect behavior:

- allocating directly from an untrusted length field;
- accepting oversized frames;
- integer overflow in length calculations;
- reading indefinitely for an impossible frame size;
- unlimited clients, streams, buffers, or reconnect loops.

### Backpressure

Backpressure must be bounded and visible.

PiK1 should:

- apply per-stream and global queue limits;
- pause or close streams when limits are reached;
- fail closed before dropping reliable traffic;
- discard stale queued data on reconnect;
- log queue overflow or timeout events.

Suspect behavior:

- infinite queues;
- one blocked stream stalling unrelated streams indefinitely;
- silent drops;
- stale buffered data taking priority over current session state;
- retrying forever without surfacing failure.

### Reconnects And Replay

Reconnect creates a new logical generation.

PiK1 should:

- invalidate stale streams;
- discard queued data from the old generation;
- require fresh open/handshake behavior;
- prevent old close/data events from affecting new sessions;
- avoid automatic command replay.

Suspect behavior:

- stream IDs surviving reconnect as if unchanged;
- old buffered writes delivered after reconnect;
- old close events killing new streams;
- duplicate open/close confusion;
- reconnect loops with no rate limit.

### Stream And Session Confusion

Each logical stream needs clear ownership and lifecycle.

Behavior must be explicit for:

- open stream;
- data on open stream;
- close stream;
- data on closed stream;
- duplicate open;
- duplicate close;
- unknown stream ID;
- stream ID reuse;
- reconnect while streams are open.

Suspect behavior:

- accepting data for unknown streams;
- silently creating streams from data packets;
- reusing stream IDs before old state is cleared;
- cross-stream data contamination.

### Ordering And Concurrency

PiK1 may rely on TCP byte ordering within one TCP connection and serial byte
ordering within one serial stream. It must not assume ordering between
independent callbacks, signal paths, queues, processes, or endpoints unless that
ordering is explicitly synchronized.

Audit targets:

- stream tables;
- fd/socket lifecycle;
- reconnect state;
- output queues;
- parser state;
- shutdown flags;
- statistics/log counters.

Suspect behavior:

- use-after-close of file descriptors;
- write after session close;
- stream table mutation during iteration;
- close/write/reconnect races;
- locks held during blocking I/O;
- shutdown deadlock.

### Observability

PiK1 should log enough context to diagnose failures without flooding normal
operation.

Important events:

- startup configuration;
- endpoint open/close;
- client connect/disconnect;
- stream open/close;
- reconnect;
- CRC/framing failure;
- invalid command/type;
- queue overflow;
- read/write timeout;
- unexpected EOF;
- forced session reset;
- version/protocol mismatch.

Suspect behavior:

- silent reconnects;
- silent drops;
- silent parser resets;
- no distinction between normal close and error close;
- logs so noisy that real failures are ignored.

### TCP Exposure

If a TCP endpoint is exposed beyond localhost or a trusted network, PiK1 becomes
a security boundary and needs stricter review.

Minimum expectations:

- bind only to intended interfaces;
- warn on wildcard binds;
- document whether the port is localhost-only or network-exposed;
- avoid accepting arbitrary network clients unless intended;
- avoid shell execution from network input;
- avoid logging secrets or tokens.

Suspect behavior:

- binding to `0.0.0.0` unintentionally;
- unauthenticated remote control exposure;
- exposing printer-control or Moonraker-equivalent traffic unintentionally;
- network input triggering shell commands, file writes, or process execution.

### Klipper And Printer Safety Interaction

PiK1 must not interfere with Klipper's ability to detect communication failure.

PiK1 should not:

- spoof keepalives;
- hide MCU disconnects;
- keep a dead control path appearing alive;
- retry printer-control traffic in a way that changes command timing;
- delay emergency stop traffic behind stale buffered data;
- prevent host shutdown behavior from triggering.

Suspect behavior:

- PiK1 remains "healthy" while downstream traffic is dead;
- buffering delays critical stop/shutdown commands;
- reconnect causes Klipper to misinterpret session state;
- watchdog disconnect behavior is masked;
- old traffic is delivered after Klipper has already changed state.

### Operator Interface Failure

If PiK1 carries UI/control traffic, UI failure should be obvious. The operator
should not be misled into believing:

- a command succeeded when it did not;
- the printer is idle when it is not;
- a connection is healthy when traffic is stalled;
- a print was stopped when the stop command was not delivered.

Suspect behavior:

- stale status presented as live status;
- bridge-generated ACKs implying downstream success;
- UI reconnect without state refresh;
- cached printer state presented as authoritative.

## Code Review Checklist

### Transport And Framing

- Does the parser handle partial reads?
- Does the writer handle partial writes?
- Are frame lengths validated before allocation?
- Are maximum frame and payload sizes enforced?
- What happens on CRC failure?
- What happens on invalid magic/header?
- What happens on unknown command/type?
- Can the parser enter an impossible state and continue?
- Is resynchronization bounded and simple?

### Sessions And Streams

- How are stream IDs assigned and reused?
- What happens to open streams on reconnect?
- Is there a session/generation ID?
- Can stale close/data events affect new streams?
- What happens if data arrives for a closed stream?
- What happens if data arrives for an unknown stream?
- What happens on duplicate open or duplicate close?
- Can traffic from one stream leak into another?

### Buffering And Backpressure

- Are output buffers bounded?
- Are input buffers bounded?
- Is there a global memory limit?
- What happens if serial output blocks?
- What happens if TCP output blocks?
- Can one slow client stall all streams?
- Are stale queued payloads discarded on reconnect?
- Are queue overflows logged?
- Is old data ever delivered after a long stall?

### Reconnect And Recovery

- Does reconnect discard old state?
- Is reconnect rate-limited?
- Are repeated failures visible in logs?
- Does PiK1 ever replay traffic after reconnect?
- Can two instances run at the same time?
- Can stale fds/sockets survive restart?
- Is recovery simple enough to reason about?

### Klipper / Printer Safety

- Can PiK1 prevent Klipper from noticing a real disconnect?
- Can PiK1 keep a connection appearing alive while traffic is broken?
- Can emergency stop or shutdown messages be delayed behind stale traffic?
- Does PiK1 generate ACKs that imply downstream success?
- Are Klipper timeout/watchdog mechanisms left independent?
- Does PiK1 require disabled Klipper safety features to work?

### Security

- Which interfaces does the TCP listener bind to?
- Is it localhost-only or network-exposed?
- Is authentication needed?
- Does it expose printer-control traffic to untrusted clients?
- Are secrets/tokens logged?
- Can network input trigger shell commands, file writes, or process execution?

## Suggested Tests

### Basic Transport

- Valid frame split across many reads.
- Multiple valid frames in one read.
- Short writes.
- TCP disconnect mid-frame.
- Serial disconnect mid-frame.
- Invalid CRC.
- Invalid length.
- Oversized frame.
- Unknown command/type.
- Data for unknown stream.
- Data for closed stream.
- Duplicate open/close.

### Backpressure

- Stall serial output while TCP data arrives.
- Stall TCP output while serial data arrives.
- Send faster than output can drain.
- Verify queue limits trigger.
- Verify failure is logged.
- Verify memory remains bounded.

### Reconnect

- Reconnect while streams are open.
- Reconnect while data is queued.
- Reconnect after partial frame.
- Reconnect rapidly in a loop.
- Verify old stream state is discarded.
- Verify old queued traffic is not replayed.
- Verify old close/data cannot affect the new session.

### Process

- Kill PiK1 during active traffic.
- Restart PiK1 during active traffic.
- Start two bridge instances.
- Restart downstream service while PiK1 remains alive.
- Restart upstream client while downstream remains alive.

### Printer-Specific

These should be run cautiously and preferably without active heaters or motion
unless the test specifically requires them.

- Confirm Klipper detects downstream disconnect.
- Confirm PiK1 failure does not disable Klipper safety features.
- Confirm emergency stop path is not delayed by stale queued traffic.
- Confirm UI reconnect refreshes real state.
- Confirm failed PiK1 does not keep printer-control path falsely healthy.
