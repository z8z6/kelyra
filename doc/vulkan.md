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
`std.graphics.vulkan.window` uses SDL3 to create a window and submit graphics
commands through its Vulkan backend. Its first pipeline mode supports procedural
triangle-list geometry without vertex buffers or shader resources.

Run `sh examples/vulkan_triangle.sh` from the `kstd` repository to compile the
Kelyra vertex and fragment functions, build the native host program, and open
the triangle window. The fragment shader imports `vulkan_triangle_palette` and
calls its public `triangle_color` function without a module prefix. This
requires SDL3 headers and library, `glslangValidator`, `spirv-val`, and a Vulkan
driver. `sh tests/vulkan_triangle.sh` compiles and
checks the shader modules and host executable without opening a window.
