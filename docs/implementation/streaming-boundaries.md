# Streaming and media boundaries

Status: implementation note. These boundaries come from the September 2026
CLI and streaming refactor; its work log is in git history.

## Constraints

- Bounded asynchronous output with ACK-based retention and back-pressure.
- Writer exit codes 0/1/2/3 and the archiver's mbuffer-style progress line.
- `TapeDevice` keeps the positioning the tools use (rewind, space to EOD,
  forward filemarks and records). `RecordSink` narrows streaming writes
  only; it does not remove media control.
- `neotape-scan` reads only the first record of each tape file, and
  `neotape-dump` copies without validation.
- Mechanisms are shared; validation and reporting policy stay with each tool.

## Module boundaries

| Component | Owns | Deliberately leaves to callers |
| --- | --- | --- |
| `writer.hpp` / `write_volume` | Validation, bounded queue, ordered record/filemark/archive-end writes, ACK, terminal result | Authentication before opening media, rewind/append, bundle installation, CLI exit mapping |
| `RecordSink` | Streaming writes and capacity accounting; null discards through the same session | Tape positioning and ioctl controls remain on the caller-owned `TapeDevice` |
| `media.hpp` | Media locators, spool ordering, record/filemark/EOD events, tape-file skip | Archive validation, salvage policy, display; scan still reads only the first header per spool file |
| `progress.hpp` | Timer lifetime, rates, count formatting and buffer ratio | Tool-specific mbuffer renderers and final diagnostics |
| `plan.hpp` | NUL-LF framing and plan metadata codec, one-record-at-a-time input | Traversal, slice packing, filesystem metadata collection |
| Pax archive session | Common resource cleanup, link resolution, slice pipeline and end marker | Planned input and directory traversal retain their distinct entry sources |

The writer output thread is the sole ACK owner. A successfully written final
record follows the same path as content; EOT/error wakes both bounded-queue
waiters and a blocked socket receiver. Successful write is not a new durability
guarantee: tape/spool persistence semantics remain those of the existing backend.
Progress snapshots in the writer are taken under one state lock.

`TapeDevice` keeps only the positioning the tools use: `rewind`,
`space_to_eod`, `space_fwd` (filemarks) and `space_fwd_records`.
Resuming an interrupted archive does not use them: the operator positions the
medium and `neotape-write --resume-mode` writes from there, while the archiver
restores identity and sequence state from its resume file (see the
[CLI reference](cli-tooling.md#resuming-an-interrupted-archive)). Resume
granularity is the slice; there is no checkpoint inside a slice.

Tape readers start at the current position; `RecordReader` never rewinds.
