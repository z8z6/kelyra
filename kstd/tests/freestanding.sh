#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
kelp=${KELP:-$root/../kelp/build/kelp}
compiler=${KELYRA:-$root/../kelyra/build/bin/kelyra}
temporary=$(mktemp -d)
trap 'rm -f "$temporary/roundtrip.txt" "$temporary/program" "$temporary/stdout"; rmdir "$temporary"' EXIT HUP INT TERM

cd "$root"
"$kelp" build
"$compiler" --emit-exe --runtime=freestanding \
  --module-path="$root/src" --external-path="$root/src" \
  --link-input="$root/.kelp/build/kstd.o" \
  -o "$temporary/program" "$root/examples/freestanding_example.kly"
cd "$temporary"
./program >stdout
test "$(cat stdout)" = "ok"
test "$(cat roundtrip.txt)" = "aaa"
