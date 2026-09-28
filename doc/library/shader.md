# Shader 与图形

Kelyra 可以使用同一份受支持的 Shader 源码，分别输出 Vulkan SPIR-V 和 DirectX 12 DXIL。着色器仍写成普通 Kelyra 函数，入口用 `@vertex` 或 `@fragment` 标记。

## 三角形 Shader

这个示例来自仓库中的实际三角形程序：

<<< ../../kstd/examples/triangle_shaders.kly{kelyra}

`@vertex_index` 读取顶点索引；`@position` 指定顶点裁剪空间位置；`@location(0)` 指定片元输出。向量类型来自 `std.graphics`。

## 编译目标

```sh
kelyra --emit-spirv --shader-entry=vertex_main -o triangle.vs.spv kstd/examples/triangle_shaders.kly
kelyra --emit-dxil --shader-entry=vertex_main -o triangle.vs.dxil kstd/examples/triangle_shaders.kly
```

两条路径都先经过 Kelyra 的 Shader IR，然后直接 lower 到目标格式；DXIL 输出还需要验证与签名。具体调用参数和环境要求请结合[图形示例脚本](https://github.com/z8z6/kelyra/tree/main/kstd/examples)核对。

## 当前范围

首版 Shader 子集支持标量、数据类、直接调用、算术比较、局部变量、`if`、`while` 和返回。宿主指针、堆分配、切片和运行时库调用不能进入 GPU 调用图。**完整 HLSL 能力尚未实现**；语言与后端的详细限制见[Shader 说明](https://github.com/z8z6/kelyra/blob/main/compiler/doc/shader.md)。

Windows 上可运行 [DX12 三角形示例](https://github.com/z8z6/kelyra/blob/main/kstd/examples/d3d12_triangle.ps1)；仓库也提供 [Vulkan 三角形示例](https://github.com/z8z6/kelyra/blob/main/kstd/examples/vulkan_triangle.ps1)。两者的窗口与图形后端不同，共享的重点是 Shader 源与语言层接口设计。
