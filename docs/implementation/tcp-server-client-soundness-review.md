# TCP Server/Client Soundness Review

Status: implementation review.

This document reviews the current TCP / Unix-domain socket architecture used by
NeoTape archive writing and reading. It is based on the implementation in:

- `include/neotape/tcp_protocol.hpp`
- `src/neotape_tcp_protocol.cpp`
- `src/neotape_tcp_server.cpp`
- `src/neotape_write_cmd.cpp`
- `src/neotape_extractor.cpp`
- `src/neotape_read_cmd.cpp`

## Executive Summary

The current architecture is fundamentally sound for the intended operator model:
a long-running archive or extraction server owns global archive state, while a
short-lived tape-side client handles exactly one mounted medium. The most
important correctness property in the write path is present: `neotape-write`
acknowledges a frame only after the local tape/spool backend reports that the
record was written. The archiver retains sent-but-not-acknowledged frames and can
redrive them after a reconnect.

The design is not yet hardened as a protocol boundary. The current code assumes
a trusted peer and well-formed traffic. The main risks are unchecked message
length allocation, loose ACK validation, ambiguous volume commit semantics after
early disconnects, and drift between `docs/spec/13-tcp-protocol.md` and the
implemented message directions.

## Implemented Roles

### Archive writing path

In the write path, the roles are:

| Process | Protocol role | Responsibility |
| --- | --- | --- |
| `neotape-archiver` | Server/listener | Generates NeoTape records from pax bytes, owns archive UUID, global frame sequence, slice sequence, and current volume sequence. |
| `neotape-write` | Client/connector | Pulls records, writes them to tape/spool, sends ACKs after successful writes, exits on EOT or archive end. |

The implemented flow is:

1. Writer connects to the archiver.
2. Writer sends `next_frame`.
3. Archiver responds with exactly one of:
   - `frame_record`
   - `tape_eof`
   - `error`
4. Writer queues `frame_record` records to its local writer thread.
5. The writer thread writes records to the tape/spool backend.
6. After a successful record write, the writer thread sends `ack_frame(seq)`.
7. On EOT, the writer stops requesting, sends an ACK for the last fully written
   frame if any, and exits non-zero.
8. A new writer process connects for the next medium.
9. When the archiver emits `ARCHIVE_END`, the writer drains queued records,
   writes the archive-end record, ACKs it, and exits zero.

This is a pull-based protocol with local client-side buffering. It is not a
strict lock-step request/write/ack loop: the writer may request more frames while
older frames are still waiting in its output queue, bounded by
`--output-buffer-size`.

### Archive reading path

In the read/extract path, the roles are:

| Process | Protocol role | Responsibility |
| --- | --- | --- |
| `neotape-extractor` | Server/listener | Requests records, validates NeoTape format, reconstructs pax payload bytes, writes extracted pax stream. |
| `neotape-read` | Client/connector | Reads physical tape/spool records and sends one record per `next_frame` request. |

The implemented flow is:

1. Reader connects to extractor.
2. Extractor sends `next_frame`.
3. Reader reads the next tape/spool record and sends `frame_record`.
4. Extractor validates frame size, hash, archive identity, global sequence,
   volume sequence, slice sequence, and channel ordering.
5. Extractor sends `ack_frame(seq)` after validation and accumulation.
6. Reader sends `tape_eof` at physical/logical EOD and exits.
7. Extractor accepts the next reader connection until it validates an
   `ARCHIVE_END` frame.

This direction is stricter than the write path because the extractor validates
the data stream before acknowledging records.

## Soundness Properties Present

### Back-pressure is natural

Both pipelines use `next_frame` as the demand signal. The producer only sends a
record in response to a request, so there is no unbounded producer-side socket
push. In the write path, `neotape-write` also enforces a local queued-byte limit
before sending another `next_frame`.

### Frame commit boundary is in the right place

The write path treats one NeoTape record as the commit unit. The writer thread
sends `ack_frame` only after `TapeDevice::write_record()` returns successfully
and post-write EOT status does not report EOT. This matches the policy in
`docs/implementation/lto-behavior-notes.md`: the archiver should advance only
past records that the tape-side process considers fully committed.

### Reconnect/redrive is supported

`src/neotape_tcp_server.cpp` keeps:

- `last_acked_global_frame`
- a retention buffer of sent records
- `next_volume_seq_num`

On reconnect, the archiver starts from `last_acked_global_frame + 1`. If a frame
was sent but not ACKed, it can be replayed from the retention buffer. If it has
not yet been generated, it is read from the frame queue.

### Volume number patching happens at send time

Generated content frames initially carry `volume_seq_num = 0`. The archiver
patches `volume_seq_num` and recomputes the frame hash immediately before
sending a record. This makes redrive across volumes possible without regenerating
pax payload bytes. A frame that was sent but not ACKed can be resent for the next
volume with a new volume sequence and a valid hash.

### Archive extraction validates the persistent format

The extractor validates more than the transport envelope. It checks decoded
record size, frame hash, archive identity consistency, gapless global frame
sequence, monotonic volume sequence, logical slice sequence, per-channel frame
ordering, and archive-end cleanliness before declaring success.

## Important Risks

### 1. Message length is not bounded in `read_message`

`docs/spec/13-tcp-protocol.md` says the current maximum payload length is
16 MiB, but `tcp::read_message()` currently trusts the 64-bit length field and
resizes a `std::vector<std::byte>` to that value.

Impact:

- A corrupt or malicious peer can force huge allocation.
- A typo or protocol desync can produce an out-of-memory failure instead of a
  clean protocol error.
- The receiver does not fail before committing memory.

Recommendation:

- Add a protocol constant such as `max_message_payload_size`.
- Reject payload lengths above the maximum before allocation.
- Pick a limit that covers `max_block_size` plus future control payloads.

### 2. Message type values are not validated at decode time

`read_message()` casts the first byte directly to `MessageType`. Unknown values
are usually rejected later by switch defaults, but the protocol layer itself does
not mark the message invalid.

Impact:

- Error handling is duplicated at every call site.
- Future switch statements can accidentally accept invalid enum values.

Recommendation:

- Validate the type byte in `read_message()`.
- Either throw a protocol error for unknown values or represent unknown values
  explicitly outside `MessageType`.

### 3. ACK validation in the archiver is too permissive

The archiver accepts any 8-byte `ack_frame` and updates:

```text
last_acked_global_frame = max(last_acked_global_frame, acked_value)
```

It does not verify that the ACK corresponds to a frame actually sent to the
current connection, that the ACK is not ahead of the highest sent frame, or that
ACKs are monotonic by exactly the expected committed sequence.

Impact:

- A buggy or malicious writer can skip archive data by ACKing a future frame.
- A corrupted ACK can advance archive state beyond retained/generated records.
- The retention buffer may discard frames that were never written.

Recommendation:

- Track `highest_sent_global_frame` per connection.
- Reject ACKs greater than `highest_sent_global_frame`.
- Reject ACKs lower than or equal to `last_acked_global_frame` unless duplicate
  ACKs are intentionally allowed.
- Prefer requiring ACKs to advance by known sent frames, not arbitrary jumps.

### 4. Volume commit currently happens on the first `next_frame`

`serve_client()` marks `volume_committed = true` when it receives the first
`next_frame`, before any record has been written or ACKed by the writer.

Impact:

- If a writer connects, requests a frame, and disconnects before a successful
  write/ACK, the archiver advances `next_volume_seq_num` even though the volume
  may be empty or unusable.
- This can create skipped or misleading volume numbers.
- It conflicts with the stricter wording in the protocol spec that volume
  sequence should be gapless.

This is not necessarily data loss because global frame resume is based on
`last_acked_global_frame`. The next writer will still request the first
uncommitted frame. The issue is archive metadata cleanliness and operator
semantics.

Recommendation:

- Define whether a volume is committed at connection start, first request, first
  successful ACK, or explicit EOT/close.
- If gapless volume numbering matters, advance `next_volume_seq_num` only after
  at least one frame ACK or after an explicit client-side volume-finalization
  signal.
- If skipped volume numbers are acceptable, document them as operator-visible
  abandoned volume attempts.

### 5. ACK-after-write cannot distinguish ACK loss from media loss

If a writer writes a frame successfully but the ACK is lost before the archiver
receives it, the archiver will redrive the same global frame on the next volume.

Impact:

- This is conservative for recovery because it avoids skipping unconfirmed data.
- It can leave duplicate frame records on an earlier physical medium.
- A later reader/extractor must rely on gapless global frame validation and
  operator volume order to avoid accepting duplicates.

Recommendation:

- Document the current semantics as at-least-once delivery across connection
  failure.
- For future stronger semantics, add a volume close/commit handshake or a
  recoverable per-volume manifest.

### 6. Protocol spec drift

`include/neotape/tcp_protocol.hpp` documents the implemented directions:

- Archive write path: writer sends `next_frame`, archiver sends `frame_record`
  or `tape_eof`, writer sends `ack_frame`.
- Extract path: extractor sends `next_frame`, reader sends `frame_record`,
  extractor sends `ack_frame`.

`docs/spec/13-tcp-protocol.md` currently has a "Protocol flow" section that
describes the writing pipeline in the opposite direction. It also states a
16 MiB payload limit that is not enforced in code.

Impact:

- New code can implement the wrong side of the protocol.
- Tests may be written against stale behavior.

Recommendation:

- Update `docs/spec/13-tcp-protocol.md` to match the implemented message
  directions.
- Either enforce the 16 MiB limit or remove the normative claim until enforced.
- Consider adding simple state-machine tables for both archive and extract
  pipelines.

### 7. Error observability is thin on the archiver side

`serve_client()` catches all exceptions and returns an incomplete
`ServeResult` without logging the exception text. This keeps the archiver alive
for reconnects, but it can hide root causes such as `EPIPE`, malformed messages,
or failed writes to the peer.

Impact:

- Operator diagnostics are weak.
- Repeated failing clients may look like normal reconnect churn.

Recommendation:

- Log exception text at debug or warning level.
- Distinguish clean disconnect, protocol error, and transport error in
  `ServeResult`.

### 8. Transport address handling is minimal

`parse_address()` splits TCP addresses with `rfind(':')`. That is enough for
`tcp://host:port` and many simple cases, but not a complete URI parser.

Impact:

- IPv6 literals are ambiguous unless constrained to a supported form.
- Empty host, empty port, and unusual Unix socket paths receive limited
  validation.

Recommendation:

- Document supported address syntax precisely.
- Add tests for empty host/port, Unix path length, and IPv6 behavior.

### 9. Security model remains trusted-peer only

The protocol has no authentication, authorization, encryption, replay defense, or
peer identity. TCP listeners can bind to non-loopback addresses.

Impact:

- A network peer can request archive data, send malformed messages, or spoof
  ACKs if it can reach the listener.
- This is especially concerning with the loose ACK validation and unbounded
  message allocation described above.

Recommendation:

- Treat TCP mode as trusted-network only until authentication exists.
- Prefer Unix-domain sockets or loopback bindings for local deployments.
- Add explicit security notes to CLI docs and `docs/spec/08-security.md`.

## Data Transfer Semantics

### Current write-path guarantee

The implemented write path provides this practical guarantee under trusted peers:

```text
The archiver will not intentionally advance past a frame until a writer has ACKed
that global frame sequence. The writer sends that ACK only after its local
backend reports a successful complete record write.
```

This is the right high-level shape for LTO-style media.

The guarantee is conditional:

- It assumes ACK payloads are honest and uncorrupted.
- It assumes `TapeDevice::write_record()` correctly reports short writes and
  deferred write failures.
- It does not provide exactly-once media placement across connection loss.
- It does not persist archiver state across archiver process restarts.

### Current read-path guarantee

The read/extract path is stricter:

```text
The extractor only ACKs a frame after it validates the NeoTape record and
successfully accumulates or flushes the payload state associated with it.
```

This makes the extractor a good reference for the validation rules the write
path may eventually want to apply defensively before writing to media.

## Recommended Fix Order

1. Enforce max payload length and message type validation in
   `tcp::read_message()`.
2. Tighten archiver ACK validation with per-connection sent-frame tracking.
3. Resolve volume commit semantics and update implementation/spec together.
4. Update `docs/spec/13-tcp-protocol.md` to match the implemented protocol.
5. Add negative protocol tests:
   - oversized message length
   - unknown message type
   - short ACK payload
   - future ACK
   - duplicate/stale ACK
   - reconnect after unacked sent frame
6. Improve archiver logging for disconnect and protocol-error cases.
7. Document trusted-peer security assumptions for TCP listeners.

## Verdict

The current architecture is sound as a trusted, single-producer/single-client
operator workflow. Its core recovery idea is good: use pull-based transfer,
acknowledge only after media write or validation, and redrive the first
unacknowledged global frame after reconnect.

It should not yet be treated as a hardened network protocol. Before exposing TCP
listeners beyond a trusted host/network, NeoTape should enforce message bounds,
validate enum values, reject impossible ACKs, and make the implemented state
machine the single source of truth in the spec.
