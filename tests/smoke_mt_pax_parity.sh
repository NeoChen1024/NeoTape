#!/bin/sh
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

mkdir -p "$TMP/src/sub"
echo hello > "$TMP/src/a.txt"
echo world > "$TMP/src/b.txt"
echo nested > "$TMP/src/sub/c.txt"

./bin/mt-pax -f "$TMP/mt.out" "$TMP/src" > /dev/null
./bin/neotape-archiver -f "$TMP/archiver.out" "$TMP/src" > /dev/null

mkdir -p "$TMP/mt-extract" "$TMP/arch-extract"
tar -xf "$TMP/mt.out" -C "$TMP/mt-extract"
tar -xf "$TMP/archiver.out" -C "$TMP/arch-extract"

# File access times differ between the two runs because reading the source
# files updates atime.  Normalize them before comparing metadata.
find "$TMP/mt-extract" "$TMP/arch-extract" -exec touch -a -d '2000-01-01' {} +

diff -r "$TMP/mt-extract" "$TMP/arch-extract"
echo "mt-pax parity: ok"
