# NeoTape — Agent guide

## What it is
LTO tape-oriented multi-volume length-framed backup container. Currently early development: only Phase 0 (pax writer) exists. NeoTape on-tape framing is NOT yet implemented.

## Build
```sh
make                    # produces bin/pax (C++20, -march=native)
make clean
```

Dependencies: libarchive (system, `-larchive`), BLAKE3 (bundled submodule `3rdparty/BLAKE3` → `lib/libb3sum.a`). mt-st is a submodule but unused in build.

## Project layout
- `src/pax.cpp` — sole source, Phase 0 pax writer
- `include/` — headers (empty, create here for new headers)
- `3rdparty/` — git submodules (BLAKE3, libarchive, mt-st). Init with `git submodule update --init --recursive`
- `docs/RFC_Draft.md` — format spec; `docs/ROADMAP.md` — implementation phases
- No test framework, no CI, no test scripts yet

## pax CLI
```
bin/pax -f <out-file|-> [-v|-vv] [-x] <path> [path...]
```
- `-f` output (`-` = stdout)
- `-v` / `-vv` verbosity
- `-x` one file system
- stderr for diagnostics/prompts; stdout for pure payload bytes
- BLAKE3 hash of output printed to stderr on completion

## Key conventions
- C++20, GNU Make, no build system other than Makefile
- libarchive `xattrheader=ALL` — all xattrs including security.capability are preserved in output
- Hardlink resolution via `archive_entry_linkresolver`
- All diagnostics on stderr, never stdout
- **No formatted comments in code** unless asked
