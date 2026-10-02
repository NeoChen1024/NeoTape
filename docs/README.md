# NeoTape Documentation

This directory is the entry point for the active NeoTape specification and
implementation notes.

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
    01-terminology.md               Shared terms and definitions
    02-frame-header.md              Unified 512-byte header layout and flags
    03-frames-and-slices.md         Frame model, channels, and sequence numbering
    04-validation.md                Shared conformance and validation rules
    05-volume-layout.md             Logical and physical volume layout
    06-spool-dir.md                 Spool directory format
    07-tcp-protocol.md              TCP/UDS protocol and writer auth handshake
    08-security.md                  Trust model, path safety, frame signing
    09-plan-metadata.md             Plan metadata emitted by `neotape-plan`
    10-appendix-layout-examples.md  Single-volume and multi-volume examples
    11-future-extensions.md         Reserved extension space and ideas

  implementation/                   Implementation-specific notes
    2026-09-refactor.md             CLI/streaming refactor plan and progress
    cli-tooling.md                  CLI reference and workflow examples
    lto-behavior-notes.md           Empirical LTO EOT/EOM observations
    lto5-hardware-regression-2026-10-02.md  Physical signed four-volume regression
    mt-pax-architecture.md          mt-pax thread roles and data flow
    path-pitfalls.md                Path handling conventions and gotchas
    phase-3.5-mt-pax-writer.md      Historical mt-pax writer phase notes
    recovery-bundle.md              BOT recovery bundle packaging strategy
    cmake-build-system.md           CMake migration decisions and verification
```

## Document Layers

### `spec/`

Use `docs/spec/` for stable format commitments: unified frame header layout,
canonical `frame_hash` rules, frame-signature semantics, tape/spool layout,
and the TCP/Unix-domain socket protocol.

Security- and transport-related behavior is split intentionally:

- `08-security.md` covers trust model, path safety, and frame signing.
- `07-tcp-protocol.md` covers the writer/reader request-response protocol,
  including writer-side challenge-response authentication of the source server.
- `04-validation.md` centralizes the conformance checks shared by readers,
  extractors, spool readers, TCP receivers, and `neotape-inspect`.

### `implementation/`

Use `docs/implementation/` for implementation-specific notes: empirical tape
behavior, mt-pax architecture, and local engineering conventions.

CLI usage and operator workflows live in `docs/implementation/cli-tooling.md`,
not in `docs/spec/`.

## Splitting Guidance

- Put stable format commitments in `spec/`.
- Put implementation tradeoffs, local behavior notes, and architecture detail
  in `implementation/`.
- Delete superseded reviews and one-off migration notes; git history keeps
  them.
- Keep each file centered on one topic that is likely to change together.
- Prefer cross-links over repeating the same rule in multiple files.
