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
| **Server** | Owns archive-global state and drives the protocol | Yes |
| **Client** | Reads or writes raw NeoTape records on a physical medium | No (per-volume) |

The endpoint that receives a `frame_record` validates it before use. This is
the Writer Client in the writing pipeline and the Extractor Server in the
reading pipeline.

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

1. Writer connects. If configured with a trusted public key, it completes the
   challenge-response exchange before opening, rewinding, or writing the target
   medium.
2. Writer sends `next_frame`.
3. Archiver responds with `frame_record` or `tape_eof` or `error`.
4. Writer validates the record, writes the complete record to media, then sends
   `ack_frame(seq)` only after the record is committed
5. Writer requests another `next_frame` (bounded by local output buffer)
6. On `tape_eof` from Archiver: Writer writes a filemark and continues
   requesting frames (slice boundary).  The Archiver may send more frames
   from subsequent slices.
7. On `archive_end` (`frame_record` with `archive_end` channel): Writer
   drains queued writes, sends final ACK, and exits zero.
8. On physical EOT (tape drive reports write failure): Writer sends ACKs only
   for records that were completely committed. It does not acknowledge the
   incomplete record and exits non-zero.

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

In the writing pipeline, the Archiver maintains authoritative archive-generation
state across Writer connections. The Writer independently validates each
received record before committing it and validates sequence continuity
observable within its current connection.

In the reading pipeline, the Extractor maintains authoritative validation state
across Reader connections and validates the complete archive stream before
emitting payload bytes. Both pipelines apply the relevant shared rules in
[docs/spec/05-validation.md](05-validation.md).

On fatal validation failure the endpoint that detected the failure SHOULD send
`error` with a human-readable message and close the connection.

## Multi-volume

The Archiver persists archive-generation and acknowledgement state across
Writer disconnects. The Extractor persists archive-validation state across
Reader disconnects. In the reading pipeline, when a new Reader connects, its
first `frame_record` MUST continue exactly from the last validated frame. Any
gap, backward jump, or identity mismatch causes the Extractor to send `error`
and close the connection.

A volume in the writing pipeline is considered committed when the Archiver
receives the first `ack_frame` from the Writer — that is, after at least one
frame has been successfully written to the physical medium and acknowledged.
If a Writer disconnects before any `ack_frame` is received (e.g. the tape drive
was not ready, or the Writer crashed immediately), the volume is **not**
committed and the Archiver reuses the same `volume_seq_num` for the next Writer.
This keeps `volume_seq_num` gapless across the archive.

## Security

The protocol is plaintext and provides no confidentiality. The base mode also
provides no peer authentication. The optional challenge-response mode below
provides one-way Archiver authentication for the writing pipeline, but does not
provide client authentication or encryption.

### Frame-level protection (SIGNED flag)

When the archive producer uses signed frames ([`SIGNED` flag](02-frame-header.md)
with [Ed25519 signature](00-format-common.md) over
`NeoTape-frame\0 || frame_hash`), a receiver configured with the corresponding
trusted public key can verify integrity and authenticity at the frame level.
A tampered or forged frame will fail verification even if the TCP connection is
unencrypted. Sequence checks reject frame duplication and reordering within the
archive state being validated; they do not prove archive freshness or prevent
replay of an entire otherwise-valid old archive.

In the writing pipeline, a Writer configured with a trusted public key enters
authenticated writing mode: it MUST complete challenge-response before opening,
rewinding, or writing the target medium, and every received frame MUST be signed
and verify successfully before being committed. In the reading pipeline, the
Reader client MAY remain a dumb forwarder; authoritative signed-frame
validation remains the Extractor's responsibility.

When signed frames are in use, the remaining threats that an external
tunnel addresses are:

- **Confidentiality** — frame payloads are transmitted in plaintext.
- **Client authentication** — challenge-response authenticates the Archiver to
  the Writer, not the Writer to the Archiver.
- **Archive freshness** — a valid signed archive can still be replayed in full
  unless the receiver independently checks an expected `archive_uuid` or other
  freshness policy.

### Without signed frames

The protocol does **not** provide:

- Peer identity verification
- Transport-layer encryption (TLS)
- Connection-level replay defense
- DoS resistance beyond the `max_message_payload_size` check

### Recommendation

For deployments that require confidentiality or mutual peer authentication, tunnel
the connection through an external secure channel (e.g. SSH port forwarding,
WireGuard, or a TLS proxy).  The NeoTape wire protocol remains plaintext by
design.

### Challenge-response authentication

This extension is primarily for the writing pipeline (Archiver = Server,
Writer = Client).  The goal is for the Writer to authenticate the Archiver
before accepting frames, using the same trust root as frame-signature
verification.

When the Writer is configured with a trusted public key, it MUST use this mode
before requesting the first frame or touching the target medium. The same key
material used for signed frames authenticates the TCP peer. The protocol adds
two message types:

| ID | Name | Direction | Payload |
|---|---|---|---|
| 0x06 | `auth_challenge` | Client → Server | 32-byte random nonce |
| 0x07 | `auth_response` | Server → Client | 64-byte Ed25519 signature |

The flow:

```
Writer                                   Archiver
  |                                        |
  |  → TCP connect                         |
  |                                        |
  |  auth_challenge(32B random nonce)  →   |
  |                                        |
  |            sign(sk, "NeoTape-auth\0"    |
  |                  || nonce)              |
  |  ← auth_response(64B Ed25519 sig)      |
  |                                        |
  |  verify(pk, "NeoTape-auth\0"           |
  |         || nonce, sig)                 |
  |  → ok → protocol proceeds              |
  |  → fail → error, close                 |
```

**Domain separation.**  The signed message is the concatenation of a constant
context string and the nonce (`NeoTape-auth\0 || nonce`), not the bare nonce.
The context string includes its trailing NUL byte. This prevents
cross-context signature replay — a valid frame signature over
`NeoTape-frame\0 || frame_hash` cannot be submitted as an `auth_response`,
and vice-versa, even when the same Ed25519 key is used for both purposes.

The context string is a fixed, null-terminated ASCII literal compiled into
both Server and Client.  The terminating NUL byte is part of the signed
message.  The context string does not travel over the wire.

When this extension is active, both frame signing and challenge-response
auth use the same Ed25519 key material over different domain strings:
`NeoTape-frame\0` (format spec: [00-format-common.md](00-format-common.md))
and `NeoTape-auth\0` (this section).  The signatures are cryptographically
independent even with a shared key.

The Writer issues a fresh random nonce per connection, so replay of a
previous `auth_response` is impossible.  The exchange costs one round-trip
after TCP handshake (negligible on localhost or LAN).  Because the Writer
verifies `auth_response` with the same trusted public key it later uses for
per-frame signature checks, connection-time peer authentication and
frame-level authenticity point at the same trust anchor.

This design assumes the Archiver alone holds the Ed25519 secret key already
used for frame signing, while the Writer is provisioned only with the trusted
public key (e.g. via a local file or out-of-band distribution).  It
authenticates the Server to the Client (one-way).  It does **not** replace
per-frame signature verification; it only proves that the peer answering the
TCP connection possesses the expected signing key.

The reading pipeline does not need the same handshake.  The Reader client can
remain a simple frame forwarder, while the Extractor continues to perform the
authoritative frame-signature, sequence, and archive-continuity validation.

The implementation footprint is small: one nonce generator, one Ed25519 sign
operation, one Ed25519 verify operation, and no external PKI.
