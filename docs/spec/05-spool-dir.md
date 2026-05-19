# Spool Directory Format

Status: specification.

## Scope

A NeoTape spool directory is a filesystem representation of one or more NeoTape
archive virtual volumes. The spool layout preserves the same logical record
order, tape-file boundaries, header semantics, and volume transition rules as
the tape-device backend, but uses ordinary files and directories instead of
sequential tape operations and filemarks.

A spool writer MUST preserve the same logical sequence of NeoTape records as
tape mode. A conforming spool archive MUST NOT require a different reader
algorithm for payload correctness. A reader MAY treat spool files as a virtual
tape: file boundaries stand in for filemarks, and volume directories stand in
for media or archive volumes.

## Directory Layout

A spool archive is a directory tree:

```text
<spool-root>/
  tape-<seq>/
    tape-file-<file-num>.<type>.ntf
    ...
  tape-<seq>/
    ...
```

The `<spool-root>` is the top-level directory of the spool archive,
conventionally named `archive-<archive_uuid>/` for a single-archive spool.

Each `tape-<seq>` directory represents one NeoTape virtual volume. `<seq>` is
a zero-padded volume sequence number.

## Tape File Names

Each NeoTape tape file is stored as a regular file inside its tape directory.
The file name encodes the position, type, and identity of the tape file:

```text
tape-file-<file-num>.<type>[-<detail>].ntf
```

where:

- `<file-num>` — zero-padded integer, the sequential tape-file number within
  the volume, matching the LTO tape-file position (0-based). This is NOT the
  volume-relative content number; it is the logical tape-file index encoding
  the canonical ordering of NeoTape records within the volume.
- `<type>` — one of `medium-header`, `volume-header`, `slice-<slice-seq>`,
  `archive-end`.
- `[-<detail>]` — optional qualifier such as continuation markers or Frame
  content-type hints, allowed but not required for correctness.
- `.ntf` — extension for "NeoTape file".

Examples:

```text
Under tape-000000:
tape-file-000000.medium-header.ntf
tape-file-000001.volume-header.ntf
tape-file-000002.slice-000001.ntf
tape-file-000003.slice-000002.ntf

Under tape-000001:
tape-file-000000.medium-header.ntf
tape-file-000001.volume-header.ntf
tape-file-000002.slice-000003.ntf
tape-file-000003.archive-end.ntf
```

The sequence numbers embedded in filenames are deliberately present so that a
reader can enumerate candidate volume directories and tape files by scanning
names matching the expected pattern, then sort them numerically to determine
the correct playback order. Zero-padding to 6 digits is a convention that
makes naive `ls` output useful for human inspection, but it is not an upper
bound (see "Volume and File Number Range" below). Tools MUST parse the numeric
value from each name and sort numerically; lexical sort by filename alone MAY
produce the wrong order when values exceed 999999.

On a physical tape this ambiguity does not arise, because the medium provides
linear record order and filemarks. The spool directory exists only in
filesystem space, where a glob + numeric sort is the correct recovery
strategy.

A tool MAY use `manifest.json` (see Manifest section) as auxiliary recovery
information to locate volume directories, tape files, and verify sizes or
digests without opening every file. If no manifest is found, the tool MUST
fall back to scanning the directory tree, opening each `.ntf` file, and
validating its NeoTape headers to determine volume membership, record order,
and content integrity.

## Semantics

- **Tape directory** = a virtual tape volume or physical medium instance.
- **File boundary** = a filemark. Each regular file in a tape directory
  corresponds to one NeoTape tape file, delimited in tape mode by a filemark.
- **Record order** = the numeric order of `tape-file-<file-num>` within a tape
  directory, then the byte order within each file.
- **Tape ordering** = the numeric order of `tape-<seq>` directories
  determines the multi-volume sequence.

The first tape directory SHOULD contain a `tape-file-000000.medium-header.ntf`
if the spool represents a complete medium-initialized archive, matching the
Medium Header in tape file 0 of a physical tape. A spool archive without a
Medium Header is valid only if it represents a bare archive volume that a
reader would interpret as already positioned past the Medium Header.

## Tape Capacity Limits

Because filesystems do not provide physical EOT, a spool writer MAY accept a
configured tape capacity limit (e.g. `--virtual-tape-size`). When the next
committed NeoTape record would exceed the configured capacity, the writer
MUST perform the same logical transition it would perform on EOT:

1. Close the current tape at the last valid boundary.
2. Create the next tape directory with a new `volume-header` tape file.
3. Continue or adjust the incomplete NeoTape record in the new tape.

This manual capacity limit is a simulation of media capacity, not an archive
semantic. Writers SHOULD track virtual volume limits in native output bytes.

## Multiple Archives and Append

A spool directory is not constrained to a single archive. Because an archive
is a logical concept identified by `archive_uuid`, a spool root MAY contain
tapes from multiple independent archives in sequence. Each new archive begins
at the next available `tape-<seq>` directory, starting from the last existing
tape directory or a fresh spool root.

Appending a new archive to an existing spool is permitted. The last `tape-<seq>`
directory MAY be reused for the new archive's first tape if its declared
capacity has not been exhausted. If the next commit would exceed the configured
tape size limit, the writer MUST close the current tape and create a new
`tape-<seq>` directory before continuing, following the same transition rules
as EOT.

For example, after writing a complete archive with tapes `tape-000001` through
`tape-000003`, a subsequent archive may begin by appending to the existing
`tape-000003` directory (if capacity remains), or by creating `tape-000004`.

## Volume and File Number Range

Volume sequence numbers and tape-file numbers use zero-padded decimal notation.
The padding width is 6 digits for conventional readability and sorted `ls`
output by name:

```text
tape-000001/  tape-file-000000.medium-header.ntf
tape-000002/  tape-file-000001.volume-header.ntf
tape-000003/  tape-file-000002.slice-000001.ntf
```

However, sequence numbers are NOT bounded by the padding width. Values larger
than 999999 (e.g. `tape-1000000`) MUST be written with however many digits
are needed and MUST be accepted by the reader. Therefore tools MUST NOT use
filename lexicographic order as the authoritative sort — they MUST parse the
numeric value from the filename and sort numerically, or rely solely on the
NeoTape header fields inside the files.

## Manifest

Each tape directory MAY contain a machine-readable manifest (`manifest.json`)
describing the files in that directory:

```json
{
  "archives": [
    {
      "archive_uuid": "<uuid>",
      "archive_name": "<NAME>",
      "volume_seq_num": 1,
      "files": [
        {"tape_file_num": 0, "path": "tape-file-000000.medium-header.ntf"},
        ...
      ]
    }
  ]
}
```

The `archives[]` array supports multiple archive instances when a tape
directory is reused via append. Each entry contains:

- `archive_uuid` — UUID of the archive instance
- `volume_seq_num` — archive-scoped volume sequence number
- `files[]` — per-file entries with `tape_file_num` and `path` (relative to tape directory)

The manifest is advisory. Restore correctness comes from NeoTape headers,
lengths, and checksums inside the spool files. A tool MAY use the manifest
as auxiliary recovery information without opening every `.ntf` file. If no
manifest is found, the tool MUST fall back to opening each `.ntf` file and
validating its NeoTape headers.

## Block Size and Record Framing

Spool files MUST preserve the same `volume_block_size` semantics as tape mode.
Each NeoTape record within a spool file is exactly `volume_block_size` bytes.
A spool writer MAY store records as fixed-size blocks within the regular file,
and the reader MUST be able to determine record boundaries from the file
contents alone.

## Reader Model

A reader SHOULD be able to accept either a tape device path or a spool
directory path, with the target adapter providing these events:

- `read-record` — read the next NeoTape record
- `next-file` — advance to the next tape file (next regular file in directory)
- `next-volume` — advance to the next virtual volume (next tape directory)
- `EOT` / `volume-limit` — no more data in current volume

When reading from a spool directory, the reader SHOULD validate:

- NeoTape magic and header type
- Header CRC32C
- `archive_uuid` consistency across volumes
- Volume sequence number continuity
- Slice continuation rules
- Final `SLICE_CONTENT` Frame Header integrity
- Archive End Header integrity when present

## Removable Media Usage (Optical Discs)

A spool tape directory can be burned to CD/DVD/BD or copied to removable
filesystem media. Each tape directory is self-contained and can be used as
the source tree for a disc image or direct burn operation. The reader then
accepts mounted tape directories or disc paths and reconstructs the archive
stream.

Optical capacity examples (practical payload budgets, reserving space for
headers, metadata, and filesystem overhead):

- CD-R: approximately 650–700 MiB
- DVD-R: approximately 4.3 GiB
- BD-R SL: approximately 25 GB
- BD-R DL: approximately 50 GB

These budgets are advisory. The writer or wrapper tool should choose suitable
virtual volume size limits for the target media.

## Non-Goals

- The spool format is not a replacement for the tape-device backend.
- It does not require a dedicated optical writer backend in NeoTape core.
- It does not require direct control of optical drives by NeoTape tools.
- It does not require ISO/UDF image generation inside the core writer.
- It does not require tape-device append semantics (seek-to-EOD safety).
