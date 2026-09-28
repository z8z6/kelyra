# 注解设计

注解成员实现、目标类型绑定与 `+` 组合的方案见
[注解实现与组合方案](annotation-implementation.md)。

> 状态：参数化语法、用户注解声明、类成员实现体、注解组合、模块级名称解析、参数类型检查以及
> `@target`、`@repeatable`、`@retention(Retention.Source|Compile)` 已实现。
> 注解参数使用 [`std.meta`](meta.md) 的具体句柄类型及 `StringSlice`。
> 内建代码生成属性和用户处理器尚未实现。

Kelyra 注解是带类型的编译期元数据。用户注解和编译器内建注解使用同一套声明、名称解析、参数绑定和类型检查机制；内建注解仅额外绑定一个编译器处理器。

## 声明与使用

```kelyra
module web;

import std.util.string;

@target(Target.Function)
@retention(Retention.Compile)
pub annotation route(
  path: StringSlice,
  method: StringSlice = "GET",
);

@target(Target.Function)
@repeatable
pub annotation middleware(
  name: StringSlice,
  priority: i32 = 0,
);

@web.route("/users", method = "POST")
@web.middleware("auth", priority = 100)
fn create_user() -> i32 {
  return 0;
}
```

```ebnf
declaration = { annotation }, [ "pub" ],
              ( function | class | enum | alias-declaration | annotation-declaration ) ;

annotation = "@", qualified-name,
             [ "(", [ annotation-arguments ], ")" ] ;

annotation-arguments = annotation-argument,
                       { ",", annotation-argument }, [ "," ] ;

annotation-argument = [ name, "=" ], annotation-value ;

annotation-declaration = "annotation", name,
                         "(", [ annotation-parameters ], ")",
                         [ "=", annotation, { "+", annotation } ],
                         ( ";" | "{", { class-member | annotation-when }, "}" ) ;

annotation-when = "when", expression, "{",
                  { class-member | annotation-when }, "}",
                  [ "else", ( "{", { class-member | annotation-when }, "}"
                            | annotation-when ) ] ;

annotation-parameters = annotation-parameter,
                        { ",", annotation-parameter }, [ "," ] ;

annotation-parameter = name, ":", type,
                       [ "=", annotation-value ] ;
```

位置参数必须出现在命名参数之前。命名参数不能重复。缺少的可选参数由声明中的默认值填充。参数值必须是编译期常量，支持布尔、字符、整数、浮点数、枚举成员、`StringSlice` 和 `std.meta` 的具体句柄类。句柄参数使用 `meta(...)` 传入；导入 `std.meta` 后，例如函数参数可声明为 `Function`，传入 `meta(handler)`。`Class` 可传给 `Type` 参数，`Symbol` 可接收任一种句柄。
`std.annotation` 显式导入 `std.meta` 和 `std.util.string`。普通项目通过标准库模块搜索路径加载这些依赖；编译器直接调用 Sema 检查单个 AST 时，内建注解声明使用引导路径。

有固定取值的标准注解使用 `std.annotation` 中的枚举：`Layout.System/C`、`CallingConvention.C/System`、`InlineMode.Auto/Always`、`Retention.Source/Compile`。例如 `@layout(Layout.C)`、`@callconv(CallingConvention.C)`。这些值必须写成对应枚举的成员；任意字符串会被类型检查拒绝。

## AST

新增三类节点：

- `ast_annotation_decl`：注解声明；
- `ast_annotation_parameter`：带类型和可选默认值的形参；
- `ast_annotation_argument`：使用处的位置参数或命名参数。

例如：

```kelyra
@web.route("/users", method = "POST")
```

对应：

```text
(Annotation "web.route"
  (AnnotationArgument (Literal "\"/users\""))
  (AnnotationArgument "method" (Literal "\"POST\"")))
```

注解仍然作为目标声明的子节点保存。Sema 提供统一查询接口，后端不得直接解释 AST 中的注解字符串。

## 名称解析与可见性

注解是模块级声明。模块内可以使用短名称，模块外通过限定名使用公开注解：

```kelyra
import serialization;

@serialization.encode("json")
fn save() -> i32 { return 0; }
```

`import module;` 允许直接使用该模块公开注解的短名称；多个导入模块提供同名
注解时，必须在使用处写模块限定名。私有注解只能在其声明模块内使用。

所有模块先注册注解签名，再检查注解实例，因此允许在定义之前使用注解。

## 语义模型

Sema 将声明注册为 `AnnotationDeclInfo`，并把每次使用转换成强类型的 `AnnotationInstance`。实例包含解析后的声明、按形参顺序排列的值和源码位置。目标节点映射到实例列表。

编译器执行以下阶段：

1. 注册模块、函数、结构体和注解声明；
2. 检查注解形参、默认值和元注解；
3. 解析目标上的注解，检查可见性并绑定参数；
4. 检查目标、重复和冲突规则；
5. 执行内建注解处理器；
6. 检查函数体并生成 IR。

IRGen 只读取 Sema 产生的强类型属性，不重新解析注解 AST。

## 元注解

编译器预声明以下元注解：

### `@target`

限制注解允许附着的位置：

```kelyra
@target(Target.Function, Target.Class)
annotation serializable();
```

目标使用 `std.annotation.Target` 枚举，当前支持 `Target.Module`、`Target.Function`、`Target.Class`、`Target.Field`、`Target.Method`、`Target.Constructor`、`Target.Destructor`、`Target.Parameter` 和 `Target.Annotation`。旧的 `@target(class)` 等写法不再接受。局部变量、语句和表达式目标留待后续实现。

`@target(Target.Class T)` 还把目标类的具体类型绑定到注解实现体中的 `T`。
`@target(Target.Field F)` 把目标字段绑定为 `F`：`F.type` 表示字段类型，
`F.name` 表示字段名称。注解体中可用 `get_${F.name}` 这样的标识符插值
生成成员名；普通名称不发生隐式替换。方法体中的 `F` 始终引用该字段，
即使局部变量或参数与字段同名。实现体生成的方法属于字段所在的类。

标识符插值仅用于注解实现体中的名称位置。`${...}` 接收编译期字符串，
可使用字符串字面量、注解字符串参数、目标绑定的 `.name`，以及这些值之间的 `+`。
展开结果必须是合法标识符，重名会像手写声明一样报错。例如
`fn ${"get_" + F.name}()` 与 `fn get_${F.name}()` 生成相同名称。

内置 `@forward` 的声明目标是 `parameter`，目前只允许标注构造函数的编译期参数包，
表示按调用处的值类别转发各个实参。

### `@repeatable`

默认情况下，同一注解在同一目标上只能出现一次。`@repeatable` 允许重复使用。

### `@retention`

- `source`：仅保留在 AST；
- `compile`：保留在 Sema 元数据中，默认值；
- `runtime`：生成运行时元数据，需要单独定义稳定 ABI。

首个实现阶段支持 `source` 和 `compile`；`runtime` 在 ABI 确定前报告不支持。

## 内建注解

已实现的内建注解声明位于标准库的
[`std.annotation`](../../kstd/src/std/annotation/annotation.kly) 模块。
编译器在编译期隐式加载该模块，因此现有 `@cfg`、`@interface`、`@layout`、
`@final`、`@static`、`@virtual`、`@override`、
`@extern`、`@intrinsic`、`@callconv`、`@main`、`@inline`、`@deprecated`、`@reflect`、
`@accessors`、`@aspect`、`@meta`、`@target`、
`@repeatable` 和 `@retention` 短名称无需
显式 `import`。也可以使用 `@std.annotation.interface` 等限定名。
`@singleton` 定义在 `std.sync.singleton`，需要显式导入。
声明和参数由 Kelyra 文件提供；需要在依赖加载之前执行的 `@cfg`，以及
布局、外部符号、接口等内建语义仍由编译器处理。

`@meta` 可标注模块、类、普通函数和方法，表示目标只供编译期使用。
标注普通类的方法时，类及其其他方法仍可在运行期使用。
`std.annotation` 与 `std.meta` 都在源码中标注了它。标注 `@meta` 的模块仍按
普通模块规则加载和检查；它的普通依赖只有在运行期代码需要时才生成实现。
注解注入普通类的成员属于目标类所在模块。

`@intrinsic` 用于 `std.meta` 中无函数体的私有函数声明。省略参数时，
操作名默认为被标注的函数名，例如 `@intrinsic fn __read_public(...)`；
也可显式指定 `@intrinsic("meta.read_public")`。编译器检查操作名和函数签名，
并按操作名执行编译期元数据查询；其他模块不能声明编译器 intrinsic。

- `@inline` 等价于 `@inline(InlineMode.Auto)`，交给优化器决定是否内联；
  `@inline(InlineMode.Always)` 标记强制内联，在无优化构建中也执行强制内联 pass。
- `@deprecated` 或 `@deprecated("改用新 API")` 标记函数或方法；使用处输出
  编译警告，不中止编译。
- `@reflect` 可以标记类或实例字段。标记类时，选中全部公开实例字段，
  以及额外标记 `@reflect` 的私有实例字段；只标记字段时，仅选中所标记的
  字段。接口常量不是实例字段，不能使用 `@reflect`。被选字段会进入
  逐类型运行时描述符，可通过 `std.reflect` 查询和读写；详见
  [`kstd` 运行时反射说明](../../kstd/doc/reflection.md)。

### 字段访问方法

`@accessors` 标注可写字段，生成公开的 `get_<字段名>()` 与
`set_<字段名>(value)` 方法。`@accessors(get = false)` 或
`@accessors(set = false)` 可只生成其中一种；静态字段生成静态方法。
原字段的可见性不变，同名方法冲突仍按普通成员声明报错。

```kelyra
class Counter {
  @accessors
  value: i32;
}

let counter = Counter();
counter.set_value(42);
let answer = counter.get_value();
```

### 函数切面

`@aspect(meta(handler))` 可重复标注有函数体的普通函数或方法。`handler` 是普通
Kelyra 函数，不需要声明为注解。需要显式 `import std.aspect;`。
多个切面按源码顺序从外到内调用；`proceed()` 每调用一次就执行一次下层函数。

```kelyra
import std.aspect;

fn traced<R>(call: *std.aspect.Invocation<R>) -> R {
  log("enter");
  let result = call.proceed();
  log("exit");
  return result;
}

@aspect(meta(traced))
fn calculate(value: i32) -> i32 { return value + 1; }
```

返回 `void` 的函数使用 `*std.aspect.VoidInvocation`，其 `proceed()` 也返回
`void`。`Invocation` 只在本次切面调用期间有效，不得保存供之后使用。
当前函数值不能按值返回类对象或多个结果，因此这些返回类型暂不支持切面；
函数类型的参数和返回值遵循现有函数值规则。

后续计划提供：

- `@export([name])`；
- `@link_name(name)`；
- 更多标准库 intrinsic 操作。

`pub` 控制 Kelyra 模块可见性，`@export` 控制目标文件符号，二者不能混为一谈。
`@intrinsic` 使用稳定的 Kelyra 操作名，不暴露 LLVM 名称。

## 用户处理器

没有实现体和组合式的用户注解只保存元数据；类注解已经可以通过实现体注入成员。
IDE、文档工具和编译期程序可以通过 `meta(...)` 读取有效注解。
更一般的声明生成和诊断处理器留待独立的 `comptime` 机制：

```kelyra
@annotation_processor(web.route)
comptime fn process_route(target: meta.declaration, value: web.route) {
  // 校验或生成模块级声明
}
```

处理器初期只允许报告诊断和生成新的模块级声明，不允许任意修改已经完成类型检查的 AST。生成声明需要重新进入注册和检查阶段，并受递归展开上限约束。

## 实现阶段

1. ~~参数化注解、注解声明、AST、格式化和解析测试；~~
2. ~~两阶段注解注册、限定名称解析、可见性、参数绑定、常量类型检查和
   `std.meta` 句柄实体解析；~~
3. ~~`@target`、`@repeatable`、`@retention(source|compile)`；~~
4. ~~`@target(Target.Class T)`、`+` 组合与类成员实现体；~~
5. `@export` 及诊断严重级别；
6. 扩展标准库需要的 `@intrinsic` 操作；
7. 通用编译期用户处理器和运行时保留 ABI。
