#!/bin/sh
set -eu

bin/test_tape

root=/tmp/neotape-file-backed-tape-root
tape=/tmp/neotape-file-backed-tape.dev
archive=/tmp/neotape-file-backed-tape.tar
out=/tmp/neotape-file-backed-tape-out

rm -rf "$root" "$tape" "$archive" "$out"
mkdir -p "$root/src" "$tape" "$out"
printf 'file backed tape restore\n' > "$root/src/file.txt"

bin/neotape backup --target "tape:$tape" -C "$root" src --name file-backed-tape >/dev/null
bin/neotape restore --source "tape:$tape" --output "$archive" >/dev/null
bsdtar -xpf "$archive" -C "$out"
cmp "$root/src/file.txt" "$out/src/file.txt"
