# Real Tape NeoTape Validation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Validate the current `bin/neotape` real tape path on `/dev/nst0` using partition 0 of an LTFS-formatted tape, including initialization, backup, restore, integrity comparison, and evidence capture.

**Architecture:** This is a destructive hardware validation plan, not a code implementation plan. It keeps all captured logs and artifacts under `~/pmem/testLTO/neotape-real-tape-test/`, uses the real `tape:/dev/nst0` locator, explicitly selects LTFS partition 0 before every NeoTape operation, creates a NeoTape plan with 8 GiB logical slices, and records every observed behavior that differs from the expected CLI contract.

**Tech Stack:** GNU Make, `bin/neotape`, Linux `mt`/`mt-st`, `/dev/nst0`, `bsdtar`, `tar`, `cmp`, `sha256sum`, shell redirection, real LTO media.

---

## Scope And Safety

This test intentionally overwrites partition 0 of the currently loaded LTFS tape. Do not run this plan if the LTFS contents on partition 0 must be preserved.

Use these fixed paths throughout the plan:

- Tape device: `/dev/nst0`
- NeoTape locator: `tape:/dev/nst0`
- Source data directory: `~/ssd/AIGC-Workflows`
- Restore workspace: `~/pmem/testLTO`
- Run artifacts: `~/pmem/testLTO/neotape-real-tape-test`
- NeoTape plan file: `~/pmem/testLTO/neotape-real-tape-test/AIGC-Workflows.slice-8GiB.plan`
- Restored tree: `~/pmem/testLTO/neotape-real-tape-test/restore`
- Restored pax file: `~/pmem/testLTO/neotape-real-tape-test/restore.pax`
- Logical slice size for `neotape plan`: `8G` / 8 GiB

Expected source size is about 66 GiB. Expected test partition capacity is about 33 GiB. Because the partition is intentionally smaller than the input, this plan treats the first run as an EOT / volume-change behavior test and records whether the current implementation fails, prompts, or writes an incomplete archive.

## Files

- Create: `~/pmem/testLTO/neotape-real-tape-test/README.run.md` for manual run notes
- Create: `~/pmem/testLTO/neotape-real-tape-test/logs/*.log` for command output
- Create: `~/pmem/testLTO/neotape-real-tape-test/status/*.txt` for tape status snapshots
- Create: `~/pmem/testLTO/neotape-real-tape-test/source-files.sha256` for source file hashes
- Create: `~/pmem/testLTO/neotape-real-tape-test/restored-files.sha256` for restored file hashes
- Create: `~/pmem/testLTO/neotape-real-tape-test/source-tree.txt` for source file metadata
- Create: `~/pmem/testLTO/neotape-real-tape-test/restored-tree.txt` for restored file metadata
- Create: `~/pmem/testLTO/neotape-real-tape-test/AIGC-Workflows.slice-8GiB.plan` for the 8 GiB-slice backup plan
- Modify: `/dev/nst0` tape contents on partition 0 via `bin/neotape init` and `bin/neotape backup`

### Task 1: Prepare Build And Artifact Directories

**Files:**
- Create: `~/pmem/testLTO/neotape-real-tape-test/README.run.md`
- Create: `~/pmem/testLTO/neotape-real-tape-test/logs/build.log`

- [ ] **Step 1: Confirm the repository builds**

Run from `/home/neo_chen/vcs/NeoTape`:

```bash
make -j "$(nproc)" > ~/pmem/testLTO/neotape-build.log 2>&1
```

Expected: exit code `0`, and `bin/neotape` exists.

- [ ] **Step 2: Create the run artifact directories**

Run:

```bash
mkdir -p ~/pmem/testLTO/neotape-real-tape-test/logs ~/pmem/testLTO/neotape-real-tape-test/status ~/pmem/testLTO/neotape-real-tape-test/restore
```

Expected: exit code `0`.

- [ ] **Step 3: Preserve the build log inside the run directory**

Run:

```bash
mv ~/pmem/testLTO/neotape-build.log ~/pmem/testLTO/neotape-real-tape-test/logs/build.log
```

Expected: exit code `0`.

- [ ] **Step 4: Record the fixed run parameters**

Create `~/pmem/testLTO/neotape-real-tape-test/README.run.md` with exactly this content:

```markdown
# NeoTape Real Tape Validation Run

- Device: /dev/nst0
- Locator: tape:/dev/nst0
- Source: ~/ssd/AIGC-Workflows
- Restore root: ~/pmem/testLTO/neotape-real-tape-test/restore
- Restored pax: ~/pmem/testLTO/neotape-real-tape-test/restore.pax
- Plan file: ~/pmem/testLTO/neotape-real-tape-test/AIGC-Workflows.slice-8GiB.plan
- NeoTape logical slice size: 8G / 8 GiB
- Tape state before test: LTFS formatted, partition 0 selected for destructive NeoTape test
- Expected source size: about 66 GiB
- Expected partition size: about 33 GiB
- Expected primary behavior under test: real tape init, write, EOT / volume-change handling, restore positioning, restored data integrity
```

Expected: file contains the run parameters above.

- [ ] **Step 5: Commit nothing**

Do not commit artifacts from `~/pmem/testLTO`; they are outside the repository and are hardware-test evidence only.

### Task 2: Verify Tape Device, Partition, And Source Inputs

**Files:**
- Create: `~/pmem/testLTO/neotape-real-tape-test/status/00-initial-status.txt`
- Create: `~/pmem/testLTO/neotape-real-tape-test/status/01-partition0-status.txt`
- Create: `~/pmem/testLTO/neotape-real-tape-test/logs/source-size.log`

- [ ] **Step 1: Confirm the source directory exists**

Run:

```bash
test -d ~/ssd/AIGC-Workflows
```

Expected: exit code `0`.

- [ ] **Step 2: Confirm the non-rewinding tape device exists**

Run:

```bash
test -c /dev/nst0
```

Expected: exit code `0`.

- [ ] **Step 3: Capture initial tape status**

Run:

```bash
mt -f /dev/nst0 status > ~/pmem/testLTO/neotape-real-tape-test/status/00-initial-status.txt 2>&1
```

Expected: exit code `0`. The status file should identify an online SCSI tape device and must not indicate write protection.

- [ ] **Step 4: Select LTFS partition 0 for the destructive test**

Run:

```bash
mt -f /dev/nst0 setpartition 0 > ~/pmem/testLTO/neotape-real-tape-test/logs/setpartition-0.log 2>&1
```

Expected: exit code `0`. If this command fails with an unsupported operation or unknown operation name, stop the run and record the exact output in `logs/setpartition-0.log`; do not continue on the wrong partition.

- [ ] **Step 5: Rewind within partition 0**

Run:

```bash
mt -f /dev/nst0 rewind > ~/pmem/testLTO/neotape-real-tape-test/logs/rewind-partition-0.log 2>&1
```

Expected: exit code `0`.

- [ ] **Step 6: Capture partition 0 status**

Run:

```bash
mt -f /dev/nst0 status > ~/pmem/testLTO/neotape-real-tape-test/status/01-partition0-status.txt 2>&1
```

Expected: exit code `0`. The status file should show BOT or equivalent beginning-of-partition state.

- [ ] **Step 7: Record source size**

Run:

```bash
du -sh ~/ssd/AIGC-Workflows > ~/pmem/testLTO/neotape-real-tape-test/logs/source-size.log 2>&1
```

Expected: exit code `0`; output should be about `66G`.

### Task 3: Create Baseline Source Manifests

**Files:**
- Create: `~/pmem/testLTO/neotape-real-tape-test/source-tree.txt`
- Create: `~/pmem/testLTO/neotape-real-tape-test/source-files.sha256`
- Create: `~/pmem/testLTO/neotape-real-tape-test/logs/source-manifest.log`

- [ ] **Step 1: Capture source tree metadata**

Run:

```bash
(cd ~/ssd && find AIGC-Workflows -xdev -printf '%P\t%y\t%s\t%M\t%u\t%g\n' | LC_ALL=C sort) > ~/pmem/testLTO/neotape-real-tape-test/source-tree.txt 2> ~/pmem/testLTO/neotape-real-tape-test/logs/source-tree.log
```

Expected: exit code `0`; `source-tree.txt` is non-empty.

- [ ] **Step 2: Capture source regular-file hashes**

Run:

```bash
(cd ~/ssd && find AIGC-Workflows -xdev -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum) > ~/pmem/testLTO/neotape-real-tape-test/source-files.sha256 2> ~/pmem/testLTO/neotape-real-tape-test/logs/source-hashes.log
```

Expected: exit code `0`; `source-files.sha256` is non-empty. This can take several minutes because it reads about 66 GiB.

- [ ] **Step 3: Record manifest line counts**

Run:

```bash
wc -l ~/pmem/testLTO/neotape-real-tape-test/source-tree.txt ~/pmem/testLTO/neotape-real-tape-test/source-files.sha256 > ~/pmem/testLTO/neotape-real-tape-test/logs/source-manifest.log
```

Expected: exit code `0`; both line counts are greater than `0`.

### Task 4: Create The 8 GiB Slice Backup Plan

**Files:**
- Create: `~/pmem/testLTO/neotape-real-tape-test/AIGC-Workflows.slice-8GiB.plan`
- Create: `~/pmem/testLTO/neotape-real-tape-test/logs/plan.log`

- [ ] **Step 1: Generate the NeoTape plan with 8 GiB logical slices**

Run from `/home/neo_chen/vcs/NeoTape`:

```bash
bin/neotape plan -C ~/ssd -o ~/pmem/testLTO/neotape-real-tape-test/AIGC-Workflows.slice-8GiB.plan --slice-size 8G AIGC-Workflows > ~/pmem/testLTO/neotape-real-tape-test/logs/plan.log 2>&1
```

Expected: exit code `0`; `AIGC-Workflows.slice-8GiB.plan` exists and is non-empty. This is required because `neotape backup` currently accepts planned slice boundaries through `-p <plan>`; the direct positional-source backup path does not expose `--slice-size`.

- [ ] **Step 2: Record the plan file size**

Run:

```bash
ls -lh ~/pmem/testLTO/neotape-real-tape-test/AIGC-Workflows.slice-8GiB.plan >> ~/pmem/testLTO/neotape-real-tape-test/logs/plan.log 2>&1
```

Expected: exit code `0`; `logs/plan.log` contains the plan file size.

### Task 5: Initialize Partition 0 As A NeoTape Medium

**Files:**
- Create: `~/pmem/testLTO/neotape-real-tape-test/logs/init.log`
- Create: `~/pmem/testLTO/neotape-real-tape-test/status/02-after-init-status.txt`

- [ ] **Step 1: Ensure partition 0 is still selected**

Run:

```bash
mt -f /dev/nst0 setpartition 0 >> ~/pmem/testLTO/neotape-real-tape-test/logs/init.log 2>&1
```

Expected: exit code `0`.

- [ ] **Step 2: Rewind before initialization**

Run:

```bash
mt -f /dev/nst0 rewind >> ~/pmem/testLTO/neotape-real-tape-test/logs/init.log 2>&1
```

Expected: exit code `0`.

- [ ] **Step 3: Initialize the tape as NeoTape**

Run from `/home/neo_chen/vcs/NeoTape`:

```bash
bin/neotape init tape:/dev/nst0 --label LTO-P0-NEOTAPE-TEST --force >> ~/pmem/testLTO/neotape-real-tape-test/logs/init.log 2>&1
```

Expected: exit code `0`; `logs/init.log` contains `medium initialized: uuid=`.

- [ ] **Step 4: Capture post-init status**

Run:

```bash
mt -f /dev/nst0 status > ~/pmem/testLTO/neotape-real-tape-test/status/02-after-init-status.txt 2>&1
```

Expected: exit code `0`.

- [ ] **Step 5: Verify Medium Header read positioning**

Run:

```bash
mt -f /dev/nst0 rewind >> ~/pmem/testLTO/neotape-real-tape-test/logs/init.log 2>&1
```

Expected: exit code `0`. This positions the device at the beginning of partition 0 for the next writer test.

### Task 6: Run Full Backup With 8 GiB Slices And Capture EOT Behavior

**Files:**
- Create: `~/pmem/testLTO/neotape-real-tape-test/logs/backup.log`
- Create: `~/pmem/testLTO/neotape-real-tape-test/status/03-after-backup-status.txt`

- [ ] **Step 1: Select partition 0 and rewind**

Run:

```bash
mt -f /dev/nst0 setpartition 0 > ~/pmem/testLTO/neotape-real-tape-test/logs/backup.log 2>&1
mt -f /dev/nst0 rewind >> ~/pmem/testLTO/neotape-real-tape-test/logs/backup.log 2>&1
```

Expected: both commands exit `0`.

- [ ] **Step 2: Start the real tape backup**

Run from `/home/neo_chen/vcs/NeoTape` in an interactive terminal attached to `/dev/tty`:

```bash
bin/neotape backup --target tape:/dev/nst0 -p ~/pmem/testLTO/neotape-real-tape-test/AIGC-Workflows.slice-8GiB.plan --name AIGC-Workflows-real-tape-test >> ~/pmem/testLTO/neotape-real-tape-test/logs/backup.log 2>&1
```

Expected for current 33 GiB partition versus 66 GiB source: one of these outcomes must be recorded exactly in `logs/backup.log`:

- Preferred correct behavior: the command detects EOT / ENOSPC, uses the shared `/dev/tty` volume-change prompt with continue, change device, shell, and abort options, and does not silently report success for an incomplete archive.
- Acceptable `--control=none` behavior: the command exits non-zero with a volume-change-required diagnostic and does not prompt.
- Bug behavior: the command exits `0` even though only partition 0 was available and the full 66 GiB source could not fit.

- [ ] **Step 3: If prompted for a next volume, abort instead of inserting another tape**

At the prompt, type `a` to abort. If the prompt is only `Insert next volume and press Enter`, press `Ctrl-C` and record that as a bug because the shared volume-change prompt was not used.

Expected: the run ends without inserting another tape.

- [ ] **Step 4: Capture post-backup status**

Run:

```bash
mt -f /dev/nst0 status > ~/pmem/testLTO/neotape-real-tape-test/status/03-after-backup-status.txt 2>&1
```

Expected: exit code `0`; status may show EOD/EOT depending on where the backup stopped.

- [ ] **Step 5: Classify the backup result**

Append exactly one of these lines to `~/pmem/testLTO/neotape-real-tape-test/README.run.md`:

```markdown
- Backup result classification: EOT prompt observed and run aborted intentionally.
```

or

```markdown
- Backup result classification: EOT/ENOSPC failure observed; command exited non-zero.
```

or

```markdown
- Backup result classification: BUG - command exited zero even though source exceeded partition capacity.
```

Expected: the README records the observed backup behavior.

### Task 7: Restore From The Written Tape And Validate Positioning Behavior

**Files:**
- Create: `~/pmem/testLTO/neotape-real-tape-test/logs/restore-from-bot.log`
- Create: `~/pmem/testLTO/neotape-real-tape-test/logs/restore-after-medium-header.log`
- Create: `~/pmem/testLTO/neotape-real-tape-test/restore.pax`

- [ ] **Step 1: Try restore from BOT to document current behavior**

Run:

```bash
mt -f /dev/nst0 setpartition 0 > ~/pmem/testLTO/neotape-real-tape-test/logs/restore-from-bot.log 2>&1
mt -f /dev/nst0 rewind >> ~/pmem/testLTO/neotape-real-tape-test/logs/restore-from-bot.log 2>&1
```

Expected: both commands exit `0`.

- [ ] **Step 2: Run restore from BOT**

Run from `/home/neo_chen/vcs/NeoTape`:

```bash
bin/neotape restore --source tape:/dev/nst0 --output ~/pmem/testLTO/neotape-real-tape-test/restore-from-bot.pax >> ~/pmem/testLTO/neotape-real-tape-test/logs/restore-from-bot.log 2>&1
```

Expected for the current reader implementation: non-zero exit with a diagnostic like `expected volume header, got medium`, because `restore` currently opens the tape at the current position and does not skip the Medium Header automatically. If this command succeeds, record that as an improvement or unexpected behavior in `README.run.md`.

- [ ] **Step 3: Reposition after the Medium Header filemark**

Run:

```bash
mt -f /dev/nst0 setpartition 0 > ~/pmem/testLTO/neotape-real-tape-test/logs/restore-after-medium-header.log 2>&1
mt -f /dev/nst0 rewind >> ~/pmem/testLTO/neotape-real-tape-test/logs/restore-after-medium-header.log 2>&1
mt -f /dev/nst0 fsf 1 >> ~/pmem/testLTO/neotape-real-tape-test/logs/restore-after-medium-header.log 2>&1
```

Expected: all commands exit `0`; the device is positioned at the first Volume Header.

- [ ] **Step 4: Restore to a pax file from the first Volume Header**

Run from `/home/neo_chen/vcs/NeoTape`:

```bash
bin/neotape restore --source tape:/dev/nst0 --output ~/pmem/testLTO/neotape-real-tape-test/restore.pax >> ~/pmem/testLTO/neotape-real-tape-test/logs/restore-after-medium-header.log 2>&1
```

Expected depends on Task 6:

- If Task 6 completed a full single-volume archive, exit code should be `0` and `restore.pax` should be non-empty.
- If Task 6 stopped at EOT before Archive End Header, `--control=auto` should prompt for the next volume at a clean volume boundary. If aborted, restore exits non-zero. With `--control=none`, restore exits non-zero with `volume change required` or an integrity diagnostic.
- A zero exit after an aborted or incomplete backup is a bug and must be recorded in `README.run.md`.

### Optional Negative Manual Cases

- Insert a tape containing a different archive UUID when restore asks for the next volume. Expected: restore reports expected/actual UUID and prompts again; it must not emit payload from the wrong volume.
- Insert a tape containing volume 2 for another archive instance. Expected: restore rejects it even if `volume_seq_num == 2`, because archive UUID differs.
- Use `--control=none` for the same cases. Expected: restore exits non-zero without prompting.

### Task 8: Extract And Compare Restored Data When A Complete Archive Exists

**Files:**
- Create: `~/pmem/testLTO/neotape-real-tape-test/logs/extract.log`
- Create: `~/pmem/testLTO/neotape-real-tape-test/restored-tree.txt`
- Create: `~/pmem/testLTO/neotape-real-tape-test/restored-files.sha256`

- [ ] **Step 1: Check whether restore produced a complete pax file**

Run:

```bash
test -s ~/pmem/testLTO/neotape-real-tape-test/restore.pax
```

Expected: exit code `0` only if Task 7 Step 4 succeeded. If this exits non-zero because the backup was intentionally incomplete, skip the remaining steps in this task and record `Restore comparison skipped because no complete archive was produced` in `README.run.md`.

- [ ] **Step 2: Extract the restored pax file**

Run:

```bash
(cd ~/pmem/testLTO/neotape-real-tape-test/restore && bsdtar -xpf ../restore.pax) > ~/pmem/testLTO/neotape-real-tape-test/logs/extract.log 2>&1
```

Expected: exit code `0`.

- [ ] **Step 3: Capture restored tree metadata**

Run:

```bash
(cd ~/pmem/testLTO/neotape-real-tape-test/restore && find AIGC-Workflows -xdev -printf '%P\t%y\t%s\t%M\t%u\t%g\n' | LC_ALL=C sort) > ~/pmem/testLTO/neotape-real-tape-test/restored-tree.txt 2> ~/pmem/testLTO/neotape-real-tape-test/logs/restored-tree.log
```

Expected: exit code `0`; `restored-tree.txt` is non-empty.

- [ ] **Step 4: Capture restored regular-file hashes**

Run:

```bash
(cd ~/pmem/testLTO/neotape-real-tape-test/restore && find AIGC-Workflows -xdev -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum) > ~/pmem/testLTO/neotape-real-tape-test/restored-files.sha256 2> ~/pmem/testLTO/neotape-real-tape-test/logs/restored-hashes.log
```

Expected: exit code `0`; `restored-files.sha256` is non-empty.

- [ ] **Step 5: Compare source and restored metadata**

Run:

```bash
cmp ~/pmem/testLTO/neotape-real-tape-test/source-tree.txt ~/pmem/testLTO/neotape-real-tape-test/restored-tree.txt > ~/pmem/testLTO/neotape-real-tape-test/logs/tree-compare.log 2>&1
```

Expected: exit code `0`. A non-zero exit means metadata differs and the diff must be inspected before claiming restore correctness.

- [ ] **Step 6: Compare source and restored hashes**

Run:

```bash
cmp ~/pmem/testLTO/neotape-real-tape-test/source-files.sha256 ~/pmem/testLTO/neotape-real-tape-test/restored-files.sha256 > ~/pmem/testLTO/neotape-real-tape-test/logs/hash-compare.log 2>&1
```

Expected: exit code `0`. A non-zero exit means restored file contents differ and the run fails.

### Task 9: Capture Final Evidence And Known Issues

**Files:**
- Create: `~/pmem/testLTO/neotape-real-tape-test/status/04-final-status.txt`
- Modify: `~/pmem/testLTO/neotape-real-tape-test/README.run.md`

- [ ] **Step 1: Capture final tape status**

Run:

```bash
mt -f /dev/nst0 status > ~/pmem/testLTO/neotape-real-tape-test/status/04-final-status.txt 2>&1
```

Expected: exit code `0`.

- [ ] **Step 2: Append observed outcomes**

Append this template to `~/pmem/testLTO/neotape-real-tape-test/README.run.md` and replace each bracketed value with the observed result before ending the run:

```markdown

## Observed Outcomes

- `neotape init tape:/dev/nst0 --force`: [exit code and summary]
- `neotape backup --target tape:/dev/nst0`: [exit code and EOT behavior]
- Restore from BOT: [exit code and diagnostic]
- Restore after `mt fsf 1`: [exit code and diagnostic]
- Extraction: [exit code or skipped]
- Metadata comparison: [pass, fail, or skipped]
- SHA256 comparison: [pass, fail, or skipped]

## Issues To File

- [issue 1 or `None`]
- [issue 2 or `None`]
```

Expected: no bracketed values remain in the final `README.run.md`.

- [ ] **Step 3: Produce a compact artifact listing**

Run:

```bash
(cd ~/pmem/testLTO/neotape-real-tape-test && find . -maxdepth 3 -type f | LC_ALL=C sort) > ~/pmem/testLTO/neotape-real-tape-test/artifacts.txt
```

Expected: exit code `0`; `artifacts.txt` lists logs, status snapshots, manifests, and restored outputs.

## Self-Review

Spec coverage:

- Uses real tape device `/dev/nst0` through `tape:/dev/nst0`.
- Explicitly selects LTFS partition 0 before destructive NeoTape operations.
- Runs `neotape init` before using the non-NeoTape medium.
- Creates a `neotape plan` with `--slice-size 8G` and runs `neotape backup` with `-p` so the test uses 8 GiB logical slices.
- Uses `~/ssd/AIGC-Workflows` as the backup input.
- Uses `~/pmem/testLTO` for restore artifacts and extraction.
- Treats the 66 GiB source versus 33 GiB partition mismatch as an intentional EOT behavior test.
- Covers wrong archive and same-sequence wrong archive instance negative manual restore cases.
- Documents restore positioning from BOT and after skipping the Medium Header.
- Includes data integrity checks when a complete archive is available.

Placeholder scan:

- The only bracketed values appear in the manual observed-outcomes template and must be replaced during the hardware run.
- No implementation step says TBD, TODO, or “write tests later”.

Type and command consistency:

- Commands consistently use `bin/neotape`, `tape:/dev/nst0`, `~/ssd/AIGC-Workflows`, and `~/pmem/testLTO/neotape-real-tape-test`.
- Restore checks account for the current implementation where `neotape restore` starts at the current tape position and expects a Volume Header.
