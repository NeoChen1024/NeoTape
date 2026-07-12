# Terminology

Status: normative.

This document defines common NeoTape terms used by the active specification.
Exact byte layouts and filesystem naming grammars live in their dedicated
chapters; this chapter is intentionally conceptual.

## Hierarchy

### Archive

A complete NeoTape backup set, identified by `archive_uuid` and optionally labeled by `archive_label`.

An Archive may span one or more backend volumes.

### Physical Medium

A sequentially writable physical storage medium used to store NeoTape archive volumes.

For LTO deployments, a physical medium is normally one LTO tape medium. It may contain zero, one, or more complete archive instances. NeoTape does not store a medium-level descriptor in the archive stream; any non-NeoTape prefix before the first NeoTape frame is ignored by readers.

### Backend Volume (or Volume for short)

A backend-defined physical or virtual container. It is not an authoritative logical record in the NeoTape stream. `volume_seq_num` is advisory and combines with `archive_label` to form an operator-facing display label (`${archive_label} #${volume_seq_num}`).

### Tape File

A logical tape file delimited by LTO filemarks.

NeoTape uses tape files for coarse seekable boundaries such as slice tape files and the Archive End frame. Frames within a slice tape file are chained by `frame_payload_size`, not by additional filemarks.

### Slice

A writer-declared content grouping, identified by `slice_seq_num`.

A slice is either a metadata-only slice containing one or more `ch_metadata`
frames, or a payload slice containing optional leading `ch_metadata` followed
by one or more `ch_content` frames and optional matching `ch_fec` groups.
At least one frame must be present. Metadata, when present, MUST precede all
`ch_content` and `ch_fec` frames. A slice MAY span backend volumes.

### Frame

A fixed-size NeoTape transport record inside an archive volume.

Each Frame occupies exactly one NeoTape record of `volume_block_size_kib * 1024` bytes. It consists of a 512-byte Frame Header followed by `frame_payload_size` meaningful bytes and zero padding to the end of the record. Frames are chained by explicit sequence numbers and length fields, not by parsing payload bytes.

### Channel

Partitions a slice into `ch_metadata`, `ch_content`, and optional `ch_fec`. `ch_metadata` remains a leading contiguous run when present. `ch_content` and `ch_fec` are identified by `channel_type` and may appear as repeated runs later in the slice. `channel_frame_seq_num` is scoped to `(slice_seq_num, channel_type)`, and `channel_frame_seq_num = 0` identifies the first frame of that channel in the slice.

### ch_content

The ordered payload byte stream carried by `ch_content` frames for one slice. This is the only per-slice byte stream emitted by normal payload readers such as `neotape restore`.

### ch_metadata

Advisory metadata bytes carried by `ch_metadata` frames for one slice. It is transport metadata for listing, diagnostics, partial restore, or acceleration. It is not part of the content stream and must not be required for basic restore correctness.

### ch_fec

Forward-error-correction repair bytes carried by `ch_fec` frames for one slice. `ch_fec` protects contiguous ranges of `ch_content` frames within the same slice and is advisory for normal restore mode: normal payload readers skip it and still emit only `ch_content`.

## Headers And Metadata

### Frame Header

The unified 512-byte fixed header at the start of every NeoTape frame. It records channel type, frame sequencing, payload size, archive identity, flags, and a BLAKE3 `frame_hash` covering the entire frame. A `signature` field holds a binary signify-style signature over the domain-separated message `NeoTape-frame\0 || frame_hash` when the `SIGNED` flag is set. There is only one header layout — no separate Volume Header or Archive End Header exists.

The exact field widths, field order, and byte positions are defined in
[02-frame-header.md](02-frame-header.md).

### Archive End Frame

The final clean archive-level frame, with `channel_type = archive_end`. An archive is not cleanly complete unless a valid Archive End frame is found with `CLEAN_END = 1`.

### Archive-Level Catalog

Optional advisory metadata associated with the whole archive. Archive-level catalog metadata may be carried in the optional payload of the Archive End frame. It is not required for basic restore correctness.

## Payload Terms

### Payload

The byte stream transported by NeoTape. NeoTape core is payload-format agnostic and does not enforce any payload profile.

## Frame Flags

### END

Frame flag indicating the last frame of a channel within a slice.

### SIGNED

Frame flag indicating that `signature` contains a binary signify-style signature over the domain-separated message `NeoTape-frame\0 || frame_hash`.

### SIDEBAND

Frame flag indicating that `sideband_data` contains meaningful
channel-type-specific data. In `header_version = 1`, it is required for
`ch_fec` and prohibited for `ch_content`, `ch_metadata`, and `archive_end`.

### FEC_PROTECTED

Frame flag valid only on `ch_content`. It identifies the frame as real
protected source material for an immediately following matching `ch_fec`
group. Exact group-size and repair rules are defined by the active FEC profile.

### CLEAN_END

Frame flag indicating a clean archive end. Only valid for `archive_end`
frames. Must be `1` on a valid end-of-archive frame.

Exact flag bit assignments are defined in [02-frame-header.md](02-frame-header.md).

## Format Constructs

### NeoTape Record

A single `volume_block_size_kib * 1024`-byte block written to the tape device or stored as a record within a spool file.

The fundamental I/O unit. Every frame header, every frame payload, and every padding chunk fits in exactly one NeoTape record.

### Fixed Header

The 512-byte fixed field area at the start of every frame.

The final 32 bytes of the header are `frame_hash`, a BLAKE3 digest over the canonical image of the entire frame. The 72-byte `signature` field holds an 8-byte key ID followed by a 64-byte Ed25519 signature over the domain-separated message `NeoTape-frame\0 || frame_hash` when the `SIGNED` flag is set.

### Common Header Prefix

The leading fields shared by all NeoTape frames: `magic`,
`header_version`, and `channel_type`.

A parser can always read the prefix, validate the magic, and dispatch by
`channel_type`. See [02-frame-header.md](02-frame-header.md) for the exact
prefix width and field sizes.

## Integrity

### frame_hash

BLAKE3 hash computed over the canonical image of the entire frame (`volume_block_size_kib * 1024` bytes). The canonical image treats `signature` and `frame_hash` as all-zero bytes. `frame_hash` is the final 32 bytes of the 512-byte header. See [docs/spec/00-format-common.md](00-format-common.md) for the full calculation rules.

### signature

72-byte field in the fixed header used when the `SIGNED` flag is set. Bytes 0-7
hold an opaque 8-byte key ID copied byte-for-byte from the signify-compatible
key file; it is not an integer and has no byte order. Bytes 8-71 hold a raw
64-byte Ed25519 signature over the domain-separated message
`NeoTape-frame\0 || frame_hash`. The context string includes its trailing NUL
byte. This mirrors OpenBSD signify's Ed25519 signature payload without the
leading two `Ed` bytes. When `SIGNED` is clear, the entire field must be zero.

## Encoding Rules

### Fixed Timestamp Format

Fields explicitly defined as using the NeoTape fixed timestamp encoding use
UTC, encoded as exactly 20 bytes:

```text
YYYY-MM-DDTHH:MM:SS\0
```

19 ASCII bytes matching `strftime("%Y-%m-%dT%H:%M:%S")` followed by one NUL
byte. No timezone suffixes, fractional seconds, or locale-specific text. This
rule does not apply to fields explicitly defined with another representation,
such as plan metadata `<mtime>`. The unified Frame Header does not contain
timestamp fields.

### nt_uuid

37-byte NUL-terminated UUID string per RFC 4122. Fixed size allows simple offset-based header parsing.

### nt_name

N-byte fixed UTF-8 text field. NUL-terminated, NUL-padded. Maximum (N-1) usable characters. `archive_label` uses a 65-byte field with at most 64 usable bytes.

### Empty Fixed-Field Encoding

When a writer does not produce a meaningful value for a field:

- Numeric fields: zero
- Fixed byte arrays: all zero bytes
- NUL-terminated strings: first byte NUL, remaining zero

### Requirement Keywords

In fixed header field tables, `MUST`, `SHOULD`, and `MAY` describe whether a writer is required to produce a meaningful value. Every fixed field is always present at its stable position; writers fill absent values with the empty encoding.

## Block Size

### volume_block_size_kib

The fixed NeoTape record size for an archive volume, encoded in KiB. The decoded record size is `volume_block_size_kib * 1024` bytes. This field is repeated in every frame.

### Block Size Constraints

| Constraint          | Value                        | Rationale                                                      |
| ------------------- | ---------------------------- | -------------------------------------------------------------- |
| Minimum             | 4 KiB (`value >= 4`)         | Below 4 KiB, header overhead dominates.                        |
| Recommended minimum | 64 KiB (`value >= 64`)       | At 4 KiB, the Frame Header consumes 12.5% of each record.      |
| Maximum             | 8 MiB (`value <= 8192`)      | LTO hardware record size limit.                                |

Non-power-of-2 block sizes are allowed by the format, but they are generally not a good choice for physical media, including LTO tape.

## Header Terminology

The exact on-wire header layout, field widths, channel enum values, and flag
bit assignments are defined only in [02-frame-header.md](02-frame-header.md).
This terminology chapter defines how those names are used conceptually and
intentionally avoids restating offsets or full field tables.

### Repeated Archive Identity Fields

Fields repeated across every frame:

| Field                   | Meaning                                                                    |
| ----------------------- | -------------------------------------------------------------------------- |
| `archive_uuid`          | Stable UUID for the archive instance.                                      |
| `archive_label`         | Human-readable archive label. Operator-facing, not a unique key.           |
| `volume_seq_num`        | Advisory volume sequence number, normally starting at 1 for a new archive. |
| `volume_block_size_kib` | Fixed NeoTape record size for this volume, encoded in KiB.                 |

### Channel and Flag Names

`channel_type` selects the role of the current frame (`ch_content`,
`ch_metadata`, `ch_fec`, or `archive_end`). `flags` carries per-frame state:
`END`, `SIGNED`, `SIDEBAND`, `FEC_PROTECTED`, and `CLEAN_END`.

For the authoritative enum values and bit assignments, see
[02-frame-header.md](02-frame-header.md).

## Spool Concepts

### Spool Directory

A filesystem representation of one or more NeoTape archive virtual volumes.
Regular files stand in for tape files, preserving the same logical record order
and frame semantics as tape mode.

The authoritative concrete directory layout, filename grammar, and reader model
for spool mode are defined in [07-spool-dir.md](07-spool-dir.md).

### Spool File

A regular file inside a spool directory that stands in for one NeoTape tape
file. In spool mode, a file boundary is the equivalent of a tape filemark.

### Virtual Tape

The conceptual model of treating a spool directory as an ordered, tape-like
sequence of files. Ordering, archive boundaries, and record framing follow
[07-spool-dir.md](07-spool-dir.md).

## Common Acronyms

| Acronym  | Expansion            | Notes                                                                                     |
| -------- | -------------------- | ----------------------------------------------------------------------------------------- |
| LTO      | Linear Tape-Open     | The tape format family targeted by NeoTape.                                                |
| BOT      | Beginning of Tape    | Physical start of a tape medium.                                                           |
| EOT      | End of Tape          | Physical end of writable medium region.                                                    |
| EOD      | End of Data          | Logical end of valid data.                                                                 |
| filemark | —                    | LTO filemark delimited region; NeoTape uses filemarks for coarse seekable boundaries.      |
| BLAKE3   | —                    | The cryptographic hash used for frame integrity.                                            |

## Relationship Rules

- An **Archive** contains one or more **Backend Volumes**.
- A **Backend Volume** is stored on one **Physical Medium** (or virtual volume).
- A **Physical Medium** may store multiple **Archive Instances** sequentially.
- A **Backend Volume** contains one or more **Slices**.
- A **Slice** contains at most one contiguous leading `ch_metadata` run. After metadata, `ch_content` and `ch_fec` MAY appear as repeated runs within the same slice. At least one frame must be present.
- A **Frame** is exactly one **NeoTape Record**.
- `ch_metadata` frames MUST precede both `ch_content` and `ch_fec` within a slice.
- A normal payload reader emits only `ch_content` frame payload bytes.
- Archive completion is declared only by a valid **Archive End** frame with `CLEAN_END = 1`.
