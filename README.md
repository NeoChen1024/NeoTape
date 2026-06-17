# NeoTape

A seekable multi-volume length-framed payload transport container designed for LTO tape drives.

NeoTape wraps a payload byte stream (typically a POSIX pax/tar archive) in a lightweight framing layer that provides per-frame structure, logical-slice grouping, per-frame integrity verification via BLAKE3, and multi-volume continuation. The on-tape format uses LTO filemarks at logical-slice boundaries, enabling native tape seek to coarse checkpoint units without requiring per-frame filemarks.

This is an early implementation-stage project. The current implementation
includes a standalone pax writer (`bin/mt-pax`), a planner (`bin/neotape-plan`)
for slicing metadata, long-running data producers (`bin/neotape-archiver`,
`bin/neotape-raw-store`), per-volume tape/spool clients (`bin/neotape-write`,
`bin/neotape-read`), a payload extractor (`bin/neotape-extractor`), and an
inspection/compliance tool (`bin/neotape-inspect`).

## Specification Status

The active format specification lives under [`docs/spec/`](docs/spec/).

## Hierarchy

```
Archive
  └─ Volume                   advisory volume_seq_num, one physical medium or virtual volume
       └─ Logical Slice       writer-chosen payload range, filemark-delimited
            └─ Channel        ch_metadata (optional) then ch_content
                 └─ Frame     fixed record with frame_payload_size
```

A NeoTape **Archive** is the complete backup set, identified by an `archive_uuid`
and optionally labeled by `archive_label`. It spans one or more **Volumes** (physical
LTO media or virtual volumes in spool mode), each carrying an advisory `volume_seq_num`.
Inside each volume, the payload is
split into **Logical Slices** — writer-declared byte ranges that are seekable by LTO
filemark. A typical slice target size is ~64 GiB but a slice may far exceed that,
especially when a single large file spans the entire archive. Each logical slice is
composed of one or more **Channels**: an optional `ch_metadata` group followed by a
`ch_content` group. Each channel group is composed of one or more **Frames**. A Frame
Header explicitly declares `frame_payload_size`, so the reader knows exactly how
many bytes in the fixed-size NeoTape record are meaningful without parsing payload
content.

All records in an archive volume use a fixed **volume_block_size_kib** (record size
in KiB), repeated in every frame. The writer must commit to this block size before
writing any payload and must use it for every subsequent record; readers treat a
block-size change within a volume as a format error.

## Project Status

| Phase | Description                        | Status |
| ----- | ---------------------------------- | ------ |
| 0     | pax writer CLI                     | Done   |
| 1     | Binary header layout               | Freeze |
| 2     | Filesystem spool backend           | Done   |
| 3     | Tape + spool reader / extractor    | Done   |
| 3.5   | mt-pax writer CLI                  | Done   |
| 4     | NeoTape/PAX producer pair          | Done   |
| 5     | Raw byte-stream store              | Done   |
| 6     | Frame inspect / compliance         | Done   |
| 7     | Recovery & salvage                 | Spec   |
| 8     | Optional BOT recovery bundle       | Spec   |

See [`docs/spec/`](docs/spec/) for the active format specification and [`docs/implementation/`](docs/implementation/) for
implementation-specific notes (mt-pax architecture, build notes).

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
`bin/neotape-raw-store`, `bin/neotape-write`, `bin/neotape-read`,
`bin/neotape-extractor`, `bin/neotape-inspect`, and test binaries.

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
                  --target <tape:/dev/nst0|spool:./dir>
                  [--erase | --append]
```

`neotape-write` connects to a running `neotape-archiver` and requests frames one
at a time. It writes each record to a tape device or a filesystem spool directory.
One writer process writes exactly one volume.

By default the writer refuses to overwrite existing content. Use `--erase` to
rewind to BOT and overwrite, or `--append` to space to EOD and continue.

### bin/neotape-raw-store (raw byte-stream producer)

```sh
bin/neotape-raw-store --listen unix:///run/neotape/raw.sock \
                      --archive-name dataset \
                      --retention-frame-count 5 < input.raw
```

Reads raw bytes from stdin, wraps them as a single logical content slice with
correct START/END sequencing, emits a `tape_eof` slice boundary, then an
`archive_end`.  The `--retention-frame-count` limits the send window for tape
back-pressure.

### bin/neotape-read (per-volume spool/tape reader)

```sh
bin/neotape-read --source spool:./vol1.spool \
                 --connect unix:///run/neotape/extractor.sock
```

Reads NeoTape frames from a spool directory (or tape device) and forwards them
to an extractor.  One reader process per volume.

### bin/neotape-extractor (payload reconstruction)

```sh
bin/neotape-extractor --listen unix:///run/neotape/extractor.sock \
                      -o /restore/backup.pax
```

Long-running service that receives frames from readers, validates archive
integrity, and reconstructs the original payload stream to a file or stdout.

### bin/neotape-inspect (frame-level verification)

```sh
bin/neotape-inspect --source spool:./vol1.spool
bin/neotape-inspect --source tape:/dev/nst0 --debug
```

Scans a spool directory or tape device and produces a per-frame header table
with BLAKE3 hash verification, followed by an archive-level compliance report
(frame integrity, sequence continuity, channel ordering, SIGNED/signature
consistency, archive-end rules).

### Examples

```sh
# Long-running archiver on a Unix-domain socket
bin/neotape-archiver --listen unix:///run/neotape/home.sock \
                     --archive-name home --volume-block-size 4M \
                     -C /data photos docs

# Write one volume to a tape device
bin/neotape-write --source unix:///run/neotape/home.sock \
                  --target tape:/dev/nst0

# Write one volume to a filesystem spool
bin/neotape-write --source unix:///run/neotape/home.sock \
                  --target spool:./vol1.spool

# Raw byte-stream store via stdin
some-command | bin/neotape-raw-store --listen unix:///run/neotape/raw.sock

# Read from spool and feed to extractor
bin/neotape-read --source spool:./vol1.spool \
                 --connect unix:///run/neotape/extractor.sock

# Inspect a spool or tape for compliance
bin/neotape-inspect --source spool:./vol1.spool
bin/neotape-inspect --source tape:/dev/nst0

# Pack a directory with 4 I/O threads
bin/mt-pax -f backup.tar --io-thread 4 src/

# Stream to stdout and pipe to bsdtar
bin/mt-pax -f - --io-thread 2 src/ | bsdtar -tvf -

# Multi-threaded with output buffer tuning for tape
bin/mt-pax -f /dev/nst0 --io-thread 4 --output-buffer-size 256M -P 50 src/
```

## Payload Format

NeoTape core is payload-format agnostic. Any byte stream can be transported in frames and logical slices. The recommended default for POSIX backup is a standard pax/tar stream: slice boundaries may be chosen at pax member boundaries, and the concatenated `ch_content` payload bytes form a valid pax stream on output, compatible with `bsdtar` and `libarchive`.

Other payload formats are possible without changing the NeoTape framing layer. Examples include **ZFS send stream** and **Btrfs send stream**, where each dataset snapshot is mapped to one or more logical slices and the reader emits bytes compatible with the corresponding `zfs receive` or `btrfs receive` command.

## License

GNU General Public License v3.0 or later. See [`LICENSE`](LICENSE).
