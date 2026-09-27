# 切片

现有定长数组写作 `[N]T`，原始指针写作 `*T`。

切片是借用一段连续元素的视图，保存首元素地址与元素个数，不拥有底层存储。
计划将 `std.meta` 和 `std.reflect` 的名称接口改用只读字节切片
`[]const u8`，不另设 `StringView` 类型。UTF-8 编码由名称接口约定，
切片本身只表示字节。

## 类型与表达式

```kelyra
let numbers: [4]i32;
let all: []i32 = numbers[:];
let middle: []i32 = numbers[1:3];
let prefix: []const i32 = all[:2];

let count: usize = middle.len;
let first: i32 = middle[0];
middle[0] = 42;
// prefix[0] = 42; // 错误：只读切片不能写入元素
```

| 写法 | 含义 |
| --- | --- |
| `[N]T` | 长度为 `N` 的定长数组，拥有元素 |
| `[]T` | 可写切片，借用连续的 `T` 元素 |
| `[]const T` | 只读切片，借用连续的 `T` 元素 |
| `a[start:end]` | 半开范围 `[start, end)` |
| `a[:end]`、`a[start:]`、`a[:]` | 省略的边界分别取零或 `a.len` |
| `a[index]` | 读取单个元素；可写切片也可作为赋值目标 |

`const` 在这里仅修饰通过切片访问的元素；本方案不同时引入通用的
`const T` 类型限定。`[]T` 可隐式转换为 `[]const T`，反向转换不允许。
切片值可以复制，复制的仍是视图，不复制元素。空切片长度为零；
非空切片必须指向至少 `len` 个有效元素。索引与子切片边界越界时给出
运行期边界错误；切片检查不受 `--safe-level` 控制。

语法片段：

```ebnf
slice-type = "[", "]", [ "const" ], type ;
slice-expression = expression, "[", [ expression ], ":",
                   [ expression ], "]" ;
```

函数参数同样使用切片类型：

```kelyra
fn sum(values: []const i32) -> i32 { /* ... */ }
fn fill(values: []u8, value: u8) -> void { /* ... */ }
```

定长数组和已有切片可用范围表达式产生切片；原始指针 `*T` 没有长度，
不能直接写 `pointer[:]`。与外部内存交互时使用显式的
`std.util.slice.from_raw_parts(pointer, length)`，调用者负责保证地址、长度和
存储生命周期有效。切片不是 C ABI 指针；传给 C 时显式传递指针与长度。

语言目前不执行借用或生命周期检查，因此调用者必须保证底层存储在切片
使用期间有效，不能返回指向已销毁局部数组的切片。反射名称引用的只读
描述符在相应程序或加载模块存续期间有效。需要独立持有名称时，将
`[]const u8` 显式复制为 `std.util.string.String`。

当前已实现类型、范围表达式、`.len`、只读元素限制与
`std.util.slice.from_raw_parts` / `from_raw_parts_const`。运行期反射名称由
`[]const u8` 表示；编译期 `std.meta` 的具体句柄仍待接入编译器。
