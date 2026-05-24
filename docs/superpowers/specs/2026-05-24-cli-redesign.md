# NeoTape CLI Redesign

Date: 2026-05-24
Status: approved design

## Goals

Redesign the NeoTape CLI around a single primary executable while preserving
standalone tools that have diagnostic or benchmarking value. The CLI should make
the stable user workflows clear, keep tape and spool backends at the same
abstraction level, and separate filesystem/PAX backup semantics from raw byte
transport semantics.

The redesign keeps these project rules:

- stdout is data-plane output only for payload-producing commands.
- diagnostics, progress, warnings, and prompts go to stderr or `/dev/tty`.
- initialized tape and spool backends are both medium-like archive containers.
- archive writing never implicitly initializes a target.

## Command Taxonomy

The primary interface is one executable:

```sh
neotape <subcommand> [options]
```

Stable subcommands:

```text
init      initialize a medium/backend target
list      list archive instances on an initialized medium
plan      create PAX/source-tree plan metadata
backup    write a PAX-profile archive instance
restore   read a PAX-profile archive instance as pax bytes
write     write a raw-profile archive instance
read      read a raw-profile archive instance as raw bytes
```

Profile mapping is fixed by subcommand:

```text
backup/restore  -> PAX profile
write/read       -> raw profile
plan             -> PAX/source-tree planning metadata
init/list        -> medium/archive lifecycle, profile-neutral
```

The profile is not selected by passing `--payload-profile` on the main CLI. Raw
transport is a first-class workflow, not a debug-only mode.

## Standalone Tools

`neotape-inspect` remains a standalone low-level debugging and forensic tool. It
may grow independently from the stable main CLI and can expose detailed header,
Frame, manifest, corruption, and recovery diagnostics.

`mt-pax` remains a standalone tool. Its CLI does not need to change as part of
this redesign. It remains useful for direct PAX stream generation, benchmarking,
and debugging, even if the main CLI reuses its library internals.

Legacy command entry points may remain during migration:

```text
neotape-plan         superseded by neotape plan
neotape-write        split into neotape backup and neotape write
neotape-cat-volumes  split into neotape restore and neotape read
neotape-init         superseded by neotape init
```

## Locator Grammar

Backend references use a simple locator:

```text
<kind>:<locator>
```

Parsing rules:

```text
kind    = bytes before the first ':'
locator = bytes after the first ':'
```

Only the first colon is special. This is not a URI syntax and does not perform
escaping or `://` interpretation. Both `kind` and `locator` must be non-empty.

Initial stable locator kinds:

```text
tape    sequential tape backend, e.g. tape:/dev/nst0
spool   filesystem spool backend, e.g. spool:./archive.spool
```

Relative and absolute spool paths are valid. Tape device paths are locator
strings and are not special-cased by the CLI parser beyond the `tape` kind.

## Command I/O Vocabulary

`init` uses a positional target because it has one primary object:

```sh
neotape init tape:/dev/nst0 --label MYTAPE001
neotape init spool:./archive.spool --label TEST001
```

Archive-reading and archive-writing commands use explicit source/target options:

```sh
neotape list --source tape:/dev/nst0
neotape backup --target spool:./archive.spool ./src
neotape restore --source spool:./archive.spool --output -
neotape write --target tape:/dev/nst0 --input payload.bin
neotape read --source tape:/dev/nst0 --output payload.bin
```

Source-tree commands use positional source paths:

```sh
neotape plan [-C <dir>] -o <plan> <source> [source ...]
neotape backup --target <target> [-C <dir>] <source> [source ...]
neotape backup --target <target> -p <plan>
```

Rules:

```text
-C may appear at most once.
positional <source> may appear multiple times.
-p <plan> is self-contained and mutually exclusive with -C and positional sources.
```

Raw byte-stream commands use explicit byte-stream inputs and outputs:

```sh
neotape write --target <target> --input <file|->
neotape read --source <source> --output <file|->
```

## Medium Lifecycle

Tape and spool targets have the same lifecycle model at the CLI layer. Both must
be initialized before archive instances can be written.

Initialization:

```sh
neotape init tape:/dev/nst0 --label MYTAPE001
neotape init spool:./archive.spool --label TEST001
neotape init spool:./archive.spool --label TEST001 --force
```

`init` creates or rewrites medium-level identity, header, and backend context. It
does not write an archive instance.

Spool filesystem semantics:

```text
If path does not exist: create it and initialize.
If path exists and is an empty directory: initialize.
If path exists and is non-empty: refuse unless --force.
If path exists and is not a directory: refuse.
```

`--force` is destructive. For spool targets, the UX contract is that previous
contents are no longer preserved as a valid initialized medium.

Archive writing never initializes:

```text
backup/write never initialize.
No --init-if-missing.
No --init-if-blank.
No implicit spool creation during backup/write.
```

## Append Semantics

An initialized medium is an archive sequence container. `backup` and `write`
append a new archive instance by default.

Rules:

```text
If the medium has only a Medium Header and no archive, write the first archive.
If prior archive instances exist, strict append preflight is required.
Strict preflight must verify that the previous archive ended with a clean Archive End Header.
If the tail is incomplete, damaged, or unverifiable, refuse to append.
No --append flag is needed.
No dangerous force append belongs in the primary v0.1 UX.
```

## Volume Change Prompt

When `backup/write` requires the next writable volume, or `restore/read` requires
the next readable volume before the selected archive has reached Archive End, the
command enters a shared volume-change prompt.

Prompts use `/dev/tty`; prompt text must never go to stdout.

Prompt shape:

```text
Volume change required for archive <uuid>, expected volume <N>.

Options:
  [c] Continue
  [d] Change device
  [s] Shell
  [a] Abort
```

Option semantics:

```text
Continue
  Retry the current source/target locator. Use this when the user has already
  changed media in the same tape drive or prepared the expected spool volume at
  the same path.

Change device
  Prompt for a replacement locator using the same <kind>:<locator> grammar, such
  as tape:/dev/nst1 or spool:/mnt/next-volume. Then verify that locator as the
  expected next volume. The replacement applies only to the running operation.

Shell
  Spawn $SHELL, or /bin/sh if SHELL is unset, attached to /dev/tty. When the
  shell exits, return to the same volume-change prompt. This allows users to run
  init, mt, mount, copy, or diagnostic commands in the current session.

Abort
  Stop the operation and exit non-zero. For read/restore the output is partial.
  For backup/write the archive is incomplete unless it had already closed before
  the prompt.
```

Every Continue or Change Device attempt must verify expected archive UUID,
volume sequence, medium/header compatibility, and read/write capability.

Control options:

```text
--control=auto
--control=none
```

`auto` uses the prompt when `/dev/tty` is available. If no `/dev/tty` is
available, it fails with diagnostics. `none` never prompts and fails immediately
when another volume is required.

## Read, Restore, And List Semantics

`read` and `restore` read the first archive instance by default. They do not scan
forward to the last archive. This keeps the normal reader path simple and
deterministic.

Explicit archive selection:

```sh
neotape restore --source tape:/dev/nst0 --archive 2 --output -
neotape read --source spool:./archive.spool --archive <uuid> --output payload.bin
```

For v0.1, `--archive` accepts a 1-based archive index from `neotape list` or an
archive UUID. Archive-name matching is not supported because names may not be
unique.

`list` lists archive instances only:

```sh
neotape list --source tape:/dev/nst0
neotape list --source spool:./archive.spool
```

It does not list PAX file contents and does not depend on catalog contents.
Suggested human-readable output:

```text
INDEX  UUID  NAME  PROFILE  VOLUMES  CREATED  STATUS
1      ...   home  pax      2        ...      clean
2      ...   raw1  raw      1        ...      incomplete
```

Machine output is available with `--json`.

Output behavior:

```text
restore stdout/output is PAX payload bytes only.
read stdout/output is raw payload bytes only.
diagnostics, warnings, progress, and prompts go to stderr or /dev/tty.
list stdout is human-readable listing by default, or JSON with --json.
```

PAX `restore` is profile-aware. If the PAX profile requires an End-of-Archive
marker for downstream `bsdtar` compatibility, `restore` may append it at Archive
End according to PAX profile policy. Raw `read` emits exactly selected archive
`SLICE_CONTENT` bytes and appends no profile finalization data.

## Plan And PAX Backup Semantics

`plan` creates self-contained PAX/source-tree plan metadata:

```sh
neotape plan [-C <dir>] -o <plan> <source> [source ...]
```

Rules:

```text
-C may appear at most once.
<source> may appear one or more times.
The plan records enough source context to be used later without passing -C or sources again.
The plan must not support multiple chdir contexts.
If /chdir exists in the plan metadata, it must be the first record and may appear only once.
```

Direct PAX backup without a plan:

```sh
neotape backup --target tape:/dev/nst0 [-C <dir>] <source> [source ...]
```

The whole archive is one logical slice unless backend volume exhaustion forces
continuation. There is no extra planned slicing and no heuristic slice splitting
in the unplanned path.

Planned PAX backup:

```sh
neotape backup --target tape:/dev/nst0 -p <plan>
```

`backup` honors planned slice boundaries from the plan. The planner can choose
slice boundaries to avoid splitting normal files where practical. Very large
files may still exceed a planned slice and require continuation.

Archive path behavior follows tar-like semantics:

```sh
neotape plan -C /data -o home.plan photos docs
neotape backup --target spool:archive.spool -p home.plan
```

This stores archive paths like:

```text
photos/...
docs/...
```

## Raw Write And Read Semantics

Raw write:

```sh
neotape write --target tape:/dev/nst0 --input payload.bin
neotape write --target spool:archive.spool --input -
```

Rules:

```text
write uses the raw profile.
write appends a complete raw-profile archive instance to an initialized medium.
write uses the same strict append preflight as backup.
write does not initialize the target.
write treats input as uninterpreted bytes.
```

Raw read:

```sh
neotape read --source tape:/dev/nst0 --output -
neotape read --source spool:archive.spool --archive 2 --output payload.bin
```

Rules:

```text
read uses the raw profile.
read emits exactly selected archive SLICE_CONTENT bytes.
read does not append PAX End-of-Archive or other profile finalization data.
read selects the first archive by default.
```

## Option Placement And Defaults

`init` options:

```text
--label <text>
--force
--virtual-tape-size <bytes>   spool targets only
```

`--virtual-tape-size` is a spool medium policy. It is accepted by `init` for
spool targets, persisted in spool manifest or initialization metadata, and read
by later `backup/write` operations. It is not accepted by `backup/write` in the
stable UX. Tape targets do not accept it because real tape capacity is reported
by backend/device behavior.

`backup/write` options:

```text
--name <name>
--volume-block-size <bytes>
--control=auto|none
```

`--name` sets `archive_name` for the new archive instance.

`--volume-block-size` is archive/Volume Header policy and belongs to
`backup/write`, not `init`. The default is 4 MiB. This is half of the 8 MiB
maximum supported LTO block size documented for current target drives, should be
supported by non-ancient LTO devices, and still gives large sequential records.

`plan` options:

```text
--slice-size <bytes>
-o <plan>
```

`read/restore` options:

```text
--archive <index|uuid>
--output <file|->
--control=auto|none
```

`list` options:

```text
--source <locator>
--json
```

## Exit And Stream Conventions

Exit status:

```text
0  success
1  runtime failure, verification failure, device/backend error, or user abort
2  CLI usage error
```

Stream conventions:

```text
read/restore stdout: payload bytes only unless --output redirects to a file.
list stdout: human-readable archive list, or JSON if --json is specified.
plan stdout: plan metadata when -o - is used.
All diagnostics, progress, and warnings: stderr.
All interactive prompts and spawned shell interaction: /dev/tty.
```
