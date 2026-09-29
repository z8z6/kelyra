# Vulkan host API

`std.graphics.vulkan` is a Linux x86-64 module for the Vulkan loader's C ABI.
It currently covers instance creation, instance version discovery, physical
device enumeration, and instance destruction. The public `@layout(Layout.C)`
classes mirror the corresponding Vulkan C records. Callers can fill
`InstanceCreateInfo.enabled_extension_names` with extensions required by a
windowing library before creating an instance.

An example is in `examples/vulkan_instance_example.kly`. Link an executable
that imports the module with the Vulkan loader, for example using the compiler's
`--link-input` option and the full path to `libvulkan.so`. The loader still needs
a Vulkan driver at run time. `tests/vulkan.sh` checks the Kelyra-to-C layout and
calls with a mock implementation, so it runs without a driver or Vulkan headers.

This module exposes host operations only. Compiling Kelyra vertex and fragment
functions to SPIR-V is supported through `kelyra --emit-spirv --shader-entry`.
Shader interface annotations (`@vertex_index`, `@position`, and `@location`)
are declared in `std.graphics`; their stage and type rules and planned HLSL
mapping are documented in `compiler/doc/shader.md`.
On Windows, `std.graphics.window.run_backend(Backend.Vulkan, ...)` creates a
Kelyra `std.graphics.ui.win.Window`, then uses the small native Vulkan adapter in
`native/vulkan_window.cpp` for the surface, swapchain, and draw commands. The
window and UI event loop remain in ordinary Kelyra code. Run
`examples/vulkan_triangle.ps1` to compile the Kelyra shaders to SPIR-V, build
the Vulkan adapter, and open a procedural triangle window. The script requires
the Vulkan SDK, `glslangValidator`, `spirv-val`, and a Vulkan driver.
`tests/vulkan_triangle.ps1` builds and runs the example, reads back one Vulkan
frame, and checks that the center contains the red triangle while the corner
contains the dark background.

The Linux Vulkan host API remains available as `std.graphics.vulkan`; the
window and presentation implementation for Linux is future work.

## Textures and text on Windows

Shader resources are declared with `std.graphics.Texture2D` and
`std.graphics.SamplerState` as separate parameters. Annotate each with
`@std.graphics.binding(set, slot)` and sample with `std.graphics.sample_2d`.
The shader IR keeps resource types and sampling target-neutral; the direct
SPIR-V lowering emits separate image and sampler descriptors. The DXIL
resource path currently reports an explicit unsupported error.

`std.graphics.vulkan.renderer.Renderer` uploads RGBA8 pixels and binds a
texture for drawing. `examples/vulkan_texture.ps1` builds the Kelyra shader
and window example; `tests/vulkan_texture.ps1` checks four colored quadrants in
a captured 800×600 frame.

`std.graphics.font.TextLayout` implements font fallback, line breaking,
bidirectional layout, glyph caching, and atlas packing in Kelyra. It exposes
measurements, clusters, pixels, and vertices. Its low-level declarations call
the published FreeType, HarfBuzz, and ICU C APIs from the pinned sources in
`third_party/`. `std.graphics.font.win` locates system font files, while
`std.graphics.font.vulkan.upload` transfers a prepared atlas to the renderer.
`examples/vulkan_text.ps1` builds and runs the Kelyra example;
`tests/vulkan_text.ps1` checks English, Chinese, Arabic, and color emoji
output, visual order, line breaks, clusters, and glyph reuse.

The Windows font adapter searches a fixed list of system font filenames
(Segoe UI, Microsoft YaHei, Segoe UI Emoji, and Arial); callers can also pass
explicit font paths in fallback order. It does not enumerate every installed
font with DirectWrite. The atlas is currently 2048×2048 per layout, and uploads
wait for the Vulkan queue. The current Kelyra module builds on Windows; a
Linux font-discovery and native-library build adapter remains to be added.
