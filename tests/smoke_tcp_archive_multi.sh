#!/bin/sh
set -e

SOCK=/tmp/neotape-multi-$$
SPOOL1=/tmp/neotape-multi-vol1-$$
SPOOL2=/tmp/neotape-multi-vol2-$$
SRC=/tmp/neotape-multi-src-$$
INSPECT1=/tmp/neotape-multi-inspect1-$$.log
INSPECT2=/tmp/neotape-multi-inspect2-$$.log
BLOCK=4096
MAX_VOL_BYTES=$((8 * BLOCK)) # force EOT after ~8 frames
HEADER_SIZE=512

cleanup() {
    if [ -n "${ARCHIVER_PID:-}" ]; then
        kill "$ARCHIVER_PID" 2>/dev/null || true
        wait "$ARCHIVER_PID" 2>/dev/null || true
    fi
    rm -rf "$SOCK" "$SPOOL1" "$SPOOL2" "$SRC" "$INSPECT1" "$INSPECT2"
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

# Verify first-record channel types.
for spool in "$SPOOL1" "$SPOOL2"; do
    FIRST=$(find "$spool" -maxdepth 1 -type f -name '*.nts' | sort | head -n1)
    if [ -z "$FIRST" ]; then
        echo "smoke_tcp_archive_multi: missing spool files in $spool"
        exit 1
    fi
    MAGIC=$(od -An -tx1 -N8 -j0 "$FIRST" | tr -d ' \n')
    if [ "$MAGIC" != "4e656f5461706500" ]; then
        echo "smoke_tcp_archive_multi: bad magic in $spool"
        exit 1
    fi
    CTYPE=$(od -An -tx1 -N1 -j9 "$FIRST" | tr -d ' \n')
    if [ "$CTYPE" != "01" ] && [ "$CTYPE" != "ff" ]; then
        echo "smoke_tcp_archive_multi: expected frame channel in $spool, got $CTYPE"
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
AE_OFFSET=$((LAST_SIZE - BLOCK))
AE_MAGIC=$(od -An -tx1 -N8 -j$AE_OFFSET "$LAST" | tr -d ' \n')
AE_TYPE=$(od -An -tx1 -N1 -j$((AE_OFFSET + 9)) "$LAST" | tr -d ' \n')
if [ "$AE_MAGIC" != "4e656f5461706500" ] || [ "$AE_TYPE" != "ff" ]; then
    echo "smoke_tcp_archive_multi: second spool missing archive end frame"
    exit 1
fi

./bin/neotape-inspect --source "spool:$SPOOL1" >"$INSPECT1" 2>&1
if ! grep -F 'Compliance: PASS' "$INSPECT1" >/dev/null; then
    echo "smoke_tcp_archive_multi: partial volume should pass inspect"
    cat "$INSPECT1"
    exit 1
fi
if ! grep -F 'Archive_end:      0' "$INSPECT1" >/dev/null; then
    echo "smoke_tcp_archive_multi: expected no archive_end on partial volume"
    cat "$INSPECT1"
    exit 1
fi

./bin/neotape-inspect --source "spool:$SPOOL2" >"$INSPECT2" 2>&1
if ! grep -F 'Compliance: PASS' "$INSPECT2" >/dev/null; then
    echo "smoke_tcp_archive_multi: completed second volume should pass inspect"
    cat "$INSPECT2"
    exit 1
fi

echo "smoke_tcp_archive_multi: ok"
