# Kide

- Run validation and packaging commands from this directory.
- Validate changes with `npm test`.
- Package the VS Code extension with
  `mkdir -p build && vsce package --out "build/kelyra-$(node -p 'require("./package.json").version').vsix"`.
- Keep generated VSIX files under `build/`; do not commit `build/`, `node_modules/`, or VSIX files.
