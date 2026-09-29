# Kelyra for VS Code

This extension adds Kelyra (`.kly`) and Kelp (`kelp.toml`) language support.
It provides syntax highlighting, snippets, formatting through `kelyra-format`,
and diagnostics, completion, hover, definitions, and references through
`kelyra-ls`. The Kelp sidebar runs project check, build, run, test, and package
commands.

The highlighting and completions recognize interfaces, enums, `@main`, C ABI
declarations such as `@extern("CreateWindowExW", "user32")`, and shader
resource bindings such as `@std.graphics.binding(0, 1)`. Shader and font APIs
are supplied by `kstd`; the editor does not compile shaders itself.

Install the generated VSIX with **Extensions: Install from VSIX**. Set
`kelyra.languageServer.path`, `kelyra.formatter.path`, or `kelp.path` if those
executables are not on `PATH`.

For development, run `npm ci --ignore-scripts` and `npm test` in this directory.
The checked-in Tree-sitter WASM parser is ready to use; regenerate it with
`npm run build:grammar` when `grammar/tree-sitter-kelyra/grammar.js` changes.
