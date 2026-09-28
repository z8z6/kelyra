# kide

Kelyra editor support for VS Code and JetBrains IDEs.

## VS Code

The extension provides `.kly` syntax highlighting, comments, brackets,
indentation, document formatting through `kelyra-format`, and hover,
completion, diagnostics, definition, and reference navigation through
`kelyra-ls`.
Go to Definition follows whatever the compiler can see: workspace members,
imported modules (including dependency sources under `.kelp/dependencies` and
local path dependencies), class members, and C declarations from
`import c "header.h"`, where the header string itself opens the header.
Find All References covers locals in their scope, class fields and methods
(including accesses through pointers and `this`), functions across the
workspace, and every import of a module.
Highlighting covers annotations, compile-time `when`/`meta` reflection,
prefix pointers, inline assembly chains, and public module imports.
Functions, module paths, variables, parameters, class fields, and member
accesses each have their own scope, so they can be colored independently:

| Element | Scope |
| --- | --- |
| Module path (`module`, `import`, qualified call) | `entity.name.namespace.kelyra` |
| Function, at its declaration and at each call | `entity.name.function.kelyra` |
| Class and annotation name | `entity.name.type.class.kelyra`, `entity.name.tag.kelyra` |
| Local variable | `variable.other.readwrite.kelyra` |
| Function parameter | `variable.parameter.kelyra` |
| Class field and member access | `variable.other.member.kelyra` |
| `this` | `variable.language.this.kelyra` |
Built-in types and annotations have documented completion and hover support;
keywords and annotations declared in the current document are also suggested.
Classes use `class`, `init`, `deinit`, and the implicit `this` pointer. These
keywords have highlighting and inline help; `kelyra-ls` supplies class member
completion, hover, and definition navigation.
Snippets cover functions, classes, control flow, assembly, annotations, and
Kelp manifests, and `[kelyra]`/`[kelp]` default to two-space indentation.
Braces and block comments fold, and `kelp.toml` completes section headers.
Parameter-name inlay hints appear at call sites: set
`kelyra.inlayHints.parameterNames` to `literals` (default), `all`, or `off`.
Hints for other modules come from indexing workspace `.kly` files and the source
trees of local Kelp path dependencies, including those outside the opened
folder. Hints are
available for the current file immediately; the cross-module index is refreshed
in the background and cached in the workspace's `.kelp/kide-index.json` for
later sessions. The cache is disposable and is rebuilt from source. Hovering a
simple `const` declaration, its expression, or a reference to it shows the
computed value when the editor can evaluate it safely. Code disabled by
`@cfg(os="...", arch="...")` is dimmed according to `[build].target` in the
nearest `kelp.toml`, falling back to the host target. Hints are styled like
inline code: `[kelyra]` defaults them to a smaller, padded font
(change it with `"[kelyra]": { "editor.inlayHints.fontSize": ... }`). To give
them a code-span background, add colors to your settings:

```json
"workbench.colorCustomizations": {
  "editorInlayHint.parameterBackground": "#2E3440",
  "editorInlayHint.parameterForeground": "#88C0D0"
}
```

The extension maintains a Tree-sitter syntax tree for each open `.kly` file.
Text edits incrementally update that tree; hover, cfg dimming, and parameter
hints reuse its tokens. Until the WASM parser finishes loading, the existing
scanner remains available as a fallback. Semantic checks and navigation still
come from the C++ compiler through `kelyra-ls`. The editor grammar is in
`grammar/tree-sitter-kelyra`; run `npm run build:grammar` after changing it to
regenerate `assets/tree-sitter-kelyra.wasm` (the Tree-sitter CLI may download a
WASI SDK on the first build).

`.kly` and `kelp.toml` files have their own file icons, shown by
the default file icon theme and by any theme that leaves the language to VS Code.

Set `kelyra.colorScheme`, or run **Kelyra: Select Color Scheme**, to choose the
Laevatain (莱万汀, default), Jue (诀), or Perlica (佩丽卡) palette. The extension adds only
`source.kelyra`-scoped rules, so other languages and the VS Code interface keep
their current theme. Each palette automatically follows light and dark theme
changes. Choose `off` to remove the Kelyra rules and use the active theme's
colors. `.kly` uses the Kelyra artwork as its icon, while `kelp.toml` uses a
static geometric icon. Neither changes with the selected character palette.
It also recognizes `kelp.toml`, provides field hover and completion for
projects, `build.kind`, `[workspace]` members, and Git or local path
dependencies, and adds a Kelp activity-bar view with format, compile, debug,
run, test, package, and members actions. Compile and check tasks feed a
contributed problem matcher, so `kelyra` diagnostics also appear in the
Problems panel.
VS Code's built-in **Format Document** command formats `.kly` files through
`kelyra-format`, preserving undo and unsaved edits. The canonical style uses
two-space indentation, one statement per line, spaces around binary operators
and after commas, braces on the declaration or control-flow line, sorted and
deduplicated imports, blank lines between top-level sections, and a final
newline. `[kelyra]` files default to this formatter and to format-on-save. Set
`kelyra.formatter.path` for custom installations; the default checks `PATH` and
`build/bin` or `kelyra/build/bin` in the source file's ancestor directories.

### Projects view

The **Kelp** activity bar has a **Projects** view listing the workspace and its
subprojects. Member data comes from `kelp members`, so the tree shows each
project's build kind and output artifact; when the executable is unavailable the
view falls back to scanning `kelp.toml` manifests, following `[workspace]
members` and nesting projects by directory. Right-click a project for check,
build, run, debug, test, and package, to reveal its output path, or to open and
reveal its manifest. The status bar shows the active project's artifact and
copies the path when clicked, and **Kelp: Show Output Path** prints it to the
Kelp output channel. Kelp also contributes `kelp` build tasks, so the commands
appear in **Tasks: Run Task** and can be bound to `launch.json` pre-launch
tasks. In `kelp.toml`, Go to Definition on a `[workspace] members` entry or a
dependency `path` jumps to the referenced project's manifest.

### Local GDB debugging

Install local **GDB** and the **Microsoft C/C++** extension (`ms-vscode.cpptools`).
Use a Kelp version supporting `output` and `build --debug`. Clicking **Debug**:

1. Selects the active editor's project (or asks for a workspace folder), saves
   files, and reads the executable path from Kelp, including custom build outputs.
2. Runs `kelp build --debug` at `-O0` without modifying `kelp.toml`.
3. Starts a `cppdbg` session using local GDB, stops at entry, and opens Run and
   Debug plus the Debug Console. No `launch.json` is required or overwritten.

Configure `kelp.debug.gdbPath` (default `gdb` on `PATH`) and `kelp.debug.args` as needed.
The Kelp sidebar links to **Variables**, **Functions / Call Stack**, **Watch
Expressions**, and **Evaluate / Debug Console**. Call Stack lists the active
function frames, not all functions in the project. Evaluation uses GDB's expression
syntax; it is not a Kelyra interpreter. You can use `-exec info functions` in the
Debug Console to list symbols.

Use the updated Kelyra compiler: `-O0` emits local/parameter names, types, storage
locations, and lexical scopes, including pointers, arrays, class fields and
destructured return values. Optimized builds currently omit this metadata.
Evaluation uses the selected stack frame. Imported C records remain opaque and
wide `f256`/`f512` values do not yet have native debug locations.
See the [VS Code GDB configuration reference](https://code.visualstudio.com/docs/cpp/launch-json-reference).

```sh
npm test
mkdir -p build
vsce package --out "build/kelyra-$(node -p 'require("./package.json").version').vsix"
```

Build `kelyra-ls` and `kelyra-format` and install the generated VSIX. The
extension discovers `build/bin/kelyra-ls` in a workspace ancestor; set
`kelyra.languageServer.path` if it is installed elsewhere. Set
`kelyra.formatter.path` if the formatter is not on `PATH` or in a workspace
build directory.

## JetBrains

The plugin provides `.kly` syntax highlighting, comments, brackets, formatting,
hover, completion, diagnostics, and definition navigation. It also highlights
`kelp.toml`. Syntax support uses the bundled TextMate plugin; language features
use the IDE's native LSP client and `kelyra-ls` from `PATH`.

```sh
cd jetbrains
gradle buildPlugin
```

Install the ZIP from `jetbrains/build/distributions` with **Install Plugin
from Disk**. JetBrains IDE 2025.2 or newer is required.
