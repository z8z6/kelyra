#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
kelp=${KELP:-$root/../kelp/build/kelp}
compiler=${KELYRA:-$root/../kelyra/build/bin/kelyra}
output=$root/build/kstd-string
stdout=${TMPDIR:-/tmp}/kstd-string-$$.stdout
trap 'rm -f "$stdout"' EXIT HUP INT TERM

cd "$root"
mkdir -p "$root/build"
# Build the library object, then link the string example against it.
"$kelp" build
"$compiler" --emit-exe \
  --module-path="$root/src" --external-path="$root/src" \
  --link-input="$root/.kelp/build/kstd.o" \
  -o "$output" "$root/examples/string_example.kly"
"$output" >"$stdout"
test "$(cat "$stdout")" = "again"
