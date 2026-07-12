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

# Corrupt one protected content payload. Normal restore mode must treat it as
# an unavailable shard and reconstruct it from the following repair group.
CONTENT=$(find "$SPOOL" -name '*.slice-*.nts' | head -n 1)
python3 - "$CONTENT" <<'PY'
import sys
with open(sys.argv[1], "r+b") as stream:
    stream.seek(4096 + 512)
    value = stream.read(1)
    stream.seek(4096 + 512)
    stream.write(bytes([value[0] ^ 1]))
PY

EXTRACT_SOCK="$TMP/repaired.sock"
REPAIRED_OUTPUT="$TMP/repaired-output.bin"
./bin/neotape-extractor --listen "unix://$EXTRACT_SOCK" \
	-o "$REPAIRED_OUTPUT" 2>"$TMP/repaired.log" &
EXTRACT_PID=$!
for _ in $(seq 1 100); do
	[ -S "$EXTRACT_SOCK" ] && break
	sleep 0.05
done
[ -S "$EXTRACT_SOCK" ] || { cat "$TMP/repaired.log"; exit 1; }
./bin/neotape-read --source "spool:$SPOOL" --connect "unix://$EXTRACT_SOCK"
wait "$EXTRACT_PID"
EXTRACT_PID=
cmp "$INPUT" "$REPAIRED_OUTPUT"
grep -F 'waiting for FEC group completion' "$TMP/repaired.log" >/dev/null
grep -F 'FEC repaired 1 unavailable content shard(s)' "$TMP/repaired.log" >/dev/null

# A damaged repair frame is also an unavailable shard, not an immediate fatal
# error. With one content and one repair shard unavailable, recovery still
# succeeds from the remaining 34 positions.
python3 - "$CONTENT" <<'PY'
import sys
with open(sys.argv[1], "r+b") as stream:
    stream.seek(32 * 4096 + 512)
    value = stream.read(1)
    stream.seek(32 * 4096 + 512)
    stream.write(bytes([value[0] ^ 1]))
PY

EXTRACT_SOCK="$TMP/repaired-two.sock"
REPAIRED_TWO_OUTPUT="$TMP/repaired-two-output.bin"
./bin/neotape-extractor --listen "unix://$EXTRACT_SOCK" \
	-o "$REPAIRED_TWO_OUTPUT" 2>"$TMP/repaired-two.log" &
EXTRACT_PID=$!
for _ in $(seq 1 100); do
	[ -S "$EXTRACT_SOCK" ] && break
	sleep 0.05
done
[ -S "$EXTRACT_SOCK" ] || { cat "$TMP/repaired-two.log"; exit 1; }
./bin/neotape-read --source "spool:$SPOOL" --connect "unix://$EXTRACT_SOCK"
wait "$EXTRACT_PID"
EXTRACT_PID=
cmp "$INPUT" "$REPAIRED_TWO_OUTPUT"
grep -F 'FEC repair unavailable' "$TMP/repaired-two.log" >/dev/null

# Add three more unavailable content shards (four content + one repair total).
# RS(32,4) cannot recover five erasures, so normal restore must terminate
# instead of falling back to partial output or waiting for another reader.
python3 - "$CONTENT" <<'PY'
import sys
with open(sys.argv[1], "r+b") as stream:
    for record_index in (2, 3, 4):
        stream.seek(record_index * 4096 + 512)
        value = stream.read(1)
        stream.seek(record_index * 4096 + 512)
        stream.write(bytes([value[0] ^ 1]))
PY

EXTRACT_SOCK="$TMP/unrecoverable.sock"
./bin/neotape-extractor --listen "unix://$EXTRACT_SOCK" \
	-o "$TMP/unrecoverable-output.bin" 2>"$TMP/unrecoverable.log" &
EXTRACT_PID=$!
for _ in $(seq 1 100); do
	[ -S "$EXTRACT_SOCK" ] && break
	sleep 0.05
done
[ -S "$EXTRACT_SOCK" ] || { cat "$TMP/unrecoverable.log"; exit 1; }
set +e
./bin/neotape-read --source "spool:$SPOOL" --connect "unix://$EXTRACT_SOCK"
READ_STATUS=$?
wait "$EXTRACT_PID"
EXTRACT_STATUS=$?
set -e
EXTRACT_PID=
[ "$EXTRACT_STATUS" -ne 0 ] || {
	echo "smoke_fec_pipeline: unrecoverable normal restore unexpectedly succeeded"
	exit 1
}
grep -F 'FEC recovery failed' "$TMP/unrecoverable.log" >/dev/null
grep -F 'unrecoverable frame validation failure' "$TMP/unrecoverable.log" >/dev/null
# The reader normally observes the extractor error as well, but extractor
# failure is the authoritative assertion because socket teardown timing varies.
: "$READ_STATUS"
echo "smoke_fec_pipeline: ok"
