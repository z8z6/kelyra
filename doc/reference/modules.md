# 模块、注解与平台

一个 `.kly` 文件对应一个模块。模块名与路径对应：`module math.vector;` 通常位于 `math/vector.kly`。

```kelyra
module app.main;
import math.vector;

@main
pub fn main() -> i32 {
  return math.vector.answer();
}
```

声明默认只在本模块可见；`pub` 让其他模块可以使用。`import` 将公开名称带入当前模块，名称冲突时可使用完整限定名。

## 注解

注解以 `@` 开头，可修饰声明或参数。例如 `@main` 指定入口，`@virtual` 标记虚方法，Shader 使用 `@vertex` 和 `@fragment`。用户也可以声明带参数的注解。

## 条件编译

`@cfg` 可在顶层按目标系统和架构选择模块或声明。

```kelyra
@cfg(os="windows", arch="x86_64")
module app.windows;
```

条件在依赖加载和语义检查之前生效。跨目标链接仍需要相应平台工具链和库。平台 API 的示例见[窗口与 UI](/library/ui.md)。
