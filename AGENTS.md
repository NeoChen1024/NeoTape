# NeoTape — Agent guide

## What it is

LTO tape-oriented multi-volume length-framed backup container. The Phase 0 pax
writer exists, and early NeoTape format/spool tooling is now present, but the
tape-device backend and full on-tape workflow are still under development.

## Build

```sh
make                    # produces bin/pax and NeoTape helper tools
make clean
```

Dependencies: libarchive (system, `-larchive`), BLAKE3 (bundled submodule
`3rdparty/BLAKE3` → `lib/libb3sum.a`), and crc32c (bundled submodule
`3rdparty/crc32c` → `lib/libcrc32c.a`). mt-st is a submodule but unused in
the build.

## Project layout

- `src/pax.cpp` — Phase 0 pax writer
- `src/neotape_*.cpp` — early NeoTape format, spool writer, inspector, and planner tools
- `include/neotape/` — shared project headers
- `3rdparty/` — git submodules (BLAKE3, crc32c mt-st). Init with `git submodule update --init --recursive`
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
- All CLI tools must use `getopt_long` (with `<getopt.h>`) for argument parsing.
  Hand-rolled `need_value`/`need` lambdas, manual `argv` iteration over positionals
  with `--` awareness, and option-value splitting are replaced by `getopt_long`'s
  built-in handling. Short options go in the optstring; long-only options use
  integer constants (≥256) as `val`. Use `optind` for positional args after the
  option loop.
- Comments are welcome when they make structure, format invariants, or non-obvious
  control flow easier to see. Prefer sparse section banners and short intent
  notes; avoid line-by-line
  narration of obvious code.
- In implementation files, prefer local `using` declarations or namespace aliases
  for frequently used standard-library types, for example `using std::string`
  and `using std::format`, or `namespace fs = std::filesystem`, instead of
  repeating long qualified names.
