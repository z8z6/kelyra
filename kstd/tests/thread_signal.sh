#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
compiler=${KELYRA:-$root/../build/bin/kelyra}
output=$(mktemp)
trap 'rm -f "$output"' EXIT HUP INT TERM

"$compiler" --emit-exe --runtime=freestanding \
  --module-path="$root/src" -o "$output" \
  "$root/examples/thread_signal_example.kly"
"$output"
