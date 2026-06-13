#!/bin/sh
set -e

SOCK=/tmp/neotape-smoke-$$
OUT=/tmp/neotape-smoke-out-$$
BLOCK=4096
FRAMES=4

cleanup() {
    rm -f "$SOCK" "$OUT"
}
trap cleanup EXIT

./bin/neotape-archiver \
    --listen "unix://$SOCK" \
    --volume-block-size "$BLOCK" \
    --dummy-frame-count "$FRAMES" \
    --archive-name smoke &
ARCHIVER_PID=$!

# Wait for socket to exist.
for i in $(seq 1 50); do
    if [ -S "$SOCK" ]; then break; fi
    sleep 0.1
done

./bin/neotape-write --source "unix://$SOCK" --output "$OUT"
wait "$ARCHIVER_PID"

EXPECTED=$((1024 + BLOCK * FRAMES + 1024))
ACTUAL=$(stat -c%s "$OUT")
if [ "$ACTUAL" -ne "$EXPECTED" ]; then
    echo "smoke_tcp_archive: size mismatch expected=$EXPECTED actual=$ACTUAL"
    exit 1
fi

echo "smoke_tcp_archive: ok"
