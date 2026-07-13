#!/bin/sh
set -eu

root=/tmp/neotape-mt-pax-pipeline-root
archive=/tmp/neotape-mt-pax-pipeline.tar
out=/tmp/neotape-mt-pax-pipeline-out
err=/tmp/neotape-mt-pax-pipeline.err

rm -rf "$root" "$archive" "$out" "$err"
mkdir -p "$root/src/dirs" "$root/src/small" "$out"

i=0
while test "$i" -lt 80; do
    mkdir -p "$root/src/dirs/dir-$i"
    printf 'small file %s\n' "$i" > "$root/src/small/file-$i.txt"
    ln -s "../small/file-$i.txt" "$root/src/dirs/dir-$i/link-$i"
    i=$((i + 1))
done

dd if=/dev/zero of="$root/src/large-a.bin" bs=1M count=5 2>/dev/null
dd if=/dev/zero of="$root/src/large-b.bin" bs=1M count=6 2>/dev/null

timeout 120 bin/mt-pax -f "$archive" -C "$root" --io-thread 4 -P 25 --output-buffer-size 8M src >/dev/null 2>"$err"
test -s "$archive"

if command -v bsdtar >/dev/null 2>&1; then
    bsdtar -xpf "$archive" -C "$out"
    cmp "$root/src/small/file-17.txt" "$out/src/small/file-17.txt"
    test -L "$out/src/dirs/dir-17/link-17"
fi

# fd-pressure regression: verify deferred fd open prevents "Can't open"
# warnings under low ulimit with slow output backpressure.
fd_root="$root-fd"
fd_archive="$archive-fd"
fd_err="$err-fd"
rm -rf "$fd_root" "$fd_archive" "$fd_err"
mkdir -p "$fd_root/src"
i=0
while test "$i" -lt 60; do
    dd if=/dev/zero of="$fd_root/src/file-$i.bin" bs=1 count=0 seek=5M 2>/dev/null
    i=$((i + 1))
done
mkfifo "$fd_archive"
(sleep 2; cat "$fd_archive" >/dev/null) &
slow_reader=$!
set +e
timeout 30 sh -c '
    ulimit -n 28
    exec ./bin/mt-pax -f "$1" -C "$2" --io-thread 1 --output-buffer-size 1M src 2>"$3"
' _ "$fd_archive" "$fd_root" "$fd_err"
fd_status=$?
set -e
wait "$slow_reader" 2>/dev/null || true
rm -f "$fd_archive"
if [ "$fd_status" -ne 0 ]; then
    printf 'FAIL: mt-pax fd-pressure test exited with status %s\n' "$fd_status" >&2
    cat "$fd_err" >&2
    exit 1
fi
if grep -q 'Can.t open' "$fd_err"; then
    printf 'FAIL: mt-pax fd-pressure test leaked file descriptors\n' >&2
    exit 1
fi
rm -rf "$fd_root" "$fd_archive" "$fd_err"
