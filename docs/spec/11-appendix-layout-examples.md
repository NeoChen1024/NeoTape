# Appendix: Layout Examples

Status: non-normative.

## Single-Volume Archive (Normal Case)

A single-volume archive fitting on one tape:

```
Tape:

File 0:   Logical Slice 1 tape file
          +-- ch_metadata Frame (START, END, slice=1, channel-seq=1)
          +-- ch_content Frame (START, slice=1, channel-seq=1)
          +-- payload bytes
          +-- ch_content Frame (slice=1, channel-seq=2)
          +-- payload bytes
          +-- ...
          +-- ch_content Frame (END, slice=1, channel-seq=N)
          +-- payload bytes
filemark
File 1:   Logical Slice 2 tape file
          +-- ch_content Frame (START, END, slice=2, channel-seq=1)
          +-- payload bytes
filemark
File 2:   Archive End frame (START, END, CLEAN_END)
filemark
```

A recovery bundle (plain pax tar) MAY be written as the first tape file before the first logical slice. Readers skip non-NeoTape data by scanning forward for the NeoTape magic.

## Metadata-Only Slice

A slice with only advisory metadata:

```
File K:   Logical Slice M tape file
          +-- ch_metadata Frame (START, slice=M, channel-seq=1)
          +-- metadata payload (catalog entries)
          +-- ch_metadata Frame (slice=M, channel-seq=2)
          +-- metadata payload
          +-- ch_metadata Frame (END, slice=M, channel-seq=3)
          +-- metadata payload
filemark
```

No `ch_content` frames are present. `frame_seq_num_within_channel` increments from 1 to 3 within the `ch_metadata` channel.

## Multi-Volume Layout

A logical slice that spans two backend volumes:

```
Tape 1:
File 0:   Logical Slice 1 tape file (START)
          +-- ch_content Frame (START, slice=1, volume=1)
          +-- payload bytes (first 60 GiB)
          ... (more ch_content frames)
          +-- ch_content Frame (slice=1, NOT END, volume=1)
          +-- payload bytes
~~EOT~~

Tape 2:
File 0:   Logical Slice 1 tape file (resumed)
          +-- ch_content Frame (slice=1, continued, volume=2)
          +-- payload bytes
          +-- ch_content Frame (END, slice=1, volume=2)
          +-- payload bytes
filemark
File 1:   Logical Slice 2 tape file
          ...
filemark
File 2:   Archive End frame
filemark
```

- `global_frame_seq_num` continues uninterrupted across the volume boundary.
- `logical_slice_seq_num` remains 1 on both volumes.
- `frame_seq_num_within_channel` continues within the same channel group.
- `volume_seq_num` changes from 1 to 2 (advisory).

## Multiple Archives on One Tape

```
... previous archive's Archive End frame ...
filemark
File X:   Logical Slice 1 tape file (new archive_uuid=B, vol_seq=1)
          ...
filemark
File Y:   Archive End frame (archive_uuid=B)
filemark
File Z:   Logical Slice 1 tape file (archive_uuid=C, vol_seq=1)
          ...
```

Each archive is independent; `archive_uuid` distinguishes instances. `volume_seq_num` restarts at 1 for each new archive (advisory).

## Frame Record Layout

```
  +-- 512-byte Frame Header
  |       magic (8) | version (1) | channel_type (1) | block_size_kib (2)
  |       archive_uuid (37) | archive_label (65) | volume_seq_num (8)
  |       global_frame_seq_num (8) | logical_slice_seq_num (8)
  |       frame_seq_num_within_channel (8)
  |       frame_payload_size (8) | flags (8) | _reserved (246)
  |       signature (72) | frame_hash (32)
  +-- frame_payload_size payload bytes
  +-- zero padding to volume_block_size_kib * 1024
```
