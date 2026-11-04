# Appendix: CLI Reference

Status: non-normative implementation reference.

This appendix documents the current NeoTape tools and their CLI usage.

Every long option has a short alias listed by each command's `-h` output.
Byte-size arguments accept case-insensitive binary `K`, `M`, `G`, and `T`
suffixes (for example `4M` or `16G`); an unsuffixed value is bytes.
Numeric arguments use unsigned decimal digits without signs or whitespace.
Byte sizes must be positive; each option checks its range before narrowing or
allocating. Invalid numeric arguments exit with usage status 2.

## TCP archive pipeline

The archiver and writer are a long-running server / short-lived client pair that
together produce a NeoTape archive stream over a single TCP or Unix-domain
socket:

```sh
# Start the archiver server (long-running, owns archive state):
build/dev/bin/neotape-archiver --listen tcp://0.0.0.0:9000 \
  -C /data photos docs

# Write one volume's worth of data to tape (short-lived, per-volume):
build/dev/bin/neotape-write --source tcp://tapehost:9000 \
  --target tape:/dev/nst0 --erase \
  --recovery-bundle output/bot.tar

# Write to a spool directory instead of a real tape device:
build/dev/bin/neotape-write --source tcp://tapehost:9000 \
  --target spool:./out

# Validate the complete stream without retaining frame data:
build/dev/bin/neotape-write --source tcp://tapehost:9000 \
  --target null --max-volume-bytes 1T
```

`--listen` is required. Use the standalone `mt-pax` command below when a plain
pax archive file is needed.

`neotape-write` exits with status `3` when the current volume is full and the
archive requires another writer invocation. Status `0` means the complete
archive, including `archive_end`, was accepted; `1` reports a runtime or source
error, and `2` reports invalid command-line usage.

By default the writer refuses to overwrite existing content. `--erase` rewinds
to BOT and overwrites; `--append` spaces to the end of data and continues
there. `--resume-mode` writes from the current position without moving the
tape; see [Resuming an interrupted archive](#resuming-an-interrupted-archive).

`-R, --recovery-bundle <tar>` is available with `--erase` or on an empty
medium. A tape target
writes the bundle at BOT using separate 256 KiB records by default. Use
`-r, --recovery-bundle-block-size <SIZE>` to override that size. The writer
pads the final bundle record, writes a filemark, and then writes NeoTape frames
at the volume's own block size. A spool target copies the original tar to
`RECOVERY.TAR`; it is deliberately outside the numbered `.NTS` stream.

An explicit `--max-volume-bytes` also provides a software capacity boundary
for tape targets. It stops before the next complete record would exceed the
limit, without waiting for physical EOT; tape recovery-bundle padding counts
toward this limit. Without this option, tape writes stop at the drive's EOT
indication. A spool target treats a full filesystem (`ENOSPC`) the same way:
the record that did not fit is removed and not acknowledged. All of these
boundaries return status 3 when another volume is needed; for a spool, the
next volume is another spool directory.

## Resuming an interrupted archive

A plan-driven archive that was interrupted can be continued from the start of
a slice instead of from the beginning. The archiver records what it needs in
a resume file; the operator positions the medium; the writer does not look at
the position.

```sh
# First run: home.resume does not exist yet and is created.
build/dev/bin/neotape-archiver --listen unix:///tmp/neotape.sock \
  --plan home.plan --archive-name home --resume-file home.resume

# After the failure: load the volume that holds the start of the slice,
# position after the filemark that ends the previous slice, then run the
# same command again. An existing resume file means "continue".
build/dev/bin/neotape-archiver --listen unix:///tmp/neotape.sock \
  --plan home.plan --archive-name home --resume-file home.resume
build/dev/bin/neotape-write --source unix:///tmp/neotape.sock \
  --target tape:/dev/nst0 --resume-mode
```

An existing resume file is continued from its last slice record, which names
the slice that was being written when the run stopped. The resumed run reuses
the archive UUID, continues the global frame sequence and the volume number
from that record, and does not write the catalog again. A file without any
slice record means slice 1 was never reached; the archive then starts over
from slice 0 under the same UUID. Once the archive is complete the file ends
with an `/archive_end/` record and the archiver refuses to use it again. To
start a different archive, use a new file. `--archive-name`,
`--volume-block-size`, and the signing key may be omitted where they have a
default; if given, they must match the resume file. The plan file must be the
original one: its BLAKE3 hash is stored in the resume file and a different
plan is refused, because replanning can move slice boundaries and leave files
out of the archive.

The slice being resumed is written again in full, so everything on the medium
from its first frame on is replaced. That first frame may be on an earlier
volume than the one in the drive when the run failed; the volume number in
the record says which one to load. Later volumes of the interrupted run are
no longer part of the archive.

Entries of the skipped slices are still examined on disk, without reading
their contents, so that a hardlink in a resumed slice to a file in a skipped
slice is written as a hardlink. If such a file has since been removed, the
link is written as a regular file with its contents.

`neotape-write --resume-mode` cannot be combined with `--erase`, `--append`,
or `--recovery-bundle`. On a tape it writes at the current position. A spool
has no position: the writer continues after the highest-numbered tape file,
so remove the files of the slice being resumed first.

### Resume file

The resume file uses the record framing of plan files: every record starts
with `/`, the first component names the record type, and the record ends with
`\0\n`. The last field of a record extends to the terminator and is not
escaped.

| Record | Meaning |
| --- | --- |
| `/neotape-resume/1` | Format and version; always the first record. |
| `/archive_uuid/<uuid>` | Archive UUID to reuse. |
| `/block_size_kib/<n>` | Record size in KiB, as in the frame header. |
| `/sign_key_id/<hex>` | 16 hex digits of the signing key ID; empty when unsigned. |
| `/plan_blake3/<hex>` | 64 hex digits; empty when unknown, which skips the plan check with a warning. |
| `/archive_name/<bytes>` | Archive label bytes, which may include `/` and newline. |
| `/slice/<slice>/<global_seq>/<volume>` | Slice `<slice>` starts at global frame sequence `<global_seq>` on volume `<volume>`. |
| `/archive_end/<global_seq>` | The `archive_end` frame with this sequence number was acknowledged. Always the last record. |

The five header records appear once each, before any `/slice/` record. A
`/slice/` record is appended, and synced, when the writer acknowledges the
first frame of that slice; the previous slice and its filemark are then on
the medium. Slice 0 is not recorded. The number of `/slice/` records with the
same volume number is the number of slice filemarks to skip on that volume,
not counting a recovery bundle or other tape files ahead of the archive.

Records are only ever appended. A `/slice/` record for slice N supersedes the
earlier records for N and for every later slice; a resumed run appends such a
record before it sends its first frame. A final record without its terminator
is the trace of a crash and is ignored; any other malformed record is an
error.

`neotape-scan --resume-output <file>` rebuilds a resume file from the medium
when the original is lost. It needs a source that holds exactly one archive
and leaves `plan_blake3` empty.

## Raw byte-stream store

`neotape-raw-store` is the raw-stream counterpart to `neotape-archiver` server
mode.  It reads one uninterpreted byte stream from stdin by default, or from
`--input <file|->`, packs it directly into `ch_content` frames, and serves those
NeoTape records to `neotape-write` over TCP or a Unix-domain socket:

```sh
# Store raw bytes from a file:
build/dev/bin/neotape-raw-store --listen tcp://0.0.0.0:9000 \
  --archive-name disk-image --input image.raw

# Or store raw bytes from a pipeline:
dd if=/dev/nvme0n1 bs=4M | \
  build/dev/bin/neotape-raw-store --listen unix:///tmp/raw-store.sock
```

The entire input stream is one slice (`slice_seq_num = 0`). Within that slice,
`channel_frame_seq_num` starts at 0 and increments for each `ch_content` frame.
The first content frame is the one with `channel_frame_seq_num = 0`, only the
last content frame carries `END`, and the store emits `tape_eof` before the
final `archive_end` frame.

## Standalone pax writer

```sh
build/dev/bin/mt-pax -f output.pax --io-thread 4 -P 50 ./source
```

Multi-threaded pax writer with worker pool.  `--io-thread N` spawns N-1
workers for small files and streams large files through the serializer.
`-P <percent>` is the output-buffer waterline write restart threshold.

## Planner

```sh
build/dev/bin/neotape-plan -C /data -o home.plan photos docs
```

Generates the record-oriented plan metadata stream consumed by
`neotape-archiver --plan`; see [09-plan-metadata.md](../spec/09-plan-metadata.md).
The archiver also stores the plan file as the archive catalog at the start of
slice 0, so the plan must be a regular file that stays unchanged while the
archiver runs. `--plan-write-mode no` leaves the catalog out; `slice0` is the
default.

## Catalog preview

```sh
# The catalog stored on the media, unchanged (it is the plan file):
build/dev/bin/neotape-catalog --source tape:/dev/nst0 > home.plan

# One line per entry, or one line per slice:
build/dev/bin/neotape-catalog --source spool:./in --list
build/dev/bin/neotape-catalog --plan home.plan --summary
```

`neotape-catalog` shows what an archive holds without restoring it. With
`--source` it reads the catalog from the start of the archive's first volume
and stops at the catalog's last frame. With `--plan` it formats a plan file
that has not been archived yet. The catalog must fit on that volume; one
that continues onto a second volume is not supported.

The default output is the catalog bytes, which `neotape-archiver --plan`
accepts again. `--list` prints slice, kind, size, modification time (local
time), owner/group, and path for each entry; `--summary` prints entry and
byte totals per slice. `/chdir/` records are not shown.

The tape is left where reading stopped. Foreign tape files ahead of
the first frame, such as the recovery bundle, are skipped. The command fails
when the archive has no catalog, when the position is not the start of an
archive, or when a catalog frame fails validation; output written before such
a failure is incomplete. Signature options match `neotape-extractor`.

## Extractor / Reader (reading pipeline)

The extractor and reader are a long-running server / short-lived client pair for
reading NeoTape archives back. The extractor applies the shared validation rules
from [04-validation.md](../spec/04-validation.md) and reassembles the pax content
stream:

```sh
# Start the extractor server (long-running, validates frames):
build/dev/bin/neotape-extractor --listen tcp://0.0.0.0:9000 -o output.pax

# Read one volume from tape and feed it to the extractor:
build/dev/bin/neotape-read --source tape:/dev/nst0 --connect tcp://tapehost:9000

# Read from a spool directory:
build/dev/bin/neotape-read --source spool:./in --connect tcp://tapehost:9000
```

`--metadata-output <file>` additionally writes the archive catalog, which for
a plan-driven archive is the original plan file. Catalog frames that fail
their hash are left out with a warning.

Normal extraction rejects any missing or damaged content record.
`--salvage` skips invalid records and relaxes archive-level consistency; the
output then has gaps and is reported as not fully verified.

## Reader (TCP client)

```sh
# Connect to extractor and read from tape:
build/dev/bin/neotape-read --source tape:/dev/nst0 --connect tcp://extractor_host:9000

# Connect to extractor and read from spool:
build/dev/bin/neotape-read --source spool:./in --connect unix:///tmp/extractor.sock
```

Reads NeoTape records from a tape device or spool directory and forwards them
to an extractor server over TCP or Unix-domain socket.  The extractor drives
the protocol (pull model).  One reader instance handles one volume; when the
volume is exhausted the reader disconnects and the operator starts a new
instance for the next volume.

## Inspect tool

`neotape-inspect` scans a spool directory or tape device and prints a
human-readable table of every NeoTape frame header with frame hash
verification status, followed by a compliance report:

```sh
# Inspect a spool directory:
build/dev/bin/neotape-inspect --source spool:./out

# Inspect a tape device:
build/dev/bin/neotape-inspect --source tape:/dev/nst0
```

The compliance report applies the full conformance rules from
[04-validation.md](../spec/04-validation.md), including per-frame structure and
integrity, archive identity consistency, sequence continuity, channel ordering,
and `archive_end` rules.

Like `neotape-read`, both `neotape-inspect` and `neotape-scan` skip records
before the first NeoTape magic, such as a BOT recovery bundle, as a
non-NeoTape prefix. Inspect lists and counts them without reporting an issue;
scan `-v` names the skipped tapefile.

## Scan tool

`neotape-scan` reads only the first NeoTape frame from each tapefile in a spool
directory or tape, deduplicates by `archive_uuid` plus `archive_label`, and
prints each new archive identity immediately when first seen. With `-v`, it
also prints every tapefile's first frame and marks whether that frame
introduced a new archive identity:

```sh
# Summarize archive identities found in a spool:
build/dev/bin/neotape-scan --source spool:./out

# Also list each tapefile's first frame on tape:
build/dev/bin/neotape-scan --source tape:/dev/nst0 -v
```

`-r, --resume-output <file>` additionally writes a
[resume file](#resume-file) for the archive on the source: its identity, and
a slice start for every tape file that opens with the first frame of a slice.

## Dump tool

```sh
build/dev/bin/neotape-dump --source tape:/dev/nst0 --target spool:./raw-dump -v
```

Copies every physical record from the tape's current position into numerically
ordered spool files, preserving filemark boundaries. It performs no header
parsing, hash verification, archive identity checks, or sequence checks. The
target directory must be empty.

## Signing and verification

NeoTape does not generate keypairs. Create one with OpenBSD `signify` or a
compatible implementation and pass the resulting `.sec` and `.pub` files.

| Tool | Option | Effect |
|---|---|---|
| `neotape-archiver`, `neotape-raw-store` | `--sign-secret-key <file.sec>` | Sign every frame. `--sign-passphrase-file` unlocks an encrypted key. |
| `neotape-write` | `--verify-pubkey <file.pub>` | Authenticate the source server by challenge-response before touching media, then verify each signed frame before writing it. |
| `neotape-extractor`, `neotape-inspect`, `neotape-catalog` | `--verify-pubkey <file.pub>` | Verify frame signatures. Without a key, signed frames are used but reported as unverified. |
| same | `--require-signed` | Reject unsigned or untrusted frames. Needs at least one `--verify-pubkey`. |

See [08-security.md](../spec/08-security.md) for the trust model.

## Backend locators

Backend locators use `<kind>:<locator>` syntax.  The split occurs at the first
colon only, so locator paths may contain additional colons.

| Kind | Syntax | Used by |
|------|--------|---------|
| `tape:` | `tape:/dev/nst0` | `neotape-write --target`, `neotape-read --source` |
| `spool:` | `spool:./dir` | `neotape-write --target`, `neotape-read --source` |
| `null` | `null` | `neotape-write --target` |
| `tcp:` | `tcp://host:port` | `neotape-archiver --listen`, `neotape-raw-store --listen`, `neotape-extractor --listen`, `neotape-write --source`, `neotape-read --connect` |
| `unix:` | `unix:///path/socket` | `neotape-archiver --listen`, `neotape-raw-store --listen`, `neotape-extractor --listen`, `neotape-write --source`, `neotape-read --connect` |

Tools that read a `tape:` locator (`neotape-read`, `neotape-inspect`,
`neotape-scan`, `neotape-dump`, `neotape-catalog`) start at the tape's current
position and never rewind, before or after reading: an archive need not start
at BOT. Position the tape first, for example with `mt -f /dev/nst0 rewind`.

Opening a `tape:` locator for reading or writing first requests variable
block mode (`MTSETBLK 0`), so each record is one physical tape block. A drive
that refuses it produces a warning and stays in its current mode; it then
works only if its fixed block size equals every record size used.

## Output conventions

| Stream | Content |
|--------|---------|
| stdout | Pure payload bytes (for readers) or structured output (for inspect/plan). |
| stderr | Diagnostics, progress, warnings, BLAKE3 hash of output. |
| `/dev/tty` | Interactive prompts for volume change, error resolution. |

Every diagnostic line starts with the executable name. Default output contains
only lifecycle events, warnings, errors, and one authoritative completion
summary per process; connection-local protocol details are debug output.
Progress display updates are serialized with ordinary diagnostics so an event
always starts on a fresh line.

Frame counters use explicit names:

- `committed_frames` counts unique frames acknowledged by the writer.
- `frame_transmissions` counts all frame records sent, including retransmits.
- `forwarded_frames` counts records sent by one reader invocation.
- `processed_frames` counts records handled by the extractor, excluding suppressed retries.

While `neotape-write` is active it updates an mbuffer-style status line once
per second:

```text
in @   136M/s, out @   133M/s, frames @     33/s, volume      4, slice     21, frame    1107012,   1.4T total, buffer  82% full
```

`in` counts bytes that passed frame validation; `out`, `frames`, and the
volume/slice/frame position advance only after the target operation succeeds.
`total` is the frame bytes handled by this writer invocation, and `buffer` is
the queued byte percentage relative to `--output-buffer-size`. The status does
not estimate physical tape capacity.

Paths in diagnostics and listings keep printable ASCII and well-formed UTF-8
as they are. A backslash is written as `\\`; control characters, bytes that
are not valid UTF-8, and code points that reorder or hide text (bidirectional
controls such as U+202E, zero-width characters, tag characters) are written
as `\xNN`, so a displayed path cannot be rearranged to look like another.
Visually similar letters from different scripts are not detected. This escaping affects display only; archived and planned
pathnames remain byte opaque.
