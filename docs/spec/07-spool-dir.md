# Spool Directory Format

Status: normative.

## Scope

A NeoTape spool directory is a filesystem representation of one or more NeoTape archive virtual volumes. The spool layout preserves the same logical record order, tape-file boundaries, and frame semantics as tape mode.

This chapter is the authoritative source for spool directory layout, filename
grammar, and spool reader ordering rules.

A spool writer MUST preserve the same logical sequence of NeoTape records as tape mode. A conforming spool archive MUST NOT require a different reader algorithm for payload correctness. A reader MAY treat spool files as a virtual tape: file boundaries stand in for filemarks.

## Directory Layout

A spool archive is a directory tree:

```text
<spool-root>/
  recovery-bundle.tar
  neotape-<file-num>.<type>.nts
  ...
```

The optional `recovery-bundle.tar` is a plain pax archive placed at the spool root for human recovery. It is not part of the NeoTape archive stream and a reader MUST ignore it when locating the first NeoTape frame.

The `<spool-root>` is the top-level directory of the spool archive, conventionally named `<archive_label>-<archive_uuid>/` for a single-archive spool.

## Tape File Names

Each NeoTape tape file is stored as a regular file. The file name encodes the position, type, and identity of the tape file:

```text
neotape-<file-num>.<type>[-<detail>].nts
```

where:

- `<file-num>` — zero-padded integer, the sequential tape-file number within the spool (0-based).
- `<type>` — one of `slice-<slice-seq>`, `archive-end`.
- `[-<detail>]` — optional qualifier.
- `.nts` — extension for "NeoTape Spool".

Examples:

```text
Under spool-dir:
recovery-bundle.tar
neotape-000000.slice-000000.nts
neotape-000001.slice-000001.nts
neotape-000002.slice-000002.nts
neotape-000003.archive-end.nts
neotape-000004.slice-000000.nts
neotape-000005.slice-000001.nts
neotape-000006.slice-000002.nts
neotape-000007.archive-end.nts
```

There is no dedicated Volume Header tape file. Volume boundaries are physical/operator events and are detected by `volume_seq_num` changes or sequence continuity checks.

Sequence numbers embedded in filenames are deliberately present so that a reader can enumerate candidate files by scanning names matching the expected pattern, then sort them numerically to determine the correct playback order. Zero-padding to 6 digits is a convention. Tools MUST parse the numeric value from each name and sort numerically.

`file-num` is scoped to the spool root and continues increasing when a new
archive is appended. `slice-seq` is scoped to one archive instance and restarts
at 0 for each new `archive_uuid`, as illustrated above.

## Semantics

- **File boundary** = a filemark. Each regular file corresponds to one NeoTape tape file.
- **Record order** = the numeric order of `neotape-<file-num>` files, then the byte order within each file.

## Multiple Archives and Append

A spool directory is not constrained to a single archive. A spool root MAY contain tapes from multiple independent archives in sequence. Each new archive begins after the previous `archive-end` file.

Appending a new archive to an existing spool is permitted.

## Block Size and Record Framing

Spool files MUST preserve the same `volume_block_size_kib` semantics as tape mode. Each NeoTape record within a spool file is exactly `volume_block_size_kib * 1024` bytes. A spool writer MUST store records as fixed-size blocks within the regular file, and the reader MUST be able to determine record boundaries from the file contents alone.

## Reader Model

A reader SHOULD be able to accept either a tape device path or a spool directory path. When reading from a spool directory, the reader SHOULD validate:

- the same shared validation rules defined in
  [docs/spec/05-validation.md](05-validation.md), plus
- spool-specific file enumeration and ordering rules from this chapter
