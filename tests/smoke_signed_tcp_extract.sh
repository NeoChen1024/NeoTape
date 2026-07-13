#!/bin/sh
set -eu

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

pubkey="$PWD/3rdparty/signify/regress/regresskey.pub"
seckey="$PWD/3rdparty/signify/regress/regresskey.sec"

mkdir "$tmp/input"
echo "hello signed world" >"$tmp/input/hello.txt"
echo "another signed file" >"$tmp/input/bar.txt"

bin/mt-pax -C "$tmp/input" -f "$tmp/reference.pax" hello.txt bar.txt

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
grep -E 'Signatures valid: [1-9][0-9]*' "$tmp/inspect.log" >/dev/null

# Integrity-only inspection accepts signed frames without claiming that their
# signatures were authenticated.
bin/neotape-inspect --source "spool:$tmp/out" \
	>"$tmp/inspect-unverified.log" 2>&1
grep -F 'Compliance: PASS' "$tmp/inspect-unverified.log" >/dev/null
grep -E 'Signed unverified: [1-9][0-9]*' \
	"$tmp/inspect-unverified.log" >/dev/null
grep -F 'Signatures valid: 0' "$tmp/inspect-unverified.log" >/dev/null
grep -F 'Signature errors: 0' "$tmp/inspect-unverified.log" >/dev/null

# require-signed without a trust root is an invalid CLI configuration.
set +e
bin/neotape-inspect --source "spool:$tmp/out" --require-signed \
	>"$tmp/inspect-no-key.log" 2>&1
inspect_no_key_rc=$?
bin/neotape-extractor --listen "unix://$tmp/invalid.sock" --require-signed \
	>"$tmp/extractor-no-key.log" 2>&1
extractor_no_key_rc=$?
set -e
if [ "$inspect_no_key_rc" -ne 2 ] || [ "$extractor_no_key_rc" -ne 2 ]; then
	echo "smoke_signed_tcp_extract: require-signed without key did not fail as usage error"
	exit 1
fi
grep -F -- '--require-signed requires at least one --verify-pubkey' \
	"$tmp/inspect-no-key.log" >/dev/null
grep -F -- '--require-signed requires at least one --verify-pubkey' \
	"$tmp/extractor-no-key.log" >/dev/null

# Even without a verification key, the Writer must reject a corrupted record
# before it reaches the target backend.
first_slice=$(find "$tmp/out" -maxdepth 1 -type f -name '*.slice-*.nts' | sort | head -n1)
corrupt_sock="$tmp/corrupt.sock"
python3 - "$corrupt_sock" "$first_slice" <<'PY' &
import os
import socket
import struct
import sys

socket_path, record_path = sys.argv[1:]
with open(record_path, "rb") as fh:
    raw = fh.read()
block_size = struct.unpack_from("<H", raw, 10)[0] * 1024
record = bytearray(raw[:block_size])
record[512] ^= 0x01

server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
server.bind(socket_path)
server.listen(1)
conn, _ = server.accept()

def recv_exact(size):
    chunks = []
    remaining = size
    while remaining:
        chunk = conn.recv(remaining)
        if not chunk:
            raise RuntimeError("unexpected disconnect")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)

header = recv_exact(9)
msg_type, payload_size = struct.unpack("<BQ", header)
if msg_type != 0x01 or payload_size != 0:
    raise RuntimeError("expected next_frame")
conn.sendall(struct.pack("<BQ", 0x02, len(record)) + record)
conn.close()
server.close()
PY
corrupt_server_pid=$!

for _ in $(seq 1 30); do
	[ -S "$corrupt_sock" ] && break
	sleep 0.1
done

set +e
bin/neotape-write --source "unix://$corrupt_sock" \
	--target "spool:$tmp/corrupt-out" --erase \
	>"$tmp/corrupt-writer.log" 2>&1
corrupt_writer_rc=$?
set -e
wait "$corrupt_server_pid"
if [ "$corrupt_writer_rc" -eq 0 ]; then
	echo "smoke_signed_tcp_extract: Writer accepted a corrupt record without a key"
	exit 1
fi
grep -F 'frame hash mismatch' "$tmp/corrupt-writer.log" >/dev/null

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

# A normal extractor may consume signed frames without a key, but must warn
# that authenticity was not verified.
unverified_sock="unix://$tmp/extractor-unverified.sock"
bin/neotape-extractor --listen "$unverified_sock" \
	-o "$tmp/extracted-unverified.pax" \
	>"$tmp/extractor-unverified.log" 2>&1 &
unverified_pid=$!

for _ in $(seq 1 30); do
	[ -S "$tmp/extractor-unverified.sock" ] && break
	sleep 0.1
done

bin/neotape-read --source "spool:$tmp/out" --connect "$unverified_sock"
wait "$unverified_pid"
grep -F 'signed frames are not authenticated' \
	"$tmp/extractor-unverified.log" >/dev/null
cmp "$tmp/extracted.pax" "$tmp/extracted-unverified.pax"

mkdir "$tmp/ref_out" "$tmp/ext_out"
bsdtar -xpf "$tmp/reference.pax" -C "$tmp/ref_out"
bsdtar -xpf "$tmp/extracted.pax" -C "$tmp/ext_out"
if diff -rq "$tmp/ref_out" "$tmp/ext_out"; then
	echo "smoke_signed_tcp_extract: ok"
else
	echo "smoke_signed_tcp_extract: FAIL - output differs from reference"
	exit 1
fi
