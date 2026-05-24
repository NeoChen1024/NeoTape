#!/bin/sh
set -eu

err=/tmp/neotape-tape-backup-wiring.err
rm -f "$err"

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
