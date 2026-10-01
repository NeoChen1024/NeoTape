# Appendix: Layout Examples

Status: non-normative.

## Single-Volume Archive (Normal Case)

A single-volume archive fitting on one tape:

```
Tape:

File 0:   Slice 0 tape file
          +-- ch_metadata Frame (END, slice=0, channel-frame-seq=0)
          +-- ch_content Frame (slice=0, channel-frame-seq=0)
          +-- payload bytes
          +-- ch_content Frame (slice=0, channel-frame-seq=1)
          +-- payload bytes
          +-- ...
          +-- ch_content Frame (END, slice=0, channel-frame-seq=N)
          +-- payload bytes
filemark
File 1:   Slice 1 tape file
          +-- ch_content Frame (END, slice=1, channel-frame-seq=0)
          +-- payload bytes
filemark
File 2:   Archive End frame (END, CLEAN_END)
filemark
```

A recovery bundle (plain pax tar) MAY be written as the first tape file before the first slice. Readers skip prefix records by checking for NeoTape magic at backend record
boundaries, then validating the candidate frame. They do not search arbitrary
byte offsets inside records for a header.

## Metadata-Only Slice

A slice with only advisory metadata:

```
File K:   Slice M tape file
          +-- ch_metadata Frame (slice=M, channel-frame-seq=0)
          +-- metadata payload (catalog entries)
          +-- ch_metadata Frame (slice=M, channel-frame-seq=1)
          +-- metadata payload
          +-- ch_metadata Frame (END, slice=M, channel-frame-seq=2)
          +-- metadata payload
filemark
```

No `ch_content` frames are present. `channel_frame_seq_num` increments from 0 to 2 within the `ch_metadata` channel.

## Multi-Volume Layout

A slice that spans two backend volumes:

```
Tape 1:
File 0:   Slice 0 tape file
          +-- ch_content Frame (slice=0, channel-frame-seq=0, volume=1)
          +-- payload bytes (one record; this run totals 60 GiB)
          ... (more ch_content frames)
          +-- ch_content Frame (slice=0, channel-frame-seq=K, NOT END, volume=1)
          +-- payload bytes
~~EOT~~

Tape 2:
File 0:   Slice 0 tape file (resumed)
          +-- ch_content Frame (slice=0, channel-frame-seq=K+1, continued, volume=2)
          +-- payload bytes
          +-- ch_content Frame (END, slice=0, channel-frame-seq=N, volume=2)
          +-- payload bytes
filemark
File 1:   Slice 1 tape file
          ...
filemark
File 2:   Archive End frame
filemark
```

- `global_frame_seq_num` continues uninterrupted across the volume boundary.
- `slice_seq_num` remains 0 on both volumes.
- `channel_frame_seq_num` continues within the same channel group.
- `volume_seq_num` changes from 1 to 2 (advisory).

## Multiple Archives on One Tape

```
... previous archive's Archive End frame ...
filemark
File X:   Slice 0 tape file (new archive_uuid=B, vol_seq=1)
          ...
filemark
File Y:   Archive End frame (archive_uuid=B)
filemark
File Z:   Slice 0 tape file (archive_uuid=C, vol_seq=1)
          ...
```

Each archive is independent; `archive_uuid` distinguishes instances. `volume_seq_num` restarts at 1 for each new archive (advisory).

## Frame Record Layout

For the exact fixed-header field widths and byte positions, see
[02-frame-header.md](02-frame-header.md). Every NeoTape record has the
following high-level structure:

```
  +-- 512-byte Frame Header
  |       exact field layout: see 02-frame-header.md
  +-- frame_payload_size payload bytes
  +-- zero padding to volume_block_size_kib * 1024
```

## Lost Acknowledgement and Replay

Suppose frames 40 and 41 reached volume 1, but the Archiver received only ACK
40 before the connection failed. Volume 2 may begin by reissuing frame 41 with
its new volume ordinal and recomputed hash/signature, then continue with 42.
A reader that already accepted 41 verifies the replay's equivalence and emits
its payload only once. A different payload for the same sequence number is a
conflict, not an acceptable retry.
