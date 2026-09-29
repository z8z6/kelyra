# Shader compilation

`kelyra --emit-spirv --shader-entry=vertex_main -o vertex.spv shader.kly`
compiles a Kelyra `@vertex` or `@fragment` function to a Vulkan SPIR-V module.
The compiler checks the Kelyra source first, lowers the supported shader subset
to shared Shader IR, then directly serializes the MLIR SPIR-V dialect module.
`spirv-val` can independently validate the emitted binary.

A shader entry returns a data class. Annotate its vertex-position field with
`@position` and other interface fields with `@location(N)`. Annotate a vertex
entry's index parameter with `@vertex_index`; other entry parameters use
`@location(N)`. Interface vectors are ordinary
classes with two to four fields of the same `f32`, `i32`, or `u32` type.
`std.graphics` provides `Vec2`, `Vec3`, and `Vec4` for this purpose. Import
`std.graphics` and use `Vec2`, `Vec3`, or `Vec4` directly in shader signatures and
constructors. This preserves the same Kelyra class and function syntax used by
host code.
See `kstd/examples/triangle_shaders.kly` for a complete vertex and
fragment pair.

## Sampled resources

`std.graphics.Texture2D` and `std.graphics.SamplerState` are separate shader
resource types. Declare them as entry parameters with
`@std.graphics.binding(set, slot)` and call `std.graphics.sample_2d` from a
fragment shader. The binding annotation resolves by its full module name, so
another module can define its own `binding` annotation. The compiler checks
resource types, binding uniqueness, and sampling arguments. The shared Shader
IR represents resources and sampling; Vulkan SPIR-V emits separate sampled
image and sampler descriptors. See `kstd/examples/texture_shaders.kly`.

DXIL resource lowering is not yet implemented. Requesting DXIL for a shader
that uses these resources fails with an explicit error instead of producing
invalid bytecode.

The first shader subset supports scalar `f32`, `i32`, `u32`, `bool`, data
classes, direct calls to local or imported Kelyra functions, arithmetic,
comparisons, typed or inferred
local variables, `if`, `while`, and return statements. Data class constructors
must copy their parameters into fields in declaration order. Unsupported
operations are diagnosed; host pointers, heap allocation, slices, and runtime
library calls cannot enter the GPU call graph. Shader interface support
currently covers Vulkan vertex position, vertex index, and user locations.

## Built-in interface values

The interface annotations are declared in `std.graphics` and refer to shader
semantics rather than backend spelling:

| Kelyra | Vulkan SPIR-V | D3D12 DXIL |
| --- | --- | --- |
| `@vertex_index` on a vertex entry's `u32` parameter | `VertexIndex` | `SV_VertexID` |
| `@position` on a vertex output's `Vec4` field | `Position` | `SV_Position` |
| `@location(0)` on a fragment output field | `Location 0` | `SV_Target0` |

The compiler checks the annotation's target, shader stage, and value type.
`@position` currently represents a vertex output in clip space; a fragment
input with a similar HLSL semantic would need an explicit extension to the
shader interface rules. The compiler emits SPIR-V and DXIL directly from Shader
IR. DXIL output is validated and signed with the DXIL validator; this step
does not compile HLSL source. See [Shader IR](shader-ir.md) for the backend
pipeline and pinned LLVM patch instructions.
