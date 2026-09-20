# 运行时反射 ABI 方案

> 状态：设计草案，暂未实现。编译器当前不会接受
> `@retention(runtime)`，也不会生成运行时反射段或 ABI 符号。

## 目标与边界

运行时反射只导出显式标记为 `runtime` 保留的注解及其所需声明，不默认暴露整个程序。ABI 不包含 AST 指针、LLVM 类型、宿主指针或 C++ 对象布局，因此元数据可以被不同语言的运行库读取。

首版只提供读取能力：枚举记录、读取名称和类型关系、读取注解及参数。动态调用、字段读写、对象构造和 ABI 自动封送不属于反射 ABI。

## 生成入口

后续实现接受：

```kelyra
@retention(runtime)
annotation route(path: meta.string);
```

链接产物导出两个只读符号：

```c
#include <stdint.h>

extern const unsigned char __kelyra_reflection_v1[];
extern const uint32_t __kelyra_reflection_v1_size;
```

无运行时元数据时不导出符号。多个 Kelyra 对象文件由最终链接步骤合并为一个 blob；普通系统链接器不负责合并记录。

## 编码约定

- 所有整数均为无符号小端编码；
- 所有引用均为从 blob 起点计算的 `u32` 字节偏移，不使用指针；
- 无效记录编号和偏移统一为 `0xffffffff`；
- 字符串采用 UTF-8，记录为 `u32 length` 加原始字节，不要求 NUL 结尾；
- 表按源码确定顺序写入，保证相同输入生成相同字节；
- 读取器必须使用 `total_size` 检查所有偏移和长度后才能访问数据。

## 头部

```text
offset  size  field
0       4     magic = "KLRM"
4       2     abi_major = 1
6       2     abi_minor = 0
8       4     total_size
12      4     flags
16      4     record_count
20      4     record_offset
24      4     annotation_count
28      4     annotation_offset
32      4     argument_count
36      4     argument_offset
40      4     id_count
44      4     id_offset
48      4     dimension_count
52      4     dimension_offset
56      4     string_size
60      4     string_offset
```

`abi_major` 不兼容时读取器必须拒绝；`abi_minor` 增加只允许使用已有长度字段可跳过的向后兼容扩展。首版 `flags` 为零。

## 固定记录

每条声明记录固定为 64 字节：

```text
u32 kind
u32 flags                 // bit 0: public
u32 name                  // 字符串偏移
u32 qualified_name        // 字符串偏移
u32 module                // 运行时记录编号
u32 type                  // 运行时记录编号
u32 symbol                // 字符串偏移
u32 child_first           // id 表索引
u32 child_count
u32 annotation_first      // annotation 表索引
u32 annotation_count
u32 type_kind
u32 bit_width
u32 pointer_depth
u32 dimension_first       // dimension 表索引
u32 dimension_count
```

`kind` 和 `type_kind` 的数值由 ABI 文档分配，不能直接使用编译器 C++ `enum` 的底层值。`id` 表元素为 `u32` 运行时记录编号，`dimension` 表元素为 `u64`。

注解记录固定为 12 字节：

```text
u32 name
u32 argument_first
u32 argument_count
```

注解参数记录固定为 16 字节：

```text
u32 name
u32 value_kind
u32 text
u32 reference            // 运行时记录编号
```

标量值保留规范化文本；`meta.type` 和 `meta.symbol` 同时填写 `reference`。未知的 `value_kind` 必须按记录长度跳过。

## 保留闭包与编号

编译器先选择包含至少一个 `runtime` 注解的目标，然后加入这些目标所引用的模块、类型、子项，以及注解参数中的 `meta.type`/`meta.symbol` 目标。闭包之外的声明不进入 blob。最终记录重新按确定顺序编号，不能把编译期 `MetaId` 直接写入文件。

私有声明只有在显式使用运行时保留注解或被闭包引用时才会导出。编译器应对此给出可配置警告，因为导出的限定名和符号名可能泄露实现信息。

## 兼容性与校验

运行库至少检查 magic、主版本、`total_size`、各表乘法溢出、表边界、字符串边界和所有记录编号。损坏的 blob 必须整体拒绝，不能部分解释。

ABI v1 冻结后，仅允许在尾部增加新表或使用已有保留位。修改现有字段含义、大小、字节序或编号含义必须提升 `abi_major` 并使用新的导出符号名。

## 实现顺序

1. 接受 `@retention(runtime)` 并在注解实例中保存保留级别；
2. 从现有 `ReflectionDatabase` 计算运行时闭包并重编号；
3. 编码并严格自校验 blob；
4. 在最终链接阶段合并元数据并导出两个符号；
5. 提供只依赖整数和字节切片的 C 读取 API；
6. 增加跨编译器版本的 ABI fixture 测试。
