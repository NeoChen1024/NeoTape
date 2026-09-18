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

## Signature Validation Modes

Readers MUST check `frame_hash` before treating a received record as valid or
verifying its signature. A failed check normally rejects that record. The
advisory metadata exception and FEC recovery rules below define when processing
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
`ch_fec`, and `archive_end`, MUST set `SIGNED` and MUST verify against a
configured trusted key. An unsigned frame MUST be rejected. Advisory metadata
handling MUST NOT bypass this requirement. Recovery of unavailable content is
subject to the authenticated FEC commitment rule below; it does not manufacture
or verify a signature for a missing original frame.

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
replays. Missing records may be handled only under the FEC recovery or salvage
rules below. Within one logical archive instance, the validator MUST enforce:

- `global_frame_seq_num` is monotonic and gapless across all frames, including
  `ch_fec` and `archive_end`
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
Readers MUST NOT emit replayed payload twice, advance logical sequence state,
or count replayed shards twice in an FEC group. An equal sequence number alone
is insufficient: a conflicting record MUST be rejected. If prior comparison
state is unavailable, the reader MUST report that it cannot verify the replay
rather than silently discard it or assume equivalence.

An equivalent replay of `archive_end` does not reopen the archive. No new
logical frame may follow it within that archive instance. Readers SHOULD
report accepted replays separately from corruption or sequence gaps.

## Slice and Channel Ordering

For an intact logical stream, the validator MUST enforce the following within
each slice. Recovery may account for unavailable records as described below;
it MUST NOT invent missing header fields to claim full conformance.

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

These are full-conformance checks on the logical stream after replay removal.
Recovery may establish payload integrity even when a missing header prevents
verification of an original `END` flag; it MUST report that distinction.

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
- a physical transition to a different channel does not imply that the prior
  channel has reached `END`;
- before advancing to a new slice, accepting `archive_end`, or treating the
  input stream as complete, every channel present in the current slice MUST
  have reached `END`.

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

Before accepting `archive_end` as proof of clean archive conformance, the
validator MUST confirm that every channel
present in the preceding slice has reached `END`. Checking only the physically
preceding frame is insufficient when channels are interleaved.

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

## FEC Validation

Readers MUST NOT emit `ch_fec` payload bytes. A reader that does not repair
still applies the applicable frame and intact-stream validation rules.

### Intact Stream Conformance

Writers MUST emit exactly four repair records per `rs_32_4` group, with
`repair_index = 0, 1, 2, 3` in order. A conformance checker MUST report missing
or damaged records; successful payload recovery does not make the original
media representation intact.

For an intact group, validators MUST check:

- descriptor structure and profile bounds from [04-fec-channel.md](04-fec-channel.md);
- agreement on group parameters and `fec_group_blake3` across repair records;
- a contiguous protected run of `1..32` content frames immediately before the
  repair group, all with `FEC_PROTECTED = 1`;
- `source_content_frame_start` and `source_frame_count` match that run;
- `source_stream_size` matches the sum of the real content payload sizes;
- exactly one occurrence of each repair index after verified replay removal.

### Recovery from Unavailable Records

A repair-capable reader SHOULD attempt recovery when content or repair records
are unavailable. Unavailable includes a record whose integrity check failed,
an unreadable tape block, and an entirely missing record. The reader MUST NOT
require all four repair records to survive. It may proceed only at a known
backend record boundary; it MUST NOT guess a byte-stream boundary after an
unframed I/O failure.

The reader MUST establish the group from at least one valid surviving FEC
descriptor, any surviving valid content headers, and surrounding sequence state.
The descriptor identifies real source positions and their total meaningful
length. Missing real source positions are erasures, not virtual zero shards.
Only positions beyond `source_frame_count` are virtual zero shards. Corrupt
headers MUST NOT be used as authoritative position or group information.

Surviving descriptors MUST agree on group identity, dimensions, and commitment.
Surviving content MUST match the described archive, slice, protected range,
and payload lengths. Each surviving repair index must be in range and unique
apart from verified replays. Gaps may be tolerated only when their placement
is unambiguously accounted for by the group's missing content or repair
records. Unrelated sequence gaps and conflicting records remain errors.

A reader MUST NOT wait indefinitely for a missing repair index. A subsequent
valid group, slice, or archive-end record, or a known end of input, may delimit
the available group once its membership is unambiguous. End of input alone
does not prove clean archive completion. At a volume transition, the reader
may retain the pending group while requesting the next volume.

The reader MUST choose enough independent surviving and virtual-zero shards
to form the invertible basis defined in [04-fec-channel.md](04-fec-channel.md).
It MUST verify `fec_group_blake3` over the reconstructed meaningful source
stream before emitting that group's content, in source order and once only.
No bytes from an unverified reconstruction may be emitted. If all source
shards survive, they may be checked directly against the group commitment
without requiring any particular number of surviving repair shards.

With trusted-key verification enabled, any descriptor used as the recovery
commitment MUST come from a frame whose hash and trusted signature both verify.
Require-signed additionally applies to every accepted surviving frame. A
signature-policy failure MUST NOT be silently treated as a valid unsigned
shard. Recovered content is authenticated through the signed group commitment;
this does not establish the original missing frame's header or signature.

Recovery can establish payload integrity without establishing full frame or
archive conformance. In particular, FEC protects content bytes, not missing
metadata, flags, signatures, or `archive_end`. Readers MUST report missing
records and recovery separately, and MUST NOT claim clean archive conformance
when missing headers or channel endings remain unverified. A missing end
marker cannot be reconstructed by this profile. If group membership or payload
completeness remains ambiguous, only explicit salvage mode may emit partial
surviving content.

## Salvage Validation Mode

Salvage mode is an explicit best-effort payload-recovery mode. It does not
claim archive conformance or clean completion.

Salvage mode MUST retain checks needed to identify an unambiguous valid frame:

- complete record framing and decoded block-size agreement;
- supported fixed-header layout and channel type;
- `frame_payload_size` bounds;
- `frame_hash` integrity;
- `SIGNED` flag/signature-field structural consistency; and
- a structurally valid FEC descriptor for any accepted `ch_fec` frame.

After those checks succeed, salvage mode MAY relax archive identity continuity,
global/slice/channel sequence continuity, channel ordering, channel `END`
completeness, and clean `archive_end` requirements. A frame that fails a
mandatory integrity check MUST NOT contribute payload bytes. The implementation
MAY skip it and continue at the next independently framed record.

A repair-capable salvage reader SHOULD attempt reconstruction under the FEC
recovery rules above, including when some repair records are missing. It MUST
verify `fec_group_blake3` before emitting reconstructed bytes and MUST preserve
the configured signature policy. If recovery is impossible, it MAY emit only
individually validated surviving content shards in channel order, reporting
that the payload is incomplete.

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
  the verified replay or explicit recovery rules, when the receiver owns that
  prior state

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
- FEC-descriptor and repair-group consistency when `ch_fec` is present
