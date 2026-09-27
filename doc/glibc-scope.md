# glibc 范围与 Kelyra 标准库路线

glibc 不只是常用 C 函数集合。它同时实现 ISO C、POSIX 和 GNU 接口，并承担
Linux 程序启动、线程运行时与 ELF 动态链接等职责。Kelyra 标准库不以完整复刻
glibc 为目标，而采用三层结构：

```text
纯 Kelyra 核心库
平台接口层
可选 C 兼容层
```

glibc 2.44 手册的[完整目录](https://sourceware.org/glibc/manual/latest/html_node/index.html)
和[功能路线图](https://sourceware.org/glibc/manual/latest/html_node/Roadmap-to-the-Manual.html)
是本文分类的依据。

## glibc 的主要代码领域

| 领域 | 内容 |
| --- | --- |
| 运行与链接 | 程序启动、退出、TLS、ABI、ELF 动态装载、重定位、符号与 `dlopen` |
| 内存 | `malloc`、arena、页映射、对齐分配和内存保护 |
| 字符串与内存 | `memcpy`、`memmove`、字符串比较、搜索和 token 化 |
| 字符与国际化 | `ctype`、宽字符、字符集转换、locale 和消息翻译 |
| 数学与数值 | libm、浮点环境、整数运算、数值转换和随机数 |
| I/O 与文件系统 | `FILE`、格式化、fd、目录、权限、mmap、终端和 syslog |
| IPC 与网络 | pipe、FIFO、socket、共享内存和信号量 |
| 进程与信号 | fork/exec/spawn/wait、环境变量、signal 和非局部跳转 |
| 线程 | ISO C threads、pthread、mutex、condvar、TLS 和原子等待 |
| 系统数据库 | DNS resolver、用户、组、主机数据库和 NSS |
| 系统管理 | 时间、资源限制、主机信息和系统配置参数 |
| 调试与调优 | backtrace、探针、tunables 和硬件能力分派 |

低级 I/O 本质上是文件描述符和内核接口的包装，详见 glibc 的
[Low-Level I/O](https://sourceware.org/glibc/manual/latest/html_node/Low_002dLevel-I_002fO.html)。
线程部分同时包含 ISO C 和 POSIX 两套接口，详见
[Threads](https://sourceware.org/glibc/manual/latest/html_node/Threads.html)。动态链接器则负责
装载程序和共享对象，详见
[Dynamic Linker](https://sourceware.org/glibc/manual/latest/html_node/Dynamic-Linker.html)。

## 当前 Kelyra 的实现边界

“纯 Kelyra”表示函数主体由 `.kly` 实现且不调用 C 函数。当前可执行文件仍由系统
`cc` 链接，所以这不等于 freestanding 或 libc-free。

| 方向 | 当前结论 |
| --- | --- |
| ASCII 字节分类和大小写 | 可以独立实现 |
| 基础整数与浮点工具 | 可以独立实现 |
| 简单非密码随机数 | 等整数溢出语义确定后实现 |
| 固定数组算法 | 可以针对具体类型和长度实现，但不适合作为通用 API |
| 通用字符串和内存函数 | 缺少指针索引、指针算术和 slice，暂不能实现 |
| 通用搜索和排序 | 缺少泛型、slice 和函数指针，暂不能实现 |
| 分配器 | 缺少可用结构体、指针算术、页分配接口和同步，暂不能实现 |
| 标准流 I/O | 需要缓冲结构、格式化、锁及底层 I/O；当前 `std.io` 使用 C 包装 |
| 文件、网络、进程 | 必须通过平台系统调用层，不属于纯算法 |
| 线程、locale、NSS、动态链接 | 当前语言与运行时能力不足 |

## 首批纯 Kelyra 模块

当前实现：

- `std.util.ascii`：ASCII 字节分类及大小写转换；
- `std.math.integer`：`i64` 基础整数工具；
- `std.math.basic`：`f32`/`f64` 基础浮点工具。

函数名暂时携带类型后缀，例如 `abs_i64`、`abs_f64`。Kelyra 尚无重载和泛型，
因此不通过复制大量同构实现来伪装完整数值库。

这些函数当前遵循 Kelyra 的普通算术和比较语义：`abs_i64`/`gcd_i64` 不接受
最小 `i64`，调用方负责避免整数溢出；浮点 `min`/`max` 不承诺 libm 的 NaN
处理规则。等语言定义溢出和浮点边界语义后，再统一收紧这些契约。

## 后续解锁顺序

1. 指针索引/算术、`null`、显式转换、位运算和移位；
2. native class 和字段访问已实现，内建 slice 仍待实现；
3. 枚举、模块常量、函数指针、全局/TLS 和原子操作；
4. 独立的平台系统调用层及 freestanding 启动/链接模式；
5. 在这些基础上实现 `std.memory`、`std.util.string`、`std.convert`、
   `std.algorithm`、分配器、文件、线程和格式化 I/O。

完整 libm、locale/iconv、NSS、pthread 和动态链接器都应在实际平台需求出现后再
规划，不能作为标准库自举的首批目标。
