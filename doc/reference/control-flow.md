# 控制流

条件和循环都使用花括号。这样多写两笔，但读代码时很容易看出边界。

## 条件与循环

```kelyra
fn clamp_to_zero(value: i32) -> i32 {
  if value < 0 {
    return 0;
  } else {
    return value;
  }
}
```

`while` 在条件为真时重复执行；`break` 结束循环，`continue` 开始下一轮。

## `for ... in`

可迭代对象需要公开 `iter()`，其迭代器需要公开返回 `Maybe<T>` 的 `next()`；迭代器本身有 `next()` 时也能直接遍历。

<<< ../../compiler/tests/cli/for_in.kly{kelyra}

标准库的 `std.util.iterator` 提供切片适配器与惰性 `pipe`。目前 `map` 回调须是无捕获函数，且保持元素类型不变。

## 枚举与 `match`

纯枚举可以给成员指定整数值。`match` 支持枚举、布尔值和整数等值匹配；对枚举与布尔值要覆盖所有可能分支。

<<< ../../compiler/tests/cli/enum_match.kly{kelyra}

当前没有带载荷的枚举、范围模式和守卫。更多细节见[枚举与 match 设计说明](https://github.com/z8z6/kelyra/blob/main/compiler/doc/enum-match.md)。
