# Kelyra

Kelyra is a neo lang for myself.

## 语言介绍

直接打开 [doc/index.html](doc/index.html) 阅读中文介绍。

文档通过 `.github/workflows/pages.yml` 发布到 GitHub Pages。
首次发布需在仓库 **Settings → Pages → Build and deployment → Source** 中选择 **GitHub Actions**。
推送 `doc/` 或工作流的修改到 `main` 后自动部署，也可在 Actions 中手动运行。
默认站点地址为 <https://z8z6.github.io/kelyra/>，实际地址以部署结果为准。

## 构建

Kelyra 源文件默认使用 `.kly` 后缀。

需要 CMake 3.20+、支持 C++20 的编译器、Python 3 和 Ninja（也可使用其他 CMake 生成器）。
LLVM 和 MLIR 使用仓库子模块中的同一版本，从源码一起构建，无需预先安装。

```sh
git submodule update --init --recursive
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target kelyra-ls kelyra-format kelyra_frontend_tests kelyra_sema_tests kelyra_ir_tests kelyra_codegen_tests kelyra_cli_tests --parallel 4
ctest --test-dir build --output-on-failure
./build/bin/kelyra-format -i examples/basic.kly
./build/bin/kelyra --emit-mlir tests/cli/add.kly
./build/bin/kelyra --emit-obj -o add.o tests/cli/add.kly
./build/bin/kelyra --emit-exe -o main tests/cli/main.kly
```

`kelyra-format -i file.kly` 原地格式化源码；`kelyra-format --check file.kly`
仅检查格式，适合 CI。不加选项时，格式化结果写到标准输出。格式化器复用 Kelyra
自己的词法器，并在修改文件前拒绝有语法错误的输入。

`kelyra-ls` 是复用同一词法器与语法树的语言服务器，提供诊断、悬浮提示、补全和
定义跳转；编辑器通过标准输入输出上的 LSP 协议启动它。

默认使用 `-O0`：不运行优化并在目标文件中保留调试信息。`-O1`、`-O2` 和
`-O3` 依次启用更高的 LLVM 优化级别。

`--emit-mlir` 对支持的源程序进行名称与类型检查，并生成标准 MLIR：

```mlir
module {
  func.func @add(%arg0: i32, %arg1: i32) -> i32 {
    %0 = arith.addi %arg0, %arg1 : i32
    return %0 : i32
  }
}
```

`src/IR/Kelyra.td` 定义项目自有 dialect；当前源程序生成使用 `func`、`arith`、`cf` 和 LLVM 标准 dialect。内置类型包括 `i8/i16/i32/i64/i128`、`u8/u16/u32/u64/u128`、`f32/f64/f128/f256/f512`、`bool` 和 `char`。`--emit-obj` 经 LLVM dialect 和 LLVM IR 生成本机目标文件；`--emit-exe` 再调用系统 `cc` 链接可执行文件，入口必须为 `fn main() -> i32`。`f256/f512` 使用 Kelyra 自有类型，目前只支持函数签名和透传，不能生成目标文件；其余数值类型支持当前算术子集。

定长数组支持多维声明和索引读写，例如 `i32[8][8]` 与
`board[row][column]`；未显式初始化的局部数组会清零。可执行的八皇后示例位于
`examples/n_queens.kly`，程序以返回值 `92` 表示解的数量。
安全级别默认为 `0`，不插入运行时检查；使用 `--safe-level=1` 或更高级别时，
普通数组访问会进行逐维越界检查。

模块按文件组织，入口文件所在目录为模块根。`import math.vector;` 加载
`math/vector.kly`；被导入文件必须声明 `module math.vector;`。声明默认私有，使用
`pub fn` 才能供其他模块通过完整名称调用。

使用 `import c "header.h";` 可让 Clang 展开并解析 C 头文件，声明统一位于 `c`
模块。`--c-source=file.c` 使用 Clang 编译 C 实现并参与可执行文件链接，例如：

普通代码应优先使用 Kelyra 类型；同位宽、同符号性的 Kelyra/C 数值标量可在 FFI
边界直接传递，例如 `i64` 与 `c.longlong`。

```sh
./build/bin/kelyra --emit-exe --c-source=tests/cli/c/print.c \
  -o print-example tests/cli/c/main.kly
```

```sh
./build/bin/kelyra --check examples/basic.kly
./build/bin/kelyra --dump-ast examples/basic.kly
./build/bin/kelyra --emit-mlir tests/cli/add.kly
./build/bin/kelyra --emit-obj -o add.o tests/cli/add.kly
./build/bin/kelyra --emit-exe -o main tests/cli/main.kly
```

语法范围、诊断与独立测试方式见 [基础前端说明](doc/frontend.md)。
首次构建需要编译依赖，耗时较长；内存不足时降低 `--parallel`。
`build/compile_commands.json` 可供 clangd 使用。

测试使用独立的 [GoogleTest](https://github.com/google/googletest) v1.17.0 子模块（`third_party/googletest`），不链接 LLVM 自带的 GoogleTest。`git submodule update --init --recursive` 会取得固定版本；CMake 配置时不下载依赖。使用 `-DBUILD_TESTING=OFF` 时无需 GoogleTest。

可直接运行 GoogleTest，也可通过 CTest 运行自动发现的测试：

```sh
./build/tests/frontend/kelyra_frontend_tests
./build/tests/sema/kelyra_sema_tests
./build/tests/ir/kelyra_ir_tests
./build/tests/codegen/kelyra_codegen_tests
./build/tests/cli/kelyra_cli_tests
ctest --test-dir build --output-on-failure
```

默认仅启用本机 LLVM 后端，关闭 LLVM/MLIR 自身的测试和 LLVM benchmarks。
可通过 `-DLLVM_TARGETS_TO_BUILD="X86;AArch64"` 等标准 LLVM 选项调整；
`LLVM_ENABLE_PROJECTS` 必须包含 `mlir;clang`。详见 [MLIR 构建文档](https://mlir.llvm.org/getting_started/)。
默认构建只编译 Kelyra 及其依赖；需要 MLIR 命令行工具时运行
`cmake --build build --target mlir-opt --parallel 4`。
