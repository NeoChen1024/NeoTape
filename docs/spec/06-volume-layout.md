# Volume Layout

Status: normative.

## Scope

This document describes the logical nesting and physical tape layout of NeoTape archives: how archives, volumes, slices, channels, and frames map to LTO tape files, filemarks, and spool directory entries.

For the fixed header layout and channel type definitions, see [docs/spec/02-frame-header.md](02-frame-header.md).

## Hierarchy

```
Archive (archive_uuid)
  └── Volume (backend-defined physical/virtual volume)
      └── Slice (slice_seq_num)
          └── Channel (channel_type: ch_metadata / ch_content / ch_fec)
              └── Frame (global_frame_seq_num, channel_frame_seq_num)
```

- **Archive** is the authoritative logical backup instance, identified by `archive_uuid`.
- **Volume** is a backend-defined physical or virtual container. It is not an authoritative logical record in the NeoTape stream. `volume_seq_num` is advisory and combines with `archive_label` to form an operator-facing display label.
- **Slice** is the unit of logical ordering within an archive. It is identified by `slice_seq_num` and may span frames and backend volumes.
- **Channel** partitions a slice into `ch_metadata`, `ch_content`, and optional `ch_fec`. Metadata, when present, precedes all non-metadata frames. A slice may contain only metadata.
- **Frame** is the concrete transport record. Every frame has exactly one `channel_type`, one `global_frame_seq_num`, and one `channel_frame_seq_num`.

## Physical Nesting

```
Physical Medium (LTO tape)
  └── (optional recovery bundle tape file, not NeoTape format)
  └── [optional filemark]
  └── Archive Volume (part of one archive on one medium)
  │     ├── Slice (tape file, one or more NeoTape records)
  │     │     ├── ch_metadata Frame (optional, NeoTape record, volume_block_size_kib * 1024 bytes)
  │     │     │     ├── 512-byte Frame Header
  │     │     │     ├── payload bytes
  │     │     │     └── zero padding
  │     │     ├── ch_content Frame (NeoTape record)
  │     │     ├── ch_fec Frame (optional, NeoTape record)
  │     │     ├── ...
  │     │     └── final channel frame(s) with END
  │     ├── filemark
  │     ├── Slice (tape file)
  │     ├── filemark
  │     ├── ...
  │     └── Archive End frame (tape file, single NeoTape record)
  └── filemark
  └── (next archive instance, if capacity remains)
```

- **Physical Medium** — a sequential storage medium holding one or more archive volumes. NeoTape does not store a medium-level descriptor; any non-NeoTape prefix before the first NeoTape frame is ignored by readers.
- **Tape file** — LTO filemark-delimited region. NeoTape uses tape files for slices and the Archive End frame.
- **NeoTape record** — a single `volume_block_size_kib * 1024`-byte block written to the tape device or stored as a record within a spool file.
- **Frame** — exactly one NeoTape record. Frames within a slice tape file are chained by `frame_payload_size`, not by filemarks.
- **Volume boundary** — a physical/operator event, identified by EOT and detected by `volume_seq_num` change (advisory) or sequence continuity checks (authoritative).

## No Volume Header

There is no dedicated Volume Header tape file. The first NeoTape record on a new volume is the first frame of the first slice (or an Archive End frame if the volume contains only the end marker). Every frame repeats `volume_block_size_kib`, `archive_uuid`, `archive_label`, and `volume_seq_num`, so a reader can bootstrap from any frame.

Within a backend-defined physical or virtual volume, all NeoTape frames SHOULD carry the same `volume_seq_num`. A reader MAY warn if `volume_seq_num` changes unexpectedly within the same backend volume.

## Single-Volume Tape Layout (Normal Case)

```
[optional recovery bundle]
[optional filemark]
File 0:   Slice 0 tape file
          ├─ ch_metadata Frame (END, slice=0, channel-frame-seq=0)
          │  (optional)
          ├─ ch_content Frame (slice=0, channel-frame-seq=0)
          ├─ ch_content Frame (slice=0, channel-frame-seq=1)
          ├─ ch_fec Frame (optional, slice=0, channel-frame-seq=0)
          ├─ ...
          └─ final `ch_content` or `ch_fec` Frame(s) with END
filemark
File 1:   Slice 1 tape file
          ...
filemark
File N:   Archive End frame (END, CLEAN_END)
filemark
```

Within a slice tape file, frames are located by `frame_payload_size`, not by additional filemarks. Metadata frames, when present, precede all non-metadata frames. `ch_content` and `ch_fec` MAY then appear as repeated runs within the same slice tape file. A slice MUST contain at least one frame across its channels.

## Multi-Volume Tape Layout

When an archive spans multiple physical tapes:

**Tape 1:**

```
Slice 0 tape file (complete)
filemark
Slice 1 tape file (payload starts but EOT before END)
~~EOT~~
```

**Tape 2:**

```
Slice 1 tape file (continuation, END)
filemark
Slice 2 tape file (END)
filemark
Archive End frame
filemark
```

If EOT interrupts a slice tape file, the next volume continues the interrupted slice in a new tape file, starting with the next uncommitted frame. Only frames whose complete fixed header and payload bytes were fully committed before EOT are considered present.

A slice MAY span backend volumes. `global_frame_seq_num`, `slice_seq_num`, and `channel_frame_seq_num` continuity do not reset at a backend volume boundary.

## Multiple Archives on One Tape

A physical tape may hold several complete archives sequentially:

```
... previous archive completed ...
filemark
Slice 0 tape file
filemark
...
filemark
Archive End frame
filemark
(some capacity remaining)
Slice 0 tape file (next archive_uuid)
filemark
...
```

Each archive instance is independent with its own `archive_uuid`. A reader locates archive instances by scanning forward through tape files, validating frames, and matching the expected or interactive `archive_uuid`.

## Frame Model

A slice consists of one or more frames across one, two, or three channels:

```
Slice[k] =
    [ ch_metadata.Frame[0].payload + ... + ch_metadata.Frame[M-1].payload ] +
    one or more ch_content payload runs +
    optional ch_fec repair runs for preceding content ranges
```

Each frame occupies exactly one NeoTape record. The frame header declares `frame_payload_size`. The reader reads exactly that many payload bytes; the remaining bytes in the record are zero padding.

Frame header flags:

- `channel_frame_seq_num = 0` — first frame of the current channel within the slice.
- `END` — last frame of the current channel within the slice.

The `archive_end` frame sets `END = 1`, and `CLEAN_END = 1`.

## EOT Continuation Rules

When the writer encounters EOT (physical end of tape) or reaches the configured virtual tape capacity limit:

1. **Frame not yet committed:** The frame is considered not created. The next volume writes the same frame fresh.
2. **Frame committed, payload partially written:** The frame is incomplete. The next volume continues with the next uncommitted frame for the same slice.
3. **END frame committed, slice-level filemark not yet written:** The slice is complete. The next volume proceeds to the next slice or Archive End frame.
4. **Archive End frame not yet committed:** The archive is not cleanly complete. The next volume completes remaining slices, then writes the Archive End frame.
