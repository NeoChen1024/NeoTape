# Path handling pitfalls

## bsdtar-compatible archive paths

NeoTape archive paths follow the bsdtar/GNU tar convention:

- **Relative paths** are stored as typed (`c/b/b` → `c/b/b`).
- **Absolute paths** only strip the leading `/` (`/tmp/foo` → `tmp/foo`).
- **No prefix stripping.** There is no common-prefix or first-component removal.

The `SourceSpec` struct captures this as two fields: `archive_prefix` (the
user argument with leading `/` stripped and trailing `/` removed) and
`open_path` (the resolved absolute path for `openat`).

### What NOT to do

- Do NOT implement `drop_first_component` or any automatic prefix shortening.
  Previous iterations tried to strip `a/b` from `a/b/c` → `c`, which is not how
  tar works and breaks explicit user intent.

- Do NOT strip trailing slashes from the archive path. The `archive_prefix`
  has its trailing slashes normalized (so `a/b/` has the same effect as `a/b`),
  but the paths of children are always clean relative paths produced by
  `lexically_relative` and do not need further processing.

- Do NOT resolve `-C <dir>` to an absolute path.  The raw user argument is
  stored and passed to `chdir`.  Plan metadata emits `/chdir/<raw>\0\n` as
  typed.

## `-C` (`--directory`) chdir

`-C` changes the working directory *before* scanning.  It does NOT alter the
archive path prefix.  If you specify `-C /some/dir src/`, the archive prefix
is `src/` and files are opened relative to `/some/dir`.

This matches GNU tar behaviour: `tar -C /tmp -cf - foo` stores `foo`, not
`/tmp/foo`.

## `archive_path_for_source` conventions

- **Root entry** (the path argument itself): result is `archive_prefix` alone
  (no trailing `/`, no `./`).
- **Child entry** (under the root): result is `archive_prefix / child_rel`,
  where `child_rel` is `lexically_relative(entry_path, open_path)`.

Do NOT prepend `./` or any other prefix.  Do not attempt to reconstruct the
original command-line argument — the archive path is always a clean relative
path.

## Symlinks

`has_trailing_slash` / `strip_trailing_slashes` were previously used to
detect symlinks-to-directories added via libarchive.  This is no longer
needed at the `SourceSpec` level — the chdir + `openat` approach handles this
transparently.  If a trailing-slash-aware entry is needed by libarchive,
handle it at the writer layer, not in shared path logic.

## Pathnames are opaque bytes

On POSIX filesystems a pathname is a byte sequence, not necessarily text in
the process locale or UTF-8. NeoTape therefore preserves filesystem pathname
bytes exactly in its pax payload. This policy applies to entry pathnames and
to pathname-valued link fields such as symlink and hardlink targets.

The pax writers use libarchive with:

```text
xattrheader=ALL,hdrcharset=BINARY
```

`hdrcharset=BINARY` prevents libarchive from converting pathname bytes through
the current locale. When a pax extended header is needed, libarchive records
the binary charset marker so a compatible reader treats its `path` and link
values as uninterpreted bytes. Valid UTF-8 names are not special-cased: their
existing UTF-8 byte sequences are preserved by the same binary policy.

Do not call `archive_entry_update_pathname_utf8`,
`archive_entry_update_symlink_utf8`, `archive_entry_update_hardlink_utf8`, or
equivalent implicit-conversion APIs on filesystem names. In particular, do not
restore the former `mark_link_target_as_utf8` step. Use the byte-oriented
`archive_entry_copy_*` interfaces and retain `std::filesystem::path::native()`
or ordinary POSIX `char` data when comparing names.

Plan files follow the same rule. The pathname remainder of an entry record is
raw filesystem bytes; parsing must not validate, normalize, decode, or
re-encode it. NUL remains impossible in a POSIX pathname and is also the plan
record terminator. `/` remains the path-component separator. Treating content
bytes as opaque does not change structural archive-path rules such as stripping
the leading slash from an absolute source argument.

Diagnostics and user interfaces must not assume these bytes are printable.
Escape invalid or control bytes when displaying a path, and keep the original
bytes available for filesystem operations and error identification. A display
conversion must never be written back into archive metadata.

### Why this is the default

A real input tree exposed mixed UTF-8 and CP932 names. Forcing
`hdrcharset=UTF-8` made libarchive attempt locale-dependent conversion during
`archive_write_header`; ordinary failures produced warnings that the writer
could turn into omitted entries, and one long run reported a fatal pathname
allocation error. The exact trigger for that fatal `ENOMEM` was not reproduced,
so it must not be documented as a proven charset-conversion bug. The important
invariant is independent of that incident: guessing an encoding cannot be both
lossless and correct for a mixed POSIX tree, whereas byte preservation is
reversible.

The regression test `mt-pax preserves opaque pathname and symlink target
bytes` creates an invalid-UTF-8 filename and symlink target, archives them, and
requires byte-identical names after extraction. The null-writer integration
test also sends an archive containing such a pathname through the complete
frame-validation path.

### Portability tradeoff

Consumers must understand pax `hdrcharset=BINARY` to recover non-UTF-8 names
without conversion; libarchive/bsdtar does. Software that assumes every archive
name is Unicode may reject or misdisplay these entries. That loss of text-level
portability is intentional: NeoTape prioritizes exact restoration of the source
filesystem namespace over guessing or silently replacing characters.

This policy is scoped to filesystem names carried inside the pax payload. It
does not change NeoTape fields that the format specification explicitly defines
as UTF-8, such as `archive_label`.

## History

- **4-field SourceSpec** (original): `archive_path`, `open_path`, `original_arg`,
  `trailing_slash`.  Over-engineered, had `drop_first_component` for prefix
  stripping, and used symlink fixup logic that was confusing.
- **2-field SourceSpec** (current): `archive_prefix`, `open_path`.  Simpler,
  correct, matches tar semantics by construction.
- **UTF-8-forced pax headers** (removed): pathname and link bytes were passed
  through libarchive's charset conversion. The writer now uses the binary pax
  charset and keeps those fields byte-opaque.
