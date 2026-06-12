# Open Questions and Unresolved Design Choices

Status: non-normative.

This document collects open questions, unresolved design choices, and decision points that affect the NeoTape format but have not yet been settled.

## Format and Layout

### Recovery Bundle Format and Member Set

The exact format, member set, and placement rules for an optional recovery
bundle at BOT are open. Should the spec recommend a plain pax tar with a fixed
member set (README, RESTORE, FORMAT-SPEC, reader source), or leave it entirely
to deployment?

### Spool Filenames

Should spool filenames (e.g. `tape-file-000002.slice-000001.ntf`) be normative
or advisory? If advisory, should the manifest be the canonical record locator?

### Manifest Status

Is `manifest.json` purely advisory, or should it have a partially standardized
schema that readers can rely on when headers are damaged?

### Frame Header Repeated Fields

Frame Header repeats `archive_uuid`, `archive_name`, `volume_block_size`, and
`payload_profile`. Should the Frame Header carry a volume reference instead
(e.g. `volume_seq_num` only) to reduce fixed-header overhead, trading off
self-description for space?

### SLICE_METADATA Item Schema

The exact metadata item table byte layout and catalog entry format are not
settled. Should the catalog be binary-only, text-friendly, or dual-layer with
binary records plus optional human-readable summaries?

## EOT and Continuation

### Partial Frame After EOT

If EOT occurs after a Frame Header is committed but before the full payload is
written, should there be a mechanism to resume at the exact byte offset within
the Frame, or must the entire Frame be retransmitted? Current design: no partial
Frame continuation; the next Frame covers the remaining payload range.

### Post-ENOSPC Early Warning Region

LTO drives can sometimes complete a write after an ENOSPC early-warning.
Should NeoTape reserve a small backend-specific area for close-out or
synchronization operations after the first ENOSPC?

## Reader Behavior

### Reader State Machine Exhaustiveness

Should the reader state machine be fully specified with every transition and
error exit, or is the current high-level model sufficient for interoperable
implementations?

### Error Class Definitions

- Which errors are retryable vs. fatal?
- Which errors should produce non-zero exit codes vs. warnings on stderr?
- Should exit codes be standardized across all NeoTape tools?

### Strict vs. Permissive Reading

Should the reader validate zero padding in Frames in normal mode, or only in
strict mode? Current spec: "SHOULD validate in strict mode, MAY ignore in salvage mode."

## Writer Behavior

### Slice Target Size Heuristic

Slice target size is a writer heuristic (commonly ~64 GiB). Should the format
recommend a specific size, or leave it entirely to implementation? How should
writers adjust slice size for payload profiles with different structure (e.g.
pax member boundaries)?

### Per-Slice EOA for PAX Profile

Should the NeoTape/PAX profile mandate per-slice pax EOA for independent slice
restorability, or keep the current design where only the concatenated stream
needs a final EOA?

## Safety and Recovery

### Encrypted Payload Profiles

How should NeoTape handle encrypted payload profiles while preserving catalog
usability? Catalog entries for encrypted payloads would need to avoid leaking
plaintext filenames.

### Incremental Archive Updates

Should incremental tar-style updates modify an existing archive instance
(erase Archive End Header, append, rewrite end header) or create a new archive
instance on the same medium? The former is more space-efficient but riskier.

### Multi-Archive Medium Index

Should NeoTape maintain a lightweight index of all archive instances on a
medium, or should discovery always be by scanning tape files? An index would
speed up listing but requires mutable state that is hard to maintain on
append-only media.

## Implementation

### Minimal Reader Embedding

Should the minimal reader (`neotape restore`) be compact enough to embed in the
recovery bundle as source code? What platform assumptions would such a reader
make?

### Minimum Supported Platform Set

What is the minimum platform set for v0.1? Linux only? Linux + FreeBSD + macOS?

### CI and Test Vectors

How should release artifacts include format test vectors and compatibility
fixtures? Golden spool archives and golden pax archives need a home.

### Hardware Compression

Should NeoTape account for LTO hardware compression when reporting or limiting
bytape capacity? Native byte tracking is simpler but may not reflect physical
tape occupancy.

## Resolved Decisions (for reference)

1. **C++ with GNU Makefile** — closed in Phase 0. No CMake, no Rust, no
   language changes for v0.1.
2. **stdout = pure payload bytes** — closed. All diagnostics on stderr or
   `/dev/tty`.
3. **No mutable state in the archive stream** — closed. Archive discovery by
   scanning tape files.
4. **Archive-level completion by Archive End Header only** — closed. EOD alone
   is not sufficient.
5. **BLAKE3 as preferred integrity hash** — closed. SHA-256 only as
   compatibility metadata if explicitly requested.
6. **No mandatory trailer rewrite** — closed. The format does not require
   seeking back to BOT.
7. **Length-framed transport** — closed. Frame boundaries by `frame_payload_size`,
    not by payload parsing.
8. **Default archive volume block size** — closed for the current CLI. `backup`
   and `write` default to 4 MiB unless explicitly overridden.
9. **Profile-specific stdout finalization** — closed for the current CLI. Core
   raw `read` emits stored bytes as-is; PAX-profile `restore` appends pax EOA
   finalization.
10. **Raw vs PAX command split** — closed for the current CLI. `write/read` are
    raw-profile commands; `backup/restore` are PAX-profile commands.
