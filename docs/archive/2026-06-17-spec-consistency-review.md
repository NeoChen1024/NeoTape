# Spec consistency review

Date: 2026-06-17

## Scope

Reviewed all current files under `docs/spec/`:

- `00-format-common.md`
- `01-frame-header.md`
- `02-terminology.md`
- `03-frames-and-slices.md`
- `04-volume-layout.md`
- `05-spool-dir.md`
- `06-security.md`
- `07-open-questions.md`
- `07-future-extensions.md`
- `08-plan-metadata.md`
- `09-appendix-cli.md`
- `10-appendix-layout-examples.md`
- `11-tcp-protocol.md`

This review focuses on cross-document consistency, stale references, and places
where two spec chapters currently prescribe different behavior.

## Findings

### S1. Frame-header field sizes drift across chapters, and one table is internally impossible

Severity: high

Impact:

- A reader or writer implemented from different chapters would derive different
  header offsets for `flags`, `_reserved`, `signature`, and `frame_hash`.
- `01-frame-header.md` currently claims a 512-byte header, but the field sizes
  in its table add up to 508 bytes, so the chapter is internally inconsistent.

Evidence:

- `docs/spec/01-frame-header.md:21-27` defines
  `frame_payload_size` as `uint32` / 4 bytes and `_reserved` as `byte[246]`,
  while also claiming a 512-byte total.
- `docs/spec/02-terminology.md:194-210` defines
  `frame_payload_size` as `uint64`.
- `docs/spec/10-appendix-layout-examples.md:101-109` also shows
  `frame_payload_size (8)` and `_reserved (246)`.

Suggested fix:

- Pick one canonical layout and restate it everywhere.
- Since `01-frame-header.md` is the natural source of truth, other chapters
  should refer back to it instead of re-encoding a second copy of the table.
- If the canonical layout is 512 bytes, correct the `_reserved` size there so
  the arithmetic works.

### S2. The spool model is described in two incompatible forms

Severity: high

Impact:

- The spec currently has two different on-disk spool layouts.
- A reader implemented from `02-terminology.md` would look for different file
  names, different directory nesting, and different auxiliary files than a
  reader implemented from `05-spool-dir.md`.

Evidence:

- `docs/spec/05-spool-dir.md:13-39` defines a flat spool root with
  `neotape-<file-num>.<type>.nts` regular files.
- `docs/spec/02-terminology.md:232-252` defines spool concepts using
  `tape-<seq>` volume directories, `tape-file-<num>.<type>.ntf`, a
  `manifest.json`, and `--virtual-tape-size`.
- `docs/spec/07-open-questions.md:13-19` still treats spool filenames and the
  manifest as open questions, which means the decision status is also out of
  sync with the dedicated spool chapter.

Suggested fix:

- Decide which spool layout is current and remove the other one from the active
  spec.
- If `05-spool-dir.md` is the intended current design, trim the spool section in
  `02-terminology.md` down to neutral terminology and stop restating concrete
  path syntax there.
- Update `07-open-questions.md` so it no longer marks already-chosen spool
  decisions as unresolved.

### S3. Metadata-channel reader behavior conflicts with the TCP protocol chapter

Severity: high

Impact:

- One chapter says corrupt or missing `ch_metadata` must not break normal
  restore behavior, while another says the server validates every frame and
  closes the connection on validation failure.
- This is a behavior-level contradiction, not just wording drift.

Evidence:

- `docs/spec/03-frames-and-slices.md:44-49` says a normal payload reader MUST
  emit only `ch_content`, MUST NOT reject solely because `ch_metadata` is
  missing/truncated/corrupt, and SHOULD warn-and-continue on metadata hash
  failure.
- `docs/spec/11-tcp-protocol.md:87-100` says the Server validates every
  `frame_record` for `frame_hash` and closes the connection on validation
  failure, with no metadata exception.
- `docs/spec/06-security.md:18-22` also reinforces that catalog data is
  advisory and payload metadata is authoritative.

Suggested fix:

- Explicitly distinguish strict compliance scanning from normal restore mode.
- The TCP protocol chapter should either:
  define a metadata exception for restore-mode servers, or
  say that its current validation rules describe strict mode / inspect mode only.

### S4. Plan metadata is described inconsistently, and the example does not match its own grammar

Severity: medium

Impact:

- The appendix says the planner emits JSON, but the plan-format chapter defines a
  NUL-delimited line protocol.
- The example entry format uses a file kind that is not listed as valid, which
  makes the grammar ambiguous even within the same chapter.

Evidence:

- `docs/spec/09-appendix-cli.md:67-74` says `neotape-plan` generates
  "slice-metadata JSON".
- `docs/spec/08-plan-metadata.md:11-16` defines records terminated by `\0\n`.
- `docs/spec/08-plan-metadata.md:40-52` defines valid `<kind>` values as
  `f`, `d`, `l`, `c`, `b`, `p`, `s`.
- `docs/spec/08-plan-metadata.md:65-71` uses `h` in the example record.

Suggested fix:

- Update the CLI appendix to describe the actual record-oriented plan format,
  not JSON.
- Fix the example so it uses one of the defined `<kind>` values and relies on
  the `<hardlink>` field for hardlink semantics.

### S5. The metadata catalog is both "specified" and "still open"

Severity: medium

Impact:

- A reader of the spec cannot tell whether the `ch_metadata` catalog format is
  already chosen or still provisional.
- This creates ambiguity for anyone trying to build interoperable metadata
  tooling.

Evidence:

- `docs/spec/08-plan-metadata.md:58-90` says the plan record format doubles as
  the slice-scoped `ch_metadata` catalog format.
- `docs/spec/07-open-questions.md:21-23` says the exact metadata item table and
  catalog entry format are not settled.

Suggested fix:

- Either declare `08-plan-metadata.md` as the current proposed catalog format
  and narrow `07-open-questions.md` to remaining unsolved details, or
- mark the catalog mapping in `08-plan-metadata.md` as explicitly provisional.

### S6. A stale cross-reference still points at `13-tcp-protocol.md`

Severity: low

Impact:

- The security chapter links to a non-existent spec file, which makes the
  internal navigation misleading and suggests an incomplete renumbering pass.

Evidence:

- `docs/spec/06-security.md:44` references `docs/spec/13-tcp-protocol.md`.
- The current TCP protocol document is `docs/spec/11-tcp-protocol.md`.

Suggested fix:

- Update the link to `11-tcp-protocol.md`.
- Search the repo for other stale `13-tcp-protocol` references in docs and tests
  so the numbering is consistent everywhere.

### S7. Volume-boundary wording conflicts with the multi-volume examples

Severity: low

Impact:

- One part of the spec says a volume boundary is identified by a filemark, while
  the multi-volume examples show EOT interrupting a slice mid-file with
  continuation on the next volume.
- That distinction matters because a physical volume boundary does not always
  coincide with a clean filemark transition.

Evidence:

- `docs/spec/04-volume-layout.md:55` says a volume boundary is "identified by a
  filemark".
- `docs/spec/04-volume-layout.md:85-111` shows EOT interrupting an in-progress
  slice and resuming it on the next tape.
- `docs/spec/11-tcp-protocol.md:72-77` also models the reader side as "hit
  physical EOT → send `tape_eof` → reconnect", not "read a filemark first".

Suggested fix:

- Reword `04-volume-layout.md` so filemarks remain the logical slice/archive
  delimiter, while physical volume boundaries are described as operator/media
  events that may occur without a slice-closing filemark.

## Summary

The most important inconsistencies are:

1. Header layout drift across `01`, `02`, and `11`.
2. Two incompatible spool-directory models in `02` vs `05`.
3. Conflicting restore semantics for corrupt `ch_metadata` in `03` vs `12`.

Those three are worth fixing first because they affect interoperability, not
just editorial clarity.
