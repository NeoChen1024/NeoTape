# IBM LTO-5 regression — 2026-10-02

Status: completed hardware regression; observations, not format guarantees.

This run covers the commits from `d130cd7` (ch_fec removal) through `57ef673`.
It supersedes the 2026-09-18 report, whose FEC findings no longer apply.

## Environment and input

- Device: `/dev/tapeB -> /dev/nst0`, IBM ULTRIUM-HH5, firmware H971.
- Kernel: `7.1.8+deb13-amd64`.
- Freshly `mkltfs`-formatted tape; NeoTape overwrote LTFS Partition 0
  (about 35 GiB usable) for every volume. No repartitioning or full erase.
- 1 MiB NeoTape records, signed frames, no FEC, `mt compression 0` during
  writes. Test-only signify regression keys.
- Source: all of `~/ssd/AIGC-Workflows`, read in place:
  76,347,730,874 bytes (71.1 GiB) in 217,360 regular files, 23,952
  directories and 5 symlinks. Contents were hashed independently with
  SHA-256 before archiving; the tree was not modified during the run.
- Optimized executables: `build/hardware`, RelWithDebInfo, with the
  BLAKE3 submodule checked out at `6aab490`.

## Four-volume campaign

One archiver and extractor retained state across all volumes. Each volume
was read back and captured to SSD before the partition was overwritten.

| Volume | Stop condition | Records | Global sequence | Frame bytes | Write | Read/capture |
| --- | --- | ---: | --- | ---: | ---: | ---: |
| 1 | 257 MiB software cap, including recovery bundle | 256 | 0–255 | 0.250 GiB | 21.1 s | 19.9 s |
| 2 | Real EOT | 35,862 | 255–36,116 | 35.02 GiB | 307.6 s | 808.7 s |
| 3 | Real EOT | 36,068 | 36,116–72,183 | 35.22 GiB | 307.7 s | 862.4 s |
| 4 | Clean archive end | 1,052 | 72,183–73,234 | 1.03 GiB | 26.8 s | 39.1 s |

Times include positioning, filemarks and validation. Observed filemark
counts were 3, 5, 5 and 2. No record was unreadable on any volume.
Readback ran at roughly 44 MiB/s against roughly 117 MiB/s for writes; this
drive is sensitive to media quality, so the difference is not treated as a
software finding.

There were 73,238 physical NeoTape records and 73,235 unique logical
records. Three logical frames were written twice and suppressed on readback:

- 255: the proxy withheld its ACK after a successful write.
- 36,116 and 72,183: at each real EOT, the final `write()` returned a
  complete record but the following status ioctl returned `ENOSPC`. The
  writer left that record unacknowledged, and the next volume reissued it.

Every retry verified independently and matched its first copy after
normalizing `volume_seq_num`, signature and `frame_hash`. The producer's
ACK sequence exactly matched the unique readback sequence. This is the
first physical run of the conservative EOT ACK rule; the 2026-09-18
campaign used it only for its final, non-EOT volume.

External bsdtar extraction matched all 241,317 manifest entries: path set,
file types, sizes, regular-file modes, symlink targets and SHA-256 digests.
The recovery bundle bytes and padding at BOT matched the source tar.

## Commit-specific checks

- Variable block mode (`4229f2f`): Volume 1 was written and read with the
  drive left at `mt setblk 512`. Both the writer and the reader restored
  block size 0. An earlier attempt accidentally used a capture tool built
  before this commit: every record at fixed 512 bytes was unreadable.
- Frame checking and replay comparison (`9c0dbff`): exercised by all three
  retries above and by the fault cases below.
- FEC removal (`d130cd7`): five SSD-only cases built from the captured
  Volume 1 restored the baseline exactly and rejected missing content,
  a damaged header, a forged signature and a conflicting replay.
  `--fec` is no longer accepted.
- Planner (`cdc0a57`) and pax writer (`7060977`): over the same source,
  `neotape-plan -j 1`, `-j 8` and the `249865f` build produced
  byte-identical plans, and `mt-pax -j 1`, `-j 8` and `249865f` produced
  byte-identical 72 GiB streams.
- CLI diagnostics (`30a53ce`): all ten executables returned status 2 for
  invalid usage and 1 for runtime failures, with their own name as prefix.
- 85/85 development CTest cases passed.

## Discovery: recovery bundle in inspect and scan

`neotape-inspect` and `neotape-scan` parsed the BOT recovery bundle
tapefile as a frame and failed with "bad magic". The `249865f` build
behaved identically, so this was not a regression, but it contradicted the
volume layout rule that readers ignore a non-NeoTape prefix. Both tools now
share `neotape-read`'s prefix rule. On the same drive, a raw-store archive
written with a recovery bundle then inspected as PASS/verified, scanned with
status 0, and read back byte-identical. The hardware harness now requires
both tools to pass on the bundle volume.

No real bad tape block was manufactured. The SSD cases do not certify
recovery from a physical read failure.

## Artifacts and final state

Reproducible opt-in tooling is documented in
[tests/hardware/README.md](../../tests/hardware/README.md). Build
`neotape_capture_volume` and `neotape_make_replay_cases` explicitly: they are
outside the default target, and a stale capture tool invalidates the run.

Run artifacts are under `output/lto5-20261002/` (ignored by Git, about
216 GiB): captures, restored data, logs, `results.json` and `audit.json`.
The final drive state was Partition 0, variable block mode, compression on.
