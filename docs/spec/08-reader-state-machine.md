# Reader State Machine

Status: normative.

## Reader Model

`neotape restore` is the minimal restore tool. Its responsibilities:

- Read and validate Volume Headers.
- By policy, scan forward through tape files to find the next candidate Volume Header (useful on multi-archive media).
- Validate `archive_uuid` and `volume_seq_num`.
- Read slice tape files by filemark boundaries.
- Read Frames by `frame_payload_size` (MUST NOT inspect payload bytes).
- Concatenate `SLICE_CONTENT` Frame payloads for each logical slice.
- Verify slice-level BLAKE3 from the END Frame Header.
- For NeoTape/PAX, emit pax bytes to stdout per profile policy (no EOA suppression or detection needed).
- Read Archive End Header; exit 0 on valid clean end.
- Keep stdout as pure payload bytes. All prompts and diagnostics go to stderr or `/dev/tty`.

## State Machine

### START

Initialize expected `archive_uuid` (from first volume if unspecified),
`expected_volume_seq_num = 1`, `expected_slice_seq_num = 1`.

From BOT or the start of a spool directory, the reader locates the first
NeoTape record. Any leading tape file whose first bytes are not the NeoTape
magic (for example an optional recovery bundle) MUST be skipped. The first
NeoTape record of an archive stream MUST be a Volume Header.

### READ_VOLUME_HEADER

Read the Volume Header for the current archive volume. The first archive
instance begins at the first NeoTape record after any non-NeoTape prefix.

If the Volume Header checksum, type, UUID, or sequence number is invalid, enter MISMATCH_HANDLER. Policy options: fail, prompt, or scan forward by filemarks to the next candidate Volume Header. Scan-forward is essential for multi-archive media where the inserted medium may be positioned past the target archive.

### READ_NEXT_TAPE_FILE

Read the first block of the next tape file and determine `header_type`. For a slice tape file, the first NeoTape record is normally a Frame Header. Subsequent Frame Headers inside the same slice are located by `frame_payload_size`, not by filemark.

### READ_FRAME_HEADER

Validate that the Frame belongs to the current archive and that slice/Frame sequence numbers match expectations. Validate `header_crc32c`.

### STREAM_FRAME_PAYLOAD

Read exactly `frame_payload_size` payload bytes. MUST NOT parse payload bytes to determine Frame or slice boundaries. Verify `frame_payload_blake3`.

### SLICE_CONTENT_COMPLETE

The END Frame for the `SLICE_CONTENT` group has been reached. Read `slice_content_size` and `slice_content_blake3` from the Frame Header. Verify `slice_content_blake3` over the concatenated `SLICE_CONTENT` Frame payloads before treating the slice as NeoTape-complete.

If `SLICE_METADATA` Frames exist, continue reading them after BLAKE3 verification. `SLICE_METADATA` verification failures are warnings only and do not affect slice integrity.

### READ_END_HEADER

Read and validate the Archive End Header. Verify `CLEAN_END` flag. Determine stdout finalization policy by payload profile. Exit 0.

### EOT_BEFORE_END

If physical EOT is reached before the Archive End Header, prompt for the next volume. Return to READ_VOLUME_HEADER.

### ERROR_HANDLER

Handle read errors, UUID mismatch, sequence mismatch, header CRC32C errors, and incomplete slices. See `09-error-handling.md` for the Retry/Inspect/Fail/Salvage model.

## Reader State Diagram

```
START ──> READ_VOLUME_HEADER ──> READ_NEXT_TAPE_FILE ──> READ_FRAME_HEADER
              │                                                   │
              │ (invalid header / mismatch)                       │
              └──> ERROR_HANDLER <────────────────────────────────┘
                       │
                       │ (scan forward)
                       └──> READ_VOLUME_HEADER (next candidate)

READ_FRAME_HEADER (valid)
    │
    ├──> STREAM_FRAME_PAYLOAD (for SLICE_CONTENT or SLICE_METADATA)
    │        │
    │        └──> READ_FRAME_HEADER (next frame in slice)
    │
    └──> SLICE_CONTENT_COMPLETE (END flag for SLICE_CONTENT)
              │
              ├──> READ_FRAME_HEADER (SLICE_METADATA frames, if any)
              │
              └──> READ_NEXT_TAPE_FILE (next slice or end header)

READ_END_HEADER ──> exit 0

EOT_BEFORE_END ──> prompt/retry ──> READ_VOLUME_HEADER
```

## Recommended CLI Usage

```sh
# Interactive restore with NeoTape/PAX profile:
neotape restore --source tape:/dev/nst0 --control=auto | bsdtar -xpf - --acls --xattrs

# Non-interactive, fail on mismatch:
neotape restore --source tape:/dev/nst0 --control=none > payload.out
```
