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

Message direction depends on the pipeline (see [Message direction](#message-direction)).
`error` is always bidirectional.

| ID | Name | Payload |
|---|---|---|
| 0x01 | `next_frame` | empty |
| 0x02 | `frame_record` | raw NeoTape record bytes |
| 0x03 | `tape_eof` | empty |
| 0x04 | `error` | UTF-8 error message |
| 0x05 | `ack_frame` | `uint64_t` LE global frame seq num |

## Wire format

Each message is framed:

```
[1 byte type][8 bytes payload length LE][N bytes payload]
```

The 8-byte length is little-endian and counts payload bytes only (not the
9-byte header).  The Server and Client MUST reject any payload larger than
16 MiB before allocating memory for it.

## Protocol flow

### Message direction

The direction of `next_frame`, `frame_record`, `tape_eof`, and `ack_frame`
depends on the pipeline.  In the **writing** pipeline, the Client (Writer)
pulls frames from the Server (Archiver): `next_frame` flows Client→Server
and `frame_record` / `tape_eof` flow Server→Client.  In the **reading**
pipeline, the Server (Extractor) pulls frames from the Client (Reader):
`next_frame` flows Server→Client and `frame_record` / `tape_eof` flow
Client→Server.  `error` is always bidirectional.

### Writing pipeline (Archiver = Server, Writer = Client)

1. Writer connects → Writer sends `next_frame`
2. Archiver responds with `frame_record` or `tape_eof` or `error`
3. Writer thread writes record to media, then sends `ack_frame(seq)`
4. Writer requests another `next_frame` (bounded by local output buffer)
5. On `tape_eof` from Archiver: Writer writes a filemark and continues
   requesting frames (slice boundary).  The Archiver may send more frames
   from subsequent slices.
6. On `archive_end` (`frame_record` with `ARCHIVE_END` channel): Writer
   drains queued writes, sends final ACK, and exits zero.
7. On physical EOT (tape drive reports write failure): Writer drains queue,
   sends final ACK, and exits non-zero.

### Reading pipeline (Extractor = Server, Reader = Client)

1. Reader connects → Extractor sends `next_frame`
2. Reader reads next record from tape/spool → sends `frame_record`
3. Extractor validates record → sends `ack_frame(seq)`
4. Repeat from step 1
5. Reader hits physical EOT → sends `tape_eof` → disconnects
6. Operator loads next volume, re-connects Reader

## Error handling

Either side may send `error` at any time.  The sender SHOULD close the
connection after sending an `error` message.  The receiver SHOULD log the
error to stderr and exit non-zero.

## Sequence validation

The Server validates every `frame_record` for framing, identity, and ordering
rules:

- `magic`, `header_version`
- Record size matches decoded `volume_block_size_kib`
- `archive_uuid` / `archive_label` consistency with first frame
- Monotonic, gapless `global_frame_seq_num`
- Monotonic `volume_seq_num` (advisory — must not go backward, may skip at
  most 1)
- `logical_slice_seq_num` and `frame_seq_num_within_channel` per spec
- Channel ordering (`ch_metadata` before `ch_content`)
- `frame_hash`, except for the restore-mode metadata exception below

In the reading pipeline, a restore-mode Server that reconstructs only
`ch_content` MAY downgrade a `ch_metadata`-only integrity failure to a warning
once the frame has already been identified as `ch_metadata` and header
parsing, record sizing, archive identity, and sequence continuity remain
unambiguous. This exception applies only to advisory metadata; it does not
permit skipping `ch_content` corruption, ambiguous headers, or any continuity
failure. When used, the Server SHOULD warn, ignore the unusable metadata
payload, and continue extraction.

On fatal validation failure the Server sends `error` with a human-readable
message and closes the connection.

## Multi-volume

The Server persists validation state across Client disconnects.  When a new
Client connects, the first `frame_record` it sends MUST carry sequence
numbers that continue exactly from the last validated frame.  Any gap,
backward jump, or identity mismatch causes the Server to send `error` and
close the connection.

A volume is considered committed when the Server receives the first
`ack_frame` from the Client — that is, after at least one frame has been
successfully written to the physical medium and acknowledged.  If a Client
disconnects before any `ack_frame` is received (e.g. the tape drive was not
ready, or the Client crashed immediately), the volume is **not** committed
and the Server reuses the same `volume_seq_num` for the next Client.
This keeps `volume_seq_num` gapless across the archive.

## Security

The TCP protocol itself provides no transport-level security.  It is designed
for operation over localhost or a trusted LAN (at minimum 2.5 Gbps; typical
deployment target 10 Gbps or faster).

### Frame-level protection (SIGNED flag)

When the archive producer uses signed frames ([`SIGNED` flag](01-frame-header.md)
with [Ed25519 signature](00-format-common.md) over `frame_hash`), **integrity
and authenticity are guaranteed at the frame level** regardless of transport.
A tampered or forged frame will fail BLAKE3 hash verification and Ed25519
signature validation during extraction, even if the TCP connection is
unencrypted and routed over an untrusted network.  Sequence number checks
further prevent replay and reordering.

When signed frames are in use, the remaining threats that an external
tunnel addresses are:

- **Confidentiality** — frame payloads are transmitted in plaintext.
- **Peer authentication at connection time** — the signature only proves
  the frame author, not the identity of the TCP peer.

### Without signed frames

The protocol does **not** provide:

- Peer identity verification
- Transport-layer encryption (TLS)
- Replay defense
- DoS resistance beyond the `max_message_payload_size` check

### Recommendation

For deployments that require confidentiality or peer authentication, tunnel
the connection through an external secure channel (e.g. SSH port forwarding,
WireGuard, or a TLS proxy).  The NeoTape wire protocol remains plaintext by
design.

### Future: challenge-response authentication

If Ed25519 signing is already in use for frames (`SIGNED` flag), the same
key material can authenticate the TCP peer at connection time with minimal
added complexity.  The envisioned extension adds two message types:

| ID | Name | Direction | Payload |
|---|---|---|---|
| 0x06 | `auth_challenge` | Server → Client | 32-byte random nonce |
| 0x07 | `auth_response` | Client → Server | 64-byte Ed25519 signature |

The flow:

```
Server                                   Client
  |                                        |
  |  ← TCP connect                         |
  |                                        |
  |  auth_challenge(32B random nonce)  →   |
  |                                        |
  |            sign(sk, "NeoTape-auth"      |
  |                  || nonce)              |
  |  ← auth_response(64B Ed25519 sig)      |
  |                                        |
  |  verify(pk, "NeoTape-auth"             |
  |         || nonce, sig)                 |
  |  → ok → protocol proceeds              |
  |  → fail → error, close                 |
```

**Domain separation.**  The signed message is the concatenation of a constant
context string and the nonce (`"NeoTape-auth" || nonce`), not the bare nonce.
This prevents cross-context signature replay — a valid `ack_frame` signature
over `frame_hash` cannot be submitted as an `auth_response`, and vice-versa,
even when the same Ed25519 key is used for both purposes.

The context string is a fixed, null-terminated ASCII literal compiled into
both Server and Client.  It does not travel over the wire.

When this extension is active, both frame signing and challenge-response
auth use the same Ed25519 key material over different domain strings:
`"NeoTape-frame"` (format spec: [00-format-common.md](00-format-common.md))
and `"NeoTape-auth"` (this section).  The signatures are cryptographically
independent even with a shared key.

The Server issues a fresh random nonce per connection, so replay of a
previous `auth_response` is impossible.  The exchange costs one round-trip
after TCP handshake (negligible on localhost or LAN).

This design assumes Server and Client share the same Ed25519 secret key
(e.g. via a local file or out-of-band distribution).  It authenticates
the Client to the Server (one-way); mutual authentication would add a
second challenge in the opposite direction but is unnecessary when the
Server is a trusted, operator-controlled host.

The implementation footprint is small: two libsodium calls
(`crypto_sign_detached` / `crypto_sign_verify_detached`), ~50 lines of
code, and no external PKI.
