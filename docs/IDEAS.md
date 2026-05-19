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


## Multi-Channel Frame Content Types

Idea: allow arbitrary `frame_content_type` values beyond the current
`SLICE_CONTENT` and `SLICE_METADATA`, interleaved within the same logical
slice tape file.  Since each Frame already carries a global
`frame_seq_num_within_slice`, different content type streams can share the
same filemark boundary while remaining independently identifiable.

This could support use cases such as:

- Interleaving per-Frame payload hashes or Merkle tree nodes alongside
  SLICE_CONTENT data, enabling real-time integrity verification without a
  separate metadata pass.
- Multiplexing multiple logical payload channels (e.g. separate streams for
  data, parity, inline catalog) within a single slice, with each channel
  identified by its own `frame_content_type` value.
- Embedding diagnostics, progress snapshots, or writer heartbeats at
  predictable Frame positions without consuming SLICE_CONTENT payload bytes.
- A dedicated FEC (Forward Error Correction) channel: parity or Reed-Solomon
  recovery data interleaved as a separate `frame_content_type`, enabling the
  reader to repair bitrot within a slice without relying on an external
  recovery tool or a second archive pass.

Resolution (May 2026):

1. New `frame_content_type` values are allocated by future versions of this
   specification. No runtime registration or namespace scheme is needed.
2. Fully arbitrary mixing is allowed. The existing
   `frame_seq_num_within_slice` already provides unambiguous ordering, so
   content-type groups need not be contiguous.
3. A reader that does not recognize a new `frame_content_type` will fail
   before reaching it: the Volume Header declares `payload_profile` and
   `header_version`, and an unrecognized content type implies a format
   version the reader was not built for. Rejection at volume-open time is
   the correct behavior.

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
