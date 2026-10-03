#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
compiler=${KELYRA:-$root/../build/bin/kelyra}
output=${TMPDIR:-/tmp}/kstd-singleton-$$
trap 'rm -f "$output"' EXIT HUP INT TERM

for example in atomic_once_example singleton_example singleton_external_example; do
  "$compiler" --emit-exe --module-search-path="$root/src" \
    -o "$output" "$root/examples/$example.kly"
  "$output"
done

if [ "$(uname -s)" = Linux ]; then
  "$compiler" --emit-exe --module-search-path="$root/src" \
    -o "$output" "$root/examples/singleton_concurrent_example.kly"
  "$output"
fi
