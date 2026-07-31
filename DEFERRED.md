# Mirage — Deferred Work

Everything previously listed here has been resolved, on branch `fixes` (2026-07-31, 32
commits). The only thing still outstanding is one action that cannot be taken from this
repository at all — see §1.

Current baseline: build clean, ctest 4/4, 17 Python suites passing, 207/207 corpus fixtures
matching (no `known_broken` entries).

---

## 1. Outstanding: bump the tree-sitter grammar's pinned commit

**This is the one item still open, and it is blocked on a push, not on work.**

Zed's syntax highlighting comes from
**[tree-sitter-mirage](https://github.com/Guthius/tree-sitter-mirage)**, a separate
repository pinned by commit SHA in `editors/zed/extension.toml`. The two syntax changes it
was missing — `//` and `/* */` comments, and the `?` optional-error return marker — are
implemented and tested there on branch **`sync-with-compiler`**, but not pushed, so there is
no SHA to point at.

To finish: push and merge that branch, then bump `rev` in `editors/zed/extension.toml`.

The in-repo half is done and safe to have landed early — `highlights.scm` already queries
the new `optional_error_marker`, `link_declaration` and `diagnostic_declaration` nodes, and a
query that matches nothing is inert. `editors/zed/README.md` § "Grammar status" has the
detail.

One thing that was not anticipated when this was written up: dropping `#` as the comment
character means `#link`/`#error`/`#warn` stop being swallowed as comments and become parse
errors, so the grammar needed rules for them in the same change.

---

## 2. What was resolved

### Defects (all were user-visible)

| Was | Now |
|---|---|
| A generic type could not refer to its own instantiation, even through a pointer | Works. By-value cycles are caught at layout time instead, which also fixed a pre-existing non-generic bug: `[3]node` inside `node` silently computed size 0. `examples/dictionary` threads its chains by pointer again. |
| `match`/`switch` could not take a field access as scrutinee | Works. The disambiguation against qualified variant construction needed one more token of lookahead, so `ast::Parser` gained `peek_at`/`check_at`. |
| Generic trait impls "went unchecked" | The premise was wrong: they did not work **at all**. Declaring one made the target type report `unknown type 'T'`, and calling a method on one hit an internal error. Both fixed, and conformance is now checked against a template receiver rather than skipped. |
| Compiler diagnostics said `<generic>` where the LSP said `T` | Fixed — and `mangle_generic_args` no longer routes symbol names through a display-only field. |
| No contextual `.Name` completion | Works, for bitset flags, enum fields and tagged-union variants, via sema's recorded type when the buffer parses and token inference when it does not. |
| Find All References was scoped to the import closure | Workspace-wide, with name-based cross-bundle identity. Locals and params are still closure-scoped, deliberately — they are function-scoped, so a sweep for one is guaranteed empty. |
| `\|\|` narrowed nothing in either branch | Its **else**-branch narrows, mirroring `&&`'s then-branch. `docs/spec.md`'s narrowing table moved with it. |
| The escaping-`&payload` check lost the pointer through a local | Followed through arm-local bindings, plus the tagged-variant-payload and `&v.field` routes that were missing. |

Three of the "refactors" turned out to be live defects rather than duplication:

- **CODEGEN-7** — `type_of(T)` inside a generic **crashed the compiler** with an uncaught
  `std::out_of_range`. Two of the three operand readers handled a generic parameter; the
  third did not.
- **TYPE-11** — `size_of(i64)` was accepted as an array length but **rejected** as a match
  arm, because arm patterns used an 8-shape const-folder while array lengths used a
  17-shape one.
- **CHECK-7** — a macro in group position reported `'twice' is not callable` (false) and
  cascaded, because the group call tree had no `MacroSymbol` arm where the value tree did.

### Refactors

All 22 IDs closed: `CHECK-7/8/9`, `CODEGEN-6/7/8/12/13`, `TYPE-5/6/7/11`, `PARSE-5/6/7/8`,
`SEMA-9/11/12`, `LSPCORE-12`, `LSPH-8/10`.

`check_expr` went 1683 → 707 lines and `check_stmt` 684 → 201.

Where a finding proposed something the code did not support, the reasoning is recorded at the
site rather than silently skipped:

- **CHECK-9b** — match and switch arm loops are *not* unified. `match` unifies arm types and
  requires exhaustiveness; `switch` does neither. That is a language rule, not an accident,
  and encoding it as a boolean flag would make both harder to read. Their genuinely shared
  front half *is* extracted.
- **LSPH-8** — `find_expr_by_location` is *not* folded into `ast_walker`. One is an
  early-exit search, the other an exhaustive callback visitor; unifying means changing four
  callback signatures and every user.
- **LSPH-10** — the two cursor-boundary conventions are *deliberately* different and now
  pinned so they cannot be "fixed" into agreement.
- **CODEGEN-12** — the *rule* is shared, not the emission: two sites build `llvm::Value*`,
  the third folds `llvm::Constant*`.

---

## 3. Verification

Every commit passed: `ctest` 4/4, `examples_smoke_test.py --strict`, and all 17 Python
suites. Commits touching layout, constant folding or emission were additionally verified by
diffing emitted LLVM IR across every buildable module — byte-identical except where a change
was intended.

Two regression guards were found to be worthless on first writing and were rewritten after
being tested against the mistake they guard (LSPH-10's cursor convention, §4's workspace
sweep). A guard that has not been seen to fail is not known to work.

The corpus grew 195 → 207 fixtures. Every new one pins behaviour that was previously
broken, unreachable, or untested.
