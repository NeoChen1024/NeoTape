# Future Extensions

Status: extension ideas; non-normative.

This document collects extension ideas for future NeoTape versions. They are not part of the current format and should not be implemented until a later version defines them.

## Additional Channel Types

Extend `channel_type` beyond `ch_content`, `ch_metadata`, and `archive_end`. Values 3–254 are reserved. Use cases include:

- **Inline parity or FEC frames** — parity or Reed-Solomon recovery data as a separate channel type, enabling bitrot repair without external tools.
- **Multiplexed payload channels** — multiple logical streams within one slice.
- **Diagnostics embedding** — writer heartbeats or progress snapshots at predictable frame positions.
- **Real-time verification nodes** — Merkle tree nodes or hash-chain data alongside content data.

New `channel_type` values are allocated by future specification versions. Arbitrary mixing is allowed since `channel_frame_seq_num` provides per-channel ordering.

## Partial Restore Index

An index that maps file paths to their exact slice and frame positions, enabling targeted partial restore without scanning all slices.

## Changer/Robot Integration

Support for automated tape library changers: load/unload media, scan barcodes, select tapes by label.

## Reed-Solomon or Parity Frames

Dedicated parity frames within a slice for erasure coding. Allows recovery of a limited number of lost or damaged frames.

## Multiple Catalog Replicas

Store catalog replicas on multiple volumes for redundancy. Essential for large multi-volume archives where a single catalog point of failure is unacceptable.

## Media Reopening After Archive End Frame

After an Archive End frame has been written, a future update mode could overwrite it and append new archive content. This may enable incremental tar-style updates.

Open questions:

- Safety on real tape devices and positioning rules.
- Distinguishing intentional reopen from accidental overwrite.
- Whether the previous Archive End frame should be recoverable.
- Interaction with multi-archive media and append-only safety policy.
