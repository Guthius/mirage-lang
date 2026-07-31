# Mirage — Deferred Work

What remains after working through `REVIEW_FINDINGS.md` (116 findings, 2026-07-29 → 30)
and then `OUTSTANDING.md` (2026-07-30 → 31, branch `fixes`). Both of those files have been
removed: every item in them is now either done or listed below.

Nothing here is a regression. Items are either a deliberate scope decision, a defect
discovered along the way that is larger than the work that found it, or something that
cannot be done from this repository at all.

Current baseline: build clean, ctest 4/4, 17 Python suites passing, 195/195 corpus
fixtures matching (no `known_broken` entries).

---

## 1. Deferred refactors

The "N near-identical implementations that have already drifted" class, deferred by
explicit decision twice now. None is a bug. Collectively they are the reason bugs like
CHECK-1 and TYPE-1/TYPE-2 existed, so consolidating reduces the rate of *future* drift
bugs rather than fixing any present one.

| Area | IDs |
|---|---|
| Call resolution | `CHECK-7`, `CHECK-8` |
| `check_expr` / `check_stmt` size (~1450 / ~625 lines) | `CHECK-9` |
| Codegen duplication | `CODEGEN-6`, `CODEGEN-7`, `CODEGEN-8`, `CODEGEN-12`, `CODEGEN-13` |
| Type/layout math | `TYPE-5`, `TYPE-6`, `TYPE-7` |
| Const-folders (three, hand-synced) | `TYPE-11` |
| Parser duplication | `PARSE-5`, `PARSE-6`, `PARSE-7`, `PARSE-8` |
| Sema duplication | `SEMA-9`, `SEMA-11`, `SEMA-12` |
| LSP | `LSPCORE-12`, `LSPH-8`, `LSPH-10` |

**Highest value first, if revisited:** `TYPE-11` — the three drifted const-folders were the
direct cause of TYPE-1, TYPE-2, and the unreported `when size_of(...)` wrong-branch bug. It
is also the largest (multi-day).

**Note on CODEGEN-12:** looks cheap but is not uniform. Two sites work on `llvm::Value*`,
the third (`const_binary`) on `llvm::Constant*`. A 2-site helper is mechanical; a 3-site one
needs templating.

---

## 2. A generic type cannot refer to its own instantiation

Found while rewriting `examples/dictionary`. Not in either review document.

```mirage
type node[T: type] = struct {
    value: T
    next: *node[T] = nil     // error: generic type instantiation cycle detected at 'node'
}
```

The same shape is fine for a non-generic type — `block_header` in `examples/mem` has
`next: *block_header`. This blocks every node-based generic container: linked lists, trees,
and chained hash tables.

**Why.** `instantiate_generic_type` (`type_resolver.cpp`) deliberately allocates no slot
before resolving the declaration's right-hand side — the comment there explains that
`resolve_type_impl`'s Struct/Enum/Union/Bitset cases already allocate-and-lay-out a fresh
slot, so the instantiation reuses that machinery. With no slot in existence yet, a
self-referential field has nothing to point at, and the recursion is caught by the
`generic_type_resolving` guard and reported as a cycle. That guard cannot distinguish
"reached through a pointer" (fine — pointer size is known without the pointee's layout)
from "reached by value" (a genuine cycle).

**What a fix needs.** Pre-allocate and register the instance before resolving its RHS, so a
pointer field can early-return the in-progress slot. That breaks the invariant
`resolve_field_type` currently documents and relies on — that `instantiate_generic_type`
always returns an already-fully-laid-out result — so by-value cycle detection has to move
from instantiation time to layout time. `size_of`/`align_of` have no completeness check
today, so a by-value self-reference would otherwise silently compute size 0 instead of
erroring. That is the delicate part, and it is the reason this was not attempted as part of
repairing an example fixture.

`examples/dictionary` threads its chains by index instead, with the workaround documented at
the top of the file.

---

## 3. Contextual `.` completion

`add_type_members`' `Bitset` arm is correct but **unreachable from `textDocument/completion`**,
and adding it changed no observable behaviour. Verified end-to-end, not inferred.

Reaching it needs a receiver expression whose type is a bitset, and no such expression
exists: a flag is only ever written contextually, as a bare `.Name` taking its meaning from
the expected type. That form has no receiver chain, so the handler takes the no-receiver
path (keywords + locals + module symbols) and never consults a type at all. The same is true
of enum fields and tagged-union variants written contextually — `.Opened` in a match arm,
`.Read` in `modes += .Read`.

Implementing it needs the *expected type at the cursor*, which nothing in the completion
handler computes. sema's recorded expression types only help when the buffer parses, and
completion runs mid-edit on buffers that usually do not.

Its absence is pinned in `tests/lsp_smoke_test.py` as current behaviour, so wiring it up is
a visible change rather than a silent one. The Trait arm of the same function **is** reached
and is covered.

---

## 4. Find All References is scoped to the import closure

`handle_references` walks `result.ast_program.modules` — the analysed module's import
closure, not the workspace. A reference from a file that does not import the target is not
found, and `referencesProvider` is advertised without qualification, so a client cannot tell.

Making it workspace-wide means indexing files no open document reaches, which is a different
shape of work from anything the LSP does today (every handler currently starts from one
analysed module). Recorded at the capability declaration in `server.cpp`.

---

## 5. Editor tooling outside this repository

Zed's syntax highlighting comes from
**[tree-sitter-mirage](https://github.com/Guthius/tree-sitter-mirage)**, pinned by commit
SHA in `editors/zed/extension.toml`. It is a separate repository, is not checked out here,
and cannot be fixed from this one.

Two syntax changes have landed here since it was last bumped:

- **`//` and `/* */` comments** (2026-07-27) replaced the older `#` syntax.
- **`?` on the last return type** (2026-07-30) marks an ignorable error.

`editors/zed/README.md` § "Updating the grammar" records exactly what each needs upstream,
and why the `.scm` files here cannot substitute — they are queries, and a query can only
match node types the grammar already produces.

VS Code is unaffected: its TextMate grammar lives here, is current on both changes, and is
now pinned by `tests/editor_grammar_test.py`.

---

## 6. `match`/`switch` cannot take a field access as its scrutinee

Found while adding `Type_Info_Generic_Arg.kind` (an inline `union(enum)` field) to
`runtime/type_info`. Not in either review document.

```mirage
switch h.kind {          // error: expected '=', got ':'
    .is_type: {}
    .is_scalar(v): {}
}

const k := h.kind        // fine
switch k { ... }
```

Only *member access* is affected. An identifier, a deref (`p.*`), a call, an index, and a
parenthesized expression all parse correctly as scrutinees, and both `match` and `switch`
behave the same way.

**Why.** It is an ambiguity with qualified tagged-variant construction, not a restriction on
scrutinees. `parse_postfix`'s member-access branch (`ast.cpp`, the `LBrace` + `Dot` lookahead)
commits to a `TaggedVariantExpr` whenever a dotted-identifier chain is followed by `{` then
`.` — that is the `Shape.circle{.radius = 3.0}` form. `switch h.kind { .is_type: ... }` has
exactly that shape, so the arms get parsed as constructor payload fields and the parser
demands `.is_type = ...`. The branch is gated on `named_type_from_expr` succeeding, which
returns `nullopt` for calls, indexes and derefs — which is precisely why those forms escape.

**What a fix needs.** One more token of lookahead past `{ . ident`: `=` means a variant
constructor's payload field, `:` or `(` means a match arm. Cheap in isolation, but it is a
change to the disambiguation rule two constructs share, so it wants fixture coverage on both
sides (a qualified constructor whose first field is named like a variant, and a match whose
scrutinee is a field) before being trusted.

`examples/example_type_info_generic_args` binds to a local first, with the reason noted inline.

---

## 7. Generic trait-impl bodies are never eagerly checked

`check_generic_type_method_bodies` (`sema_check.cpp`) finds the methods to check by walking
`ProgramModule::methods`, which is populated for `impl TYPE { ... }` blocks. `impl TRAIT for
TYPE { ... }` methods are registered only into `Program::trait_impls_by_type`
(`sema_declare.cpp`), so a *generic* trait impl — `examples/example_generics_orphan_impl` has
one — has no eagerly-checked template instance at all.

Two consequences, both pre-existing and neither a regression:

- Type errors in such a body go unreported until something instantiates it, unlike every
  other generic declaration.
- The LSP has no template `ExprSideTables` to read for it, so hover/inlay hints there fall
  back to `shadow_instantiate_and_resolve`'s `u8` placeholder — the behaviour every other
  generic body was just moved off. That fallback's doc comment names this as its remaining
  reason to exist.

Extending the eager pass to cover trait impls is a handful of lines, but it starts reporting
diagnostics in bodies that have never been checked, in this repository and in user code
alike. That is the reason it is here rather than done: it wants its own pass over the corpus,
not a ride along with an LSP fix.

---

## 8. Compiler diagnostics still say `<generic>` where the LSP says `T`

`ResolvedType::opaque_param_index` now carries a generic parameter's spelling for display,
and `type_to_string` (`src/lsp/type_printer.cpp`) uses it — so an editor reports `mut n :=
value` inside `fn write_int[T: type]` as `: T`. `describe_type` (`sema.cpp`), which formats
the same types for compiler diagnostics, still prints `<generic>` / `<generic: Trait>`.

Wiring it up is three lines and would make several eager-check messages considerably clearer.
It changes diagnostic *text*, though, so it needs `tests/generics_test.py` and
`tests/examples_expected.json` re-baselined in the same commit — kept separate from the LSP
change so that neither obscures the other.

---

## 9. Smaller notes

- **`compute_condition_narrowing` still does not descend into `||`.** Widened to narrow
  every operand of an `&&` chain; `||` deliberately unchanged, since an operand being true
  proves nothing about any other. Documented in `spec.md`'s narrowing table. If a future
  spec decision wants `||` handled in the *else*-branch (where it does prove something),
  that is where to start.
- **The escaping-`&payload` check is syntactic.** It over-reports (a call that only reads
  the pointer during the arm is flagged) and under-reports (assigning the pointer to an
  arm-local first is not followed), which is why it is a warning. A real fix needs
  provenance on `LocalBinding` and propagation through assignment, field stores, call
  arguments and returns — materially more than the `ErrorState` typestate pass does today.
