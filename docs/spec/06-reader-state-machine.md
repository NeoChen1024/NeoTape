# Reader State Machine

Status: normative.

## Reader Model

`neotape restore` is the minimal restore tool. Its responsibilities:

- Read and parse NeoTape frame records.
- By policy, scan forward through tape files to find the next valid frame (useful on multi-archive media).
- Validate `archive_uuid` and frame/slice/channel sequence continuity.
- Read slice tape files by filemark boundaries.
- Read frames by `frame_payload_size` (MUST NOT inspect payload bytes).
- Concatenate `ch_content` frame payloads for each logical slice.
- Verify per-frame integrity via `frame_hash`.
- Read Archive End frame; exit 0 on valid clean end.
- Keep stdout as pure `ch_content` payload bytes. All prompts and diagnostics go to stderr or `/dev/tty`.

## State Machine

The reader uses a single frame-processing loop. There are no separate states for Volume Header or Archive End Header — every record is a frame dispatched by `channel_type`.

### START

Initialize expected `archive_uuid` (from first frame if unspecified), `expected_global_frame_seq_num = 1`, `expected_slice_seq_num = 1`, and `expected_channel_seq_num = 1`.

From BOT or the start of a spool directory, the reader locates the first NeoTape record. Any leading tape file whose first bytes are not the NeoTape magic (for example an optional recovery bundle) MUST be skipped. The first NeoTape record of an archive stream is the first frame of the first logical slice.

### READ_FRAME

1. Read one backend record/tape block.
2. Decode `volume_block_size_kib` and validate that the backend record length is the decoded record size when the backend exposes that length.
3. Parse the fixed 512-byte header.
4. Validate magic, version, and `frame_hash`.
5. Dispatch by `channel_type`:
   - `ch_content` — stream payload, track START/END, validate `frame_seq_num_within_channel`.
   - `ch_metadata` — stream advisory payload, track START/END. Hash verification failures are warnings only.
   - `archive_end` — verify `CLEAN_END` and finish.

### CHANNEL_GROUP_TRACKING

The reader tracks the current channel group state:

- When a frame with `START` flag arrives, begin a new channel group for that `channel_type`.
- Validate `global_frame_seq_num` continuity across all frames.
- Validate `logical_slice_seq_num` continuity.
- Validate `frame_seq_num_within_channel` continuity within the current channel group.
- When a frame with `END` flag arrives, the current channel group is complete.
- When the reader encounters a filemark, it expects the next frame to either continue the current slice (same `logical_slice_seq_num`, new channel group with `START`) or begin a new slice (incremented `logical_slice_seq_num`, new channel group with `START`), or be the `archive_end` frame.

### ARCHIVE_END

The `archive_end` frame must appear after the final filemark. Validate:

- `channel_type = archive_end`.
- `START = 1`, `END = 1`, `CLEAN_END = 1`.
- `global_frame_seq_num` is one greater than the last data/metadata frame.

On success, exit 0.

### EOT_BEFORE_END

If physical EOT is reached before the Archive End frame, prompt for the next volume. The next volume begins with a new logical slice frame; validate `volume_seq_num` change (advisory) and sequence continuity.

### MULTI_ARCHIVE_HANDLER

On a mismatch (UUID change, unexpected sequence numbers), the reader may scan forward through tape files looking for the next valid frame or an `archive_end` frame with `CLEAN_END = 1`. This enables multi-archive media where the inserted medium may be positioned past the target archive.

## Reader State Diagram

```
START ──> READ_FRAME
              │
              ├── ch_content / ch_metadata ──> stream payload ──> READ_FRAME
              │       │
              │       └── (END flag) ──> channel group complete
              │
              ├── (filemark) ──> validate sequence continuity ──> READ_FRAME
              │
              ├── archive_end ──> verify CLEAN_END ──> exit 0
              │
              └── (error / mismatch) ──> ERROR_HANDLER
                                             │
                                             ├── scan forward ──> READ_FRAME
                                             └── prompt / fail
```

## Recommended CLI Usage

```sh
# Interactive restore:
neotape restore --source tape:/dev/nst0 --control=auto | bsdtar -xpf - --acls --xattrs

# Non-interactive, fail on mismatch:
neotape restore --source tape:/dev/nst0 --control=none > payload.out
```
