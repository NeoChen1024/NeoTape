# Opt-in LTO regression

These tools are excluded from the default build and CTest. `run_lto5.py`
**overwrites Partition 0 of the explicitly named test tape**. Run it only
after the operator has authorized destroying that partition's contents.
It does not reformat the tape or change partition sizes.

The scripts require Python 3.11 or newer, mt and bsdtar.

Use an optimized build for streaming. A Debug build can starve the drive:

```sh
cmake -S . -B build/hardware -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON
cmake --build build/hardware -j 4 --target \
  neotape_plan neotape_archiver neotape_write neotape_read neotape_extractor \
  neotape_capture_volume neotape_make_replay_cases test_tape_io test_tape_reader
```

`prepare_source.py SOURCE NEW_RUN_DIRECTORY PLANNER` copies a 36.5 GiB subset
without modifying the source. It records independent SHA-256 digests, preserves
file modes and symlink targets, and creates a six-slice plan (depending on the
source sizes). The run directory needs space for the snapshot, captured records,
restored pax stream, and extracted files: allow approximately 160 GiB.

`run_lto5.py --device DEVICE --root RUN_DIRECTORY --repo REPOSITORY
--bin-dir OPTIMIZED_BIN_DIRECTORY` performs:

1. `mt compression 0`, followed by a 257 MiB software-capacity volume including
   a recovery bundle; the proxy withholds ACK 255.
2. One readback/capture of that volume into a persistent extractor.
3. Overwrite of the same partition with the archive continuation until real EOT.
4. Readback/capture before overwriting with the final archive tail.
5. Final readback, external bsdtar extraction, and comparison with the source
   manifest.
6. `mt compression 1` in `finally`, including failure and handled termination.

The controller checks Partition 0 and ONLINE before starting. Do not run other
tape commands concurrently. SIGKILL or machine failure bypasses Python cleanup;
in that case the operator must restore compression explicitly after stopping
all tape processes.

`capture_volume` shares the production `RecordReader`. It preserves physical
record/filemark observations and forwards each frame to the real extractor.
Do not run a second full tape scan just to produce an inspection report.

`make_replay_cases CAPTURED_SLICE_ZERO NEW_CASE_DIRECTORY TEST_SECRET_KEY`
builds nine SSD-only fault cases from a complete first slice. It adds a signed
test end marker without altering the captured originals. `replay_cases.py
REPOSITORY CASE_DIRECTORY` verifies expected recovery or rejection using the
production reader/extractor. The keys under `3rdparty/signify/regress` are
public test fixtures, not keys for real backups.

Finally run `audit_run.py RUN_DIRECTORY` to compare producer ACKs, readback
record counts, normalized retry equality, and the source-comparison result.
Keep controller logs and raw captures when any check fails.
