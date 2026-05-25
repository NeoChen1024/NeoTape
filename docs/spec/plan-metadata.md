# Plan Metadata Format

Status: draft.

The `neotape plan` command emits a machine-readable metadata stream describing
how source filesystem trees should be packed into slices. Downstream tools
consume this stream to produce pax slices that match the plan.

**Required for resumable archive creation.**

## Record Format

Every record is a single line terminated by `\0\n` (NUL byte followed by
newline). NUL cannot appear in file paths, so `\0\n` is an unambiguous record
separator even when paths contain embedded newlines.

The first field of each record determines its type. Records with a leading
decimal digit are **entry records**; all other records are **directives**.

## Record Types

### `/chdir/<path>\0\n`

Changes the working directory before processing subsequent sources. The `<path>` is
a filesystem path to a directory or symlink to a directory (resolved when there's trailing `/`).

This directive is emitted whenever the user specified `-C <dir>` on the command
line.

Chdir directives appear in the order the user specified them and remain in
effect for all subsequent entry records.

### `/<slice>/<file_num>/<kind>/<size>/<filepath>\0\n`

An entry record. Fields:

| Field          | Description                                                                                                 |
| -------------- | ----------------------------------------------------------------------------------------------------------- |
| `<slice>`    | Slice number, not zero-padded, 0-based.                                                                     |
| `<file_num>` | Index within the slice, not zero-padded, 0-based.                                                           |
| `<kind>`     | File type:`f` regular, `d` directory, `l` symlink, `c` char, `b` block, `p` fifo, `s` socket. |
| `<size>`     | Apparent file size in bytes (decimal).                                                                      |
| `<filepath>` | Archive path (relative, may include the source-directory prefix, no leading `/`).                         |

## Example

```
/chdir//home/user\0\n
/0/0/f/1234/src/main.c\0\n
/0/1/f/5678/docs/readme.txt\0\n
```
