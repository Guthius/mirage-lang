# Mirage — Outstanding Issues

State of the codebase after working through `REVIEW_FINDINGS.md` on branch `fixes`
(2026-07-29 → 2026-07-30, 54 commits). Every one of that document's 116 findings was
either fixed or deliberately decided; this file lists **only what is still open**, and
why each thing was left.

Nothing here is a regression from that work. Items are either (a) explicitly deferred by
scope decision, (b) blocked on a decision that is not the implementer's to make, or
(c) pre-existing rot that predates the review.

Current baseline: build clean, ctest 4/4, 12 Python suites passing, 143/143 corpus
fixtures matching.

---

## 1. Blocked on a decision

These are ready to act on, but the call belongs to the project owner.

### 1.1 "Find All References" is compiled but not enabled

`src/lsp/handlers/references.{hpp,cpp}` was never in the build — not in `CMakeLists.txt`,
never dispatched by the server, never advertised in `initialize`, and it **did not
compile**. It has been added to the build and the compile error fixed, so it can no
longer rot, and LSPH-4/5/11 were applied to it.

It is still **not advertised**, deliberately: `tests/lsp_smoke_test.py:138` asserts
`referencesProvider` is absent, beside `renameProvider`, marking it a known
unimplemented feature.

To finish it:

- **Match-arm variant patterns don't resolve.** `MatchExpr::VariantPattern` is a
  *pattern*, not an expression, so `AstVisitor::on_expr` never sees it. Since match arms
  are where variants are most used, references on a variant currently finds close to
  nothing.
- **Bitset literal members don't resolve.** `BitsetExpr::members` is a
  `vector<std::string>` — the flags in `{.Close, .Flush}` are not expression nodes at
  all, so there is nothing to attach a location to. Fixing this means changing the AST.
- Then: dispatch the method, advertise the capability, and update the smoke-test
  assertion.

**Decision needed:** ship it, or leave it dormant.

### 1.2 Escaping `&payload` captures are silently wrong

Capturing a match/switch payload by reference and using it after its arm ends reads a
compiler temporary — the switch operand is emitted as a *value*, so the scratch slot
holds a copy and `&payload` never pointed at the user's object.

Before CODEGEN-1 this appeared to work, by accident: each loop iteration got fresh stack.
Now that the slot is correctly hoisted, such a capture reliably reads the most recent
iteration. Neither behaviour is *correct* — the code was always out of contract.

Diagnosing it needs sema-side lifetime analysis (does this reference outlive its arm?).
Until then it is a silent footgun.

### 1.3 CHECK-6 — narrowing only recognizes simple conditions

`compute_condition_narrowing` recognizes the error variable only as the whole condition
(`err`, `!err`) or as the **leftmost** operand of a single `&&`/`||`. So `x && err`,
`err1 && err2` and any deeper nesting narrow nothing, and the following `match err`
reports "unknown state" with no hint that the condition's *shape* is the reason.

Widening this is a **spec question, not a code change**: a compound condition has to be
decided operand by operand, and `||` proves nothing about either operand in the
then-branch. Documented at `sema_check.cpp`'s fall-through return.

### 1.4 PARSE-11 — `arr[..5]` does not parse

`SliceExpr` requires both bounds, unlike `RangeExpr::lower` which is optional (`for x in
..upper` works). `arr[..5]` reports a clean `expected expression, got '..'` — it is a
missing feature, not a misparse.

Making `start` optional is a language change requiring `spec.md`, `grammar.md` and the
docs site to move with the parser. Documented on `SliceExpr` in `ast.hpp`.

---

## 2. Blocked on missing test coverage

### 2.1 SEMA-8 — a loop that cannot be proven dead

`resolve_signatures_for_module`'s type-forcing loop looks redundant with
`ensure_module_declared`'s, and mostly is. But the two differ in exactly one respect:
this one redirects a **bare-import alias** to its origin's `(module_path, symbol_name)`
so the shared global slot is laid out with the origin as resolution context.

Since `ensure_module_declared` runs first and sets `layout_done`, that redirect cannot
currently take effect — which means *either* the loop is dead, *or* the redirect is
correct and `ensure_module_declared` should be doing it too. **Those have opposite
fixes.**

**Nothing in `examples/` or `runtime/` uses a bare import**, so no test can distinguish
them. Deleting the loop on the strength of a green suite would prove nothing.

**Blocker:** add bare-import test coverage first, then decide.

---

## 3. Deferred refactors (scope decision)

The "N near-identical implementations that have already drifted" class, deferred by
explicit decision at the start of the campaign. None is a bug; collectively they are the
reason bugs like CHECK-1 and TYPE-1/TYPE-2 existed, so consolidating reduces the rate of
*future* drift bugs.

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

`PARSE-9` and `SEMA-13` were pulled back in and completed, being genuinely mechanical.

**Highest value first, if revisited:** `TYPE-11` — the three drifted const-folders were
the direct cause of TYPE-1, TYPE-2, and the unreported `when size_of(...)` wrong-branch
bug. It is also the largest (multi-day).

**Note on CODEGEN-12:** looks cheap but is not uniform. Two sites work on
`llvm::Value*`, the third (`const_binary`) on `llvm::Constant*`. A 2-site helper is
mechanical; a 3-site one needs templating.

---

## 4. Missing LSP features (not defects)

- **LSPCORE-11 — no `workspace/didChangeWatchedFiles`.** Cached analysis is invalidated
  only by editor notifications, so a dependency edited outside the editor (a `git pull`,
  a generated file) keeps stale diagnostics and hover indefinitely, bounded only by the
  unrelated `MAX_CACHED_MODULES = 32` LRU.
- **LSPCORE-7 — `didClose` clears diagnostics unconditionally.** Correct for
  buffer-derived diagnostics (and what the spec suggests), wrong for a closed file that
  still has on-disk errors *and* remains in an open module's import closure — its
  squiggles vanish until an unrelated edit re-touches the closure. Fixing it needs
  bookkeeping that distinguishes buffer-derived from closure-derived diagnostics.
  `lsp_smoke_test.py:489` pins the current behaviour.
- **LSPH-9 — no completion inside a generic instantiation's argument list**
  (`List[<cursor>]` offering type names). Needs the cursor classifier to recognize a `[`
  context distinct from indexing.

---

## 5. Verification gaps

### 5.1 Trait/bitset completion is unverified end-to-end

`add_type_members` gained `Trait` and `Bitset` branches (LSPH-1, LSPH-2). These mirror
the `resolve_member` changes that **were** verified end-to-end for hover and
go-to-definition, but `textDocument/completion` has its own cursor-classification step
that I could not get a clean probe through. The branches are correct by symmetry with a
verified fix, not independently confirmed.

**To close:** drive a completion request at `shape.<cursor>` (trait handle) and
`modes.<cursor>` (bitset) and assert the offered items.

### 5.2 Three corpus fixtures are stale and excluded from real coverage

Pre-existing breakage, present on `master` before this work, recorded as `known_broken`
in `tests/examples_expected.json` rather than silently fixed. Each still fails:

| Fixture | Failure |
|---|---|
| `examples/dictionary` | `main.mir:7` uses pre-`[]` paren generics: `type entry(K: type, V: type)`, replaced when generics landed 2026-07-29 |
| `examples/example_enum` | `import("example_enum_def")` targets a *sibling* directory, not a subdirectory, so it cannot resolve |
| `examples/example_default_undefined` | `main.mir:24` writes `{ x = 5, y = default }` without the leading dots the braced-initializer syntax requires |

Each is a few minutes' work; all three were left because repairing example content was
outside the review's scope. Until fixed they contribute no regression coverage.

(`examples/lexer` was in this category and **has** been fixed — it hung the compiler
forever and now compiles.)

---

## 6. Known sharp edges worth documenting for users

Not bugs, but surprising, and currently only recorded in code comments:

- **`switch` is not exhaustiveness-checked**, unlike `match`. Correct — `match` is an
  expression and must produce a value on every path, `switch` is a statement and falling
  through does nothing — but the asymmetry with `match`'s headline feature reads as an
  oversight. Worth a line in `spec.md`.
- **`#link(flag, ...)` is an unrestricted escape hatch**, passed to the linker verbatim.
  Intentional, and not a privilege boundary (it comes from source the user is already
  compiling), but it is the one directive with no validation.
- **`import("..")` traversal is unrestricted.** Only *absolute* paths are rejected.
  Upward traversal is how the corpus's own sibling-module imports work
  (`import("../../runtime/type_info")`), so constraining it would break working
  multi-directory projects — but there is no project-root boundary.
