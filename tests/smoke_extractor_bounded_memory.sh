#!/bin/sh
set -eu

TMP=$(mktemp -d)
RAW_SOCK="$TMP/raw.sock"
EXTRACT_SOCK="$TMP/extract.sock"
INPUT="$TMP/input.bin"
OUTPUT="$TMP/output.bin"
SPOOL="$TMP/spool"
BLOCK=1048576

cleanup() {
	if [ -n "${RAW_PID:-}" ]; then kill "$RAW_PID" 2>/dev/null || true; fi
	if [ -n "${EXTRACT_PID:-}" ]; then kill "$EXTRACT_PID" 2>/dev/null || true; fi
	rm -rf "$TMP"
}
trap cleanup EXIT

# This single FEC-protected slice is larger than the extractor's address-space
# limit. A slice-sized accumulator fails; group-sized streaming stays bounded.
truncate -s 320M "$INPUT"

./bin/neotape-raw-store \
	--listen "unix://$RAW_SOCK" \
	--input "$INPUT" \
	--volume-block-size "$BLOCK" \
	--archive-name bounded-memory-smoke \
	--fec 2>"$TMP/raw.log" &
RAW_PID=$!

for _ in $(seq 1 100); do
	[ -S "$RAW_SOCK" ] && break
	sleep 0.05
done
[ -S "$RAW_SOCK" ] || { cat "$TMP/raw.log"; exit 1; }

./bin/neotape-write --source "unix://$RAW_SOCK" --target "spool:$SPOOL"
wait "$RAW_PID"
RAW_PID=

(
	ulimit -v 262144
	exec ./bin/neotape-extractor --listen "unix://$EXTRACT_SOCK" \
		-o "$OUTPUT"
) 2>"$TMP/extractor.log" &
EXTRACT_PID=$!

for _ in $(seq 1 100); do
	[ -S "$EXTRACT_SOCK" ] && break
	if ! kill -0 "$EXTRACT_PID" 2>/dev/null; then
		cat "$TMP/extractor.log"
		exit 1
	fi
	sleep 0.05
done
[ -S "$EXTRACT_SOCK" ] || { cat "$TMP/extractor.log"; exit 1; }

./bin/neotape-read --source "spool:$SPOOL" --connect "unix://$EXTRACT_SOCK"
wait "$EXTRACT_PID"
EXTRACT_PID=

cmp "$INPUT" "$OUTPUT"
grep -F 'archive extraction complete' "$TMP/extractor.log" >/dev/null
echo "smoke_extractor_bounded_memory: ok"
