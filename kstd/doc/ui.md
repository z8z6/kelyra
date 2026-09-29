# UI components

`std.graphics.ui` is a platform-neutral retained component tree. `std.graphics.ui.win.Window`
owns the Win32 window and forwards input and size changes to the tree. The
window's `handle()` can also be used by a Vulkan or DX12 swapchain.

```kelyra
import std.graphics.ui;
import std.graphics.ui.win;

let window = std.graphics.ui.win.Window("Controls", 800, 600);
let panel = std.graphics.ui.Panel(10, 10, 780, 580);
panel.set_anchors(std.graphics.ui.Anchor.Stretch, std.graphics.ui.Anchor.Stretch, 10, 10);
let button = std.graphics.ui.Button("Apply", 20, 20, 100, 30);
panel.add(&button);
window.ui.add(&panel);
window.set_ui_paint(true);
while window.poll() { }
```

`Component` combines the `Layoutable`, `Renderable`, and `EventTarget`
interfaces. `Widget` implements them with virtual methods and shared geometry,
tree, visibility, and event state. `Panel`, `Label`, `Button`, and `Slider`
inherit `Widget`; user components can do the same and override `measure`,
`arrange`, `paint`, or `on_event`. The renderer uses the `Painter` interface;
`std.graphics.ui.win.gdi.GdiPainter` is the Win32 implementation. Control behavior lives
in the component, without numeric type tags in the window controller.

`bounds` is relative to the parent; `frame` is the resolved client-area
rectangle. `Anchor.Start` preserves a near-edge position, `Anchor.End` uses
the far-edge margin, and `Anchor.Stretch` retains the near position and
far-edge margin while changing size. The root re-arranges descendants after
resize and pointer events. Override `measure` for a component's preferred
size and `arrange` for custom child placement. A custom `arrange` should call
`super.arrange(frame)` when it wants the default anchor layout.

The tree borrows component pointers. Application-owned controls and text must
remain alive while attached. `Widget.detach()` removes a component from its
parent. `Window.remove(child)` does the same for any descendant owned by the
window. Detaching a subtree clears focus and pointer capture within it;
destruction also detaches a component. Adding a component rejects cycles and
already attached children.

`Widget.set_handler` installs an optional callback for application actions.
The virtual `on_event` method handles component-specific behavior: Button
generates `Click` on pointer release or Enter/Space, and Slider updates its
normalized value while pressed. `Window` routes pointer capture, drag, focus,
keyboard, resize, and close events. The Win32 host captures the mouse during a
press so dragging continues outside the client area.

The Windows host queues messages from its window procedure, including messages
sent synchronously while a window is created, shown, or resized. Call `poll()`
to deliver them to the UI tree. `Event` carries left, right, middle, and X
mouse buttons and double clicks; vertical and horizontal wheel deltas (in native wheel units);
signed client coordinates; Shift, Ctrl, and Alt modifier bits (1, 2, and 4);
and keyboard virtual key, scan code, repeat count, previous-key state, and
extended-key state. `TextInput.character` is a Unicode code point, with UTF-16
surrogate pairs combined by the host. `PointerLeave`, `CaptureLost`, `Focus`,
and `Blur` are also forwarded. Mark a `Close` event as handled to keep the
window open. The low-level `std.win.input` module exposes Win32 input helpers
when native details are needed.

`set_ui_paint(true)` enables GDI backgrounds, outlines, UTF-8 labels, and
slider drawing. Call `window.repaint()` when application code changes a
component outside input handling. GPU swapchain hosts can leave GDI disabled
and pass another `Painter` implementation to `window.ui.paint`.

`examples/ui_layout_example.kly` checks layout and input,
`examples/ui_win_smoke_example.kly` checks the native window and GDI output,
`examples/ui_input_smoke_example.kly` checks synchronous input messages,
and `examples/ui_controls_example.kly` opens an interactive UI.
