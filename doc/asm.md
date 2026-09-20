# `asm` 语法设计

> 状态：已实现语法、语义检查和 LLVM inline asm lowering。

`asm` 由不加引号的原始汇编块和链式约束组成。`{name}` 是由编译器替换的
寄存器占位符：

```kelyra
let result: i64 = left;

asm {
  add {result}, {right}
}
.in(result)
.in(right)
.out(result)
.op(intel, nomem, nostack);
```

`result` 同时是输入和输出，因此编译器为两者分配同一个寄存器。`right`
只是输入。汇编块中不需要字符串、位置编号或 GCC 约束字符串。

## 语法

```ebnf
asm          = "asm", raw-block, { asm-chain }, ";" ;
asm-chain    = ".", "in", "(", binding, ")"
             | ".", "out", "(", binding, ")"
             | ".", "op", "(", option, { ",", option }, ")" ;
binding      = name, [ ",", register ] ;
register     = name ;
option       = name | "clobber", "(", register, { ",", register }, ")" ;
```

`raw-block` 是 `{` 与配对的 `}` 之间的原始文本。编译器只识别其中的
`{name}` 占位符，其余内容原样交给当前 target 的 LLVM 集成汇编器。
`{{` 和 `}}` 表示汇编文本中的字面大括号。

## 自动寄存器分配

- `.in(value)` 把 Kelyra 变量 `value` 放入自动分配的寄存器。
- `.out(result)` 在汇编结束后把自动分配的寄存器写回 `result`。
- 同一名称同时出现在 `.in` 和 `.out` 中时，两者是 tied operand，因而无需
  `.inout`。
- 寄存器类别由操作数类型和目标架构决定：整数和指针使用通用寄存器，
  浮点值使用对应浮点或向量寄存器。
- `.out` 的名称必须指向可赋值变量。

所有占位符都必须在 `.in` 或 `.out` 中声明；自动分配的绑定也必须在
汇编块中被引用。固定寄存器绑定可以不写占位符，因为指令本身已经指定了寄存器。

## 固定寄存器

第二个参数用于要求架构定义的固定寄存器。例如 x86-64 `rdtsc`：

```kelyra
let low: u32 = 0;
let high: u32 = 0;

asm {
  rdtsc
}
.out(low, eax)
.out(high, edx)
.op(nomem, nostack);
```

这表示：

```text
low  <- eax
high <- edx
```

固定寄存器也可用于输入：

```kelyra
asm {
  syscall
}
.in(number, rax)
.in(argument, rdi)
.out(result, rax)
.op(nostack, clobber(rcx, r11, cc));
```

`rax` 同时出现在输入和输出约束中，所以它是 tied operand。

## `.op`

`.op` 声明方言、副作用和优化约束：

- `nomem`：不读也不写未通过操作数声明的内存。
- `nostack`：不使用栈，也不修改栈指针。
- `preserves_flags`：不修改目标架构的条件标志。
- `intel`：在 x86 上使用 Intel 语法；其他架构拒绝该选项。
- `clobber(rcx, r11, cc)`：声明除 `.out` 外还会被破坏的寄存器或状态。

已出现在 `.out` 中的寄存器不再写入 `clobber`。默认情况下，编译器认为
汇编有副作用、可能读写内存且可能改变标志位。

Kelyra 不提供 `unsafe` 或 `mut` 门槛。错误的指令、约束、副作用或栈规则
可以直接导致未定义行为，责任由程序员承担。

## 实现边界

首版 lowering 到 LLVM inline asm。编译器必须校验：

- 每个占位符都有对应的输入或输出绑定；
- 固定寄存器适用于当前 target；
- 输入输出类型与寄存器宽度兼容；
- 输出可写，互斥选项没有同时出现。

首版不支持 `asm goto`、标签操作数、裸函数或跨架构自动翻译指令。
