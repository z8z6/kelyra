#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
compiler=${KELYRA:-$root/../build/bin/kelyra}
output=${TMPDIR:-/tmp}/kstd-slice-$$
trap 'rm -f "$output"' EXIT HUP INT TERM

"$compiler" --emit-exe --module-search-path="$root/src" \
  -o "$output" "$root/examples/slice_example.kly"
"$output"
