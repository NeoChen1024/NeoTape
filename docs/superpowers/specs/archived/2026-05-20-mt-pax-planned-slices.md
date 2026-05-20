# mt-pax Planned Slices and Library Interface

Date: 2026-05-20
Status: Approved design

## Problem

`src/mt-pax.cpp` is currently a standalone CLI implementation. It walks source
paths, serializes pax entries with worker threads, and writes one continuous pax
byte stream to an output file or stdout.

Future NeoTape archive writing needs the same pax producer as a reusable
component. It also needs optional support for `neotape-plan` metadata so a prior
planning pass can choose slice boundaries and downstream tools can produce pax
bytes in that planned slice order.

The CLI should remain useful as a development/debug tool. In planned mode it
should be able to materialize each planned slice as a separate file containing
exactly the pax payload bytes that would be written into NeoTape frames.

## Decision

Refactor the multi-threaded pax producer into a reusable library and keep
`bin/mt-pax` as a thin command-line wrapper around that library.

The library exposes callback-oriented output. It does not own output files and
does not know about tape, spool directories, or NeoTape Frame Headers. Consumers
receive ordered pax bytes and slice lifecycle events.

Two input modes are supported:

- Unplanned mode walks user-provided source paths with the existing
  `archive_read_disk` behavior and emits one logical slice, slice `0`.
- Planned mode reads `neotape-plan` metadata, applies `/chdir/<path>`
  directives, processes entry records in plan order, and emits a new logical
  slice whenever the record slice number changes.

## Plan Metadata Semantics

The existing `docs/spec/plan-metadata.md` format remains unchanged.

For each planned entry record:

```text
/<slice>/<file_num>/<kind>/<size>/<filepath>\0\n
```

`<filepath>` is used as both:

- the filesystem path to stat/open after applying the current `/chdir`
  directive, and
- the pax archive pathname written into the output stream.

This matches the current plan metadata model and avoids adding source-path
fields. Planned mode therefore assumes plan creation and plan consumption use
the same working-directory semantics.

## Library Interface

Add a public header, `include/neotape/pax_writer.hpp`, and an implementation,
`src/neotape_pax_writer.cpp`.

The interface contains:

- `PaxWriterOptions` for source paths, optional plan path, `-C`, `-x`,
  `--io-thread`, output buffer sizing, waterline percent, and verbosity.
- `PaxWriterCallbacks` for `begin_slice`, `write_chunk`, and `end_slice`.
- `PaxWriteResult` with bytes written, entries walked/emitted, slice count, and
  the final BLAKE3 digest over the concatenated pax bytes.
- `write_pax(PaxWriterOptions, PaxWriterCallbacks)` as the primary entry point.

The callback contract is synchronous from the caller's perspective: when
`write_pax` returns successfully, all slice events and byte chunks have been
delivered or an exception has been thrown.

## CLI Behavior

Existing unplanned behavior remains compatible:

```sh
bin/mt-pax -f <out-file|-> [-v|-vv] [-x] [-C <dir>] \
           [-P <buffer-percent>] [--io-thread <N>] \
           [--output-buffer-size <bytes>] <path> [path ...]
```

New planned/debug behavior:

```sh
bin/mt-pax --plan <plan-file> --slice-output-prefix <prefix> \
           [-v|-vv] [-P <buffer-percent>] [--io-thread <N>] \
           [--output-buffer-size <bytes>]
```

The CLI writes one file per planned slice using six-digit zero-padded slice
numbers, for example:

```text
<prefix>000000.pax
<prefix>000001.pax
```

Slice debug files contain raw pax payload bytes only. They do not include pax
End-of-Archive markers. Concatenating all slice files in slice order produces
the same raw pax byte stream that a NeoTape writer would frame as
`SLICE_CONTENT`.

`-f` remains the output selector for continuous stream mode. `--plan` with
`--slice-output-prefix` selects multi-file debug output. Supplying both `-f` and
`--slice-output-prefix` is invalid because they represent different output
shapes.

## Data Flow

The existing mt-pax pipeline is preserved internally:

- Metadata-only entries are serialized inline and queued.
- Small regular files are serialized by worker threads.
- Large regular files are streamed by the serializer through a bounded output
  buffer.
- The serializer merges results by sequence number to preserve plan or walk
  order.

The output thread is generalized from `FILE *` writes to a callback sink. It
still supports the buffer waterline behavior and still computes BLAKE3 over the
bytes it delivers.

In planned mode, the entry dispatcher obtains filesystem metadata for each plan
entry directly from `<filepath>` rather than traversing directories. Hardlink
resolution still uses `archive_entry_linkresolver` over entries in plan order.

## Error Handling

The library reports fatal errors by throwing `std::runtime_error` or a more
specific standard exception. The CLI catches exceptions and prints diagnostics
with the `pax:` prefix to preserve existing user-facing style.

Non-fatal filesystem or libarchive warnings remain warnings on stderr. Entries
that cannot be opened are skipped consistently with current mt-pax behavior.

Plan parsing errors are fatal and include the plan path and record number where
possible.

## Testing

Verification should cover:

- Existing unplanned `mt-pax` output still builds and produces a pax stream.
- A small fixture planned with a tiny `--slice-size` produces multiple slice
  files.
- Concatenating planned slice files yields a pax stream that `bsdtar -tf -` can
  list, allowing for missing-EOA behavior.
- Planned `/chdir` records are honored when resolving entry paths.
- `make -j "$(nproc)"` succeeds.

## Non-Goals

- Do not change the `neotape-plan` metadata format.
- Do not make slice files independently valid tar archives.
- Do not add NeoTape Frame Header emission to `mt-pax`.
- Do not require full pax stream buffering in planned mode.
