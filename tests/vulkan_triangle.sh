#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
compiler=${KELYRA:-$root/../kelyra/build/bin/kelyra}
output=${TMPDIR:-/tmp}/kstd-vulkan-triangle-$$
trap 'rm -f "$output.vert.spv" "$output.frag.spv" "$output"' EXIT HUP INT TERM

"$compiler" --emit-spirv --shader-entry=vertex_main \
  --module-path="$root/src" -o "$output.vert.spv" \
  "$root/examples/vulkan_triangle_shaders.kly"
"$compiler" --emit-spirv --shader-entry=fragment_main \
  --module-path="$root/src" -o "$output.frag.spv" \
  "$root/examples/vulkan_triangle_shaders.kly"
spirv-dis "$output.vert.spv" | rg -q 'OpEntryPoint Vertex'
spirv-dis "$output.vert.spv" | rg -q 'BuiltIn Position'
spirv-dis "$output.vert.spv" | rg -q 'BuiltIn VertexIndex'
spirv-dis "$output.frag.spv" | rg -q 'OpEntryPoint Fragment'
spirv-dis "$output.frag.spv" | rg -q 'Location 0'

"$compiler" --emit-exe --module-path="$root/src" \
  --c-source="$root/src/std/graphics/vulkan/window/window.c" \
  --link-input=/usr/lib/libSDL3.so \
  -o "$output" "$root/examples/vulkan_triangle_example.kly"
