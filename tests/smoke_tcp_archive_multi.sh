#!/bin/sh
set -e

SOCK=/tmp/neotape-multi-$$
SPOOL1=/tmp/neotape-multi-vol1-$$
SPOOL2=/tmp/neotape-multi-vol2-$$
SRC=/tmp/neotape-multi-src-$$
BLOCK=4096
MAX_VOL_BYTES=$((8 * BLOCK)) # force EOT after ~8 frames
HEADER_SIZE=1024

cleanup() {
    if [ -n "${ARCHIVER_PID:-}" ]; then
        kill "$ARCHIVER_PID" 2>/dev/null || true
        wait "$ARCHIVER_PID" 2>/dev/null || true
    fi
    rm -rf "$SOCK" "$SPOOL1" "$SPOOL2" "$SRC"
}
trap cleanup EXIT

mkdir -p "$SRC"
dd if=/dev/urandom of="$SRC/blob.bin" bs=1M count=5 status=none

./bin/neotape-archiver \
    --listen "unix://$SOCK" \
    --volume-block-size "$BLOCK" \
    --archive-name multi-smoke \
    --retention-frame-count 64 \
    "$SRC" &
ARCHIVER_PID=$!

for i in $(seq 1 50); do
    if [ -S "$SOCK" ]; then break; fi
    sleep 0.1
done

# Writer 1: tiny volume.
set +e
./bin/neotape-write --source "unix://$SOCK" --target "spool:$SPOOL1" \
    --output-buffer-size 8388608 --max-volume-bytes "$MAX_VOL_BYTES"
rc=$?
set -e
if [ "$rc" -ne 1 ]; then
    echo "smoke_tcp_archive_multi: writer 1 exited with $rc, expected 1"
    exit 1
fi

# Writer 2: large enough to finish.
./bin/neotape-write --source "unix://$SOCK" --target "spool:$SPOOL2" \
    --output-buffer-size 8388608

wait "$ARCHIVER_PID"

# Verify two distinct volume headers.
for vh in "$SPOOL1" "$SPOOL2"; do
    FIRST=$(find "$vh" -maxdepth 1 -type f -name '*.nts' | sort | head -n1)
    if [ -z "$FIRST" ]; then
        echo "smoke_tcp_archive_multi: missing spool files in $vh"
        exit 1
    fi
    MAGIC=$(od -An -tx1 -N8 -j0 "$FIRST" | tr -d ' \n')
    if [ "$MAGIC" != "4e656f5461706500" ]; then
        echo "smoke_tcp_archive_multi: bad magic in $vh"
        exit 1
    fi
    HTYPE=$(od -An -tx1 -N1 -j9 "$FIRST" | tr -d ' \n')
    if [ "$HTYPE" != "01" ]; then
        echo "smoke_tcp_archive_multi: expected volume header in $vh"
        exit 1
    fi
done

# Verify the second spool ends with an archive end header.
LAST=$(find "$SPOOL2" -maxdepth 1 -type f -name '*.nts' | sort | tail -n1)
if [ -z "$LAST" ]; then
    echo "smoke_tcp_archive_multi: missing spool files in $SPOOL2"
    exit 1
fi
LAST_SIZE=$(stat -c%s "$LAST")
if [ "$LAST_SIZE" -lt "$HEADER_SIZE" ]; then
    echo "smoke_tcp_archive_multi: second spool file too small"
    exit 1
fi
AE_OFFSET=$((LAST_SIZE - HEADER_SIZE))
AE_MAGIC=$(od -An -tx1 -N8 -j$AE_OFFSET "$LAST" | tr -d ' \n')
AE_TYPE=$(od -An -tx1 -N1 -j$((AE_OFFSET + 9)) "$LAST" | tr -d ' \n')
if [ "$AE_MAGIC" != "4e656f5461706500" ] || [ "$AE_TYPE" != "03" ]; then
    echo "smoke_tcp_archive_multi: second spool missing archive end header"
    exit 1
fi

echo "smoke_tcp_archive_multi: ok"
