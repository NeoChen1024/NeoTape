# NeoTape — Agent guide

## What it is

LTO tape-oriented multi-volume length-framed backup container. A pax writer
exists: `bin/mt-pax` (multi-threaded with `--io-thread`, `src/mt-pax.cpp`).
A planner (`bin/neotape-plan`) exists for slice metadata. A split producer/writer
pair (`bin/neotape-archiver` or `bin/neotape-raw-store`, and `bin/neotape-write`)
generates and consumes NeoTape-framed records over a TCP or Unix-domain socket.
The tape-device backend
(`namespace mt`) remains available for future tools.

## Build

```sh
make -j "$(nproc)"      # produces bin/mt-pax, bin/neotape-archiver,
                        # bin/neotape-raw-store, bin/neotape-write,
                        # bin/neotape-plan, and NeoTape helper tools
make clean
```

Dependencies: libarchive (system, `-larchive`), BLAKE3 (bundled submodule
`3rdparty/BLAKE3` → `lib/libb3sum.a`), and crc32c (bundled submodule
`3rdparty/crc32c` → `lib/libcrc32c.a`). mt-st is a submodule but unused in
the build.

## clangd / LSP

The repository includes `.clangd` and `compile_commands.json` so that clangd
can parse the project out of the box. If you add or remove source files,
regenerate the compilation database:

```sh
make compile_commands
```

clangd is configured to use GCC 15's libstdc++ headers because GCC 16's
headers currently confuse clangd 22.

## Header Parser

- NeoTape uses one handwritten unified 512-byte Frame Header parser/serializer.
- `include/neotape/format.hpp` declares the format API.
- `src/neotape_format.cpp` owns byte offsets, parsing, serialization, and frame-hash helpers.
- Do not reintroduce header codegen unless the format grows multiple independent layouts again.

## Project layout

- `src/mt-pax.cpp` — multi-threaded pax writer (worker pool, serializer, streaming large files)
- `src/neotape_plan_cmd.cpp` — planner for slice metadata
- `src/neotape_archiver_cmd.cpp` — `bin/neotape-archiver` CLI entry point
- `src/neotape_raw_store_cmd.cpp` — `bin/neotape-raw-store` raw-stream server CLI entry point
- `src/neotape_write_cmd.cpp` — `bin/neotape-write` CLI entry point
- `src/neotape_tcp_server.cpp` — archiver server and frame packing
- `src/neotape_tcp_protocol.cpp` — framed TCP message I/O
- `src/neotape_*.cpp` — NeoTape format and tape manipulation library (format layer and `namespace mt`)
- `include/neotape/tcp_protocol.hpp` — TCP archive protocol message types
- `include/neotape/tcp_server.hpp` — archiver server interface
- `include/neotape/bounded_buffer.hpp` — thread-safe bounded buffer used by mt-pax
- `include/neotape/` — shared project headers (common types, format helpers)
- `3rdparty/` — git submodules (BLAKE3, crc32c, mt-st). Init with `git submodule update --init --recursive`
- `docs/spec/` — active format spec; `docs/mt-pax.md` — mt-pax architecture
- `tests/smoke_mt_pax_pipeline.sh`, `tests/smoke_tcp_archive.sh`, `tests/smoke_raw_store.sh`, `tests/smoke_mt_pax_parity.sh` — smoke tests; no test framework or CI yet

## Architecture pattern

NeoTape separates long-running data producers/consumers from short-lived tape
I/O clients over a single TCP or Unix-domain socket:

- **Listener / long-running role** (`neotape-archiver`, `neotape-raw-store`, future `neotape-extractor`)
  owns archive state (archive UUID, volume sequence, and frame sequence numbers) and serves
  fully-formed NeoTape records through a framed request-response protocol. It
  stays up for the lifetime of the archive and does not know about physical
  media changes.

- **Tape client / short-lived role** (`neotape-write`, future `neotape-read`)
  connects to a listener, requests one volume's worth of data, and writes it to
  a tape device, spool directory, or raw file. One client process handles
  exactly one volume; when it reaches end-of-tape it writes a trailing filemark
  and exits, letting the operator mount a new medium and start another client.

This split lets archive generation/extraction run on one host while tape
hardware is attached to another, keeps media handling out of the archive state
machine, and provides natural back-pressure because the client requests frames
one at a time.

## mt-pax CLI

```
bin/mt-pax -f <out-file|-> [-v|-vv] [-x] [-C <dir>]
           [-P <buffer-percent>] [--io-thread <N>]
           [--output-buffer-size <bytes>] <path> [path...]
```

All `pax` options plus:

- `--io-thread <N>` — total I/O threads (default 1).  `N=1` uses no worker
  threads (serializer reads all files directly, large and small).  `N>1`
  spawns `N-1` workers for small files and streams large files via the
  serializer.
- `--output-buffer-size <bytes>` — size of the internal output
  `BoundedBuffer` (default 64 MB).  Larger values reduce write fragmentation
  at the cost of memory.
- `-P <percent>` — waterline write restart threshold as percentage of the
  output buffer (default 0).  When non-zero the output thread waits until
  the buffer reaches at least this full before starting to drain, useful for
  sequential writes on HDD/tape.

## neotape-archiver CLI

```
bin/neotape-archiver --listen <tcp://host:port|unix://path>
                     [--volume-block-size <bytes>] [--archive-name <name>]
                     [-C <dir>] [-P <percent>] [--io-thread <N>]
                     [--output-buffer-size <bytes>] [--plan <file>]
                     [-v|-vv] [-x] <path> [path...]
```

In server mode (`--listen`) the archiver is a long-running producer that serves
NeoTape records over a single TCP/UDS connection. Without `--listen` it behaves
like `mt-pax` and writes a plain pax stream to `-f`.

## neotape-raw-store CLI

```
bin/neotape-raw-store --listen <tcp://host:port|unix://path>
                       [--input <file|->]
                       [--volume-block-size <bytes>]
                       [--archive-name <name>]
                       [--retention-frame-count <N>] [--debug]
```

Long-running raw byte-stream producer. It reads raw bytes from stdin by default
or from `--input`, stores the entire input as one logical content slice, uses
spec-correct channel frame sequencing/START/END flags, emits a slice-closing
`tape_eof`, then emits `archive_end`.

## neotape-write CLI

```
bin/neotape-write --source <tcp://host:port|unix://path>
                  --target <tape:/dev/nst0|spool:./dir>
                  [--erase | --append]
```

Short-lived per-volume writer client. Connects to an archiver and requests frames
one at a time. Writes to a tape device or filesystem spool directory.

By default the writer refuses to overwrite existing content. Use `--erase` to
rewind to BOT and overwrite, or `--append` to space to EOD and continue.

## Thread architecture (mt-pax)

See `docs/mt-pax.md` for the full data-flow diagram and detailed
responsibilities of each thread role:

- **Walker** (main thread) — filesystem traversal, entry dispatch (bb0 /
  worker slot / large slot)
- **Serializer** (1 thread) — ordered merge of bb0, completed_queue, large
  slot → output BoundedBuffer
- **Workers** (0 or `--io-thread - 1`) — `serialize_entry` for small files
- **Output** (1 thread) — drain BoundedBuffer → output file + BLAKE3
- **Stats** (1 thread) — live progress display to stderr

## Key conventions

- C++20, GNU Make, no build system other than Makefile
- Order functions top-down: define a function before its first caller.
  Avoid forward declarations for file-internal functions within the same
  translation unit; move the definition above the call site instead.
- libarchive `xattrheader=ALL` — all xattrs including security.capability are preserved in output
- Hardlink resolution via `archive_entry_linkresolver`
- All CLI tools must use `getopt_long` (with `<getopt.h>`) for argument parsing.
  Hand-rolled `need_value`/`need` lambdas, manual `argv` iteration over positionals
  with `--` awareness, and option-value splitting are replaced by `getopt_long`'s
  built-in handling. Short options go in the optstring; long-only options use
  integer constants (≥256) as `val`. Use `optind` for positional args after the
  option loop.
- Comments are welcome when they make structure, format invariants, or non-obvious
  control flow easier to see. Prefer sparse section banners and short intent
  notes; avoid line-by-line narration of obvious code.
- In implementation files, prefer local `using` declarations or namespace aliases
  for frequently used standard-library types, for example `using std::string`
  and `using std::format`, or `namespace fs = std::filesystem`, instead of
  repeating long qualified names.
