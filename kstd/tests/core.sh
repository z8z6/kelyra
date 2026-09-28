#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
compiler=${KELYRA:-$root/../kelyra/build/bin/kelyra}
output=${TMPDIR:-/tmp}/kstd-core-$$
trap 'rm -f "$output"' EXIT HUP INT TERM

cd "$root"
for example in ascii math; do
  "$compiler" --emit-exe --module-path="$root/src" -o "$output" "examples/${example}_example.kly"
  "$output"
done
