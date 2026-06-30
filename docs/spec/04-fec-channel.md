# FEC Channel

Status: normative.

This chapter defines the `ch_fec` channel used for localized forward-error
correction of `ch_content` frames within one slice.

## Scope

`ch_fec` is an optional repair channel. A normal payload reader emits only
`ch_content` payload bytes and skips `ch_fec`.

The preferred on-media layout is repeated local groups inside the slice:

```text
Slice[k]:
  [optional ch_metadata...]
  C0  C1  ... C31   F0.0 F0.1 F0.2 F0.3
  C32 C33 ... C63   F1.0 F1.1 F1.2 F1.3
  ...
```

A FEC group is not a slice. It is a repair-coding unit over a contiguous range
of `ch_content` frames within one slice. Group boundaries do not define slice
boundaries and do not reset any sequence numbers.

## `ch_fec` Sideband Descriptor

`ch_fec` MUST set `SIDEBAND = 1`. Its `sideband_data` MUST contain the
following fixed 128-byte descriptor:

| Field                        | Type       | Size | Meaning |
| ---------------------------- | ---------- | ---- | ------- |
| `fec_version`                | `uint8`    | 1    | FEC descriptor version. This spec defines value `1`. |
| `fec_profile`                | `uint8`    | 1    | FEC profile identifier. This spec defines value `1` = `rs_32_4`. |
| `fec_flags`                  | `uint16`   | 2    | Profile-defined flags. Unused bits MUST be zero. |
| `source_content_frame_start` | `uint64`   | 8    | First protected `ch_content.channel_frame_seq_num`. |
| `source_frame_count`         | `uint16`   | 2    | Number of protected content frames in the group. |
| `repair_index`               | `uint16`   | 2    | Zero-based repair-symbol index carried by this frame. |
| `source_stream_size`         | `uint64`   | 8    | Protected source bytes before FEC zero padding. |
| `fec_group_blake3`           | `byte[32]` | 32   | BLAKE3 over the protected source stream. |
| `reserved`                   | `byte[72]` | 72   | MUST be all zero. |

All descriptor bytes are protected by `frame_hash` because `sideband_data` is
part of the canonical frame image.

## Defined FEC Profile

This specification defines the initial profile:

```text
fec_version             = 1
fec_profile             = 1   # rs_32_4

RS(32, 4)
```

For this profile:

- `rs_32_4` always uses 32 data shard positions and 4 repair shard positions.
- One FEC group protects `1..32` contiguous `ch_content` frames from the same
  slice.
- The normal layout is repeated local `32C + 4F` runs.
- The final group of a slice MAY be shortened to `mC + 4F`, where `1 <= m <= 32`.
- Because `fec_profile = 1` is defined as `RS(32, 4)`, `repair_index` MUST be
  in the range `0..3`.
- `source_frame_count` MUST be in the range `1..32`.

For `fec_profile = rs_32_4`, the parent code is fixed at:

```text
data_shards   = 32
repair_shards = 4
```

A full group uses `source_frame_count = 32`.

For a shortened final group with `source_frame_count = s`, where `1 <= s <= 32`:

- data shard positions `0..s-1` are real `ch_content` payload shards;
- data shard positions `s..31` are virtual all-zero shards;
- repair shard positions `32..35` are computed from all 32 data shard positions;
- only the `s` real content shards and 4 repair shards are emitted on media.

## Protected Source Material

The protected source stream is the concatenation of the protected content
payloads in `ch_content` order:

```text
fec_source_stream =
    Cg0.payload || Cg1.payload || ... || CgN.payload
```

The group-level commitment is:

```text
fec_group_blake3 = BLAKE3(fec_source_stream[0:source_stream_size])
```

Virtual zero padding used to fill FEC symbols is part of the encoder/decoder
only. It is not emitted by readers and is not included in `source_stream_size`
or in `fec_group_blake3`.

For a shortened final group with `source_frame_count = s`, where `1 <= s <= 32`,
data shard positions `0..s-1` are the protected `ch_content` payloads, and
data shard positions `s..31` are virtual all-zero shards used only by the
encoder and decoder. The virtual shards are not emitted as `ch_content` frames
and are not included in `source_stream_size` or `fec_group_blake3`.

Repair shard positions are `32 + repair_index`, where `repair_index` is in
`0..3`. All repair shards MUST be computed from the fixed 32 data shard
positions.

## Reader Behavior

Mode-specific validation and acceptance rules for `ch_fec`, including repair
acceptance criteria, are centralized in
[docs/spec/05-validation.md](05-validation.md).
