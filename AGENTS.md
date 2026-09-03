# NeoTape — Agent guide

## What it is

LTO tape-oriented multi-volume length-framed backup container. A pax writer
exists: `build/dev/bin/mt-pax` (multi-threaded with `--io-thread`, `src/mt-pax.cpp`).
A planner (`build/dev/bin/neotape-plan`) exists for slice metadata. Long-running data
producers (`build/dev/bin/neotape-archiver`, `build/dev/bin/neotape-raw-store`) generate
NeoTape-framed records over a TCP or Unix-domain socket. Short-lived per-volume
clients (`build/dev/bin/neotape-write`, `build/dev/bin/neotape-read`) connect to producers or
consumers and write/read frames to/from tape or spool. The writer also has a
validation-only `null` target that discards validated frames. An
`build/dev/bin/neotape-extractor` consumes frames from a reader and reconstructs the
payload stream. An `build/dev/bin/neotape-inspect` scans spool or tape for frame-level
verification and archive compliance reporting. `build/dev/bin/neotape-dump` performs a
validation-free tape-to-spool copy. Producers optionally emit local
`rs_32_4` FEC groups, and extractor salvage mode repairs unavailable protected
content shards. The tape-device backend
(`namespace mt`) implements both tape and spool I/O behind a shared
`mt::TapeDevice` interface. Optional Ed25519 frame signing uses external
signify-compatible key files; NeoTape verifies but does not generate keys.

## Build

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

The `release` preset builds optimized production binaries without tests. Build
products are always below `build/<preset>/`. Dependencies are CMake 3.24+,
Ninja, system libarchive, system Catch2 3 when `BUILD_TESTING=ON`, and bundled
BLAKE3, ISA-L, and signify submodules.

## clangd / LSP

The repository includes `.clangd`, which reads CMake's real compilation
database from `build/dev`. Configure the `dev` preset before using clangd.

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
- `src/neotape_archiver_cmd.cpp` — `build/dev/bin/neotape-archiver` CLI entry point
- `src/neotape_raw_store_cmd.cpp` — `build/dev/bin/neotape-raw-store` raw-stream server CLI entry point
- `src/neotape_read_cmd.cpp` — `build/dev/bin/neotape-read` CLI entry point
- `src/neotape_extractor_cmd.cpp` — `build/dev/bin/neotape-extractor` CLI entry point
- `src/neotape_extractor.cpp` — extractor state machine (frame accumulation, payload reassembly)
- `src/neotape_inspect_cmd.cpp` — `build/dev/bin/neotape-inspect` CLI entry point
- `src/neotape_signature.cpp` — signify-compatible key loading, frame signing, auth nonce signing
- `src/neotape_fec.cpp` — FEC descriptor codec and ISA-L `rs_32_4` encode/recovery
- `src/neotape_dump_cmd.cpp` — validation-free tape-to-spool dumper
- `src/neotape_validate.cpp` — shared archive-frame validation (root: `include/neotape/validate.hpp`)
- `src/neotape_write_cmd.cpp` — `build/dev/bin/neotape-write` CLI entry point
- `src/neotape_tcp_server.cpp` — archiver server and frame packing
- `src/neotape_tcp_protocol.cpp` — framed TCP message I/O
- `src/neotape_*.cpp` — NeoTape format and tape manipulation library (format layer and `namespace mt`)
- `include/neotape/signature.hpp` — signify-compatible signing / verification interface
- `include/neotape/tcp_protocol.hpp` — TCP archive protocol message types
- `include/neotape/tcp_server.hpp` — archiver server interface
- `include/neotape/extractor.hpp` — extractor interface
- `include/neotape/bounded_buffer.hpp` — thread-safe bounded buffer used by mt-pax
- `include/neotape/` — shared project headers (common types, format helpers)
- `3rdparty/` — git submodules (BLAKE3, signify). Init with `git submodule update --init --recursive`
- `docs/spec/` — active format spec; `docs/implementation/mt-pax-architecture.md` — mt-pax architecture
- `tests/test_*_integration.cpp` and `tests/support/` — Catch2 process-level integration tests and their POSIX process harness

## Architecture pattern

NeoTape separates long-running data producers/consumers from short-lived tape
I/O clients over a single TCP or Unix-domain socket:

- **Listener / long-running role** (`neotape-archiver`, `neotape-raw-store`, `neotape-extractor`)
  owns archive state (archive UUID, volume sequence, and frame sequence numbers) and serves
  fully-formed NeoTape records through a framed request-response protocol. It
  stays up for the lifetime of the archive and does not know about physical
  media changes.

- **Tape client / short-lived role** (`neotape-write`, `neotape-read`)
  connects to a listener, requests one volume's worth of data, and writes it to
  a tape device, spool directory, or raw file. One client process handles
  exactly one volume; when it reaches end-of-tape it writes a trailing filemark
  and exits, letting the operator mount a new medium and start another client.

This split lets archive generation/extraction run on one host while tape
hardware is attached to another, keeps media handling out of the archive state
machine, and provides natural back-pressure because the client requests frames
one at a time.

When signed frames are enabled, the writing pipeline is asymmetric by design:
the Archiver/Raw Store holds the secret key, while the Writer is provisioned
only with trusted public keys. The Writer may authenticate the source server by
verifying `NeoTape-auth\0 || nonce`, then verifies each signed frame before
writing it. In the reading pipeline, the Reader remains a dumb forwarder and
the Extractor is the authoritative signed-frame validator.

## mt-pax CLI

All long CLI options have short aliases shown by `-h`. Byte-size arguments use
`SIZE` and accept case-insensitive binary `K`, `M`, `G`, and `T` suffixes, such
as `4M` or `16G`; values without a suffix are bytes.

```
build/dev/bin/mt-pax -f <out-file|-> [-v|-vv] [-x] [-C <dir>]
           [-P <buffer-percent>] [--io-thread <N>]
           [--output-buffer-size <SIZE>] <path> [path...]
```

All `pax` options plus:

- `--io-thread <N>` — total I/O threads (default 1).  `N=1` uses no worker
  threads (serializer reads all files directly, large and small).  `N>1`
  spawns `N-1` workers for small files and streams large files via the
  serializer.
- `-B, --output-buffer-size <SIZE>` — size of the internal output
  `BoundedBuffer` (default 64 MB).  Larger values reduce write fragmentation
  at the cost of memory.
- `-P <percent>` — waterline write restart threshold as percentage of the
  output buffer (default 0).  When non-zero the output thread waits until
  the buffer reaches at least this full before starting to drain, useful for
  sequential writes on HDD/tape.

## neotape-archiver CLI

```
build/dev/bin/neotape-archiver --listen <tcp://host:port|unix://path>
                     [--volume-block-size <SIZE>] [--archive-name <name>]
                     [-C <dir>] [-P <percent>] [--io-thread <N>]
                     [--output-buffer-size <SIZE>] [--plan <file>]
                     [--retention-frame-count <N>]
                     [--fec]
                     [--sign-secret-key <file.sec>]
                     [--sign-passphrase-file <path>] [--debug]
                     [-v|-vv] [-x] <path> [path...]
```

The archiver is a long-running producer that serves NeoTape records over a
single TCP/UDS connection. `--listen` is required; use `mt-pax` for standalone
pax output. `--sign-secret-key` signs every served frame with a
signify-compatible secret key file. `--fec` protects content in local `32C + 4F`
groups.

## neotape-raw-store CLI

```
build/dev/bin/neotape-raw-store --listen <tcp://host:port|unix://path>
                       [--input <file|->]
                       [--volume-block-size <SIZE>]
                       [--archive-name <name>]
                       [--retention-frame-count <N>]
                       [--fec]
                       [--sign-secret-key <file.sec>]
                       [--sign-passphrase-file <path>] [--debug]
```

Long-running raw byte-stream producer. It reads raw bytes from stdin by default
or from `--input`, stores the entire input as one logical content slice, uses
spec-correct channel frame sequencing/START/END flags, emits a slice-closing
`tape_eof`, then emits `archive_end`. It can also sign frames with the same
signify-compatible key handling as `neotape-archiver`.

## neotape-inspect CLI

```
build/dev/bin/neotape-inspect --source <spool:./dir|tape:/dev/nst0>
                    [--verify-pubkey <file.pub>]...
                    [--require-signed] [--debug] [--raw] [-h]
```

Scans a spool directory or tape device for NeoTape frames, parses and validates
every frame header, and prints a human-readable table with frame hash
verification, followed by an archive-level compliance report.  Compliance checks
cover per-frame integrity (magic, version, block size, hash, flags, signature
consistency, reserved bytes) and archive continuity (`global_frame_seq_num`,
slice/channel sequence, channel ordering, archive-end rules). With
`--verify-pubkey`, it also validates frame signatures; `--require-signed`
requires at least one trusted `--verify-pubkey` and upgrades unsigned or
untrusted frames to compliance failures. Without a public key, signed frames
are reported as signed and unverified.

## neotape-read CLI

```
build/dev/bin/neotape-read --source <tape:/dev/nst0|spool:./dir>
       --connect <tcp://host:port|unix://path>
```

Short-lived per-volume reader client. Connects to a tape device or spool
directory, reads NeoTape frames, and forwards them to an extractor over a TCP
or Unix-domain socket. One reader process handles exactly one volume.

## neotape-extractor CLI

```
build/dev/bin/neotape-extractor --listen <tcp://host:port|unix://path>
       [-o <file>] [--verify-pubkey <file.pub>]...
       [--require-signed] [--salvage] [-v] [-h]
```

Long-running payload consumer. Listens for incoming reader connections,
receives NeoTape frames, validates them via the shared `FrameValidator`, and
reconstructs the original payload byte stream. Writes to a file or stdout.
Signature validation is optional unless `--verify-pubkey` is configured;
`--require-signed` requires at least one `--verify-pubkey` and rejects unsigned
or untrusted frames. Without a public key, signed frames remain readable but
are reported as signed and unverified.

`--salvage` retains frame integrity checks but relaxes archive-level identity,
sequence, ordering, and clean-end consistency. It skips invalid frames with an
explicit unverified-output warning and attempts FEC recovery for protected
groups before falling back to surviving shards.

FEC decoding is automatic in normal extraction and does not require
`--salvage` or an extractor-side `--fec`. Protected content is buffered until
the matching repair group is accepted. Normal mode fails if recovery or the
group commitment fails; salvage alone permits surviving-shard fallback. See
`docs/implementation/fec-restore-behavior.md`.

## neotape-dump CLI

```
build/dev/bin/neotape-dump --source <tape:/dev/nst0>
                  --target <spool:./dir> [-v] [-h]
```

Rewinds and copies physical tape records into spool files while preserving
filemark boundaries. It intentionally performs no NeoTape consistency or
integrity validation; the target directory must be empty.

## neotape-write CLI

```
build/dev/bin/neotape-write --source <tcp://host:port|unix://path>
                  --target <tape:/dev/nst0|spool:./dir|null>
                  [--verify-pubkey <file.pub>]...
                  [--erase | --append]
                  [--recovery-bundle <tar>]
                  [--recovery-bundle-block-size <SIZE>]
                  [--output-buffer-size <SIZE>]
                  [--max-volume-bytes <SIZE>] [--debug]
```

Short-lived per-volume writer client. Connects to an archiver and requests frames
one at a time. Writes to a tape device or filesystem spool directory. The `null`
target performs full validation and acknowledgement while discarding records;
`--max-volume-bytes` can simulate capacity for spool and null targets.

By default the writer refuses to overwrite existing content. Use `--erase` to
rewind to BOT and overwrite, or `--append` to space to EOD and continue. When
`--verify-pubkey` is present, the writer authenticates the source server before
opening/rewinding media, then verifies every signed frame before writing it.
In non-append mode, `--recovery-bundle` writes a tar before the NeoTape stream.
Physical tape recovery records use a separate block size (256 KiB by default,
configurable with `--recovery-bundle-block-size`), while spool targets store
the exact tar as `recovery-bundle.tar`.

## Thread architecture (mt-pax)

See `docs/implementation/mt-pax-architecture.md` for the full data-flow diagram and detailed
responsibilities of each thread role:

- **Walker** (main thread) — filesystem traversal, entry dispatch (bb0 /
  worker slot / large slot)
- **Serializer** (1 thread) — ordered merge of bb0, completed_queue, large
  slot → output BoundedBuffer
- **Workers** (0 or `--io-thread - 1`) — `serialize_entry` for small files
- **Output** (1 thread) — drain BoundedBuffer → output file + BLAKE3
- **Stats** (1 thread) — live progress display to stderr

## Key conventions

- C++20 with a CMake/Ninja build system
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
