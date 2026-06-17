# Open Questions and Unresolved Design Choices

Status: non-normative.

This document collects open questions, unresolved design choices, and decision points that affect the NeoTape format but have not yet been settled.

## Format and Layout

### Recovery Bundle Format and Member Set

The exact format, member set, and placement rules for an optional recovery bundle at BOT are open. Should the spec recommend a plain pax tar with a fixed member set (README, RESTORE, FORMAT-SPEC, reader source), or leave it entirely to deployment?

Spool layout, filename grammar, and spool reader expectations are now defined
in [05-spool-dir.md](05-spool-dir.md). The plan stream and slice-scoped
catalog (`ch_metadata`) record format are now defined in
[09-plan-metadata.md](09-plan-metadata.md); these topics are no longer tracked
as open questions here.

## EOT and Continuation

### Partial Frame After EOT

If EOT occurs after a frame header is committed but before the full payload is written, should there be a mechanism to resume at the exact byte offset within the frame, or must the entire frame be retransmitted? Current design: no partial frame continuation; the next frame covers the remaining payload range.

### Post-ENOSPC Early Warning Region

LTO drives can sometimes complete a write after an ENOSPC early-warning. Should NeoTape reserve a small backend-specific area for close-out or synchronization operations after the first ENOSPC?

## Reader Behavior

### Reader State Machine Exhaustiveness

Should the reader state machine be fully specified with every transition and error exit, or is the current high-level model sufficient for interoperable implementations?

### Error Class Definitions

- Which errors are retryable vs. fatal?
- Which errors should produce non-zero exit codes vs. warnings on stderr?
- Should exit codes be standardized across all NeoTape tools?

### Strict vs. Permissive Reading

Should the reader validate zero padding in frames in normal mode, or only in strict mode? Current design: padding is included in `frame_hash`, so padding validation is implicit.

### Unknown Channel Type Handling

Should salvage mode allow skipping unknown channel types? Current design: normal mode MUST reject; salvage mode MAY skip only when sequence continuity is preserved.

## Writer Behavior

### Slice Target Size Heuristic

Slice target size is a writer heuristic (commonly ~64 GiB). Should the format recommend a specific size, or leave it entirely to implementation?

## Safety and Recovery

### Encrypted Payloads

How should NeoTape handle encrypted payloads while preserving catalog usability? Catalog entries for encrypted payloads would need to avoid leaking plaintext filenames.

### Incremental Archive Updates

Should incremental tar-style updates modify an existing archive instance (overwrite Archive End frame, append, rewrite end frame) or create a new archive instance on the same medium?

### Multi-Archive Medium Index

Should NeoTape maintain a lightweight index of all archive instances on a medium, or should discovery always be by scanning tape files?

## Implementation

### Minimal Reader Embedding

Should the minimal reader (`neotape restore`) be compact enough to embed in the recovery bundle as source code?

### Minimum Supported Platform Set

What is the minimum platform set? Linux only? Linux + FreeBSD + macOS?

### CI and Test Vectors

How should release artifacts include format test vectors and compatibility fixtures?

### Hardware Compression

Should NeoTape account for LTO hardware compression when reporting or limiting tape capacity?

## Resolved Decisions (for reference)

1. **C++ with GNU Makefile** — closed. No CMake, no Rust, no language changes.
2. **stdout = pure ch_content payload bytes** — closed. All diagnostics on stderr or `/dev/tty`.
3. **No mutable state in the archive stream** — closed. Archive discovery by scanning tape files.
4. **Archive-level completion by Archive End frame only** — closed. EOD alone is not sufficient.
5. **BLAKE3 as frame integrity hash** — closed. `frame_hash` covers the entire frame.
6. **No mandatory trailer rewrite** — closed. The format does not require seeking back to BOT.
7. **Length-framed transport** — closed. Frame boundaries by `frame_payload_size`, not by payload parsing.
8. **Single unified frame header** — closed. No separate Volume Header or Archive End Header; all records use the same 512-byte layout with `channel_type` dispatch.
9. **No payload profile in core format** — closed. Removed `payload_profile` field; core is payload-format agnostic.
10. **No slice-level hash** — closed. Removed `slice_content_size` and `slice_content_blake3`; per-frame `frame_hash` is sufficient.
11. **Channel-local frame sequence** — closed. `frame_seq_num_within_channel` replaces `frame_seq_num_within_slice`; channel groups are independently sequenced.
12. **Metadata-first ordering** — closed. `ch_metadata` MUST precede `ch_content` within each logical slice.
13. **Advisory volume_seq_num** — closed. Volume identity is advisory; archive continuity is validated by `archive_uuid` and sequence numbers.
14. **KiB-encoded block size** — closed. `volume_block_size_kib` is a `uint16` storing KiB, not bytes.
15. **512-byte fixed header** — closed. Reduced from 1024 bytes.
