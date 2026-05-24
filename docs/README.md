# NeoTape Documentation

This directory is the entry point for NeoTape design notes, format drafts, and
implementation planning.

## Specification Precedence

`docs/spec/` is the active home of the NeoTape format specification. When a
topic is defined in `docs/spec/`, that definition replaces and supersedes the
corresponding material in `docs/RFC_Draft.md`.

`docs/RFC_Draft.md` remains useful as source material, rationale, and historical
draft context. It should not be treated as authoritative for sections that have
already been split into `docs/spec/`.

The documentation is split by stability and purpose.

## Layout

```text
docs/
  README.md                     This file
  ROADMAP.md                    Implementation milestones and sequencing
  IDEAS.md                      Early design ideas (not yet in spec/roadmap)
  RFC_Draft.md                  Historical draft (superseded by spec/)

  spec/                         Normative or near-normative format specification
    terminology.md              Common terms and definitions
    00-format-common.md         Common format rules, datatypes, encoding
    01-medium-header.md         Medium Header field inventory
    02-volume-header.md         Volume Header field inventory
    03-frame-header.md          Frame Header field inventory
    04-archive-end-header.md    Archive End Header field inventory
    05-spool-dir.md             Spool directory format
    06-volume-layout.md         Logical and physical tape layout
    07-frames-and-slices.md     Frame model, slice completion, content types
    08-payload-profiles.md      Payload profile definitions (PAX, raw)
    09-reader-state-machine.md  Reader transitions and validation
    10-error-handling.md        Retry/Inspect/Fail/Salvage model
    11-security.md              Trust model, path safety, authentication
    12-future-extensions.md     Extension ideas (multi-channel, FEC, encryption)
    appendix-layout-examples.md Single-volume, multi-volume, multi-archive layouts
    appendix-cli.md             CLI usage examples for all tools
    open-questions.md           Unresolved design choices and open questions
    plan-metadata.md            Plan metadata format (neotape-plan)

  implementation/               Implementation-specific notes
    phase-1-header-layout.md    Byte-offset decisions, serializer/parser design
    phase-3.5-mt-pax-writer.md  Multi-threaded pax writer architecture and EOA suppression
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

Use `docs/spec/` for the NeoTape format specification: tape model, transport
semantics, header fields, Frame and slice rules, continuation behavior, reader
state machine, error handling, payload profile contracts, and open questions.

### `implementation/`

Use `docs/implementation/` for implementation-specific notes: C++ design
decisions, Makefile structure, libarchive behavior, BLAKE3 integration,
empirical LTO observations, and path handling gotchas.

### `ROADMAP.md`

Keep the roadmap focused on milestones, status, and sequencing. Avoid turning
it into a second specification.

### `IDEAS.md`

Early-stage ideas that are not yet part of the spec or roadmap. May feed into
future extensions.

## Splitting Guidance

- Put stable format commitments in `spec/`.
- Put experiments, implementation tradeoffs, and local build notes in
  `implementation/`.
- Keep each file centered on one topic that is likely to change together.
- Avoid very tiny files for every subsection; split where ownership and edit
  frequency naturally separate.
- Prefer cross-links over repeating the same rule in multiple files.
- Keep open questions close to the topic they affect, then summarize broad
  unresolved items in `spec/open-questions.md`.

## Phase 0 Status

Phase 0 is complete enough to move on to Phase 1 discussion.

The current `mt-pax` tool can create POSIX pax archives that `bsdtar` can restore,
while preserving important filesystem metadata such as xattrs and hardlinks.
This validates the initial payload producer direction and lets Phase 1 focus on
NeoTape binary headers and transport framing.
