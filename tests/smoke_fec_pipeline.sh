#!/bin/sh
set -eu

TMP=$(mktemp -d)
RAW_SOCK="$TMP/raw.sock"
EXTRACT_SOCK="$TMP/extract.sock"
INPUT="$TMP/input.bin"
OUTPUT="$TMP/output.bin"
SPOOL="$TMP/spool"
RAW_LOG="$TMP/raw.log"
EXTRACT_LOG="$TMP/extract.log"

cleanup() {
	if [ -n "${RAW_PID:-}" ]; then kill "$RAW_PID" 2>/dev/null || true; fi
	if [ -n "${EXTRACT_PID:-}" ]; then kill "$EXTRACT_PID" 2>/dev/null || true; fi
	rm -rf "$TMP"
}
trap cleanup EXIT

python3 - "$INPUT" <<'PY'
import sys
capacity = 4096 - 512
size = capacity * 33 + 123
with open(sys.argv[1], "wb") as output:
    output.write(bytes((i * 29 + 7) % 251 for i in range(size)))
PY

./bin/neotape-raw-store \
	--listen "unix://$RAW_SOCK" \
	--input "$INPUT" \
	--volume-block-size 4096 \
	--archive-name fec-smoke \
	--fec 2>"$RAW_LOG" &
RAW_PID=$!

for _ in $(seq 1 100); do
	[ -S "$RAW_SOCK" ] && break
	sleep 0.05
done
[ -S "$RAW_SOCK" ] || { cat "$RAW_LOG"; exit 1; }

./bin/neotape-write --source "unix://$RAW_SOCK" --target "spool:$SPOOL"
wait "$RAW_PID"
RAW_PID=

./bin/neotape-inspect --source "spool:$SPOOL" >"$TMP/inspect.log"
grep -F 'Compliance: PASS' "$TMP/inspect.log" >/dev/null
grep -F 'fec' "$TMP/inspect.log" >/dev/null

./bin/neotape-extractor --listen "unix://$EXTRACT_SOCK" -o "$OUTPUT" \
	2>"$EXTRACT_LOG" &
EXTRACT_PID=$!
for _ in $(seq 1 100); do
	[ -S "$EXTRACT_SOCK" ] && break
	sleep 0.05
done
[ -S "$EXTRACT_SOCK" ] || { cat "$EXTRACT_LOG"; exit 1; }

./bin/neotape-read --source "spool:$SPOOL" --connect "unix://$EXTRACT_SOCK"
wait "$EXTRACT_PID"
EXTRACT_PID=

cmp "$INPUT" "$OUTPUT"

# Corrupt one protected content payload. Salvage mode must treat it as an
# unavailable shard and reconstruct it from the following repair group.
CONTENT=$(find "$SPOOL" -name '*.slice-*.nts' | head -n 1)
python3 - "$CONTENT" <<'PY'
import sys
with open(sys.argv[1], "r+b") as stream:
    stream.seek(4096 + 512)
    value = stream.read(1)
    stream.seek(4096 + 512)
    stream.write(bytes([value[0] ^ 1]))
PY

EXTRACT_SOCK="$TMP/salvage.sock"
SALVAGE_OUTPUT="$TMP/salvage-output.bin"
./bin/neotape-extractor --listen "unix://$EXTRACT_SOCK" --salvage \
	-o "$SALVAGE_OUTPUT" 2>"$TMP/salvage.log" &
EXTRACT_PID=$!
for _ in $(seq 1 100); do
	[ -S "$EXTRACT_SOCK" ] && break
	sleep 0.05
done
[ -S "$EXTRACT_SOCK" ] || { cat "$TMP/salvage.log"; exit 1; }
./bin/neotape-read --source "spool:$SPOOL" --connect "unix://$EXTRACT_SOCK"
wait "$EXTRACT_PID"
EXTRACT_PID=
cmp "$INPUT" "$SALVAGE_OUTPUT"
grep -F 'salvage skipped frame: frame hash mismatch' "$TMP/salvage.log" >/dev/null
echo "smoke_fec_pipeline: ok"
