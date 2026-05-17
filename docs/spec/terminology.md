# Terminology

Status: active terminology draft.

This document defines common NeoTape terms used by the active specification.
When terminology here conflicts with `docs/RFC_Draft.md`, this document wins.

## Logical Hierarchy

### Archive

A complete NeoTape backup set, identified by `archive_uuid` and optionally
named by `archive_name`.

An Archive may span one or more archive volumes.

### Archive Instance

A complete NeoTape backup instance identified by one `archive_uuid`.

An archive instance may occupy one or more physical media, and multiple archive
instances may be stored sequentially on one physical medium.

### Physical Medium

A sequentially writable physical storage medium initialized by NeoTape.

For LTO deployments, a physical medium is normally one LTO tape medium. A
physical medium begins with a Medium Header and may contain zero, one, or more
complete archive instances.

### Archive Volume

The part of an archive instance stored on one physical medium or virtual volume.

An archive volume has `volume_seq_num`, starting at 1 within an `archive_uuid`.
Its Volume Header is stored at the first archive tape file for that archive
volume.

### Tape File

A logical tape file delimited by LTO filemarks.

NeoTape uses tape files for coarse seekable boundaries such as the Medium
Header, Volume Header, logical slice tape files, and Archive End Header.
Frames are normally not independent tape files.

### Logical Slice

A writer-declared logical content byte range.

Each logical slice consists of one or more `SLICE_CONTENT` Frames and may be
followed by advisory `SLICE_METADATA` Frames. The final `SLICE_CONTENT` Frame
carries the `END` flag and records the authoritative `slice_content_size` and
`slice_content_blake3`.

### Frame

A fixed-size NeoTape transport record inside an archive volume.

Each Frame occupies exactly one NeoTape record of `volume_block_size` bytes. It
consists of a 1024-byte Frame Header followed by `frame_payload_size`
meaningful bytes and zero padding to the end of the record. Frames are chained
by explicit sequence numbers and length fields, not by parsing payload bytes.

### Slice Content

The ordered payload byte stream carried by `SLICE_CONTENT` Frames for one
logical slice.

Slice content is the only per-slice byte stream emitted by normal payload
readers such as `neotape-cat-volumes`.

### Slice Metadata

Advisory metadata bytes carried by `SLICE_METADATA` Frames for one logical
slice.

Slice metadata is transport metadata for listing, diagnostics, partial restore,
or acceleration. It is not part of the payload stream and must not be required
for basic restore correctness.

## Headers And Metadata

### Medium Header

The mandatory immutable header at the beginning of a physical medium.

The Medium Header records medium-level identity and initialization metadata,
then points to an ar metadata bundle. It is not a mutable table of contents and
MUST NOT record archive lists, free space, last write position, or other mutable
media state.

### Volume Header

The first archive-time header for an archive volume.

It identifies the archive instance, stores `archive_name`, declares
`volume_seq_num`, records `payload_profile`, and fixes `volume_block_size` for
all NeoTape records in that archive volume.

### Frame Header

The fixed header at the start of each Frame.

It records Frame sequencing, content type, meaningful byte count, Frame payload
hash, repeated archive identity fields such as `archive_name`, and for final
`SLICE_CONTENT` Frames the slice-level integrity fields.

### Archive End Header

The final clean archive-level header.

An archive is not cleanly complete unless a valid Archive End Header is found
and its clean-end semantics validate.

It repeats archive identity fields such as `archive_name` so the clean end
record remains human inspectable without auxiliary tools.

### Archive-Level Catalog

Optional advisory metadata associated with the whole archive.

Archive-level catalog metadata may be referenced by the Archive End Header. It
is separate from slice metadata and is not required for basic restore
correctness.

## Payload Terms

### Payload

The byte stream transported by NeoTape.

NeoTape core is payload-format agnostic. The payload may be a pax stream, raw
bytes, a filesystem-native send stream, or a future payload profile.

### Payload Profile

The rule set for interpreting payload bytes.

NeoTape/PAX is the initial recommended payload profile. Payload profile semantics
belong inside payload bytes or profile-specific tools, not in NeoTape core
framing.

### EOA

End of Archive marker inside a payload format.

For tar/pax, EOA is usually at least two 512-byte zero records. In NeoTape core,
EOA is payload-profile data only and is never used as a slice or Frame
boundary.

## Flags

### START

Frame flag indicating the first Frame of a content-type group within a logical
slice.

### END

Frame flag indicating the last Frame of a content-type group within a logical
slice.

The Frame Header carrying `END` with `frame_content_type = SLICE_CONTENT` also
carries the authoritative `slice_content_size` and `slice_content_blake3` for
that logical slice's content byte stream.
