# Future Extensions

Status: extracted from RFC_Draft.md §23 and IDEAS.md; non-normative.

This document collects extension ideas for future NeoTape versions. They are not part of the v0.1 format and should not be implemented until a later version defines them.

## LTO Partition Metadata Mode

LTO tape drives support partitioning (e.g. two partitions on a single tape). A future version could use a metadata partition to store catalogs, indices, or recovery data separately from the payload partition.

## Per-Frame Hash Chain

Chain Frame payload hashes by including the previous Frame's `frame_payload_blake3` in the current Frame Header. This creates a hash chain that allows the reader to detect missing or reordered Frames.

## Per-Slice Merkle Tree

Build a Merkle tree over Frame payload hashes within a slice, recording the root in the END Frame Header. This enables efficient verification of individual Frame ranges without reading the entire slice.

## Multi-Channel Frame Content Types

Extend `frame_content_type` beyond `SLICE_CONTENT` and `SLICE_METADATA` to support interleaved data channels within a single slice tape file. Use cases include:

- **Inline per-Frame hashes or Merkle tree nodes** alongside SLICE_CONTENT data, enabling real-time integrity verification.
- **Multiplexed payload channels** — multiple logical streams (data, parity, inline catalog) within one slice.
- **Diagnostics embedding** — writer heartbeats or progress snapshots at predictable Frame positions.
- **FEC (Forward Error Correction)** — parity or Reed-Solomon recovery data interleaved as a separate content type, enabling bitrot repair without external tools.

New `frame_content_type` values are allocated by future versions of the specification. No runtime registration scheme is needed. Arbitrary mixing is allowed since `frame_seq_num_within_slice` provides unambiguous ordering.

## Partial Restore Index

An index that maps file paths to their exact logical slice and Frame positions, enabling targeted partial restore without scanning all slices.

## Filesystem-Native Payload Profiles

Support for ZFS send streams and Btrfs send streams:

- Each dataset, subvolume, or snapshot send stream is a payload sub-stream that normally maps to one or more logical slices.
- A logical slice SHOULD NOT contain bytes from more than one sub-stream unless explicitly allowed by that payload profile.
- Catalog fields for dataset/subvolume identity, snapshot names, parent snapshot dependencies, receive order, stream-level checksums, and slice ranges.

NeoTape core remains payload-format agnostic; restore semantics are handled by the receive tool.

## Changer/Robot Integration

Support for automated tape library changers: load/unload media, scan barcodes, select tapes by label.

## Machine-Readable JSON Control Protocol

A JSON-based control protocol over a separate channel for library automation, instead of parsing stderr output.

## Reed-Solomon or Parity Frames

Dedicated parity frames within a slice for erasure coding. Allows recovery of a limited number of lost or damaged Frames.

## Encryption and Key Metadata

Support for encrypted payload profiles with key metadata stored in the catalog or metadata bundle. Preserve catalog usability without exposing plaintext filenames.

## Multiple Catalog Replicas

Store catalog replicas on multiple volumes for redundancy. Essential for large multi-volume archives where a single catalog point of failure is unacceptable.

## Multi-Archive Medium Index

A lightweight index of all archive instances stored sequentially on one physical tape, enabling fast listing without scanning all tape files.

## Media Reopening After Archive End Header

After an Archive End Header has been written, a future update mode could erase or overwrite that header and then append new archive content. This may enable incremental tar-style updates where the medium can be reopened by removing the terminal marker, writing additional payload and metadata, and committing a new Archive End Header.

Open questions:
- Safety on real tape devices and positioning rules.
- Distinguishing intentional reopen from accidental overwrite.
- Whether the previous Archive End Header should be recoverable.
- Interaction with multi-archive media and append-only safety policy.
- Whether incremental updates should instead create a new archive instance.

## Strict Data Validation Mode

On read, buffer the entire Frame payload in memory and verify `frame_payload_blake3` before emitting any output bytes. This ensures corrupted data is never forwarded to the consumer.

For large Frames exceeding available memory, a streaming alternative (hash update during read, verify at end) could avoid OOM while still refusing output on mismatch. The two models are not equivalent: the former guarantees zero bad output, the latter bounds memory at the cost of potentially emitting partial data to a pipe before detecting corruption.

## Low-Level SCSI Passthrough

Optional low-level SCSI passthrough profiles for diagnostics only. Normal operation should use the standard sequential tape device interface.
