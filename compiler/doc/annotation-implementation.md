# 注解实现与组合方案

> 状态：类目标绑定、成员注入、组合、循环检测和单例迁移已实现。
> 字段目标绑定、向所属类注入方法与函数包装已实现。`@accessors` 的方法体
> 由标准库中的 Kelyra 注解定义；`@aspect` 使用编译器的受限包装机制，
> 切面函数和调用接口由 Kelyra 定义。
> 注解体支持用 `when` 条件选择要注入的字段和方法。
> 显式的编译器效果声明与 IDE 生成来源跳转仍待实现。

## 声明形式

```kelyra
import std.util.string;

@target(Target.Class)
pub annotation entity(table: StringSlice);

@target(Target.Class T)
pub annotation sealed_entity(table: StringSlice) =
  @entity(table) + @final;

@target(Target.Class T)
pub annotation counted() {
  @static
  count: usize;

  @static
  pub fn next() -> usize {
    count = count + 1;
    return count;
  }
}

@target(Target.Class T)
pub annotation tracked() = @reflect {
  @static
  count: usize;
}
```

纯标记注解以分号结束，只写入带类型的元数据。`=` 后的 `+` 表示按源码顺序
应用其他注解。类注解的实现体可注入目标类的字段和方法；字段注解的实现体
可向所属类注入方法。首版不提供
`require`、任意 AST 改写或模块级声明生成。`@aspect` 可通过受限的编译器
包装机制调用原函数体，不开放任意函数体替换。

```ebnf
annotation-declaration = { annotation }, [ "pub" ], "annotation", name,
                         "(", [ parameters ], ")",
                         [ "=", annotation-use, { "+", annotation-use } ],
                         ( ";" | "{", { annotation-member }, "}" ) ;
annotation-member = class-member | annotation-when ;
annotation-when = "when", expression, "{", { annotation-member }, "}",
                  [ "else", ( annotation-when
                            | "{", { annotation-member }, "}" ) ] ;
```

## 目标绑定

`@target(Target.Class T)` 将 `T` 绑定为使用注解的具体类。`T` 是编译期类型名，不是
运行期参数；可以出现在注入成员的类型、泛型实参和 `meta(T)` 中。泛型类按
具体实例展开，因此每个具体类型的静态字段独立。本文不定义隐式 `Self`。

`@target(Target.Field F)` 将 `F` 绑定为使用注解的字段。实现体中的 `F.type` 表示
字段类型，`F.name` 表示字段名称。`get_${F.name}` 这样的标识符插值用于
生成方法名；普通标识符不会隐式改名。表达式中的 `F` 绑定到字段，
即使生成的方法有同名参数。字段实现体的方法注入所属类；静态字段会使这些方法成为静态方法。
字段注解也可在实现体中使用 `when`，按布尔参数决定是否注入方法。

注解体中的 `when` 可依据布尔参数和目标类原始声明选择成员：

```kelyra
@target(Target.Class T)
annotation identified(enabled: bool = true) {
  when enabled && meta(T).has_field("id") {
    pub fn get_id() -> i32 { return id; }
  }
}
```

条件支持布尔字面量、`!`、`&&`、`||`、`==`、`!=`，以及
`meta(T).is_public()`、`has_field`、`has_function`、`has_member`。
查询只读取目标类使用注解前的原始成员；本轮或其他注解注入的成员不参与判断。
未选分支不注入，也不做成员类型检查。条件无法得到编译期布尔值时报错。

带实现体的注解只允许一个带绑定名的 `class` 或 `field` 目标；纯标记注解继续支持
多个目标。注入成员遵守普通类成员的可见性、静态存储、类型检查和布局规则；
同名成员报错，不覆盖原有成员。类外函数和类型在注解定义模块解析，目标绑定名
和生成成员在目标类解析。生成成员会保留注解定义模块信息；IDE 跳转到定义与
使用处的双向来源映射仍待实现。

## 组合语义

使用 `@C` 时，从左到右展开 `C` 的组合项，然后注入 `C` 自己的成员。
组合项按直接使用时的规则检查参数、可见性、目标及 `@repeatable`。
组合形成环时在声明阶段报错。源码中的 `@C` 保留；展开得到的注解加入
有效注解集合，参与其语义效果和反射查询。`@retention` 只控制元数据保留，
不跳过展开。首版不允许普通组合包含需要在模块加载前运行的 `@cfg`。

## 编译阶段

1. 解析模块，注册注解签名和组合依赖；检查组合环与目标兼容性。
2. 绑定注解实参，展开组合项，绑定目标类型并注入类成员。
3. 检查重复成员和可见性，完成泛型实例、类布局与函数体类型检查。
4. 将原始及有效注解记录到反射数据库，再生成 IR。

需要改变继承、布局、ABI 等语言规则的注解，将通过受限的编译器效果声明绑定
底层能力；目前这些内建效果仍由编译器直接识别。`@singleton` 的成员和初始化
逻辑已在 Kelyra 标准库中定义，不需要 `Singleton<T>` 类或 `@derive`。

## `meta` 标准库

`meta(T)` 仅由语言取得只读编译期元数据句柄。所有公开查询方法，包括
`is_public`、`has_annotation`，都在 `std.meta` 中用 Kelyra 定义。它们调用的
私有编译器操作使用无函数体的 `@intrinsic` 声明；省略参数时，函数名就是
操作名，也可以显式指定操作名。注解操作名是编译器与标准库的绑定点。
标准库方法可以调用少量私有、稳定的元数据读取原语；编译器不以公开方法名
识别查询。注解展开期间只能读取已完成的原始声明信息，不能读取尚在展开的
生成成员，以避免展开顺序影响结果。
