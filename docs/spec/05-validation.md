# Validation and Conformance

Status: normative.

This chapter defines the shared validation rules for NeoTape readers,
extractors, spool readers, transport receivers, and inspection tools.

Any implementation that claims NeoTape conformance checking, including
`neotape-inspect`, MUST apply the applicable rules in this chapter. A normal
payload reader MAY expose only a subset of diagnostics, but it MUST make the
same accept/reject decisions for the validated conditions that apply to its
mode.

## Scope

This chapter covers:

- Frame-level structural validation
- Archive identity and sequence continuity
- Slice and channel ordering rules
- `archive_end` conformance
- Mode-specific exceptions for advisory metadata and repair-capable FEC reads

This chapter does not redefine the on-wire format. The authoritative layout and
field semantics remain in:

- [00-format-common.md](00-format-common.md)
- [02-frame-header.md](02-frame-header.md)
- [03-frames-and-slices.md](03-frames-and-slices.md)
- [06-volume-layout.md](06-volume-layout.md)
- [04-fec-channel.md](04-fec-channel.md)

## Validation Levels

### Baseline reader validation

A conforming reader, extractor, or TCP receiver MUST validate enough state to
ensure that emitted payload bytes come from an unambiguous, structurally valid
NeoTape stream.

### Full conformance validation

A conformance-checking tool such as `neotape-inspect` MUST validate all
applicable rules in this chapter, including advisory-field consistency and
archive-compliance checks that a pure payload reader may choose not to report
separately.

## Frame-Level Validation

For every received frame record, the validator MUST check:

- NeoTape magic
- `header_version`
- Record size matches decoded `volume_block_size_kib`
- `volume_block_size_kib` is within supported bounds
- `frame_payload_size` fits within the decoded record size
- Any non-`archive_end` frame with `END = 0` uses the full payload capacity of
  its record
- `frame_hash`
- Allowed `channel_type`
- Allowed flag bits for the current `header_version`
- Reserved fixed-header bytes that are required to be zero
- `SIDEBAND` rules for the current `channel_type`
- `SIGNED` flag vs. signature-field consistency

Specifically:

- Unknown `channel_type` values MUST be rejected in normal mode.
- For `ch_content`, `ch_metadata`, and `ch_fec`, a frame with `END = 0` MUST
  satisfy `frame_payload_size = (volume_block_size_kib * 1024) - 512`.
- For `ch_fec` under `rs_32_4`, every frame MUST satisfy
  `frame_payload_size = (volume_block_size_kib * 1024) - 512`, regardless of
  the `END` flag.
- `ch_content`, `ch_metadata`, and `archive_end` MUST have `SIDEBAND = 0` and
  zero-filled `sideband_data`.
- `ch_fec` MUST have `SIDEBAND = 1` and a valid descriptor per
  [04-fec-channel.md](04-fec-channel.md).
- `FEC_PROTECTED` MUST be rejected on any non-`ch_content` frame.
- `archive_end` is the only frame type allowed to set `CLEAN_END`.

## Archive Identity Validation

Within one logical archive instance, the validator MUST check:

- `archive_uuid` consistency across all normal frames
- `archive_label` consistency across all normal frames
- Decoded `volume_block_size_kib` consistency unless a new archive begins

`volume_seq_num` is advisory. A validator MUST NOT use it as the sole
authoritative continuity check, but it MAY warn if it moves backward or changes
unexpectedly within one backend volume.

## Sequence Continuity

The validator MUST enforce:

- `global_frame_seq_num` is monotonic and gapless across all frames, including
  `ch_fec` and `archive_end`
- `slice_seq_num` remains constant within one slice and increments by one when
  a new slice begins
- `channel_frame_seq_num` is contiguous within each
  `(slice_seq_num, channel_type)` stream

Volume boundaries do not reset `global_frame_seq_num`, `slice_seq_num`, or
`channel_frame_seq_num`.

When resuming after EOT or a client reconnect, the first new frame MUST
continue exactly from the last validated frame. Any gap, backward jump, or
archive identity mismatch MUST be rejected.

## Slice and Channel Ordering

Within each slice, the validator MUST enforce:

- `ch_metadata`, when present, forms at most one contiguous leading run
- No `ch_metadata` frame appears after the first `ch_content` or `ch_fec` frame
- `ch_fec` MUST NOT appear before the first `ch_content` frame of that slice
- `ch_fec` MUST describe a protected contiguous range of prior `ch_content`
  within the same slice
- A `FEC_PROTECTED` run MUST be a contiguous run of `ch_content` frames
- Each `FEC_PROTECTED` run MUST be immediately followed by one matching
  `ch_fec` group for the active FEC profile
- No later `ch_content` frame may appear before that matching `ch_fec` group
- Once a slice starts using `FEC_PROTECTED = 1` on `ch_content`, later
  `ch_content` frames in the same slice MUST NOT revert to `FEC_PROTECTED = 0`
- Once an archive starts using `FEC_PROTECTED = 1` on `ch_content`, later
  slices that contain `ch_content` MUST NOT revert to entirely unprotected
  `ch_content`

Under the local `32C + 4F` layout, `ch_content` and `ch_fec` MAY be physically
interleaved as repeated runs within the slice. This does not reset
`channel_frame_seq_num` for either channel.

## END Flag Rules

The validator MUST interpret `END` as "final frame of this channel within the
current slice".

It MUST check:

- `channel_frame_seq_num = 0`, `END = 0` means the first frame of a multi-frame
  channel stream
- `channel_frame_seq_num = 0`, `END = 1` means a single-frame channel stream
- `channel_frame_seq_num > 0`, `END = 1` means the final frame of a multi-frame
  channel stream

For local FEC layout:

- Intermediate FEC-group boundaries MUST NOT be inferred from `END`
- The final `ch_content` frame of the slice carries `END` for `ch_content`
- The final `ch_fec` frame of the slice carries `END` for `ch_fec`

## `archive_end` Validation

A conforming validator MUST check that an `archive_end` frame has:

- `channel_type = archive_end`
- `END = 1`
- `CLEAN_END = 1`
- `slice_seq_num = 0`
- `channel_frame_seq_num = 0`

A non-zero `frame_payload_size` is allowed for optional implementation-specific
archive-end metadata, but it does not change the control-frame semantics above.

## Advisory Metadata Exception

`ch_metadata` is advisory in normal restore mode.

Once a frame has already been identified as `ch_metadata`, a restore-mode
reader or extractor MAY downgrade a metadata-only integrity failure to a
warning if all of the following remain unambiguous:

- Record framing
- Header parsing
- Archive identity
- Sequence continuity

When this exception is used, the implementation SHOULD warn, ignore the
unusable metadata payload, and continue.

This exception applies only to already-identified `ch_metadata`. It MUST NOT be
used to skip:

- `ch_content` corruption
- Ambiguous headers
- Frame-size ambiguity
- Sequence or identity failures

## FEC Validation

A normal payload reader MAY verify and skip `ch_fec`, but it MUST NOT emit
`ch_fec` payload bytes.

A repair-capable validator MUST additionally check:

- `ch_fec` descriptor structure and field consistency per
  [04-fec-channel.md](04-fec-channel.md)
- Agreement among all FEC frames in one group on group parameters and
  `fec_group_blake3`
- `repair_index` range for the active FEC profile
- Reject `ch_fec` that describes any `ch_content` frame without
  `FEC_PROTECTED = 1`
- `source_content_frame_start` matches the `channel_frame_seq_num` of the first
  frame in the immediately preceding protected run
- `source_frame_count` matches the number of real `ch_content` frames in that
  protected run
- Reject duplicate `repair_index` values within one group
- Reject missing or non-continuous `repair_index` values within one group, as
  defined by the active FEC profile
- Profile-specific group-size rules MUST be enforced. For `rs_32_4`, reject a
  `FEC_PROTECTED` run longer than 32 real `ch_content` frames, and reject a
  matching group unless it contains exactly four `ch_fec` frames with
  `repair_index = 0, 1, 2, 3`

A repaired FEC group MUST NOT be accepted unless:

```text
BLAKE3(reconstructed_source_stream[0:source_stream_size]) ==
    fec_group_blake3
```

## Spool Validation

When reading from a spool directory, the validator MUST preserve tape-order
semantics:

- Enumerate candidate NeoTape files by filename grammar
- Sort by numeric `file-num`
- Treat each regular file boundary as one tape-file boundary
- Apply the same frame and continuity validation rules as tape mode

The optional `recovery-bundle.tar` is not part of the NeoTape stream and MUST
be ignored for validation ordering.

## TCP Receiver Validation

In the TCP reading or writing pipeline, the server-side validator MUST apply
the same frame, identity, and continuity rules as any other conforming reader.

For `frame_record` messages, it MUST additionally reject:

- Payloads larger than the protocol maximum
- Records whose byte length does not match decoded `volume_block_size_kib`
- Connection resumes whose first frame does not continue from prior validated
  state

On fatal validation failure, a TCP endpoint SHOULD send `error` with a
human-readable diagnostic and close the connection.

## Conformance Reporting

A full conformance checker such as `neotape-inspect` SHOULD report findings in
at least these categories:

- Per-frame structure and integrity
- Archive identity consistency
- Sequence continuity
- Slice/channel ordering
- `archive_end` compliance
- Signature / flag / sideband consistency
- FEC-descriptor and repair-group consistency when `ch_fec` is present
