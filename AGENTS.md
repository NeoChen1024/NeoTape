# NeoTape — Agent guide

## What it is

LTO tape-oriented multi-volume length-framed backup container. Two pax
writers exist: `bin/pax` (single-threaded, `src/pax.cpp`) and `bin/mt-pax`
(multi-threaded with `--io-thread`, `src/mt-pax.cpp`).  Early NeoTape
format/spool tooling is also present, but the tape-device backend and full
on-tape workflow are still under development.

## Build

```sh
make -j "$(nproc)"      # produces bin/pax, bin/mt-pax, and NeoTape helper tools
make clean
```

Dependencies: libarchive (system, `-larchive`), BLAKE3 (bundled submodule
`3rdparty/BLAKE3` → `lib/libb3sum.a`), and crc32c (bundled submodule
`3rdparty/crc32c` → `lib/libcrc32c.a`). mt-st is a submodule but unused in
the build.

## Project layout

- `src/pax.cpp` — single-threaded Phase 0 pax writer (stable baseline)
- `src/mt-pax.cpp` — multi-threaded pax writer (worker pool, serializer, streaming large files)
- `src/neotape_*.cpp` — NeoTape format, spool writer, inspector, planner, and reader tools
- `include/neotape/bounded_buffer.hpp` — thread-safe bounded buffer used by mt-pax
- `include/neotape/` — shared project headers (common types, format helpers)
- `3rdparty/` — git submodules (BLAKE3, crc32c, mt-st). Init with `git submodule update --init --recursive`
- `docs/RFC_Draft.md` — format spec; `docs/ROADMAP.md` — implementation phases; `docs/mt-pax.md` — mt-pax architecture
- No test framework, no CI, no test scripts yet

## pax CLI

```
bin/pax -f <out-file|-> [-v|-vv] [-x] [-C <dir>] <path> [path...]
```

- `-f` output (`-` = stdout)
- `-v` / `-vv` verbosity
- `-x` one file system
- `-C <dir>` chdir before walking
- stderr for diagnostics/prompts; stdout for pure payload bytes
- BLAKE3 hash of output printed to stderr on completion

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
