# Kelyra documentation site

`doc/` is the public VitePress site. `compiler/doc/` contains implementation and design notes. Site articles should describe code that works in the current repository and mark limitations explicitly.

```sh
npm ci --prefix doc
npm run dev --prefix doc
npm run build --prefix doc
```

The site reuses the Kelyra TextMate grammar from `kide/syntaxes/` and imports tested `.kly` examples from `compiler/tests/cli/` and `kstd/examples/`. When an article uses `<<<` to show a source file, the build expands that snippet in its downloadable Markdown copy. The Markdown copies live in `.vitepress/dist/markdown/` and are generated after VitePress renders the site.

Set `DOCS_BASE=/kelyra/` for the GitHub Pages repository path. The Pages workflow handles this automatically. The PDF action opens the browser's print dialog, where readers can choose **Save as PDF**; the print stylesheet removes site navigation and article controls.
