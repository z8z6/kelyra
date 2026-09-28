# 用 Kelp 管理项目

Kelp 是仓库自带的项目管理器。它读取 `kelp.toml`，负责检查、构建、运行和测试项目。

## 创建项目

构建仓库后，在希望放置项目的目录运行：

```powershell
<仓库路径>\build\bin\kelp.exe new hello
cd hello
<仓库路径>\build\bin\kelp.exe build
<仓库路径>\build\bin\kelp.exe run
```

将 `<仓库路径>` 换成实际的绝对路径。如果已把 `build/bin` 加入 `PATH`，可以直接使用 `kelp`。

## 项目清单

`kelp.toml` 说明入口、输出和依赖。下面是核心部分：

```toml
[project]
name = "hello"
version = "0.1.0"
entry = "src/main.kly"

[build]
kind = "executable"
output = "hello"

[dependencies.kstd]
path = "../kstd"
```

实际路径要与项目位置相符；`kelp new` 生成的清单可以作为起点。

## 常用命令

| 命令 | 用途 |
| --- | --- |
| `kelp check` | 检查项目 |
| `kelp build` | 构建项目 |
| `kelp run` | 运行可执行项目 |
| `kelp test` | 运行清单里的测试源文件 |
| `kelp output` | 打印配置的产物路径 |
| `kelp members` | 查看工作区成员 |

Kelp 支持多成员工作区。详细配置和命令约定见[项目说明](https://github.com/z8z6/kelyra/blob/main/kelp/README.md)。
