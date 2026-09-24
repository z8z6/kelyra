# `@interface class`

`@interface` 注解把普通 `class` 声明标记为接口类；不再使用独立的 `interface` 关键字。目前支持常量字段与有实现或无实现的方法：

```kelyra
@interface pub class Reader {
  pub const BUFFER_SIZE: i32 = 40 + 2;
  pub fn read(count: usize) -> i64;
  pub fn ready() -> bool { return true; }
}
```

常量字段不占对象布局，初始化式必须只包含字面量及其一元、二元纯表达式，
类型目前限数值、`bool`，或由字符串字面量初始化的 `*c.char`。
可通过 `Reader.BUFFER_SIZE` 使用，不能赋值。
接口方法的无实现形式以分号结尾；有实现形式使用函数体。无实现方法只能经由
接口引用动态调用。
接口不允许普通实例字段、`init`、`deinit`、`copy` 或 `move`，也不能实例化。

`class X: Reader` 声明实现接口；编译器检查类或其基类提供公开且签名匹配的方法。
接口也可以继承多个接口。使用 `*Reader` 接收实现类指针后，可动态调用接口方法：

```kelyra
fn consume(reader: *Reader) -> i64 { return reader.read(1); }
let value = MyReader();
let reader: *Reader = &value;
```

接口引用保存对象指针和每个方法的入口。实现类的虚方法在转换时读取当前虚方法槽，
因此经由接口调用仍会派发到派生类覆写。接口引用也可向父接口转换。
接口引用不拥有对象；对象存活时间必须覆盖引用的使用。
