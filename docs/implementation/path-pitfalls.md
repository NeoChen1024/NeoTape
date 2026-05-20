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

## History

- **4-field SourceSpec** (original): `archive_path`, `open_path`, `original_arg`,
  `trailing_slash`.  Over-engineered, had `drop_first_component` for prefix
  stripping, and used symlink fixup logic that was confusing.
- **2-field SourceSpec** (current): `archive_prefix`, `open_path`.  Simpler,
  correct, matches tar semantics by construction.
