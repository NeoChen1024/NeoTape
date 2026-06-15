#!/bin/sh
set -e

SOCK=/tmp/neotape-smoke-$$
SPOOL=/tmp/neotape-smoke-spool-$$
SRC=/tmp/neotape-smoke-src-$$
BLOCK=4096

cleanup() {
    rm -rf "$SOCK" "$SPOOL" "$SRC"
}
trap cleanup EXIT

mkdir -p "$SRC"
echo "hello" > "$SRC/a.txt"

./bin/neotape-archiver \
    --listen "unix://$SOCK" \
    --volume-block-size "$BLOCK" \
    --archive-name smoke \
    "$SRC" &
ARCHIVER_PID=$!

# Wait for socket to exist.
for i in $(seq 1 50); do
    if [ -S "$SOCK" ]; then break; fi
    sleep 0.1
done

./bin/neotape-write --source "unix://$SOCK" --target "spool:$SPOOL"
wait "$ARCHIVER_PID"

# NeoTape fixed header size; volume and archive-end headers are each this size.
HEADER_SIZE=512

# The spool should contain exactly one finalized tape file for a small archive.
FILE_COUNT=$(find "$SPOOL" -maxdepth 1 -type f -name '*.nts' | wc -l)
if [ "$FILE_COUNT" -ne 1 ]; then
    echo "smoke_tcp_archive: expected 1 spool file, got $FILE_COUNT"
    exit 1
fi

OUT=$(find "$SPOOL" -maxdepth 1 -type f -name '*.nts' | head -n1)
ACTUAL=$(stat -c%s "$OUT")
if [ "$ACTUAL" -lt $((BLOCK * 2)) ]; then
    echo "smoke_tcp_archive: output too small: $ACTUAL"
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
AE_OFFSET=$((ACTUAL - BLOCK))
AE_MAGIC=$(od -An -tx1 -N8 -j$AE_OFFSET "$OUT" | tr -d ' \n')
AE_TYPE=$(od -An -tx1 -N1 -j$((AE_OFFSET + 9)) "$OUT" | tr -d ' \n')
if [ "$AE_MAGIC" != "4e656f5461706500" ] || [ "$AE_TYPE" != "ff" ]; then
    echo "smoke_tcp_archive: archive end frame mismatch"
    exit 1
fi

echo "smoke_tcp_archive: ok"
