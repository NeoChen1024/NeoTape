#!/bin/sh
set -eu

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# Create test input.
mkdir "$tmp/input"
echo "hello world" >"$tmp/input/hello.txt"
echo "another file" >"$tmp/input/bar.txt"

# Run archiver in local (non-server) mode to get reference pax.
bin/neotape-archiver -C "$tmp/input" -f "$tmp/reference.pax" hello.txt bar.txt

# ---- Write phase: archiver server → writer → spool ----
archiver_sock="unix://$tmp/archiver.sock"

bin/neotape-archiver --listen "$archiver_sock" \
	--archive-name smoke-extract -C "$tmp/input" hello.txt bar.txt &
archiver_pid=$!

# Wait for Unix socket to appear.
for _ in $(seq 1 30); do
	[ -S "$tmp/archiver.sock" ] && break
	sleep 0.1
done

bin/neotape-write --source "$archiver_sock" --target "spool:$tmp/out" --erase
wait "$archiver_pid"

# ---- Read phase: extractor server ← reader ← spool ----
extractor_sock="unix://$tmp/extractor.sock"

bin/neotape-extractor --listen "$extractor_sock" -o "$tmp/extracted.pax" &
extractor_pid=$!

for _ in $(seq 1 30); do
	[ -S "$tmp/extractor.sock" ] && break
	sleep 0.1
done

bin/neotape-read --source "spool:$tmp/out" --connect "$extractor_sock"
wait "$extractor_pid"

# Verify output matches reference pax.
if cmp "$tmp/reference.pax" "$tmp/extracted.pax"; then
	echo "smoke_tcp_extract: ok"
else
	echo "smoke_tcp_extract: FAIL - output differs from reference"
	exit 1
fi
