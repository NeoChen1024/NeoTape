# Volume Layout

Status: specification.

## Scope

This document describes the logical nesting and physical tape layout of
NeoTape archives: how archives, volumes, logical slices, and Frames map to
LTO tape files, filemarks, and spool directory entries.

For header field definitions see the individual header specs:

- Volume Header → [01-volume-header.md](01-volume-header.md)
- Frame Header → [02-frame-header.md](02-frame-header.md)
- Archive End Header → [03-archive-end-header.md](03-archive-end-header.md)
- Spool directory layout → [04-spool-dir.md](04-spool-dir.md)
- Terminology → [terminology.md](terminology.md)

## Logical Nesting

```
Archive (identified by archive_uuid)
  └── Volume[1] (volume_seq_num=1)
  │     ├── Volume Header
  │     ├── Logical Slice[1]
  │     │     ├── Frame[1]   (START flag)
  │     │     ├── Frame[2]
  │     │     ├── ...
  │     │     └── Frame[N]   (END flag, slice-level BLAKE3)
  │     ├── Logical Slice[2]
  │     └── ...
  ├── Volume[2] (continuation)
  │     ├── Volume Header
  │     ├── Logical Slice[k] (continued from Volume[1])
  │     └── ...
  └── Archive End Header
```

## Physical Nesting

How the logical concepts map to physical tape constructs:

```
Physical Medium (LTO tape)
  └── (optional recovery bundle tape file, not NeoTape format)
  └── filemark
  └── Archive Volume (part of one archive on one medium)
  │     ├── Volume Header (tape file, single NeoTape record)
  │     ├── filemark
  │     ├── Logical Slice (tape file, one or more NeoTape records)
  │     │     ├── Frame (NeoTape record, volume_block_size bytes, is one tape block)
  │     │     │     ├── Frame Header (1024 bytes)
  │     │     │     ├── payload bytes (frame_payload_size)
  │     │     │     └── zero padding
  │     │     ├── Frame (NeoTape record)
  │     │     ├── ...
  │     │     └── Frame (END flag, slice-level BLAKE3)
  │     ├── filemark
  │     ├── Logical Slice (tape file)
  │     ├── filemark
  │     ├── ...
  │     └── Archive End Header (tape file, single NeoTape record)
  └── filemark
  └── (next archive instance, if capacity remains)
```

- **Physical Medium** → a sequential storage medium holding one or more archive
  volumes. NeoTape does not store a medium-level descriptor; any non-NeoTape
  prefix before the first Volume Header is ignored by readers.
- **Tape file** → LTO filemark-delimited region. NeoTape uses tape files for
  Volume Headers, logical slices, and Archive End Headers.
- **NeoTape record** → a single `volume_block_size`-byte block written to the
  tape device or stored as a record within a spool file.
- **Frame** → exactly one NeoTape record. Frames within a slice tape file are
  chained by `frame_payload_size`, not by filemarks.

## Single-Volume Tape Layout (Normal Case)

A single-volume archive on one physical tape:

```
File 0:   Volume Header (archive_uuid, volume_seq_num=1)
filemark
File 1:   Logical Slice 1 tape file
          ┌─ Frame Header (START, slice=1, frame=1)
          ├─ payload bytes
          ├─ Frame Header (slice=1, frame=2)
          ├─ payload bytes
          ├─ ...
          ├─ Frame Header (END, slice=1, frame=N, slice_content_size, slice_content_blake3)
          └─ payload bytes
filemark
File 2:   Logical Slice 2 tape file
          ...
filemark
File N:   Archive End Header (clean_end, last_slice_seq_num)
filemark
```

Each filemark-delimited region is a NeoTape tape file. Within a slice tape
file, Frames are located by `frame_payload_size`, not by additional filemarks.

A slice tape file may contain two Frame groups:

1. **SLICE_CONTENT** — one or more Frames carrying the logical slice's
   payload bytes. The final SLICE_CONTENT Frame carries the `END` flag and
   records the authoritative `slice_content_size` and `slice_content_blake3`.
2. **SLICE_METADATA** — zero or more advisory Frames following the final
   SLICE_CONTENT Frame inside the same tape file. These carry per-slice
   metadata such as catalog entries or diagnostics. SLICE_METADATA has its
   own slice-level BLAKE3 integrity hash recorded in its END Frame Header,
   separate from the SLICE_CONTENT hash. A reader MUST NOT require
   SLICE_METADATA for basic restore correctness.

The format design permits interleaving different frame types, but this is
not currently implemented in normal slice handling. Such interleaving will
be investigated further in the future.

The first NeoTape record in a normal archive stream is a Volume Header. On
physical tape an optional recovery bundle (for example a plain pax tar file)
MAY precede the Volume Header; readers locate the Volume Header by scanning
forward for the NeoTape magic.

## Multi-Volume Tape Layout

When an archive spans multiple physical tapes:

**Tape 1:**

```
File 0:   Volume Header (volume_seq_num=1)
filemark
File 1:   Logical Slice 1 tape file (complete, END flag)
filemark
File 2:   Logical Slice 2 tape file (payload starts but EOT before END)
~~EOT~~
```

**Tape 2:**

```
File 0:   Volume Header (volume_seq_num=2)
filemark
File 1:   Logical Slice 2 tape file (continuation, END flag)
filemark
File 2:   Logical Slice 3 tape file (END flag)
filemark
File 3:   Archive End Header
filemark
```

If EOT interrupts a slice tape file, the next volume begins with a new Volume
Header. The incomplete logical slice is resumed in a new tape file on the next
volume, starting with the next uncommitted Frame. Only Frames whose complete
Frame Header + frame_payload_size bytes were fully committed before EOT are
considered present.

## Multiple Archives on One Tape

A physical tape may hold several complete archives sequentially:

```
 ... previous archive completed ...
filemark
File M:   Volume Header (new archive_uuid, volume_seq_num=1)
filemark
File M+1: Logical Slice 1 tape file
filemark
...
filemark
File P:   Archive End Header
filemark
(size capacity remaining)
File P+1: Volume Header (next archive_uuid, volume_seq_num=1)
filemark
...
```

Each archive instance is independent with its own `archive_uuid`,
`volume_seq_num` restarting at 1, and Archive End Header. A reader locates
archive instances by scanning forward through tape files, validating Volume
Headers, and matching the expected or interactive `archive_uuid`.

## Frame Model

A logical slice consists of one or more Frames:

```
LogicalSlice[k] = Frame[k,1].payload + Frame[k,2].payload + ... + Frame[k,M].payload
```

Each Frame occupies exactly one NeoTape record (`volume_block_size` bytes):

```
  ┌─ 1024-byte Frame Header ─┬─ frame_payload_size payload bytes ─┬─ padding ─┐
  └──────────────────────────┴────────────────────────────────────┴───────────┘
  <-------------------------- volume_block_size ------------------------------>
```

The Frame Header declares `frame_payload_size`. The reader reads exactly that
many payload bytes; the remaining bytes in the record are zero padding.
Frames within a slice tape file are located by `frame_payload_size`, not by
filemarks. Slice-level filemark is written only after the final Frame (with
`END` flag) is committed.

Frame Header flags:

- `START` — this Frame begins a new content-type group (e.g. SLICE_CONTENT,
  SLICE_METADATA).
- `END` — this Frame ends a content-type group. When combined with
  `frame_content_type = SLICE_CONTENT`, the Frame Header also carries
  `slice_content_size` and `slice_content_blake3` for the entire logical
  slice's concatenated SLICE_CONTENT payload. When combined with
  `frame_content_type = SLICE_METADATA`, the same fields carry the size and
  BLAKE3 hash of the concatenated SLICE_METADATA payload.

## EOT Continuation Rules

When the writer encounters EOT (physical end of tape) or reaches the configured
virtual tape capacity limit:

1. **Frame Header not yet committed:** The Frame is considered not created.
   The next volume writes the same Frame Header fresh.
2. **Frame Header committed, payload partially written:** The Frame is
   incomplete. The next volume opens with a Volume Header, then the same
   logical slice continues with the next uncommitted Frame covering the
   remaining payload range.
3. **END Frame Header committed, slice-level filemark not yet written:** The
   logical slice is complete. The next volume opens with a Volume Header and
   proceeds to the next logical slice or Archive End Header.
4. **Archive End Header not yet committed:** The archive is not cleanly
   complete. The next volume completes remaining logical slices, then writes
   the Archive End Header.
