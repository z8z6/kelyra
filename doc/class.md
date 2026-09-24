# `class`、构造函数与 RAII 设计

> 状态：首版已实现，包括构造、析构、作用域 RAII、成员工具支持和编译期元数据。
> 运行时反射 ABI 仍仅为方案，暂不实现。

`class` 是带方法和确定性析构的栈上值类型。它用于文件、锁、分配器句柄等需要
“获得资源后，在离开作用域时释放”的对象。Kelyra 仍使用普通原始指针，不增加
`mut`、借用检查或隐式引用计数。

Kelyra 不同时保留 `struct`：原有 Kelyra `struct` 声明和关键字已由
`class` 完全替代。纯数据聚合、资源对象和普通值对象都使用 `class` 表达。C 头文件中的
结构体继续作为 `c.Type` 外部记录导入，但不能在 Kelyra 源码中用 `struct` 声明。

当前实现提供单继承、虚方法分派、单一构造函数、可选析构函数，以及可自定义的复制/移动构造方法。
不包含异常、运算符重载或隐式堆分配。

## 单例类

`@singleton` 类通过 `Class.instance()` 返回唯一实例的指针。首次访问时运行无参数
`init`，并发访问只初始化一次。直接调用 `Class()`、按值复制单例，以及继承单例类
都会报错。单例类可以省略 `init`，由编译器生成默认初始化。

```kelyra
@singleton
class Registry {
  count: i32;

  pub fn next() -> i32 {
    count = count + 1;
    return count;
  }
}

let registry: *Registry = Registry.instance();
let id = registry.next();
```

实例的存储持续到进程结束，编译器不会在退出时自动调用它的 `deinit`。
`init` 不应直接或间接调用本类的 `instance()`。
泛型单例类按具体类型分别拥有实例，例如 `Cache<i32>.instance()` 和
`Cache<u8>.instance()` 返回不同对象。

## 静态成员

`@static` 可标注类字段和方法。静态字段不计入对象布局，从零初始化，并在程序运行期间
保留；泛型类的每个具体类型各有一份，例如 `Counter<i32>.value` 和
`Counter<i64>.value` 互不影响。静态方法没有 `this`，通过 `Class.method()` 调用。
字段和方法仍由 `pub` 控制模块外可见性。静态字段目前支持数值、布尔、字符、指针及其
定长数组；class 值需要明确的初始化和析构语义，暂不支持作为静态字段。

```kelyra
class Counter<T> {
  @static
  value: T;

  @static
  pub fn add(amount: T) -> T {
    value = value + amount;
    return value;
  }
}

let first = Counter<i32>.add(1);
```

## 基本语法

```kelyra
module app.file;

pub class File {
  handle: c.int;

  pub init(path: *c.char) {
    handle = c.open(path, 0);
  }

  pub fn valid() -> bool {
    return handle >= 0;
  }

  deinit() {
    if handle >= 0 {
      c.close(handle);
    }
  }
}
```

使用构造表达式建立对象：

```kelyra
let file = app.file.File("input.bin");
if file.valid() {
  // use file
}
// 此处自动执行 file.deinit()
```

语法：

```ebnf
declaration       = { annotation }, [ "pub" ],
                    ( function | class | annotation-declaration ) ;
class             = "class", name, [ ":", type, { ",", type } ],
                    "{", { class-member }, "}" ;
class-member      = { annotation }, [ "pub" ],
                    ( class-field | method | constructor | destructor ) ;
class-field       = name, ":", type, ";" ;
method            = "fn", name, "(", [ parameters ], ")",
                    [ "->", result-type ], block ;
copy-method       = "fn", "copy", "(", name, ":", "*", class-name, ")", block ;
move-method       = "fn", "move", "(", name, ":", "*", class-name, ")", block ;
constructor       = "init", [ "<", name, "...", ">" ], "(", [ parameters ], ")", block ;
destructor        = "deinit", "(", ")", block ;
construction      = qualified-name, "(", [ arguments ], ")" ;
```

`class` 和 `this` 是保留字；`init`、`deinit` 只在 class 成员位置作为上下文关键字。
一个 class 最多有一个 `init`，最多有一个 `deinit`。没有写 `init` 时，编译器生成一个
无参数默认构造函数：按字段声明顺序，普通字段清零，class 类型字段调用其自身的默认
构造函数。因此字段类型必须可以无参数构造；否则必须在 `init` 中显式构造该字段。

构造函数也可以使用编译期参数包转发实参：

```kly
class Box<T> {
  value: T;

  pub init<Args...>(
    @forward
    args: ...Args
  ) {
    this.value = T(...args);
  }
}
```

`Args...` 按调用处的各个实参分别推导类型，允许不同类型及零个实参。
`@forward` 只用于构造函数的参数包；`...args` 在调用位置展开，且一个构造函数体
只能展开一次。编译器先按从左到右的顺序求值，再转发给目标构造函数。左值按目标签名
复制，临时类值按目标签名移动。也可以用 `super(...args)` 转发给基类构造函数。
参数包不允许作为普通值使用，构造函数体不能提前 `return`。
没有 `deinit` 时编译器生成空析构体，但仍会析构 class 类型的字段。首版不提供构造
函数重载；替代构造路径写成接受对象指针的普通模块函数。

普通函数和方法可显式写 `-> void`；省略返回类型也等价于 `void`，使用 `return;` 或到达函数末尾返回。
`init` 和
`deinit` 固定无返回值，不能写返回类型。

方法也支持多返回值和函数值返回，`result-type` 语法见 [前端文档](frontend.md)。

## 字段、方法与 `this`

- 字段按声明顺序布局，默认私有；`pub` 字段可从模块外访问。
- 方法默认私有，`pub fn` 可从 class 所在模块之外调用。
- `pub init` 决定模块外能否构造该 class；`deinit` 不能标记为 `pub`，也不能由源码
  直接调用。
- 方法、`init` 和 `deinit` 中隐式提供 `this: *ClassName`。Kelyra 没有 `mut`，因此
  方法都可以修改 `this` 指向的对象。
- 在没有同名参数或局部变量时，字段可以直接写成 `handle`，等价于 `this.handle`。
  参数和局部变量会遮蔽同名字段；此时必须写 `this.handle` 才能访问字段。
- 同一 class 的方法在没有同名可见函数时可以直接调用。若成员方法与可见模块函数同名，
  必须用 `this.method()` 选择成员方法，或用限定名选择模块函数。
- `value.method(args)` 静态改写为 `ClassName.method(&value, args)`；若接收者已经是
  `*ClassName`，则直接传递该指针。
- `value.field` 与 `pointer.field` 都可访问字段；对指针形式执行隐式一次解引用，不增加
  C++ 风格的 `->` 操作符。
- 普通 class 方法为静态分派，`@virtual` 方法经由接收者的虚方法槽分派。
  无需接收者的操作继续写成模块函数，不增加 `static fn`。

## 继承与覆写

```kelyra
class Base {
  value: i32;
  pub init(value: i32) { this.value = value; }

  @virtual
  pub fn read() -> i32 { return value; }
}

class Child: Base {
  extra: i32;
  pub init(value: i32) {
    super(value);
    extra = 1;
  }

  @override
  pub fn read() -> i32 { return super.read() + extra; }
}
```

继承列表中最多有一个普通基类，且必须写在接口之前。派生类显式 `init` 的第一条语句
必须调用 `super(...)`，然后按顺序初始化本类字段；没有显式 `init` 时会调用基类的
无参数构造函数。析构时先执行本类析构体，再析构字段与基类部分。
覆写基类虚方法必须标记 `@override` 并保持签名；`super.method()` 直接调用基类实现。
派生类指针可隐式转换为基类指针，通过基类指针调用虚方法仍会派发到派生类实现。
派生类目前不能自定义 `copy` 或 `move`。`@final` 类不可作为基类。

class、函数和其他类型的限定名不能在同一模块中产生构造调用歧义。跨模块 class 类型
使用限定名，例如 `app.file.File`；现有 `import module.*` 仍只省略公开函数的模块前缀，
不把类型名称导入当前作用域。

## 构造与字段初始化

构造表达式会建立一个 class 值。直接初始化局部变量或字段时采用构造消除；
用于整体赋值或按值传参时先建立临时对象，再移动其值：

```kelyra
let file = File(path);
let file: File = File(path);
child = Child(value); // 仅在当前 init 的字段初始化阶段
file = File(path);
consume(File(path));
```

直接初始化时编译器先为对象分配最终存储，再以该地址作为 `this` 调用 `init`。
`let file: File;` 非法，class 局部变量必须有构造或复制初始化式。

为了避免复杂且容易出错的部分初始化数据流，`init` 的开头必须按字段声明顺序，对每个
字段恰好初始化一次：

```kelyra
class Buffer {
  data: *u8;
  size: usize;
  lock: Lock;

  init(size: usize) {
    data = allocate(size);
    this.size = size; // 参数 size 遮蔽字段，必须显式使用 this
    lock = Lock();

    // 所有字段初始化后，才可出现普通语句。
  }
}
```

初始化前缀中不允许分支、循环、`return`，也不能读取尚未初始化的字段。普通字段使用
赋值表达式初始化；class 字段可用构造表达式或另一个 class 值初始化。初始化完成后，
字段可以继续赋值。class 整体赋值会先将右值复制/移动到临时对象，再析构目标并移动
临时对象到目标；这使自赋值安全。

按值递归包含自身的 class 非法，`*Self` 字段合法。支持 class 字段和按值参数，
仍不支持 class 数组；普通函数和方法可以按值返回 class。

## RAII 与析构顺序

一个 class 对象在 `init` 正常返回后进入存活状态。每个存活的 class 局部变量在
离开其词法作用域时恰好析构一次：

1. 同一作用域中的局部变量按构造完成的逆序析构；
2. 先执行对象自己的 `deinit` 主体；
3. 再按字段声明的逆序析构 class 类型字段；
4. 普通字段和原始指针没有自动释放行为。

所有正常控制流出口都必须执行相应清理：

```kelyra
{
  let outer = Outer();
  while condition {
    let item = Item();
    if skip {
      continue; // 先析构 item
    }
    if stop {
      break;    // 先析构 item
    }
  }
  return value; // 先求 value，再析构 outer，再返回
}
```

正常落到 `}`、`return`、`break` 和 `continue` 都触发清理。`when` 只为实际选中的分支
生成对象和清理。循环体中的局部对象每次迭代分别构造、析构。

当前语言没有异常或 unwind。运行期 trap、进程终止、错误的裸指针或错误的内联汇编
不会执行析构。构造函数也没有隐式失败通道；需要报告失败时，先使用返回状态的模块
函数或显式的“无效但可析构”状态。未来若加入异常，必须另行定义部分构造对象的 unwind，
不能暗中改变本规则。

`&object` 可以逃逸出作用域，编译器不做借用或生命周期检查；对象析构后继续使用该
指针是程序错误。RAII 管理 class 自身声明拥有的资源，不把普通 `*T` 自动视为所有权。

## 复制、移动和 ABI 边界

每个 class 都有 `copy(other: *Class)` 和 `move(other: *Class)`；没有显式定义时编译器
逐字段生成。左值初始化、赋值和按值传参使用 `copy`；构造临时值的赋值/传参使用
`move`。`let x = Class(...)` 采用构造消除，不额外调用 `move`。

默认 `move` 在转移字段后清零源对象字段；源对象随后仍会析构。自定义 `move` 必须
留下可安全析构的源对象，自定义 `copy` 必须正确复制所拥有的资源，避免双重释放。
两种方法的 `this` 与 `init` 一样指向未初始化的目标存储，必须按字段声明顺序初始化
全部字段；源码中不能像普通方法那样直接调用。按值参数在被调用函数内拥有独立对象，
退出时析构。class 按值返回时，左值通过 `copy` 构造返回对象，临时值通过 `move`
转移；`*Class` 指针传递不转移析构责任。

class 采用 Kelyra 内部 ABI；当前原生编译按宿主类型大小、字段对齐和尾部填充布局，不承诺
跨编译器版本稳定。class 不能直接按值穿过 C ABI；C 接口使用显式包装函数和不透明指针。
后续若需要 C 布局，应单独设计 `repr(c)`，不能默认把 RAII class 当作 C ABI record。

## 编译器实现模型

实现沿用现有的“先注册、后检查”结构：

1. 注册所有 class 名称，允许字段和方法引用同文件中稍后定义的 class；
2. 注册字段、布局、`init`、`deinit` 和方法签名，检查可见性及递归布局；
3. 检查构造初始化前缀和方法体；
4. 为方法生成普通函数，隐式第一个参数为 `*Class`；
5. 为 class 生成目标相关的 LLVM aggregate 布局，以及一个包含用户析构体和字段逆序析构的
   内部析构函数；
6. 在 IRGen 的每个词法作用域记录已完成构造的 `(class, address)`，在正常落空和每条
   控制流退出边上逆序发出析构调用。

class 类型应在 `sema::Type` 中拥有独立种类和声明标识，不能伪装成现有 opaque
`c.Record`。字段访问 lowering 使用已验证的字段序号生成 GEP。构造调用直接接收目标地址，
析构调用也只接收对象地址。

`return expression` 必须先求值并保存结果，再清理函数内仍存活的对象。循环上下文除
break/continue 目标块外，还需记录应保留的作用域深度，使 IRGen 只清理真正退出的作用域。
该清理栈以后可以复用于已经保留关键字但尚未实现的 `defer`，本阶段不同时实现 `defer`。

## 反射、注解与工具

反射模型增加 `Class`、`Method`、`Constructor` 和 `Destructor` 声明种类；class 记录字段、
方法、大小和对齐；用户声明的析构体有独立的 Destructor 记录。`meta(File)` 返回 class 类型记录。

注解目标增加 `class`、`field`、`method`、`constructor` 和 `destructor`，均支持用户自定义注解。
格式化器、语言服务器
和 VS Code grammar 同步识别 `class`、`init`、`deinit`、`this`，并提供成员补全、跳转和
悬停信息。

## 诊断与验收

Sema 必须明确拒绝：

- 重复的 `init`、重复的 `deinit`；
- 字段漏初始化、重复初始化、顺序错误或初始化前读取；
- 直接调用 `deinit`，或构造表达式出现在无法承接 class 值的位置；
- class 数组；
- 私有 class、字段、构造函数或方法的跨模块访问；
- 按值递归布局以及 class 与函数同名造成的构造歧义。

最小验收程序应通过一个 C 记录函数验证事件顺序，覆盖普通落空、嵌套作用域、字段析构、
`return`、`break` 和 `continue`。另以 Sema 测试覆盖上述拒绝路径，并检查生成 IR 中析构调用
位于每条退出边、且顺序相反。端到端测试在 `-O0` 和 `-O3` 下执行并检查事件序列。

## 实现范围

1. 将现有 `struct` token、AST、注解目标和反射种类迁移为 `class`，不保留双语法；
2. 完成无返回值函数语义，并实现 native class 类型、布局、字段访问和静态方法；
3. 实现单一 `init`、直接目标构造及字段初始化前缀检查；
4. 实现 `deinit`、字段逆序析构和所有正常控制流出口的 RAII 清理；
5. 接入模块可见性、反射、注解、formatter、LSP 与 VS Code；

前五项及复制/移动构造方法、单一 class 按值返回已实现；class 数组仍待设计。
