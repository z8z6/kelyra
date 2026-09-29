# Windows API bindings

The Windows x64 declarations live under `std.win`. Import only the modules
needed by a program. These modules are ordinary Kelyra source files using
`@layout(Layout.C)`, function pointer fields, `@callconv(cc.System)`,
and `@extern("symbol", "import-library")`.

| Module | First-version coverage |
| --- | --- |
| `std.win.window` | Window classes, wide-character window creation, window procedure, message loop, client rectangle. |
| `std.win.message`, `input` | Named window-message codes, send/post helpers, packed mouse and keyboard data, capture and coordinate conversion. |
| `std.win.thread`, `event`, `wait`, `handle` | Native thread callback and handle, thread ID, address wait/wake, events, object waits, handle closing. |
| `std.win.guid`, `com` | GUID storage, `IUnknown` vtable, reference counting, apartment initialization, instance creation, COM task memory, GUID conversion. |
| `std.win.dxgi` | Factory, WARP adapter, window swap chain, presentation, buffers. |
| `std.win.d3d12` | Device, queue, allocators, command lists, descriptors, graphics pipeline, resources, barriers, fences, draw commands. |
| `std.win.d3dcompiler` | HLSL compilation with `D3DCompile`. |
| `std.win.system`, `error`, `memory`, `file`, `io`, `signal` | Process/time/environment, last error, virtual memory, file and console I/O, console control. |

These are the SDK declarations needed for the first rendering path and common
host operations. They do not yet enumerate every Win32, COM, DXGI or Direct3D
interface, method, structure, flag and enum. COM interface pointers are
represented as `*u8`; `std.win.com.method` reads a vtable slot by index.
The slot number and function signature must match the requested interface.
Returned interface pointers own one reference; release each owned reference.

`D3D12_RESOURCE_BARRIER` contains a C union. `std.win.d3d12.ResourceBarrier`
stores the tag plus the full 24-byte payload; its transition, aliasing and UAV
helpers write the matching payload. Kelyra's C importer already supports
`c.union`, so a new language union annotation was not needed for this version.
Importing the whole `d3d12.h` is still blocked by unsupported declarations in
that header. A general importer improvement should retain usable declarations
when unrelated declarations cannot be translated. Aggregate return ABI support
would also let Kelyra expose certain COM methods with native by-value signatures
instead of explicit output-storage wrappers.

`@extern` names an import library, with or without `.lib` or `.dll` suffix;
the linker resolves it at build time. It does not dynamically load a DLL at
runtime. If a DLL and its import library have different base names, name the
import library explicitly. The sample requires the Windows SDK import libraries
for Kernel32, User32, Ole32, DXGI and D3D12.

Applications can use `std.graphics.window.run_backend` to select Direct3D12 or
Vulkan on Windows. Its Win32 host and UI event loop are written in Kelyra.
Direct3D12 uses `std.win.graphics.window`; Vulkan uses `std.graphics.ui.win.Window` and
the native Vulkan adapter. The shader bytecode is DXIL or SPIR-V respectively.
Linux window presentation remains future work.

The component host at `std.graphics.ui.win.Window` receives sent and posted
Win32 messages in its window procedure, then delivers typed `std.graphics.ui.Event`
values during `poll()`. Raw Win32 messages remain available through
`std.win.window.Message`. `kstd/tests/ui.ps1` builds and runs the UI behavior
and native input examples on Windows.

Build and run the pure Kelyra triangle sample from PowerShell:

```powershell
pwsh -NoProfile -File kstd/examples/d3d12_triangle.ps1
```

Pass `-BuildOnly` to compile without opening a window. The script uses
`build/bin/kelyra.exe`, the project's bundled Clang, and Kelyra sources
from `kstd/src`. It compiles `examples/triangle_shaders.kly` to DXIL with DXC,
then writes the executable and bytecode under the ignored `kstd/build/`
directory. The application imports only `std.graphics.window` for window and
rendering operations. The backend creates a Unicode window, records a DX12
triangle draw, presents it, and releases its native resources on exit. Install
DXC and put `dxc.exe` on `PATH` to build the shader bytecode.
