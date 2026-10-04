# Future Extensions

Status: extension ideas; non-normative.

This document collects extension ideas for future NeoTape versions. They are not part of the current format and should not be implemented until a later version defines them.

## Additional Channel Types

Extend `channel_type` beyond `ch_content`, `ch_metadata`, and `archive_end`. Values 0 and 3–254 are reserved.

New `channel_type` values would be allocated by future specification versions.
Channels of a slice may already be interleaved freely, so an extension would
only need to define its payload, completion rules, and any interaction with
existing channels.

## Sideband Data Area

The 128-byte `sideband_data` area in the fixed header (see [02-frame-header.md](02-frame-header.md)) is reserved for channel-type-specific extensions. The `SIDEBAND` flag marks a frame as carrying meaningful sideband data; the encoding, internal layout, and per-frame consistency rules are defined by the `channel_type` that uses it.

In `header_version=1`, no defined `channel_type` sets `SIDEBAND`; every frame requires `sideband_data` to be all zero. Candidate future uses, each gated on a new `channel_type` allocation, include:

- Per-frame Merkle/proof nodes for real-time verification.
- Partial-restore index pointers.

A new `channel_type` that uses `sideband_data` MUST specify its internal structure (including any type/version tag if multiple sub-encodings are possible) and whether the data must be constant within a frame, slice, channel group, or archive.

## Partial Restore Index

An index that maps file paths to their exact slice and frame positions, enabling targeted partial restore without scanning all slices.

## Slice-Level Resume

Restart an interrupted archive run from the slice after the last completely
written one, instead of from the beginning.

This depends on slice boundaries being fixed ahead of time by a plan file.
Replanning the same source tree may produce different boundaries, and a file
could then fall on the already-written side of a moved boundary and be left
out of the archive. The resumed run must therefore reuse the original plan; archives created without a plan
cannot be resumed this way.

Open questions:

- How the last completed slice is determined: writer-side state, or reading
  the media back.
- Positioning and overwrite rules for the partially written slice.
- Sequence, validation, and archive identity state for the resumed run.

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

## Repair Channels

`header_version=1` previously defined a `ch_fec` channel (value 3) carrying
local RS(32,4) repair records after every 32 content records, together with a
`FEC_PROTECTED` flag (bit 3). It was removed because the failures it covered,
an isolated one to four unreadable records, are rare on LTO: the drive's
read-after-write verification and dataset-level ECC leave either no residual
error or a damaged region far larger than one group.

A future repair channel must work within the streaming model: one drive,
sequential volumes, bounded memory, and no seeking back. Redundancy across
volumes (RAIT-style parity) needs either several drives writing in parallel
or a volume-sized disk staging area, and is outside that model. Operators who
need media redundancy should write independent copies.
