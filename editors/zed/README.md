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

The `.scm` files in `languages/mirage/` are **queries against that grammar**, not
part of it. A query can only match node types the grammar already produces — it
cannot introduce one. So any syntax change that needs a new node is a two-step
job: land it upstream first, bump `rev`, then write the query here.

### Known stale, in landing order

Neither of these can be fixed from this repository.

**`//` and `/* */` comments (landed 2026-07-27)** replaced the older `#` comment
syntax. `highlights.scm` already asks for `(comment) @comment` and `config.toml`
already declares the right delimiters for comment toggling, so nothing here needs
to change — but both depend on the upstream grammar's comment rule recognising the
new forms. If it still matches `#`, comments highlight wrongly in Zed while being
correct in VS Code, whose grammar lives in this repo
(`editors/vscode/syntaxes/mirage.tmLanguage.json`, pinned by
`tests/editor_grammar_test.py`).

**`?` on the last return type (landed 2026-07-30)** marks an ignorable error:

```mirage
fn alloc(size: usize) -> (anyptr, ?Allocator_Error)
fn touch(size: usize) -> ?error(Alloc_Error)
```

The grammar needs an optional-marker node on its return-slot rule — the `?` is
legal only on the **last** return type, so it belongs to that rule rather than
being a free-floating token. Once it exists, add a query for it here.

Until then `highlights.scm` scopes a bare `"?"` as `@operator`, which is the
ternary's scope. That is the right fallback: the marker gets operator colouring
rather than no colouring, and nothing is mis-scoped as a type. Do not try to
special-case it in a query — without a distinct node there is nothing to match on
that the ternary would not also match.

## Publishing

To publish for real (not just as a local dev extension), follow Zed's
[extension publishing guide](https://zed.dev/docs/extensions/developing-extensions#publishing-your-extension):
open a PR against [`zed-industries/extensions`](https://github.com/zed-industries/extensions)
adding this directory as a git submodule.
