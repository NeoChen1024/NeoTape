#!/bin/sh
set -eu

tmp="${TMPDIR:-/tmp}/neotape-restore-validation-$$"
trap 'rm -rf "$tmp"' EXIT INT TERM

mkdir -p "$tmp/src-a" "$tmp/src-b" "$tmp/out"
printf 'archive-a\n' > "$tmp/src-a/file.txt"
printf 'archive-b\n' > "$tmp/src-b/file.txt"

bin/neotape init "spool:$tmp/a.spool" --label A --virtual-tape-size 64M >/dev/null 2>&1
bin/neotape init "spool:$tmp/b.spool" --label B --virtual-tape-size 64M >/dev/null 2>&1
bin/neotape backup --target "spool:$tmp/a.spool" -C "$tmp" src-a --name A >/dev/null 2>&1
bin/neotape backup --target "spool:$tmp/b.spool" -C "$tmp" src-b --name B >/dev/null 2>&1

mkdir -p "$tmp/mixed.spool"
cp "$tmp/a.spool"/tape-file-000000.medium-header.nts "$tmp/mixed.spool"/
cp "$tmp/a.spool"/tape-file-000001.volume-header.nts "$tmp/mixed.spool"/
cp "$tmp/b.spool"/tape-file-000001.volume-header.nts "$tmp/mixed.spool"/tape-file-000002.volume-header.nts

if bin/neotape restore --source "spool:$tmp/mixed.spool" --output "$tmp/out.pax" --control=none >"$tmp/stdout" 2>"$tmp/stderr"; then
    echo "expected restore to reject wrong archive volume" >&2
    exit 1
fi

if ! grep -q "archive uuid mismatch" "$tmp/stderr"; then
    echo "expected archive uuid mismatch diagnostic" >&2
    cat "$tmp/stderr" >&2
    exit 1
fi
