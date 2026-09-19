# 基础前端

当前实现提供词法分析、Pratt 表达式解析、递归下降声明/语句解析、带源码范围的 AST 和错误诊断。`--check` 只检查语法；`--emit-mlir` 生成 MLIR；`--emit-obj -o <file>` 生成本机目标文件；`--emit-exe -o <file>` 生成本机可执行文件。

Kelyra 源文件默认使用 `.kly` 后缀。

## 使用

```sh
cmake -S . -B build -DCMAKE_CXX_STANDARD=20
cmake --build build --target kelyra_frontend_tests kelyra_sema_tests kelyra_codegen_tests --parallel 4
ctest --test-dir build --output-on-failure
./build/bin/kelyra --check examples/basic.kly
./build/bin/kelyra --dump-ast examples/basic.kly
./build/bin/kelyra --emit-mlir tests/cli/add.kly
./build/bin/kelyra --emit-obj -o add.o tests/cli/add.kly
./build/bin/kelyra --emit-exe -o main tests/cli/main.kly
```

默认优化级别为 `-O0`，不运行优化并保留调试信息；也可指定 `-O1`、`-O2`
或 `-O3`。

`--check` 成功时无输出，`--dump-ast` 输出 S 表达式，`--emit-mlir` 输出 `builtin + func + arith` MLIR，`--emit-obj` 输出可链接的本机目标文件，`--emit-exe` 通过系统 `cc` 链接可执行文件并要求入口为 `fn main() -> i32`。当前支持函数、参数、数值与布尔常量、`+ - * / %`、一元正负号和单条 `return`。退出码：0 成功，1 词法、语法、语义或代码生成错误，2 参数或文件读取错误。

内置类型为 `i8/i16/i32/i64/i128`、`u8/u16/u32/u64/u128`、`f32/f64/f128/f256/f512`、`bool` 和 `char`。`bool` lowering 为 `i1`；`char` 表示 Unicode 标量值并 lowering 为 `i32`。MLIR 没有原生 `f256/f512`，因此它们表示为 `!kelyra.f256` 和 `!kelyra.f512`，目前仅支持函数签名与参数透传，不支持字面量和算术。

前端库本身没有 LLVM 依赖。测试统一使用独立的 GoogleTest 子模块，可按测试套件筛选：

```sh
./build/tests/frontend/kelyra_frontend_tests
./build/tests/frontend/kelyra_frontend_tests --gtest_filter='Frontend.*'
./build/tests/sema/kelyra_sema_tests
./build/tests/codegen/kelyra_codegen_tests
```

测试包含 AST 结构、错误定位与恢复、嵌套限制、dialect 验证和命令行行为。CTest 仅负责运行 GoogleTest 自动发现的用例。

## 当前语法

```ebnf
module       = [ module-decl ], { import }, { declaration } ;
module-decl  = "module", qualified-name, ";" ;
import       = "import", qualified-name, ";" ;
qualified-name = name, { ".", name } ;
declaration  = { annotation }, [ "pub" ], ( function | struct ) ;
annotation   = "@", name ;
function     = "fn", name, "(", [ parameters ], ")", [ "->", type ], block ;
parameters   = parameter, { ",", parameter }, [ "," ] ;
parameter    = name, ":", type ;
struct       = "struct", name, "{", [ fields ], "}" ;
fields       = field, { ",", field }, [ "," ] ;
field        = name, ":", type ;
type         = name, { "[", integer, "]" } ;
block        = "{", { statement }, "}" ;
statement    = block | let | assignment | return | if | while
             | "break", ";" | "continue", ";" | expression, ";" ;
let          = "let", [ "mut" ], name, [ ":", type ], [ "=", expression ], ";" ;
assignment   = expression, "=", expression, ";" ;
return       = "return", [ expression ], ";" ;
if           = "if", expression, block, [ "else", ( block | if ) ] ;
while        = "while", expression, block ;
```

一个文件对应一个模块。入口文件所在目录是模块根目录，例如
`import math.vector;` 会加载 `<模块根>/math/vector.kly`，该文件必须声明
`module math.vector;`。声明默认仅在当前模块可见；加 `pub` 后可由直接导入该模块的
代码通过完整名称调用：

```kelyra
module app.main;
import math.vector;

pub fn main() -> i32 {
  return math.vector.answer();
}
```

Kelyra 函数会使用包含模块路径的符号名；可执行程序的 `main` 保留平台入口名。

C 头文件使用固定的 `c` 模块导入：

```kelyra
import c "print.h";

pub fn main() -> i32 {
  c.print();
  return 0;
}
```

Clang 负责预处理、目标平台基础类型布局和 C 声明解析，Kelyra 通过 LibTooling
直接遍历 `ASTContext` 和 `FunctionDecl` 建立外部函数声明。C 实现文件由
`--c-source` 交给 Clang 编译并链接；额外的头文件
路径、宏和目标参数可重复使用 `--c-arg` 传入：

```sh
kelyra --emit-exe --c-source=print.c -o app main.kly
```

当前 C 导入支持表中的 C 标量类型、数据指针、完整结构体和可变参数函数。结构体值
保持 opaque，由 Clang 编译 ABI thunk 负责按值传递；可使用 `c.Pair*` 标注指针类型，
字符串字面量可传给 `char*` 参数。结构体字段访问、函数指针、枚举、宏常量和非默认
calling convention 尚未支持。

`let` 必须包含类型或初始值。赋值左边只接受名称、成员访问、索引；链式赋值暂不支持。条件后的花括号不可省略。函数参数类型必须写出；省略返回类型的语义留待类型系统确定。

表达式支持名称、十进制整数/小数/指数形式、字符串、布尔值、括号，以及下表中的运算。从低到高排列，二元运算均左结合：

| 优先级 | 语法 |
| --- | --- |
| 10 | `a || b` |
| 20 | `a && b` |
| 30 | `a == b`、`a != b` |
| 40 | `a < b`、`a <= b`、`a > b`、`a >= b` |
| 50 | `a + b`、`a - b` |
| 60 | `a * b`、`a / b`、`a % b` |
| 70 | `-a`、`+a`、`!a` |
| 80 | `f(args)`、`a[i]`、`a.member` |

调用支持空参数和末尾逗号，后缀可以连续组合。比较链（包括 `a < b == c`）必须通过括号明确分组。不支持逗号运算符、隐式相乘或赋值表达式。

标识符目前为 ASCII `[A-Za-z_][A-Za-z_0-9]*`。支持 `//` 行注释和可嵌套的 `/* */` 注释。字符串支持 `\n`、`\r`、`\t`、`\0`、`\\`、`\"` 转义，不允许原始换行或控制字符。数字不含符号，不支持基数前缀、分隔符或类型后缀；数值范围检查留给语义阶段。

## AST 与诊断

`Lexer/Lexer.h` 提供 `parse`、`parseExpression` 和 `dumpAst`。结果持有源码、全部 Token（包括注释和 EOF）、AST 和诊断；诊断可直接写入输出流，格式与 GCC 一致。节点文本保存名称、运算符或字面量原始拼写，字符串尚未解码。节点由 `unique_ptr` 持有，子节点按源码顺序排列。

源码位置统一使用 `Location`，包含文件、字节偏移、行、列和长度；对应的源码范围是半开字节区间 `[Offset, Offset + Len)`。行号、列号从 1 开始，列号按字节计算，制表符计一列。AST 中的分组节点保留显式括号。

所有诊断种类、编号和文本集中定义在 `Diagnostic.h`：`K0001` 词法错误、`K0002` 语法错误、`K0003` 嵌套过深、`K0004` 语义错误。词法错误时不继续解析；语法错误时在语句或顶层声明边界尝试恢复。失败结果可能含不完整 AST，使用前必须检查 `ok()`。

递归解析和 AST 高度均设 128 上限，超限给出诊断，避免异常输入耗尽栈。由于 AST 中还包含语句、函数等外围节点，源代码允许的嵌套层数会略少于该值。

首版未实现泛型、C 导入、结构体/数组字面量、模式匹配、引用类型和完整 CST。后续按实际语言规则增加对应解析分支。
