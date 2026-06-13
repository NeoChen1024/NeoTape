# NeoTape TCP Archive Generation Design

Date: 2026-06-13  
Status: design / pending implementation  
Related specs: `docs/spec/*.md`, especially `docs/spec/01-volume-header.md`,
`docs/spec/02-frame-header.md`, `docs/spec/03-archive-end-header.md`,
`docs/spec/06-frames-and-slices.md`.

## 1. Problem Statement

NeoTape needs a CLI workflow that separates long-running archive generation from
tape writing. A pax producer can take hours to scan a filesystem and serialize
members, while a tape writer must run on a machine physically attached to an LTO
drive. We want a clean protocol that lets these two roles run on different hosts
or on the same host without tying their lifecycles together.

## 2. Design Decision

Use a **single TCP or Unix-domain socket connection** with a binary framed
request-response protocol. There is no separate control channel. The archiver is
long-running and owns the complete archive state; the writer is short-lived and
writes exactly one tape volume per process.

This is the simplest design that satisfies:

- Remote operation over TCP or local operation over UDS.
- Natural back-pressure via request-response (no sliding window or ACK logic).
- Simple resume across tape changes: the producer keeps running and a new writer
  reconnects to continue from the last unwritten frame.
- Minimal protocol surface: seven message types.

## 3. Roles

### Archiver (`neotape-archiver`)

- Long-running process.
- A functional superset of `mt-pax`: it supports all `mt-pax` flags and
  behaviors (multi-threaded traversal, xattr preservation, hardlink resolution,
  large-file streaming, etc.) and can also output NeoTape-framed records over the
  wire protocol described here.
- Reads source paths, optionally a slice plan produced by `neotape-plan`, and
  emits fully-formed NeoTape records.
- Remembers archive identity (`archive_uuid`, `archive_name`, etc.), current
  `volume_seq_num`, and the next frame/slice index to emit.
- Continues until the entire archive has been consumed and an Archive End Header
  has been sent, then exits cleanly.
- Does not know or care how many physical volumes the archive spans.
- Uses only a modest internal buffer. Because the writer requests frames one at a
  time, the archiver does not need a large output buffer; its main concern is
  keeping the serializer pipeline fed.

### Writer (`neotape-write`)

- Short-lived process, one per tape volume.
- Connects to an archiver over TCP or a Unix-domain socket.
- Requests the current Volume Header, then requests frames one at a time.
- Writes each received record to the current volume as-is.
- Writes a tape filemark when the archiver sends `TAPE_EOF`.
- Exits when:
  - it receives `ARCHIVE_END_HEADER` for the archive, or
  - it reaches the physical end-of-tape (EOT) of the volume it is writing.
- A new writer process can connect to the same archiver to continue the next
  volume.
- Uses a large internal output buffer (similar to `mt-pax`'s
  `--output-buffer-size`) because it must feed a sequential tape device
  efficiently. The request-response protocol naturally back-pressures the
  archiver so the writer's buffer can stay full without overflowing the
  archiver.

## 4. Wire Protocol

### 4.1 Transport

- A single byte stream over either:
  - `tcp://<host>:<port>`, or
  - `unix://<path>` (Unix-domain socket).
- The archiver listens; the writer connects.
- All integers are little-endian.

### 4.2 Message Framing

```text
+--------+--------+------------------+
| type   | length | payload          |
| uint8  | uint64 | length bytes     |
+--------+--------+------------------+
```

- `type`: message type identifier.
- `length`: payload length in bytes (may be zero).
- `payload`: message-specific data.

A message is exactly `9 + length` bytes. There is no streaming of a single
message across multiple frames; the receiver must read `length` bytes before
interpreting the payload.

### 4.3 Message Types

| Type name                 | Value | Direction | Payload                                    |
| ------------------------- | ----- | --------- | ------------------------------------------ |
| `GET_VOLUME_HEADER`       | 0x01  | W → P     | empty                                      |
| `VOLUME_HEADER`           | 0x02  | P → W     | 1024-byte NeoTape Volume Header            |
| `NEXT_FRAME`              | 0x03  | W → P     | empty                                      |
| `FRAME_RECORD`            | 0x04  | P → W     | one complete NeoTape record                |
| `ARCHIVE_END_HEADER`      | 0x05  | P → W     | 1024-byte NeoTape Archive End Header       |
| `TAPE_EOF`                | 0x06  | P → W     | empty                                      |
| `ERROR`                   | 0x07  |  either   | UTF-8 error message, not NUL-terminated    |

Values `0x00` and `0x80`–`0xFF` are reserved. `0x01`–`0x07` are the initial
protocol set; future extensions may add request or response types.

### 4.4 Request-Response Rules

1. After connecting, the writer **MUST** send `GET_VOLUME_HEADER` before sending
   any `NEXT_FRAME` request. The archiver **MUST** respond with `VOLUME_HEADER`.
2. After receiving `VOLUME_HEADER`, the writer may send `NEXT_FRAME` repeatedly.
3. The archiver **MUST** respond to each `NEXT_FRAME` with exactly one of:
   - `FRAME_RECORD` — a complete NeoTape record whose length equals the
     `volume_block_size` declared in the Volume Header.
   - `ARCHIVE_END_HEADER` — the archive is complete; the writer writes it and
     exits.
   - `TAPE_EOF` — the archiver asks the writer to write a tape filemark at the
     current position and then continue; the writer may send another
     `NEXT_FRAME` afterwards.
4. The archiver **MAY** send `TAPE_EOF` at any time in place of a frame. The
   writer **MUST** write a filemark and then continue requesting frames.
5. Either side **MAY** send `ERROR` to report a recoverable or fatal problem. The
   sender **SHOULD** close the connection after sending `ERROR`; the receiver
   **MUST** treat an unexpected close as an error.
6. For fatal errors that prevent sending `ERROR` (e.g., a tape write I/O error),
   the writer **MAY** close the connection abruptly. The archiver **MUST** expect
   disconnections and keep its state so a new writer can resume.

## 5. Archiver State

The archiver maintains at least the following state for the lifetime of an
archive:

- `archive_uuid`, `archive_name`, `archive_time`.
- `volume_seq_num` of the volume currently being produced.
- `volume_block_size` (fixed for the entire archive).
- `slice_index` of the next slice to emit.
- `frame_index` of the next frame within that slice.
- Whether the current slice is still open or already closed.

When a new writer connects:

1. The archiver continues with the same `volume_seq_num` if no writer has
   completed a volume since the last connection.
2. If the previous writer reported that it reached EOT, the archiver increments
   `volume_seq_num` and generates a fresh Volume Header for the new connection.

The archiver does not need to persist state across its own restarts in this
version. Slice-level resume is a future extension (see Section 9).

## 6. Writer State

The writer maintains only the state needed for the current volume:

- The Volume Header received at startup.
- The current tape/spool position.
- Whether it has written a filemark after the last frame.

It does not remember the previous volume header; it obtains a fresh one from the
producer at startup.

## 7. End-of-Tape Behavior

- The writer is responsible for detecting EOT. On a real tape device this comes
  from the drive (`EOT` or `EOM` status). On a spool backend it comes from a
  configured virtual tape size.
- When the writer detects EOT while writing a frame, it **MUST** finish writing
  the current frame if possible, write a trailing filemark, and then exit.
- The writer **MUST NOT** request another `NEXT_FRAME` after detecting EOT.
- If the archiver has already sent a frame that does not fit on the current
  volume, the writer exits after writing the current record as far as possible.
  The next writer reconnects and requests the next frame from the archiver;
  the archiver continues from the first frame that has not been fully written
  to a completed volume. A later design may refine partial-tail handling; this
  version treats any incomplete tail frame as archiver-side redrive.

## 8. CLI Sketch

The exact command names and flags are illustrative and may change before
stabilization. The goal is an archiver daemon and a per-volume writer binary.

### Archiver

```sh
neotape-archiver --listen tcp://0.0.0.0:9123 \
                 --archive-name home \
                 --volume-block-size 4M \
                 -C /data photos docs
```

The archiver scans sources, emits slice metadata internally, and serves the
protocol until the archive is complete. When invoked without `--listen` it
behaves like `mt-pax` and writes a standard pax stream to stdout or `-f`.

### Writer

```sh
neotape-write --source tcp://backup-host:9123 --target tape:/dev/nst0
neotape-write --source unix:/run/neotape/home.sock --target spool:./vol3.spool
```

One writer process per volume. When it exits with code 0 and no Archive End
Header was written, the operator loads the next tape and starts another writer
pointing at the same source.

### Planner

`neotape-plan` remains a separate offline tool:

```sh
neotape-plan -C /data -o home.plan photos docs
neotape-archiver --plan home.plan --listen tcp://0.0.0.0:9123
```

## 9. Future Extensions

The following are explicitly out of scope for the first implementation but are
reserved for later:

- **Slice-level resume**: allowing an archiver to restart from a saved checkpoint
  rather than regenerating the whole archive.
- **Writer-initiated status queries**: e.g., `QUERY_STATUS` from writer to
  producer.
- **Producer-initiated pause/resume**: e.g., `PAUSE_FRAMES` / `RESUME_FRAMES`.
- **Multiple concurrent writers**: one producer feeding multiple writers for
  parallel volumes (requires splitting the archive stream).
- **Compression/encryption negotiation**: currently the archiver produces the
  exact bytes that belong on tape.

## 10. Trade-off Summary

### Chosen approach: single-connection request-response

**Pros**:
- Minimal protocol surface.
- Natural back-pressure.
- Easy multi-volume support via reconnect.
- Works over TCP and UDS with the same code.
- Archiver is the single source of archive truth.

**Cons**:
- Producer must stay alive and keep state until the archive finishes.
- No separate control plane for monitoring or out-of-band commands.
- Frame-by-frame RTT adds latency; mitigated by large frames (≥256 KiB recommended,
  up to 8 MiB).

### Rejected alternatives

- **Dual data + control channels**: more flexible but over-engineered for the
  "one writer per volume" model. All control needs fit into request-response
  messages.
- **Raw byte-stream pipeline**: simpler still but lacks back-pressure, frame
  boundaries, and clean resume semantics.

## 11. Open Questions

1. Should the producer auto-increment `volume_seq_num` on every writer
   connection, or only when the previous writer explicitly signaled EOT?
   - Auto-increment on every connection is simpler, the writer writes the header as is

2. How should the writer handle a `FRAME_RECORD` whose length does not match the
   declared `volume_block_size`?
   - This is critical error, it's not allowed by the spec. The writer should report an error and exit. The producer should never send such a frame; if it does, it's a bug that needs fixing.
3. Should the protocol include a capability/version exchange at connection start?
   - For the initial version, we can assume a single version and no capabilities. Future versions can add a `HELLO` message or similar if needed.
4. What exit codes should `neotape-write` use to distinguish "archive complete",
   "EOT reached", and "error"?
   - Exit code 0: archive complete (received `ARCHIVE_END_HEADER`)
   - Exit code 1: EOT reached (detected EOT while writing a frame), can be integrated by external scripts to initiate tape change with automatic tape library.
   - Exit code 2: error (received `ERROR` or encountered a local error)
