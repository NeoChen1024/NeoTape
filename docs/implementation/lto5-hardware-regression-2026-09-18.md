# IBM LTO-5 regression — 2026-09-18

Status: completed hardware regression; observations, not format guarantees.

## Environment and input

- Device: `/dev/tapeB -> /dev/nst0`, IBM ULTRIUM-HH5, firmware H971.
- Kernel: `7.1.8+deb13-amd64`.
- Existing LTFS Partition 0; no repartitioning or full erase was performed.
- Variable block mode, 1 MiB NeoTape records, FEC `rs_32_4` and signed frames.
- Test-only signify regression keys; production secrets were not used.
- Source: a copied subset of `~/ssd/AIGC-Workflows`, 39,192,687,477 bytes
  (36.501 GiB) in 24,443 regular files, 5,127 directories and four symlinks.
- Source content was hashed independently with SHA-256 during copying.
  The original source contents were not changed.
- Optimized executables: `build/hardware`, RelWithDebInfo.
- `mt -f /dev/tapeB compression 0` before writes; `compression 1` succeeded
  after extraction and source verification, including cleanup of aborted runs.

## Successful three-volume campaign

One archiver and extractor retained state across all three logical volumes.
Each volume was read back and captured before overwriting the same partition.

| Volume | Stop condition | Records | Global sequence | Frame bytes | Write time | Read/capture time |
| --- | --- | ---: | --- | ---: | ---: | ---: |
| 1 | 257 MiB software cap, including padded recovery bundle | 256 | 0–255 | 0.250 GiB | 38.1 s | 26.4 s |
| 2 | Real EOT, no software cap | 35,958 | 255–36,212 | 35.115 GiB | 306.7 s | 1,415.2 s |
| 3 | Clean archive end | 5,931 | 36,213–42,143 | 5.792 GiB | 68.0 s | 166.9 s |

Times include positioning, filemarks and validation; they are not pure
streaming benchmarks. Volume 1 also contains one 256 KiB recovery-bundle
record, outside the frame-byte totals. Observed filemark counts were 3, 4 and 3.

The proxy withheld ACK 255 from the archiver after its successful tape write.
Volume 2 reissued that logical frame with a new volume ordinal and signature.
Readback verified both copies, compared their normalized bytes, and emitted
the payload once. There were 42,145 physical NeoTape records and 42,144 unique
logical records. The producer's received-ACK sequence exactly matched the
unique readback sequence.

At real EOT, the final `write()` returned a complete record; its subsequent
status ioctl returned `ENOSPC`. All 35,958 acknowledged records, including the
last one, were present and valid on readback. This observation does not make a
general durability guarantee for buffered tape writes.

The final `archive_end` and explicit trailing filemark completed successfully.
External bsdtar extraction and comparison verified the complete path set, file
types, sizes, regular-file modes, symlink targets and SHA-256 content digests
against all 29,574 source-manifest entries.

## Fixes and discoveries

Before writing, physical record writes were changed to reject positive short
writes instead of appending the remainder as another tape record. Explicit
tape close failures now propagate before reporting success or a clean volume
change. Tape recovery-bundle capacity accounting includes on-tape padding.

The initial small-volume test exposed connection-local FEC initialization:
a later volume can start in content or repair frames after earlier FEC groups.
The validator now initializes the unseen channel state from that partial
context. A regression checks every possible starting position in several
successive groups.

The EOT observation also motivated a conservative ACK correction: a failed
post-write status/flush operation now leaves the just-written record
unacknowledged, permitting a verified retry. A mock reproduces the old erroneous
success return and verifies the corrected behavior. Volume 2 used the earlier
ACK behavior; the corrected writer was used for Volume 3. No additional full
partition write was performed solely to repeat the EOT case.

A Debug campaign was stopped early after roughly 18.5 MiB/s sustained writes.
The optimized 1 GiB null preflight reached approximately 132 MiB/s, and a
steady hardware-write sample reached approximately 135 MiB/s.

Volume 2 readback exposed repeated failing `MTIOCPOS` requests and 8 MiB read
requests for 1 MiB records. It was completed without restarting the scan.
The reader now uses driver file/block counters for local positioning and
adopts the record size only after verifying a frame, resetting the maximum
probe size at archive end. Volume 3 exercised that corrected reader. A short
syscall sample changed from roughly 17–21 ms tape reads plus failing position
queries to roughly 2 ms tape reads with successful `MTIOCGET`. This per-call
sample is not a claim of an equivalent whole-run speedup.

## Additional verification

- 97/97 development CTest cases passed, including memory-bound extraction.
- Optimized tape I/O and tape-reader mock cases passed.
- Nine SSD-only cases derived from actual Volume 1 readback passed:
  baseline, missing content, missing repair, missing both, missing final repair,
  damaged header, excessive erasures, invalid signature, and conflicting retry.
- The latter three cases were rejected; successful recoveries matched the
  original captured content stream exactly.
- `git diff --check` passed.

No real bad tape block was manufactured. Unreadable-record positioning and
positive short-write/close-error behavior were fault-injected in mocks; the
SSD cases do not certify recovery from a real drive/media read failure.
Only one complete real-EOT write was performed in the successful campaign.

## Artifacts and final state

Reproducible opt-in tooling is documented in
[tests/hardware/README.md](../../tests/hardware/README.md).

Run artifacts are under `output/lto5-20260918/` (ignored by Git, approximately
154 GiB): raw volume captures, source snapshot/manifest, restored data, process
logs, syscall samples, `results.json` and `audit.json`. Earlier attempts are
preserved separately: one CLI-only rejection, the FEC seeding failure, and the
early-stopped Debug throughput run.

The original hardware-run diff is retained separately from the later read-path
and conservative ACK fixes. The final drive state was Partition 0, ONLINE,
EOD, variable block mode. The final `mt compression 1` command returned zero.
