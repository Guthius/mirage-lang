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

### Grammar status

Both syntax changes that were outstanding here have landed upstream:

- **`//` and `/* */` comments** (2026-07-27) replaced the older `#` syntax.
  `highlights.scm` already asked for `(comment) @comment` and `config.toml` already
  declared the right delimiters, so nothing here needed changing — but both depended
  on the upstream grammar recognising the new forms.

  Removing `#` as a comment character had a consequence worth recording: `#` is the
  sigil for compile-time directives (`#link`, `#error`, `#warn`), and while it *was*
  the comment character those were silently swallowed. The grammar therefore gained
  rules for them at the same time, or every directive would have become a parse error
  — a whole line of red. `highlights.scm` scopes them as `@keyword.directive`.

- **`?` on the last return type** (2026-07-30) marks an ignorable error:

  ```mirage
  fn alloc(size: usize) -> (anyptr, ?Allocator_Error)
  fn touch(size: usize) -> ?error(Alloc_Error)
  ```

  The grammar now emits an `optional_error_marker` node for it, so it can be told
  apart from the ternary `?`. Without a distinct node the two are the same token and
  no query can separate them, which is why `highlights.scm` used to scope a bare `"?"`
  as `@operator` and leave it there. It is now `@keyword.operator` — the marker is part
  of the TYPE, saying how the caller must treat the result, rather than an operator
  applied to two operands.

If `rev` in `extension.toml` is older than those changes, comments and the `?` marker
will highlight wrongly in Zed while being correct in VS Code, whose TextMate grammar
lives in this repository and is pinned by `tests/editor_grammar_test.py`.

## Publishing

To publish for real (not just as a local dev extension), follow Zed's
[extension publishing guide](https://zed.dev/docs/extensions/developing-extensions#publishing-your-extension):
open a PR against [`zed-industries/extensions`](https://github.com/zed-industries/extensions)
adding this directory as a git submodule.
