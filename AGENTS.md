# Kelyra Monorepo

- `compiler/`, `kstd/`, `kelp/`, and `kide/` belong to this repository.
  Commit their changes from the monorepo root.
- `kelyra-llvm/` is a submodule of the Kelyra LLVM fork. Commit LLVM
  changes in that fork, then update the submodule pointer in this repository.
- Format C++ using the LLVM style and name class member variables in
  PascalCase.
- Validate compiler and Kelp changes with `ctest --test-dir build`, standard
  library changes with the relevant `kstd/tests/` scripts, and editor changes
  with `npm test --prefix kide`.
- Do not commit generated `build/`, `.kelp/`, `node_modules/`, or VSIX files.
- Before developing a new feature, study relevant designs in Rust, Go, Zig,
  and other languages. Present a design proposal and wait for review before
  implementing it.
- Implement features through general language mechanisms. Standard library
  code must use ordinary Kelyra syntax and behavior.
- Ask about important unresolved questions before making implementation
  decisions.
