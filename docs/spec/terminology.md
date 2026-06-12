# Terminology

Status: active terminology draft.

This document defines common NeoTape terms used by the active specification.

## Logical Hierarchy

### Archive

A complete NeoTape backup set, identified by `archive_uuid` and optionally
named by `archive_name`.

An Archive may span one or more archive volumes.

### Physical Medium

A sequentially writable physical storage medium used to store NeoTape archive
volumes.

For LTO deployments, a physical medium is normally one LTO tape medium. It may
contain zero, one, or more complete archive instances. NeoTape does not store a
medium-level descriptor in the archive stream; any non-NeoTape prefix before the
first Volume Header is ignored by readers.

### Archive Volume (or Volume for short)

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
readers such as `neotape restore`.

### Slice Metadata

Advisory metadata bytes carried by `SLICE_METADATA` Frames for one logical
slice.

Slice metadata is transport metadata for listing, diagnostics, partial restore,
or acceleration. It is not part of the payload stream and must not be required
for basic restore correctness.

## Headers And Metadata

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

## Frame Flags

### START

Frame flag indicating the first Frame of a content-type group within a logical
slice.

### END

Frame flag indicating the last Frame of a content-type group within a logical
slice.

The Frame Header carrying `END` also carries the authoritative
`slice_content_size` and `slice_content_blake3` for the current
`frame_content_type` group:

- For `SLICE_CONTENT` END — these fields describe the logical slice's content
  byte stream.
- For `SLICE_METADATA` END — these fields describe the concatenated metadata
  byte stream.

## Format Constructs

### NeoTape Record

A single `volume_block_size`-byte block written to the tape device or stored as
a record within a spool file.

The fundamental I/O unit after the Volume Header is committed. Every NeoTape
fixed header, every Frame, and every payload chunk fits in exactly one NeoTape
record.

### Fixed Header

The 1024-byte fixed field area at the start of every header (Volume, Frame,
Archive End).

All header types share this 1024-byte size. Every fixed header places its CRC32C
field in the final 4 bytes. The remaining 1020 bytes contain the common prefix,
type-specific fields, and reserved space.

### Common Header Prefix

The first 10 bytes shared by all NeoTape fixed headers:

| Field              | Size |
| ------------------ | ---- |
| `magic`          | 8    |
| `header_version` | 1    |
| `header_type`    | 1    |

A parser can always read 10 bytes, validate the magic, and dispatch by
`header_type`.

## Integrity

### frame_payload_blake3

BLAKE3 hash computed over exactly `frame_payload_size` bytes of a single
Frame's payload. Verified after reading the Frame payload.

### slice_content_blake3

BLAKE3 hash over exactly `slice_content_size` bytes of the current
`frame_content_type` group's concatenated payload, in Frame sequence order.

For `SLICE_CONTENT` END — computed over concatenated content payload bytes;
`SLICE_METADATA` Frame bytes are NOT included.

For `SLICE_METADATA` END — computed over concatenated metadata bytes;
`SLICE_CONTENT` Frame bytes are NOT included.

Carried by the END Frame Header of a content-type group. Zero on non-END
Frames.

### header_crc32c

CRC32C (Castagnoli) checksum covering all preceding bytes of the 1024-byte
fixed header area, excluding the CRC32C field itself. Used for accidental
corruption detection with minimal parser complexity.

### CRC32C Algorithm

NeoTape uses CRC32C with the Castagnoli polynomial `0x82F63B78`:

- Initial value: `0xFFFFFFFF`
- Final XOR: `0xFFFFFFFF`
- Input reflection: yes
- Result reflection: yes

## Encoding Rules

### Timestamp Format

All fixed-header timestamp fields use UTC, encoded as exactly 20 bytes:

```text
YYYY-MM-DDTHH:MM:SS\0
```

19 ASCII bytes matching `strftime("%Y-%m-%dT%H:%M:%S")` followed by one NUL
byte. No timezone suffixes, fractional seconds, or locale-specific text.

### nt_uuid

37-byte NUL-terminated UUID string per RFC 4122. Fixed size allows simple
offset-based header parsing.

### nt_name

256-byte fixed UTF-8 text field. NUL-terminated, NUL-padded. Maximum 255
usable characters.

### Empty Fixed-Field Encoding

When a writer does not produce a meaningful value for a field:

- Numeric fields: zero
- Fixed byte arrays: all zero bytes
- NUL-terminated strings: first byte NUL, remaining zero

CRC32C always includes every fixed field byte, including empty values.

### Requirement Keywords

In fixed header field tables, `MUST`, `SHOULD`, and `MAY` describe whether a
writer is required to produce a meaningful value. Every fixed field is always
present at its stable position; writers fill absent values with the empty
encoding.

## Block Size

### volume_block_size

The fixed NeoTape record size for an archive volume, declared in the Volume
Header. After commitment, all NeoTape records in that volume MUST use this
size.

### Block Size Constraints

| Constraint          | Value                   | Rationale                                               |
| ------------------- | ----------------------- | ------------------------------------------------------- |
| Minimum             | 4 KiB (4096 bytes)      | Below 4 KiB, header overhead dominates.                 |
| Recommended minimum | 64 KiB                  | At 4 KiB, the Frame Header consumes 25% of each record. |
| Maximum             | 8 MiB (8388608 bytes)   | LTO hardware record size limit.                         |

Non-power-of-2 block sizes are allowed by the format, but they are generally
not a good choice for physical media, including LTO tape.

## Header Field Terminology

### Archive-Time Identity Fields

Fields repeated across Volume, Frame, and Archive End headers:

| Field               | Type           | Description                                                    |
| ------------------- | -------------- | -------------------------------------------------------------- |
| `archive_uuid`    | `nt_uuid`    | Stable UUID for the archive instance.                          |
| `archive_name`    | `nt_name`    | Human-readable archive name. Not a unique key.                 |
| `volume_seq_num`  | `uint64`     | Volume sequence number within `archive_uuid`, starting at 1. |
| `payload_profile` | `uint8_enum` | Payload profile enum (e.g.`raw`, `pax`).                   |

### Volume Header Fields

| Field                   | Description                                        |
| ----------------------- | -------------------------------------------------- |
| `volume_block_size`   | Fixed NeoTape record size for this archive volume. |
| `volume_write_at_utc` | Volume write timestamp.                            |
| `flags`               | Reserved feature or compatibility flags.           |

### Frame Header Fields

| Field                          | Description                                                                                                     |
| ------------------------------ | --------------------------------------------------------------------------------------------------------------- |
| `logical_slice_seq_num`      | Logical slice sequence number within the archive.                                                               |
| `global_frame_seq_num`       | Frame sequence number scoped to the archive instance.                                                           |
| `frame_seq_num_within_slice` | Frame sequence number scoped to the logical slice.                                                              |
| `frame_payload_size`         | Meaningful bytes in this record after the 1024-byte header.                                                     |
| `frame_content_type`         | Content type:`SLICE_CONTENT` or `SLICE_METADATA`.                                                           |
| `frame_payload_blake3`       | BLAKE3 over exactly `frame_payload_size` bytes.                                                               |
| `slice_content_size`         | Slice-level size for the current `frame_content_type` group; valid only when END flag is set, otherwise zero. |
| `slice_content_blake3`       | BLAKE3 over the current group's concatenated bytes; valid only when END flag is set, otherwise zero.            |

### Archive End Header Fields

| Field                          | Description                                                |
| ------------------------------ | ---------------------------------------------------------- |
| `last_logical_slice_seq_num` | Last completed logical slice sequence number.              |
| `last_global_frame_seq_num`  | Last Frame sequence number scoped to the archive instance. |
| `created_by_implementation`  | Writer implementation name and version.                    |
| `created_by_build_id`        | Source revision, build ID, or other diagnostic identifier. |
| `archive_end_at_utc`         | Archive end timestamp.                                     |

## Enum Values

### HeaderType

| Value | Name            |
| ----- | --------------- |
| 1     | `volume`      |
| 2     | `frame`       |
| 3     | `archive_end` |

### PayloadProfile

| Value | Name                                                        |
| ----- | ----------------------------------------------------------- |
| 1     | `raw` — opaque byte stream, no profile-defined structure |
| 2     | `pax` — POSIX pax/tar byte stream (NeoTape/PAX profile)  |

### FrameContentType

| Value | Name                                                                     |
| ----- | ------------------------------------------------------------------------ |
| 1     | `SLICE_CONTENT` — opaque bytes belonging to the logical slice payload |
| 2     | `SLICE_METADATA` — advisory metadata bytes for the logical slice      |

### Archive End Header Flags

| Bit | Name                | Meaning                                                       |
| --- | ------------------- | ------------------------------------------------------------- |
| 0   | `CLEAN_END`       | Archive completed cleanly. MUST be 1 for a valid completion.  |
| 1   | `CATALOG_PRESENT` | Archive-level catalog metadata is present before this header. |

## Spool Concepts

### Spool Directory

A filesystem representation of one or more NeoTape archive virtual volumes.
Regular files and directories stand in for tape filemarks. Preserves the same
logical record order and volume transition rules as tape mode.

### tape-\<seq\> (Volume Directory)

A directory representing one NeoTape virtual volume. `<seq>` is a zero-padded
volume sequence number (e.g. `tape-000001`).

### tape-file-\<num\>.\<type\>.ntf

A regular file representing one NeoTape tape file. `<num>` is a zero-padded
tape-file index. `<type>` is one of `volume-header`, `slice-<seq>`,
`archive-end`. The `.ntf` extension stands for "NeoTape file".

### Manifest (manifest.json)

Advisory JSON file in a spool tape directory listing archives and their
constituent tape files. Restore correctness comes from NeoTape headers, not the
manifest.

### Virtual Tape Size

A configured capacity limit (`--virtual-tape-size`) that simulates EOT for
spool mode. When exceeded, the writer transitions to the next virtual volume.

## Common Acronyms

| Acronym  | Expansion            | Notes                                                                                 |
| -------- | -------------------- | ------------------------------------------------------------------------------------- |
| LTO      | Linear Tape-Open     | The tape format family targeted by NeoTape.                                           |
| BOT      | Beginning of Tape    | Physical start of a tape medium.                                                      |
| EOT      | End of Tape          | Physical end of writable medium region.                                               |
| EOD      | End of Data          | Logical end of valid data (may be before BOT on a blank tape).                        |
| filemark | —                   | LTO filemark delimited region; NeoTape uses filemarks for coarse seekable boundaries. |
| CRC32C   | CRC-32C (Castagnoli) | The specific CRC variant used for fixed header integrity.                             |
| BLAKE3   | —                   | The cryptographic hash used for payload and metadata integrity.                       |

## Relationship Rules

- An **Archive** contains one or more **Archive Volumes**.
- An **Archive Volume** is stored on one **Physical Medium** (or virtual volume).
- A **Physical Medium** may store multiple **Archive Instances** sequentially.
- An **Archive Volume** contains one or more **Logical Slices**.
- A **Logical Slice** contains one or more **Frames**.
- A **Frame** is exactly one **NeoTape Record**.
- **SLICE_METADATA** Frames follow the last **SLICE_CONTENT** Frame and precede the slice-level filemark.
- A normal payload reader emits only **SLICE_CONTENT** Frame payload bytes.
- Archive completion is declared only by a valid **Archive End Header** with `CLEAN_END` set.
