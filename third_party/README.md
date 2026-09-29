# Third-party sources

These directories are Git submodules pinned by the monorepo commit:

| Directory | Version | Purpose |
| --- | --- | --- |
| `freetype/` | 2.14.3 | Rasterize font glyphs |
| `harfbuzz/` | 14.5.0 | Shape Unicode text into positioned glyphs |
| `icu/` | 78.3 | Bidirectional text and line boundaries |
| `googletest/` | Pinned commit | C++ tests |

After cloning Kelyra, initialize them with:

```sh
git submodule update --init --recursive --depth 1
```

Build output belongs under the ignored `build/` directory. See each
submodule's license and notice files for its terms.
