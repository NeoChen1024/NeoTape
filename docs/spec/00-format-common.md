# Common Format Rules

Status: normative.

This document defines common rules and conventions shared across the unified
NeoTape frame format. The fixed header layout and field semantics are defined in
[docs/spec/02-frame-header.md](02-frame-header.md).

## Datatype Reference

All fixed header field tables in this specification use the following datatypes:

| Datatype       | Description                                                                           |
| -------------- | ------------------------------------------------------------------------------------- |
| `uint8`        | Unsigned 8-bit integer.                                                               |
| `uint8_enum`   | Unsigned 8-bit integer with enumerated values defined per field.                      |
| `uint16`       | Unsigned 16-bit integer, little-endian.                                               |
| `uint64`       | Unsigned 64-bit integer, little-endian.                                               |
| `char[N]`      | Fixed N-byte character array. NUL-terminated or NUL-padded per field rules.           |
| `byte[N]`      | Fixed N-byte raw binary array.                                                        |
| `byte[*]`      | Auto-calculated zero padding. Expands to the number of zero bytes needed to fill the 512-byte fixed header area. Writers MUST write zero; readers MUST include all bytes in `frame_hash`. |
| `nt_name`      | Fixed N-byte UTF-8 text field. NUL-terminated, NUL-padded after the terminator, required to end with NUL. |
| `nt_uuid`      | NUL-terminated UUID string per RFC 4122. 37 bytes.                                    |
| `nt_hash`      | BLAKE3 256-bit hash. 32 bytes.                                                        |

## Magic Value

All NeoTape frames MUST use the same 8-byte magic value:

```text
NeoTape\0
```

Frame semantics are distinguished by the `channel_type` field, not by a different magic value.

## Field Packing

All fields in the NeoTape fixed header MUST be packed contiguously — no padding, alignment, or gap bytes between fields. This is equivalent to a C or C++ packed struct (`__attribute__((packed))`).

Field sizes in the table are exact and cumulative: the byte offset of a field equals the sum of all preceding field sizes.

## Common Header Prefix

Every NeoTape frame MUST begin with the same three fields in this exact order:

| Field          | datatype   | size (in bytes) |
| -------------- | ---------- | --------------- |
| magic          | char[8]    | 8               |
| header_version | uint8      | 1               |
| channel_type   | uint8_enum | 1               |

A parser can always read the first 10 bytes, validate the magic value, determine the layout version, and dispatch reader logic by channel type.

## Repeated Archive Identity Fields

Every frame repeats `archive_uuid`, `archive_label`, `volume_seq_num`, and `volume_block_size_kib`. `archive_uuid` is the authoritative machine identifier. `archive_label` is a human-readable hint and MUST NOT be used as a unique key.

## Header Position Rule

Every NeoTape fixed header MUST begin at byte 0 of its containing NeoTape record. No fixed header may start at a non-zero offset within a record.

## Fixed Header Size

Every NeoTape fixed header field area MUST occupy exactly 512 bytes.

The final 32 bytes of the header are `frame_hash`, a BLAKE3 digest over the canonical image of the entire frame. See [Frame Hash Calculation](#frame-hash-calculation) below for the exact rules.

## Frame Hash Calculation

`frame_hash` covers exactly `volume_block_size_kib * 1024` bytes: the 512-byte fixed header, `frame_payload_size` bytes of payload, and all trailing padding bytes through the end of the frame.

```text
frame_hash = BLAKE3(canonical_image)
```

where `canonical_image` is the full `volume_block_size_kib * 1024`-byte frame with two fields treated as all-zero bytes:

- `signature` (72 bytes)
- `frame_hash` (32 bytes)

All other fixed header fields, payload bytes, and padding bytes are included exactly as stored.

Padding bytes after `frame_payload_size` and before the end of the decoded record size MUST be zero-filled by writers. Readers include padding bytes in `frame_hash` verification.

## Signing Sequence

The `signature` field (72 bytes) holds a binary, unarmored Ed25519
signature payload when the `SIGNED` flag is set.  Bytes 0-7 hold a 64-bit
key ID.  Bytes 8-71 hold the raw 64-byte Ed25519 signature over the
domain-separated message `NeoTape-frame\0 || frame_hash` (see
[Format Write Order](#format-write-order) step 4).  The domain string
includes its trailing NUL byte and is followed immediately by the 32 raw
bytes of `frame_hash`.  This mirrors OpenBSD
signify's Ed25519 signature payload without the leading two `Ed` bytes.
When `SIGNED` is clear, writers MUST write the entire `signature` field
as zero and readers MUST ignore it.

The writer-side sequence is:

1. Fill the final header fields, including the final `SIGNED` flag value.
2. Write payload bytes and zero-fill all padding bytes through the decoded record size.
3. Compute `frame_hash` over the canonical image (see above).
4. If `SIGNED` is set, produce an Ed25519 signature over the
   domain-separated message `NeoTape-frame\0 || frame_hash` and write
   the 8-byte key ID followed by the 64-byte Ed25519 signature into
   `signature`.
5. Write `frame_hash` into the final 32 bytes of the header.

## Data Continuation Rule

Payload bytes MUST begin in the same NeoTape record immediately after the 512-byte fixed field area, without padding or alignment gap.

Any bytes after `frame_payload_size` and before the end of the decoded record size are zero padding. Writers MUST write this padding as zero. Readers include padding bytes in `frame_hash` verification.

## Block Size Constraints

### Minimum

The NeoTape record block size MUST be at least 4 KiB (4096 bytes), encoded as `volume_block_size_kib >= 4`.

A writer SHOULD use at least 64 KiB (65536 bytes) in practice (`volume_block_size_kib >= 64`). Below 64 KiB the Frame Header overhead (512 bytes per record) becomes significant: at 4 KiB the header consumes 12.5% of each record.

### Maximum

The NeoTape record block size MUST NOT exceed 8 MiB (8388608 bytes), encoded as `volume_block_size_kib <= 8192`. A reader SHOULD reject larger values as unsupported.

### Shape

The format MAY use non-power-of-2 block sizes. In practice, that is usually a poor choice for any physical medium, especially LTO tape. Writers SHOULD prefer power-of-2 block sizes unless they have a concrete medium-specific reason not to.

### Scope

`volume_block_size_kib` is repeated in every frame in the archive volume. The decoded record size is `volume_block_size_kib * 1024` bytes.

## Requirement Keywords

For fixed header field tables, `MUST`, `SHOULD`, and `MAY` describe whether a writer is required or expected to produce a meaningful value. They do not make the field itself optional.

Every fixed field listed in the header table has a stable position and fixed encoded size. Writers MUST NOT omit fields from the encoded header. If a writer does not produce a meaningful value for a `SHOULD` or `MAY` field, it MUST write the field's empty value instead.

Empty fixed-field values are encoded as follows:

- Numeric fields: zero.
- Fixed byte arrays: all zero bytes.
- NUL-terminated string fields: first byte NUL, remaining bytes zero.
- `nt_name` fields: first byte NUL, remaining bytes zero.

`frame_hash` calculations over fixed fields MUST include every fixed field byte, including empty values and reserved fields. `signature` and `frame_hash` are treated as zero for hash calculation.

## Timestamp Format

NeoTape timestamp fields (for example in plan metadata or spool manifests) MUST use UTC and MUST be encoded as a 20-byte NUL-terminated string.

The timestamp text before the NUL byte MUST match this exact `strftime` format:

```text
%Y-%m-%dT%H:%M:%S
```

This is exactly 19 ASCII bytes followed by one NUL byte:

```text
YYYY-MM-DDTHH:MM:SS\0
```

Writers MUST NOT use timezone suffixes, numeric offsets, fractional seconds, locale-specific text, RFC 3339 variants, ISO 8601 variants, or any other date format. The unified Frame Header defined in [docs/spec/02-frame-header.md](02-frame-header.md) does not contain timestamp fields.
