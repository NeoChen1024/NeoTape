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
- Mode-specific exceptions for advisory metadata and salvage reads

This chapter does not redefine the on-wire format. The authoritative layout and
field semantics remain in:

- [00-format-common.md](00-format-common.md)
- [02-frame-header.md](02-frame-header.md)
- [03-frames-and-slices.md](03-frames-and-slices.md)
- [05-volume-layout.md](05-volume-layout.md)

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
- `SIDEBAND` clear and `sideband_data` all zero
- `SIGNED` flag vs. signature-field consistency

Specifically:

- Unknown `channel_type` values MUST be rejected in normal mode.
- For `ch_content` and `ch_metadata`, a frame with `END = 0` MUST satisfy
  `frame_payload_size = (volume_block_size_kib * 1024) - 512`.
- Every frame MUST have `SIDEBAND = 0` and zero-filled `sideband_data`.
- `archive_end` is the only frame type allowed to set `CLEAN_END`.

## Signature Validation Modes

Readers MUST check `frame_hash` before treating a received record as valid or
verifying its signature. A failed check normally rejects that record. The
advisory metadata exception and salvage rules below define when processing
may continue without accepting the failed record as valid. Signature
verification does not replace frame integrity validation.

In every signature mode, `SIGNED = 0` requires all 72 signature bytes to be
zero. Non-zero bytes MUST be rejected, including on advisory metadata.

### Integrity-only mode

When no trusted public key is configured:

- a frame with `SIGNED = 0` MUST have an all-zero `signature` field;
- a frame with `SIGNED = 1` is interpreted as an 8-byte key ID followed by a
  64-byte Ed25519 signature and MUST NOT have an all-zero `signature` field,
  but cannot be cryptographically authenticated without a trusted public key;
- the implementation MAY accept a structurally valid signed frame, but MUST
  report it as signed but unverified and MUST NOT claim authenticity.

### Signature-verification mode

When one or more trusted public keys are configured:

- every frame with `SIGNED = 1` MUST verify against the trusted key selected by
  its key ID;
- an unknown key ID, malformed signature, or failed Ed25519 verification MUST
  be rejected;
- a frame with `SIGNED = 0` MAY be accepted as unauthenticated unless
  require-signed mode is active.

### Require-signed mode

Require-signed mode MUST NOT be enabled without at least one trusted public
key. In this mode every NeoTape frame, including `ch_metadata`, `ch_content`,
and `archive_end`, MUST set `SIGNED` and MUST verify against a configured
trusted key. An unsigned frame MUST be rejected. Advisory metadata handling
MUST NOT bypass this requirement.

## Archive Identity Validation

Within one logical archive instance, the validator MUST check:

- `archive_uuid` consistency across all frames, including `archive_end`
- `archive_label` consistency across all frames, including `archive_end`
- Decoded `volume_block_size_kib` consistency across the entire archive, including volume transitions and `archive_end`

`volume_seq_num` is advisory. A validator MUST NOT use it as the sole
authoritative continuity check, but it MAY warn if it moves backward or changes
unexpectedly within one backend volume.

## Sequence Continuity

These rules describe the logical frame stream, after suppressing verified
replays. Missing records may be skipped only under the salvage rules below.
Within one logical archive instance, the validator MUST enforce:

- `global_frame_seq_num` is monotonic and gapless across all frames, including
  `archive_end`
- `slice_seq_num` remains constant within one slice and increments by one when
  a new normal slice begins
- `channel_frame_seq_num` is contiguous within each
  `(slice_seq_num, channel_type)` stream

Volume boundaries do not reset `global_frame_seq_num`, `slice_seq_num`, or
`channel_frame_seq_num`.

After a valid `archive_end` closes the current archive context, a different
`archive_uuid` begins a new independent sequence context as defined under
[`archive_end` Validation](#archive_end-validation).

When resuming after EOT or a client reconnect, the first new logical frame
MUST continue the prior logical stream. A replayed suffix may precede it.
Unexplained gaps, conflicting replays, and archive identity mismatches MUST be
rejected outside explicit salvage mode.

### Replayed Records

Loss of an acknowledgement can cause a producer to replay a contiguous suffix
of records already stored on an earlier volume. Readers SHOULD accept such
replays at a volume or connection boundary when equivalence can be verified.
Physical spool file-number collisions are separate enumeration errors and are
not permitted by this rule. This applies to any channel, including `archive_end`.

A replay MUST independently pass frame integrity and the active signature
policy. To establish equivalence with an already accepted frame, compare the
entire record, excluding only `volume_seq_num`, `signature`, and `frame_hash`.
All other header bytes, payload bytes, and padding MUST match. These excluded
fields may change when a frame is reissued on a new volume; they still undergo
their own validation. A normalized digest of these comparison bytes MAY be
retained instead of the full original record.

The replay must start at a previously accepted global sequence number, proceed
in order, and reach the prior logical endpoint before new frames are accepted.
Readers MUST NOT emit replayed payload twice or advance logical sequence
state. An equal sequence number alone
is insufficient: a conflicting record MUST be rejected. If prior comparison
state is unavailable, the reader MUST report that it cannot verify the replay
rather than silently discard it or assume equivalence.

An equivalent replay of `archive_end` does not reopen the archive. No new
logical frame may follow it within that archive instance. Readers SHOULD
report accepted replays separately from corruption or sequence gaps.

## Slice and Channel Ordering

For an intact logical stream, the validator MUST enforce the following within
each slice:

- `ch_metadata`, when present, forms at most one contiguous leading run
- No `ch_metadata` frame appears after the first `ch_content` frame
## END Flag Rules

These are full-conformance checks on the logical stream after replay removal.

The validator MUST interpret `END` as "final frame of this channel within the
current slice".

It MUST check:

- `channel_frame_seq_num = 0`, `END = 0` means the first frame of a multi-frame
  channel stream
- `channel_frame_seq_num = 0`, `END = 1` means a single-frame channel stream
- `channel_frame_seq_num > 0`, `END = 1` means the final frame of a multi-frame
  channel stream

For every channel that appears in a slice, the validator MUST track whether
that channel has reached `END`:

- each present channel MUST have exactly one final frame carrying `END`;
- no later frame with the same `(slice_seq_num, channel_type)` may appear after
  that channel has reached `END`;
- before advancing to a new slice, accepting `archive_end`, or treating the
  input stream as complete, every channel present in the current slice MUST
  have reached `END`.

## `archive_end` Validation

A conforming validator MUST check that an `archive_end` frame has:

- `channel_type = archive_end`
- `END = 1`
- `CLEAN_END = 1`
- `slice_seq_num = 0`
- `channel_frame_seq_num = 0`

Before accepting `archive_end` as proof of clean archive conformance, the
validator MUST confirm that every channel present in the preceding slice has
reached `END`. Checking only the physically preceding frame is insufficient: a
metadata channel that never reached `END` may precede the content channel.

Each cleanly completed logical archive instance MUST contain exactly one
logical `archive_end`. Equivalent physical replays are handled under
[Replayed Records](#replayed-records). Once the end is accepted, that archive
validation context is closed:

- any later new logical frame belonging to the same archive instance MUST be
  rejected;
- a later frame MAY begin a new archive instance only with a different
  `archive_uuid` and fresh archive-local sequence state starting at
  `global_frame_seq_num = 0` and `slice_seq_num = 0`.

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
- Non-zero unsigned signature fields or other structural flag violations
- Signature verification failures or require-signed policy failures

A signed metadata record whose hash fails MUST NOT be accepted as authenticated
metadata. With trusted-key verification enabled, the reader MUST reject it
rather than use this exception to bypass signature verification.

## Salvage Validation Mode

Salvage mode is an explicit best-effort payload-recovery mode. It does not
claim archive conformance or clean completion. NeoTape has no in-format repair
channel: outside salvage mode, a missing or damaged `ch_content` record is
fatal.

Salvage mode MUST retain checks needed to identify an unambiguous valid frame:

- complete record framing and decoded block-size agreement;
- supported fixed-header layout and channel type;
- `frame_payload_size` bounds;
- `frame_hash` integrity;
- `SIGNED` flag/signature-field structural consistency.

After those checks succeed, salvage mode MAY relax archive identity continuity,
global/slice/channel sequence continuity, channel ordering, channel `END`
completeness, and clean `archive_end` requirements. A frame that fails a
mandatory integrity check MUST NOT contribute payload bytes. The implementation
MAY skip it and continue at the next independently framed record.

A salvage reader MUST preserve the configured signature policy. It emits only
individually validated `ch_content` payloads in stream order; a skipped record
leaves a gap in the output, and the reader MUST report that the payload is
incomplete.

Salvage mode MUST prominently report that its output is not fully verified.
Diagnostics belong on stderr and MUST NOT contaminate payload stdout.

## Spool Validation

When reading from a spool directory, the validator MUST preserve tape-order
semantics:

- Enumerate candidate NeoTape files by filename grammar
- Reject duplicate numeric `file-num` values before reading any records, then sort numerically
- Treat each regular file boundary as one tape-file boundary
- Apply the same frame and per-archive continuity validation rules as tape mode

The optional `recovery-bundle.tar` is not part of the NeoTape stream and MUST
be ignored for validation ordering.

## TCP Receiver Validation

In either TCP pipeline, the endpoint receiving a `frame_record` message MUST
validate the record before committing it to media or emitting its payload. In
the writing pipeline this endpoint is the Writer Client; in the reading
pipeline it is the Extractor Server.

The receiving endpoint MUST apply all frame-level rules and the identity and
continuity rules observable from the state it owns. The Writer validates
continuity within its current connection; the Archiver retains authoritative
archive-generation and cross-connection state. The Extractor retains
authoritative archive-validation state across Reader connections.

For `frame_record` messages, it MUST additionally reject:

- Payloads larger than the protocol maximum
- Records whose byte length does not match decoded `volume_block_size_kib`
- Connection resumes that neither continue prior logical state nor satisfy
  the verified replay rules, when the receiver owns that prior state

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
- Signature verification status when signed frames are present
