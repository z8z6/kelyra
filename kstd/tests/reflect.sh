#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
kelp=${KELP:-$root/../build/bin/kelp}
compiler=${KELYRA:-$root/../build/bin/kelyra}
temporary=$(mktemp -d)
trap 'rm -f "$temporary/option" "$temporary/result" "$temporary/reflect"; rmdir "$temporary"' EXIT HUP INT TERM

cd "$root"
"$kelp" build
for example in option result reflect; do
  "$compiler" --emit-exe --runtime=freestanding \
    --module-search-path="$root/src" --external-path="$root/src" \
    --link-input="$root/.kelp/build/kstd.o" \
    -o "$temporary/$example" "$root/examples/${example}_example.kly"
  "$temporary/$example"
done
