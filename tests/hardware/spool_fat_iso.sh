#!/bin/bash
# Manual, needs sudo for loop mounts; needs mkfs.fat and xorriso.
# Spool on a FAT12 image mounted with the short-name-only msdos driver, and
# a spool read back from an ISO 9660 Level 1 image.
set -u
BIN=${1:?usage: spool_fat_iso.sh BIN_DIRECTORY}
W=$(mktemp -d /tmp/neotape-fat-XXXXXX)
cleanup() {
    for m in fat1 fat2 iso; do mountpoint -q "$W/$m" && sudo umount "$W/$m"; done
    rm -rf "$W"
}
trap cleanup EXIT
mkdir -p "$W/src" "$W/fat1" "$W/fat2" "$W/iso" "$W/disk"
head -c $((3 * 1024 * 1024 + 321)) /dev/urandom > "$W/src/random.bin"

# Two FAT12 images: 2.5 MiB (not a multiple of the 1 MiB block) and 8 MiB.
truncate -s 2560k "$W/fat1.img"; mkfs.fat -F 12 "$W/fat1.img" > /dev/null
truncate -s 8m "$W/fat2.img";   mkfs.fat -F 12 "$W/fat2.img" > /dev/null
for n in 1 2; do
    sudo mount -t msdos -o loop,uid=$(id -u),gid=$(id -g) "$W/fat$n.img" "$W/fat$n"
done
df -k "$W/fat1" "$W/fat2" | tail -2

"$BIN/neotape-archiver" -l "unix://$W/a.sock" -b 1M -n fat -C "$W/src" . 2> "$W/archiver.err" &
ARCHIVER=$!
for i in $(seq 50); do [ -S "$W/a.sock" ] && break; sleep 0.1; done
for n in 1 2; do
    "$BIN/neotape-write" -s "unix://$W/a.sock" -t "spool:$W/fat$n" 2> "$W/write$n.err"
    echo "write fat$n exit=$?"; grep -h "capacity\|complete\|warning" "$W/write$n.err"
done
wait $ARCHIVER; echo "archiver exit=$?"
echo "--- files on msdos mounts"; ls -l "$W/fat1" "$W/fat2" | grep -v total

echo "--- restore from the msdos mounts"
"$BIN/neotape-extractor" -l "unix://$W/e.sock" -o "$W/out.pax" 2> "$W/extract.err" &
EXTRACTOR=$!
for i in $(seq 50); do [ -S "$W/e.sock" ] && break; sleep 0.1; done
for n in 1 2; do
    "$BIN/neotape-read" -s "spool:$W/fat$n" -c "unix://$W/e.sock" 2> "$W/read$n.err"
    echo "read fat$n exit=$?"; grep -h "warning" "$W/read$n.err"
done
wait $EXTRACTOR; echo "extractor exit=$?"
mkdir "$W/restore"; bsdtar -xf "$W/out.pax" -C "$W/restore" && diff -r "$W/src" "$W/restore" && echo "restore matches source"

echo "--- ISO 9660 Level 1: image of both spools, read back"
"$BIN/neotape-inspect" -s "spool:$W/fat1" > /dev/null 2>&1; echo "inspect fat1 exit=$?"
xorriso -as mkisofs -iso-level 1 -o "$W/spool.iso" -graft-points "V1=$W/fat1" "V2=$W/fat2" > "$W/iso.log" 2>&1 || { cat "$W/iso.log"; exit 1; }
sudo mount -t iso9660 -o loop,norock,nojoliet "$W/spool.iso" "$W/iso"
echo "--- files on the ISO (norock, nojoliet)"; ls -l "$W/iso"/v1 "$W/iso"/v2 | grep -v total
"$BIN/neotape-extractor" -l "unix://$W/e2.sock" -o "$W/out2.pax" 2> "$W/extract2.err" &
EXTRACTOR=$!
for i in $(seq 50); do [ -S "$W/e2.sock" ] && break; sleep 0.1; done
for v in v1 v2; do
    "$BIN/neotape-read" -s "spool:$W/iso/$v" -c "unix://$W/e2.sock" 2> "$W/read$v.err"
    echo "read iso/$v exit=$?"; grep -h "warning\|error" "$W/read$v.err"
done
wait $EXTRACTOR; echo "extractor exit=$?"
cmp "$W/out.pax" "$W/out2.pax" && echo "ISO readback identical"
