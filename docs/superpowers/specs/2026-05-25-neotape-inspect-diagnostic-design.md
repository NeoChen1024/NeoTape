# neotape-inspect Diagnostic Scanner Design

## Goal

Redesign `neotape-inspect` as a low-level diagnostic scanner for NeoTape
physical tape files. It should help debug both real tape media and spool
archives by printing what it can decode, reporting malformed data in place,
and continuing as far as the source allows.

`spool:` and `tape:` are equal source types. The tool should expose the same
inspection semantics for both; only the source backend differs.

## CLI

```sh
bin/neotape-inspect [-r|--read] <spool-dir|spool:<dir>|tape:<device>>
```

The existing bare spool directory form remains supported for compatibility.

Default mode is a fast first-header scan. `-r` / `--read` enables deep frame
reading.

Exit codes:

- `0`: scan completed and no malformed data or consistency errors were found.
- `1`: scan completed or partially completed, and at least one malformed item,
  validation error, or consistency error was found.
- `2`: command-line usage error.

Hard device or filesystem failures that prevent meaningful scanning are errors
and should return non-zero. When possible, the tool should still print any data
decoded before the failure.

## Source Model

Introduce a source-neutral view of the archive as an ordered sequence of
physical tape files.

Each backend provides:

- A stable display label for each physical tape file.
- A way to read the first record/header for default mode.
- A way to stream all records in the tape file for `--read` mode.
- A way to advance to the next physical tape file.

For `spool:`, physical tape files are regular files in the deterministic spool
layout. Slice files may contain multiple frame records.

For `tape:`, physical tape files are filemark-delimited regions on the tape.
Slice tape files may contain multiple frame records before the next filemark.

The inspect core should not branch on `spool:` versus `tape:` except through
this backend interface.

## Default Mode

Default mode performs a quick structural scan:

- Visit each physical tape file in source order.
- Read only the first NeoTape record needed to identify the file.
- Parse the fixed header and print a line with as many decoded fields as are
  reliable.
- If the fixed header cannot be parsed, print `malformed` with a concise reason
  and continue to the next physical tape file when possible.
- Track archive-level consistency in the background and report mismatches as
  errors instead of aborting immediately.

Default mode should not read full frame payloads except as required by the
source backend to obtain the first record.

## Deep Read Mode

`-r` / `--read` performs full frame inspection for both `spool:` and `tape:`.

For frame tape files, it should read every frame record in order and validate:

- Fixed header magic, version, type, and CRC32C.
- Frame payload size is within the volume block size.
- Record size matches the expected volume block size when that size is known.
- Frame payload BLAKE3.
- Zero padding after payload.
- Frame sequence consistency.
- Slice START/END state.
- Slice content size and BLAKE3 at END.

For non-frame tape files, deep mode should decode and validate the first logical
header record. It does not need to invent additional records for file types that
the format defines as one-record files.

When a malformed frame is found, the tool should print the tape file label,
frame position or byte offset when known, the malformed reason, and continue
with the next frame or next tape file when possible.

## Output

Output remains line-oriented text on stdout. Diagnostics should be suitable for
humans and simple scripts.

Example default output:

```text
tape:file-0: medium version=1 crc=ok uuid=ff8910c7-093b-44bc-aee3-523c037c13ea label=test1 block=65536 count=1 initialized=2026-05-25T12:34:56Z flags=0x0000 impl=NeoTape build=...
tape:file-1: volume version=1 crc=ok archive=... name=AIGC-Workflows volume=1 block=65536 profile=pax written=2026-05-25T12:35:00Z flags=0x0000
tape:file-2: frame version=1 crc=ok archive=... volume=1 global=1 slice=1 within=1 payload=64512 type=slice_content flags=0x0001
tape:file-3: malformed reason="bad magic" bytes=65536
summary: files=4 malformed=1 errors=1 warnings=0 archive=... volumes=1 frames=1 slices=0 end=no
```

Example deep read output:

```text
spool:volume-000001/file-000002: frame index=0 offset=0 global=1 slice=1 within=1 payload=64512 type=slice_content flags=0x0001 crc=ok
spool:volume-000001/file-000002: frame index=1 offset=65536 global=2 slice=1 within=2 payload=64512 type=slice_content flags=0x0000 crc=ok
spool:volume-000001/file-000002: frame index=2 offset=131072 malformed reason="payload BLAKE3 mismatch" bytes=65536
summary: files=3 malformed=1 errors=1 warnings=0 archive=... volumes=1 frames=2 slices=0 end=no
```

The exact field order may follow existing code style, but the output should be
consistent between `spool:` and `tape:`.

## Error Handling

Malformed data is not fatal to the scan by itself. It increments error counters,
prints an in-place diagnostic, and causes final exit code `1`.

Examples of malformed or error conditions:

- Short fixed header.
- Bad magic.
- Unsupported header version.
- Unknown header type.
- CRC mismatch.
- Invalid block size.
- Unexpected header order.
- Archive UUID mismatch.
- Volume block size mismatch.
- Frame payload too large.
- Frame payload hash mismatch.
- Non-zero padding.
- Missing archive end.

Tape read errors, filesystem open errors, or inability to advance to the next
file should be printed with context. If they prevent further scanning, the tool
should stop and return `1` after printing the summary when practical.

## Testing

Add coverage for both source types where practical:

- Existing smoke spool archive still inspects successfully in default mode.
- Existing smoke spool archive still inspects successfully with `--read`.
- UTF-8 filename smoke remains covered by backup/restore tests, not by inspect
  payload parsing.
- Malformed spool physical tape file reports `malformed`, continues scanning,
  and exits `1`.
- A spool slice file with a bad frame payload hash reports the frame-level error
  in `--read` mode and exits `1`.
- Existing tape-oriented unit tests continue to pass.

Real `/dev/nst0` validation should be run after implementation if hardware is
available, but automated tests should not depend on real tape hardware.

## Non-Goals

- Do not add JSON output in this change.
- Do not add salvage or extraction behavior to `neotape-inspect`.
- Do not make malformed data exit `0`.
- Do not make `spool:` and `tape:` expose different inspect semantics.
