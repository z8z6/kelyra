# Freestanding 启动与链接设计

> 状态：Linux x86-64 已实现并验证；Windows x86-64 启动与链接已接入，
> 仍需在 Windows 本机工具链上验证。

## 范围

Kelyra 当前使用系统 `cc` 链接可执行文件，并要求入口为
`fn main() -> i32`。默认 C 工具链通常会自动加入 CRT 启动对象、libc 和编译器
运行时。例如 Linux PIE 链接常见的输入顺序是：

```text
Scrt1.o crti.o crtbeginS.o
Kelyra 目标文件
libgcc libc
crtendS.o crtn.o
```

具体文件随平台和工具链变化。它们主要负责：

- 提供内核或系统加载器识别的进程入口；
- 将原始进程状态转换成 `main` 的参数和调用环境；
- 初始化 TLS、运行时状态及全局构造器；
- 调用 `main`，运行终结逻辑并结束进程；
- 提供目标机器不能直接完成的算术、展开等编译器辅助函数。

Freestanding 模式的目标是让不使用 C 的 Kelyra 程序无需 CRT 或 libc 即可生成
可执行文件。它不要求重新实现动态加载器、内核或完整 C 运行时。

## 归属

启动运行时不属于 Kstd。即使程序没有导入 Kstd，它也必须能够成为可执行文件。

| 内容 | 所属仓库 |
| --- | --- |
| 入口符号、启动 thunk、入口检查和链接模式 | Kelyra |
| 构建选项透传和产物管理 | Kelp |
| allocator、字符串、I/O 等可选库功能 | Kstd |
| Kstd 对 freestanding 能力的使用说明 | Kstd 文档 |

依赖方向必须保持为：

```text
应用程序 ---------> Kelyra 最小运行时
    |
    `-------------> 可选 Kstd
                         |
                         `-> Kelyra 最小运行时
```

Kelyra 编译器不能依赖 Kstd。暂不创建独立的运行时仓库；只有当 TLS、线程、异常
或其他运行时功能显著增长时，再评估拆分。

## 两种链接模式

默认行为保持兼容，并新增显式的 freestanding 模式。选项名称暂定，最终以 CLI
设计为准：

| 模式 | 入口 | 链接内容 |
| --- | --- | --- |
| `host` | 平台 CRT 调用 `main` | 当前 `cc` 默认行为 |
| `freestanding` | Kelyra 启动 thunk 调用 `main` | Kelyra 对象、启动对象及显式库 |

Freestanding 首版仍可借用 Clang/GCC 驱动执行链接，但必须传递 `-nostdlib`、入口
符号和明确的链接输入。使用编译器驱动不等于依赖 libc；是否引入 CRT/libc 由
最终链接参数和未解析符号决定。只有需要减少外部工具依赖或支持交叉链接时，才
改为直接驱动 LLD。

使用 `kelyra --emit-exe --runtime=freestanding -o app src/main.kly`，或在
Kelp 的 `[build]` 中设置 `runtime = "freestanding"`。默认值是 `"host"`。
Freestanding 只适用于可执行文件，仍要求入口签名为 `fn main() -> i32`。
也可使用 `--emit-obj --runtime=freestanding --target=<triple>` 生成包含
`__kelyra_start` 的目标文件，再由匹配目标平台的链接器与对应的
`runtime/<platform>/start.S` 启动对象手动链接；这便于交叉构建验收。
Linux 链接显式使用 `-nostdlib -static`，因此正常产物没有 `PT_INTERP`
和 libc 依赖。若程序调用 libc 或需要尚未提供的编译器辅助函数，需自行提供
链接输入，否则链接报未解析符号。

## 启动结构

普通 Kelyra 函数可能生成函数序言，无法可靠读取内核提供的原始栈，也不能充当
所有平台的第一条指令。入口应由编译器选择并链接一个很小的目标相关汇编 thunk，
而不是给语言增加通用裸函数能力。

### Linux x86-64

```text
Linux 内核
  -> _start
  -> 从初始 rsp 取得 argc、argv、envp
  -> 建立符合 System V ABI 的调用环境
  -> __kelyra_start(argc, argv, envp)
  -> main()
  -> exit_group(status) syscall
```

首版生成静态 ELF，避免引入 ELF 动态加载器。`_start` 不返回；程序结束使用
`exit_group` syscall。参数和环境信息可以先由最小运行时接收，但当前
`fn main() -> i32` 不暴露它们。

### Windows x86-64

```text
Windows PE loader
  -> Kelyra PE entry
  -> __kelyra_start()
  -> main()
  -> ExitProcess(status)
```

Windows 后端使用稳定的 Win32 系统 ABI 调用 `ExitProcess`，不使用裸 Windows
syscall。PE loader 继续负责映射系统 DLL 和解析导入；这不是 CRT 依赖。

### 公共启动函数

`__kelyra_start` 是编译器与启动 thunk 的内部 ABI，不是用户 API。当前由编译器
生成，只调用 `main` 并返回退出码。后续职责包括：

1. 运行编译器收集的模块级初始化函数；
2. 调用 `fn main() -> i32`；
3. 逆序运行已登记的终结函数；
4. 将状态码交回平台退出路径。

当前尚无模块级变量和初始化器，第 1、3 步为空，不为未来功能预先实现注册表。

## 代码位置

最小布局为：

```text
kelyra/
  runtime/
    linux-x86_64/start.S
    windows-x86_64/start.S
  doc/
    freestanding.md
```

启动对象随编译器提供，由 driver 根据目标 triple 选择。平台无关的启动调用由
编译器生成；平台汇编只负责无法安全表达为普通函数的入口交接。

## 编译器改动

按依赖顺序实现：

1. 增加明确的运行时/链接模式，默认继续使用 `host`；
2. 从 LLVM target triple 选择受支持的启动对象；
3. freestanding 模式仍验证 `fn main() -> i32`，但额外生成或引用
   `__kelyra_start`；
4. 保证 `_start` 或 PE entry 使用平台要求的原始符号名和可见性；
5. 以 `-nostdlib` 链接启动对象和程序对象，并显式设置入口；
6. 检测并报告未提供的 libc、CRT 或 compiler-runtime 符号；
7. 在 Windows 上加入最小系统导入库，在 Linux 静态模式下不加入 libc。

不通过把用户 `main` 直接设为 ELF/PE 入口来省略 thunk。内核入口状态不满足
普通函数调用契约，而且 `main` 返回后没有合法调用者。

## 编译器运行时辅助函数

即使源码不调用 libc，LLVM 仍可能为宽整数除法、栈保护、原子操作或展开生成
辅助符号。Freestanding 模式必须采用以下规则：

- 首版只接受无需额外辅助函数的已验证操作；
- 出现未解析辅助符号时明确报错，不静默加入 libgcc 或 libc；
- 实际需要后，再提供最小的 Kelyra compiler-runtime builtin；
- panic、异常展开、TLS 和线程初始化不在首版范围内。

## 实施阶段

| 阶段 | 产物 | 验收 |
| --- | --- | --- |
| 1 | Linux x86-64 启动对象 | 无 libc 的程序返回指定退出码 |
| 2 | `freestanding` 链接模式 | 产物无 `PT_INTERP`、无 libc 依赖 |
| 3 | Windows x86-64 启动对象 | 无 CRT 的程序通过 `ExitProcess` 返回状态码 |
| 4 | Kelp 配置透传 | 同一项目可选择 `host` 或 `freestanding` |
| 5 | Kstd 集成 | 使用平台 allocator 的程序在两端运行 |

## 测试

最小测试程序只返回固定状态码，随后增加 syscall/Win32 输出和 Kstd allocator
测试。每个平台必须检查：

- 实际入口符号正确；
- `main` 的返回值成为进程退出码；
- 产物没有意外的 libc、CRT 或 heap allocator 导入；
- 未解析 compiler-runtime helper 会产生可理解的诊断；
- 默认 `host` 模式行为没有变化。

Linux 使用 `readelf`/`nm` 检查解释器和未解析符号；Windows 使用 PE 导入表工具
确认只保留显式允许的系统 DLL 导入。交叉编译在两端原生构建稳定后再加入。
