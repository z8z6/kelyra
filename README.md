# Kelyra

This repository contains the Kelyra compiler, standard library, Kelp package
manager, and Kide editor tooling. The project-maintained LLVM fork is pinned as
a source submodule under `toolchain/llvm-project/`.

| Directory | Purpose |
| --- | --- |
| `compiler/` | Kelyra parser, semantics, Shader IR, code generation, runtime, and tests |
| `kstd/` | Standard library and platform graphics examples |
| `kelp/` | Package manager |
| `kide/` | Editor extensions and language grammar |
| `toolchain/llvm-project/` | Pinned [Kelyra LLVM fork](https://github.com/z8z6/kelyra-llvm) |

Clone with submodules, configure from the repository root, then build the
compiler and package manager:

```sh
git clone --recurse-submodules git@github.com:z8z6/kelyra.git
cd kelyra
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
npm ci --prefix kide
npm test --prefix kide
```

On Windows, configure from a Visual Studio developer environment so Clang can
find the MSVC and Windows SDK libraries. Generated files belong in `build/`,
`kstd/.kelp/`, or `kide/node_modules/` and are not committed.

Shader architecture and current HLSL compatibility limits are documented in
[`compiler/doc/shader-ir.md`](compiler/doc/shader-ir.md).
