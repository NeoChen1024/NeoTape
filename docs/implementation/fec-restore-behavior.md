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
2. A hash-invalid protected content or repair record is unavailable. Its
   header and descriptor are not used to advance logical state; later valid
   headers must account for the gap.
3. Valid FEC descriptors identify missing source positions, real source length,
   and the expected group commitment. Whole missing records are erasures just
   like damaged payloads. Missing repair indices do not prevent recovery when
   the surviving matrix rows are sufficient.
4. The group closes at repair index 3 or an unambiguous following group, slice,
   or archive-end boundary. A pending group is retained across volume changes.
5. The decoder uses real surviving shards and virtual-zero positions, verifies
   the reconstructed source-stream hash, and emits source bytes once in order.
6. With trusted keys configured, the recovery commitment must come from a
   verified signed repair frame. Recovery does not authenticate a missing
   original frame header; diagnostics distinguish recovered payload from
   intact media conformance.

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

Both modes verify the reconstructed group commitment. Salvage may emit
individually validated surviving content when a group cannot be reconstructed;
normal mode rejects incomplete recovery.

## Retry Handling and Media Boundaries

The shared validator retains normalized BLAKE3 fingerprints for the most recent
8192 accepted records. Comparison excludes only volume ordinal, signature, and
frame hash; each replay still undergoes integrity and signature checks. An
equivalent replay does not advance logical sequence state or contribute output
or a second FEC shard. An older retry outside retained state fails explicitly.
This bounds retry memory independently of archive size.

Spool enumeration rejects duplicate numeric file numbers before playback.
Established fixed record framing permits skipping an unreadable header without
searching arbitrary byte offsets. Tape read failures are distinguished from
filemarks: continuation requires reported file/block counters and confirmed
progress to another record boundary. Failed or unavailable positioning stops
the reader instead of retrying indefinitely.

## Diagnostics and Tests

Normal restore reports unavailable records and verified retry suppression.
A successful repair reports the recovered shard count; uncertain original
headers are reported separately from payload integrity. Inspector reports
archive completeness separately from conformance of the observed records.

`tests/test_recovery_integration.cpp` checks actual whole-record deletion,
missing content and repair shards together, shortened groups, damaged headers,
signed recovery commitments, replay across Reader connections, conflicting
retries, and unrecoverable non-FEC gaps. It also exercises mismatched ACKs and
spool finalization errors. These are spool/socket tests, not verification of
physical tape positioning.

`tests/test_bounded_memory_integration.cpp` restores a 320 MiB single-slice FEC
stream under a 256 MiB address-space limit and compares the streamed output
byte-for-byte. Buffering remains bounded by a local FEC group and retry history.
