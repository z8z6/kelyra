# 标准库概览

标准库源码位于仓库 `kstd/src/`，由普通 Kelyra 代码构成。按用途选择模块即可，编译器会通过 `import` 加载依赖。

## 从输入输出开始

`std.io` 提供打印、换行和读取。这里直接展示仓库中的示例：

<<< ../../kstd/examples/io_example.kly{kelyra}

## 常见方向

| 用途 | 入口 |
| --- | --- |
| 输入输出 | `std.io` 与 `std.io.file` |
| 数学 | `std.math.basic` 与 `std.math.integer` |
| 迭代与可选值 | `std.util.iterator`、`std.util.maybe` |
| Windows 系统接口 | `std.win` 下按功能划分的模块 |
| UI 组件 | `std.ui` |
| 图形与 Shader 数据 | `std.graphics` |

标准库仍在完善；公共 API 请以[当前源码](https://github.com/z8z6/kelyra/tree/main/kstd/src)和编译测试为准。

## 探索示例

[标准库示例目录](https://github.com/z8z6/kelyra/tree/main/kstd/examples)包含数学、字符串、线程、UI、DX12 与 Vulkan 示例。图形示例需要相应平台的驱动、SDK 和本机库。
