#!/bin/sh
set -eu

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

check_help() {
	program=$1
	shift
	"$program" --help >"$tmp/help" 2>&1
	for expected in "$@"; do
		if ! grep -F -- "$expected" "$tmp/help" >/dev/null; then
			echo "smoke_cli_options: $program help is missing $expected" >&2
			cat "$tmp/help" >&2
			exit 1
		fi
	done
}

check_help bin/mt-pax '-j|--io-thread' '-B|--output-buffer-size' \
	'-p|--plan' '-S|--slice-output-prefix' 'SIZE accepts K, M, G, or T'
check_help bin/neotape-plan '-s|--slice-size' '-m|--metadata-buffer-size' \
	'-j|--io-threads' 'SIZE accepts K, M, G, or T'
check_help bin/neotape-archiver '-l|--listen' '-B|--output-buffer-size' \
	'-r|--retention-frame-count' '-F|--fec' '-k|--sign-secret-key' \
	'SIZE accepts K, M, G, or T'
check_help bin/neotape-raw-store '-l|--listen' '-b|--volume-block-size' \
	'-r|--retention-frame-count' '-F|--fec' 'SIZE accepts K, M, G, or T'
check_help bin/neotape-write '-s|--source' '-t|--target' \
	'-B|--output-buffer-size' '-m|--max-volume-bytes' \
	'-R|--recovery-bundle' '-r|--recovery-bundle-block-size' \
	'SIZE accepts K, M, G, or T'
check_help bin/neotape-extractor '-l|--listen' '-k|--verify-pubkey' \
	'-S|--require-signed' '-s|--salvage'
check_help bin/neotape-inspect '-s|--source' '-k|--verify-pubkey' \
	'-S|--require-signed' '-d|--debug' '-r|--raw'
check_help bin/neotape-read '-s|--source' '-c|--connect'
check_help bin/neotape-scan '-s|--source' '-v|--verbose'
check_help bin/neotape-dump '-s|--source' '-t|--target' '-v|--verbose'

mkdir "$tmp/input"
printf 'short option test\n' >"$tmp/input/file.txt"
bin/neotape-plan -C "$tmp/input" -s 16M -m 4M -j 1 \
	-o "$tmp/plan" file.txt
bin/mt-pax -C "$tmp/input" -B 8M -j 1 -f "$tmp/output.pax" file.txt
test -s "$tmp/plan"
test -s "$tmp/output.pax"

echo "smoke_cli_options: ok"
