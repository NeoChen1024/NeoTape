#!/bin/sh
set -eu

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

pubkey="$PWD/3rdparty/signify/regress/regresskey.pub"
seckey="$PWD/3rdparty/signify/regress/regresskey.sec"

mkdir "$tmp/input"
echo "hello signed world" >"$tmp/input/hello.txt"
echo "another signed file" >"$tmp/input/bar.txt"

bin/neotape-archiver -C "$tmp/input" -f "$tmp/reference.pax" hello.txt bar.txt

archiver_sock="unix://$tmp/archiver.sock"

bin/neotape-archiver --listen "$archiver_sock" \
	--archive-name smoke-signed \
	--sign-secret-key "$seckey" \
	-C "$tmp/input" hello.txt bar.txt &
archiver_pid=$!

for _ in $(seq 1 30); do
	[ -S "$tmp/archiver.sock" ] && break
	sleep 0.1
done

bin/neotape-write --source "$archiver_sock" --target "spool:$tmp/out" --erase \
	--verify-pubkey "$pubkey"
wait "$archiver_pid"

bin/neotape-inspect --source "spool:$tmp/out" \
	--verify-pubkey "$pubkey" --require-signed >"$tmp/inspect.log" 2>&1

grep -F 'Compliance: PASS' "$tmp/inspect.log" >/dev/null
grep -F 'Signature errors: 0' "$tmp/inspect.log" >/dev/null

extractor_sock="unix://$tmp/extractor.sock"

bin/neotape-extractor --listen "$extractor_sock" -o "$tmp/extracted.pax" \
	--verify-pubkey "$pubkey" --require-signed &
extractor_pid=$!

for _ in $(seq 1 30); do
	[ -S "$tmp/extractor.sock" ] && break
	sleep 0.1
done

bin/neotape-read --source "spool:$tmp/out" --connect "$extractor_sock"
wait "$extractor_pid"

mkdir "$tmp/ref_out" "$tmp/ext_out"
bsdtar -xpf "$tmp/reference.pax" -C "$tmp/ref_out"
bsdtar -xpf "$tmp/extracted.pax" -C "$tmp/ext_out"
if diff -rq "$tmp/ref_out" "$tmp/ext_out"; then
	echo "smoke_signed_tcp_extract: ok"
else
	echo "smoke_signed_tcp_extract: FAIL - output differs from reference"
	exit 1
fi
