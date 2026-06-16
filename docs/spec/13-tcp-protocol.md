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
9-byte header).  The Server and Client MUST reject any payload larger than
16 MiB before allocating memory for it.

## Protocol flow

### Writing pipeline (Archiver = Server, Writer = Client)

1. Writer connects → Writer sends `next_frame`
2. Archiver responds with `frame_record` or `tape_eof` or `error`
3. Writer thread writes record to media, then sends `ack_frame(seq)`
4. Writer requests another `next_frame` (bounded by local output buffer)
5. On EOT, Writer drains queued writes, sends final ACK, and exits non-zero
6. On `tape_eof` from Archiver, Writer drains queue and exits zero

In this pipeline `tape_eof` is sent by the Archiver to signal that no more
frames will be produced.  The Writer never sends `tape_eof` in this
direction.

### Reading pipeline (Extractor = Server, Reader = Client)

1. Reader connects → Extractor sends `next_frame`
2. Reader reads next record from tape/spool → sends `frame_record`
3. Extractor validates record → sends `ack_frame(seq)`
4. Repeat from step 1
5. Reader hits EOT → sends `tape_eof` → disconnects
6. Operator loads next volume, re-connects Reader

## Error handling

Either side may send `error` at any time.  The sender SHOULD close the
connection after sending an `error` message.  The receiver SHOULD log the
error to stderr and exit non-zero.

## Sequence validation

The Server validates every `frame_record` for:

- `magic`, `header_version`
- `frame_hash`
- Record size matches decoded `volume_block_size_kib`
- `archive_uuid` / `archive_label` consistency with first frame
- Monotonic, gapless `global_frame_seq_num`
- Monotonic `volume_seq_num` (advisory — must not go backward, may skip at
  most 1)
- `logical_slice_seq_num` and `frame_seq_num_within_channel` per spec
- Channel ordering (`ch_metadata` before `ch_content`)

On validation failure the Server sends `error` with a human-readable message
and closes the connection.

## Multi-volume

The Server persists validation state across Client disconnects.  When a new
Client connects, the first `frame_record` it sends MUST carry sequence
numbers that continue exactly from the last validated frame.  Any gap,
backward jump, or identity mismatch causes the Server to send `error` and
close the connection.

## Security

Security is **not** in scope for this protocol.  It is designed for operation
over localhost or a trusted LAN (at minimum 2.5 Gbps; typical deployment
targets 10 Gbps or faster).  In this environment the threat model assumes a
benign, non-adversarial network and mutually trusted peers.

The protocol does **not** provide:

- Authentication or peer identity verification
- Transport-layer encryption (TLS)
- Replay defense
- DoS resistance beyond the `max_message_payload_size` check

For deployments where any of these properties are required, the operator
SHOULD tunnel the connection through an external secure channel (e.g.
SSH port forwarding, WireGuard, or a TLS proxy).  The NeoTape wire protocol
remains plaintext by design.
