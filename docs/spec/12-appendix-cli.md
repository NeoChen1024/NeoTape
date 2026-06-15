# Appendix: CLI Reference

Status: non-normative implementation reference.

This appendix documents the current NeoTape tools and their CLI usage.

## TCP archive pipeline

The archiver and writer are a long-running server / short-lived client pair that
together produce a NeoTape archive stream over a single TCP or Unix-domain
socket:

```sh
# Start the archiver server (long-running, owns archive state):
bin/neotape-archiver --listen tcp://0.0.0.0:9000 \
  -C /data photos docs

# Write one volume's worth of data to tape (short-lived, per-volume):
bin/neotape-write --source tcp://tapehost:9000 \
  --target tape:/dev/nst0

# Write to a spool directory instead of a real tape device:
bin/neotape-write --source tcp://tapehost:9000 \
  --target spool:./out
```

Without `--listen`, the archiver behaves like `mt-pax` and writes a plain pax
stream to `-f`:

```sh
bin/neotape-archiver -f output.pax -C /data photos docs
```

## Standalone pax writer

```sh
bin/mt-pax -f output.pax --io-thread 4 -P 50 ./source
```

Multi-threaded pax writer with worker pool.  `--io-thread N` spawns N-1
workers for small files and streams large files through the serializer.
`-P <percent>` is the output-buffer waterline write restart threshold.

## Planner

```sh
bin/neotape-plan -C /data -o home.plan photos docs
```

Generates slice-metadata JSON consumed by `neotape-archiver --plan`.

## Reader (raw record copier)

```sh
# Copy records from a spool to another spool:
bin/neotape-read --source spool:./in --target spool:./out

# Copy from a real tape device:
bin/neotape-read --source tape:/dev/nst0 --target spool:./out
```

`neotape-read` is a raw NeoTape record copier.  It reads frame records from a
tape device or spool directory and writes them to a spool target.  It does
*not* extract pax payload or pipe to `bsdtar` in this version.

## Backend locators

Backend locators use `<kind>:<locator>` syntax.  The split occurs at the first
colon only, so locator paths may contain additional colons.

| Kind | Syntax | Used by |
|------|--------|---------|
| `tape:` | `tape:/dev/nst0` | `neotape-write --target`, `neotape-read --source` |
| `spool:` | `spool:./dir` | `neotape-write --target`, `neotape-read --source` / `--target` |
| `tcp:` | `tcp://host:port` | `neotape-archiver --listen`, `neotape-write --source` |
| `unix:` | `unix:///path/socket` | `neotape-archiver --listen`, `neotape-write --source` |

## Output conventions

| Stream | Content |
|--------|---------|
| stdout | Pure payload bytes (for readers) or structured output (for inspect/plan). |
| stderr | Diagnostics, progress, warnings, BLAKE3 hash of output. |
| `/dev/tty` | Interactive prompts for volume change, error resolution. |
