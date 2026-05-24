#!/bin/sh
set -eu

err=/tmp/neotape-tape-backup-wiring.err
dir=/tmp/neotape-tape-backup-wiring-dir
rm -rf "$err" "$dir"
mkdir -p "$dir"

if bin/neotape backup --target tape:/tmp/not-a-tape README.md \
    2>"$err"; then
    printf 'expected tape backup against a non-tape path to fail\n' >&2
    exit 1
fi

if grep -q 'backup currently supports spool' "$err"; then
    printf 'tape backup is still blocked at CLI layer\n' >&2
    exit 1
fi

grep -q '/tmp/not-a-tape' "$err"

if bin/neotape backup --target "tape:$dir" README.md \
    2>"$err"; then
    printf 'expected tape backup against a directory to fail\n' >&2
    exit 1
fi

grep -q "$dir" "$err"

if bin/neotape restore --source "tape:$dir" --output /tmp/neotape-tape-backup-wiring.tar \
    2>"$err"; then
    printf 'expected tape restore against a directory to fail\n' >&2
    exit 1
fi

grep -q "$dir" "$err"
