# Future Extensions

Status: extension ideas; non-normative.

This document collects extension ideas for future NeoTape versions. They are not part of the current format and should not be implemented until a later version defines them.

## Additional Channel Types

Extend `channel_type` beyond `ch_content`, `ch_metadata`, `ch_fec`, and `archive_end`. Values 0 and 4–254 are reserved.

New `channel_type` values would be allocated by future specification versions.
Each extension would need to define allowed ordering, interaction with existing
channels, and completion rules. Per-channel sequence numbers alone do not
permit arbitrary interleaving.

## Sideband Data Area

The 128-byte `sideband_data` area in the fixed header (see [02-frame-header.md](02-frame-header.md)) is reserved for channel-type-specific extensions. The `SIDEBAND` flag marks a frame as carrying meaningful sideband data; the encoding, internal layout, and per-frame consistency rules are defined by the `channel_type` that uses it.

In `header_version=1`, `ch_fec` is the only defined `channel_type` that sets `SIDEBAND`; `ch_content`, `ch_metadata`, and `archive_end` still require `sideband_data` to be all zero. Candidate future uses, each gated on a new `channel_type` allocation, include:

- Per-frame Merkle/proof nodes for real-time verification.
- Partial-restore index pointers.

A new `channel_type` that uses `sideband_data` MUST specify its internal structure (including any type/version tag if multiple sub-encodings are possible) and whether the data must be constant within a frame, slice, channel group, or archive.

## Partial Restore Index

An index that maps file paths to their exact slice and frame positions, enabling targeted partial restore without scanning all slices.

## Changer/Robot Integration

Support for automated tape library changers: load/unload media, scan barcodes, select tapes by label.

## Multiple Catalog Replicas

Store catalog replicas on multiple volumes to improve listing and partial
restore availability. Catalogs remain advisory; their loss must not prevent
basic payload restoration.

## Media Reopening After Archive End Frame

After an Archive End frame has been written, a future update mode could overwrite it and append new archive content. This may enable incremental tar-style updates.

Open questions:

- Safety on real tape devices and positioning rules.
- Distinguishing intentional reopen from accidental overwrite.
- Whether the previous Archive End frame should be recoverable.
- Interaction with multi-archive media and append-only safety policy.
