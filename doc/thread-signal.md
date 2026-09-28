# Threads and signals

`std.sync.thread` provides common thread IDs, scheduler yield, and aligned
32-bit wait/wake operations on Linux x86-64 and Windows x64. Its signatures
use `i64` results on both platforms. The platform implementations are
`std.sync.thread.linux` and `std.win.thread`.

| Operation | Linux | Windows |
| --- | --- | --- |
| `current_id` | `gettid` | `GetCurrentThreadId` |
| `yield_now` | `sched_yield` | `SwitchToThread` |
| `wait_word` | private futex wait | `WaitOnAddress` |
| `wake_one`, `wake_all` | private futex wake, returns count | `WakeByAddressSingle`, `WakeByAddressAll`, returns 0 |

Change the word with an atomic operation before waking a waiter. Recheck it
in a loop after waking because races and spurious wakeups are possible.
`wait_word` on Windows waits indefinitely. This module does not provide an
atomic write or mutex abstraction.

`std.sync.signal` exposes Linux `send_process` (`kill`) and `send_thread`
(`tgkill`). Windows console control is a different facility, exposed as
`std.win.signal.send_console_event`, `ignore_control_c`,
`register_console_handler`, and `unregister_console_handler`. The Windows
handler signature is `fn(u32) -> i32` and must match the native ABI.

On Linux, hosted callbacks are in `std.sync.thread.hosted` (`spawn`, `join`
using pthreads) and `std.sync.signal.hosted` (`install_handler` using libc).
On Windows, `std.win.thread.spawn`, `join`, and `close` wrap `CreateThread`,
`WaitForSingleObject`, `GetExitCodeThread`, and `CloseHandle`. `spawn` returns
an owned handle; `join` does not close it. Keep callbacks and their context
alive until the thread exits or the handler is removed. Capturing closures
are not supported by these native callback signatures.

Other Windows synchronization primitives are grouped under `std.win.wait`,
`std.win.event`, and `std.win.handle`. `WaitOnAddress` and its wake functions
use the `Synchronization` import library, selected by `@extern`.

`sh tests/thread_signal.sh` and `sh tests/callback.sh` exercise the Linux
paths. `examples/thread_signal_example.kly` and
`examples/callback_windows_example.kly` exercise the Windows paths.
