#!/bin/sh
set -e

SOCK="$PWD/build/neotape-scan-smoke-$$.sock"
SPOOL=/tmp/neotape-scan-spool-$$
INPUT=/tmp/neotape-scan-input-$$.bin
SERVER_LOG=/tmp/neotape-scan-server-$$.log
WRITER_LOG=/tmp/neotape-scan-writer-$$.log
SCAN_LOG=/tmp/neotape-scan-result-$$.log
SCAN_VERBOSE_LOG=/tmp/neotape-scan-verbose-$$.log
BLOCK=4096
PAYLOAD_CAP=$((BLOCK - 512))
INPUT_SIZE=$((PAYLOAD_CAP * 2 + 123))

cleanup() {
	if [ -n "${RAW_STORE_PID:-}" ]; then
		kill "$RAW_STORE_PID" 2>/dev/null || true
	fi
	rm -rf "$SOCK" "$SPOOL" "$INPUT" "$SERVER_LOG" "$WRITER_LOG" \
		"$SCAN_LOG" "$SCAN_VERBOSE_LOG"
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
	--archive-name scan-smoke \
	--retention-frame-count 1 \
	--input "$INPUT" \
	2>"$SERVER_LOG" &
RAW_STORE_PID=$!

for _ in $(seq 1 50); do
	if [ -S "$SOCK" ]; then break; fi
	sleep 0.1
done
if [ ! -S "$SOCK" ]; then
	echo "smoke_scan: raw-store did not create socket"
	cat "$SERVER_LOG"
	exit 1
fi

./bin/neotape-write --source "unix://$SOCK" --target "spool:$SPOOL" 2>"$WRITER_LOG"
wait "$RAW_STORE_PID"
RAW_STORE_PID=

./bin/neotape-scan --source "spool:$SPOOL" >"$SCAN_LOG" 2>&1
./bin/neotape-scan --source "spool:$SPOOL" -v >"$SCAN_VERBOSE_LOG" 2>&1

if ! grep -F 'Archive first seen at tapefile #0: archive_uuid=' "$SCAN_LOG" >/dev/null; then
	echo "smoke_scan: missing streaming archive line"
	cat "$SCAN_LOG"
	exit 1
fi

if ! grep -F 'Unique archives found: 1' "$SCAN_LOG" >/dev/null; then
	echo "smoke_scan: unexpected archive count"
	cat "$SCAN_LOG"
	exit 1
fi

if ! grep -F 'archive_label="scan-smoke"' "$SCAN_LOG" >/dev/null; then
	echo "smoke_scan: missing/incorrect archive label"
	cat "$SCAN_LOG"
	exit 1
fi

if ! grep -F 'Tapefiles scanned: 2' "$SCAN_LOG" >/dev/null; then
	echo "smoke_scan: missing/incorrect tapefile count"
	cat "$SCAN_LOG"
	exit 1
fi

UUID=$(sed -n 's/^Archive first seen at tapefile #0: archive_uuid=\([^ ]*\) archive_label="scan-smoke"$/\1/p' "$SCAN_LOG")
if [ -z "$UUID" ]; then
	echo "smoke_scan: missing archive uuid"
	cat "$SCAN_LOG"
	exit 1
fi

if ! grep -F 'Tapefile #0: channel=CH_CONTENT global_frame_seq_num=0 slice_seq_num=0 channel_frame_seq_num=0' "$SCAN_VERBOSE_LOG" >/dev/null; then
	echo "smoke_scan: missing first content-frame line"
	cat "$SCAN_VERBOSE_LOG"
	exit 1
fi

if ! grep -F 'Tapefile #0: channel=CH_CONTENT global_frame_seq_num=0 slice_seq_num=0 channel_frame_seq_num=0 archive_uuid='"$UUID"' archive_label="scan-smoke" new_archive=yes' "$SCAN_VERBOSE_LOG" >/dev/null; then
	echo "smoke_scan: first verbose line did not mark new archive"
	cat "$SCAN_VERBOSE_LOG"
	exit 1
fi

if ! grep -F 'Tapefile #1: channel=ARCHIVE_END global_frame_seq_num=3' "$SCAN_VERBOSE_LOG" >/dev/null; then
	echo "smoke_scan: missing archive-end line"
	cat "$SCAN_VERBOSE_LOG"
	exit 1
fi

if ! grep -F "archive_uuid=$UUID archive_label=\"scan-smoke\" new_archive=no" "$SCAN_VERBOSE_LOG" >/dev/null; then
	echo "smoke_scan: verbose output did not include expected archive identity"
	cat "$SCAN_VERBOSE_LOG"
	exit 1
fi

echo "smoke_scan: ok"
