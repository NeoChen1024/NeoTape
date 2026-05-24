# NeoTape

A seekable multi-volume length-framed payload transport container designed for LTO tape drives.

NeoTape wraps a payload byte stream (typically a POSIX pax/tar archive) in a lightweight framing layer that provides per-slice and per-Frame structure, integrity verification via BLAKE3, and multi-volume continuation. The on-tape format uses LTO filemarks at logical-slice boundaries, enabling native tape seek to independently verifiable checkpoint units without requiring per-Frame filemarks.

This is an early implementation-stage project. The current implementation
includes standalone pax writers (`bin/pax`, `bin/mt-pax`) plus a primary
`bin/neotape <subcommand>` CLI for initializing spool or tape media, writing raw
or PAX profile archives, listing archive instances, and reading/restoring from
spool archives. The spool backend is the hardware-free file-backed backend and
uses the same single-root `.nts` tape-file model as the tape abstraction.
`tape:` locators are reserved for real tape devices such as `/dev/nst0`; directory
fallback for `tape:<dir>` is intentionally not supported. Real tape-device
end-to-end validation is still pending.

## Specification Status

The active format specification lives under [`docs/spec/`](docs/spec/). When a topic is
defined in [`docs/spec/`](docs/spec/), that definition supersedes any older or conflicting
text in [`docs/RFC_Draft.md`](docs/RFC_Draft.md).

[`docs/RFC_Draft.md`](docs/RFC_Draft.md) remains useful as background design material and historical
draft text, but it is no longer the authoritative source for sections that have
been split into [`docs/spec/`](docs/spec/).

## Hierarchy

```
Archive
  └─ Volume                   volume_seq_num, one physical medium or virtual volume
       └─ Logical Slice       writer-chosen payload range, filemark-delimited
            └─ Frame           fixed record with frame_payload_size
```

A NeoTape **Archive** is the complete backup set, identified by an `archive_uuid` and optionally named by `archive_name`. It spans one or more **Volumes** (physical LTO media or virtual volumes in spool mode), each carrying a `volume_seq_num`. Inside each volume, the payload is split into **Logical Slices** — writer-declared byte ranges that are independently verifiable via the slice's BLAKE3 digest and seekable by LTO filemark. A typical slice target size is ~64 GiB but a slice may far exceed that, especially when a single large file spans the entire archive. Each logical slice is composed of one or more **Frames**. A Frame Header explicitly declares `frame_payload_size`, so the reader knows exactly how many bytes in the fixed-size NeoTape record are meaningful without parsing payload content. Frames carry either `SLICE_CONTENT` bytes or advisory `SLICE_METADATA` bytes.

All records in an archive volume use a fixed **volume_block_size** declared in the Volume Header. The writer must commit to this block size before writing any payload and must use it for every subsequent record; readers treat a block-size change within a volume as a format error.

## Project Status

| Phase | Description                        | Status |
| ----- | ---------------------------------- | ------ |
| 0     | pax writer CLI                     | Done   |
| 1     | Binary header layout               | Freeze |
| 2     | Filesystem spool backend           | Review |
| 3     | Minimal reader                     | Review |
| 3.5   | mt-pax writer CLI                  | Done   |
| 4     | NeoTape/PAX integration            | Review |
| 5     | Slice metadata Frames & catalog    | Spec   |
| 6     | Tape device backend                | Spec   |
| 7     | Recovery & salvage                 | Spec   |
| 8     | Medium Header & self-description   | Spec   |
| 9     | Filesystem-native payload profiles | Spec   |

See [`docs/spec/`](docs/spec/) for the active format specification, [`docs/RFC_Draft.md`](docs/RFC_Draft.md) for
background draft material, [`docs/ROADMAP.md`](docs/ROADMAP.md) for the implementation plan, and
[`docs/implementation/`](docs/implementation/) for implementation-specific notes (mt-pax architecture,
EOA suppression, build notes).

## Dependencies

- **C++20** compiler with `<format>` support (GCC 13+ or Clang 16+)
- **GNU Make**
- **libarchive** (system package, linked via `-larchive`)
- **BLAKE3** (bundled git submodule)

### Initialize submodules

```sh
git submodule update --init --recursive
```

## Build

```sh
make
```

Produces `bin/neotape`, `bin/mt-pax`, and standalone NeoTape helper tools.

## Usage

### bin/neotape

```sh
bin/neotape init spool:./archive.spool --label TEST --virtual-tape-size 100G
bin/neotape plan -C /data -o home.plan photos docs
bin/neotape backup --target spool:./archive.spool -p home.plan --name home
bin/neotape restore --source spool:./archive.spool --output home.pax

bin/neotape write --target spool:./archive.spool --input payload.bin --name raw1
bin/neotape read --source spool:./archive.spool --output payload.out
bin/neotape list --source spool:./archive.spool --json
```

Use `spool:<dir>` for file-backed testing. Use `tape:<device>` only for real
tape devices, for example `tape:/dev/nst0`; do not use `tape:<dir>` as a spool
shortcut.

Default archive `--volume-block-size` is 4 MiB. Payload-producing commands keep
stdout as payload bytes only; diagnostics and prompts go to stderr or
`/dev/tty`.

### bin/pax (single-threaded standalone PAX writer)

```sh
bin/pax -f <output-file|-> [-v|-vv] [-x] [-C <dir>] <path> [path ...]
```

- `-f`  Output file path, or `-` for stdout
- `-v`  Verbose output (file listing)
- `-vv` Very verbose output (detailed metadata per entry)
- `-x`  Stay within one file system (do not cross mount points)
- `-C`  Change directory before walking

All diagnostics are written to stderr. Stdout contains pure archive payload bytes. On completion, a BLAKE3 digest of the output is printed to stderr.

### bin/mt-pax (Phase 3.5, multi-threaded)

```sh
bin/mt-pax -f <output-file|-> [-v|-vv] [-x] [-C <dir>]
           [-P <buffer-percent>] [--io-thread <N>]
           [--output-buffer-size <bytes>] <path> [path ...]
```

All `bin/pax` options plus:

- `--io-thread <N>`  Total I/O threads (default 1). N=1 uses no worker threads;
  N>1 spawns N-1 workers for small files.
- `--output-buffer-size <bytes>`  Internal output buffer size (default 64 MB).
- `-P <percent>`  Waterline write restart threshold (0-100). When non-zero the
  output thread waits until the buffer reaches at least this full before
  draining, useful for sequential writes on HDD/tape.

### Examples

```sh
# Pack a directory (single-threaded)
bin/pax -f backup.tar src/

# Stream to stdout and pipe to bsdtar
bin/pax -f - src/ | bsdtar -tvf -

# Multi-threaded with 4 I/O threads
bin/mt-pax -f backup.tar --io-thread 4 src/

# Multi-threaded with output buffer tuning for tape
bin/mt-pax -f /dev/nst0 --io-thread 4 --output-buffer-size 256M -P 50 src/
```

## Payload Profiles

NeoTape core is payload-format agnostic. Any byte stream can be transported in Frames and slices. The **NeoTape/PAX** payload profile is the recommended default for POSIX backup, producing a standard pax/tar stream compatible with `bsdtar` and `libarchive`. In this profile, slice boundaries may be chosen at pax member boundaries, and the concatenated slice content bytes form a valid pax stream on output.

Other profile types are possible without changing the NeoTape framing layer. Planned examples include **ZFS send stream** and **Btrfs send stream**, where each dataset snapshot is mapped to one or more logical slices and the reader emits bytes compatible with the corresponding `zfs receive` or `btrfs receive` command.

## License

GNU General Public License v3.0 or later. See [`LICENSE`](LICENSE).
