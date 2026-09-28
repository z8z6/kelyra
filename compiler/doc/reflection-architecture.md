# 编译期元数据与运行期反射

> 状态：运行期类型与字段描述符已移入 `std.reflect`；编译期具体句柄
> 可用于注解参数，通用编译期求值和完整运行期导出类别仍待实现。当前状态见
> [`meta` 反射设计](meta.md)和
> [`kstd` 运行期反射说明](../../kstd/doc/reflection.md)。

## 决策

编译器的 `ReflectionDatabase` 是声明信息的唯一事实来源。编译期查询读取完整的、
当前模块可见的语义记录；运行期反射读取从这些记录筛选、序列化出的描述符。
两边共享名称、类型关系、继承关系及字段归属等语义，不共享 Kelyra 描述符类、
内部编号或对象布局。
公开的 `Field`、`Function`、`Parameter` 用各自阶段的 `Type` 表示关联类型；
序列化格式中的类型编号仅供装载描述符时建立这些对象，不作为字段接口暴露。
`Field.owner` 使用 `*Class`，`Parameter.owner` 使用 `*Function`。自由函数属于
模块，方法属于类，因此 `Function.owner` 使用指向通用 `Symbol` 的指针。
编译期 `Type`、`Field`、`Function`、`Parameter`、`Annotation` 均继承 `Symbol`，
`Class` 继承 `Type`，
共有的 `id` 和 `name: std.util.string.StringSlice` 只在基类定义一次。

| 阶段 | 公开模块 | 描述符 | 可取得的数据 |
| --- | --- | --- | --- |
| 编译期 | `std.meta` | `Symbol`、`Type`、`Class`、`Field`、`Function`、`Parameter`、`Annotation` | 当前编译中可见的声明；布局属性须待布局完成 |
| 运行期 | `std.reflect` | `Type`、`Class`、`Field`、`Function`、`Parameter`、`Value` | 明确导出的元数据及对象值 |

`meta(T)` 是编译期操作符，按目标静态返回 `std.meta.Class`、
`std.meta.Field` 等句柄。句柄不能进入运行期代码。运行期的
`std.reflect.class_of<T>()` 返回 `Result<std.reflect.Class, ReflectError>`；
没有导出描述符时返回错误。两类 `Field` 不能互换，
`std.reflect.read<T>()` 只接受 `std.reflect.Field`。

以下是目标接口草案，当前不能作为完整的可编译示例：

```kelyra
when meta(User).has_annotation(meta(serializable)) {
  // meta(User): std.meta.Class
}

let found = std.reflect.class_of<User>();
if found.valid && found.ok {
  let class = found.value();
  let field = class.field("name"); // std.reflect.Field
}
```

## 查询语义

两阶段尽量使用相同的查询名，例如 `name()`、`type()` 和
`base_class()`。编译期成员集合遍历接口暂不提供，待编译期执行机制
支持集合求值后再设计。编译期视图遵守模块可见性，
不会因反射而越过 `pub`；显式导出的运行期私有字段则按导出策略可见。

编译期查询可以看到有效注解，包含组合展开产生的注解。运行期查询只看到
`@retention(std.annotation.Retention.Runtime)` 保留的注解；查询一个没有运行期保留的注解应给出诊断，
不能把“未导出”解释为“没有标注”。

运行期描述符应记录字段、方法、注解等元数据类别是否已导出。缺少某类别时，
相关查询返回 `MissingMetadata`；只有类别已导出且集合确实为空时才返回空集合。
现有 `@reflect` 对公开实例字段和显式标记的私有字段的选择规则保持不变；
动态方法调用、对象构造和 ABI 自动封送不属于首轮范围。

描述符名称由只读字节[切片](slice.md)表示，例如
`name() -> []const u8`。名称使用 UTF-8，不要求 NUL 结尾；
需要拥有文本的调用方可以显式复制到 `std.util.string.String`。
编译期切片借用编译器保存的文本，运行期切片借用只读描述符。

## 身份、阶段与 ABI

编译期 `MetaId` 只在一次编译中有效，不能写入运行期描述符。
运行期类型身份应通过不可修改的描述符标识及 `same_type()` 比较，
不把当前 `u64` 哈希值作为唯一相等依据，也不把内部编号公开为稳定 API。
泛型具体类型按实际类型实参形成独立身份。

注解展开期间只能读取已经确定的原始声明信息。大小、对齐和字段偏移要等
类布局完成后才可查询；在更早阶段请求这些属性应给出阶段错误。
编译期求值器负责解释 `std.meta` 的公开方法调用，编译器仅提供私有、稳定的
元数据读取原语。

当前逐类型字段描述符 `KLRF` 已实现，通用注解描述符 `KLRM` 仍是草案。
后续应让字段、方法和注解使用同一版本化记录模式及导出规则，避免形成两套
互不一致的类型和字段定义。对象文件仍可按类型保存片段，但链接后的引用与
类别标记必须按统一规则解析。ABI 数据使用偏移和长度，不保存编译器指针。

## 迁移顺序

1. 为 `std.meta` 定义真正的编译期句柄类型，让 `meta(...)` 返回具体类型，
   并实现可在 `when` 中使用的查询。
2. 已将运行期字段解析和 `type_of<T>()` 移回 `std.reflect`；
   `TypeInfo`、`FieldInfo` 名称暂保留为该模块中的别名。
3. 从 `ReflectionDatabase` 统一生成运行期描述符，区分未导出、空集合和
   已导出的成员；迁移字段读写和跨模块链接用例。
4. 按需扩展方法与运行期注解记录，并统一 `KLRF` 与 `KLRM` 的格式规划。

本方案不改变当前已经运行的 `@reflect` 字段功能。`std.meta` 与
`std.reflect` 的描述符类现已分离，`meta(...)` 在注解参数中按具体
句柄类型检查；通用编译期表达式尚未开放句柄。KLRF v1 仍只导出字段。
