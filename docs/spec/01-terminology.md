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

NeoTape uses tape files for coarse seekable boundaries such as slice tape files and the Archive End frame. Frames within a tape file are delimited by backend record boundaries or the decoded record size. `frame_payload_size` identifies meaningful payload bytes and does not locate the next frame.

### Slice

A writer-declared content grouping, identified by `slice_seq_num`.

A slice carries at most one `ch_metadata` stream and at most one `ch_content`
stream. At least one frame must be present. Frames of the two channels MAY
appear in any order and MAY be interleaved. A slice MAY span backend volumes.

### Frame

A fixed-size NeoTape transport record inside an archive volume.

Each Frame occupies exactly one NeoTape record of `volume_block_size_kib * 1024` bytes. It consists of a 512-byte Frame Header followed by `frame_payload_size` meaningful bytes and zero padding to the end of the record. Frames are chained by explicit sequence numbers and length fields, not by parsing payload bytes.

### Channel

Partitions a slice into `ch_metadata` and `ch_content`. Frames of different channels may be interleaved in any order. `channel_frame_seq_num` is scoped to `(slice_seq_num, channel_type)`, and `channel_frame_seq_num = 0` identifies the first frame of that channel in the slice.

### ch_content

The ordered payload byte stream carried by `ch_content` frames for one slice. This is the only per-slice byte stream emitted by normal payload readers such as `neotape-extractor`.

### ch_metadata

Advisory metadata bytes carried by the `ch_metadata` frames of one slice. Its records may describe other slices; see [Archive Catalog](#archive-catalog). It is transport metadata for listing, diagnostics, partial restore, or acceleration. It is not part of the content stream and must not be required for basic restore correctness.

## Headers And Metadata

### Frame Header

The unified 512-byte fixed header at the start of every NeoTape frame. It records channel type, frame sequencing, payload size, archive identity, flags, an optional signature, and a `frame_hash` covering the entire frame. There is only one header layout — no separate Volume Header or Archive End Header exists.

The exact field widths, field order, and byte positions are defined in
[02-frame-header.md](02-frame-header.md).

### Archive End Frame

The final clean archive-level frame, with `channel_type = archive_end`. An archive is not cleanly complete unless a valid Archive End frame is found with `CLEAN_END = 1`.

### Archive Catalog

Optional advisory metadata listing the planned contents of the whole archive: the plan file, carried as the leading `ch_metadata` run of slice 0 (see [09-plan-metadata.md](09-plan-metadata.md)). It is not required for basic restore correctness. The Archive End frame carries no payload.

## Payload Terms

### Payload

The byte stream transported by NeoTape. NeoTape core is payload-format agnostic and does not enforce any payload profile.

## Frame Flags

### END

Frame flag indicating the last frame of a channel within a slice.

### SIGNED

Frame flag indicating that the `signature` field holds a signature of this frame.

### SIDEBAND

Frame flag indicating that `sideband_data` contains meaningful
channel-type-specific data. No `header_version = 1` channel defines sideband
data, so the flag is prohibited on every frame.

### CLEAN_END

Frame flag indicating a clean archive end. Only valid for `archive_end`
frames. Must be `1` on a valid end-of-archive frame.

Exact flag bit assignments are defined in [02-frame-header.md](02-frame-header.md).

## Format Constructs

### NeoTape Record

A single `volume_block_size_kib * 1024`-byte block written to the tape device or stored as a record within a spool file.

The fundamental I/O unit. Every frame header, every frame payload, and every padding chunk fits in exactly one NeoTape record.

### Fixed Header

The 512-byte fixed field area at the start of every frame; see
[02-frame-header.md](02-frame-header.md).

### Common Header Prefix

The leading fields shared by all NeoTape frames: `magic`,
`header_version`, and `channel_type`.

A parser can always read the prefix, validate the magic, and dispatch by
`channel_type`. See [02-frame-header.md](02-frame-header.md) for the exact
prefix width and field sizes.

## Integrity

### frame_hash

BLAKE3 hash over the canonical image of the entire frame, stored in the final
32 bytes of the header. The calculation is defined in
[00-format-common.md](00-format-common.md#frame-hash-calculation).

### signature

72-byte header field holding a key ID and an Ed25519 signature over the
domain-separated `frame_hash` when `SIGNED` is set, and all zero otherwise.
The encoding and signing sequence are defined in
[00-format-common.md](00-format-common.md#signing-sequence).

## Encoding and Block Size

Datatypes (`nt_uuid`, `nt_name`, …), empty-field encoding, requirement
keywords, and block-size limits are defined in
[00-format-common.md](00-format-common.md). Field widths, channel values, and
flag bits are defined in [02-frame-header.md](02-frame-header.md).

### volume_block_size_kib

The fixed NeoTape record size for an entire archive, including all its volumes, encoded in KiB. The decoded record size is `volume_block_size_kib * 1024` bytes. This field is repeated in every frame.

### Repeated Archive Identity Fields

`archive_uuid`, `archive_label`, `volume_seq_num`, and `volume_block_size_kib`
are repeated in every frame, so any frame identifies its archive on its own.
`archive_uuid` is the authoritative identity; `archive_label` and
`volume_seq_num` are operator-facing.

## Spool Concepts

### Spool Directory

A filesystem representation of one or more NeoTape archive virtual volumes.
Regular files stand in for tape files, preserving the same logical record order
and frame semantics as tape mode.

The authoritative concrete directory layout, filename grammar, and reader model
for spool mode are defined in [06-spool-dir.md](06-spool-dir.md).

### Spool File

A regular file inside a spool directory that stands in for one NeoTape tape
file. In spool mode, a file boundary is the equivalent of a tape filemark.

### Virtual Tape

The conceptual model of treating a spool directory as an ordered, tape-like
sequence of files. Ordering, archive boundaries, and record framing follow
[06-spool-dir.md](06-spool-dir.md).

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
- A **Backend Volume** stores complete slices or portions of slices, and may contain only an Archive End frame. A slice may span volumes.
- A **Slice** contains at most one stream per channel, in any order and possibly interleaved. At least one frame must be present.
- A **Frame** is exactly one **NeoTape Record**.
- A normal payload reader emits only `ch_content` frame payload bytes.
- Archive completion is declared only by a valid **Archive End** frame with `CLEAN_END = 1`.
