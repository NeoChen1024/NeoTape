# Spool Directory Format

Status: specification.

## Scope

A NeoTape spool directory is a filesystem representation of one or more NeoTape
archive virtual volumes. The spool layout preserves the same logical record
order, tape-file boundaries, header semantics, but without volume transition as
the tape-device backend, it uses ordinary files and directories instead of
sequential tape operations and filemarks.

A spool writer MUST preserve the same logical sequence of NeoTape records as
tape mode. A conforming spool archive MUST NOT require a different reader
algorithm for payload correctness. A reader MAY treat spool files as a virtual
tape: file boundaries stand in for filemarks, just like an arbitrarily long
tape volume.

## Directory Layout

A spool archive is a directory tree:

```text
<spool-root>/
  recovery-bundle.tar
  neotape-<file-num>.<type>.nts
  ...
```

The optional `recovery-bundle.tar` is a plain pax archive placed at the spool
root for human recovery. It is not part of the NeoTape archive stream and a
reader MUST ignore it when locating the first Volume Header.

The `<spool-root>` is the top-level directory of the spool archive,
conventionally named `<archive_name>-<archive_uuid>/` for a single-archive spool.

## Tape File Names

Each NeoTape tape file is stored as a regular file inside its tape directory.
The file name encodes the position, type, and identity of the tape file:

```text
neotape-<file-num>.<type>[-<detail>].nts
```

where:

- `<file-num>` — zero-padded integer, the sequential tape-file number within
  the spool, matching the LTO tape-file position (0-based). This is NOT the
  volume-relative content number; it is the logical tape-file index encoding
  the canonical ordering of NeoTape records within the volume.
- `<type>` — one of `volume-header`, `slice-<slice-seq>`, `archive-end`.
- `[-<detail>]` — optional qualifier such as continuation markers or Frame
  content-type hints, allowed but not required for correctness.
- `.nts` — extension for "NeoTape Spool".

Examples:

```text
Under spool-dir:
recovery-bundle.tar
tape-file-000000.volume-header.nts
tape-file-000001.slice-000001.nts
tape-file-000002.slice-000002.nts
tape-file-000003.slice-000003.nts
tape-file-000004.archive-end.nts
tape-file-000005.volume-header.nts
tape-file-000006.slice-000001.nts
tape-file-000007.slice-000002.nts
tape-file-000008.slice-000003.nts
tape-file-000009.archive-end.nts
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
fall back to scanning the directory tree, opening each `.nts` file, and
validating its NeoTape headers to determine volume membership, record order,
and content integrity.

## Semantics

- **File boundary** = a filemark. Each regular file in a tape directory
  corresponds to one NeoTape tape file, delimited in tape mode by a filemark.
- **Record order** = the numeric order of `tape-file-<file-num>` within a tape
  directory, then the byte order within each file.

The spool directory SHOULD contain a `tape-file-000000.volume-header.nts` if
the spool represents an archive volume.

## Multiple Archives and Append

A spool directory is not constrained to a single archive. Because an archive
is a logical concept identified by `archive_uuid`, a spool root MAY contain
tapes from multiple independent archives in sequence. Each new archive begins
at the next available `tape-<seq>` directory, starting from the last existing
tape directory or a fresh spool root.

Appending a new archive to an existing spool is permitted.

## File and Slice Number Range

Volume sequence numbers and tape-file numbers use zero-padded decimal notation.
The padding width is 6 digits for conventional readability and sorted `ls`
output by name:

```text
neo-tape-000000.volume-header.nts
neo-tape-000001.slice-000001.nts
neo-tape-000002.slice-000002.nts
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
      "files": [
        {"tape_file_num": 0, "path": "neo-tape-000001.volume-header.nts"},
        ...
      ]
    }
  ]
}
```

The `archives[]` array supports multiple archive instances when a tape
directory is reused via append. Each entry contains:

- `archive_uuid` — UUID of the archive instance
- `archive_name` — optional human-readable name of the archive instance
- `files[]` — per-file entries with `tape_file_num` and `path` (relative to tape directory)

The manifest is advisory. Restore correctness comes from NeoTape headers,
lengths, and checksums inside the spool files. A tool MAY use the manifest
as auxiliary recovery information without opening every `.nts` file. If no
manifest is found, the tool MUST fall back to opening each `.nts` file and
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
- `EOT` — no more data in current spool

When reading from a spool directory, the reader SHOULD validate:

- NeoTape magic and header type
- Header CRC32C
- `archive_uuid` consistency across volumes
- Volume sequence number continuity
- Slice continuation rules
- Final `SLICE_CONTENT` Frame Header integrity
- Archive End Header integrity when present
