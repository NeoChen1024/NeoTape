# Future Extensions

Status: extension ideas; non-normative.

This document collects extension ideas for future NeoTape versions. They are not part of the current format and should not be implemented until a later version defines them.

## LTO Partition Metadata Mode

LTO tape drives support partitioning (e.g. two partitions on a single tape). A future version could use a metadata partition to store catalogs, indices, or recovery data separately from the payload partition.

## Per-Frame Hash Chain

Chain frame hashes by including the previous frame's `frame_hash` in the current frame header. This creates a hash chain that allows the reader to detect missing or reordered frames independently of sequence numbers.

## Per-Slice Merkle Tree

Build a Merkle tree over `frame_hash` values within a slice, recording the root in the final frame header. This enables efficient verification of individual frame ranges without reading the entire slice.

## Additional Channel Types

Extend `channel_type` beyond `ch_content`, `ch_metadata`, and `archive_end`. Values 3–254 are reserved. Use cases include:

- **Inline parity or FEC frames** — parity or Reed-Solomon recovery data as a separate channel type, enabling bitrot repair without external tools.
- **Multiplexed payload channels** — multiple logical streams within one slice.
- **Diagnostics embedding** — writer heartbeats or progress snapshots at predictable frame positions.
- **Real-time verification nodes** — Merkle tree nodes or hash-chain data alongside content data.

New `channel_type` values are allocated by future specification versions. Arbitrary mixing is allowed since `frame_seq_num_within_channel` provides per-channel ordering.

## Partial Restore Index

An index that maps file paths to their exact logical slice and frame positions, enabling targeted partial restore without scanning all slices.

## Filesystem-Native Payloads

Support for ZFS send streams and Btrfs send streams:

- Each dataset, subvolume, or snapshot send stream is a payload sub-stream that normally maps to one or more logical slices.
- Catalog fields for dataset/subvolume identity, snapshot names, parent snapshot dependencies, receive order, stream-level checksums, and slice ranges.

NeoTape core remains payload-format agnostic; restore semantics are handled by the receive tool.

## Changer/Robot Integration

Support for automated tape library changers: load/unload media, scan barcodes, select tapes by label.

## Machine-Readable JSON Control Protocol

A JSON-based control protocol over a separate channel for library automation, instead of parsing stderr output.

## Reed-Solomon or Parity Frames

Dedicated parity frames within a slice for erasure coding. Allows recovery of a limited number of lost or damaged frames.

## Encryption and Key Metadata

Support for encrypted payloads with key metadata stored in the catalog or metadata channel. Preserve catalog usability without exposing plaintext filenames.

## Multiple Catalog Replicas

Store catalog replicas on multiple volumes for redundancy. Essential for large multi-volume archives where a single catalog point of failure is unacceptable.

## Multi-Archive Medium Index

A lightweight index of all archive instances stored sequentially on one physical tape, enabling fast listing without scanning all tape files.

## Media Reopening After Archive End Frame

After an Archive End frame has been written, a future update mode could overwrite it and append new archive content. This may enable incremental tar-style updates.

Open questions:
- Safety on real tape devices and positioning rules.
- Distinguishing intentional reopen from accidental overwrite.
- Whether the previous Archive End frame should be recoverable.
- Interaction with multi-archive media and append-only safety policy.

## Streaming Data Validation

On read, buffer the entire frame payload in memory and verify `frame_hash` before emitting any output bytes. This ensures corrupted data is never forwarded to the consumer. For large frames, a streaming alternative (hash update during read, verify at end) could avoid OOM.

## Low-Level SCSI Passthrough

Optional low-level SCSI passthrough profiles for diagnostics only. Normal operation should use the standard sequential tape device interface.
