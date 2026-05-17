# Terminology

Status: active terminology draft.

This document defines common NeoTape terms used by the active specification.
When terminology here conflicts with `docs/RFC_Draft.md`, this document wins.

## Logical Hierarchy

### Archive

A complete NeoTape backup set, identified by `archive_uuid`.

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
Physical segments are normally not independent tape files.

### Logical Slice

A writer-declared logical payload byte range.

Each logical slice consists of one or more physical segments. The final PAYLOAD
segment carries the `SLICE_END` flag and records the authoritative
`slice_payload_size` and `slice_payload_blake3`.

### Physical Segment

A length-framed segment inside a logical slice tape file.

A physical segment consists of a 1024-byte Segment Header followed by exactly
the segment payload bytes described by that header. Segments are chained by
explicit length fields, not by parsing payload bytes.

### Continuation Segment

A segment that continues data after EOT / ENOSPC or a virtual volume limit.

Continuation does not create a new logical slice. It resumes the same segment or
logical slice according to the continuation fields in the Segment Header.

## Headers And Metadata

### Medium Header

The mandatory immutable header at the beginning of a physical medium.

The Medium Header records medium-level identity and initialization metadata,
then points to an ar metadata bundle. It is not a mutable table of contents and
MUST NOT record archive lists, free space, last write position, or other mutable
media state.

### Volume Header

The first archive-time header for an archive volume.

It identifies the archive instance, declares `volume_seq_num`, and fixes
`volume_block_size` for all NeoTape records in that archive volume.

### Segment Header

The fixed header at the start of each physical segment.

It records segment length, sequencing, content type, optional continuation
state, optional segment payload hash, and for `SLICE_END` segments the
slice-level integrity fields.

### Archive End Header

The final clean archive-level header.

An archive is not cleanly complete unless a valid Archive End Header is found
and its clean-end semantics validate.

### TRAILER_METADATA Segment

A Segment Header with `segment_content_type = TRAILER_METADATA`.

TRAILER_METADATA segments are advisory metadata segments that may follow the
last PAYLOAD segment of a logical slice. They are not payload, are not emitted
to stdout, and must not be required for basic restore correctness.

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
EOA is payload-profile data only and is never used as a slice or segment
boundary.

## Flags

### SLICE_START

Segment flag indicating that the segment starts a logical slice.

### SLICE_CONTINUATION

Segment flag indicating that the segment continues an existing logical slice.

### SLICE_END

Segment flag indicating that the writer declares the logical slice complete.

The Segment Header carrying `SLICE_END` also carries the authoritative
`slice_payload_size` and `slice_payload_blake3` for that logical slice.
