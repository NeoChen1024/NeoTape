# NeoTape

A seekable multi-volume length-framed payload transport container designed for LTO tape drives.

NeoTape wraps a payload byte stream (typically a POSIX pax/tar archive) in a lightweight framing layer that provides per-slice and per-Frame structure, integrity verification via BLAKE3, and multi-volume continuation. The on-tape format uses LTO filemarks at logical-slice boundaries, enabling native tape seek to independently verifiable checkpoint units without requiring per-Frame filemarks.

This is an early implementation-stage project. The current implementation
includes a standalone pax writer (`bin/mt-pax`), a planner (`bin/neotape-plan`)
for slicing metadata, and a split producer/writer pair (`bin/neotape-archiver`
and `bin/neotape-write`) that generate NeoTape-framed records over a TCP or
Unix-domain socket. The `namespace mt` tape manipulation library remains
available for future tools.

## Specification Status

The active format specification lives under [`docs/spec/`](docs/spec/).

## Hierarchy

```
Archive
  └─ Volume                   volume_seq_num, one physical medium or virtual volume
       └─ Logical Slice       writer-chosen payload range, filemark-delimited
            └─ Frame           fixed record with frame_payload_size
```

A NeoTape **Archive** is the complete backup set, identified by an `archive_uuid`
and optionally named by `archive_name`. It spans one or more **Volumes** (physical
LTO media or virtual volumes in spool mode), each carrying a `volume_seq_num`.
NeoTape does not store a medium-level descriptor in the archive stream; physical
medium identity is handled outside the format. Inside each volume, the payload is
split into **Logical Slices** — writer-declared byte ranges that are independently
verifiable via the slice's BLAKE3 digest and seekable by LTO filemark. A typical
slice target size is ~64 GiB but a slice may far exceed that, especially when a
single large file spans the entire archive. Each logical slice is composed of one
or more **Frames**. A Frame Header explicitly declares `frame_payload_size`, so the
reader knows exactly how many bytes in the fixed-size NeoTape record are
meaningful without parsing payload content. Frames carry either `SLICE_CONTENT`
bytes or advisory `SLICE_METADATA` bytes.

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
| 8     | Optional BOT recovery bundle       | Spec   |
| 9     | Filesystem-native payload profiles | Spec   |

See [`docs/spec/`](docs/spec/) for the active format specification, [`docs/ROADMAP.md`](docs/ROADMAP.md)
for the implementation plan, and [`docs/implementation/`](docs/implementation/) for
implementation-specific notes (mt-pax architecture, EOA suppression, build notes).

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

Produces `bin/mt-pax`, `bin/neotape-plan`, `bin/neotape-archiver`,
`bin/neotape-write`, and standalone NeoTape helper tools.

## Usage

### bin/neotape-plan

```sh
bin/neotape-plan -C /data -o home.plan photos docs
```

`neotape-plan` scans source paths and emits per-slice metadata for later use by a
NeoTape writer. It does not write archives.

### bin/mt-pax (multi-threaded PAX writer)

```sh
bin/mt-pax -f <output-file|-> [-v|-vv] [-x] [-C <dir>]
           [-P <buffer-percent>] [--io-thread <N>]
           [--output-buffer-size <bytes>] <path> [path ...]
```

Additional options:

- `--io-thread <N>`  Total I/O threads (default 1). N=1 uses no worker threads;
  N>1 spawns N-1 workers for small files.
- `--output-buffer-size <bytes>`  Internal output buffer size (default 64 MB).
- `-P <percent>`  Waterline write restart threshold (0-100). When non-zero the
  output thread waits until the buffer reaches at least this full before
  draining, useful for sequential writes on HDD/tape.

### bin/neotape-archiver (long-running producer)

```sh
bin/neotape-archiver --listen <tcp://host:port|unix://path>
                     [--volume-block-size <bytes>] [--archive-name <name>]
                     [-C <dir>] [-P <percent>] [--io-thread <N>]
                     [--output-buffer-size <bytes>] [--plan <file>]
                     [-v|-vv] [-x] <path> [path...]
```

`neotape-archiver` is a functional superset of `mt-pax`. In server mode it
listens on a TCP or Unix-domain socket and serves NeoTape-framed records to
`neotape-write` clients. Without `--listen` it writes a plain pax stream to
`-f <out-file|->`, matching `mt-pax` output.

### bin/neotape-write (per-volume writer client)

```sh
bin/neotape-write --source <tcp://host:port|unix://path>
                  [--target <tape:/dev/nst0|spool:./dir> | -o <file|->]
```

`neotape-write` connects to a running `neotape-archiver`, requests the current
Volume Header, then requests frames one at a time. It writes each record to a
tape device, a filesystem spool directory, or a raw file. One writer process
writes exactly one volume.

### Examples

```sh
# Long-running archiver on a Unix-domain socket
bin/neotape-archiver --listen unix:///run/neotape/home.sock \
                     --archive-name home --volume-block-size 4M \
                     -C /data photos docs

# Write one volume to a tape device
bin/neotape-write --source unix:///run/neotape/home.sock \
                  --target tape:/dev/nst0

# Write one volume to a filesystem spool (useful for testing)
bin/neotape-write --source unix:///run/neotape/home.sock \
                  --target spool:./vol1.spool

# Pack a directory with 4 I/O threads
bin/mt-pax -f backup.tar --io-thread 4 src/

# Stream to stdout and pipe to bsdtar
bin/mt-pax -f - --io-thread 2 src/ | bsdtar -tvf -

# Multi-threaded with output buffer tuning for tape
bin/mt-pax -f /dev/nst0 --io-thread 4 --output-buffer-size 256M -P 50 src/
```

## Payload Profiles

NeoTape core is payload-format agnostic. Any byte stream can be transported in Frames and slices. The **NeoTape/PAX** payload profile is the recommended default for POSIX backup, producing a standard pax/tar stream compatible with `bsdtar` and `libarchive`. In this profile, slice boundaries may be chosen at pax member boundaries, and the concatenated slice content bytes form a valid pax stream on output.

Other profile types are possible without changing the NeoTape framing layer. Planned examples include **ZFS send stream** and **Btrfs send stream**, where each dataset snapshot is mapped to one or more logical slices and the reader emits bytes compatible with the corresponding `zfs receive` or `btrfs receive` command.

## License

GNU General Public License v3.0 or later. See [`LICENSE`](LICENSE).
