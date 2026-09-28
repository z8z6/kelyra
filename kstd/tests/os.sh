#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
compiler=${KELYRA:-$root/../kelyra/build/bin/kelyra}
output=${TMPDIR:-/tmp}/kstd-os-$$
stdout=${TMPDIR:-/tmp}/kstd-os-$$.stdout
trap 'rm -f "$output" "$stdout"' EXIT HUP INT TERM

cd "$root"
"$compiler" --emit-exe \
  --module-path="$root/src" \
  -o "$output" "$root/examples/os_example.kly"
KSTD_OS_TEST=works "$output" >"$stdout"
test "$(cat "$stdout")" = "works"
