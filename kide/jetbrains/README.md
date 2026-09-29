# Kelyra for JetBrains IDEs

This plugin provides Kelyra and Kelp syntax highlighting through the bundled
TextMate grammar, and `.kly` language features through the IDE's LSP client and
`kelyra-ls` on `PATH`. It requires JetBrains IDE 2025.2 or newer with the
bundled TextMate plugin.

Build the plugin from this directory with `gradle buildPlugin`, then install
the ZIP in `build/distributions/` using **Install Plugin from Disk**. The build
copies the shared highlighting grammar from `../vscode/syntaxes/`.
Set `KIDE_IDEA_HOME` to a local IntelliJ IDEA installation to build without
downloading the IDE. Otherwise Gradle resolves the pinned 2025.2.6.1 platform.
For environments where Gradle worker processes cannot connect locally, set
`KIDE_JAVAC` to the path of a JDK 21 `javac` executable.
