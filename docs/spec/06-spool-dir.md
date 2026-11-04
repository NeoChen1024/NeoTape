# Spool Directory Format

Status: normative.

## Scope

A NeoTape spool directory is a filesystem representation of one or more NeoTape archive virtual volumes. The spool layout preserves the same logical record order, tape-file boundaries, and frame semantics as tape mode.

This chapter is the authoritative source for spool directory layout, filename
grammar, and spool reader ordering rules.

A spool writer MUST preserve the same logical sequence of NeoTape records as tape mode. A conforming spool archive MUST NOT require a different reader algorithm for payload correctness. A reader MAY treat spool files as a virtual tape: file boundaries stand in for filemarks.

## Directory Layout

A spool archive is a directory:

```text
<spool-root>/
  RECOVERY.TAR
  NT<file-num>.NTS
  ...
```

The optional `RECOVERY.TAR` is a plain pax archive placed at the spool root for human recovery. It is not part of the NeoTape archive stream and a reader MUST ignore it when locating the first NeoTape frame.

`<spool-root>` is any directory. Keep its name within 8 characters when the
medium requires it.

## Tape File Names

Each NeoTape tape file is stored as a regular file. Names fit 8.3 so that a
spool can be stored on FAT without long-name support and on ISO 9660 Level 1
media:

```text
NT<file-num>.NTS
```

- `NT` — fixed prefix.
- `<file-num>` — 1 to 6 decimal digits, the sequential tape-file number
  within the spool (0-based). Writers pad to 6 digits; leading zeroes do not
  affect identity.
- `.NTS` — "NeoTape Spool".

Names are case-insensitive: readers MUST accept any letter case and writers
MUST write upper case. Nothing else is encoded in a name. The frame headers
are authoritative for archive identity, slice numbers and the archive end;
the file holding an `archive_end` frame is named like any other.

A writer MAY hold the file being written under another name and rename it
when the tape file is complete; this implementation uses `NT<file-num>.TMP`.
Readers ignore files that do not match the grammar.

Examples:

```text
Under spool-dir:
RECOVERY.TAR
NT000000.NTS      slice 0
NT000001.NTS      slice 1
NT000002.NTS      slice 2
NT000003.NTS      archive_end
NT000004.NTS      slice 0 of a second archive
NT000005.NTS      slice 1
NT000006.NTS      slice 2
NT000007.NTS      archive_end
```

There is no dedicated Volume Header tape file. Volume transitions are backend
or operator events; `volume_seq_num` is advisory, and sequence checks validate
logical continuity rather than identify physical boundaries.

The file number lets a reader enumerate candidate files by the grammar and
sort them numerically to determine the playback order. Tools MUST parse the
numeric value from each name and sort numerically.

`file-num` is scoped to the spool root and continues increasing when a new
archive is appended. Before reading records, readers MUST reject two
candidate files that parse to the same numeric `file-num`, regardless of
their padding. They MUST report an error and stop; lexical tie-breaking is
not allowed.

Writers MUST allocate consecutive file numbers. Readers MAY continue across
missing file numbers only while reporting the missing tape-file boundary and
applying the recovery rules in [04-validation.md](04-validation.md); filename
gaps alone do not establish which frames or payload bytes are missing.

## Semantics

- **File boundary** = a filemark. Each regular file corresponds to one NeoTape tape file.
- **Record order** = the numeric order of `neotape-<file-num>` files, then the byte order within each file.

## Multiple Archives and Append

A spool directory is not constrained to a single archive. A spool root MAY contain tapes from multiple independent archives in sequence. Each new archive begins in the tape file after the one holding the previous archive's `archive_end` frame.

Appending a new archive to an existing spool is permitted.

## Block Size and Record Framing

Spool files MUST preserve the same `volume_block_size_kib` semantics as tape mode. Each NeoTape record within a spool file is exactly `volume_block_size_kib * 1024` bytes. A spool writer MUST store records as fixed-size blocks within the regular file, and the reader MUST be able to determine record boundaries from the file contents alone.

### Partial Records

An interrupted write (no space left, a crash) can leave a file ending in
fewer bytes than one record. Such a partial record is not part of the
archive. A writer MUST NOT acknowledge it and SHOULD remove it, so that the
frame is written again on the next volume. A reader MUST treat it as the end
of that tape file, exactly as a record that was never written, and SHOULD
report that it was ignored.

Running out of space on the spool's filesystem is the spool equivalent of
physical EOT: the writer ends the volume, and the archive continues in
another spool directory.

## Reader Model

A reader SHOULD be able to accept either a tape device path or a spool directory path. When reading from a spool directory, the reader SHOULD validate:

- the same shared validation rules defined in
  [docs/spec/04-validation.md](04-validation.md), plus
- spool-specific file enumeration and ordering rules from this chapter
