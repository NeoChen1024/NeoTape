# libarchive Pax Writer Notes

Status: implementation note.

## Overview

NeoTape uses libarchive's pax writer to produce POSIX pax/tar byte streams. The
writer is initialized once (original single-threaded writer) or per-entry
(multi-threaded `mt-pax` with EOA suppression).

## Initialization

### Single-Threaded Writer

```cpp
struct archive *a = archive_write_new();
archive_write_add_filter_none(a);           // no compression
archive_write_set_format_pax(a);            // POSIX.1-2001 pax
archive_write_set_options(a, "xattrheader=ALL,hdrcharset=UTF-8");
archive_write_open(a, &sink, drop_open, sink_write, drop_close);
```

A single archive writer is opened at the start and closed at the end. EOA (two
zero-filled 512-byte blocks) is produced exactly once during the final
`archive_write_close()`.

### Per-Entry Writer (mt-pax, `src/mt-pax.cpp`)

Each entry gets its own archive writer:

```cpp
struct archive *a = archive_write_new();
archive_write_add_filter_none(a);
archive_write_set_format_pax(a);
archive_write_set_options(a, "xattrheader=ALL");
archive_write_open(a, ctx, drop_open, drop_write, drop_close);
```

The per-entry writer writes exactly one entry (header + data) and then the EOA
is suppressed via the drop-before-close pattern (see `phase-3.5-mt-pax-writer.md`).

## Key libarchive Options

### `xattrheader=ALL`

Preserves all extended attributes, including `security.capability`. Without this
flag, libarchive may skip security-related xattrs.

### `hdrcharset=UTF-8`

Ensures pax extended headers use UTF-8 encoding. Used in the single-threaded
writer; the per-entry writer inherits this from the archive format default.

## Entry Writing Pattern

### Single Entry

```cpp
archive_write_header(a, entry);          // write pax extended header + tar header
archive_write_data(a, buf, size);        // write file data
archive_write_finish_entry(a);           // finalize entry metadata
```

Directory entries, symlinks, and other zero-size entries skip the
`archive_write_data` call.

## Hardlink Resolution

Hardlinks are resolved using `archive_entry_linkresolver`:

```cpp
struct archive_entry_linkresolver *resolver = archive_entry_linkresolver_new();
archive_entry_linkresolver_set_strategy(resolver, ARCHIVE_FORMAT_TAR_PAX_INTERCHANGE);

// For each entry from the disk walker:
struct archive_entry *matched = nullptr;
archive_entry_linkify(resolver, &entry, &matched);
if (matched)
    archive_entry_linkresolver_free(resolver);  // or process matched entry
```

The resolver detects hardlinks by (device, inode) pairs and sets
`archive_entry_hardlink()` on subsequent entries pointing to the first
occurrence.

## Directory Traversal

### Single-Threaded

Uses `archive_read_disk` for filesystem traversal:

```cpp
struct archive *rd = archive_read_disk_new();
archive_read_disk_set_symlink_physical(rd);    // follow symlinks
while (archive_read_next_header2(rd, entry) == ARCHIVE_OK) {
    // process entry
}
```

Symlink following is controlled by the `-x` (one file system) flag using
`archive_read_disk_set_callback_skip_filesystem()`.

## Important Caveats

- libarchive's pax writer produces EOA only during `archive_write_close()`, not
  during `archive_write_finish_entry()`. This is why the drop-mode EOA
  suppression works correctly.
- `archive_write_data()` accepts a buffer and length and returns the number of
  bytes consumed. Large files should be streamed in chunks rather than loaded
  entirely into memory.
- The pax format stores metadata in extended headers (`SCHILY.xattr.*` for
  xattrs). libarchive handles this transparently when
  `archive_write_set_format_pax()` is used.
- `archive_write_finish_entry()` finalizes checksums and sizes for the current
  entry. It must be called before the next entry's header.
