# Appendix: CLI Reference

Status: non-normative implementation reference.

This appendix collects example CLI invocations for NeoTape tools. These are illustrative; exact flag names and defaults may change before v0.1 stabilization.

## Main CLI

```sh
neotape init tape:/dev/nst0 --label MYTAPE001
neotape init spool:./archive.spool --label TEST --virtual-tape-size 100G

neotape plan -C /data -o home.plan photos docs
neotape backup --target tape:/dev/nst0 -p home.plan --name home
neotape backup --target spool:./archive.spool ./source

neotape restore --source tape:/dev/nst0 --output - | bsdtar -xpf -

neotape write --target spool:./archive.spool --input payload.bin --name raw1
neotape read --source spool:./archive.spool --output payload.out

neotape list --source spool:./archive.spool
neotape list --source spool:./archive.spool --json
```

Default archive `--volume-block-size` is 4 MiB for `backup` and `write` (encoded as `volume_block_size_kib = 4096`).
Commands that emit payload data keep stdout as payload bytes only; diagnostics
and prompts use stderr or `/dev/tty`.

## Backend Locators

Backend locators use `<kind>:<locator>` syntax. The split occurs at the first
colon only, so locator paths may contain additional colons.

`spool:<dir>` selects the file-backed spool backend. A spool root is a directory
containing single-root `.nts` tape files such as
`neotape-000000.slice-000001.nts`, and
`neotape-000001.archive-end.nts`. Use this backend for hardware-free CLI tests
and development. An optional `recovery-bundle.tar` may be present at the spool
root for human recovery; it is not part of the NeoTape archive stream.

`tape:<device>` selects a real tape device, for example `tape:/dev/nst0`.
Directory locators are not accepted as a `tape:` fallback; use `spool:<dir>` for
file-backed archives.

## Standalone Tools

```sh
# Multi-threaded pax writer:
bin/mt-pax -f output.pax --io-thread 4 -P 50 ./source

# Inspect archive volumes and headers:
bin/neotape-inspect /path/to/archive.spool
```

`bin/mt-pax` is the standalone PAX writer. `bin/neotape-inspect`
remains the low-level inspection and forensic tool.

## Output Conventions

All NeoTape tools follow these conventions:

| Stream | Content |
|--------|---------|
| stdout | Pure payload bytes (for readers) or structured output (for inspect/plan). |
| stderr | Diagnostics, progress, warnings, BLAKE3 hash of output. |
| `/dev/tty` | Interactive prompts for volume change, error resolution. |
