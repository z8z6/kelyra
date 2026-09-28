# `for ... in` 迭代

```kelyra
for item in collection {
  use(item);
}
```

容器需要公开实例方法 `iter() -> I`，其中 `I` 是迭代器类。迭代器需要
公开实例方法 `next() -> std.util.maybe.Maybe<T>`。如果循环右侧本身具有
`next()`，也可以直接遍历该迭代器。编译器按方法签名检查，不要求继承
特定基类。旧的 `begin/end` 协议不再支持。

循环右侧只求值一次。编译器取得迭代器后反复调用 `next()`；每次返回
有值的 `Maybe<T>`，就将其值绑定到循环变量；返回空值时结束。
`continue` 进入下一次 `next()`，`break` 销毁迭代器和容器临时值。
循环变量仅在循环体内可见。

`std.util.iterator.slice<T>(values)` 将只读切片包装为可迭代容器。对于
标量值，`std.util.iterator.pipe<T>(values)` 提供惰性的 `map`、`filter` 和
`take` 链式操作：

```kelyra
for value in std.util.iterator.pipe<i32>(numbers[:])
    .map(twice).filter(positive).take(3) {
  use(value);
}
```

目前 `map` 只能保持元素类型 `T`，回调必须是不能捕获变量的函数值。
语言尚不支持泛型实例方法和闭包，因此改变元素类型的 `map<U>` 以及
捕获外部状态的回调仍需后续语言支持。

`Maybe<T>` 独立于用于命令行参数的 `std.util.option.Option`。它用
`std.memory.Raw<T>` 在对象内部保留与 `T` 相同大小和对齐的空间，
仅在有值时构造和析构 `T`。`Maybe<T>` 本身不会为每个元素分配内存。
