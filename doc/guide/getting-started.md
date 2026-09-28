# 安装与第一个程序

这页从仓库源码开始。当前项目没有承诺稳定的预编译发行版，因此先使用仓库自带的构建流程。

## 准备源码

需要 Git、CMake、C++ 工具链和 Ninja 或其他 CMake 生成器。LLVM 源码作为子模块随仓库固定版本。Windows 请在 Visual Studio Developer PowerShell 中运行构建命令，以便编译器找到 MSVC 与 Windows SDK。

```sh
git clone --recurse-submodules git@github.com:z8z6/kelyra.git
cd kelyra
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

如果已经浅克隆了主仓库，还需要初始化子模块：

```sh
git submodule update --init --recursive
```

完整构建会编译项目固定的 LLVM，因此第一次需要一些时间。

## 编译并运行

先看一个最小程序。`@main` 标记入口，入口没有参数并返回 `i32`。退出码 `42` 是示例输出，不会自动打印到终端。

<<< ../../compiler/tests/cli/main.kly{kelyra}

在仓库根目录执行：

```powershell
.\build\bin\kelyra.exe --emit-exe -o hello.exe compiler\tests\cli\main.kly
.\hello.exe
$LASTEXITCODE
```

Linux 上将可执行文件路径改为 `./build/bin/kelyra`，输出文件可命名为 `hello`。

## 打印文字

想看到真正的文字输出，可以使用 `std.io`：

```kelyra
import std.io;

@main
fn main() -> i32 {
  std.io.println("Hello, Kelyra!");
  return 0;
}
```

保存为 `hello.kly`，再编译并运行：

```powershell
.\build\bin\kelyra.exe --emit-exe -o hello.exe hello.kly
.\hello.exe
```

`import std.io;` 会从配置好的标准库目录加载模块。更完整的输入输出示例见[仓库源码](https://github.com/z8z6/kelyra/blob/main/kstd/examples/io_example.kly)。

## 下一步

使用 [Kelp](./kelp.md) 管理多文件项目；或者从[语法速览](/reference/syntax.md)继续了解函数、条件与类型。
