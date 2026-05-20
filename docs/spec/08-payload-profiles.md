# Payload Profiles

Status: extracted from RFC_Draft.md §§14.1, 20; normative.

## Overview

NeoTape core is payload-format agnostic. A payload profile defines how the byte stream payload is interpreted on output, how it is produced on input, and any profile-specific rules for writer and reader behavior.

## NeoTape/PAX Profile (Recommended v0.1)

The NeoTape/PAX payload profile uses POSIX pax/tar as the payload byte stream.

### Writer Behavior

The writer produces a continuous pax byte stream using libarchive's pax writer. The byte stream preserves:

- Regular files, directories, symlinks, hardlinks
- Device nodes (block, char), FIFOs, sockets
- UID/GID names and numeric IDs
- File modes and timestamps
- Extended attributes (`xattrheader=ALL`) including security.capability
- Long paths (via pax extended headers)
- Hardlink relationships (via `archive_entry_linkresolver`)

The writer MAY split the continuous pax byte stream into NeoTape logical slices at pax member boundaries when practical, but NeoTape core does not require this. On-tape slices do not need to be independently valid pax archives and do not need slice-local pax EOA markers.

### Reader Behavior

The reader (`neotape-cat-volumes --payload-profile=pax`) emits concatenated pax bytes to stdout. The output is a valid pax stream that `bsdtar -xpf -` can restore.

If the last logical slice does not end with pax EOA (two zero-filled 512-byte blocks), a profile-aware output tool MAY append EOA before piping to bsdtar for clean compatibility. Core reader behaviour is to emit the concatenated payload bytes as-is.

### 512-Byte Alignment

The writer SHOULD preserve 512-byte tar record alignment when practical. This is a payload-profile rule, not a core framing requirement.

## Raw Profile

The `raw` payload profile treats the payload as an opaque byte stream with no profile-defined structure.

### Writer Behavior

The writer reads source bytes from a file or stdin and writes them as NeoTape payload. No format-specific processing is applied.

### Reader Behavior

The reader emits the concatenated payload bytes to stdout without any profile-specific transformation. This is useful for testing, diagnostics, and non-pax payloads.

## Source Reader Profiles

Payload profiles are orthogonal to source reader profiles. A writer MAY support multiple source reader strategies for the same payload profile:

### Multi-Threaded Small-File Reader

Multiple worker threads read small files in parallel. Suitable for HDD-backed filesystems with many small files where a single thread cannot saturate the LTO write rate.

### Single Sequential Reader

One file reader thread reads all files sequentially. Suitable for filesystems where a single read thread can saturate available I/O bandwidth.

### Hybrid

Large files use a sequential reader; small files use a bounded worker pool. The serializer merges results in the order determined by the planner.

### External Manifest

The writer reads an external manifest or file list to discover source files and metadata, then uses available information for slice packing decisions.

## Profile-Specific Rules

Each payload profile must define:

1. **stdout semantics** — what bytes `neotape-cat-volumes` emits for this profile.
2. **Source ingestion** — how the writer produces payload bytes for this profile.
3. **EOA/finalization** — whether the output stream needs a profile-specific end marker.
4. **Independent restorability** — whether individual slices can be independently restored by external tools.
5. **Slice boundary constraints** — whether slice boundaries must fall at profile-specific structure boundaries.

The NeoTape/PAX profile uses the rules in this document. Future profiles will document their own.
