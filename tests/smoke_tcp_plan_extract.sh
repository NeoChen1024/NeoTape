#!/bin/sh
set -eu

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

mkdir "$tmp/input" "$tmp/ref_out" "$tmp/ext_out"
printf 'hello plan mode\n' >"$tmp/input/hello.txt"
dd if=/dev/zero of="$tmp/input/blob.bin" bs=1M count=6 status=none

bin/neotape-plan -C "$tmp/input" -o "$tmp/plan" hello.txt blob.bin
bin/mt-pax --plan "$tmp/plan" --slice-output-prefix "$tmp/reference-" --io-thread 4

archiver_sock="unix://$tmp/archiver.sock"
bin/neotape-archiver --listen "$archiver_sock" \
	--archive-name smoke-plan \
	--io-thread 4 \
	--plan "$tmp/plan" \
	--output-buffer-size 1M \
	-P 80 &
archiver_pid=$!

for _ in $(seq 1 30); do
	[ -S "$tmp/archiver.sock" ] && break
	sleep 0.1
done

bin/neotape-write --source "$archiver_sock" --target "spool:$tmp/out" --erase
wait "$archiver_pid"

extractor_sock="unix://$tmp/extractor.sock"
bin/neotape-extractor --listen "$extractor_sock" -o "$tmp/extracted.pax" &
extractor_pid=$!

for _ in $(seq 1 30); do
	[ -S "$tmp/extractor.sock" ] && break
	sleep 0.1
done

bin/neotape-read --source "spool:$tmp/out" --connect "$extractor_sock"
wait "$extractor_pid"

bsdtar -xpf "$tmp/reference-000000.pax" -C "$tmp/ref_out"
bsdtar -xpf "$tmp/extracted.pax" -C "$tmp/ext_out"
if diff -rq "$tmp/ref_out" "$tmp/ext_out"; then
	echo "smoke_tcp_plan_extract: ok"
else
	echo "smoke_tcp_plan_extract: FAIL - output differs from reference"
	exit 1
fi
