# Kstd

Modules are grouped by purpose: `std.util.*` contains value and text helpers,
`std.sync.*` contains concurrency primitives, `std.math.*` contains numeric
helpers, and `std.io.file` contains file operations. Core facilities such as
`std.memory`, `std.alloc`, `std.io`, `std.meta`, and `std.reflect` remain directly
under `std`.

Kstd is the standard library for Kelyra. Its pure Kelyra modules are:

- `std.annotation`: source declarations for compiler-recognized annotations;
  the compiler imports this module implicitly, so short annotation names work
- `std.util.ascii`: ASCII byte classification and case conversion
- `std.math.integer`: basic `i64` integer utilities
- `std.math.basic`: basic `f32` and `f64` utilities
- `std.util.option`: `Option(name, argument)` parses one command-line option. It
  recognizes an exact flag such as `--verbose` and an assigned value such as
  `--output=file.txt`. Inspect `matched`, `has_value`, and `value`.
- `std.util.result`: `Result<T, E>` holds a success value or an error value.
  Construct it with `success<T, E>(value)` or `failure<T, E>(error)`. Check
  `valid` before using `ok`, then call `value()` or `error()` for that branch.
  Neither type needs a default constructor; each result allocates storage for
  its active branch. `valid == false` indicates allocation failure.
- `std.reflect`: opt-in runtime type and field lookup for `@reflect` classes,
  including `has_field`, `field`, `FieldInfo.type()`, and checked field
  `read<T>`/`write<T>`. See [`doc/reflection.md`](doc/reflection.md).

Platform implementation modules use `@cfg` on their `module` declaration;
shared modules retain conditional imports to select the target implementation.

The current `std.io` module provides:

- `print`, `println`, `eprint`, and `eprintln`
- `newline` and `flush`
- `print_int` and `read_int`
- `read_stdin`, `write_stdout`, and `write_stderr`, returning byte counts or
  negative native error codes
- `read_line(output, capacity)`, using a caller-owned buffer

`std.alloc` uses Linux `mmap` or Windows `VirtualAlloc` and provides byte-level
memory access:

- `allocate`, `reallocate`, and `release`
- `copy` and `fill`
- `load` and `store` for one indexed byte
- `is_null`, so allocation failure is observable without a null literal

`@accessors` generates public get and set methods for a field. `@aspect(handler)`
wraps a function or method with an ordinary Kelyra handler. Import
`std.aspect` for its `Invocation<R>` and `VoidInvocation` callback types.

`std.util.string` builds an owned, mutable byte string on top of `std.alloc`:

```kelyra
import std.util.string;

let text = std.util.string.String("hello");
text.append(", world");
text.append_byte(33);
text.length();          // 13
text.at(0);             // 104
text.set(0, 72);
text.equals("Hello, world!");  // true
let bytes: std.util.string.StringSlice = text.view();
std.util.string.equal(bytes, std.util.string.from_cstr("Hello, world!")); // true
let owned = std.util.string.to_owned(bytes);
text.data_pointer();    // *c.char, null terminated
// deinit releases the buffer at the end of the scope
```

`String` owns its buffer and supports deep copying and moving. It has a single
constructor taking `*c.char`; use `String("")` for an empty value. `view()`
borrows its bytes, so keep the `String` alive and avoid mutation while using
the view. `assign_view`, `append_view`, and `equals_view` accept byte slices;
`append_view` requires a source that does not borrow the same `String`.
`StringSlice` is an alias for `[]const u8`.

`std.io.file` provides `open(path, mode)`, `read`, `write`, and `close`. Mode `0`
opens an existing file for reading; mode `1` creates or truncates a file for
writing. Handles and negative error values are native to the target OS; see
[`doc/file.md`](doc/file.md) for the byte I/O contracts.

`std.os` selects Linux x86-64 system calls or Win32 API wrappers at compile
time via `@cfg(os=..., arch=...)`. Both backends are written in Kelyra; see
[`doc/os.md`](doc/os.md) for APIs, platform selection and FFI limitations.

`std.sync.thread` exposes native thread IDs, scheduler yield and 32-bit wait/wake
primitives. `std.sync.signal` exposes Linux signal sending. Windows-specific
APIs, including thread handles, console handlers, windows, COM and DX12, live
under `std.win`; see [`doc/windows.md`](doc/windows.md). Linux callback APIs live in
`std.sync.thread.hosted` and `std.sync.signal.hosted` and require libc and pthreads.
See [`doc/thread-signal.md`](doc/thread-signal.md).

`std.graphics.window.run` provides one procedural rendering call on Windows
DX12 and a Linux Vulkan interface. Both use the same Kelyra shader source;
the target bytecode format is selected during shader compilation.

Kstd's alloc, io, file and string implementations are Kelyra sources; the
build has no C sources. Kelp uses the checked-in `kelp.toml` to build a library
object. Library sources live in `src/`, while runnable sample programs live in
`examples/`. `src/kstd.kly` imports the freestanding-safe modules. `std.os` remains
available as a separate host module because its Linux environment lookup
references libc `getenv`. `std.math.basic` remains source-only because Windows
floating-point object code currently requires the MSVC `_fltused` runtime
symbol. The boundary between pure library
code, platform code, and C compatibility is documented in
[`doc/glibc-scope.md`](doc/glibc-scope.md). The Linux/Windows allocator and
native system ABI plan is documented in
[`doc/platform-allocator.md`](doc/platform-allocator.md).

From the `kstd/` directory:

```sh
. ./env.sh
kelp check
kelp build
kelp test
```

`kelp build` produces the library object `.kelp/build/kstd.o`; a program links it
instead of recompiling the standard modules. The library uses `safe-level = 0`
so that its object has no host `abort`/`puts` dependencies in freestanding
programs.

`env.sh` aliases the monorepo Kelp and Kelyra build paths for the current shell.

A consumer depends on the library and links its object. With Kelp, point a
dependency at this repository and let Kelp pass `--external-path` and
`--link-input`; to compile directly, link the self-contained library object:

```sh
kelyra --emit-exe \
  --module-path=kstd/src --external-path=kstd/src \
  --link-input=kstd/.kelp/build/kstd.o \
  -o app main.kly
```

Then import it from Kelyra:

```kelyra
import std.io;

@main
pub fn main() -> i32 {
  std.io.println("hello");
  std.io.print_int(42);
  std.io.newline();
  return 0;
}
```

Test:

```sh
kelp test
sh tests/io.sh
sh tests/string.sh
sh tests/core.sh
sh tests/os.sh
sh tests/freestanding.sh
sh tests/thread_signal.sh
sh tests/callback.sh
sh tests/reflect.sh
```
