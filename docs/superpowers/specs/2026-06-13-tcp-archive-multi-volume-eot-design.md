# NeoTape TCP Archive Multi-Volume & EOT Design

Date: 2026-06-13
Status: design / pending implementation
Related specs:
- `docs/superpowers/specs/2026-06-13-tcp-archive-generation-design.md`
- `docs/spec/01-volume-header.md`
- `docs/spec/02-frame-header.md`
- `docs/spec/03-archive-end-header.md`
- `docs/spec/06-frames-and-slices.md`

## 1. Goal

Extend the existing `neotape-archiver` / `neotape-write` pair so that a single
long-running archiver can stream a large archive across multiple physical tape
volumes. The writer process is still short-lived and handles exactly one volume
per invocation. When a tape fills up, the writer exits with a distinct code, the
operator (or an external script) starts another writer process against the same
archiver, and the archive continues from the next unwritten frame.

This document also covers end-of-tape (EOT) detection and a minimal read-back
verification workflow for the test run on `/dev/tapeA` and `/dev/tapeB`.

## 2. Test Scenario

- Two LTO-5 drives: `/dev/tapeA` (symlink to `nst0`) and `/dev/tapeB` (symlink
to `nst1`).
- Both drives contain blank LTO-5 cartridges formatted with LTFS, leaving an
A/B partition layout. The head is positioned at partition 0 (the A partition,
approximately 33 GiB usable for the test).
- Source data: `./testing/data`, approximately 67 GiB.
- Volume block size: 4 MiB (default).
- Hardware compression is enabled on both drives, so the effective capacity is
expected to exceed 33 GiB and the archive should fit on two volumes.
- The writer is invoked with `--erase` for both drives because the LTFS format
already wrote metadata on the tapes.

Expected flow:

1. Start `neotape-archiver --listen ... --volume-block-size 4M testing/data`.
2. `neotape-write --source ... --target tape:/dev/tapeA --erase` writes volume 1
until it reaches EOT, then exits with code 1.
3. `neotape-write --source ... --target tape:/dev/tapeB --erase` writes volume 2
until the archive end header is received, then exits with code 0.
4. The archiver exits cleanly after sending the archive end header.
5. The tapes are read back into spool directories for verification.

## 3. Existing Gaps

### 3.1 Archiver stops after the first connection

`run_tcp_archiver()` currently calls `accept()` once. After the first writer
disconnects, the function returns and the process exits. Multi-volume support
requires the archiver to keep accepting new connections until the archive is
complete.

### 3.2 `volume_seq_num` does not advance between volumes

The archiver currently reuses the same `volume_seq_num` for every connection.
Each physical volume must receive a unique volume sequence number.

### 3.3 Writer EOT detection is incomplete

`neotape-write` only checks `status().eot()` before writing a frame. On real
tape hardware the more common EOT signal is a failed `write()` returning
`ENOSPC` (or, less commonly, the `EOT` bit appearing in the drive status after
a write). The writer must treat these as end-of-tape, finish the current frame
if possible, write a trailing filemark, and exit with code 1.

### 3.4 No read-back verification tool

There is no `neotape-read` command. For this test we will add a minimal
tape-to-spool reader that records each tape record as a separate file and
inserts filemark boundaries into the spool naming, so that the on-tape layout
can be compared with the produced archive.

### 3.5 No ack-based flow control

The current protocol sends `NEXT_FRAME` and immediately receives the next frame
without confirming that the previous frame reached the tape. If a writer uses
any buffering, a volume change could lose frames that left the archiver but
never reached the medium. This design adds explicit per-frame acknowledgements
so the archiver knows exactly which frames are durable on tape.

## 4. Design Decisions

### 4.1 Global frame sequence numbers

Every frame record carries a `global_frame_seq_num` in its NeoTape Frame
Header. This number is **monotonically increasing across the entire archive**,
regardless of slice boundaries or frame content types (data, metadata, etc.).
`ARCHIVE_END_HEADER` records the last global frame sequence number that belongs
to the archive.

The wire protocol uses this same number for acknowledgements and resume.

### 4.2 Protocol additions

The base protocol from `2026-06-13-tcp-archive-generation-design.md` is
extended with one new message:

| Type name            | Value | Direction | Payload                               |
| -------------------- | ----- | --------- | ------------------------------------- |
| `ACK_FRAME`          | 0x08  | W → P     | uint64 little-endian `global_frame_seq_num` |

`ACK_FRAME(g)` is a **cumulative** acknowledgement: because tape writes are
strictly sequential, acknowledging global frame `g` means that every frame
with `global_frame_seq_num <= g` has been durably written to the current
volume.

### 4.3 Archiver-side frame retention

The archiver keeps a bounded retention buffer of recently emitted frames.
When the writer acknowledges a frame via `ACK_FRAME(g)`, the archiver may
discard all frames with `global_frame_seq_num <= g`. The buffer must be large
enough to hold every frame that has been sent to the writer but not yet
acknowledged.

For the first implementation the retention buffer capacity is expressed as a
frame count (for example, 256 frames ≈ 1 GiB for 4 MiB blocks). This is a
configuration option in `TcpArchiverOptions`; the CLI can set it with
`--retention-frame-count`.

### 4.4 Writer-side buffering

The writer maintains an output queue so that tape writes can be pipelined
while the next frame is being fetched from the archiver. Because real LTO
drives buffer roughly 256 MiB internally, the writer buffer should be at least
that large to be useful. The buffer size is therefore expressed in bytes:

- Default: 256 MiB.
- Minimum: 8 MiB (the maximum allowed frame size).
- Configurable via `--output-buffer-size` on `neotape-write`.

The writer must not allow unacknowledged frames to outrun the archiver
retention window. In practice the writer waits for room in its output buffer
before sending `NEXT_FRAME`, and the archiver retention buffer is sized to hold
several times the writer buffer.

After a frame is successfully written to tape, the writer sends
`ACK_FRAME(global_frame_seq_num)`. Because acknowledgements are cumulative,
each ack reflects the most recently written frame.

### 4.5 Volume sequence advancement: implicit commit via `NEXT_FRAME`

To avoid incrementing `volume_seq_num` for writers that never actually commit a
volume header to tape, the archiver only treats a volume as "committed" once it
receives the first `NEXT_FRAME` request on that connection.

Rules:

- The archiver starts with `next_volume_seq_num = 1`.
- On a new connection it sends `VOLUME_HEADER(next_volume_seq_num)`.
- If the connection closes before the first `NEXT_FRAME`, the same sequence
number is reused for the next connection.
- After the first `NEXT_FRAME` is received, the archiver marks the current
volume as committed. When that connection eventually closes (EOT, error, or
archive end), `next_volume_seq_num` is incremented before the next `accept()`.
- When the archive completes, the archiver stops listening and exits.

### 4.6 One writer at a time

The archiver continues to accept only one connection at a time. This keeps the
serialization pipeline single-threaded and avoids splitting the archive stream.

### 4.7 Writer exit codes

Per the base spec, exit codes are:

- `0` — archive complete (`ARCHIVE_END_HEADER` written).
- `1` — EOT reached while writing a frame; a trailing filemark has been
written and the operator may continue with a new volume.
- `2` — error (protocol error, I/O error other than EOT, frame size mismatch,
archiver-reported error, retention window underrun, etc.).

### 4.8 EOT detection and resume in the writer

A write attempt is considered to have hit EOT if any of the following occur:

1. `TapeDevice::status()` reports `EOT` immediately before the write.
2. `write_record()` fails with `ENOSPC`.
3. After a successful `write_record()`, the drive status reports `EOT`.

When EOT is detected:

- The writer stops accepting new frames from the archiver and waits for its
output queue to drain as far as possible.
- The writer sends `ACK_FRAME` for every frame that was fully written to the
current volume.
- The writer writes one trailing filemark.
- The writer exits with code 1.

When a new writer connects, it resumes from `last_acked_global_frame + 1`.
The archiver must have retained that frame; if it has been discarded, the
archiver sends `ERROR` and exits because the retention window was too small.

### 4.9 Initial handshake and resume

The first message from a writer is still `GET_VOLUME_HEADER`. The archiver
responds with the current volume header. The writer then sends `NEXT_FRAME`
requests as before.

On a resume connection (any connection after the first), the archiver already
knows the last acknowledged global frame from the previous writer, so the next
`FRAME_RECORD` it emits starts from `last_acked_global_frame + 1`. The writer
does not need to send the resume point explicitly.

### 4.10 Read-back verification tool

A new `neotape-read` binary is added for this test. Its CLI:

```sh
neotape-read --source <tape:/dev/nst0|spool:./dir> --target <spool:./out>
```

Behavior:

- Reads records sequentially from the source.
- Writes each record to a separate file in the target spool directory,
following the existing `SpoolTapeDevice` naming convention.
- Inserts a filemark by finalizing the current spool file whenever a tape
filemark is encountered.
- Stops at EOD / end of spool.
- Verifies that the first record is a valid NeoTape Volume Header and that the
last record is a valid Archive End Header.
- Reports the total number of records, filemarks, and any parse errors.

For the tape test, `neotape-read` is run against `/dev/tapeA` and
`/dev/tapeB` separately. The combined record count should match the frame
count emitted by the archiver, and the archive end header on the second volume
should indicate a clean end.

## 5. Archiver State Machine

```text
[listen]
  |
  v
accept() <----------------------------------------------------+
  |                                                           |
  v                                                           |
Send VOLUME_HEADER(next_volume_seq_num)                       |
  |                                                           |
  v                                                           |
Wait for first NEXT_FRAME ----------+                         |
  |               | connection      |                         |
  v               | closed before   |                         |
Mark committed    | first NEXT      |                         |
  |               v                 |                         |
  |        Reuse same volume_seq_num|                         |
  v                                 |                         |
Serve frames, retain unacked        |                         |
  |                                 |                         |
  +-- ACK_FRAME(g) -> discard frames <= g                    |
  |                                 |                         |
  +-- ARCHIVE_END_HEADER written -> close listener, exit      |
  |                                                           |
  +-- connection closed (EOT/error) -> increment              |
          next_volume_seq_num, go to accept() ----------------+
```

## 6. Writer Flow

```text
connect -> GET_VOLUME_HEADER -> receive VOLUME_HEADER -> write to tape
loop:
    NEXT_FRAME -> receive FRAME_RECORD / ARCHIVE_END_HEADER / TAPE_EOF
    FRAME_RECORD:
        add to output queue
        drain queue to tape, sending ACK_FRAME after each successful write
        if EOT detected during drain:
            drain as much as possible
            send ACK_FRAME for all fully written frames
            write filemark
            exit 1
        continue loop
    ARCHIVE_END_HEADER: write to tape -> exit 0
    TAPE_EOF: write filemark -> continue loop
```

## 7. Files to Modify / Create

- `include/neotape/tcp_protocol.hpp` — add `ack_frame` message type.
- `include/neotape/tcp_server.hpp` — add retention count option and
`initial_volume_seq_num` field.
- `src/neotape_tcp_protocol.cpp` — handle encoding/decoding of `ACK_FRAME`.
- `src/neotape_tcp_server.cpp` — implement multi-connection accept loop,
volume commit tracking, sequence advancement, frame retention, ack handling,
and resume.
- `src/neotape_archiver_cmd.cpp` — expose `--retention-frame-count` CLI flag.
- `src/neotape_write_cmd.cpp` — implement EOT detection, output queue,
ack sending, and exit codes.
- `src/neotape_read_cmd.cpp` — new file for the read-back tool.
- `Makefile` — add `bin/neotape-read` target and update test rules.
- `tests/smoke_tcp_archive.sh` — extend to exercise multi-volume spool
behavior with an artificial capacity limit.

## 8. Testing Plan

### 8.1 Spool-based smoke test

Before touching real tape hardware, add a spool variant that limits the spool
capacity to force an EOT and reconnect:

- Start archiver on a Unix-domain socket with a small retention window.
- Start writer with a small output queue and a small virtual tape size
(for example, 8 MiB).
- Expect writer 1 to exit 1 after writing a few frames.
- Start writer 2 on the same source with a fresh spool and a larger size.
- Expect writer 2 to exit 0.
- Verify that the spools contain unique volume headers (seq 1 and 2), that
frame records do not overlap, and that the archive end header is present on the
second spool.

### 8.2 Real tape test

Execute the scenario described in Section 2:

1. Build all binaries.
2. `neotape-archiver --listen tcp://127.0.0.1:9123 --volume-block-size 4M \
   --archive-name neotape-test testing/data`
3. In another terminal: `neotape-write --source tcp://127.0.0.1:9123 \
   --target tape:/dev/tapeA --erase`
4. Wait for exit 1.
5. `neotape-write --source tcp://127.0.0.1:9123 --target tape:/dev/tapeB \
   --erase`
6. Wait for exit 0; archiver should also exit 0.
7. Read back both tapes:
   - `neotape-read --source tape:/dev/tapeA --target spool/vol1`
   - `neotape-read --source tape:/dev/tapeB --target spool/vol2`
8. Inspect spool contents with `neotape-inspect` or `od`/custom scripts and
confirm the combined record sequence is continuous and ends with the archive
end header.

## 9. Future Work

- Slice-level resume and persistent archiver state are explicitly out of scope.
- A full `neotape-restore` tool that reconstructs files from the pax payload is
not part of this test.
