#!/bin/sh
set -e

SOCK=/tmp/neotape-smoke-$$
OUT=/tmp/neotape-smoke-out-$$
SRC=/tmp/neotape-smoke-src-$$
BLOCK=4096

cleanup() {
    rm -rf "$SOCK" "$OUT" "$SRC"
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

./bin/neotape-write --source "unix://$SOCK" --output "$OUT"
wait "$ARCHIVER_PID"

# NeoTape fixed header size; volume and archive-end headers are each this size.
HEADER_SIZE=1024

ACTUAL=$(stat -c%s "$OUT")
if [ "$ACTUAL" -lt $((HEADER_SIZE * 2 + BLOCK)) ]; then
    echo "smoke_tcp_archive: output too small: $ACTUAL"
    exit 1
fi

# First 8 bytes must be the NeoTape magic ("NeoTape\0").
MAGIC=$(od -An -tx1 -N8 -j0 "$OUT" | tr -d ' \n')
if [ "$MAGIC" != "4e656f5461706500" ]; then
    echo "smoke_tcp_archive: bad magic at start"
    exit 1
fi

# Byte at offset 9 is the header type; the first header is a volume header (0x01).
HTYPE=$(od -An -tx1 -N1 -j9 "$OUT" | tr -d ' \n')
if [ "$HTYPE" != "01" ]; then
    echo "smoke_tcp_archive: expected volume header type 0x01, got $HTYPE"
    exit 1
fi

# Byte at offset 1024+9 is the header type of the first frame header (0x02).
FHTYPE=$(od -An -tx1 -N1 -j$((HEADER_SIZE + 9)) "$OUT" | tr -d ' \n')
if [ "$FHTYPE" != "02" ]; then
    echo "smoke_tcp_archive: expected frame header type 0x02, got $FHTYPE"
    exit 1
fi

# The archive end header follows all frame payloads and is type 0x03.
AE_OFFSET=$((ACTUAL - HEADER_SIZE))
AE_MAGIC=$(od -An -tx1 -N8 -j$AE_OFFSET "$OUT" | tr -d ' \n')
AE_TYPE=$(od -An -tx1 -N1 -j$((AE_OFFSET + 9)) "$OUT" | tr -d ' \n')
if [ "$AE_MAGIC" != "4e656f5461706500" ] || [ "$AE_TYPE" != "03" ]; then
    echo "smoke_tcp_archive: archive end header mismatch"
    exit 1
fi

echo "smoke_tcp_archive: ok"
