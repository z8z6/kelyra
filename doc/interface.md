# `@interface class` 首版

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
接口方法的无实现形式以分号结尾；有实现形式使用函数体。无实现方法不能直接调用。
接口不允许普通实例字段、`init`、`deinit`、`copy` 或 `move`，也不能实例化。

当前版本只完成声明与静态检查。`class X: Reader`、接口值、实现关系检查和动态
派发尚未实现，因而接口方法目前不构成可运行的多态调用协议。
