# NeoTape

A seekable multi-volume length-framed payload transport container designed for LTO tape drives.

NeoTape wraps a payload byte stream (typically a POSIX pax/tar archive) in a lightweight framing layer that provides per-slice and per-segment structure, integrity verification via BLAKE3, and multi-volume continuation. The on-tape format uses LTO filemarks at logical-slice boundaries, enabling native tape seek to independently verifiable checkpoint units without requiring per-segment filemarks.

This is a design-stage project. The current implementation is a **Phase 0 pax writer** — a libarchive-based CLI that produces a plain POSIX pax-format tar stream. The NeoTape framing layer (volume headers, segment headers with SLICE_END, TRAILER_METADATA segments, catalog, tape backend) exists only as a specification.

## Specification Status

The active format specification lives under `docs/spec/`. When a topic is
defined in `docs/spec/`, that definition supersedes any older or conflicting
text in `docs/RFC_Draft.md`.

`docs/RFC_Draft.md` remains useful as background design material and historical
draft text, but it is no longer the authoritative source for sections that have
been split into `docs/spec/`.

## Hierarchy

```
Archive
  └─ Volume                   volume_seq_num, one physical medium or virtual volume
       └─ Logical Slice       writer-chosen payload range, filemark-delimited
            └─ Physical Segment  length-framed record with segment_payload_size
```

A NeoTape **Archive** is the complete backup set, identified by an `archive_uuid`. It spans one or more **Volumes** (physical LTO media or virtual volumes in spool mode), each carrying a `volume_seq_num`. Inside each volume, the payload is split into **Logical Slices** — writer-declared byte ranges that are independently verifiable via the slice's BLAKE3 digest and seekable by LTO filemark. A typical slice target size is ~64 GiB but a slice may far exceed that, especially when a single large file spans the entire archive. Each logical slice is composed of one or more **Physical Segments**; a segment header explicitly declares `segment_payload_size`, so the reader knows exactly how many bytes to read without parsing payload content. Segment size is determined by the writer's memory buffer (commonly ~4 GiB), since the writer streams payload directly without spooling entire slices to disk. A segment may continue across volumes if end-of-tape is reached mid-slice.

All records in an archive volume use a fixed **volume_block_size** declared in the Volume Header. The writer must commit to this block size before writing any payload and must use it for every subsequent record; readers treat a block-size change within a volume as a format error.

## Project Status

| Phase | Description                         | Status |
| ----- | ----------------------------------- | ------ |
| 0     | pax writer CLI                      | Done   |
| 1     | Binary header layout                | Spec   |
| 2     | Filesystem spool backend            | Spec   |
| 3     | Minimal reader                      | Spec   |
| 4     | NeoTape/PAX integration             | Spec   |
| 5     | TRAILER_METADATA segments & catalog  | Spec   |
| 6     | Tape device backend                 | Spec   |
| 7     | Recovery & salvage                  | Spec   |
| 8     | Medium Header & self-description    | Spec   |
| 9     | Filesystem-native payload profiles  | Spec   |

See `docs/spec/` for the active format specification, `docs/RFC_Draft.md` for
background draft material, and `docs/ROADMAP.md` for the implementation plan.

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

Produces `bin/pax`.

## Usage

```sh
bin/pax -f <output-file|-> [-v|-vv] [-x] <path> [path ...]
```

- `-f`  Output file path, or `-` for stdout
- `-v`  Verbose output (file listing)
- `-vv` Very verbose output (detailed metadata per entry)
- `-x`  Stay within one file system (do not cross mount points)

All diagnostics are written to stderr. Stdout contains pure archive payload bytes. On completion, a BLAKE3 digest of the output is printed to stderr.

### Examples

```sh
# Pack a directory into a tar file
bin/pax -f backup.tar src/

# Stream to stdout and pipe to bsdtar
bin/pax -f - src/ | bsdtar -tvf -
```

## Payload Profiles

NeoTape core is payload-format agnostic. Any byte stream can be transported in segments and slices. The **NeoTape/PAX** payload profile is the recommended default for POSIX backup, producing a standard pax/tar stream compatible with `bsdtar` and `libarchive`. In this profile, slice boundaries may be chosen at pax member boundaries, and the concatenated slice payloads form a valid pax stream on output.

Other profile types are possible without changing the NeoTape framing layer. Planned examples include **ZFS send stream** and **Btrfs send stream**, where each dataset snapshot is mapped to one or more logical slices and the reader emits bytes compatible with the corresponding `zfs receive` or `btrfs receive` command.

## License

GNU General Public License v3.0 or later. See `LICENSE`.
