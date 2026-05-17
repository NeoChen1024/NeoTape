# Ideas

This file collects early design ideas that are not yet part of the NeoTape
specification or roadmap.

## Reopening After Archive End Header

Idea: after an Archive End Header has been written, a future update mode could
erase or overwrite that Archive End Header and then append new archive content.

This may be useful if NeoTape later supports incremental tar-style updates. In
that model, a clean Archive End Header would mark the current closed state, but
the medium could potentially be reopened by removing that terminal marker,
writing additional payload and metadata, and then committing a new Archive End
Header.

Open questions:

- Whether this is safe on real tape devices and under which positioning rules.
- How to distinguish intentional reopen from accidental overwrite.
- Whether the previous Archive End Header should be recoverable or logged
  elsewhere before being replaced.
- How this interacts with multi-archive media and append-only safety policy.
- Whether incremental tar support should instead create a new archive instance
  rather than mutating the previous one.


## Use for Optical Media backup

* the spool directory structure can be used for making ISO images or even direct burning to optical media


## Strict Data Validation

Idea: on read, buffer the entire Frame payload in memory and verify
`frame_payload_blake3` before emitting any output bytes. This ensures that
corrupted or truncated data is never forwarded to the consumer.

Open question: for large Frames exceeding available memory, a streaming
alternative (hash update during read, verify at end) could avoid OOM while
still refusing output on mismatch. The strict buffer-verify-then-emit model
and the streaming hash-verify-then-report model are not equivalent — the
former guarantees zero bad output, the latter bounds memory at the cost of
potentially emitting partial data to a pipe before detecting corruption.
