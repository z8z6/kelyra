# C 互操作

Kelyra 可以导入 C 头文件，也能直接声明外部符号。这里是靠近系统的一层，需要格外留意 ABI 与指针有效期。

## 导入 C 头文件

以下示例面向提供 `getpid` 的目标平台：

```kelyra
import c "print.h";
import c;

@main
fn main() -> i32 {
  print();
  return 0;
}
```

编译器使用 Clang 解析 C 声明。C 实现文件可通过 `--c-source` 一同编译，已有目标文件或库可用 `--link-input` 链接。

## 声明外部函数

```kelyra
@extern("getpid", "c")
@callconv(CallingConvention.C)
fn process_id() -> c.int;
```

`@extern` 的第二个参数是链接库名，不是头文件名。Windows 上可填写导入库基础名，例如 `"user32"`。具体库名随目标平台而异。

## C 布局

`@layout(Layout.C)` 可让类字段采用宿主平台 C 结构体布局，也支持函数指针字段。嵌套类也需要适当布局标记。C 头文件中的联合体可由 C 导入机制解析；手写 Kelyra 联合体的语法仍需以当前编译器实现为准。

当前能力仍有边界：外部函数声明主要面向顶层函数；某些按值结构体和可变参数调用需要 Clang ABI thunk。完整规则见[前端文档](https://github.com/z8z6/kelyra/blob/main/compiler/doc/frontend.md)。
