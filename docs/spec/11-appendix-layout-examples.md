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

## FEC-Enabled Slice With Local `32C + 4F` Runs

One slice using the preferred local FEC layout:

```
File P:   Slice K tape file
          +-- ch_metadata Frame (optional)
          +-- ch_content Frame (slice=K, channel-frame-seq=0)
          +-- ...
          +-- ch_content Frame (slice=K, channel-frame-seq=31)
          +-- ch_fec Frame (slice=K, channel-frame-seq=0, repair-index=0)
          +-- ch_fec Frame (slice=K, channel-frame-seq=1, repair-index=1)
          +-- ch_fec Frame (slice=K, channel-frame-seq=2, repair-index=2)
          +-- ch_fec Frame (slice=K, channel-frame-seq=3, repair-index=3)
          +-- ch_content Frame (slice=K, channel-frame-seq=32)
          +-- ...
          +-- final ch_content Frame (END, slice=K, channel-frame-seq=N)
          +-- final ch_fec Frame (END, slice=K, channel-frame-seq=M)
filemark
```

- `ch_content.channel_frame_seq_num` continues from 0 to `N` across the whole slice, even though `ch_fec` frames appear in between.
- `ch_fec.channel_frame_seq_num` is its own per-slice stream, continuing from 0 to `M` across all FEC groups in that slice.
- `END` marks the final frame of each channel in the slice, not the end of each local FEC group.

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

## Missing Content and Repair Records

In a full `32C + 4F` group, suppose content shard C0 and repair shard F3 cannot
be read. The 31 surviving content shards and F0 supply an invertible basis for
this example; F1 and F2 are additional surviving repairs. A repair-capable
reader can recover C0, verify the source-stream hash from a surviving valid
descriptor, and emit the 32 source payloads once in order. It need not wait for
F3 to appear. Group closure is established by the following valid records or
input boundary, subject to the recovery validation rules.

The missing original headers are not reconstructed by payload FEC. The reader
reports recovered payload separately from full media conformance. If the
remaining descriptors disagree or no valid descriptor survives, this example
does not authorize guessing a group or emitting unverified reconstruction.
