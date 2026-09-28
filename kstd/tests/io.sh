#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
kelp=${KELP:-$root/../kelp/build/kelp}
compiler=${KELYRA:-$root/../kelyra/build/bin/kelyra}
output=$root/build/kstd-io
stdout=${TMPDIR:-/tmp}/kstd-io-$$.stdout
stderr=${TMPDIR:-/tmp}/kstd-io-$$.stderr
trap 'rm -f "$stdout" "$stderr"' EXIT HUP INT TERM

cd "$root"
mkdir -p "$root/build"
# Build the library object, then link the example against it: the example only
# declares the std modules and resolves their definitions from the object.
"$kelp" build
"$compiler" --emit-exe \
  --module-path="$root/src" --external-path="$root/src" \
  --link-input="$root/.kelp/build/kstd.o" \
  -o "$output" "$root/examples/io_example.kly"
printf 'hello from input\n' | "$output" >"$stdout" 2>"$stderr"
test ! -s "$stderr"
test "$(cat "$stdout")" = "answer = 42
hello from input"
