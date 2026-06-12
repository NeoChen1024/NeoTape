# Appendix: Layout Examples

Status: non-normative.

## Single-Volume Archive (Normal Case)

A single-volume archive fitting on one tape:

```
Tape:

File 0:   Volume Header (archive_uuid=A, volume_seq_num=1)
filemark
File 1:   Logical Slice 1 tape file
          +-- Frame Header (START, slice=1, frame=1)
          +-- payload bytes (8 GiB)
          +-- Frame Header (slice=1, frame=2)
          +-- payload bytes (8 GiB)
          +-- ...
          +-- Frame Header (END, slice=1, frame=N, slice_content_size, slice_content_blake3)
          +-- payload bytes
filemark
File 2:   Logical Slice 2 tape file
          ...
filemark
File N:   Archive End Header (clean_end, last_slice_seq_num=N-1)
filemark
```

A recovery bundle (plain pax tar) MAY be written in File 0 before the first
Volume Header; readers skip any non-NeoTape prefix.

## Multi-Volume Archive

When an archive spans two physical tapes:

**Tape 1:**

```
File 0:   Volume Header (archive_uuid=A, volume_seq_num=1)
filemark
File 1:   Logical Slice 1 tape file (complete, END flag)
filemark
File 2:   Logical Slice 2 tape file (payload starts, EOT before END)
~~EOT~~
```

**Tape 2:**

```
File 0:   Volume Header (archive_uuid=A, volume_seq_num=2)
filemark
File 1:   Logical Slice 2 tape file (continuation, END flag)
          +-- Frame Header (END, slice=2, frame=M, slice_content_size, slice_content_blake3)
          +-- remaining payload bytes
          +-- optional SLICE_METADATA Frames
filemark
File 2:   Logical Slice 3 tape file (complete, END flag)
filemark
File 3:   Archive End Header (clean_end, last_slice_seq_num=3)
filemark
```

Output of `neotape restore` for NeoTape/PAX:

```
slice 1 payload bytes (contiguous pax entries)
slice 2 payload bytes (contiguous pax entries)
slice 3 payload bytes (contiguous pax entries)
```

No NeoTape header bytes are emitted to stdout. Slices do not contain pax EOA markers; pax finalization is a profile-specific output policy.

## Multiple Archives on One Tape

```
File 0:   Volume Header (archive_uuid=A, volume_seq_num=1)
filemark
File 1..N:  Logical slice tape files for Archive A
filemark
File N+1: Archive End Header (archive_uuid=A, clean_end)
filemark
File N+2: Volume Header (archive_uuid=B, volume_seq_num=1)
filemark
File N+3..M:  Logical slice tape files for Archive B
filemark
File M+1: Archive End Header (archive_uuid=B, clean_end)
filemark
(remaining capacity, optionally more archives)
```

Each archive instance is independent with its own `archive_uuid`, `volume_seq_num` restarting at 1, and Archive End Header.

## EOT Mid-Slice Detail

If EOT interrupts a slice tape file between Frame writes:

1. Frames whose complete `volume_block_size` record was committed before EOT are present.
2. The next volume begins with a Volume Header.
3. The incomplete logical slice resumes in a new tape file on the next volume, starting with the next uncommitted Frame.
4. The incomplete slice's continuation begins at a new `logical_slice_seq_num` — same number, next Frame sequence number.
