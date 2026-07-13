#!/bin/sh
set -e

SOCK=/tmp/neotape-smoke-$$
SPOOL=/tmp/neotape-smoke-spool-$$
SRC=/tmp/neotape-smoke-src-$$
WRITER_LOG=/tmp/neotape-smoke-writer-$$.log
CLI_LOG=/tmp/neotape-smoke-cli-$$.log
BLOCK=4096

cleanup() {
	rm -rf "$SOCK" "$SPOOL" "$SRC" "$WRITER_LOG" "$CLI_LOG"
}
trap cleanup EXIT

mkdir -p "$SRC"
echo "hello" >"$SRC/a.txt"

if ./bin/neotape-archiver "$SRC" >"$CLI_LOG" 2>&1; then
	echo "smoke_tcp_archive: archiver unexpectedly accepted no --listen"
	exit 1
fi
if ! grep -F -- '--listen is required' "$CLI_LOG" >/dev/null; then
	echo "smoke_tcp_archive: missing required --listen diagnostic"
	cat "$CLI_LOG"
	exit 1
fi

./bin/neotape-archiver \
	-l "unix://$SOCK" \
	-b 4K \
	-n smoke \
	"$SRC" &
ARCHIVER_PID=$!

# Wait for socket to exist.
for _ in $(seq 1 50); do
	if [ -S "$SOCK" ]; then break; fi
	sleep 0.1
done

./bin/neotape-write -s "unix://$SOCK" -t "spool:$SPOOL" -B 8M 2>"$WRITER_LOG"
wait "$ARCHIVER_PID"

if ! grep -F 'writer: first frame parsed block_size=4096' "$WRITER_LOG" >/dev/null; then
	echo "smoke_tcp_archive: missing first-frame block size log"
	cat "$WRITER_LOG"
	exit 1
fi
if ! grep -F 'archive_label="smoke"' "$WRITER_LOG" >/dev/null; then
	echo "smoke_tcp_archive: missing first-frame archive label log"
	cat "$WRITER_LOG"
	exit 1
fi
if ! grep -F 'volume_seq=1' "$WRITER_LOG" >/dev/null; then
	echo "smoke_tcp_archive: missing first-frame volume sequence log"
	cat "$WRITER_LOG"
	exit 1
fi
if ! grep -F 'slice_seq=0' "$WRITER_LOG" >/dev/null; then
	echo "smoke_tcp_archive: missing first-frame slice sequence log"
	cat "$WRITER_LOG"
	exit 1
fi

# The spool should contain two files: one for the content slice
# (filemark after tape_eof) and one for the archive end frame.
FILE_COUNT=$(find "$SPOOL" -maxdepth 1 -type f -name '*.nts' | wc -l)
if [ "$FILE_COUNT" -ne 2 ]; then
	echo "smoke_tcp_archive: expected 2 spool files, got $FILE_COUNT"
	exit 1
fi

# First file is the content slice.
OUT=$(find "$SPOOL" -maxdepth 1 -type f -name '*.nts' | sort | head -n1)
ACTUAL=$(stat -c%s "$OUT")
if [ "$ACTUAL" -lt "$BLOCK" ]; then
	echo "smoke_tcp_archive: first spool file too small: $ACTUAL"
	exit 1
fi

# Second file is the archive end frame.
OUT2=$(find "$SPOOL" -maxdepth 1 -type f -name '*.nts' | sort | tail -n1)
AE_SIZE=$(stat -c%s "$OUT2")
if [ "$AE_SIZE" -ne "$BLOCK" ]; then
	echo "smoke_tcp_archive: archive end file wrong size: $AE_SIZE, expected $BLOCK"
	exit 1
fi

# First 8 bytes must be the NeoTape magic ("NeoTape\0").
MAGIC=$(od -An -tx1 -N8 -j0 "$OUT" | tr -d ' \n')
if [ "$MAGIC" != "4e656f5461706500" ]; then
	echo "smoke_tcp_archive: bad magic at start"
	exit 1
fi

# Byte at offset 9 is the channel type; the first record is ch_content (0x01).
CTYPE=$(od -An -tx1 -N1 -j9 "$OUT" | tr -d ' \n')
if [ "$CTYPE" != "01" ]; then
	echo "smoke_tcp_archive: expected ch_content channel 0x01, got $CTYPE"
	exit 1
fi

# The archive end frame follows all frame payloads and is type 0xff.
AE_MAGIC=$(od -An -tx1 -N8 -j0 "$OUT2" | tr -d ' \n')
AE_TYPE=$(od -An -tx1 -N1 -j9 "$OUT2" | tr -d ' \n')
if [ "$AE_MAGIC" != "4e656f5461706500" ] || [ "$AE_TYPE" != "ff" ]; then
	echo "smoke_tcp_archive: archive end frame mismatch"
	exit 1
fi

echo "smoke_tcp_archive: ok"
