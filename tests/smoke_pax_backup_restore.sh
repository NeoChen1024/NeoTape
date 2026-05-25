#!/bin/sh
set -eu

root=/tmp/neotape-pax-smoke-root
spool=/tmp/neotape-pax-smoke.spool
archive=/tmp/neotape-pax-smoke.tar
out=/tmp/neotape-pax-smoke-out
list_json=/tmp/neotape-pax-smoke-list.json
planned_spool=/tmp/neotape-pax-planned-smoke.spool
planned_archive=/tmp/neotape-pax-planned-smoke.tar
planned_out=/tmp/neotape-pax-planned-smoke-out
plan=/tmp/neotape-pax-smoke.plan
planned_backup_err=/tmp/neotape-pax-planned-smoke.err
missing_spool=/tmp/neotape-missing-volume.spool
missing_in=/tmp/neotape-missing-volume.in
missing_out=/tmp/neotape-missing-volume.out
missing_err=/tmp/neotape-missing-volume.err

rm -rf "$root" "$spool" "$archive" "$out" "$list_json" "$planned_spool" \
    "$planned_archive" "$planned_out" "$plan" "$planned_backup_err" "$missing_spool" \
    "$missing_in" "$missing_out" "$missing_err"
mkdir -p "$root/src/dir" "$out"
printf 'hello pax\n' > "$root/src/dir/file.txt"
printf 'planned pax\n' > "$root/src/dir/planned.txt"
printf 'utf8 pax\n' > "$root/src/dir/Pérez.txt"

bin/neotape init "spool:$spool" --label PAX --virtual-tape-size 64M >/dev/null
bin/neotape backup --target "spool:$spool" -C "$root" src --name pax-smoke >/dev/null
test -f "$spool/tape-file-000000.medium-header.nts"
test -f "$spool/tape-file-000001.volume-header.nts"
test -f "$spool/tape-file-000002.slice-000001.nts"
test -f "$spool/tape-file-000003.archive-end.nts"
if test -d "$spool/tape-000001"; then
    printf 'spool backend must use single-root .nts layout, found tape-000001 directory\n' >&2
    exit 1
fi
bin/neotape restore --source "spool:$spool" --output "$archive" >/dev/null
test -s "$archive"
bin/neotape backup --target "spool:$spool" -C "$root" src --name pax-smoke-2 >/dev/null
bin/neotape list --source "spool:$spool" --json > "$list_json"
grep -q '"name": "pax-smoke"' "$list_json"
grep -q '"name": "pax-smoke-2"' "$list_json"

if command -v bsdtar >/dev/null 2>&1; then
    bsdtar -xpf "$archive" -C "$out"
    cmp "$root/src/dir/file.txt" "$out/src/dir/file.txt"
    cmp "$root/src/dir/Pérez.txt" "$out/src/dir/Pérez.txt"
fi

bin/neotape plan -C "$root" -o "$plan" --slice-size 4096 src >/dev/null
bin/neotape init "spool:$planned_spool" --label PAXPLAN --virtual-tape-size 64M >/dev/null
bin/neotape backup --target "spool:$planned_spool" -p "$plan" \
    --name pax-planned -P 25 -B 8M -T 2 >/dev/null 2>"$planned_backup_err"
grep -q 'in @ .* out @ .* files @ .* slice .* total, buffer' "$planned_backup_err"
if grep -q 'planned entries=' "$planned_backup_err"; then
    printf 'planned backup used legacy progress format\n' >&2
    exit 1
fi
planned_first_slice_size=$(stat -c %s "$planned_spool/tape-file-000002.slice-000001.nts")
if test "$planned_first_slice_size" -ne 4194304; then
    printf 'planned backup wrote extra frame data: first slice size=%s\n' \
        "$planned_first_slice_size" >&2
    exit 1
fi
bin/neotape restore --source "spool:$planned_spool" --output "$planned_archive" >/dev/null
test -s "$planned_archive"

if command -v bsdtar >/dev/null 2>&1; then
    mkdir -p "$planned_out"
    bsdtar -xpf "$planned_archive" -C "$planned_out"
    cmp "$root/src/dir/planned.txt" "$planned_out/src/dir/planned.txt"
fi

dd if=/dev/zero of="$missing_in" bs=1024 count=20 2>/dev/null
bin/neotape init "spool:$missing_spool" --label MISS --virtual-tape-size 12K >/dev/null
bin/neotape write --target "spool:$missing_spool" --input "$missing_in" \
    --volume-block-size 4096 --name missing-volume >/dev/null
rm -f "$missing_spool/tape-file-000003.archive-end.nts"
if bin/neotape read --source "spool:$missing_spool" --output "$missing_out" \
    --control=none 2>"$missing_err"; then
    printf 'expected read with missing continuation volume to fail\n' >&2
    exit 1
fi
grep -q 'volume change required but --control=none is set' "$missing_err"
