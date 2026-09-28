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
mapping are documented in `kelyra/doc/shader.md`.
On Windows, `std.graphics.window.run_backend(Backend.Vulkan, ...)` creates a
Kelyra `std.ui.win.Window`, then uses the small native Vulkan adapter in
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
