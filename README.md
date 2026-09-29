# Kelyra

This repository contains the Kelyra compiler, standard library, Kelp package
manager, and Kide editor tooling. The project-maintained LLVM fork is pinned as
a source submodule under `kelyra-llvm/`.

| Directory | Purpose |
| --- | --- |
| `compiler/` | Kelyra parser, semantics, Shader IR, code generation, runtime, and tests |
| `kstd/` | Standard library and platform graphics examples |
| `kelp/` | Package manager |
| `kide/` | Editor extensions and language grammar |
| `doc/` | Kelyra language website and exportable documentation |
| `kelyra-llvm/` | Pinned [Kelyra LLVM fork](https://github.com/z8z6/kelyra-llvm) |

Clone with submodules, configure from the repository root, then build the
compiler and package manager:

```sh
git clone --recurse-submodules git@github.com:z8z6/kelyra.git
cd kelyra
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
npm ci --prefix kide/vscode
npm test --prefix kide/vscode
```

On Windows, configure from a Visual Studio developer environment so Clang can
find the MSVC and Windows SDK libraries. Generated files belong in `build/`,
`kstd/.kelp/`, or `kide/vscode/node_modules/` and are not committed.

Shader architecture and current HLSL compatibility limits are documented in
[`compiler/doc/shader-ir.md`](compiler/doc/shader-ir.md).

The public language site lives in `doc/`. Run `npm ci --prefix doc` and
`npm run dev --prefix doc` to preview it locally; `npm run build --prefix doc`
creates the static site and downloadable Markdown pages in
`doc/.vitepress/dist/`. The GitHub Pages workflow builds it at the root of
the `kelyra.io` custom domain. Article PDF export uses the browser's
print-to-PDF flow.
