# 窗口与 UI

`std.ui` 以组件树组织界面。`Widget` 保存通用状态，`Panel`、`Label`、`Button`、`Slider` 等组件继承它；布局、绘制和事件行为通过接口约定与方法覆写组合。

## 一个可检查的组件示例

这个仓库示例建立窗口、面板和自定义组件，检查布局、绘制、点击与移除组件后的状态清理。它是行为测试，**不会显示原生窗口**。

<<< ../../kstd/examples/ui_component_example.kly{kelyra}

## 原生窗口

Windows 图形窗口示例目前使用 `std.graphics.window` 打开窗口并绘制三角形。它依赖编译好的 Kelyra Shader 与 DX12 环境。完整流程见[Shader 与图形](./shader.md)。

Linux 的 Vulkan 图形示例已有代码，但跨平台 UI 后端仍在设计与完善；不能把 `std.ui` 的行为测试等同于已完成的跨平台原生组件渲染。
