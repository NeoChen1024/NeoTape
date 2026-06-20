#!/bin/sh
set -e

SOCK=/tmp/neotape-raw-store-smoke-$$
SPOOL=/tmp/neotape-raw-store-spool-$$
INPUT=/tmp/neotape-raw-store-input-$$.bin
WRITER_LOG=/tmp/neotape-raw-store-writer-$$.log
SERVER_LOG=/tmp/neotape-raw-store-server-$$.log
BLOCK=4096
PAYLOAD_CAP=$((BLOCK - 512))
INPUT_SIZE=$((PAYLOAD_CAP * 2 + 123))

cleanup() {
	if [ -n "${RAW_STORE_PID:-}" ]; then
		kill "$RAW_STORE_PID" 2>/dev/null || true
	fi
	rm -rf "$SOCK" "$SPOOL" "$INPUT" "$WRITER_LOG" "$SERVER_LOG"
}
trap cleanup EXIT

python3 - "$INPUT" "$INPUT_SIZE" <<'PY'
import sys
path = sys.argv[1]
size = int(sys.argv[2])
with open(path, 'wb') as f:
    f.write(bytes((i % 251 for i in range(size))))
PY

./bin/neotape-raw-store \
	--listen "unix://$SOCK" \
	--volume-block-size "$BLOCK" \
	--archive-name raw-smoke \
	--retention-frame-count 1 \
	--input "$INPUT" \
	2>"$SERVER_LOG" &
RAW_STORE_PID=$!

for _ in $(seq 1 50); do
	if [ -S "$SOCK" ]; then break; fi
	sleep 0.1
done
if [ ! -S "$SOCK" ]; then
	echo "smoke_raw_store: raw-store did not create socket"
	cat "$SERVER_LOG"
	exit 1
fi

./bin/neotape-write --source "unix://$SOCK" --target "spool:$SPOOL" 2>"$WRITER_LOG"
wait "$RAW_STORE_PID"
RAW_STORE_PID=

FILE_COUNT=$(find "$SPOOL" -maxdepth 1 -type f -name '*.nts' | wc -l)
if [ "$FILE_COUNT" -ne 2 ]; then
	echo "smoke_raw_store: expected 2 spool files, got $FILE_COUNT"
	cat "$SERVER_LOG" "$WRITER_LOG"
	exit 1
fi

CONTENT=$(find "$SPOOL" -maxdepth 1 -type f -name '*.slice-000000.nts' | sort | head -n1)
ARCHIVE_END=$(find "$SPOOL" -maxdepth 1 -type f -name '*.archive-end.nts' | sort | head -n1)
if [ -z "$CONTENT" ] || [ -z "$ARCHIVE_END" ]; then
	echo "smoke_raw_store: missing content or archive-end spool file"
	find "$SPOOL" -maxdepth 1 -type f -print | sort
	exit 1
fi

python3 - "$CONTENT" "$ARCHIVE_END" "$BLOCK" "$INPUT_SIZE" "$INPUT" <<'PY'
import struct
import sys
from pathlib import Path

content = Path(sys.argv[1]).read_bytes()
archive_end = Path(sys.argv[2]).read_bytes()
block = int(sys.argv[3])
input_size = int(sys.argv[4])
raw_input = Path(sys.argv[5]).read_bytes()
cap = block - 512
expected_records = (input_size + cap - 1) // cap

if len(content) != expected_records * block:
    raise SystemExit(f"content size {len(content)} != {expected_records * block}")
if len(archive_end) != block:
    raise SystemExit(f"archive_end size {len(archive_end)} != {block}")

END = 1
payloads = []
for idx in range(expected_records):
    rec = content[idx * block:(idx + 1) * block]
    if rec[:8] != b"NeoTape\0":
        raise SystemExit(f"record {idx + 1}: bad magic")
    channel = rec[9]
    if channel != 1:
        raise SystemExit(f"record {idx + 1}: channel {channel} != ch_content")
    global_seq = struct.unpack_from('<Q', rec, 122)[0]
    slice_seq = struct.unpack_from('<Q', rec, 130)[0]
    channel_frame_seq = struct.unpack_from('<Q', rec, 138)[0]
    payload_size = struct.unpack_from('<I', rec, 146)[0]
    flags = struct.unpack_from('<Q', rec, 150)[0]

    expected_payload = cap if idx + 1 < expected_records else input_size - cap * idx
    expected_flags = 0
    if idx + 1 == expected_records:
        expected_flags |= END

    if global_seq != idx:
        raise SystemExit(f"record {idx + 1}: global_seq {global_seq}")
    if slice_seq != 0:
        raise SystemExit(f"record {idx + 1}: slice_seq {slice_seq}")
    if channel_frame_seq != idx:
        raise SystemExit(f"record {idx + 1}: channel_frame_seq {channel_frame_seq}")
    if payload_size != expected_payload:
        raise SystemExit(f"record {idx + 1}: payload_size {payload_size} != {expected_payload}")
    if flags != expected_flags:
        raise SystemExit(f"record {idx + 1}: flags {flags} != {expected_flags}")
    payloads.append(rec[512:512 + payload_size])

if b''.join(payloads) != raw_input:
    raise SystemExit("stored payload bytes differ from raw input")

if archive_end[:8] != b"NeoTape\0" or archive_end[9] != 0xff:
    raise SystemExit("archive_end frame mismatch")
archive_end_global = struct.unpack_from('<Q', archive_end, 122)[0]
archive_end_flags = struct.unpack_from('<Q', archive_end, 150)[0]
if archive_end_global != expected_records:
    raise SystemExit(f"archive_end global_seq {archive_end_global}")
if archive_end_flags != (END | (1 << 63)):
    raise SystemExit(f"archive_end flags {archive_end_flags}")
PY

if ! grep -F 'raw-store served 4 frames' "$SERVER_LOG" >/dev/null; then
	echo "smoke_raw_store: missing served-frame log"
	cat "$SERVER_LOG"
	exit 1
fi

echo "smoke_raw_store: ok"
