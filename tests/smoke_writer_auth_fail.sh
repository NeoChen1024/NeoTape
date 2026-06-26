#!/bin/sh
set -eu

tmp=$(mktemp -d)
trap 'if [ -n "${archiver_pid:-}" ]; then kill "$archiver_pid" 2>/dev/null || true; wait "$archiver_pid" 2>/dev/null || true; fi; rm -rf "$tmp"' EXIT

pubkey="$PWD/3rdparty/signify/regress/regresskey.pub"

mkdir "$tmp/input"
echo "unsigned auth failure" >"$tmp/input/hello.txt"

archiver_sock="unix://$tmp/archiver.sock"

bin/neotape-archiver --listen "$archiver_sock" \
	--archive-name smoke-auth-fail \
	-C "$tmp/input" hello.txt &
archiver_pid=$!

for _ in $(seq 1 30); do
	[ -S "$tmp/archiver.sock" ] && break
	sleep 0.1
done

set +e
bin/neotape-write --source "$archiver_sock" --target "spool:$tmp/out" --erase \
	--verify-pubkey "$pubkey" >"$tmp/writer.log" 2>&1
rc=$?
set -e

if [ "$rc" -eq 0 ]; then
	echo "smoke_writer_auth_fail: writer unexpectedly succeeded"
	cat "$tmp/writer.log"
	exit 1
fi

if ! grep -Ei 'auth|signer' "$tmp/writer.log" >/dev/null; then
	echo "smoke_writer_auth_fail: missing auth failure message"
	cat "$tmp/writer.log"
	exit 1
fi

echo "smoke_writer_auth_fail: ok"
