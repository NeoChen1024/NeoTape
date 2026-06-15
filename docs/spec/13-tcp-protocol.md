# TCP Protocol

Status: normative.

## Overview

NeoTape separates long-running archive servers from short-lived per-volume
clients over a single TCP or Unix-domain socket.  The same protocol serves
both the writing pipeline (archiver ↔ writer) and the reading pipeline
(extractor ↔ reader).

## Roles

| Role | Responsibility | Long-lived? |
|---|---|---|
| **Server** | Owns archive state, validates frames, drives the protocol | Yes |
| **Client** | Reads/writes raw NeoTape records from a physical medium | No (per-volume) |

## Message types

| ID | Name | Direction | Payload |
|---|---|---|---|
| 0x01 | `next_frame` | Server → Client | empty |
| 0x02 | `frame_record` | Client → Server | raw NeoTape record bytes |
| 0x03 | `tape_eof` | Client → Server | empty |
| 0x04 | `error` | bidirectional | UTF-8 error message |
| 0x05 | `ack_frame` | Server → Client | `uint64_t` LE global frame seq num |

## Wire format

Each message is framed:

```
[1 byte type][8 bytes payload length LE][N bytes payload]
```

The 8-byte length is little-endian and counts payload bytes only (not the
9-byte header).  Maximum payload length is implementation-defined; the
current limit is 16 MiB.

## Protocol flow

### Writing pipeline (Archiver = Server, Writer = Client)

Writer connects → Archiver responds with `frame_record` → Writer sends
`ack_frame` → Archiver sends `tape_eof` at slice boundaries.  Writer
disconnects at end-of-tape; operator loads next volume and re-connects.

### Reading pipeline (Extractor = Server, Reader = Client)

Reader connects → Extractor sends `next_frame` → Reader reads tape and sends
`frame_record` → Extractor validates and sends `ack_frame(seq_num)` →
repeat.  Reader sends `tape_eof` at end-of-tape, then disconnects.
Operator loads next volume and re-connects Reader.

## Error handling

Either side may send `error` at any time.  The sender SHOULD close the
connection after sending an `error` message.  The receiver SHOULD log the
error to stderr and exit non-zero.

## Sequence validation

The Server validates every `frame_record` for:

- `magic`, `header_version`
- `frame_hash`
- `archive_uuid` / `archive_label` consistency with first frame
- Monotonic, gapless `global_frame_seq_num`
- Monotonic `volume_seq_num` (advisory but gapless — no skip, no backward)
- `logical_slice_seq_num` and `frame_seq_num_within_channel` per spec

On validation failure the Server sends `error` with a human-readable message
and closes the connection.

## Multi-volume

The Server persists validation state across Client disconnects.  When a new
Client connects, the first `frame_record` it sends MUST carry sequence
numbers that continue exactly from the last validated frame.  Any gap,
backward jump, or identity mismatch causes the Server to send `error` and
close the connection.
