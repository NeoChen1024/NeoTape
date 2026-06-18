# Plan Metadata Format

Status: normative.

The `neotape plan` command emits a machine-readable metadata stream describing
how source filesystem trees should be packed into slices. Downstream tools
consume this stream to produce pax slices that match the plan.

This chapter is the authoritative source for both the planning stream consumed
by `neotape-archiver --plan` and the slice-scoped catalog record format written
into `ch_metadata`.

**Required for resumable archive creation.**

## Record Format

Every record is a single line terminated by `\0\n` (NUL byte followed by
newline). NUL cannot appear in file paths, so `\0\n` is an unambiguous record
separator even when paths contain embedded newlines.

The leading path component after the initial `/` determines the record type.
Records whose leading component is a decimal `<slice>` number are **entry
records**. A record whose leading component is `chdir` is a **directive** —
it MUST appear at most once and MUST be the first record in the stream. If
present, it changes the working directory for all subsequent entry records.

## Record Types

### `/chdir/<path>\0\n`

Changes the working directory before processing subsequent sources. The `<path>` is
a filesystem path to a directory or symlink to a directory (resolved when there's trailing `/`).

This directive is emitted whenever the user specified `-C <dir>` on the command
line.

`/chdir/` MUST appear at most once and MUST be the first record in the stream.
All entry records that follow are resolved relative to this directory.

### `/<slice>/<file_num>/<kind>/<size>/<mtime>/<uid>/<uname>/<gid>/<gname>/<filepath>\0\n`

An entry record. Fields:

| Field          | Description                                                                                                            |
| -------------- | ---------------------------------------------------------------------------------------------------------------------- |
| `<slice>`    | Slice number, not zero-padded, 0-based.                                                                                |
| `<file_num>` | Index within the slice, not zero-padded, 0-based.                                                                      |
| `<kind>`     | File type: `f` regular, `h` hardlink, `d` directory, `l` symlink, `c` char, `b` block, `p` fifo, `s` socket.        |
| `<size>`     | Apparent file size in bytes (decimal). Hardlink entries (`<kind> = h`) MUST use `0`.                                 |
| `<mtime>`    | Modification time as Unix timestamp (decimal seconds).                                                                 |
| `<uid>`      | Numeric owner user ID (decimal).`0` when unknown.                                                                    |
| `<uname>`    | Owner user name as a string (empty when unknown).                                                                      |
| `<gid>`      | Numeric group ID (decimal).`0` when unknown.                                                                         |
| `<gname>`    | Group name as a string (empty when unknown).                                                                           |
| `<filepath>` | Archive path (relative, may include the source-directory prefix, no leading `/`).                                    |

All fields are mandatory — every entry record carries all 9 fields. Unknown
or unavailable values use a reasonable zero sentinel (`0`, `""`, or timestamp `0`)
rather than being omitted.

This record doubles as the `ch_metadata` catalog for each logical slice. A
downstream reader can parse the same record format to list archive contents,
compute per-slice progress, or verify slice integrity against the planned
file set.

## Example

```
/chdir//home/user\0\n
/0/0/f/1234/1718400000/1000/neo_chen/1000/neogroup/src/main.c\0\n
/0/1/h/0/1718400000/1000/neo_chen/1000/neogroup/share/also-main.c\0\n
/0/2/d/0/1718400000/1000/neo_chen/1000/neogroup/src/\0\n
/0/3/f/5678/1718400000/0//0//docs/readme.txt\0\n
```

## Catalog Role (`ch_metadata`)

The plan metadata stream serves a dual purpose:

1. **Planning** — the archiver reads entry records to determine slice boundaries
   and file packing order.
2. **Catalog** — the same entry records, when written into `ch_metadata` frames
   within each logical slice, form a machine-readable index of the slice's
   contents. A reader can parse them to list files, verify completeness, or
   display progress without inspecting the `ch_content` payload.

The catalog is slice-scoped: each logical slice's `ch_metadata` contains only
the entry records whose `<slice>` field matches the enclosing
`logical_slice_seq_num`. This keeps metadata streaming and avoids the need for
an archive-level preamble that would require knowing total file counts in
advance.

`/chdir/` directives are *not* written into `ch_metadata` — they are
planning-only instructions consumed by the archiver.
