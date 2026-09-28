# Shader IR

Kelyra shader source uses ordinary functions and data classes. The shared
representation is an MLIR module in the Kelyra dialect, with its shader types
and operations declared in `src/IR/Shader.td` and included by `Kelyra.td`.

`--emit-shader-ir --shader-entry=<name>` prints the verified module. It has
records with field names, types, and interface decorations; functions with a
stage, typed arguments, and regions; typed expressions, mutable locals,
branches, loops, calls, returns, and `shader.intrinsic` for target-neutral GPU
functions such as `sqrt` and `pow`.

The shader compiler first runs ordinary Kelyra parsing and semantic analysis,
discovers the entry's reachable functions and records, and builds this module.
Both backends consume the same verified Shader IR, including its stage
interface metadata:

- `--emit-spirv` lowers it to the MLIR SPIR-V dialect, verifies that module,
  and serializes it directly to SPIR-V.
- `--emit-dxil` lowers it to LLVM IR for the DirectX target, emits a DXIL
  container, then uses the DXIL validator to validate and sign that container.
  The validator does not compile HLSL source.

The pinned Kelyra LLVM fork includes the graphics signature fixes needed by
this backend. Its source is checked out under `toolchain/llvm-project/` and
built with the compiler. On Windows, the DXIL validator requires
`dxcompiler.dll` or `dxil.dll` available to the compiler at runtime.

Kelyra clip space uses positive Y upward; the Vulkan vertex wrapper flips Y
to match the Direct3D12 image orientation.

The first supported subset has scalar `f32`, `i32`, `u32`, `bool`, data class
records, local variables, arithmetic, comparisons, `if`, `while`, direct calls,
and value returns. `@gpu_builtin("name")` marks a bodyless shader intrinsic;
its name becomes a `shader.intrinsic` operation and must be a target function
identifier. Shader resources, textures, descriptor bindings, and compute stages
remain future work. A future HLSL source frontend can share this IR and the
binary backends as its type and operation coverage grows; HLSL syntax and
semantics are not part of the current Kelyra shader subset.

Both Windows triangle examples use `kstd/examples/triangle_shaders.kly` as the
single shader source. `kstd/examples/d3d12_triangle.ps1` emits DXIL, while
`kstd/examples/vulkan_triangle.ps1` emits SPIR-V and runs native Vulkan on
Windows. Linux keeps the Vulkan host API and public graphics interface design;
Linux window presentation remains future work.
