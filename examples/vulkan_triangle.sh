#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
compiler=${KELYRA:-$root/../kelyra/build/bin/kelyra}

mkdir -p "$root/build"
"$compiler" --emit-spirv --shader-entry=vertex_main \
  --module-path="$root/src" -o "$root/build/vulkan_triangle.vert.spv" \
  "$root/examples/vulkan_triangle_shaders.kly"
"$compiler" --emit-spirv --shader-entry=fragment_main \
  --module-path="$root/src" -o "$root/build/vulkan_triangle.frag.spv" \
  "$root/examples/vulkan_triangle_shaders.kly"
"$compiler" --emit-exe --module-path="$root/src" \
  --c-source="$root/src/std/graphics/vulkan/window/window.c" \
  --link-input=/usr/lib/libSDL3.so \
  -o "$root/build/vulkan_triangle" \
  "$root/examples/vulkan_triangle_example.kly"
cd "$root"
exec "$root/build/vulkan_triangle"
