# NeoTape EOT Frame Probe

This directory contains a standalone experimental tool for validating the current
Frame-per-record design against real tape EOT/EOM behavior.

The tool is intentionally not a stable NeoTape archival writer. It is a probe for
these questions:

- Does `write()` near EOT complete the full `volume_block_size` record?
- Does the OS report a short write, `ENOSPC`, or deferred error?
- Can any partial Frame be read back?
- What do `MTIOCGET` fields report at write/read boundary conditions?
- Does readback find only complete, verifiable Frames?

## Build

```sh
c++ -std=c++20 -O2 -Wall -Wextra -pedantic \
  testing/tape_eot_frame_probe.cpp \
  -o tape_eot_frame_probe
```

## Safer file test first

```sh
./tape_eot_frame_probe write \
  --path /tmp/probe.ntframes \
  --block-size 1048576 \
  --max-frames 8 \
  --yes-write \
  --log write-file.jsonl

./tape_eot_frame_probe read \
  --path /tmp/probe.ntframes \
  --block-size 1048576 \
  --max-frames 8 \
  --log read-file.jsonl
```

## Tape test outline

Prepare a deliberately small tape partition first, then run against the non-rewind
device. The tool does not rewind automatically.

```sh
mt -f /dev/nst0 rewind

./tape_eot_frame_probe write \
  --path /dev/nst0 \
  --block-size 1048576 \
  --set-fixed-block \
  --allow-character-device \
  --yes-write \
  --log eot-write.jsonl

mt -f /dev/nst0 rewind

./tape_eot_frame_probe read \
  --path /dev/nst0 \
  --block-size 1048576 \
  --allow-character-device \
  --log eot-read.jsonl
```

Try at least:

```text
64 KiB
1 MiB
4 MiB
```

## Log format

The tool writes JSON Lines. Important events include:

```json
{"event":"write","frame":123,"ret":1048576,"errno":0,"complete":true}
{"event":"write","frame":124,"ret":-1,"errno":28,"errno_text":"No space left on device","complete":false}
{"event":"mt_status","label":"write_error","mt_resid":0,"mt_gstat":0,"mt_erreg":0,"mt_fileno":0,"mt_blkno":0}
{"event":"read","index":123,"ret":1048576,"result":"valid","frame":123}
{"event":"read","index":124,"ret":0,"result":"zero"}
```

## Notes

- The Frame header follows the current field order in `docs/spec/03-frame-header.md`.
- The `frame_payload_blake3` field is filled with a deterministic local probe digest, not real BLAKE3.
- Payload data is generated from a deterministic PRNG keyed by `seed` and `global_frame_seq_num`.
- Read mode verifies header CRC32C, probe digest, and full PRNG payload bytes.
