# CLI and Multi-Volume Session Redesign

Date: 2026-05-20
Status: Draft design

## Problem

The current tools grew from implementation milestones: `pax`, `mt-pax`,
`neotape-plan`, `neotape-write`, `neotape-init`, `neotape-cat-volumes`, and
inspection helpers. This was useful while building infrastructure, but the
long-term CLI should reflect the actual NeoTape workflow.

An early redesign idea split archive generation and tape writing into separate
processes connected by a FIFO or stdin/stdout stream. That model is attractive
because it keeps each command narrow, but it has serious problems for real
multi-volume tape operation:

- FIFOs deliver EOF when the writer process closes the pipe.
- Slices can be very large and are not capped; 8-64 GiB or more is expected.
- A slice may span multiple tape media.
- Restarting archive generation only at slice boundaries can force huge replay.
- Adding frame acknowledgements, bounded replay queues, and producer restarts
  recreates a session coordinator outside the tools.

The CLI should preserve streamability and small checkpoint metadata without
forcing large payload spools in memory or on disk.

This objection is specifically about splitting the archive producer from the
tape writer during archive creation. A FIFO can still be useful as a restore
payload sink when `neotape-archive-read` itself remains the coordinated tape
session process.

## Decision

Keep separate Unix-style tools for stable responsibilities, but make live
multi-volume archive writing and reading single coordinated session processes.

The session process keeps internal components separated by bounded queues, not
external FIFOs. This avoids FIFO lifetime problems while preserving streaming
behavior and avoiding full-archive spooling.

## Tool Set

### `neotape-plan`

Optional source scanning and slice planning pass.

Usage shape:

```sh
neotape-plan --job-dir <dir> [options] <path> [path ...]
```

Responsibilities:

- Build a durable file list and optional slice plan for one archive job.
- Store standardized plan files inside `--job-dir`.
- Improve restartability, predictability, and reporting.
- Remain optional; normal archive writing must work without a prior plan.

The plan contains metadata only. It must not spool payload data.

### `neotape-archive-write`

Primary coordinated multi-volume archive writer.

Usage shape:

```sh
neotape-archive-write --job-dir <dir> -f <tape-device> [options] <path> [path ...]
```

Responsibilities:

- Walk source paths directly, or consume the optional plan from `--job-dir`.
- Generate the PAX payload stream internally, initially using/refactoring the
  `mt-pax` pipeline. (`mt-pax` refactoring is done, library interface should be usable)
- Generate NeoTape Volume, Frame, Slice, and Archive End structures.
- Write to one or more initialized tape media.
- Manage volume transitions within one long-running process.
- Keep payload streaming through bounded queues; do not require full payload,
  full pax stream, or full slice spooling.
- Sliced pax stream requires using `neotape-plan` beforehand. (Also enables
  resumable backup, hardlink handling will require further investigating)

Data Path:

```
[plan-file (optional)] -> [mt-pax writer (has internal bounded buffer)] ->
	[neotape slice & framing handling] -> [tape / spool writer] -> [job metadata]
```

On medium full / EOT:

1. Stop the tape backend at the last safe frame boundary when possible.
2. Close the tape device.
3. Persist job checkpoint state.
4. Ask for the next medium according to `--on-volume-end`.
5. Reopen the tape device.
6. Validate the Medium Header on the new medium.
7. Synthesize and write the next Volume Header.
8. Continue the same archive stream.

The process may block internal producer threads while waiting for the next
medium. It must not require an external FIFO to survive volume changes.

### `neotape-archive-read`

Primary coordinated multi-volume restore reader.

Usage shape:

```sh
neotape-archive-read --job-dir <dir> -f <tape-device> [options] |
  bsdtar -xpf - --acls --xattrs
```

Alternative FIFO restore shape:

```sh
mkfifo restore.pax
bsdtar -xpf restore.pax --acls --xattrs &
neotape-archive-read --job-dir <dir> -f <tape-device> --output restore.pax [options]
```

Responsibilities:

- Read one archive across multiple tape media in one coordinated session.
- Emit one continuous payload stream to stdout by default, or to
  `--output <path>` when specified.
- Keep stdout pure payload bytes when stdout is selected as the payload output.
- Send diagnostics, prompts, and progress to stderr or `/dev/tty`.
- Preserve tar/pax hardlink behavior by not restoring slice-by-slice.

On end of current medium:

1. Stop reading the current tape.
2. Close the tape device.
3. Persist restore checkpoint state.
4. Ask for the next medium according to `--on-volume-end`.
5. Reopen and validate the next initialized medium.
6. Locate the expected next Volume Header.
7. Continue emitting payload bytes to the same selected output stream.

The receiving end is a standard `bsdtar` or GNU `tar` process reading either
from a pipe connected to stdout or from a FIFO named by `--output`. NeoTape
should not make the normal restore workflow depend on slice-local extraction.

### `neotape-tape-init`

Initialize one physical tape medium.

Usage shape:

```sh
neotape-tape-init -f <tape-device> --label <text> [options]
```

Responsibilities:

- Write a Medium Header.
- Fill medium-level identity and metadata fields.
- Store an optional metadata bundle as an ar archive.
- Never write archive-specific fields.

Medium UUIDs and Archive UUIDs are separate identities:

- Medium UUID identifies one initialized physical medium.
- Archive UUID identifies one logical archive instance.
- `neotape-archive-write` records associations between archive volumes and
  medium UUIDs, but does not derive one UUID kind from the other.

`neotape-archive-write` and `neotape-archive-read` must validate the Medium
Header before reading or writing archive data. If the medium is not initialized,
they fail with a clear diagnostic telling the user to run `neotape-tape-init`.

### `neotape-tape-ls`

List archive volumes stored on one tape medium.

Usage shape:

```sh
neotape-tape-ls -f <tape-device> [--json] [--verbose]
```

Responsibilities:

- Scan one physical medium.
- List Volume Headers, Archive End Headers, archive UUIDs, archive names,
  volume sequence numbers, and approximate slice ranges when available.
- Treat Medium Header corruption as a warning, not a fatal error.
- Continue best-effort scanning when the medium header is missing or corrupt.
- Never emit payload bytes.

This tool is inventory/diagnostic oriented and should be useful before starting
a restore job or while auditing tape library contents.

### Existing/Transitional Tools

- `bin/pax` and `bin/mt-pax` remain useful development/debug tools.
- `mt-pax` internals should be refactored for use by `neotape-archive-write`.
- `neotape-cat-volumes` can remain as a spool/prototype reader during the
  transition, but the long-term physical restore command is
  `neotape-archive-read`.
- `neotape-init` should be renamed to `neotape-tape-init`; no public alias is
  required unless compatibility becomes a real concern.

## Job Directory

`--job-dir <dir>` is the shared state directory for one archive or restore job.
It stores metadata and checkpoint state, not bulk payload data.

Recommended layout:

```text
job.json
plan.ntplan
archive.state.json
restore.state.json
tape.state.json
volume-header-template.ntblk
volumes/
  volume-000000.json
  volume-000001.json
logs/
tmp/
```

`job.json` stores archive/job identity and global options. Per-tool state files
store checkpoints and are updated atomically.

The job directory may contain small temporary or checkpoint records, but the
default architecture must not require spooling the full archive, pax stream, or
large slice payloads into it.

## Checkpoints

There are two distinct checkpoint concepts:

- Frame checkpoints are transport/session checkpoints.
- Slice checkpoints are restore/integrity checkpoints.

Frames are the practical restart and volume-switching unit. A slice may span
multiple frames and multiple tape media, so requiring slice-boundary replay for
media changes is not acceptable.

Slices remain the minimum unit of guaranteed recovery and validation. The job
state records completed slices when their final frame and verification fields
have been processed.

`neotape-archive-write` should checkpoint at completed frame boundaries and
record completed slice boundaries when available.

`neotape-archive-read` should similarly checkpoint completed frame progress and
record completed validated slices.

## Volume Headers

The archive session owns archive identity. The tape backend owns physical volume
placement.

`neotape-archive-write` writes the first Volume Header from the archive stream
state, then synthesizes later Volume Headers when new media are loaded. Later
headers carry the same archive identity but an incremented `volume_seq_num` and
new physical placement information as required by the format.

The first Volume Header can be stored as `volume-header-template.ntblk` in the
job directory. Later Volume Headers are generated from this template with fields
updated and CRCs recomputed.

## Volume-End Policy

Both `neotape-archive-write` and `neotape-archive-read` support volume
transition policy options, but not every policy is safe for both directions:

```text
--on-volume-end=prompt
--on-volume-end=sigstop
--on-volume-end=exec:<cmd>
--on-volume-end=exit
```

Policy meanings:

- `prompt`: wait on `/dev/tty` for a human to load the next initialized medium.
- `sigstop`: close the tape, persist state, then stop the process for an
  external tape-library supervisor to resume with `SIGCONT`.
- `exec:<cmd>`: run a changer command, then continue.
- `exit`: stop cleanly after current medium so a wrapper can restart from the
  job directory.

The default should be human-friendly, likely `prompt`. `exec` is the preferred
automation mode for both writing and reading.

`sigstop` is suitable for archive writing, where the process does not need to
keep stdout connected to a downstream payload consumer. It is also suitable for
restore when `neotape-archive-read` writes payload bytes to a FIFO through
`--output <fifo>` and leaves stdout connected to the controlling TTY.

`sigstop` is not suitable for the default piped restore form where stdout is the
payload stream connected directly to `bsdtar` or GNU `tar`; job-control behavior
for a stopped background process depends on the controlling TTY. In that form,
restore automation should use `prompt`, `exec:<cmd>`, or a purpose-built
supervisor that preserves the stdout pipe to the tar process.

## Restore Semantics

Normal restore emits one continuous payload stream to a standard tar reader.
NeoTape slices are transport and validation units, not normal restore units.

Example:

```sh
neotape-archive-read --job-dir ./restore-job -f /dev/nst0 |
  bsdtar -xpf - --acls --xattrs
```

This avoids breaking pax/tar hardlink handling, which can depend on archive-wide
entry ordering.

When stdout is the payload stream, restore prompts and changer control must not
use stdout. They belong on `/dev/tty`, stderr, or the configured
`--on-volume-end=exec:<cmd>` path. When `--output <path>` is used, diagnostics
should still avoid stdout unless the output mode explicitly documents stdout as
human-facing.

When `--output <fifo>` is used, the same continuous stream is written to the
FIFO instead. This keeps tar connected to one uninterrupted payload source while
allowing `neotape-archive-read` to keep stdout on the controlling TTY, which
makes `--on-volume-end=sigstop` viable for restore automation.

## Streamability Requirements

All primary archive workflows must be streamable:

- No full archive pre-spool is required.
- No full pax stream is held in memory.
- No full slice payload is required in memory or in the job directory.
- Bounded queues and bounded buffers are allowed.
- Optional planning metadata is allowed and encouraged for large jobs.

The optional spool backend remains useful for testing, optical/removable media
staging, and debugging, but it is not the default physical tape architecture.

## Implementation Priorities

1. Rename `neotape-init` to `neotape-tape-init` and keep Medium Header handling
   focused on physical medium identity.
2. Define the `--job-dir` layout and atomic state update helpers.
3. Refactor `neotape-plan` output into a durable plan format stored under the
   job directory.
4. Refactor `mt-pax` internals so planned and unplanned archive writing can use
   the same streaming serializer.
5. Build `neotape-archive-write` as the coordinated multi-volume writer.
6. Build `neotape-archive-read` as the coordinated multi-volume reader to
   stdout by default, with optional FIFO/file payload output.
7. Add `neotape-tape-ls` for best-effort physical medium inventory.

## Non-Goals

- Do not require Unix domain sockets or file descriptor passing.
- Do not require external FIFOs between archive generation and tape writing.
- Do not require FIFO-based restore, though it is allowed as a payload output
  mode for workflows that need `--on-volume-end=sigstop`.
- Do not make slice boundaries the only restart boundary for tape writing, that should be frames.
- Do not make the job directory a mandatory full-payload spool.
- Do not derive Medium UUIDs from Archive UUIDs or vice versa.
