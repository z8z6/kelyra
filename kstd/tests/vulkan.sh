#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
compiler=${KELYRA:-$root/../build/bin/kelyra}
output=${TMPDIR:-/tmp}/kstd-vulkan-$$
trap 'rm -f "$output"' EXIT HUP INT TERM

"$compiler" --emit-exe --module-search-path="$root/src" \
  --c-source="$root/tests/vulkan_mock.c" \
  -o "$output" "$root/examples/vulkan_instance_example.kly"
"$output"
