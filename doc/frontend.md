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
./build/bin/kelyra --dump-class-layout tests/cli/inheritance.kly
./build/bin/kelyra --emit-mlir tests/cli/add.kly
./build/bin/kelyra --emit-obj -o add.o tests/cli/add.kly
./build/bin/kelyra --emit-exe -o main tests/cli/main.kly
```

构建时会读取标准库的 `std/annotation.kly` 和 `c.kly`，分别加载内置注解与 C ABI 类型声明。
默认查找同级 `kstd/src`；标准库在别处时可传入
`-DKELYRA_STDLIB_SOURCE_DIR=/path/to/kstd/src`。

默认优化级别为 `-O0`，不运行优化并保留调试信息；也可指定 `-O1`、`-O2`
或 `-O3`。

`--check` 成功时无输出，`--dump-ast` 输出 S 表达式，
`--dump-class-layout` 在语义检查后输出类的大小、对齐，以及每个实际字段的偏移、
大小、对齐和布局索引。`--emit-mlir` 输出 MLIR，`--emit-obj` 输出可链接的本机目标文件，
`--emit-exe` 链接可执行文件并要求一个标记 `@main` 的无参数、返回 `i32` 的函数。
退出码：0 成功，1 词法、语法、语义或代码生成错误，2 参数或文件读取错误。

内置类型为 `i8/i16/i32/i64/i128/isize`、`u8/u16/u32/u64/u128/usize`、`f32/f64/f128/f256/f512`、`bool` 和 `char`。`usize` 和 `isize` 使用本机指针宽度。`bool` lowering 为 `i1`；`char` 表示 Unicode 标量值并 lowering 为 `i32`。MLIR 没有原生 `f256/f512`，因此它们表示为 `!kelyra.f256` 和 `!kelyra.f512`，目前仅支持函数签名与参数透传，不支持字面量和算术。

`alias Name = Existing;` 声明透明类型别名，也可写成 `alias Pointer<T> = *T;`，在使用时通过 `Pointer<i32>` 指定参数。别名可加 `pub` 并通过模块名引用；别名和目标类型可以直接互用。C ABI 类型由标准库 `c.kly` 中的公开别名提供，例如 `c.int`、`c.long` 和 `c.char`。

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
module-decl  = { annotation }, "module", qualified-name, ";" ;
import       = { annotation }, "import", qualified-name, [ ".", "*" ], ";" ;
qualified-name = name, { ".", name } ;
declaration  = { annotation }, [ "pub" ],
               ( function | class | alias-declaration | annotation-declaration ) ;
alias-declaration = "alias", name, [ generic-parameters ], "=", type, ";" ;
annotation   = "@", qualified-name,
               [ "(", [ annotation-arguments ], ")" ] ;
annotation-arguments = annotation-argument,
                       { ",", annotation-argument }, [ "," ] ;
annotation-argument = [ name, "=" ], expression ;
annotation-declaration = "annotation", name,
                         "(", [ annotation-parameters ], ")", ";" ;
annotation-parameters = annotation-parameter,
                        { ",", annotation-parameter }, [ "," ] ;
annotation-parameter = name, ":", type, [ "=", expression ] ;
meta-expression = "meta", "(", ( qualified-name | type ), ")" ;
function     = "fn", name, [ generic-parameters ], "(", [ parameters ], ")", [ "->", result-type ],
               ( block | ";" ) ;
generic-parameters = "<", generic-parameter,
                     { ",", generic-parameter }, ">" ;
generic-parameter = name, [ "..." ] ;
result-type  = type | "(", type, ",", type, { ",", type }, [ "," ], ")" ;
parameters   = parameter, { ",", parameter }, [ "," ] ;
parameter    = { annotation }, name, ":", [ "..." ], type ;
class        = "class", name, [ generic-parameters ],
               [ ":", type, { ",", type } ], "{", { class-member }, "}" ;
class-member = { annotation }, [ "pub" ],
               ( field | constant-field | function | constructor | destructor ) ;
(* @interface class 仅允许 constant-field 与 function，后者可用分号省略函数体。 *)
constant-field = "const", name, ":", type, "=", constant-expression, ";" ;
field        = name, ":", type, ";" ;
constructor  = "init", [ "<", name, "...", ">" ], "(", [ parameters ], ")", block ;
destructor   = "deinit", "(", ")", block ;
type         = "*", type | qualified-name, [ "<", type, { ",", type }, ">" ],
               { "[", integer, "]" }
             | "fn", "(", [ type, { ",", type } ], ")", [ "->", result-type ] ;
block        = "{", { statement }, "}" ;
statement    = block | let | assignment | return | if | while
             | asm | "break", ";" | "continue", ";" | expression, ";" ;
let          = "let", name, [ ":", type ], [ "=", expression ], ";"
             | "let", "(", name, { ",", name }, ")", "=", expression, ";" ;
assignment   = expression, "=", expression, ";" ;
return       = "return", [ expression, { ",", expression } ], ";" ;
if           = "if", expression, block, [ "else", ( block | if ) ] ;
while        = "while", expression, block ;
asm          = "asm", raw-block, { asm-chain }, ";" ;
asm-chain    = ".", ( "in" | "out" ), "(", name, [ ",", name ], ")"
             | ".", "op", "(", asm-option, { ",", asm-option }, ")" ;
asm-option   = name | "clobber", "(", name, { ",", name }, ")" ;
```

一个文件对应一个模块。入口文件所在目录是模块根目录，例如
`import math.vector;` 会加载 `<模块根>/math/vector.kly`，该文件必须声明
`module math.vector;`。声明默认仅在当前模块可见；加 `pub` 后可由直接导入该模块的
代码通过完整名称调用：

声明的模块名应与文件路径后缀一致，例如 `module math.vector;` 应位于
`math/vector.kly`。不一致时编译器输出警告，但仍继续编译。

```kelyra
module app.main;
import math.vector;

@main
pub fn main() -> i32 {
  return math.vector.answer();
}
```

Kelyra 函数会使用包含模块路径的符号名；`@main` 函数无论源码名称为何，
都会导出平台入口符号 `main`。入口可位于当前文件或导入的模块中，但整个
程序必须恰有一个。未标记的同名函数只是普通函数。
`--external-path` 下的导入模块仅用于读取声明、类型和布局，不检查函数体，
也不生成实现；对应的预编译 object 或 archive 应通过 `--link-input` 链接。
同一文件中的函数先统一注册签名再检查函数体，因此可以在函数定义之前调用它。

顶层 `@cfg(os="linux")` 或 `@cfg(os="windows", arch="x86_64")` 可标注
`module`、`import`、函数、class 或注解声明。`os`、`arch` 均可单独使用；同时指定时须
全部匹配，且同一参数不能重复。
不满足条件的节点在加载依赖与语义检查前移除，因此可以引用仅在目标平台存在的
模块。`@cfg(...) module name;` 不匹配时整个文件不可用于当前目标；若仍被
无条件导入，编译器会报错。条件默认取自编译器本机目标，也可用 `--target=<LLVM triple>`
指定目标。跨目标 C ABI 布局和最终链接仍需匹配的工具链与库；函数内部的
编译期分支继续使用 `when`。

外部函数可用无函数体声明，符号名默认等于函数名，也可显式指定：

```kelyra
@extern("getpid")
@callconv("c")
fn process_id() -> c.int;
```

返回 C 函数指针可声明为 Kelyra 函数值，例如
`@extern("get_callback") fn get_callback() -> fn(c.int) -> c.int;`，
调用方可使用 `get_callback()(42)`。向外部函数传入回调也使用同一类型；
回调必须是无捕获的顶层函数。

`@extern` 和 `@callconv` 首版只支持顶层函数；参数可为标量、指针或
`fn(...) -> ...` 函数值，返回值还可为 `void`。函数值可作为回调传递或调用，
其签名需与实际 C 函数指针一致；调用约定可写 `"c"` 或 `"system"`。在当前支持的
Linux/Windows x86-64 目标上二者均使用平台 C ABI，Windows 上可直接调用
Kernel32 等系统导出符号。可执行文件仍需通过链接器找到相应库；该功能不
生成或导入 C 头文件。结构体按值、可变参数和其他调用约定尚未支持。

用户可以声明带类型的注解，并在定义之前使用。注解支持位置参数、命名参数和默认值；
参数值必须是编译期常量。模块外使用公开注解时需要写限定名，例如
`@web.route("/")`。完整模型见 [`annotation` 设计](annotation.md)。

`meta(target)` 构造目标的编译期反射引用，可用于 `meta.type` 和
`meta.symbol` 注解参数；反射值不能进入普通运行期表达式。完整模型见
[`meta` 反射设计](meta.md)。

`import math.vector.*;` 会加载同一模块，并让其公开函数可以不带
`math.vector.` 前缀调用。它不导入私有函数或类型。当前模块中的同名函数
优先；如果两个通配导入同时提供同名函数，调用处会报歧义错误。

C 头文件使用固定的 `c` 模块导入：

```kelyra
import c "print.h";
import c.*;

@main
pub fn main() -> i32 {
  print();
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

`--emit-obj` 也会把 `--c-source` 指定的 C 文件编译并合并到输出 object 中。
已有的 C object 或 archive 可通过 `--link-input` 直接链接，无需再传对应的
`--c-source`。若希望为导入的 C 头文件生成可 import 的 Kelyra 包装模块：

```sh
kelyra --emit-c-defs --c-defs-module=bridge -o bridge.kly main.kly
```

生成文件包含 `module bridge;`、原始 C header 导入和公开的非可变参数函数包装。
将 `bridge.kly` 放在模块搜索目录下后，可用 `import bridge;` 调用这些函数。
无法表达的签名会跳过；原始 `c.*` API 仍可直接使用。包装模块仍需相应的 C
实现或预编译库参与最终链接。

`@layout(c)` 可用于 class，字段按宿主平台的 C 结构体顺序、大小和对齐排列。
当前仅适用于可表达为 C 数据字段的类型；嵌套 class 也必须标注
`@layout(c)`。该注解不自动导出 C 函数符号。

当前 C 导入支持表中的 C 标量类型（含 `c.float`、`c.double`）、数据指针、
完整结构体和可变参数函数。结构体值
保持 opaque，由 Clang 编译 ABI thunk 负责按值传递；可使用 `*c.Pair` 标注指针类型，
字符串字面量可传给 `*c.char` 参数。结构体字段访问、函数指针、枚举、宏常量和非默认
calling convention 尚未支持。

原始指针类型写作 `*T`。一元 `&` 取得名称、数组元素或解引用表达式的地址，一元 `*` 读取指针指向的值，也可以作为赋值目标。指针不区分只读和可写，编译器不执行借用或生命周期检查。

`asm { ... }` 保留原始汇编文本；`{name}` 通过 `.in(name)` 或
`.out(name)` 绑定 Kelyra 变量并自动分配寄存器。第二个参数可指定固定
寄存器，例如 `.out(low, eax)`。完整规则见 [`asm` 设计](asm.md)。

`let` 必须包含类型或初始值。赋值左边只接受名称、成员访问、索引；链式赋值暂不支持。条件后的花括号不可省略。函数参数类型必须写出；省略返回类型的语义留待类型系统确定。

表达式支持名称、十进制整数/小数/指数形式、字符串、布尔值、括号，以及下表中的运算。从低到高排列，二元运算均左结合：

显式转换使用 `value as T`，支持整数与浮点类型之间转换、扩宽和截断，
指针类型之间的转换，以及显式的指针与整数互转。整数转指针属于不安全操作，
调用方须保证地址有效；窄整数可能截断地址。
`as` 的结合优先级高于乘除、低于调用和成员访问。

| 优先级 | 语法 |
| --- | --- |
| 10 | `a || b` |
| 20 | `a && b` |
| 30 | `a == b`、`a != b` |
| 40 | `a < b`、`a <= b`、`a > b`、`a >= b` |
| 50 | `a + b`、`a - b` |
| 60 | `a * b`、`a / b`、`a % b` |
| 70 | `value as T` |
| 70 | `-a`、`+a`、`!a`、`*a`、`&a` |
| 80 | `f(args)`、`a[i]`、`a.member` |

调用支持空参数和末尾逗号，后缀可以连续组合。比较链（包括 `a < b == c`）必须通过括号明确分组。不支持逗号运算符、隐式相乘或赋值表达式。

标识符目前为 ASCII `[A-Za-z_][A-Za-z_0-9]*`。支持 `//` 行注释和可嵌套的 `/* */` 注释。字符串支持 `\n`、`\r`、`\t`、`\0`、`\\`、`\"` 转义，不允许原始换行或控制字符。数字不含符号，不支持基数前缀、分隔符或类型后缀；数值范围检查留给语义阶段。

## AST 与诊断

`Lexer/Lexer.h` 提供 `parse`、`parseExpression` 和 `dumpAst`。结果持有源码、全部 Token（包括注释和 EOF）、AST 和诊断；诊断可直接写入输出流，格式与 GCC 一致。节点文本保存名称、运算符或字面量原始拼写，字符串尚未解码。节点由 `unique_ptr` 持有，子节点按源码顺序排列。

源码位置统一使用 `Location`，包含文件、字节偏移、行、列和长度；对应的源码范围是半开字节区间 `[Offset, Offset + Len)`。行号、列号从 1 开始，列号按字节计算，制表符计一列。AST 中的分组节点保留显式括号。

所有诊断种类、编号和文本集中定义在 `Diagnostic.h`：`K0001` 词法错误、`K0002` 语法错误、`K0003` 嵌套过深、`K0004` 语义错误。词法错误时不继续解析；语法错误时在语句或顶层声明边界尝试恢复。失败结果可能含不完整 AST，使用前必须检查 `ok()`。

递归解析和 AST 高度均设 128 上限，超限给出诊断，避免异常输入耗尽栈。由于 AST 中还包含语句、函数等外围节点，源代码允许的嵌套层数会略少于该值。

首版未实现聚合/数组字面量、模式匹配、引用类型和完整 CST。后续按实际语言规则增加对应解析分支。

泛型 class 和顶层函数支持一个或多个类型参数，使用显式实例化：

```kelyra
class Pair<T, U> { first: T; second: U; }
fn identity<T>(value: T) -> T { return value; }
let value: Pair<i32, Box<i32>> = Pair<i32, Box<i32>>(1, Box<i32>(2));
let copy = identity<i32>(3);
```

类型参数允许嵌套具体泛型类型。表达式中的泛型实参仅在调用处解析，普通的 `<`、`>`
仍是比较运算符；泛型调用目前必须显式给出类型实参，不做推导。模板按使用点生成具体实例，
包括从已编译模块导入的模板；后者仍需提供源文件以读取模板定义。未实例化的泛型类本身
不能作为类型实参。单个多返回值项暂不支持 class。

构造函数的 `init<Args...>(@forward args: ...Args)` 会从调用实参推导编译期参数包。
普通泛型函数也支持末尾类型包，但沿用显式类型实参：

```kly
fn make<Args...>(args: ...Args) -> Pair { return Pair(...args); }
let pair = make<i32, bool>(42, true);
```

函数参数包的各元素按值传入；空类型包写作 `make<>()`。运行时同类型可变参数尚未实现。

已实现 `class`、`init`、`deinit`、`copy`、`move` 和确定性作用域 RAII；接收者为 `this`，无歧义时可省略。
没有写 `init` 的 class 会自动获得一个无参数默认构造函数，按字段声明顺序清零普通字段、
默认构造 class 类型字段。
普通函数和方法支持 `-> void`，省略返回类型等价于 `void`，允许 `return;` 或自然返回。
`void` 不可用于变量、字段、参数、数组或指针元素类型。不再支持 Kelyra `struct` 声明。
完整语义与首版限制见 [`class` 与 RAII](class.md)。

`@interface class` 支持常量字段和有实现或无实现的方法；常量可写作
`InterfaceName.CONSTANT`。普通类可在继承列表中声明实现接口，编译器检查方法签名。
接口不能实例化；`*Interface` 可以接收实现类指针并动态调用接口方法。详见
[`@interface class`](interface.md)。

单继承使用 `class Child: Base, Interface`。派生类显式 `init` 的第一条语句必须是
`super(...)`，随后按声明顺序初始化自己的字段。`@virtual` 标记基类方法，
`@override` 标记覆写；通过基类指针调用虚方法会派发到实际类型。`super.method()`
直接调用基类实现。派生类指针可以隐式转换为基类指针，类值不会隐式切片。
`@final` 只能标注普通类，被标注的类不可继承。当前派生类不能自定义 `copy` 或 `move`。

## 多返回值

```kelyra
fn divide(value: i32, divisor: i32) -> (i32, i32) {
  return value / divisor, value % divisor;
}
fn forward() -> (i32, i32) { return divide(17, 5); }
@main
fn main() -> i32 {
  let (quotient, remainder) = forward();
  return quotient + remainder;
}
```

函数和方法均支持两个或更多返回值；顺序、数量和类型必须匹配。解构声明推断每个变量的类型，
调用只执行一次。`return f();` 可转发完全匹配的多返回值；`return a, b;` 从左到右求值，
全部求值完成后执行 RAII 清理。调用结果也可整体丢弃。

首版多返回值项支持标量、指针和函数值（数值位宽不超过 128 位）。不支持嵌套返回列表、class/数组/C record
作为多返回值项、列表参数或普通列表变量；必须用 `let (a, b) = f();` 接收。单一返回值可以是 class。
后端使用 LLVM aggregate 承载返回值，不引入堆分配；这是内部 ABI，不承诺与 C ABI 兼容。

## 函数值与返回函数

```kelyra
fn increment(value: i32) -> i32 { return value + 1; }
fn choose() -> fn(i32) -> i32 { return increment; }
fn apply(callback: fn(i32) -> i32, value: i32) -> i32 {
  return callback(value);
}
@main
fn main() -> i32 {
  let callback = choose();
  return callback(41);
}
```

`fn(参数类型...) -> 返回类型` 表示函数值类型；函数类型内部省略返回类型同样表示 `void`。
直接使用函数名称获得函数值，不需要 `&`；支持限定名和 `import module.*` 的可见性规则。
可存入变量、作为参数或返回值，也可作为多返回值的一项。支持 `choose()(41)` 连续调用。
签名必须精确匹配；调用时先求接收的函数值，再从左到右求各参数。

首版函数值是无捕获的原生函数地址，不包含环境或隐式分配。尚不支持匿名闭包、捕获、绑定方法，
也不直接把 C 导入函数转换成函数值；可用普通 Kelyra 包装函数适配 C 调用。
函数值签名暂不支持 class、数组和 C record 按值参数/返回，以及超过 128 位的数值。
函数值本身不做生命周期管理；函数型字段可保存回调，但不会自动获取接收对象的所有权。

## 本机调试与编译进度

`-O0`（默认）生成 DWARF 函数、参数及局部变量信息，包括变量名、类型、地址和词法作用域。
GDB 可查看标量、指针、数组、class 字段、函数值及多返回值解构变量；嵌套作用域同名变量
按当前栈帧位置解析。`this` 作为方法的隐式参数记录。赋值沿用变量地址，无需为每次赋值
创建新的调试变量。`-O1` 至 `-O3` 暂不生成这些调试信息。

调试器采用 C 风格表达式求值，不是 Kelyra 解释器；导入 C record 暂只有不透明类型信息，
尚无原生降低的 `f256`/`f512` 参数不生成变量位置记录。

`--progress` 将读取的 `.kly` 文件、C 头文件、语义检查、代码生成及编译/链接目标写到 stderr。
Kelp 构建默认启用此选项，并打印三个阶段的进度；不影响程序 stdout 或 `kelp output`。
