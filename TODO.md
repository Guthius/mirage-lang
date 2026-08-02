# Outstanding issues and gaps

Started as a review of the compiler at commit `2ece065`. Items resolved on branch
`fix/todo-resolution` are struck through with the commit that closed them; what remains is
below, with scope measured rather than estimated.

Every behavioural claim here was reproduced against `build/mirage`.

---

## 1. Correctness bugs — **all resolved**

- ~~**1.1** `@no_discard` on a generic function rejected with a wrong diagnostic, blocking
  legitimate code.~~ `ec78a2c` — a template's `return_types` is never populated, so the
  empty list read as "returns nothing". Both `@no_discard` and `@no_return` now skip the
  return-shape half of their checks for a generic. The second half of TODO 2.3 went with
  it: `@no_discard` on a generic could not have fired at a call site even once accepted,
  because generic calls resolve through `record_instance`, which did not record it.
- ~~**1.2** `@export` on a method of a *generic type* silently ignored.~~ `601775e` — a
  generic type's methods are never signature-resolved, so both validation passes skipped
  them. Now rejected, for bare and trait impls alike.
- ~~**1.3** An `@export` name could collide with the compiler's own mangling.~~ `89b3a29` —
  `__mir_` and `__mirage_` are reserved.

## 2. Accepted-but-inert features

- **2.1** `@import` is validated and never read. **Blocked on the wasm backend** (§3,
  stage 7), which is its only consumer. Not a defect in itself — the spec already documents
  it as target-conditional — but nothing acts on it yet.
- **2.2** `@export` does not produce a wasm *export section* entry. **Blocked on the same
  stage.** On the current emscripten path `emcc`'s `-sEXPORTED_FUNCTIONS` decides this.
- ~~**2.3** `@no_discard` fired on only three call paths.~~ Generic calls closed by
  `ec78a2c`; trait dispatch by `61a0811`. The function-pointer case is deliberate and now
  documented (spec §21): `@no_discard` is a property of the declaration, and a
  function-pointer type does not carry it — the same reasoning as `@callconv`'s v1 limit.
- **2.4** `@callconv("c")` unsupported on impl methods. **Kept as a v1 limit by decision**,
  with a clear error and a spec note. Lifting it means marshalling the receiver and routing
  method call sites (including trait dispatch) through the C-ABI path.
- ~~**2.5** Attributes not permitted on globals or trait method declarations.~~ `f726edb`
  (`@export` on module-scope globals) and `61a0811` (`@no_discard` on trait methods, binding
  every caller reaching them through a handle).

## 3. The backend — **in progress, the large remaining item**

`docs/backend.md` is the design record: stages, sequencing, validation strategy, decisions
D1–D8.

Done:

- **Stage 1** MIR — types, builder, verifier, printer (`src/compiler/mir.{hpp,cpp}`,
  33 unit tests in ctest).
- **Stage 2, first increment** `1595324` — `mirgen` lowers scalar functions, `ext fn`
  declarations, globals, locals as slots, arithmetic with correct signedness, comparisons,
  conversions, assignment, direct calls, `if`/`while`, and short-circuit `&&`/`||` (via
  block parameters, the case phi existed for). `--emit-mir` prints it; the verifier runs on
  every lowering. Anything unlowered is diagnosed by name and summarised, never skipped.

- **Stage 2, second increment** — the lvalue path (`emit_address`): struct field access,
  array/slice/pointer indexing, pointer auto-deref and address-of all became one address
  computation shared by reads, writes and `&`. With it: member reads, assignment to any
  lvalue, scalar casts, `default` on an aggregate (a slot memset), and named coverage
  reporting.

- **Stage 2, third increment** — aggregates as memory (`default` → memset, copy → memcpy,
  an aggregate expression's value IS its address), string literals (interned private
  constant globals, NUL-terminated so one global also backs a `*u8` for C), `len` on arrays
  and slices, cross-module `mod.fn()` calls, and `++`/`--`.

  **Coverage: 22 of 271 corpus modules lower fully** (from 18 at the start of the session).
  Module-level coverage lags badly because most modules hit several blockers at once; the
  per-construct histogram is the real signal. Cleared this session: casts 103→0, member
  access 70→0, indexing 30→0, string literals 91→0, `len` 51→0, aggregate initializers
  156→0, aggregate assignment 48→0, cross-module calls resolved.

- **Stage 2, fourth increment** — methods (declared, bodies emitted, and called, with the
  receiver as a leading pointer parameter) and braced initializers (struct and array
  literals, built into a slot: zero fill then per-element stores at sema offsets).

  **Coverage: 25 of 271 corpus modules lower fully.** Cleared: method calls 80→0, braced
  initializers 67→0.

  Largest remaining blockers, by occurrence: `return_err` (62), `return_ok` (50), `switch`
  (45), `match` (30), calls through a function pointer (28), trait-handle method calls
  (26), non-integer conditions (23), `type_of` (22), group declarations (21).

- **Stage 2, fifth and sixth increments** — sret (any aggregate or multi-return travels
  through a caller-owned pointer; returning a callee slot's address would dangle),
  `return_ok`/`return_err` building the `error(...)` tagged blob into it, enum variant
  references in both `.Variant` and `Type.Variant` spellings, `switch` on integer/bool/enum
  operands via the MIR switch terminator, and conditions on pointers (null test) and error
  values (Ok/Failed tag).

  **Coverage: 27 of 271 corpus modules lower fully.**

- **Stage 2, seventh increment** — character literals, `type_of` (a constant type id),
  and calls through a function pointer (`call.indirect`, carrying its signature explicitly
  because wasm needs it as a type index).

- **Stage 2, eighth increment** — `when` statements, which sema has already folded: only
  the selected branch is emitted, and no runtime control flow at all.

- **Stage 2, ninth increment** — `for-in` over arrays and slices (all three binding forms:
  value, `i, value`, and `&value`), `break`/`continue`, and slice expressions (`xs[..]`,
  `xs[a..b]`).

  **Coverage: 32 of 271 corpus modules lower fully.**

  Largest remaining blockers: multi-return in all its forms (`-> (T, error(E))` returns,
  `return_ok` with value slots, group declarations — ~99 combined, and one lowering that
  covers all of them), `switch`/`match` on tagged unions (80), `try` (31), trait-handle
  method calls (26).

- **Stage 2, tenth increment** — multi-return. One blob layout (`multi_return_layout`:
  each value at its naturally-aligned offset in the caller-owned sret slot) shared by
  every writer (`return a, b`, `return_ok` with value slots, `return_err` from a
  multi-return function, exact-match forwarded `return f()`) and every reader (group
  declarations, all three call forms — direct, method, indirect — sized from the callee's
  sema return list, since a multi-return call expression has no recorded `expr_type`).
  The error-wrapping half moved into a shared `emit_error_value_into` that also handles
  the 2+-member inner dispatch union (the previous single-member-only path wrote the
  member where the inner TAG belongs) and the trailing-error-slot sugar in a plain
  `return`. Calls dropping a trailing `?error(...)` are now diagnosed by name — the
  runtime unhandled-error panic path does not exist in MIR yet, and silently returning
  the blob where the surviving value belongs would miscompile.

  **Coverage: 33 of 271 corpus modules lower fully.** Cleared: `return_err` 49→0,
  `return_ok` with value slots 29→0, multi-return `return` 22→0, group declarations 21→0.

  Largest remaining blockers: `switch`/`match` on tagged unions (82), `try` (35, now
  including the group-declaration form), trait-handle method calls (27), ternaries (20),
  `type_info_of` (18), `defer` (17).

- **Stage 2, eleventh increment** — tagged unions end to end: constructors
  (`.Variant{...}`, payload-free `.Variant` / `Type.Variant`, and sema's implicit
  `VariantCoercion` applied at the one emit_expr funnel), and `switch`/`match` dispatch
  through a shared `emit_arm_dispatch` skeleton mirroring codegen's — tag switch on a
  fresh copy of the operand, payload captures by value and by reference, the transparent
  error-value unwrap, `match` results merging through a block parameter (scalar) or a
  result slot (aggregate).

  The wider coverage exposed four latent lowering bugs, all fixed with the coverage that
  found them: a declaration with an explicit type but no initializer fell back to `i64`
  (a `mut out: [20]u8` local was an 8-byte scalar); uninitialized locals were not
  zeroed (codegen's `emit_default_value` semantics — `undefined` is now the explicit
  opt-out); pointer ± integer emitted an integer `add` on a `ptr` operand instead of
  scaled address arithmetic; and the array↔slice representation changes (`out = s`
  copy-and-zero-fill, array→slice headers, slice→pointer data words at call arguments)
  were blind byte copies. Aggregate stores now funnel through one coercion-aware
  `store_aggregate_value`; assignment targets resolve global symbols' types.

  **Coverage: 48 of 271 corpus modules lower fully; zero verifier failures across the
  corpus.** Cleared: tagged-union `switch` 41→0, `match` 41→0, tagged-variant
  constructors 16→0, `undefined` 14→0.

  Largest remaining blockers: `try` (35), calls into not-yet-declared shapes (29,
  mostly generics), trait-handle method calls (27), ternaries (~20), `type_info_of`
  (18), `defer` (17).

- **Stage 2, twelfth increment** — `try` in all three positions (statement, expression,
  group declaration), through one propagate-or-continue skeleton: branch on the callee
  error's tag, on failure write the error into the caller's own last return slot and
  return, on success fall through. Needed the enclosing return list from expression
  position, so mirgen grew codegen's `current_returns_` member. Subset error unions
  (`error(E)` propagated into `error(E | F)`) re-tag through `emit_error_retag` —
  compile-time for a single-member callee, a runtime switch on the inner dispatch tag
  for a multi-member one; payload bytes are identical between any two unions carrying
  the same member, so only tags are ever rewritten. The same helper serves `return_err`
  and trailing-error-slot returns with a subset operand.

  **Coverage: 50 of 271; zero verifier failures.** Cleared: `try` 35→0, subset error
  propagation 12→0.

  Largest remaining blockers: calls into not-yet-declared shapes (28, mostly generics),
  trait-handle method calls (27), ternaries (23), `type_info_of` (18), `defer` (17),
  `size_of` (13).

- **Stage 2, thirteenth increment** — trait handles end to end. Vtables are constant
  globals whose entries are `mir::Relocation`s (the machinery they were designed for):
  one per `impl TRAIT for TYPE` plus one synthesized sub-vtable per composed component,
  method slots in `TraitInfo::methods` order followed by component back-pointer slots,
  exactly codegen's `declare_vtables`. Pointer-to-handle coercion, handle-to-handle
  composition narrowing (a load from the pre-computed trailing slot), dynamic dispatch
  (`call.indirect` through the vtable slot at `method_order_index`, keyed off sema's
  `expr_trait_dispatch` record — never re-derived from the receiver's shape), and
  handle-vs-nil comparison on the data word. Two bugs found on the way, both fixed in
  their own right: trait-impl methods live ONLY in `trait_impls_by_type` (never in
  `ProgramModule::methods`), so mirgen had never declared or emitted them — its comment
  claiming otherwise was wrong; and aggregate parameters were bound by storing the
  incoming POINTER into an aggregate-sized slot that every reader treats as holding the
  aggregate itself (separate commit, since it affected every aggregate parameter, not
  just traits).

  **Coverage: 60 of 271; zero verifier failures.** Cleared: trait-handle method calls
  27→0.

  Largest remaining blockers: calls into not-yet-declared shapes (28, mostly
  generics), ternaries (24), `type_info_of` (18), `defer` (17), `size_of` (13),
  calls dropping an ignorable error (13).

  Still entirely absent from stage 2: `defer`, generics (monomorphized instances),
  inline `asm`, global initializers.

Remaining:

- **Stage 2, rest** — aggregates (structs, arrays, slices, trait handles, `any`, error
  unions), `for-in`, `switch`/`match`, `defer`, `when`, multi-return, method and trait
  dispatch, generics, inline `asm`, string literals and global initializers. The
  aggregate work is the one non-mechanical part; `docs/backend.md` describes the
  in-place-or-copy `Val` wrapper that keeps the port's shape.
- **Stage 3** `promote_slots` (mem2reg-lite; `Slot::address_escapes` already exists for it)
  and a peephole pass.
- **Stages 4–6** x86-64: legalize → ISel → **trivial** regalloc → frame layout → encoder →
  ELF writer; then inline asm through that encoder; then linear-scan regalloc plus a
  machine-level verifier. Build trivial regalloc first — it validates everything downstream
  before linear scan exists.
- **Stages 7–9** wasm: standalone `.wasm` (dispatch-loop control flow first, Relooper much
  later), then the relocatable form emscripten needs (decision D5).
- **Stage 10** flip `--backend=native` to the default, soak, then delete `codegen.cpp` and
  the LLVM dependency **after** the soak period (decision revised this session: keep LLVM
  reachable via `--backend=llvm` while the differential test is most needed).
- **Pending sub-item** function-pointer ↔ `anyptr` casts must become a target-conditional
  sema error on wasm — a wasm funcref is a table index, not an address.

## 4. Documentation and spec — **all resolved** (`d422a63`)

- ~~**4.1** Broken `#compile-time-configuration` anchor at `spec.md:652`.~~
- ~~**4.2** `mirage test -o` silently ignored.~~ Now an error; `--emit-ir` under `test`
  documented as the way to inspect the generated wrappers.
- ~~**4.3** Undocumented limitations.~~ Multi-byte character literals and the
  `@no_discard`-through-a-function-pointer hole are now documented. Two items on that list
  were wrong: `ext fn` unions were already documented at `spec.md:1249`, and the
  globals/trait-method restrictions became features rather than limits. The
  "multi-value capture is not yet supported here" message, which read as an unfinished
  feature, now names the construct that fixes it.
- ~~**4.4** No spec version marker.~~ Added.

## 5. Tooling and infrastructure — **all resolved**

- ~~**5.1** Editor tooling stale; `editor_grammar_test.py` could not catch that class of
  drift.~~ `81bdc3d` — the test now *derives* the attribute and `$`-directive sets from
  `ast.cpp` and asserts the VS Code grammar matches in both directions (verified to fail on
  the previous state). The tree-sitter grammar gained attribute support and had a real bug
  fixed: it spelled the type queries `sizeof` rather than `size_of`/`align_of`/`type_of`/
  `type_info_of`, which made `highlights.scm` fail to *compile*, costing Zed all
  highlighting. That also cut parse errors on a real source file from 43 to 11.
  The Zed extension has since been dropped (unused), so the tree-sitter grammar is no
  longer consumed by anything in this repo; its remaining 11 parse errors are that repo's
  concern if another editor ever wants it.
- ~~**5.2** No `just test`, no CI.~~ `f410a70` — `just test` runs ctest plus every
  `tests/*_test.py`, continuing past failures and listing them together;
  `.github/workflows/ci.yml` does the same, checking the stdlib out as a sibling so the
  Justfile's own default applies unchanged. **The workflow has never run** — written
  without a runner to verify against, so expect the first push to need adjustment.
- ~~**5.3** `MIRAGE_STD` requirement undocumented.~~ Now in the README and the recipe.

## 6. Test-suite hygiene

- ~~**6.1** Stale known-bug annotation claiming `examples/lexer` hangs the parser.~~
  `cce8954` — it parses in ~3 ms; the hang was fixed by `85d73f9` and the comment was never
  updated.
- **6.2** ~145 positive fixtures still pinned by exit code in `examples_expected.json`
  rather than migrated to `@test` functions in `tests/mir/` (decision D7). **Deferred by
  decision this session** in favour of the backend. Not a correctness gap — those fixtures
  all pass — but their assertions say "exit 24" where they could say what they expected.
  Clusters, largest first: generics 18, attributes 13, asm 9, traits 9, errors 7, types 7.
- ~~**6.3** `#link(lib, …)` linking untested.~~ Claim was wrong: both `ext_abi_test.py` and
  `cdecl_abi_test.py` link through `#link(lib, "helper.o")` end to end.

## 7. Smaller observations — **all resolved**

- ~~Latent `.at()` aborts reachable from a legal program.~~ `dc10a75` — two found and
  fixed, both the same shape as the `@test` body-emission crash: `validate_hosted_main`
  accepted a declaration codegen never inserts into `functions_` (a generic `main`, and
  `@test` on `main`), and `emit_start` then aborted with an uncaught `out_of_range` and no
  diagnostic at all. Regression fixtures pin both.
- ~~`mirage test` did not validate `main`'s signature.~~ Same commit — absent is fine under
  `test`, present is now validated under every action.
- ~~Module iteration order was hash-derived.~~ `a8604de` — codegen now emits in
  `ast::Program::module_order` (source order). Never a reproducibility bug; the point is
  that an unrelated change no longer reshuffles the whole output, which matters because
  reading and diffing emitted IR is the backend port's primary debugging surface.
