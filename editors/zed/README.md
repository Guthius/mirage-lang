# Mirage for Zed

Zed extension for [Mirage](https://github.com/Guthius/mirage-lang): syntax
highlighting (via [tree-sitter-mirage](https://github.com/Guthius/tree-sitter-mirage))
plus diagnostics, hover, go-to-definition, and completion through `mirage-lsp`.

## Prerequisites

- `mirage-lsp` built and either on your `PATH`, or its location set via the
  `lsp.mirage-lsp.binary.path` setting (see below). Build it from the root of
  this repository with `just build`, which produces `build/mirage-lsp`.
- Rust's `wasm32-wasip1` target, for Zed to compile this extension:
  ```sh
  rustup target add wasm32-wasip1
  ```

## Installing as a dev extension

1. Open Zed.
2. Run `zed: install dev extension` from the command palette.
3. Select this directory (`editors/zed`).

Zed compiles the extension (Rust → WASM) and clones/compiles the
`tree-sitter-mirage` grammar automatically. Reload the window if `.mir` files
don't immediately pick up highlighting.

## Settings

`mirage-lsp` is located the same way `mirage build`/`mirage run` would find
any other tool: first an explicit path from settings, then your shell `PATH`.
To override the binary path/arguments, or to pass `MIRAGE_PATH` for
standard-library import resolution (relevant if Zed wasn't launched from a
shell that already has it exported), add to your Zed `settings.json`:

```json
{
  "lsp": {
    "mirage-lsp": {
      "binary": {
        "path": "/absolute/path/to/mirage-lsp",
        "env": {
          "MIRAGE_PATH": "/absolute/path/to/mirage/std"
        }
      }
    }
  }
}
```

All three fields (`path`, `arguments`, `env`) are optional; omit `binary`
entirely to just search `PATH` and inherit Zed's own shell environment.

## Updating the grammar

`extension.toml`'s `[grammars.mirage]` pins a specific commit of
[tree-sitter-mirage](https://github.com/Guthius/tree-sitter-mirage) by `rev`.
After pushing a grammar change there, bump `rev` to the new commit SHA here
and reload the dev extension.

## Publishing

To publish for real (not just as a local dev extension), follow Zed's
[extension publishing guide](https://zed.dev/docs/extensions/developing-extensions#publishing-your-extension):
open a PR against [`zed-industries/extensions`](https://github.com/zed-industries/extensions)
adding this directory as a git submodule.
