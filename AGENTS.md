# NeoTape — Agent guide

NeoTape is an LTO-oriented, multi-volume backup container implemented in C++20.

## Sources of truth

- [Documentation index](docs/README.md): routes format/protocol questions to
  `docs/spec/` and implementation questions to `docs/implementation/`.
- [CLI reference](docs/implementation/cli-tooling.md): usage and workflows;
  executable `--help` lists the implemented options.
- [Pax architecture](docs/implementation/mt-pax-architecture.md): pipeline ownership.
- [Path handling](docs/implementation/path-pitfalls.md): pathname preservation.
- [Streaming boundaries](docs/implementation/2026-09-refactor.md): shared media,
  writer sessions, and positioning constraints.

Keep this guide about engineering workflow. Update the owning document when a
rule changes; link to it instead of duplicating specs, CLI tables, defaults,
thread diagrams, or lifecycle descriptions here. Historical notes are not specs.

## Build and verification

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Build products belong under `build/<preset>/`; release builds omit tests.
See [build notes](docs/implementation/cmake-build-system.md) for dependencies.
Configure dev before using clangd; `.clangd` owns its compilation database and
toolchain configuration.

## Code navigation and conventions

- Shared APIs live in `include/neotape/`, implementations in `src/`.
  CLI entry points are `src/*_cmd.cpp` and `src/mt-pax.cpp`.
  Integration process/fixture helpers live in `tests/support/`.
- Define file-internal functions before their callers; avoid forward declarations.
- Use `getopt_long`, short aliases for long options, and `optind` for positionals.
  Use common numeric parsers with explicit destination/range limits.
- Prefer local `using` declarations and namespace aliases for repeated names.
- Comments should explain intent, invariants, or non-obvious control flow,
  not narrate individual statements.

## Code sharing

- Search existing APIs before adding parsers, codecs, I/O, progress, or test helpers.
- Share mechanisms with identical semantics; keep caller policy separate.
  Tape record I/O is not byte-stream I/O. Do not merge them merely because
  their syscall loops look similar.
- Put shared logic in its owning domain module; use common for small,
  domain-independent utilities, not as a catch-all.
- Migrate applicable callers when extracting shared logic; avoid parallel old
  and new implementations of the same rule.
- Prefer concrete helpers over speculative frameworks or boolean-heavy wrappers.
  Preserve needed media-positioning capabilities when narrowing streaming APIs.
- Header layout/parsing/hashing belongs to `format.hpp` /
  `neotape_format.cpp`; do not add another codec or revive codegen without
  an actual independent layout requirement.

## Tests

- Verify external contracts, data integrity, failure behavior, and resource
  bounds, not incidental internal structure or diagnostic formatting.
- Fixed on-media/wire values and byte offsets are compatibility contracts:
  retain independent checks, not just encoder/decoder round trips.
- Expected results must not be produced solely by the implementation under test.
  Use source data, independent vectors, or interoperability checks.
- A regression test must trigger the condition it claims to cover and identify
  the failure it detects.
- Use native Catch2 assertions and independently runnable behavioral cases.
  Share fixture mechanics, not another assertion framework; keep important
  capacity, fault, and expected-exit parameters visible at the call site.
- Concurrent tests need deadlines and failure cleanup. Sleeps do not establish
  synchronization; use process isolation for cases that may deadlock permanently.
- Do not claim spool/null tests verify physical tape semantics.
