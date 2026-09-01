# CMake build system migration

## Status

- Overall: complete
- Started: 2026-09-01
- Completed: 2026-09-01
- Current phase: migration verified
- Build-system policy: CMake replaces GNU Make in one change; no compatibility
  Makefile is retained afterward.
- Test policy: C++ unit and process-level integration tests use Catch2. CTest is
  the project-wide test runner.

## Goals

- Model NeoTape components and third-party dependencies as CMake targets rather
  than repeated object-file lists.
- Keep all generated files and build products under the selected build tree.
- Use the system-installed Catch2 package when tests are enabled.
- Replace hand-written C++ test dispatch and shell smoke tests with discoverable
  Catch2 test cases.
- Generate the compilation database from the real build configuration.
- Preserve all current CLI, archive-format, FEC, signature, multi-volume,
  salvage, and bounded-memory behavior.

## Explicit decisions

- There is no Make/CMake transition period. The Makefile and included `.mk`
  fragments are removed after the CMake build and tests work.
- Source-tree `bin/`, `lib/`, object, dependency, and generated compilation
  database outputs are not supported.
- CMake targets use underscore-separated internal names. CLI files retain their
  hyphenated names with the `OUTPUT_NAME` target property.
- `BUILD_TESTING` controls all test targets. When it is on, Catch2 3 is a
  required system dependency; when it is off, configuring NeoTape does not
  search for Catch2.
- Benchmarks are controlled separately by `NEOTAPE_BUILD_BENCHMARKS` and are
  not part of the default CTest suite.
- Integration tests use a reusable C++ process harness with RAII cleanup,
  deadlines, captured output, temporary directories, and Unix-socket readiness
  checks. Tests receive executable paths from CMake and never assume `./bin`.
- Bundled BLAKE3, ISA-L, and signify remain the production dependency sources.
  ISA-L may continue to use its upstream make-based build internally, but its
  artifacts live in the CMake build tree and are exposed through a CMake target.
- `-march=native` is opt-in through `NEOTAPE_NATIVE_ARCH`; it is not a project
  default.

## Planned target structure

Third-party targets:

- `NeoTape::Blake3`
- `NeoTape::Isal`
- `NeoTape::Signify`
- `LibArchive::LibArchive`
- `Threads::Threads`

NeoTape libraries are split by responsibility so usage requirements propagate
through links rather than executable-specific object lists:

- common utilities
- frame format and hashing
- socket and TCP protocol
- FEC
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
- `neotape-dump`

## Execution plan and progress

### 1. Establish the CMake build

- [x] Record migration policy and implementation plan in this document.
- [x] Add the top-level `CMakeLists.txt`.
- [x] Add build and test presets.
- [x] Set C17 and C++20 requirements without compiler extensions.
- [x] Put runtime, archive, and library outputs below the build directory.
- [x] Enable the real CMake compilation database.
- [x] Add project-local warning and optional native-architecture settings.
- [x] Find libarchive and the platform thread library through imported targets.

### 2. Integrate bundled dependencies

- [x] Build BLAKE3 with architecture-appropriate sources and per-source SIMD
  flags.
- [x] Build bundled ISA-L out of tree and expose its library/include usage
  requirements through `NeoTape::Isal`.
- [x] Build bundled signify with target-local compatibility definitions,
  forced includes, and include paths.
- [x] Ensure third-party code does not inherit NeoTape warning policy.

### 3. Model production code and CLIs

- [x] Create responsibility-based NeoTape library targets.
- [x] Create all ten CLI targets with their existing output names.
- [x] Remove duplicated source/object dependency lists from the CMake build.
- [x] Verify a clean Debug build with tests disabled.
- [x] Verify a clean Release build with tests disabled.

### 4. Replace unit tests with Catch2

- [x] Require `Catch2 3` only inside the `BUILD_TESTING` branch.
- [x] Convert format tests.
- [x] Convert TCP protocol tests.
- [x] Convert validator tests.
- [x] Convert signature tests.
- [x] Convert FEC tests.
- [x] Convert pax pipeline tests.
- [x] Replace fixed sleeps in concurrency tests with explicit synchronization
  and bounded deadlines.
- [x] Register test cases through `catch_discover_tests()` with useful labels.

### 5. Replace shell smoke tests with Catch2 integration tests

- [x] Add reusable temporary-directory support.
- [x] Add a child-process abstraction with stdout/stderr capture, environment
  control, exit-status checks, deadlines, and RAII termination.
- [x] Add Unix-domain socket readiness support.
- [x] Add helpers for binary files, spool files, directory comparison, and test
  fixture construction.
- [x] Convert CLI option tests.
- [x] Convert mt-pax pipeline tests, including the low-file-descriptor pressure
  case.
- [x] Convert archive, raw-store, inspect, scan, recovery-bundle, and raw-frame
  integration tests.
- [x] Convert FEC, salvage, and signed/authentication integration tests,
  including repair-shard loss and unrecoverable erasure limits.
- [x] Convert single- and multi-volume extraction tests.
- [x] Convert plan and hardlink integration tests.
- [x] Implement the bounded-memory test with `setrlimit()` in the child process.
- [x] Assign CTest labels and process-level timeouts.

Integration tests must not rely on fixed sleeps to infer readiness. Socket
services are considered ready only after the expected socket exists and the
child remains alive. All waits have deadlines, and failures include captured
child output.

### 6. Tooling and cleanup

- [x] Add `NEOTAPE_BUILD_BENCHMARKS` and migrate the FEC benchmark target.
- [x] Add an ASan/UBSan preset.
- [x] Point clangd at the CMake-generated compilation database.
- [x] Remove the hand-written compilation database generator and tracked
  generated database.
- [x] Remove the Makefile and third-party `.mk` fragments.
- [x] Remove obsolete shell tests after their Catch2 replacements pass.
- [x] Update `.gitignore`, `AGENTS.md`, and build documentation.

### 7. Final verification

- [x] Configure the development preset from a clean build directory.
- [x] Build every production and test target.
- [x] Run the complete CTest suite with failure output enabled.
- [x] Run ASan/UBSan configurations.
- [x] Confirm `BUILD_TESTING=OFF` does not search for Catch2.
- [x] Confirm missing Catch2 is a configure error when `BUILD_TESTING=ON`.
- [x] Confirm the source tree receives no build products.
- [x] Confirm all CLI output names and help interfaces remain available.

## Intended developer workflow

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

## Completion criteria

The migration is complete when:

- a clean preset configure, build, and full CTest run succeeds;
- all production binaries are generated under the build tree;
- all C++ tests use Catch2 and all former shell smoke behavior is represented
  by Catch2 integration tests;
- tests do not contain unbounded child processes or timing-only readiness
  assumptions;
- disabling tests removes the Catch2 dependency;
- BLAKE3 SIMD dispatch, ISA-L FEC, signify signatures, libarchive pax output,
  tape/spool I/O, and every current end-to-end workflow remain functional;
- the Make build, old shell tests, and manually generated compilation database
  have been removed; and
- the source tree remains free of build artifacts.

## Known implementation risks

- BLAKE3 needs architecture and compiler capability checks plus distinct SIMD
  flags for individual translation units.
- ISA-L's upstream build must be driven without writing into its source tree,
  and CMake must know its library byproduct and build dependency.
- Process integration tests must report useful diagnostics while reliably
  cleaning up servers after assertion failures or timeouts.
- The bounded-memory test is platform-specific and needs an explicit Linux/Unix
  label or availability check.
- Existing generated-format files are ignored by Git patterns but must not be
  accidentally globbed into a target. CMake source lists remain explicit.

## Progress log

- 2026-09-01: Approved a direct Make-to-CMake replacement, normal CMake build
  output layout, system Catch2 guarded by `BUILD_TESTING`, and Catch2-based
  process integration tests. Added this tracked implementation plan.
- 2026-09-01: Added CMake presets, bundled dependency targets, responsibility-
  based production libraries, and all ten CLI targets. A Release build with
  `BUILD_TESTING=OFF` completed successfully.
- 2026-09-01: Converted the six existing C++ test executables to Catch2 and
  registered them with CTest discovery. The development build and all six
  discovered unit suites passed. Signature fixtures were changed to use an
  explicit source path rather than CTest's working directory.
- 2026-09-01: Added the C++ temporary-directory and child-process test support,
  including captured output, deadlines, RAII termination, resource limits, and
  Unix-socket readiness. Converted CLI option, mt-pax pipeline, archiver,
  raw-store, inspect, and scan behavior to five discovered integration cases.
  All six unit and five integration cases passed outside the restricted test
  sandbox; the socket cases predictably cannot bind inside that sandbox.
- 2026-09-01: Added Catch2 integration coverage for a complete
  archiver/writer/reader/extractor pax round trip and for planner hardlink
  encoding plus extracted inode identity. The suite now discovers six unit and
  seven integration cases; all thirteen pass in the socket-capable environment.
- 2026-09-01: Added non-sparse multi-volume archive continuation and restore,
  plan-driven archiver restore, FEC content-shard repair, and salvage skip
  coverage. The focused multi-volume, plan, FEC, and salvage cases pass.
- 2026-09-01: Added signed raw-store/writer/inspect/extractor coverage, including
  trusted and unverified modes, invalid `--require-signed` configurations, and
  unsigned source authentication rejection. Added the full 320 MiB FEC
  bounded-memory scenario with a 256 MiB child address-space limit and a
  separately selectable `large` CTest label. All focused cases pass.
- 2026-09-01: Removed the Make build, its third-party fragments, the manual
  compilation-database generator, the tracked source-tree database, and all 16
  shell smoke tests. Updated clangd, ignore rules, the agent guide, README, and
  implementation documentation for the CMake build-tree layout.
- 2026-09-01: Verified clean Debug and Release builds with tests disabled,
  required-system-Catch2 failure behavior, the benchmark preset, and the
  recovery-bundle target. The clean development build passes all 21 CTest cases,
  including the separately labelled bounded-memory case.
- 2026-09-01: The ASan/UBSan suite passes all 20 compatible cases. The
  bounded-memory case is intentionally run in the normal development suite
  because its 256 MiB `RLIMIT_AS` conflicts with ASan's reserved virtual address
  space. Sanitizer verification also exposed and led to removal of a bundled
  signify/glibc base64 symbol collision.
