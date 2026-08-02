# Implementation Prompt: `@test` Attribute and `mirage test` Support

## Context

Mirage is a systems programming language (compiler: `mirec`, C++23, LLVM backend) with a
full pipeline: lexer, parser, module resolver, multi-phase semantic analysis, and LLVM
codegen. This prompt specifies a new testing framework feature spanning the parser, sema,
codegen, and the CLI driver, plus a new reserved stdlib module (`core/testing`) whose
*implementation* is explicitly out of scope for this prompt (see §7).

This is a **language + compiler change**, not a documentation-only change. Update
`grammar.md` and `spec.md` alongside the implementation, per project convention.

Read `grammar.md` and `spec.md` in full before starting — several existing mechanisms
(`@init`'s ordering graph, `#link`'s module-scope collection, trait-handle coercion,
boolean coercion on `error(...)`, the `when`-branch "both sides type-checked" posture vs.
generics' "unreached instantiation never checked" posture) are reused or deliberately
diverged from below, and the reasoning depends on knowing which is which.

---

## 1. Grammar / Parser Changes

### 1.1 `@test` attribute

Add `test` to the fixed known-attribute set the parser validates `@name`/`@name(...)`
clauses against (alongside the existing `no_return`, `naked`, `always_inline`, `section`,
`init`). No new grammar production is needed — `@test` uses the existing bare
single-attribute form (`'@' IDENT`, no arguments).

`@test` is legal in the same two syntactic positions as the existing attributes: preceding
a `fn_decl`, and preceding a `method_decl` inside an `impl` block. The **method** case is
rejected in sema (see §2.1), not the parser — same split `@init` already uses (`@init` is
parseable on a method, then rejected structurally by sema).

### 1.2 CLI: `--load <path>` flag

Add a new, repeatable driver flag `--load <path>`. Each occurrence adds one module path to
the forced-module list (see §2.4). Resolution uses the exact same two-step rule as an
ordinary `import(...)` path (root-module-relative first, then `MIRAGE_PATH`/`--std`).

### 1.3 CLI: `test` action

Add a new top-level driver action: `mirage test <root_module>`, alongside the existing
build/run actions. Parses and resolves `<root_module>` exactly like `build`/`run`.

Driver-level validation for the `test` action:

- **`--freestanding` + `test` is a driver error.** `core/testing`'s implementation needs
  `fork`/`waitpid` (POSIX), which a freestanding target has no guaranteed access to. Emit a
  clear error and do not proceed to sema.
  ```
  error: 'mirage test' is not supported with '--freestanding' (test isolation
         requires POSIX process primitives not available in freestanding builds)
  ```
- **`--noinit` is honored as-is** — if passed, `_init` generation/invocation is suppressed
  in test mode exactly as it is in build/run mode (see §3.3).
- **A `main` in the root module is legal and simply never called** in test mode — do not
  warn or error on its presence.
- The driver unconditionally adds `core/testing` to the forced-module list for this
  invocation, in addition to any user-supplied `--load` paths (see §2.4, §2.5).

---

## 2. Sema Changes

### 2.1 `@test` signature restrictions (unconditional — every driver action)

These are declaration-level structural checks and apply regardless of driver action
(`build`, `run`, `test`) — a malformed `@test` function is always an error, so switching
actions never changes whether the declaration itself is valid:

- **No parameters** — a `@test` function must declare zero parameters (not even defaulted
  ones). Same rationale as `@init`: the harness must call it uniformly with no arguments.
- **Return type must be exactly `error(...)`** — single error type or a union
  (`error(A | B)`), any member type(s) permitted (this is a deliberate divergence from
  `@init`, which restricts to `enum(i32)` only — state this explicitly in spec.md so the
  asymmetry is documented, not implied). Any other return shape (void, a value type, a
  multi-return list) is a sema error.
- **No `generic_params`** — same restriction and same rationale as `ext fn`/`macro`/`trait`
  (§22): no way to call an uninstantiated template uniformly from the harness.
- **Not allowed on a `method_decl`** — explicit sema error, mirroring `@init`'s:
  ```
  error: '@test' is not allowed on impl methods; declare a module-scope function instead
  ```

### 2.2 Conflicting attributes

Add to the existing "Conflicting Attributes" rejection list (§21):

- `@test` + `@naked` — a naked function has no compiler-generated body shape; the harness
  needs an ordinary calling convention.
- `@test` + `@no_return` — a test that never returns can't report a status.
- `@test` + `@init` — two different automatic-invocation mechanisms; combining them is
  ambiguous.
- `@test` + `@always_inline` is **legal** — do not reject this combination. Nothing
  prevents a test body from being inlined into its synthesized wrapper's call site.

### 2.3 Mode-dependent checking depth and call-site diagnostics

**Two-tier checking, split by driver action:**

- **Signature-level restrictions (§2.1, §2.2)** are checked in every action.
- **Body type-checking and codegen** happen **only** under the `test` action. Under
  `build`/`run`, a `@test` function's body is not type-checked and not emitted at all —
  this mirrors §22's "an unreached generic instantiation is never type-checked" posture,
  **not** `when`'s "both branches always type-checked" posture. Document this explicitly
  in spec.md as a deliberate asymmetry: a `@test` body with e.g. a bad member access
  compiles cleanly under `mirage build` and only surfaces under `mirage test`.

**Call-site / address-of diagnostic** (direct call, or taking the function's address —
assigning to a function-pointer-typed variable, passing as a callback):

- Under `build`/`run`: **hard sema error**.
  ```
  error: cannot call '@test' function 'foo' outside of 'mirage test'
  ```
- Under `test`: **warning** (tests should not call other tests; helpers meant to be shared
  between tests should simply not be `@test`-annotated).
  ```
  warning: '@test' function 'foo' called directly; tests should not call other tests
  ```
- Trigger conditions are identical in both modes: a direct call expression, or the function
  name used as a value (address-of / function-pointer assignment). A test calling another
  test warns/errors the same as any other caller — no special-casing test-to-test calls.

### 2.4 Forced-module loading (new sema mechanism)

Add a new, independent input to the module-loading phase: a **forced-module list** —
module paths supplied by the driver (via `--load`, plus `core/testing` unconditionally
under the `test` action). This is a new mechanism, not an extension of `import_expr` or
`bare_import_decl`.

**Algorithm:**

1. Resolve and load the normal module graph from the root module exactly as today (all
   modules transitively reached via `const := import(...)` / bare `import(...)`).
2. For each path in the forced-module list, resolve it using the identical two-step
   `import(...)` resolution rule (root-module-relative, then `MIRAGE_PATH`/`--std`).
3. If a resolved forced-module path is **already loaded** (reached normally, or already
   forced by an earlier entry in the list — including the root module itself), this is a
   **no-op** — do not double-load, do not error.
4. Otherwise, load and fully compile the module, and add it to the set of modules
   participating in every whole-program sema concern on identical terms to a normally-
   loaded module:
   - Ordinary declaration type-checking.
   - `impl` coherence (single-impl-per-`(TRAIT, TYPE)`, orphan-impl rule) against the rest
     of the program.
   - `@init` collection and ordering (dependency edges built from actual symbol
     references in `@init` bodies, exactly as today — this works correctly regardless of
     load-phase ordering, since the graph only needs all modules loaded before it's built,
     not loaded in reference order).
   - `#link` directive collection.
   - `@test` discovery (see §2.5) — applies uniformly to forced modules too, though
     `core/testing` itself is expected to declare none.

**Critical restriction — no symbol visibility:** loading a forced module creates **no**
`const` binding and injects **no** name into any other module's symbol table. A forced
module is unreachable by any Mirage source expression, in any module, via any identifier —
there is no "collision with an existing name" concern to check, because nothing is ever
inserted anywhere for user code to collide with. This is the entire point of the mechanism
(see §7's driver-registration example) and must not regress into an accessible alias by
any implementation shortcut (e.g. do not implement this by synthesizing a hidden `const`
binding with a mangled name that's merely *hard* to spell — it must be genuinely
unreachable through ordinary identifier resolution).

**Compiler-internal access to forced-module symbols:** codegen needs to call into
specific declarations of a forced module (e.g. `core/testing`'s `_run_tests`) with no
source-level identifier to resolve. Implement this as a **fixed, compiler-known symbol
handle** — the compiler already mangles every cross-module symbol to a fixed name derived
from `(module path, declaration name)` for linking purposes; for forced modules this
mangled reference is looked up directly by the compiler, bypassing identifier resolution
entirely. This is conceptually the same "known contract, not built into the compiler
itself" posture already used for `runtime/type_info`'s `Type_Info`.

### 2.5 `@test` discovery in test mode

Under the `test` action only, after module loading (§2.4) completes:

- Scan **every currently-loaded module** (reached normally or via forcing — one uniform
  scan, no special-casing based on how a module entered the set) for `@test`-annotated
  functions.
- `@test` functions need not be `pub` to be discovered — same posture as `@init`, since
  discovery/codegen has whole-program reach regardless of visibility.
- Row/collection order does not need to imply anything about execution order (execution
  order is entirely `core/testing`'s runtime concern, see §7) — but for deterministic,
  diffable codegen output, collect in: source declaration order within a module, modules
  ordered by first-reached position in **import-graph traversal from the root, with forced
  modules appended after the normal graph in forced-module-list order**. Use this same
  order when building the `Test_Info` array in §3.2.

---

## 3. Codegen Changes

### 3.1 Per-test wrapper synthesis

`@test` functions may each declare their own distinct `error(...)` return type (a
different union per test is expected and legal), so no single function-pointer type can
point at a real `@test` function's address directly (function pointer types are
structurally exact per §15/§2 — `fn() -> error(A)` and `fn() -> error(B)` are distinct,
non-interchangeable types).

For each discovered `@test` function `mod.foo`, synthesize one compiler-generated wrapper
function with a **fixed, uniform signature**:

```mirage
fn() -> bool
```

The wrapper body calls the real test function and collapses its `error(...)` result to
`bool` using the existing boolean-coercion rule (§16: an error value in boolean context
tests the `Ok`/`Failed` tag) — `true` means `Ok`, `false` means `Failed`:

```mirage
// compiler-synthesized, mangled name (not user-writable, same precedent as
// generic-instantiation mangled symbols, §22)
fn __test_wrapper_N() -> bool {
    return !(mod.foo())
}
```

This wrapper is the **only** compiler-generated function body for this feature beyond the
`Test_Info` constant itself (§3.2) — the fork/report/timing loop is ordinary
`core/testing` library code (§7), not compiler-generated.

### 3.2 `Test_Info` synthesis

Using the fixed contract types from `core/testing` (§7) — `Test_Case`, `Test_Info` — emit
one compile-time `Test_Info` constant listing every discovered `@test` function, in the
order established in §2.5, with each `Test_Case` populated as:

- `module_name`: the discovered function's module path (root-relative, same convention
  `import(...)` paths use).
- `function_name`: the function's own declared name (unqualified).
- `function`: a pointer to that test's synthesized wrapper (§3.1).

### 3.3 `_start` synthesis for `test` action

Under the `test` action, codegen synthesizes `_start` following the same shape as the
existing hosted-build `_start`, with the `main` call replaced:

1. Call the generated `_init` (unless `--noinit` was passed — identical behavior to
   build/run mode; this is unchanged).
2. Instead of calling `main` (even if present — see §1.3), call `core/testing`'s
   `_run_tests`, passing a pointer to the synthesized `Test_Info` constant (§3.2). This
   call is resolved via the fixed compiler-internal handle described in §2.4, not through
   ordinary identifier resolution.
3. The process's exit code is whatever `_run_tests` itself produces (see §7's expected
   exit-code contract) — codegen does not impose an additional exit-code layer on top.

`main`, if present in the root module, is compiled (it's an ordinary `pub fn main` unless
otherwise dead-code-eliminated by existing rules) but is never called by the `test`-mode
`_start`.

---

## 4. Reserved Module Path

`core/testing` is a **reserved module path**, documented alongside `runtime/type_info`'s
existing "fixed contract, not built into the compiler" framing (§1). Under the `test`
action, the compiler assumes this path resolves successfully and exposes exactly the
declarations in §7's contract — this is a **driver-level error**, not a deep sema/codegen
failure, if the module is missing or the required names/shapes don't match:

```
error: 'core/testing' could not be resolved or does not expose the expected
       testing contract (Test_Function, Test_Case, Test_Info, _run_tests) —
       required for 'mirage test'
```

Validate this early (right after forced-module loading, before proceeding to `@test`
discovery/codegen) so the failure is legible and doesn't surface as a confusing type
mismatch deep in `Test_Info` synthesis.

---

## 5. Tests to Write

Cover at minimum:

**Parser / attribute validation**
- `@test` accepted on a bare `fn`; parses on a `method_decl` (to be rejected later by sema,
  not the parser).
- `@(test, always_inline)` grouped form.
- Unknown-attribute error still fires for typos (`@tests`, `@Test`).

**Sema — signature restrictions**
- `@test` with parameters → error.
- `@test` with generic params → error.
- `@test` returning void / a bare value / a multi-return list without `error(...)` last →
  error.
- `@test` returning `error(A | B)` → accepted.
- `@test` on a `method_decl` → error, exact diagnostic text.
- `@test` + `@naked` / `@no_return` / `@init` → each rejected.
- `@test` + `@always_inline` → accepted.

**Sema — mode-dependent behavior**
- A `@test` function with a deliberately broken body compiles clean under `build`; the
  same source fails under `test`.
- Direct call to a `@test` function under `build`/`run` → hard error.
- Direct call to a `@test` function under `test` → warning, compilation still succeeds.
- Taking a `@test` function's address (function-pointer assignment) triggers the same
  call-site diagnostic as a direct call, in both modes.

**Sema — forced modules**
- `--load` a module not otherwise reached → its declarations are checked, its `@init`
  participates in ordering, its `#link` is collected.
- `--load` a module already reached normally → no double-load, no duplicate `@init`
  invocation.
- `--load` the root module itself → no-op, no error.
- Forced module's declarations are **not** resolvable by name from any other module (no
  accessible alias is created) — write a test that asserts referencing a forced module's
  symbol by any spelling from another module fails as "undefined identifier."
- Two forced modules with an `@init`-reference dependency between them still order
  correctly relative to each other and to the normal graph.

**Codegen**
- A program with two `@test` functions with differing `error(...)` unions compiles —
  confirms the per-test wrapper approach actually resolves the structural function-pointer
  type mismatch described in §3.1.
- Synthesized `Test_Info` contains the expected number of cases with correct
  `module_name`/`function_name` values, including for a `@test` function discovered only
  via a forced (`--load`) module.
- `main` present in root module under `test` action: compiles, and (via an integration-
  level test, e.g. checking `_start`'s emitted calls) is never invoked.

**Driver**
- `mirage test --freestanding <root>` → driver error, no sema/codegen attempted.
- `mirage test --noinit <root>` → `_init` not generated/invoked; test run still proceeds.
- Missing/malformed `core/testing` → the driver-level error from §4, not a downstream
  crash or confusing diagnostic.

---

## 6. Documentation Updates

- **grammar.md**: add `test` to the fixed known-attribute-name set referenced in the
  `attribute` production's surrounding prose (same place `no_return`/`naked`/etc. are
  listed).
- **spec.md §21 "Declaration Attributes"**: add a `@test` subsection following the
  existing `@init` subsection's structure — signature restrictions, the
  method-declaration rejection diagnostic, and an entry in "Conflicting Attributes."
  Explicitly call out the `error(...)` restriction's divergence from `@init`'s narrower
  `enum(i32)`-only rule.
- **spec.md §11 "Modules"**: add a new subsection on forced-module loading — the
  mechanism, the "no accessible alias" guarantee, the `--load` flag, and the driver
  pattern worked example (a generic-interface module + registration-only driver modules,
  per §7 below) as motivation independent of testing.
- **spec.md**: add a new numbered section, "Testing," covering: the `test` driver action,
  `core/testing` as a reserved module path and its fixed contract (§7), the mode-dependent
  body-checking asymmetry (§2.3) stated as an explicit v1 note in the same style as §22's
  "No Bounds in v1," and the exit-code/output contract expected of `_run_tests` (§7).
- Update the reserved-keywords list (§19) — **no change needed**, since `test` is used
  only immediately after `@`, exactly like `no_return`/`naked`/`always_inline`/`section`/
  `init` — it remains a plain, unreserved identifier everywhere else. State this
  explicitly alongside the existing sentence covering those five names.

---

## 7. Out of Scope for This Prompt: `core/testing`'s Implementation

The compiler's obligation ends at the fixed contract below — the actual test-runner logic
(forking, waiting, timing, table formatting, exit-code selection) is ordinary Mirage
source code living in the `core/testing` stdlib module, to be implemented as a **separate,
follow-on task**, the same way `runtime/type_info`'s `Type_Info` definition is out of
scope for whatever implements `type_info_of`.

The compiler-facing contract this module **must** expose (exact names, used by codegen
per §3.1–§3.3):

```mirage
pub type Test_Function = fn() -> bool   // true = Ok, false = Failed

pub type Test_Case = struct {
    module_name: []u8
    function_name: []u8
    function: Test_Function
}

pub type Test_Info = struct {
    cases: []Test_Case
}

pub fn _run_tests(tests: *Test_Info)
```

Guidance for whoever picks up that follow-on task (not binding on this prompt's
implementer, but recorded here so the compiler-side contract above is shaped correctly
for it):

- **Isolation via `fork`/`waitpid` per test**, not per-test binaries — compile once, fork
  per `Test_Case`; child calls `.function()` and `exit()`s with a status encoding the
  bool result (e.g. 0 for `true`/Ok, 1 for `false`/Failed); parent `waitpid`s and
  classifies the outcome into **three** states, not two: `WIFEXITED(0)` → ok,
  `WIFEXITED(1)` → failed, anything else (`WIFSIGNALED`, unexpected exit code) → crashed
  (name the signal, e.g. `crashed (SIGSEGV)`). This gives true crash isolation — a
  segfaulting test doesn't take down the run — without paying per-test LLVM/link cost.
  Needs `fork`/`waitpid`/`exit` declared as `ext fn`; no compiler support required beyond
  what `ext fn` already provides.
- **Timing** taken by the parent immediately around the `fork`/`waitpid` pair, via
  `ext fn clock_gettime`/`gettimeofday`, same unglamorous FFI posture as any other libc
  call.
- **Output**: a table with columns module path / function name / result / elapsed time,
  row order matching `Test_Info`'s array order (deterministic per §2.5, not tied to actual
  execution order).
- **Exit code**: 0 if every case is ok; a fixed non-zero sentinel (not a count) if any
  case is failed or crashed — same "fixed sentinel, not a raw/derived value" shape `_init`
  already uses for its own failure exit code.
- Known v1 limitations to carry over, not solve here: sequential execution only (no
  parallel jobs), no hang/timeout protection, no error-payload stringification in output
  (tag only — `Ok`/`Failed`/crashed, no field/variant detail).

## Appendix: The Driver-Registration Pattern (motivation for §2.4, non-testing use case)

Forced-module loading has a second, independent motivating use case worth confirming works
end-to-end once §2.4 is implemented (a good candidate for an additional integration
test): a Go-style blank-import driver-registration pattern, where a generic interface
module exposes a registry, and concrete driver modules register themselves via `@init`
with no consumer-visible import:

```mirage
// core/db.mir
pub type Driver = trait {
    fn connect(self, dsn: []u8) -> (Connection, error(Db_Error))
}
pub fn register(name: []u8, driver: Driver) { ... }
pub fn get(name: []u8) -> Driver { ... }
```

```mirage
// drivers/postgres.mir — loaded only via --load drivers/postgres, never bound-imported
const db := import("core/db")
type Postgres_Driver = struct { }
impl db.Driver for Postgres_Driver {
    fn connect(self, dsn: []u8) -> (db.Connection, error(db.Db_Error)) { ... }
}
@init
fn register_self() {
    mut instance: Postgres_Driver = default
    db.register("postgres", &instance)
}
```

This should work with no special-casing beyond §2.4 itself: `register_self`'s reference to
`db.register` is a real symbol reference, so `core/db`'s own `@init` (if any) is ordered
first per the existing `@init` dependency-graph rule; the trait-handle coercion on
`&instance` already hides the concrete type by construction (§10, no downcasting). Note
for documentation: this pattern makes the concrete driver type merely *unadvertised*, not
provably unreachable — nothing stops another module from writing its own
`import("drivers/postgres")` directly, since forced-loading doesn't alter the driver
module's own `pub` visibility. State this caveat explicitly in the spec.md addition per
§6, so it isn't mistaken for an encapsulation guarantee.
