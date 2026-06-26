# NeoTape Documentation

This directory is the entry point for the active NeoTape specification,
implementation notes, and archived review material.

## Specification Precedence

`docs/spec/` is the authoritative home of the current NeoTape format and
protocol specification.

Use other directories for implementation-specific notes or historical material,
not for normative format rules.

## Layout

```text
docs/
  README.md                         This file

  spec/                             Active format and protocol specification
    00-format-common.md             Common datatypes, canonical hashing, signatures
    01-frame-header.md              Unified 512-byte header layout and flags
    02-terminology.md               Shared terms and definitions
    03-frames-and-slices.md         Frame model, channels, and sequence numbering
    04-volume-layout.md             Logical and physical volume layout
    05-spool-dir.md                 Spool directory format
    06-security.md                  Trust model, path safety, frame signing
    07-future-extensions.md         Reserved extension space and ideas
    08-plan-metadata.md             Plan metadata emitted by `neotape-plan`
    09-appendix-cli.md              CLI reference and examples
    10-appendix-layout-examples.md  Single-volume and multi-volume examples
    11-tcp-protocol.md              TCP/UDS protocol and writer auth handshake

  implementation/                   Implementation-specific notes
    lto-behavior-notes.md           Empirical LTO EOT/EOM observations
    mt-pax-architecture.md          mt-pax thread roles and data flow
    path-pitfalls.md                Path handling conventions and gotchas
    phase-3.5-mt-pax-writer.md      Historical mt-pax writer phase notes

  archive/                          Historical review and migration notes
    2026-06-17-project-review-findings.md
    2026-06-17-spec-consistency-review.md
```

## Document Layers

### `spec/`

Use `docs/spec/` for stable format commitments: unified frame header layout,
canonical `frame_hash` rules, frame-signature semantics, tape/spool layout,
and the TCP/Unix-domain socket protocol.

Security- and transport-related behavior is split intentionally:

- `06-security.md` covers trust model, path safety, and frame signing.
- `11-tcp-protocol.md` covers the writer/reader request-response protocol,
  including writer-side challenge-response authentication of the source server.

### `implementation/`

Use `docs/implementation/` for implementation-specific notes: empirical tape
behavior, mt-pax architecture, and local engineering conventions.

### `archive/`

Use `docs/archive/` for historical review documents and migration notes that
are useful context but are no longer the active spec.

## Splitting Guidance

- Put stable format commitments in `spec/`.
- Put implementation tradeoffs, local behavior notes, and architecture detail
  in `implementation/`.
- Put superseded reviews or one-off migration notes in `archive/`.
- Keep each file centered on one topic that is likely to change together.
- Prefer cross-links over repeating the same rule in multiple files.

## Current Focus

As of June 2026, the active spec reflects these major current-format decisions:

- NeoTape uses one unified 512-byte frame header for all records.
- `frame_hash` is a canonical BLAKE3 digest over the whole frame with the
  `signature` and `frame_hash` fields zeroed during hashing.
- Optional signed frames use a binary signify-style Ed25519 signature over
  `NeoTape-frame\0 || frame_hash`.
- In the writing pipeline, a verifying writer may authenticate the archiver by
  checking a signature over `NeoTape-auth\0 || nonce` before it accepts frames.
- Authoritative signed-frame validation in the reading pipeline remains the
  extractor's responsibility.
