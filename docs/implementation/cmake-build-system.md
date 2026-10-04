# Build system notes

Status: implementation note. NeoTape builds with CMake presets; the migration
log from GNU Make is in git history.

## Dependencies

- C++20 compiler with `<format>` (GCC 13+ or Clang 16+), CMake 3.24+, Ninja.
- libarchive and a threads library, found as imported targets.
- Catch2 3 from the system, only when tests are enabled.
- BLAKE3 and signify, bundled as git submodules under `3rdparty/`.
- `tar`, for the `bot_bundle` target, and `bsdtar` and `dd` for the tests.

## Decisions

- Source-tree `bin/`, `lib/`, object, dependency, and generated compilation
  database outputs are not supported.
- CMake targets use underscore-separated internal names. CLI files retain their
  hyphenated names with the `OUTPUT_NAME` target property.
- `BUILD_TESTING` controls all test targets. When it is on, Catch2 3 is a
  required system dependency; when it is off, configuring NeoTape does not
  search for Catch2.
- Integration tests use a reusable C++ process harness with RAII cleanup,
  deadlines, captured output, temporary directories, and Unix-socket readiness
  checks. Tests receive executable paths from CMake and never assume `./bin`.
- Bundled BLAKE3 and signify remain the production dependency sources.
- `-march=native` is opt-in through `NEOTAPE_NATIVE_ARCH`; it is not a project
  default.

## Target structure

Third-party targets:

- `NeoTape::Blake3`
- `NeoTape::Signify`
- `LibArchive::LibArchive`
- `Threads::Threads`

NeoTape libraries are split by responsibility so usage requirements propagate
through links rather than executable-specific object lists:

- common utilities
- frame format and hashing
- socket and TCP protocol
- signatures
- validation
- tape/spool I/O
- pax production
- frame building and volume serving
- extractor implementation

The public executable names remain:

- `mt-pax`
- `neotape-plan`
- `neotape-archiver`
- `neotape-raw-store`
- `neotape-write`
- `neotape-read`
- `neotape-extractor`
- `neotape-inspect`
- `neotape-scan`
- `neotape-catalog`
- `neotape-dump`

## Developer workflow

Development build:

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Production build without test dependencies:

```sh
cmake --preset release
cmake --build --preset release
```

Focused test execution uses CTest names or labels, for example:

```sh
ctest --test-dir build/dev -L unit --output-on-failure
ctest --test-dir build/dev -L integration --output-on-failure
```

The `asan-ubsan` preset builds with AddressSanitizer and UndefinedBehaviorSanitizer.
BLAKE3 SIMD sources are compiled per translation unit with their own flags
after compiler capability checks.
