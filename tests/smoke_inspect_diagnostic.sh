#!/bin/sh
set -eu

tmp=${TMPDIR:-/tmp}/neotape-inspect-diagnostic.$$
rm -rf "$tmp"
mkdir -p "$tmp"
trap 'rm -rf "$tmp"' EXIT INT TERM

root="$tmp/root"
spool="$tmp/inspect.spool"
other_spool="$tmp/inspect-other.spool"
bad_header_spool="$tmp/inspect-bad-header.spool"
bad_payload_spool="$tmp/inspect-bad-payload.spool"
bad_within_spool="$tmp/inspect-bad-within.spool"
bad_end_spool="$tmp/inspect-bad-end.spool"
missing_end_spool="$tmp/inspect-missing-end.spool"
default_out="$tmp/inspect-default.out"
read_out="$tmp/inspect-read.out"
bad_header_out="$tmp/inspect-bad-header.out"
bad_payload_out="$tmp/inspect-bad-payload.out"
bad_within_out="$tmp/inspect-bad-within.out"
bad_end_out="$tmp/inspect-bad-end.out"
missing_end_out="$tmp/inspect-missing-end.out"

mkdir -p "$root/src"
dd if=/dev/zero of="$root/src/large.bin" bs=1024 count=5120 2>/dev/null
printf 'inspect diagnostic\n' > "$root/src/readme.txt"

bin/neotape init "spool:$spool" --label INSPECT --virtual-tape-size 64M >/dev/null
bin/neotape backup --target "spool:$spool" -C "$root" src \
    --name inspect-smoke >/dev/null
bin/neotape init "spool:$other_spool" --label INSPECT2 --virtual-tape-size 64M >/dev/null
bin/neotape backup --target "spool:$other_spool" -C "$root" src \
    --name inspect-smoke-other >/dev/null

expect_exit_1() {
    set +e
    "$@"
    rc=$?
    set -e
    if [ "$rc" -eq 0 ]; then
        printf 'expected exit 1 from:' >&2
        printf ' %s' "$@" >&2
        printf '\n' >&2
        exit 1
    fi
    if [ "$rc" -ne 1 ]; then
        printf 'expected exit 1, got %s from:' "$rc" >&2
        printf ' %s' "$@" >&2
        printf '\n' >&2
        exit 1
    fi
}

expect_exit_2() {
    set +e
    "$@"
    rc=$?
    set -e
    if [ "$rc" -ne 2 ]; then
        printf 'expected exit 2, got %s from:' "$rc" >&2
        printf ' %s' "$@" >&2
        printf '\n' >&2
        exit 1
    fi
}

expect_exit_2 bin/neotape-inspect badkind:/tmp >/dev/null 2>&1

bin/neotape-inspect "spool:$spool" > "$default_out"
grep -q 'medium' "$default_out"
grep -q 'volume' "$default_out"
grep -q 'frame' "$default_out"
grep -q 'archive_end' "$default_out"
grep -q 'summary: .*errors=0' "$default_out"
grep -Eq 'summary: .*frames=[1-9][0-9]* .*slices=[1-9][0-9]* .*end=yes' "$default_out"

bin/neotape-inspect --read "spool:$spool" > "$read_out"
grep -q 'frame index=0' "$read_out"
grep -q 'summary: .*errors=0' "$read_out"

cp -R "$spool" "$bad_header_spool"
printf 'BROKEN!!' | dd of="$bad_header_spool/tape-file-000002.slice-000001.nts" \
    bs=1 seek=0 conv=notrunc 2>/dev/null
expect_exit_1 bin/neotape-inspect "spool:$bad_header_spool" > "$bad_header_out"
grep -q 'malformed' "$bad_header_out"
grep -q 'bad magic' "$bad_header_out"
grep -q 'archive_end' "$bad_header_out"
grep -Eq 'summary: .*errors=[1-9][0-9]*' "$bad_header_out"

cp -R "$spool" "$bad_payload_spool"
printf 'X' | dd of="$bad_payload_spool/tape-file-000002.slice-000001.nts" \
    bs=1 seek=1024 conv=notrunc 2>/dev/null
expect_exit_1 bin/neotape-inspect --read "spool:$bad_payload_spool" > "$bad_payload_out"
grep -q 'payload BLAKE3 mismatch' "$bad_payload_out"
grep -Eq 'summary: .*errors=[1-9][0-9]*' "$bad_payload_out"

cp -R "$spool" "$bad_within_spool"
python3 - "$bad_within_spool/tape-file-000002.slice-000001.nts" <<'PY'
import sys

path = sys.argv[1]
poly = 0x82F63B78

def crc32c(data):
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ poly
            else:
                crc >>= 1
    return crc ^ 0xFFFFFFFF

with open(path, 'rb') as f:
    data = bytearray(f.read())

block_size = int.from_bytes(data[10:14], 'little')
record = block_size
field = record + 332
data[field:field + 8] = (9).to_bytes(8, 'little')
crc = crc32c(data[record:record + 1020])
data[record + 1020:record + 1024] = crc.to_bytes(4, 'little')

with open(path, 'wb') as f:
    f.write(data)
PY
expect_exit_1 bin/neotape-inspect --read "spool:$bad_within_spool" > "$bad_within_out"
grep -q 'expected frame within slice 2, got 9' "$bad_within_out"
grep -Eq 'summary: .*errors=[1-9][0-9]*' "$bad_within_out"

cp -R "$spool" "$bad_end_spool"
cp "$other_spool/tape-file-000003.archive-end.nts" \
    "$bad_end_spool/tape-file-000003.archive-end.nts"
expect_exit_1 bin/neotape-inspect "spool:$bad_end_spool" > "$bad_end_out"
grep -q 'archive uuid mismatch' "$bad_end_out"
if grep -q 'missing Archive End Header' "$bad_end_out"; then
    printf 'present invalid archive end should not report missing archive end\n' >&2
    exit 1
fi
grep -Eq 'summary: .*errors=[1-9][0-9]*.*end=no' "$bad_end_out"

cp -R "$spool" "$missing_end_spool"
rm "$missing_end_spool/tape-file-000003.archive-end.nts"
bin/neotape-inspect "spool:$missing_end_spool" > "$missing_end_out"
if grep -q 'missing Archive End Header' "$missing_end_out"; then
    printf 'missing archive end should not be an inspect error\n' >&2
    exit 1
fi
grep -Eq 'summary: .*errors=0.*end=no' "$missing_end_out"
