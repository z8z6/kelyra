# Platform API modules

Kstd implements its OS calls in Kelyra. `std.os` is the public entry point.
Its `@cfg(os=..., arch=...)` imports select `std.os.linux` or `std.os.windows`
before module loading. Neither backend has a C implementation file.

The Linux import also requires `arch="x86_64"` in the same annotation. Other Linux
architectures do not currently provide `std.os` functions; the x86-64 assembly
is excluded from those builds. The Windows import also requires x86-64,
because its external declarations use the Windows x64 system ABI. Other
architectures have no `std.os` implementation yet.

| API | Linux x86-64 | Windows x64 |
| --- | --- | --- |
| Process ID | `process_id() -> i64`, syscall 39 | `process_id() -> u32`, `GetCurrentProcessId` |
| Monotonic milliseconds | `monotonic_millis() -> i64`, syscall 228 | `monotonic_millis() -> i64`, `QueryPerformanceCounter` |
| Sleep | `sleep_millis(i64) -> i64`, syscall 35; 0 or positive errno | `sleep_millis(u32) -> void`, `Sleep` |
| Environment | `environment(*c.char) -> *c.char`, borrowed `getenv` pointer or null | `environment(*c.char, *u8, u32) -> u32`, `GetEnvironmentVariableA` |
| Current directory | `working_directory(*u8, usize) -> i64`, syscall 79; bytes including NUL or negative errno | `working_directory(*u8, u32) -> u32`, `GetCurrentDirectoryA` |

The Linux backend uses Kelyra inline assembly and a `@layout(Layout.C)`
timespec class. Its syscall numbers, register bindings and layout are specific
to Linux x86-64. `sleep_millis` retries after `EINTR` and returns `EINVAL`
(22) for a negative duration. The monotonic clock has no calendar-time
meaning. `environment` still calls the host libc's `getenv`, because Kelyra
cannot currently declare the process's external `environ` symbol. Its result
is borrowed and can be invalidated by environment changes; do not free it.

The Windows module wraps Kernel32 and exposes `last_error()` so callers can
inspect `GetLastError` immediately after a failed call. OS errors are not
normalized to Linux errno values. Environment and directory functions use
caller-owned byte buffers and the ANSI `A` API, so Unicode paths and values
are not handled losslessly. The target program must link Kernel32. External
signatures are declared directly in `windows.kly` with `@extern` and
`@callconv(CallingConvention.System)`; Linux's `getenv` is similarly declared in `linux.kly`.
These platform wrappers need no C header or C implementation.

The Kelp manifest does not import `std.os` into its freestanding-safe library
object, because Linux `environment` references libc `getenv`. Import `std.os`
directly from source in a host program. Kelyra determines the target OS from
`--target` or the native LLVM target triple, then filters inactive imports and
declarations before semantic analysis. Kelp needs to pass the selected target
through to Kelyra for cross builds. The Windows backend has not been run on
Windows.

`sh tests/os.sh` builds and runs the Linux module, testing process ID,
monotonic time, sleep, environment and current directory. The standard library
object's alloc/io/file/string modules have no C runtime. The optional
`std.os.environment` still requires libc on Linux. Freestanding startup and
linking are available in Kelyra for Linux x86-64.
