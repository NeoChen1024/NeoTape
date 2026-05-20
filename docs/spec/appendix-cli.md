# Appendix: CLI Reference

Status: extracted from RFC_Draft.md Appendix C; non-normative.

This appendix collects example CLI invocations for NeoTape tools. These are illustrative; exact flag names and defaults may change before v0.1 stabilization.

## neotape-cat-volumes (Reader)

```sh
# Interactive restore with NeoTape/PAX profile:
neotape-cat-volumes --payload-profile=pax --control=auto /dev/nst0 |
  bsdtar -xpf - --acls --xattrs

# Non-interactive, fail on any mismatch:
neotape-cat-volumes --control=none --on-eot=fail --on-mismatch=fail \
  /dev/nst0 > payload.out

# Inspect mode — verify header structure without emitting payload:
neotape-cat-volumes --inspect /dev/nst0

# Read from spool directory:
neotape-cat-volumes --target=spool /path/to/archive.spool > payload.out

# Removable media — list of mounted directories:
neotape-cat-volumes /mnt/disc1 /mnt/disc2 /mnt/disc3 | bsdtar -xpf -
```

## neotape-write (Writer)

```sh
# Write to spool directory:
neotape-write --target=spool --payload-profile=pax \
  --virtual-tape-size=64G ./source

# Write directly to tape device:
neotape-write --target=tape /dev/nst0 --payload-profile=pax ./source

# Initialize medium then write:
neotape-init /dev/nst0 --label MYTAPE001
neotape-write --target=tape /dev/nst0 --payload-profile=pax ./source

# Multi-threaded pax writer:
bin/mt-pax -f output.pax --io-thread 4 -P 50 ./source

# Single-threaded pax writer:
bin/pax -f output.pax -v ./source
```

## neotape-init (Medium Initialization)

```sh
# Initialize blank tape with label:
neotape-init /dev/nst0 --label MYTAPE001

# Force re-initialize (overwrite existing data):
neotape-init /dev/nst0 --label MYTAPE001 --force

# Initialize with custom block size:
neotape-init /dev/nst0 --block-size=4M --label MYTAPE001
```

## neotape-plan (Slice Planner)

```sh
# Generate plan metadata for a source tree:
neotape-plan ./source > plan.meta

# Generate plan with custom slice budget:
neotape-plan --metadata-buffer-size=256M ./source > plan.meta
```

## neotape-inspect (Diagnostics)

```sh
# Inspect archive volumes and headers:
neotape-inspect /dev/nst0

# Inspect spool archive:
neotape-inspect /path/to/archive.spool
```

## Output Conventions

All NeoTape tools follow these conventions:

| Stream | Content |
|--------|---------|
| stdout | Pure payload bytes (for readers) or structured output (for inspect/plan). |
| stderr | Diagnostics, progress, warnings, BLAKE3 hash of output. |
| `/dev/tty` | Interactive prompts for volume change, error resolution. |
