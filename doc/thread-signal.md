# Thread and signal system APIs

`std.sync.thread` and `std.sync.signal` provide low-level OS calls on Linux x86-64 and
Windows x64. Their original wait, wake, ID, and event APIs remain freestanding
safe. Linux callback APIs use libc/pthreads and require a hosted executable.
Platform selection uses `@cfg(os="...", arch="x86_64")`.

| Module | Linux x86-64 | Windows x64 |
| --- | --- | --- |
| `std.sync.thread.current_id` | `gettid`, `i64` | `GetCurrentThreadId`, widened to `i64` |
| `std.sync.thread.yield_now` | `sched_yield`, zero or `-errno` | `SwitchToThread`, returns zero even if no peer was scheduled |
| `std.sync.thread.wait_word` | private futex wait, zero or `-errno` | `WaitOnAddress`, zero or negative `GetLastError` |
| `std.sync.thread.wake_one` / `wake_all` | private futex wake, count or `-errno` | `WakeByAddressSingle` / `WakeByAddressAll`, returns zero |
| `std.sync.signal` | `send_process` (`kill`) and `send_thread` (`tgkill`), zero or `-errno` | `send_console_event` (`GenerateConsoleCtrlEvent`) and `ignore_control_c` (`SetConsoleCtrlHandler(NULL, ...)`) |

`wait_word` compares one aligned 32-bit word and waits while it has the
expected value. Callers must change the word with an atomic operation *before*
waking other threads, and must recheck it in a loop after waking: spurious
wakeups and races are allowed. This module does not yet provide atomic writes
or a mutex, so it is a building block rather than a safe concurrency
abstraction. Linux `wait_word` returns `-11` (`EAGAIN`) if
the value already differs. Windows `wait_word` currently waits indefinitely.
The public `std.sync.thread` signatures use `i64` on both platforms; Linux `wake_*`
reports a nonnegative wake count whereas Windows returns zero because its API
does not report the count. The platform-specific modules retain their native
return signatures.

Linux signal numbers are kernel signal numbers; signal zero probes process or
thread existence/permission without delivery. Windows console control events
are **not** POSIX signals. `send_console_event` only accepts event 0 (`CTRL_C`)
or 1 (`CTRL_BREAK`), and requires a suitable shared console/process group.
`ignore_control_c` toggles a process-wide, inheritable state; use with care.

Callback APIs use Kelyra `fn(...) -> ...` values. On Linux, import
`std.sync.thread.hosted` for `spawn` and `join`, which wrap `pthread_create` and
`pthread_join`; the callback has type `fn(*u8) -> *u8`. Import
`std.sync.signal.hosted` for `install_handler`, which wraps libc `signal`, accepting
`fn(i32) -> void` and returning the previous handler. `SIG_ERR` is represented
by `(result as usize) == ((-1) as usize)`; check before using the result. Signal
handlers run asynchronously: use only operations that are safe in that context.
On Windows, `std.sync.thread.spawn`, `join`, and `close`
wrap `CreateThread`, `WaitForSingleObject`, `GetExitCodeThread`, and `CloseHandle`.
Its callback type is `fn(*u8) -> u32`; `spawn` returns a handle owned by the
caller, and `join` does not close it. `std.sync.signal.register_console_handler`
and `unregister_console_handler` wrap `SetConsoleCtrlHandler` with a
`fn(u32) -> i32` handler. Keep callback code and its context alive until the
thread exits or the handler is removed. Callback signatures must match the
native ABI exactly; these APIs do not support capturing closures.

`sh tests/callback.sh` exercises the hosted Linux thread and signal callbacks.
`examples/callback_windows_example.kly` provides a Windows x64 compilation example.

`sh tests/thread_signal.sh` runs a freestanding Linux smoke test. A Windows
cross build can use `--target=x86_64-pc-windows-msvc` and link both
`Kernel32.lib` and `Synchronization.lib`. The address-wait functions are
imported from the Windows synchronization API set, not Kernel32's import
library. The Windows smoke test was linked and run from WSL on Windows x64
with both import libraries and returned zero; `tests/thread_signal.sh` remains
Linux-only because its executable format and launcher are native to Linux.

The main kstd library imports `std.sync.thread`. Its Kelp manifest declares
`windows-import-libraries = ["Synchronization.lib"]`, which Kelp propagates
to Windows consumers of the library. Direct compiler users must still pass
the import library explicitly when linking a program that includes kstd.
