# Build and Dependencies

Status: implementation note.

## Build System

NeoTape uses a standard GNU Makefile with no external build system (no CMake,
no Meson, no autotools).

```sh
make -j "$(nproc)"       # Build all tools
make clean               # Clean build artifacts
```

Build outputs go to `bin/` (executables) and `lib/` (static libraries).

## Dependencies

### Required (system packages)

| Dependency              | Purpose                                | Linkage                                |
| ----------------------- | -------------------------------------- | -------------------------------------- |
| libarchive              | pax/tar writing and reading            | `-larchive` (shared or static)       |
| BLAKE3 (bundled)        | Payload and metadata integrity hashing | Static library via `lib/libb3sum.a`  |
| CRC32C (bundled)        | Fixed header integrity                 | Static library via `lib/libcrc32c.a` |
| nlohmann-json (bundled) | JSON parsing (manifest, diagnostics)   | Header-only; include path via `-I`   |
| C++20 compiler          | Language standard                      | GCC or Clang with `-std=c++20`       |

### Bundled Submodules

```text
3rdparty/BLAKE3/         → lib/libb3sum.a
3rdparty/crc32c/         → lib/libcrc32c.a
3rdparty/nlohmann-json/  → header-only, include path
```

Initialize submodules:

```sh
git submodule update --init --recursive
```

### Include Paths

Bundled header-only libraries (nlohmann-json) are added via `-I`:

```makefile
INCS = -Iinclude -Itests -Llib -I/usr/local/include \
       -I3rdparty/nlohmann-json/single_include
```

### No Other Dependencies

The NeoTape format layer, spool backend, and tools are pure C++20 with POSIX
system calls. No boost, no protobuf, no heavy external frameworks.

### JSON Usage

The `manifest.json` in spool tape directories (see `docs/spec/05-spool-dir.md`)
is optionally parsed by the reader for auxiliary recovery information (volume
ordering, file list). It is never required for payload correctness — restore
integrity comes from NeoTape headers, lengths, and checksums. The reader
falls back to scanning the directory tree if no manifest is found.

## Build Details

### Makefile Structure

- **`Makefile`** — top-level, defines targets, flags, and rules for all tools.
- Object files go to `build/`.
- Static libraries go to `lib/`.
- Executables go to `bin/`.

### Compiler Flags

```makefile
INCS    = -Iinclude -Itests -Llib -I/usr/local/include -Lusr/local/lib \
          -I3rdparty/nlohmann-json/single_include
CXXFLAGS = -std=c++20 -Wall -Wextra -pedantic -O2 -g $(INCS)
```

Debug builds optionally add `-DDEBUG` and `-fsanitize=address`.

### Link Order

NeoTape tools link in this order:

```makefile
LDLIBS = -larchive -lb3sum -lcrc32c -lpthread
```

The `-lpthread` dependency is for mt-pax (multi-threaded pax writer) and
`neotape plan` (worker pool).

## Source Layout

```text
src/              — Implementation files (*.cpp), one per tool or module.
include/neotape/  — Public headers (*.hpp), shared across tools.
tests/            — Test device support and test programs.
3rdparty/         — Bundled library submodules.
                   BLAKE3/       → lib/libb3sum.a
                   crc32c/       → lib/libcrc32c.a
                   nlohmann-json/ → header-only (include path)
```
