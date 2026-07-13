# Appendix: CLI Reference

Status: non-normative implementation reference.

This appendix documents the current NeoTape tools and their CLI usage.

Every long option has a short alias listed by each command's `-h` output.
Byte-size arguments accept case-insensitive binary `K`, `M`, `G`, and `T`
suffixes (for example `4M` or `16G`); an unsuffixed value is bytes.

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
  --target tape:/dev/nst0 --erase \
  --recovery-bundle output/bot.tar

# Write to a spool directory instead of a real tape device:
bin/neotape-write --source tcp://tapehost:9000 \
  --target spool:./out
```

`--listen` is required. Use the standalone `mt-pax` command below when a plain
pax archive file is needed.

`-R, --recovery-bundle <tar>` is available in non-append mode. A tape target
writes the bundle at BOT using separate 256 KiB records by default. Use
`-r, --recovery-bundle-block-size <SIZE>` to override that size. The writer
pads the final bundle record, writes a filemark, and then writes NeoTape frames
at the volume's own block size. A spool target copies the original tar to
`recovery-bundle.tar`; it is deliberately outside the numbered `.nts` stream.

## Raw byte-stream store

`neotape-raw-store` is the raw-stream counterpart to `neotape-archiver` server
mode.  It reads one uninterpreted byte stream from stdin by default, or from
`--input <file|->`, packs it directly into `ch_content` frames, and serves those
NeoTape records to `neotape-write` over TCP or a Unix-domain socket:

```sh
# Store raw bytes from a file:
bin/neotape-raw-store --listen tcp://0.0.0.0:9000 \
  --archive-name disk-image --input image.raw

# Or store raw bytes from a pipeline:
dd if=/dev/nvme0n1 bs=4M | \
  bin/neotape-raw-store --listen unix:///tmp/raw-store.sock
```

The entire input stream is one slice (`slice_seq_num = 0`). Within that slice,
`channel_frame_seq_num` starts at 0 and increments for each `ch_content` frame.
The first content frame is the one with `channel_frame_seq_num = 0`, only the
last content frame carries `END`, and the store emits `tape_eof` before the
final `archive_end` frame.

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

Generates the record-oriented plan metadata stream consumed by
`neotape-archiver --plan`; see [10-plan-metadata.md](10-plan-metadata.md).

## Extractor / Reader (reading pipeline)

The extractor and reader are a long-running server / short-lived client pair for
reading NeoTape archives back. The extractor applies the shared validation rules
from [05-validation.md](05-validation.md) and reassembles the pax content
stream:

```sh
# Start the extractor server (long-running, validates frames):
bin/neotape-extractor --listen tcp://0.0.0.0:9000 -o output.pax

# Read one volume from tape and feed it to the extractor:
bin/neotape-read --source tape:/dev/nst0 --connect tcp://tapehost:9000

# Read from a spool directory:
bin/neotape-read --source spool:./in --connect tcp://tapehost:9000
```

FEC correction is automatic during normal extraction whenever the stream
contains `FEC_PROTECTED` content and matching `ch_fec` groups; no extractor
flag is required. `--salvage` changes consistency and unrepairable-group
fallback policy, not whether decoding runs. See
[fec-restore-behavior.md](fec-restore-behavior.md) for the state flow and error
policy.

## Reader (TCP client)

```sh
# Connect to extractor and read from tape:
bin/neotape-read --source tape:/dev/nst0 --connect tcp://extractor_host:9000

# Connect to extractor and read from spool:
bin/neotape-read --source spool:./in --connect unix:///tmp/extractor.sock
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
bin/neotape-inspect --source spool:./out

# Inspect a tape device:
bin/neotape-inspect --source tape:/dev/nst0
```

The compliance report applies the full conformance rules from
[05-validation.md](05-validation.md), including per-frame structure and
integrity, archive identity consistency, sequence continuity, channel ordering,
`archive_end` rules, and FEC-group consistency when `ch_fec` is present.

## Scan tool

`neotape-scan` reads only the first NeoTape frame from each tapefile in a spool
directory or tape, deduplicates by `archive_uuid` plus `archive_label`, and
prints each new archive identity immediately when first seen. With `-v`, it
also prints every tapefile's first frame and marks whether that frame
introduced a new archive identity:

```sh
# Summarize archive identities found in a spool:
bin/neotape-scan --source spool:./out

# Also list each tapefile's first frame on tape:
bin/neotape-scan --source tape:/dev/nst0 -v
```

## Backend locators

Backend locators use `<kind>:<locator>` syntax.  The split occurs at the first
colon only, so locator paths may contain additional colons.

| Kind | Syntax | Used by |
|------|--------|---------|
| `tape:` | `tape:/dev/nst0` | `neotape-write --target`, `neotape-read --source` |
| `spool:` | `spool:./dir` | `neotape-write --target`, `neotape-read --source` |
| `tcp:` | `tcp://host:port` | `neotape-archiver --listen`, `neotape-raw-store --listen`, `neotape-extractor --listen`, `neotape-write --source`, `neotape-read --connect` |
| `unix:` | `unix:///path/socket` | `neotape-archiver --listen`, `neotape-raw-store --listen`, `neotape-extractor --listen`, `neotape-write --source`, `neotape-read --connect` |

## Output conventions

| Stream | Content |
|--------|---------|
| stdout | Pure payload bytes (for readers) or structured output (for inspect/plan). |
| stderr | Diagnostics, progress, warnings, BLAKE3 hash of output. |
| `/dev/tty` | Interactive prompts for volume change, error resolution. |
