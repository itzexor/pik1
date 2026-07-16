# Failure Model

This document defines the expected failure behavior for PiK1's shared link,
logical services, and transports. It is a design contract for implementation
and code review, not a user-facing safety certification.

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
- retry or reconstruct traffic above the link layer (link-layer
  retransmission is a documented exception);
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
- replaying traffic from a previous session or link generation;
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
4. PiK1 never accepts frames with invalid integrity checks.
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

## Documented Exceptions

### Link-layer retransmission

Motivation: a byte transport can lose, delay, duplicate, or tear traffic without
providing a clean session boundary. Bounded link-layer retransmission is allowed
only to heal loss inside a live, synchronized session. It must not become
application-level retry, command replay, or cross-session recovery.

Mechanism: a receiver that detects a missing or damaged frame may request
retransmission from the next expected sequence point. It must deliver nothing
past the gap until the gap is healed, and it must drop damaged or out-of-order
frames rather than exposing them to services. The sender may retransmit only
byte-identical stored frames from bounded history. If the gap cannot be healed
within a bounded window, or if the requested data is no longer in history, the
link fails closed.

Retransmission is valid only after both peers are synchronized to the same
session. A fresh session must discard stale in-flight traffic from the previous
generation until synchronization completes. Once a session change is observed,
old session state and retransmit history are invalid.

Why the invariants hold:

- Delivery stays exactly-once and in-order; payloads are never generated,
  modified, or reordered by PiK1 (invariants 1, 2). Duplicates arising from
  retransmission overlap are detected by sequence number and discarded.
- No replay across session boundaries: retransmit history lives and dies with
  the link, and session change / reconnect behavior is unchanged
  (invariants 3, 7, 13).
- Bounded memory: history rings are fixed-size; frames age out
  (invariants 5, 6).
- Bounded time, then fail closed: an unhealed gap fails the link after a
  bounded window, and a retransmission request for data that has aged out of
  history fails the link immediately (invariants 8, 11).
- Corrupted input is never delivered: a frame that fails integrity checks is
  dropped without being parsed further (invariant 4). Post-sync it opens the
  same bounded heal-or-fail gap window, so a persistently garbled link still
  fails closed; pre-sync it is discarded within the bring-up grace window and
  fails the link after it.
- Nothing is hidden: every gap, retransmit, heal (with latency and discard
  counts), and budget failure is logged (invariants 9, 10).

Related: a short bring-up grace window may discard and count stale traffic that
cannot belong to the new synchronized stream. After that bounded window, stale
or malformed traffic fails closed normally.

Expected tests: gap detection, bounded heal, budget expiry, history-window
expiry, duplicate discard, byte-identical retransmission, stale bring-up
traffic, grace expiry, and corrupt-frame recovery/failure.

### Shared-link TX Scheduling

One link carries control, MCU, and tunnel traffic, so a tunnel burst could
in principle delay MCU frames (see "delay emergency stop traffic behind
stale buffered data"). To bound that, outbound frames should pass through
bounded per-service queues drained by explicit priority into a shallow wire
queue. Bulk or tunnel traffic must not be able to occupy enough admitted wire
queue to starve fresh control or MCU traffic.

Worst-case head-of-line delay for high-priority traffic must be bounded by a
small amount of already-admitted data and the current transport write.
Service-queue overflow fails closed instead of silently dropping reliable
traffic. Backpressure should keep queues below their limits in normal
operation.

Expected tests: priority traffic overtakes queued bulk traffic, queue limits
are enforced, overflow is visible, and backpressure prevents unbounded growth.

### Transport Completion Progress

Event-driven transports should not rely solely on the event path for progress.
If a transport can have completed work that is not immediately surfaced to the
main loop, it needs a bounded progress deadline that drains or checks the
transport anyway. This is a transport progress mechanism, not a protocol retry
and not a printer liveness signal.

Why the invariants hold:

- Completion drains do not generate, reorder, or modify payloads; they only
  move already-completed transport bytes into or out of the shared link
  (invariants 1, 2).
- The shared link still owns integrity, ordering, session identity, and
  retransmission; the transport interval cannot replay data across reconnect
  because transport cleanup tears down the active link (invariants 3, 7, 13).
- The progress deadline is bounded and visible in the daemon's deadline
  calculation, so transport progress is not dependent on an unbounded event wait
  (invariant 11).
- Completion errors still fail the link; the progress check does not spoof
  keepalives or hide a dead endpoint (invariants 9, 10).

Expected tests: normal event-driven progress, completed work discovered by the
bounded progress path, transport error handling, and no payload replay across
transport restart.

### Absent Optional Local Endpoints

Some logical streams terminate at local endpoints that may not currently have an
application attached. If the endpoint is explicitly absent or unopened, PiK1 may
discard data for that logical stream after logging instead of failing the whole
shared link. This exception is endpoint absence, not backpressure, and must not
apply to printer-control or MCU traffic.

Why the invariants hold:

- The data is not delivered, transformed, replayed, or claimed as accepted by an
  active reliable endpoint (invariants 1, 2, 3).
- The discard is scoped to the affected optional stream and logged, so it does
  not hide shared-link or MCU failure (invariants 9, 10).
- Once an endpoint is active, normal reliable-stream behavior applies again:
  bounded buffering, explicit backpressure, or fail-closed overflow
  (invariants 5, 6, 8).

Expected tests: absent endpoint discard is logged and scoped, the shared link
remains usable, later endpoint attachment receives only new data, and the same
discard path is rejected for printer-control or MCU traffic.

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

Invalid framing, invalid length, bad magic, bad integrity check, malformed
packet, unknown type, impossible command, or impossible state should fail
closed.

PiK1 should:

- reject the frame;
- log the failure;
- close or reset the affected connection/session;
- only resynchronize if the behavior is simple, bounded, and tested.

Suspect behavior:

- accepting frames with bad integrity checks;
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
- framing or integrity failure;
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
- Does each event-driven transport also have a bounded progress mechanism?
- Are frame lengths validated before allocation?
- Are maximum frame and payload sizes enforced?
- What happens on integrity failure?
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
- Completion event delivered normally.
- Completed transport work discovered by the bounded progress path.
- TCP disconnect mid-frame.
- Serial disconnect mid-frame.
- Invalid integrity check.
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
