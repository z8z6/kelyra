# `meta` 反射设计

> 状态：编译期反射数据库、稳定 `MetaId`、基础 `meta(...)` 语法、
> 注解中的 `meta.type`/`meta.symbol` 实体引用以及 `when` 已经实现。

`meta` 专门表示只读反射元数据，不承担宏展开、AST 修改或编译期函数执行职责：

```text
annotation 声明元数据结构
@annotation(...) 写入元数据
meta(...) 读取反射元数据
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
annotation converter(
  source: meta.type,
  target: meta.type,
  function: meta.symbol,
);

@converter(source = Source, target = Target, function = convert)
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
  Struct,
  Field,
  Parameter,
  Type,
  Annotation,
};
```

所有记录包含名称、限定名、所属模块、可见性、源码位置和注解实例。函数记录还包含参数及返回类型；结构体记录包含字段、大小和对齐；类型记录包含种类、位宽、指向类型、元素类型和数组长度。

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

结构体额外提供：

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

## 编译阶段

1. 解析全部模块；
2. 注册模块和声明并分配 `MetaId`；
3. 解析类型、函数签名和注解声明；
4. 解析注解实例；
5. 将 `meta.type`、`meta.symbol` 转换为 `MetaId`；
6. 完成并冻结反射数据库；
7. 执行 `when` 等编译期查询；
8. 检查函数体并生成 IR。

用户代码不能观察未完成解析的数据库。

## 可见性

当前模块可以反射自己的私有声明。跨模块反射要求模块已导入且声明为 `pub`。构建工具、LSP 和文档工具可以通过宿主 API 请求完整数据库，但语言代码仍然遵循模块可见性。

## 编译期与运行期

`meta(...)` 默认只存在于编译期，不能作为普通函数参数、返回值或局部变量逃逸到运行期。运行期反射必须显式使用 `@retention(runtime)`；ABI 方案见 [运行时反射 ABI](runtime-reflection-abi.md)，当前暂不实现。

## `when`

`when` 在 Sema 中求值，只检查并生成被选中的分支：

```kelyra
when meta(handler).has_annotation(route) && meta(handler).is_public {
  return register(handler);
} else {
  return 0;
}
```

首版条件支持 `true`、`false`、`!`、`&&`、`||`、`==`、`!=`，以及：

- `meta(target).is_public`
- `meta(target).has_annotation(annotation_name)`

条件必须得到编译期 `bool`。普通变量、函数调用和其他运行期表达式会产生错误。逻辑与、逻辑或采用短路求值；未选分支不做名称解析、类型检查或 IR 生成。

`meta` 也不作为函数修饰符。未来编译期执行使用独立的 `comptime` 机制：

- `meta`：读取反射数据；
- `annotation`：写入元数据；
- `when`：编译期条件选择；
- `comptime`：编译期函数执行；
- `@annotation_processor`：注册注解处理器。

## 首轮实现

- [x] 模块、函数、参数、注解声明及基础类型记录；
- [x] 稳定 `MetaId` 和只读数据库；
- [x] 名称、限定名、模块、可见性、位置、函数签名和注解实例；
- [x] `meta(...)` AST；
- [x] `meta.type`、`meta.symbol` 的真实实体解析；
- [x] 编译期限定与跨模块可见性检查。
- [x] `when` 编译期求值和死分支裁剪。

结构体完整布局、AST 修改、局部变量/语句反射、运行时 ABI 和用户处理器留待相应语言阶段实现。
