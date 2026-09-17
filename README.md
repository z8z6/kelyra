# Kelyra

Kelyra is a neo lang for myself.

## 构建

需要 CMake 3.20+、支持 C++17 的编译器、Python 3 和 Ninja（也可使用其他 CMake 生成器）。
LLVM 和 MLIR 使用仓库子模块中的同一版本，从源码一起构建，无需预先安装。

```sh
git submodule update --init --recursive
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target kelyra --parallel 4
ctest --test-dir build --output-on-failure
./build/bin/kelyra
```

当前入口仅创建并打印空的 MLIR `module`，用于验证 LLVM/MLIR 的头文件、生成文件和链接依赖。
首次构建需要编译依赖，耗时较长；内存不足时降低 `--parallel`。
`build/compile_commands.json` 可供 clangd 使用。

默认仅启用本机 LLVM 后端，关闭 LLVM/MLIR 自身的测试和 LLVM benchmarks。
可通过 `-DLLVM_TARGETS_TO_BUILD="X86;AArch64"` 等标准 LLVM 选项调整；
`LLVM_ENABLE_PROJECTS` 必须包含 `mlir`。详见 [MLIR 构建文档](https://mlir.llvm.org/getting_started/)。
默认构建只编译 Kelyra 及其依赖；需要 MLIR 命令行工具时运行
`cmake --build build --target mlir-opt --parallel 4`。
