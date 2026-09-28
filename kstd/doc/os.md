# Portable operating-system APIs

`std.os` provides the same signatures on Linux x86-64 and Windows x64. Its
`@cfg` imports select `std.os.linux` or `std.win.system` at compile time.
The implementations are ordinary Kelyra code. Other architectures do not yet
have an implementation.

| Function | Contract |
| --- | --- |
| `process_id() -> i64` | Current process ID. |
| `monotonic_millis() -> i64` | Monotonic milliseconds, or -1 on clock failure. |
| `sleep_millis(i64) -> i64` | 0 on success; positive platform error for an invalid duration or syscall failure. |
| `environment(*c.char, *u8, usize) -> i64` | Required byte capacity including NUL; -1 if absent. Copies only when capacity is sufficient. |
| `working_directory(*u8, usize) -> i64` | Bytes written including NUL, or a negative platform error. |

The caller owns the output buffers. `environment` takes a NUL-terminated name
and returns the required size when a buffer is too small. Windows uses the ANSI
`GetEnvironmentVariableA` and `GetCurrentDirectoryA` functions, so values and
paths outside the active code page are not lossless. Windows-specific error
inspection is in `std.win.error` and `std.win.system`; the portable result
values do not turn Win32 errors into Linux errno values.

Linux uses x86-64 syscalls for process ID, clock, sleep and current directory.
`sleep_millis` retries after `EINTR`. Its environment lookup uses libc `getenv`
internally; `std.os` therefore remains a separate host module rather than part
of Kelp's freestanding-safe library object. Windows calls are declared in
`std.win.system` with `@extern` and `@callconv(CallingConvention.System)`.
The linker selects the Windows import libraries named by the declarations.

`examples/os_example.kly` checks these APIs. On Windows, set `KSTD_OS_TEST` and
compile the example with the Kelyra compiler and `--module-path=kstd/src`.
`sh tests/os.sh` is the Linux-only smoke test.
