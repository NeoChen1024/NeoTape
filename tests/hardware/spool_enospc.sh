#!/bin/bash
# Manual, needs sudo for tmpfs mounts. End-to-end: archive across spool directories on tiny tmpfs mounts whose
# capacity is not a multiple of the 1 MiB block size, then restore.
set -u
BIN=${1:?usage: spool_enospc.sh BIN_DIRECTORY}
W=$(mktemp -d /tmp/neotape-enospc-XXXXXX)
UIDGID="uid=$(id -u),gid=$(id -g)"
cleanup() {
    for v in 1 2 3; do mountpoint -q "$W/vol$v" && sudo umount "$W/vol$v"; done
    rm -rf "$W"
}
trap cleanup EXIT
echo "workdir $W"

mkdir -p "$W/src" "$W/vol1" "$W/vol2" "$W/vol3"
head -c $((9 * 1024 * 1024 + 123)) /dev/urandom > "$W/src/random.bin"
head -c 777 /dev/urandom > "$W/src/small.bin"
# 4 MiB + 300 KiB, 2 MiB + 700 KiB, then room for the rest.
sudo mount -t tmpfs -o size=4396k,$UIDGID tmpfs "$W/vol1"
sudo mount -t tmpfs -o size=2748k,$UIDGID tmpfs "$W/vol2"
sudo mount -t tmpfs -o size=40m,$UIDGID tmpfs "$W/vol3"
df -k "$W/vol1" "$W/vol2" "$W/vol3" | tail -3

"$BIN/neotape-archiver" -l "unix://$W/a.sock" -b 1M -n enospc -C "$W/src" . \
    2> "$W/archiver.err" &
ARCHIVER=$!
for i in $(seq 50); do [ -S "$W/a.sock" ] && break; sleep 0.1; done

for v in 1 2 3; do
    "$BIN/neotape-write" -s "unix://$W/a.sock" -t "spool:$W/vol$v" \
        2> "$W/write$v.err"
    echo "write vol$v exit=$?"
    grep -h "volume capacity\|archive complete\|warning" "$W/write$v.err"
done
wait $ARCHIVER; echo "archiver exit=$?"
grep -h "volume committed\|archive complete\|warning" "$W/archiver.err"

echo "--- spool files"
for v in 1 2 3; do
    for f in "$W/vol$v"/*; do
        sz=$(stat -c %s "$f")
        echo "vol$v/$(basename "$f") size=$sz mod1MiB=$((sz % 1048576))"
    done
done

echo "--- restore"
restore() {
    local out=$1
    "$BIN/neotape-extractor" -l "unix://$W/e.sock" -o "$out" 2> "$W/extract.err" &
    local EXTRACTOR=$!
    for i in $(seq 50); do [ -S "$W/e.sock" ] && break; sleep 0.1; done
    for v in 1 2 3; do
        "$BIN/neotape-read" -s "spool:$W/vol$v" -c "unix://$W/e.sock" 2> "$W/read$v.err"
        echo "read vol$v exit=$?"
        grep -h "warning\|partial" "$W/read$v.err"
    done
    wait $EXTRACTOR; echo "extractor exit=$?"
    grep -h "warning\|restore finished" "$W/extract.err"
    rm -rf "$W/restore"; mkdir "$W/restore"
    bsdtar -xf "$out" -C "$W/restore" && diff -r "$W/src" "$W/restore" && echo "restore matches source"
}
restore "$W/out1.pax"

echo "--- crash simulation: partial record appended to vol1's last tape file"
last=$(ls "$W/vol1"/*.NTS | tail -1)
head -c 123456 /dev/urandom >> "$last"
stat -c "%n size=%s" "$last"
restore "$W/out2.pax"
cmp "$W/out1.pax" "$W/out2.pax" && echo "identical pax output"
