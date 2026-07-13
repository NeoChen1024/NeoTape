#!/bin/sh
set -eu

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

mkdir "$tmp/src"
printf 'hello hardlink\n' >"$tmp/src/a.txt"
ln "$tmp/src/a.txt" "$tmp/src/b.txt"

bin/neotape-plan -C "$tmp/src" -o "$tmp/plan" a.txt b.txt

tr -d '\000' <"$tmp/plan" >"$tmp/plan.txt"
if ! grep -F '/0/1/h/0/' "$tmp/plan.txt" >/dev/null; then
    echo "smoke_plan_hardlink: expected second entry to be hardlink kind with size 0"
    cat "$tmp/plan.txt"
    exit 1
fi

bin/mt-pax --plan "$tmp/plan" --slice-output-prefix "$tmp/planned-" --io-thread 4

mkdir "$tmp/out"
bsdtar -xpf "$tmp/planned-000000.pax" -C "$tmp/out"

if ! cmp -s "$tmp/src/a.txt" "$tmp/out/a.txt"; then
    echo "smoke_plan_hardlink: extracted a.txt content mismatch"
    exit 1
fi
if ! cmp -s "$tmp/src/b.txt" "$tmp/out/b.txt"; then
    echo "smoke_plan_hardlink: extracted b.txt content mismatch"
    exit 1
fi

inode_a=$(stat -c%i "$tmp/out/a.txt")
inode_b=$(stat -c%i "$tmp/out/b.txt")
if [ "$inode_a" != "$inode_b" ]; then
    echo "smoke_plan_hardlink: extracted files are not hardlinked"
    exit 1
fi

echo "smoke_plan_hardlink: ok"
