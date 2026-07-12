#!/bin/sh
set -eu

TMP=$(mktemp -d)
RAW_SOCK="$TMP/raw.sock"
EXTRACT_SOCK="$TMP/extract.sock"
INPUT="$TMP/input.bin"
EXPECTED="$TMP/expected.bin"
OUTPUT="$TMP/output.bin"
SPOOL="$TMP/spool"
LOG="$TMP/extractor.log"

cleanup() {
	if [ -n "${RAW_PID:-}" ]; then kill "$RAW_PID" 2>/dev/null || true; fi
	if [ -n "${EXTRACT_PID:-}" ]; then kill "$EXTRACT_PID" 2>/dev/null || true; fi
	rm -rf "$TMP"
}
trap cleanup EXIT

python3 - "$INPUT" <<'PY'
import sys
capacity = 4096 - 512
size = capacity * 2 + 123
with open(sys.argv[1], "wb") as output:
    output.write(bytes(i % 251 for i in range(size)))
PY

./bin/neotape-raw-store --listen "unix://$RAW_SOCK" --input "$INPUT" \
	--volume-block-size 4096 --archive-name salvage-smoke 2>"$TMP/raw.log" &
RAW_PID=$!
for _ in $(seq 1 100); do [ -S "$RAW_SOCK" ] && break; sleep 0.05; done
[ -S "$RAW_SOCK" ] || { cat "$TMP/raw.log"; exit 1; }
./bin/neotape-write --source "unix://$RAW_SOCK" --target "spool:$SPOOL"
wait "$RAW_PID"
RAW_PID=

CONTENT=$(find "$SPOOL" -name '*.slice-*.nts' | head -n 1)
python3 - "$CONTENT" "$INPUT" "$EXPECTED" <<'PY'
import sys
record, original, expected = sys.argv[1:]
capacity = 4096 - 512
with open(record, "r+b") as stream:
    stream.seek(4096 + 512)
    byte = stream.read(1)
    stream.seek(4096 + 512)
    stream.write(bytes([byte[0] ^ 1]))
with open(original, "rb") as source:
    data = source.read()
with open(expected, "wb") as output:
    output.write(data[:capacity])
    output.write(data[capacity * 2:])
PY

./bin/neotape-extractor --listen "unix://$EXTRACT_SOCK" --salvage \
	-o "$OUTPUT" 2>"$LOG" &
EXTRACT_PID=$!
for _ in $(seq 1 100); do [ -S "$EXTRACT_SOCK" ] && break; sleep 0.05; done
[ -S "$EXTRACT_SOCK" ] || { cat "$LOG"; exit 1; }
./bin/neotape-read --source "spool:$SPOOL" --connect "unix://$EXTRACT_SOCK"
wait "$EXTRACT_PID"
EXTRACT_PID=

cmp "$EXPECTED" "$OUTPUT"
grep -F 'SALVAGE MODE: output is not fully verified' "$LOG" >/dev/null
grep -F 'salvage skipped frame: frame hash mismatch' "$LOG" >/dev/null
echo "smoke_salvage: ok"
