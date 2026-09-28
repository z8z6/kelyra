# 跨平台分配器与系统 ABI 路线

## 目标

`std.alloc` 最终不再依赖 `malloc`、`realloc`、`free` 或随库编译的 C
实现文件。首批运行目标是 Linux x86-64 和 Windows x86-64，且保持当前公开
接口不变：

- `allocate`、`reallocate`、`release`；
- `copy`、`fill`、`load`、`store`；
- `is_null`。

这里的“标准库自举”分为三个层级：

1. `std.alloc` 不依赖宿主 allocator；
2. Kstd 不包含 C 实现文件；
3. 最终程序不依赖 libc、CRT 或宿主启动代码。

本路线先完成前两项。第三项还需要独立的进程入口和链接模式，不属于分配器
改造。

## C 语言与系统 ABI

调用 Win32 API 不要求 Kelyra 包含 C 编译器、C 头文件或 libc。Kelyra 需要的
是外部 ABI/FFI：外部符号声明、参数布局、调用约定和链接库信息。Win32 API
通常由 C 头文件描述，但它在二进制层面是 DLL 导出函数。

规划中的直接声明形式如下；具体语法以 Kelyra 实现为准：

```kelyra
@link("kernel32")
extern "system" fn VirtualAlloc(
  address: *void,
  size: usize,
  allocation_type: u32,
  protection: u32,
) -> *void;
```

这与现有 `import c "header.h"` 是两个不同层次：

| 能力 | 用途 | 是否依赖 C 头文件解析 |
| --- | --- | --- |
| `import c` | 使用大型既有 C 库 | 是 |
| `extern "C"` / `extern "system"` | 直接声明稳定的外部 ABI | 否 |

标准库的平台后端使用后一种能力；C importer 继续作为第三方库互操作工具。

这种设计与其他跨平台语言一致：Rust 为 Windows API 推荐
[`extern "system"`](https://doc.rust-lang.org/reference/items/external-blocks.html)，
Zig 可以直接声明 DLL 函数并指定
[`callconv(.winapi)`](https://ziglang.org/learn/samples/)，Go 通过 Windows DLL
过程调用层访问系统函数，.NET 则使用
[P/Invoke](https://learn.microsoft.com/en-us/dotnet/standard/native-interop/pinvoke)。
它们都具备外部 ABI 边界，但不要求标准库实现本身使用 C。

## 平台后端

```text
std.alloc 公共契约
        |
        +-- Linux x86-64: mmap / munmap syscall
        |
        `-- Windows x86-64: VirtualAlloc / VirtualFree
```

Linux 后端使用 `mmap` 和 `munmap`。不采用 `brk`：它要求维护全局堆顶、空闲
块和同步，还可能与进程内其他 allocator 冲突。`mremap` 也不进入公共契约，
因为 Windows 没有等价操作。

Windows 后端通过普通系统 ABI 调用 `VirtualAlloc` 和 `VirtualFree`，不使用裸
Windows syscall。Windows syscall 编号和入口不是稳定的应用 ABI；Kernel32
导出的 Win32 API 才是支持边界。第一版用 `MEM_RESERVE | MEM_COMMIT` 和
`PAGE_READWRITE` 分配，用 `MEM_RELEASE` 释放整块区域。相关契约见
[`VirtualAlloc`](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc)
和
[`VirtualFree`](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualfree)。

## 第一版内存布局

每次用户分配对应一块 OS 映射：

```text
映射基址
+----------------+----------------+--------------------+
| requested size | mapped size    | 用户数据           |
| 8 bytes        | 8 bytes        | 返回这个地址       |
+----------------+----------------+--------------------+
```

16 字节头部保存用户请求长度和平台释放所需的映射长度，同时保持返回地址的
16 字节对齐。公共语义为：

- 大小加头部发生溢出时返回 null；
- `allocate(0)` 返回一个可传给 `release` 的最小块，失败时仍可返回 null；
- `release(null)` 不执行操作；
- `reallocate(null, size)` 等同 `allocate(size)`；
- 扩缩容采用“分配新块、复制 `min(old, new)` 字节、释放旧块”；
- 扩缩容失败时返回 null，旧块保持有效。

第一版不实现 slab、arena、空闲链表或线程缓存。一块映射对应一次分配，性能
上限明确，但实现无共享 allocator 状态，足以完成自举并天然适用于多线程。
只有基准确认系统调用成本成为问题后，才增加小块 allocator。

## Kelyra 前置能力

按依赖顺序实现以下最小能力：

1. 从 LLVM target triple 得到编译期 OS、架构和 ABI；
2. `extern "C"` 和 `extern "system"` 的无函数体声明；
3. 外部符号名、链接库和 LLVM calling convention lowering；
4. `null`、显式整数/指针转换和指针偏移或索引；
5. 在构建时只选择一个平台后端。

`"system"` 必须由目标决定实际调用约定。Windows x64 首期只有平台统一调用
约定，但保留 `system` 语义可以正确扩展到 Windows x86；调用约定不匹配可能
导致数据损坏。参见 Microsoft 的
[非托管调用约定说明](https://learn.microsoft.com/en-us/dotnet/standard/native-interop/calling-conventions)。

## 实施阶段

| 阶段 | 仓库 | 产物 | 验收条件 |
| --- | --- | --- | --- |
| 1 | Kelyra | 编译期目标信息 | 能区分 Linux/Windows 与 x86-64 |
| 2 | Kelyra | 原生 `extern "system"` | Windows 程序能直接调用一个 Kernel32 函数 |
| 3 | Kelp | 平台模块和系统库选择 | 同一 Kstd 清单能在两端构建 |
| 4 | Kstd | 两个平台的页分配后端 | 两端均可分配、读写和释放 |
| 5 | Kstd | 公共 `reallocate` 和内存原语 | 当前 String 行为保持不变 |
| 6 | Kstd | 删除 `alloc.c` 和 `alloc.h` | 不再引用 `malloc/realloc/free` |
| 7 | CI | Linux、Windows 原生运行测试 | 两个平台实际执行测试程序 |

交叉编译不作为第一阶段前提。先确保编译器分别在 Linux x86-64 和 Windows
x86-64 原生构建与运行，再增加 `--target`、sysroot、SDK 和交叉链接配置。

## 测试契约

平台共用同一组黑盒测试：

- 分配 0、1、15、16 和跨页大小的块；
- 写入并读回首、中、末字节；
- 扩容和缩容后验证共有范围内容不变；
- 多个分配互不影响；
- null 释放安全；
- 溢出请求返回 null；
- 重复执行分配、扩缩容和释放流程。

Linux 产物还应确认没有 `malloc/realloc/free` 未解析符号；Windows 产物允许导入
Kernel32，但不得因 allocator 引入 CRT heap API。
