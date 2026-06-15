# Volume Layout

Status: normative.

## Scope

This document describes the logical nesting and physical tape layout of NeoTape archives: how archives, volumes, logical slices, channels, and frames map to LTO tape files, filemarks, and spool directory entries.

For the fixed header layout and channel type definitions, see [docs/spec/01-frame-header.md](01-frame-header.md).

## Logical Hierarchy

```
Archive (archive_uuid)
  └── Volume (backend-defined physical/virtual volume)
      └── Logical Slice (logical_slice_seq_num)
          └── Channel (channel_type: ch_metadata / ch_content)
              └── Frame (global_frame_seq_num, frame_seq_num_within_channel)
```

- **Archive** is the authoritative logical backup instance, identified by `archive_uuid`.
- **Volume** is a backend-defined physical or virtual container. It is not an authoritative logical record in the NeoTape stream. `volume_seq_num` is advisory and combines with `archive_label` to form an operator-facing display label.
- **Logical Slice** is the unit of logical ordering within an archive. It is identified by `logical_slice_seq_num` and may span frames and backend volumes.
- **Channel** partitions a logical slice into `ch_metadata` and `ch_content`. Metadata, when present, precedes content. A slice may contain only metadata.
- **Frame** is the concrete transport record. Every frame has exactly one `channel_type`, one `global_frame_seq_num`, and one `frame_seq_num_within_channel`.

## Physical Nesting

```
Physical Medium (LTO tape)
  └── (optional recovery bundle tape file, not NeoTape format)
  └── [optional filemark]
  └── Archive Volume (part of one archive on one medium)
  │     ├── Logical Slice (tape file, one or more NeoTape records)
  │     │     ├── ch_metadata Frame (optional, NeoTape record, volume_block_size_kib * 1024 bytes)
  │     │     │     ├── 512-byte Frame Header
  │     │     │     ├── payload bytes
  │     │     │     └── zero padding
  │     │     ├── ch_content Frame (NeoTape record)
  │     │     ├── ...
  │     │     └── ch_content Frame (END flag)
  │     ├── filemark
  │     ├── Logical Slice (tape file)
  │     ├── filemark
  │     ├── ...
  │     └── Archive End frame (tape file, single NeoTape record)
  └── filemark
  └── (next archive instance, if capacity remains)
```

- **Physical Medium** — a sequential storage medium holding one or more archive volumes. NeoTape does not store a medium-level descriptor; any non-NeoTape prefix before the first NeoTape frame is ignored by readers.
- **Tape file** — LTO filemark-delimited region. NeoTape uses tape files for logical slices and the Archive End frame.
- **NeoTape record** — a single `volume_block_size_kib * 1024`-byte block written to the tape device or stored as a record within a spool file.
- **Frame** — exactly one NeoTape record. Frames within a slice tape file are chained by `frame_payload_size`, not by filemarks.
- **Volume boundary** — a physical/operator event, identified by a filemark and detected by `volume_seq_num` change (advisory) or sequence continuity checks (authoritative).

## No Volume Header

There is no dedicated Volume Header tape file. The first NeoTape record on a new volume is the first frame of the first logical slice (or an Archive End frame if the volume contains only the end marker). Every frame repeats `volume_block_size_kib`, `archive_uuid`, `archive_label`, and `volume_seq_num`, so a reader can bootstrap from any frame.

Within a backend-defined physical or virtual volume, all NeoTape frames SHOULD carry the same `volume_seq_num`. A reader MAY warn if `volume_seq_num` changes unexpectedly within the same backend volume.

## Single-Volume Tape Layout (Normal Case)

```
[optional recovery bundle]
[optional filemark]
File 0:   Logical Slice 1 tape file
          ├─ ch_metadata Frame (START, slice=1, channel=within=1)
          │  (optional)
          ├─ ch_content Frame (START, slice=1, channel=within=1)
          ├─ ch_content Frame (slice=1, channel=within=2)
          ├─ ...
          └─ ch_content Frame (END, slice=1, channel=within=N)
filemark
File 1:   Logical Slice 2 tape file
          ...
filemark
File N:   Archive End frame (START, END, CLEAN_END)
filemark
```

Within a slice tape file, frames are located by `frame_payload_size`, not by additional filemarks. Metadata frames, when present, precede content frames. A logical slice MUST contain at least one frame across its channels.

## Multi-Volume Tape Layout

When an archive spans multiple physical tapes:

**Tape 1:**

```
Logical Slice 1 tape file (complete)
filemark
Logical Slice 2 tape file (payload starts but EOT before END)
~~EOT~~
```

**Tape 2:**

```
Logical Slice 2 tape file (continuation, END)
filemark
Logical Slice 3 tape file (END)
filemark
Archive End frame
filemark
```

If EOT interrupts a slice tape file, the next volume continues the interrupted logical slice in a new tape file, starting with the next uncommitted frame. Only frames whose complete fixed header and payload bytes were fully committed before EOT are considered present.

A logical slice MAY span backend volumes. `global_frame_seq_num`, `logical_slice_seq_num`, and `frame_seq_num_within_channel` continuity do not reset at a backend volume boundary.

## Multiple Archives on One Tape

A physical tape may hold several complete archives sequentially:

```
... previous archive completed ...
filemark
Logical Slice 1 tape file
filemark
...
filemark
Archive End frame
filemark
(some capacity remaining)
Logical Slice 1 tape file (next archive_uuid)
filemark
...
```

Each archive instance is independent with its own `archive_uuid`. A reader locates archive instances by scanning forward through tape files, validating frames, and matching the expected or interactive `archive_uuid`.

## Frame Model

A logical slice consists of one or more frames across one or two channels:

```
LogicalSlice[k] =
    (optional) ch_metadata.Frame[1].payload + ... + ch_metadata.Frame[M].payload +
    (optional) ch_content.Frame[1].payload + ... + ch_content.Frame[N].payload
```

Each frame occupies exactly one NeoTape record. The frame header declares `frame_payload_size`. The reader reads exactly that many payload bytes; the remaining bytes in the record are zero padding.

Frame header flags:

- `START` — first frame of the current channel group.
- `END` — last frame of the current channel group.

The `archive_end` frame sets `START = 1`, `END = 1`, and `CLEAN_END = 1`.

## EOT Continuation Rules

When the writer encounters EOT (physical end of tape) or reaches the configured virtual tape capacity limit:

1. **Frame not yet committed:** The frame is considered not created. The next volume writes the same frame fresh.
2. **Frame committed, payload partially written:** The frame is incomplete. The next volume continues with the next uncommitted frame for the same logical slice.
3. **END frame committed, slice-level filemark not yet written:** The logical slice is complete. The next volume proceeds to the next logical slice or Archive End frame.
4. **Archive End frame not yet committed:** The archive is not cleanly complete. The next volume completes remaining logical slices, then writes the Archive End frame.
