# NeoTape Documentation

This directory is the entry point for NeoTape design notes, format drafts, and implementation planning.

## Specification Precedence

`docs/spec/` is the active home of the NeoTape format specification.

The documentation is split by stability and purpose.

## Layout

```text
docs/
  README.md                     This file

  spec/                         Normative or near-normative format specification
    00-format-common.md         Common format rules, datatypes, encoding, frame hash
    01-frame-header.md          Fixed header layout, channel types, flags, field semantics
    02-terminology.md           Common terms and definitions
    03-frames-and-slices.md     Frame model, channels, sequence numbering
    04-volume-layout.md         Logical and physical tape layout
    05-spool-dir.md             Spool directory format
    06-reader-state-machine.md  Reader processing model and state transitions
    07-error-handling.md        Retry/Inspect/Fail/Salvage model
    08-security.md              Trust model, path safety, signing
    09-open-questions.md        Unresolved design choices and open questions
    10-future-extensions.md     Extension ideas (multi-channel, FEC, encryption)
    11-plan-metadata.md         Plan metadata format (`neotape plan`)
    12-appendix-cli.md          CLI usage examples for all tools
    13-appendix-layout-examples.md  Single-volume, multi-volume, multi-archive layouts

  superpowers/
    specs/                      Design specs and proposals
      ...                       See individual files for archived and active designs
    plans/                      Implementation plans derived from designs

  implementation/               Implementation-specific notes
    phase-1-header-layout.md    Byte-offset decisions, serializer/parser design
    phase-3.5-mt-pax-writer.md  Multi-threaded pax writer architecture
    build-and-dependencies.md   Makefile structure, dependencies, source layout
    libarchive-pax-notes.md     libarchive call patterns, pax writer setup
    blake3-notes.md             BLAKE3 integration points and performance notes
    lto-behavior-notes.md       LTO EOT/EOM empirical observations
    tape-append-semantics.md    Tape backend append safety and initialization
    mt-pax-architecture.md      mt-pax thread roles and data flow
    spool-optical-backup.md     Spool backend as optical/removable media staging
    path-pitfalls.md            Path handling conventions and gotchas
```

## Document Layers

### `spec/`

Use `docs/spec/` for the NeoTape format specification: tape model, unified frame header layout, channel semantics, frame and slice rules, continuation behavior, reader state machine, error handling, and open questions.

NeoTape uses a **single unified Frame Header** for all records. The header layout is specified in `docs/spec/01-frame-header.md`.

### `implementation/`

Use `docs/implementation/` for implementation-specific notes: C++ design decisions, Makefile structure, libarchive behavior, BLAKE3 integration, empirical LTO observations, and path handling gotchas.

## Splitting Guidance

- Put stable format commitments in `spec/`.
- Put experiments, implementation tradeoffs, and local build notes in `implementation/`.
- Keep each file centered on one topic that is likely to change together.
- Avoid very tiny files for every subsection; split where ownership and edit frequency naturally separate.
- Prefer cross-links over repeating the same rule in multiple files.
- Keep open questions close to the topic they affect, then summarize broad unresolved items in `spec/08-open-questions.md`.

## Key Format Changes (June 2026)

The format was significantly simplified in June 2026:

- **Single unified 512-byte Frame Header** replaces the three previous headers (Volume, Frame, Archive End).
- **`channel_type`** (`ch_content`, `ch_metadata`, `archive_end`) replaces `header_type` and `frame_content_type`.
- **`frame_hash`** (BLAKE3 over the entire frame) replaces `header_crc32c` and `frame_payload_blake3`.
- **`volume_seq_num`** is now advisory; archive continuity is validated by `archive_uuid` and sequence numbers.
- **No payload profile** in core format; core is payload-format agnostic.
- **No slice-level hash**; per-frame `frame_hash` is sufficient.
- **Channel-local `frame_seq_num_within_channel`** replaces `frame_seq_num_within_slice`.
- **Metadata-first ordering**: `ch_metadata` MUST precede `ch_content` within each logical slice.
- **`volume_block_size_kib`** (`uint16`, KiB-encoded) replaces `volume_block_size` (`uint32`, bytes).
