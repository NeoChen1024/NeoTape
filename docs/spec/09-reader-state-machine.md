# Reader State Machine

Status: extracted from RFC_Draft.md §§16–17; normative.

## Reader Model

`neotape-cat-volumes` is the minimal restore tool. Its responsibilities:

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

Initialize expected `archive_uuid` (from first volume if unspecified), `expected_volume_seq_num = 1`, `expected_slice_seq_num = 1`.

### READ_VOLUME_HEADER

Read the Volume Header for the current archive volume. The first archive instance normally starts after the Medium Header. From BOT, the reader MUST skip the Medium Header and scan for the requested `archive_uuid`.

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

Handle read errors, UUID mismatch, sequence mismatch, header CRC32C errors, and incomplete slices. See `10-error-handling.md` for the Retry/Inspect/Fail/Salvage model.

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
neotape-cat-volumes --payload-profile=pax --control=auto /dev/nst0 | bsdtar -xpf - --acls --xattrs

# Non-interactive, fail on mismatch:
neotape-cat-volumes --control=none --on-eot=fail --on-mismatch=fail /dev/nst0 > payload.out
```
