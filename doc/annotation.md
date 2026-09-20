# 注解设计

> 状态：参数化语法、用户注解声明、模块级名称解析、参数类型检查以及
> `@target`、`@repeatable`、`@retention(source|compile)` 已实现。
> `meta.symbol`/`meta.type` 已由 [`meta` 反射系统](meta.md)提供真实实体引用。
> 内建代码生成属性和用户处理器尚未实现。

Kelyra 注解是带类型的编译期元数据。用户注解和编译器内建注解使用同一套声明、名称解析、参数绑定和类型检查机制；内建注解仅额外绑定一个编译器处理器。

## 声明与使用

```kelyra
module web;

@target(function)
@retention(compile)
pub annotation route(
  path: meta.string,
  method: meta.string = "GET",
);

@target(function)
@repeatable
pub annotation middleware(
  name: meta.string,
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
              ( function | class | annotation-declaration ) ;

annotation = "@", qualified-name,
             [ "(", [ annotation-arguments ], ")" ] ;

annotation-arguments = annotation-argument,
                       { ",", annotation-argument }, [ "," ] ;

annotation-argument = [ name, "=" ], annotation-value ;

annotation-declaration = "annotation", name,
                         "(", [ annotation-parameters ], ")", ";" ;

annotation-parameters = annotation-parameter,
                        { ",", annotation-parameter }, [ "," ] ;

annotation-parameter = name, ":", type,
                       [ "=", annotation-value ] ;
```

位置参数必须出现在命名参数之前。命名参数不能重复。缺少的可选参数由声明中的默认值填充。参数值必须是编译期常量，首版支持布尔、字符、整数、浮点数、`meta.string`、`meta.symbol` 和 `meta.type`。

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

`import module.*` 仍然只导入该模块的公开函数，不导入注解短名称。私有注解只能在其声明模块内使用。

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
@target(function, class)
annotation serializable();
```

当前支持 `function`、`class`、`field`、`method`、`constructor`、`destructor` 和 `annotation`。`module`、`parameter`、`local`、`statement` 和 `expression` 留待后续实现。

### `@repeatable`

默认情况下，同一注解在同一目标上只能出现一次。`@repeatable` 允许重复使用。

### `@retention`

- `source`：仅保留在 AST；
- `compile`：保留在 Sema 元数据中，默认值；
- `runtime`：生成运行时元数据，需要单独定义稳定 ABI。

首个实现阶段支持 `source` 和 `compile`；`runtime` 在 ABI 确定前报告不支持。

## 内建注解

内建注解也以预声明方式进入注解符号表。完成普通参数验证后，Sema 根据稳定的 `BuiltinAnnotationKind` 调用处理器。计划提供：

- `@inline(default|hint|always|never)`；
- `@deprecated([message])`；
- `@export([name])`；
- `@link_name(name)`；
- `@compiler.intrinsic(name)`。

`pub` 控制 Kelyra 模块可见性，`@export` 控制目标文件符号，二者不能混为一谈。`@compiler.intrinsic` 使用稳定的 Kelyra intrinsic 名称，由各后端映射，不暴露 LLVM 名称。

## 用户处理器

用户注解默认只是元数据。IDE、文档工具和将来的编译期程序可以通过
`meta(...)` 读取它。`meta` 只负责反射；后续通过独立的 `comptime`
函数和显式处理器赋予行为：

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
   `meta.symbol`/`meta.type` 实体解析；~~
3. ~~`@target`、`@repeatable`、`@retention(source|compile)`；~~
4. `@inline`、`@deprecated`、`@export` 及诊断严重级别；
5. 标准库需要的 `@compiler.intrinsic`；
6. `meta` 查询 API、用户处理器和运行时保留 ABI。
