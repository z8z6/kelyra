# 类与接口

类将字段与方法放在一起。字段默认只在当前模块可见；需要从外部访问时加 `pub`。

## 构造与继承

```kelyra
class Counter {
  pub value: i32;
  pub init(start: i32) { this.value = start; }
  pub fn next() -> i32 {
    value = value + 1;
    return value;
  }
}
```

类可以继承基类；虚方法使用 `@virtual`，覆写方法使用 `@override`。`super` 可访问基类实现。

## 接口

`@interface class` 定义行为契约。下面的测试同时展示了接口、基类、覆写和接口指针。

<<< ../../compiler/tests/cli/interface_polymorphism.kly{kelyra}

接口和虚方法按完整参数签名匹配，同名不同参数的方法可以共存。当前接口是类系统的一部分，更多规则见[接口设计说明](https://github.com/z8z6/kelyra/blob/main/compiler/doc/interface.md)。
