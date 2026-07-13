#!/bin/sh
set -e

SOCK=/tmp/neotape-extract-multi-$$
SPOOL1=/tmp/neotape-extract-vol1-$$
SPOOL2=/tmp/neotape-extract-vol2-$$
SRC=/tmp/neotape-extract-src-$$
EXTRACTOR_SOCKET=/tmp/neotape-extract-sock-$$
REF_PAX=/tmp/neotape-extract-ref-$$
OUT_PAX=/tmp/neotape-extract-out-$$
REF_DIR=/tmp/neotape-extract-refdir-$$
OUT_DIR=/tmp/neotape-extract-outdir-$$
BLOCK=4096
MAX_VOL_BYTES=$((8 * BLOCK)) # force EOT after ~8 frames

cleanup() {
	if [ -n "${EXTRACTOR_PID:-}" ]; then
		kill "$EXTRACTOR_PID" 2>/dev/null || true
		wait "$EXTRACTOR_PID" 2>/dev/null || true
	fi
	if [ -n "${ARCHIVER_PID:-}" ]; then
		kill "$ARCHIVER_PID" 2>/dev/null || true
		wait "$ARCHIVER_PID" 2>/dev/null || true
	fi
	rm -rf "$SOCK" "$EXTRACTOR_SOCKET" "$SPOOL1" "$SPOOL2" "$SRC" \
		"$REF_PAX" "$OUT_PAX" "$REF_DIR" "$OUT_DIR"
}
trap cleanup EXIT

mkdir -p "$SRC"
dd if=/dev/urandom of="$SRC/blob.bin" bs=1M count=5 status=none

# ---- Build reference with the standalone pax writer ----
./bin/mt-pax -C "$SRC" -f "$REF_PAX" blob.bin

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
ARCHIVER_PID=

# ---- Read phase: extractor server ← reader × 2 volumes ----
EXTRACTOR_SOCK="unix://$EXTRACTOR_SOCKET"

./bin/neotape-extractor --listen "$EXTRACTOR_SOCK" -o "$OUT_PAX" &
EXTRACTOR_PID=$!

for _ in $(seq 1 50); do
	[ -S "$EXTRACTOR_SOCKET" ] && break
	sleep 0.1
done

# Reader 1: reads volume 1 (SPOOL1 only).
./bin/neotape-read --source "spool:$SPOOL1" --connect "$EXTRACTOR_SOCK"

# Reader 2: reads volume 2 (SPOOL2 only).
./bin/neotape-read --source "spool:$SPOOL2" --connect "$EXTRACTOR_SOCK"

wait "$EXTRACTOR_PID"
EXTRACTOR_PID=

# ---- Verify: compare extracted content against reference ----
mkdir "$REF_DIR" "$OUT_DIR"
bsdtar -xpf "$REF_PAX" -C "$REF_DIR"
bsdtar -xpf "$OUT_PAX" -C "$OUT_DIR"
if diff -rq "$REF_DIR" "$OUT_DIR"; then
	echo "smoke_tcp_extract_multi: ok"
else
	echo "smoke_tcp_extract_multi: FAIL"
	exit 1
fi
