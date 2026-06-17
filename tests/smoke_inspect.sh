#!/bin/sh
set -e

SOCK=/tmp/neotape-inspect-smoke-$$
SPOOL=/tmp/neotape-inspect-spool-$$
INPUT=/tmp/neotape-inspect-input-$$.bin
SERVER_LOG=/tmp/neotape-inspect-server-$$.log
WRITER_LOG=/tmp/neotape-inspect-writer-$$.log
INSPECT_LOG=/tmp/neotape-inspect-result-$$.log
BLOCK=4096

cleanup() {
	if [ -n "${RAW_STORE_PID:-}" ]; then
		kill "$RAW_STORE_PID" 2>/dev/null || true
	fi
	rm -rf "$SOCK" "$SPOOL" "$INPUT" "$SERVER_LOG" "$WRITER_LOG" "$INSPECT_LOG"
}
trap cleanup EXIT

# Create a raw input that spans multiple frames.
python3 - "$INPUT" <<'PY'
import sys
size = (4096 - 512) * 3 + 100
with open(sys.argv[1], 'wb') as f:
    f.write(bytes((i % 251 for i in range(size))))
PY

./bin/neotape-raw-store \
	--listen "unix://$SOCK" \
	--volume-block-size "$BLOCK" \
	--archive-name inspect-test \
	--retention-frame-count 1 \
	--input "$INPUT" \
	2>"$SERVER_LOG" &
RAW_STORE_PID=$!

for _ in $(seq 1 50); do
	if [ -S "$SOCK" ]; then break; fi
	sleep 0.1
done
if [ ! -S "$SOCK" ]; then
	echo "smoke_inspect: raw-store did not create socket"
	cat "$SERVER_LOG"
	exit 1
fi

./bin/neotape-write --source "unix://$SOCK" --target "spool:$SPOOL" 2>"$WRITER_LOG"
wait "$RAW_STORE_PID"
RAW_STORE_PID=

# --- inspect the spool ---
./bin/neotape-inspect --source "spool:$SPOOL" >"$INSPECT_LOG" 2>&1

if ! grep -F 'Compliance: PASS' "$INSPECT_LOG" >/dev/null; then
	echo "smoke_inspect: compliance check failed"
	cat "$INSPECT_LOG"
	exit 1
fi

# Verify expected frame count (4 content + 1 archive_end = 5 frames).
if ! grep -F 'Total frames:     5' "$INSPECT_LOG" >/dev/null; then
	echo "smoke_inspect: unexpected frame count"
	cat "$INSPECT_LOG"
	exit 1
fi

# Verify 4 content frames.
if ! grep -F 'Content frames:   4' "$INSPECT_LOG" >/dev/null; then
	echo "smoke_inspect: unexpected content frame count"
	cat "$INSPECT_LOG"
	exit 1
fi

# Verify archive_end present.
if ! grep -F 'Archive_end:      1' "$INSPECT_LOG" >/dev/null; then
	echo "smoke_inspect: missing archive_end"
	cat "$INSPECT_LOG"
	exit 1
fi

# Verify archive label.
if ! grep -F 'Archive label:    "inspect-test"' "$INSPECT_LOG" >/dev/null; then
	echo "smoke_inspect: missing/incorrect archive label"
	cat "$INSPECT_LOG"
	exit 1
fi

# Verify volume block size.
if ! grep -F 'Volume block:     4096 bytes' "$INSPECT_LOG" >/dev/null; then
	echo "smoke_inspect: missing/incorrect volume block size"
	cat "$INSPECT_LOG"
	exit 1
fi

# All hashes should be OK — count OK entries.
OK_COUNT=$(grep -c '|    OK' "$INSPECT_LOG" || true)
if [ "$OK_COUNT" -lt 5 ]; then
	echo "smoke_inspect: expected at least 5 OK hashes, got $OK_COUNT"
	cat "$INSPECT_LOG"
	exit 1
fi

# No FAIL entries.
if grep -F '| FAIL' "$INSPECT_LOG" >/dev/null; then
	echo "smoke_inspect: found FAIL entries"
	cat "$INSPECT_LOG"
	exit 1
fi

echo "smoke_inspect: ok"
