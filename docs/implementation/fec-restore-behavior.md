# FEC Production and Restore Behavior

This document records the implementation policy around the normative
`ch_fec` format in `docs/spec/04-fec-channel.md`. It describes CLI behavior and
extractor control flow; the specification remains authoritative for on-media
bytes and validation requirements.

## CLI Policy

`--fec` is a producer-side write policy option on `neotape-archiver` and
`neotape-raw-store`. It marks content as `FEC_PROTECTED` and emits local
`rs_32_4` groups in the preferred `32C + 4F` layout. The final group may contain
fewer than 32 real content frames.

The extractor does not have or require an `--fec` option. FEC is
self-describing: the extractor automatically buffers a group when it sees
`FEC_PROTECTED` content and consumes the matching `ch_fec` frames. A normal
restore therefore benefits automatically from FEC written by a producer.

`--salvage` is independent of FEC enablement. It selects relaxed consistency
and best-effort fallback policy; it is not the switch that enables RS decoding.

## Normal Restore State Flow

Unprotected content is validated and streamed to the output directly. For an
FEC-protected run, emission waits only for the matching local repair group:

1. Content that passes integrity and signature policy becomes an available
   real data shard.
2. Protected content with a hash mismatch advances through strict
   sequence/channel validation using its structurally parsed header, but its
   payload is never trusted; its shard position is marked unavailable.
3. A valid `ch_fec` frame becomes an available repair shard. A repair frame
   with a hash mismatch is also marked unavailable when its descriptor and
   strict ordering remain structurally valid.
4. After `repair_index = 3`, the extractor builds the fixed 36-position shard
   set, including virtual zero positions for a shortened group, and runs the
   `rs_32_4` decoder.
5. Recovered real data is concatenated, truncated to `source_stream_size`, and
   accepted only if `fec_group_blake3` matches.
6. Only after that commitment succeeds are the content bytes streamed to the
   output.

The extractor never buffers a complete slice. Its retained payload is bounded
by one incomplete FEC group, including groups split across volume boundaries.
Completed groups are emitted immediately, so slice size does not determine RAM
usage.

Valid surviving content is authoritative. Only unavailable real content
positions are reconstructed, and corrupt frame bytes are never supplied as
candidate shard data.

If there are not 32 independent known positions, the descriptor/group is
inconsistent, or the reconstructed commitment fails, normal restore is fatal.
No bytes from that FEC group are emitted.

Signature policy remains separate from FEC. A frame that fails configured
signature requirements is not silently accepted merely because repair data
exists.

## Salvage Differences

Salvage uses the same buffering, erasure decoding, and group commitment check
as normal restore. Its differences apply outside successful decoding:

- archive identity, sequence continuity, channel ordering, and clean-end
  consistency are relaxed as documented in `docs/spec/05-validation.md`;
- integrity-invalid frames do not contribute shard or payload bytes;
- when recovery fails, salvage warns and may emit only surviving real content
  shards in channel order;
- stderr prominently marks the overall output as not fully verified.

The same repairable damage therefore yields the same reconstructed payload in
normal and salvage modes. Salvage changes what happens to unrepairable or
inconsistent input, not whether FEC correction is attempted.

## Diagnostics and Tests

Normal restore reports an invalid protected frame as unavailable while waiting
for group completion. Successful repair reports the number of unavailable
content shards recovered. Failure is fatal in normal mode and downgraded only
in salvage mode.

`tests/test_recovery_integration.cpp` covers undamaged normal restore, normal recovery
of damaged protected content, and normal recovery with both a content and a
repair shard unavailable. It also verifies that five unavailable positions are
fatal and cause the normal extractor to exit rather than emit partial group
output or wait for another reader. The salvage test in the same file covers non-FEC
best-effort skipping and unverified-output diagnostics.
`tests/test_bounded_memory_integration.cpp` restores a 320 MiB single-slice FEC
stream under a 256 MiB address-space limit and compares the streamed output
byte-for-byte, preventing reintroduction of slice-sized buffering.
