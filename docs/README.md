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

The documentation should be split by stability and purpose. The goal is to keep
the future on-tape format readable while still leaving room for implementation
notes, experiments, and roadmap planning.

## Recommended Layout

```text
docs/
  README.md
  roadmap.md

  spec/
    terminology.md
    00-header-common.md
    01-medium-header.md
    02-volume-header.md
    03-segment-header.md
    04-archive-end-header.md
    06-volume-layout.md
    07-segments-and-slices.md
    08-continuation.md
    09-payload-profiles.md
    10-reader-state-machine.md
    11-error-handling.md
    12-security.md
    13-future-extensions.md
    appendix-layout-examples.md
    appendix-cli.md
    open-questions.md

  implementation/
    phase-0-pax-writer.md
    phase-1-header-layout.md
    build-and-dependencies.md
    libarchive-pax-notes.md
    blake3-notes.md
```

## Document Layers

### `spec/`

Use `docs/spec/` for the NeoTape format specification.

This is where normative or near-normative material belongs: tape model,
transport semantics, header fields, segment and slice rules, continuation
behavior, reader state machine, error handling, and payload profile contracts.

Phase 1 header byte layout work should primarily land in:

```text
docs/spec/terminology.md
docs/spec/00-header-common.md
docs/spec/01-medium-header.md
docs/spec/02-volume-header.md
docs/spec/03-segment-header.md
docs/spec/04-archive-end-header.md
```

### `implementation/`

Use `docs/implementation/` for implementation-specific notes.

This includes C++ design notes, GNU Makefile structure, libarchive behavior,
BLAKE3 integration, fixture strategy, and decisions that help the current
implementation without becoming part of the on-tape format.

Phase 1 implementation planning should primarily land in:

```text
docs/implementation/phase-1-header-layout.md
```

### `roadmap.md`

Keep the roadmap focused on milestones, status, and sequencing.

Avoid turning the roadmap into a second specification. If a milestone needs
technical depth, put that detail in `spec/` or `implementation/` and link to it.

## Current Drafts

The existing large drafts are still useful source material:

```text
docs/RFC_Draft.md
docs/ROADMAP.md
```

They should be gradually split into the structure above instead of edited into
even larger monolithic documents. Once a topic has been split into `docs/spec/`,
the `docs/spec/` version is authoritative.

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

The current `pax` tool can create POSIX pax archives that `bsdtar` can restore,
while preserving important filesystem metadata such as xattrs and hardlinks.
This validates the initial payload producer direction and lets Phase 1 focus on
NeoTape binary headers and transport framing.
