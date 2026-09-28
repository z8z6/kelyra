# 语法速览

这页是一张路线图，不要求你一次记住全部语法。遇到感兴趣的部分，点进对应章节看更具体的规则。

## 源文件与入口

Kelyra 源文件使用 `.kly`。一个文件对应一个模块；未写 `module` 时，也可以编写简单的单文件程序。

```kelyra
@main
fn main() -> i32 {
  return 0;
}
```

`@main` 入口必须是无参数、返回 `i32` 的函数。程序里只能有一个这样的入口。

## 变量与表达式

`let` 至少给出类型或初始值。类型明确时写 `let count: i32 = 3;`，可推导时写 `let count = 3;`。

```kelyra
fn square(value: i32) -> i32 {
  let result = value * value;
  return result;
}
```

函数参数类型需要写出。当前没有隐式数值提升；跨数值类型时使用显式 `as` 转换。

## 常用结构

| 代码 | 作用 | 继续阅读 |
| --- | --- | --- |
| `if condition { ... }` | 条件分支 | [控制流](./control-flow.md) |
| `while condition { ... }` | 条件循环 | [控制流](./control-flow.md) |
| `for item in values { ... }` | 遍历迭代器 | [控制流](./control-flow.md) |
| `match value { ... }` | 按值选择分支 | [控制流](./control-flow.md) |
| `class Name { ... }` | 定义类型与行为 | [类与接口](./classes.md) |
| `module app.main;` | 声明模块 | [模块、注解与平台](./modules.md) |
| `import std.io;` | 导入模块 | [模块、注解与平台](./modules.md) |

## 语法边界

Kelyra 仍在开发。比如构造函数重载、同名泛型函数模板、闭包和 `match` 守卫尚未支持。需要精确规则时，可参考[当前前端文档](https://github.com/z8z6/kelyra/blob/main/compiler/doc/frontend.md)和对应的编译器测试。
