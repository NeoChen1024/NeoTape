#!/bin/sh
set -e

SOCK=/tmp/neotape-extract-multi-$$
SPOOL1=/tmp/neotape-extract-vol1-$$
SPOOL2=/tmp/neotape-extract-vol2-$$
SRC=/tmp/neotape-extract-src-$$
BLOCK=4096
MAX_VOL_BYTES=$((8 * BLOCK)) # force EOT after ~8 frames

cleanup() {
	if [ -n "${EXTRACTOR_PID:-}" ]; then
		kill "$EXTRACTOR_PID" 2>/dev/null || true
		wait "$EXTRACTOR_PID" 2>/dev/null || true
	fi
	rm -rf "$SOCK" "$SPOOL1" "$SPOOL2" "$SRC"
}
trap cleanup EXIT

mkdir -p "$SRC"
dd if=/dev/urandom of="$SRC/blob.bin" bs=1M count=5 status=none

# ---- Build reference: archiver in local mode ----
REF_PAX=/tmp/neotape-extract-ref-$$
./bin/neotape-archiver -C "$SRC" -f "$REF_PAX" blob.bin

# ---- Write phase: archiver server → writer × 2 volumes ----
ARCHIVER_SOCK="unix://$SOCK"

./bin/neotape-archiver \
	--listen "$ARCHIVER_SOCK" \
	--volume-block-size "$BLOCK" \
	--archive-name multi-extract \
	--retention-frame-count 64 \
	-C "$SRC" blob.bin &
ARCHIVER_PID=$!

for _ in $(seq 1 50); do
	[ -S "$SOCK" ] && break
	sleep 0.1
done

# Writer 1: tiny volume to force EOT.
set +e
./bin/neotape-write --source "$ARCHIVER_SOCK" --target "spool:$SPOOL1" \
	--output-buffer-size 8388608 --max-volume-bytes "$MAX_VOL_BYTES"
rc=$?
set -e
if [ "$rc" -ne 1 ]; then
	echo "smoke_tcp_extract_multi: writer 1 exited with $rc, expected 1"
	exit 1
fi

# Writer 2: finish the archive.
./bin/neotape-write --source "$ARCHIVER_SOCK" --target "spool:$SPOOL2" \
	--output-buffer-size 8388608

wait "$ARCHIVER_PID"

# ---- Read phase: extractor server ← reader × 2 volumes ----
EXTRACTOR_SOCK="unix:///tmp/neotape-extract-sock-$$"
OUT_PAX=/tmp/neotape-extract-out-$$

./bin/neotape-extractor --listen "$EXTRACTOR_SOCK" -o "$OUT_PAX" &
EXTRACTOR_PID=$!

for _ in $(seq 1 50); do
	[ -S "/tmp/neotape-extract-sock-$$" ] && break
	sleep 0.1
done

# Reader 1: reads volume 1 (SPOOL1 only).
./bin/neotape-read --source "spool:$SPOOL1" --connect "$EXTRACTOR_SOCK"

# Reader 2: reads volume 2 (SPOOL2 only).
./bin/neotape-read --source "spool:$SPOOL2" --connect "$EXTRACTOR_SOCK"

wait "$EXTRACTOR_PID"

# ---- Verify: compare extracted content against reference ----
mkdir /tmp/neotape-extract-refdir-$$ /tmp/neotape-extract-outdir-$$
bsdtar -xpf "$REF_PAX" -C "/tmp/neotape-extract-refdir-$$"
bsdtar -xpf "$OUT_PAX" -C "/tmp/neotape-extract-outdir-$$"
if diff -rq "/tmp/neotape-extract-refdir-$$" "/tmp/neotape-extract-outdir-$$"; then
	echo "smoke_tcp_extract_multi: ok"
else
	echo "smoke_tcp_extract_multi: FAIL"
	exit 1
fi
