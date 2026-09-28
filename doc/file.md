# File and byte I/O

`std.io.file` and the byte-oriented part of `std.io` have the same Kelyra
signatures on Linux x86-64 and Windows x64. Both are implemented in `.kly`.
Linux uses the `openat`, `read`, `write` and `close` syscalls. Windows uses
Kernel32 `CreateFileA`, `ReadFile`, `WriteFile` and `CloseHandle`.

```kelyra
import std.alloc;
import std.io.file;
import std.io;

let handle: i64 = std.io.file.open("notes.txt", 0);
if handle >= 0 {
  let buffer: *u8 = std.alloc.allocate(4096);
  let count: i64 = std.io.file.read(handle, buffer, 4096);
  if count > 0 {
    std.io.write_stdout(buffer, count as usize);
  }
  std.alloc.release(buffer);
  std.io.file.close(handle);
}
```

`std.io.file.open(path: *c.char, mode: u32) -> i64` takes a NUL-terminated path.
Mode `0` opens an existing file for reading. Mode `1` creates or truncates a
file for writing; on Linux it uses permissions `0644` before the process umask.
Other modes are invalid. The returned handle is opaque: it is a Linux file
descriptor or a Win32 HANDLE carried in an `i64`. On failure `open` returns a
negative native error code.

`std.io.file.read(handle, output, count)` and `std.io.file.write(handle, data, count)`
return the actual byte count, which can be less than `count`; zero from `read`
means EOF. Both return a negative native error code on failure. The Windows
backend limits a single call to `0xffffffff` bytes. `std.io.file.close(handle)`
returns zero on success or a negative native error code on failure. A handle
must not be used after a successful close.

`std.io.read_stdin`, `write_stdout` and `write_stderr` use the same
`(*u8, usize) -> i64` byte-count/error convention. The text helpers
`print`, `println`, `eprint`, `eprintln`, `print_int`, `read_line` and `read_int`
are built from those operations in Kelyra. `read_line(output, capacity)`
requires a caller-owned buffer; it appends a NUL terminator and returns its
non-NUL byte count.

Negative errors are Linux errno values or Win32 `GetLastError` values. This
low-level API does not normalize them. Windows paths use the ANSI `A` API and
are not lossless for all Unicode filenames. A future UTF-16 path API is needed
for that case. In freestanding mode, the Linux backend needs no libc; the
Windows backend uses the Kernel32 import library named by its `@extern`
declarations.
