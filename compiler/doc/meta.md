# `meta` 反射设计

> 状态：编译期反射数据库、稳定 `MetaId`、基础 `meta(...)` 语法、
> 注解中的 `std.meta` 具体句柄类型检查及 `when` 已经实现；
> 标量编译期求值器与 `meta { ... }` 已接入，复合值和反射集合遍历仍待实现。

`meta(...)` 表示只读反射句柄；`meta { ... }` 执行编译期表达式：

```text
annotation 声明元数据结构
@annotation(...) 写入元数据
meta(...) 读取反射元数据
meta { ... } 执行编译期求值
```

## 语法

```kelyra
meta(User)
meta(parse)
meta(app.main)
meta(*u8)
```

```ebnf
meta-expression = "meta", "(", meta-target, ")" ;
meta-target     = qualified-name | type ;
```

`meta(...)` 是编译期操作符，不是普通函数。目标必须解析为模块、声明或类型。它产生不可逃逸到运行期的元数据句柄。

注解可以保存真实反射引用：

```kelyra
import std.meta;

annotation converter(
  source: Type,
  target: Type,
  function: Function,
);

@converter(source = meta(Source), target = meta(Target), function = meta(convert))
fn register_converter() -> i32 { return 0; }
```

`Source`、`Target` 和 `convert` 必须在 Sema 中解析成稳定 `MetaId`，不能保存成字符串。

## 元数据模型

反射数据库使用稳定编号，不向语言层暴露 AST 指针、LLVM 类型或编译器地址：

```cpp
using MetaId = std::uint32_t;

enum class MetaKind {
  Module,
  Function,
  Class,
  Method,
  Constructor,
  Destructor,
  Field,
  Parameter,
  Type,
  Annotation,
};
```

所有记录包含名称、限定名、所属模块、可见性、源码位置和注解实例。函数记录还包含参数及返回类型；class 记录包含字段、方法、大小和对齐；类型记录包含种类、位宽、指向类型、元素类型和数组长度。

公开查询通过只读 `ReflectionDatabase` 完成：

```cpp
MetaId GetId(const lex::Node &Node) const;
const MetaDeclaration &Get(MetaId Id) const;
```

数据库中的记录顺序必须确定：模块按编译输入顺序，声明、参数和字段按源码顺序。记录一旦完成解析便冻结，后续阶段只能读取。

## 反射视图

公共声明属性：

```text
name
qualified_name
kind
module
visibility
location
annotations
```

函数额外提供：

```text
parameters
return_type
symbol
```

class 的规划视图额外提供：

```text
fields
size
alignment
layout
```

类型额外提供：

```text
type_kind
bit_width
pointee
element
array_length
```

多返回值的类型记录使用 `MetaTypeKind::Results`，`Children` 按返回顺序引用各项类型。
无返回值函数（省略返回类型或 `-> void`）统一记录为 `void`。
函数值类型使用 `MetaTypeKind::Function`，`Children` 是参数类型，`Type` 是返回类型。

## 编译阶段

1. 解析全部模块；
2. 注册模块和声明并分配 `MetaId`；
3. 解析类型、函数签名和注解声明；
4. 解析注解实例；
5. 将 `std.meta` 句柄转换为 `MetaId`；
6. 完成并冻结反射数据库；
7. 执行 `when` 等编译期查询；
8. 检查函数体并生成 IR。

用户代码不能观察未完成解析的数据库。

## 可见性

当前模块可以反射自己的私有声明。跨模块反射要求模块已导入且声明为 `pub`。构建工具、LSP 和文档工具可以通过宿主 API 请求完整数据库，但语言代码仍然遵循模块可见性。

## 编译期与运行期

`meta(...)` 默认只存在于编译期，不能作为普通函数参数、返回值或局部变量逃逸到运行期。
当前运行期字段反射由 `@reflect` 选择；`@retention(std.annotation.Retention.Runtime)` 的通用注解保留仍处于草案阶段。
两阶段的目标边界见[编译期元数据与运行期反射](reflection-architecture.md)。

## `when`

`when` 在 Sema 中求值，只检查并生成被选中的分支：

```kelyra
when meta(handler).has_annotation(meta(route)) && meta(handler).is_public() {
  return register(handler);
} else {
  return 0;
}
```

首版条件支持 `true`、`false`、`!`、`&&`、`||`、`==`、`!=`，以及：

- `meta(target).is_public()`
- `meta(target).has_annotation(meta(annotation_name))`

条件必须得到编译期 `bool`。普通运行期变量和函数调用会产生错误；
`std.meta` 的编译期查询方法可用。逻辑与、逻辑或采用短路求值；
未选分支不做名称解析、类型检查或 IR 生成。

当前标准库的 [`std.meta`](../../kstd/src/std/meta/meta.kly) 已声明
`Symbol`、`Type`、`Class`、`Field`、`Function`、`Parameter`，
这些类继承 `Symbol` 的 `id`、`name: std.util.string.StringSlice` 和基础查询方法；
`Class` 还继承 `Type`。`Annotation` 也是独立的 `Symbol` 子类。
`Field.owner`、`Function.owner`、`Parameter.owner` 分别为 `*Class`、
`*Symbol`、`*Function`。`Class` 提供 `has_constructor()`、
`has_default_constructor()`、`has_destructor()`、`has_base_class()`、
`is_interface()` 和 `is_final()`；前者只判断显式声明的构造函数，
默认构造函数查询也包含编译器生成的构造函数。
成员集合遍历接口暂不提供；编译期求值器目前不能执行反射集合遍历。
现在可在 `when` 中使用 `Class.has_member(name)`、`has_field(name)` 和
`has_function(name)` 查询当前类直接声明的成员。参数是字符串字面量；
`has_function` 包含方法、构造函数和析构函数。跨模块查询仅计入公开成员。
`Field.is_static()` 以及 `Function.is_static()`、`is_method()`、
`is_constructor()`、`is_destructor()` 也可用于 `when`。例如：

```kelyra
when meta(User).has_field("name") &&
     !meta(User.name).is_static() {
  // User 声明了实例字段 name
}
```

成员集合暂不能在 `when` 中遍历，其他元信息字段（例如名称、偏移和类型）
也还没有通用的编译期求值支持。注解参数会按具体句柄类型检查，
但普通表达式尚不能保存或传递 `meta(...)` 句柄。
运行期描述符已经归入
[`std.reflect`](../../kstd/src/std/reflect/reflect.kly)，两阶段的类彼此独立。
编译器仅提供 `meta(...)` 句柄与私有元数据读取原语。原语在标准库中以
无函数体的 `@intrinsic` 函数声明；未指定操作名时使用函数名。编译期求值器
执行标准库方法的普通 Kelyra 函数体，只在调用私有原语时按其注解操作名读取
宿主元数据。
如果本次编译加载了 `std.meta` 模块，查询以加载的模块定义为准；
直接调用 Sema 而没有提供该模块时，才使用构建时嵌入的定义。

`meta(...)` 读取反射对象；`meta { ... }` 执行编译期块并产生常量。
`@meta` 标在模块、类或函数上，限制声明仅供编译期使用。例子：

```kelyra
@meta
fn twice(value: i32) -> i32 { return value * 2; }

let count = meta { let base = twice(3); base + 1 };
```

编译期块按普通 Kelyra 规则检查类型，然后解释局部变量、赋值、`if`、
`when`、`while`、返回语句及函数调用。解释器限制执行步数和调用深度。
在编译期块中使用 `meta(...)` 的属性或方法时，按普通模块规则导入 `std.meta`。
目前可向运行期输出布尔、整数、浮点字面量和字符串常量；反射句柄不能作为
运行期常量。枚举判别值、整数注解参数、名称插值和普通 `when` 已开始共用
标量求值器。数组长度现在可写编译期整数表达式，如 `[1 + 2]i32`。
注解展开阶段的反射查询仍受原始成员快照和当前接口限制。

## 首轮实现

- [x] 模块、函数、参数、注解声明及基础类型记录；
- [x] 稳定 `MetaId` 和只读数据库；
- [x] 名称、限定名、模块、可见性、位置、函数签名和注解实例；
- [x] `meta(...)` AST；
- [x] 注解参数中的 `std.meta` 具体句柄类型检查和实体解析；
- [x] 编译期限定与跨模块可见性检查。
- [x] `when` 编译期求值和死分支裁剪。

class、字段、方法、构造/析构记录及内部布局已实现；这些布局信息通过宿主数据库查询，尚未开放语言层的 `size` 等属性。

AST 修改、局部变量/语句反射、运行时 ABI 和用户处理器留待相应语言阶段实现。
