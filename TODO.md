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

- **Stage 2, fourteenth increment** — the middle of the histogram in one pass:
  ternaries and non-folded `when` expressions (one shared then/else/join shape, control
  flow rather than `select` because the unchosen side's effects must not run; scalar
  results merge through a block parameter, aggregates through a result slot), folded
  `when` expressions (only the sema-selected side is emitted), `size_of`/`align_of`
  (compile-time constants read from sema's layout, with the bare-type-name operand
  resolved through the symbol table exactly as codegen's `resolve_operand_type` does),
  and `defer` — scope-tracked, emitted LIFO at the block's end, at every `return`
  (including `try`'s propagate path), and at `break`/`continue` down to and including
  the loop body's scope. Return values are evaluated and sret slots written BEFORE the
  defers run, matching codegen's order. The mirgen_test refuse-loudly probe moved from
  `defer` to inline `asm`, which needs the stage-5 encoder.

  **Coverage: 73 of 271; zero verifier failures.** Cleared: ternaries 24→0, `defer`
  17→0, `size_of` 13→0, `when` expressions 7→0, `align_of` 6→0.

  Largest remaining blockers: calls into not-yet-declared shapes (28, mostly
  generics), `type_info_of` (18), calls dropping an ignorable error (13), generic
  method calls (`try_get` 12, `get`, `put`, ...), slice-forming casts (9), inline
  `asm` (9+7), bitset literals (9).

- **Stage 2, fifteenth increment** — bitsets (literals folding to one mask constant,
  member references AS their one-bit `1 << (value + 1)` masks, `in` as
  `(set & mask) == mask`, `+=`/`-=` as set union/difference) and slice-forming casts
  (the `(data, len)` header, with the explicit length winning over the operand's own
  extent for every operand shape — the mirage303 ISSUES.md #6 rule). Plus a compound-
  assignment fix committed separately: `emit_assign` never read `AssignExpr::op`, so
  `x += 2` lowered as `x = 2` — type-correct MIR, invisible to the verifier, found by
  reading against codegen.

  **Coverage: 77 of 271; zero verifier failures.**

  Largest remaining blockers: generics (~55 combined: 28 "this call form" plus the
  named generic-method calls), `type_info_of` (18), calls dropping an ignorable
  error (13), inline `asm` (16), member accesses through generic types (9).

- **Stage 2, sixteenth increment** — generics. One function per instance in
  `generic_fn_instances_needed` (an ordered set, so declaration order is
  deterministic), mangled `symbol_name(module, instance.mangled_name)` to match
  codegen. Call sites route through sema's `expr_generic_fn_instance` record — checked
  before ANY shape-based resolution, since the same call node resolves to a different
  instance per enclosing instantiation — consulting BOTH key spellings sema uses (the
  Expr variant-slot address for value calls, the CallExpr's own address for
  group/forwarded/`try` calls). Instance bodies are emitted with that instance's own
  expr side tables and its substitution env pushed on `active_generic_env_stack`,
  which is what lets `size_of(T)` and parameter-dependent local type annotations
  (`mut buf: [N]u8`) resolve; `operand_named_type` reads the env for a bare `T`
  exactly as codegen's `resolve_operand_type` case 2 does. `fnv1a[i32]` as a
  function-pointer VALUE resolves to the instance's address.

  **Coverage: 86 of 271; zero verifier failures.** Cleared: the whole generic
  cluster (~55): "this call form" 28→0 in its generic portion, `try_get`/`get`/`put`/
  `remove`/... method calls, generic member accesses, `size_of(T)`-in-instance.

  Largest remaining blockers: `type_info_of` (18), calls with defaulted arguments
  (13), calls dropping an ignorable error (13), inline `asm` (16), `default` (6),
  `for-in` over non-array operands (6), argument spreads (4).

- **Stage 2, seventeenth increment** — the next tail tier in one pass: range `for-in`
  (a usize-width counting loop with the index bound to the COUNTER, the element
  re-narrowed per iteration — codegen's RangeExpr arm), untagged unions (members alias
  offset 0; the single-member literal; `lvalue_type` through a union), member chains
  rooted at namespaced globals (`other.origin.x = 42`), `default`/`undefined` in
  expression and initializer-element positions, bare-import call redirection to the
  origin module's declaration, and defaulted arguments — evaluated at the call site in
  the CALLEE's context (declaring module's tables, no caller locals in scope), with a
  generic's defaults additionally emitted under the instance's substitution env and
  its own expr tables.

  **Coverage: 104 of 271; zero verifier failures.**

  Largest remaining blockers: `type_info_of` (18) and the reflection tables behind
  it, calls dropping an ignorable error (13, needs the runtime unhandled-error panic
  path), inline `asm` (18, needs the stage-5 encoder), macro calls (`align_up` 9 +
  `hook` 2 — expression-template expansion), argument spreads (4), `stackalloc`,
  `$option` as a value, and a handful of singletons.

  Still entirely absent from stage 2: inline `asm`, global initializers,
  `type_info_of`/reflection tables, macros, the runtime unhandled-error check.

- **Stage 2, eighteenth increment** — macros: expression-template expansion, with each
  argument carrying its own captured call-site context (module, expr tables, and the
  macro args active there) so a parameter reference inside the template evaluates the
  argument where it was written, while the template itself emits under the macro's
  declaring module — codegen's `MacroArg`/`emit_macro_arg` shape, `outer_args`
  restoration for nested macros included.

  **Coverage: 110 of 271; zero verifier failures.**

  Largest remaining blockers: `type_info_of` (18) and the reflection tables, calls
  dropping an ignorable error (13), inline `asm` (18), argument spreads (4), plus
  singletons (`stackalloc`, `$option` as a value, a forwarded multi-return with slot
  coercions, an array `...` fill).

- **Stage 2, nineteenth increment** — Mirage-native variadics: the tail collects into
  a fresh backing array with a `(data, len)` header, `xs...` forwards an existing
  slice verbatim, an empty tail is a zero slice. Found as a SILENT hazard, not a
  diagnostic: `emit_call` passed trailing arguments raw (correct only for C `ext fn`
  variadics), so a Mirage variadic callee would have read a slice header out of
  whatever scalar landed in that position.

  **Coverage: 112 of 271; zero verifier failures.**

- **Stage 2, twentieth increment (several commits)** — the long tail: array `...`
  fills (evaluated once, repeated), switch/match patterns folded through sema's own
  `evaluate_integer_constant`, `$option`/`$env` values, method calls through pointer
  receivers (`self.bump()` — lookup strips one pointer level via `lvalue_type`),
  struct-field function-pointer calls rerouting to the indirect path, VALUE generic
  parameters (`[N: usize]`) read off the substitution env, and the runtime
  unhandled-error check: a per-union noreturn panic helper (named
  `__mirage_panic_unhandled_error.<index>` for symbol parity with codegen) that
  dispatches the member name at compile time, writes
  `panic: unhandled Type.Variant at file:line:col` to stderr through lazily-declared
  libc `write`, and exits 101. Value-position drops destructure the surviving slot;
  group declarations check the trailing slot themselves and bind the rest — the same
  route split codegen uses.

  **Coverage: 125 of 271; zero verifier failures.**

  Remaining: `type_info_of` and the reflection tables (18), inline `asm` (stage 5 by
  design, 18), `stackalloc`, a forwarded multi-return with slot coercions (2), and a
  few singletons. The freestanding panic path (syscall, no libc) is deferred with
  inline `asm`, which it needs.

- **Stage 2, twenty-first increment** — reflection. Type_Info descriptors are constant
  globals assembled as BYTE BLOBS with relocations — the representation codegen's
  bespoke packed LLVM struct types were fighting to express — with every offset from
  sema: nested payload structs spliced at field offsets, name strings and entry slices
  as relocations to interned/backing globals, `Type_Kind_Or_Info` degrading a shapeless
  or cyclic reference to `.kind(K)`, one descriptor per `types_needing_info` entry (an
  ordered set), and the sorted `__mirage_type_info_table`. `type_info_of(type_of(T))`
  folds to the descriptor's address (nil for builtin scalar ids 1–15);
  a runtime id — including `type_of(any)`'s word-0 load — goes through the same inline
  binary search codegen emits. Value-to-`any` coercions ride the emit_expr funnel:
  a two-word `{id, data}` blob, scalars spilled to a frame temporary.

  **Coverage: 136 of 271; zero verifier failures.** Cleared: `type_info_of` 18→0,
  `any` coercions, `type_of(any)`.

  Remaining: inline `asm` (18, stage 5 by design, freestanding panic path with it),
  defaulted arguments on trait/generic-shaped method calls (6), a residual
  dropped-error tail (3), `stackalloc` (1), a forwarded multi-return with slot
  coercions (2), and singletons (~8).

- **Stage 2, twenty-second increment — the language surface is COMPLETE except
  inline `asm`.** The last shapes: trait-dispatch and trait-impl-method defaulted
  arguments (a trait impl never redeclares defaults — they live on the TRAIT's method
  declaration and evaluate in the trait's module, codegen's
  `emit_method_trailing_arg`); `callee_return_types` learning trait dispatch, `self`
  receivers (`lvalue_type` + one stripped pointer level), and struct-field
  function-pointer callees — which is what the dropped-error handling on those call
  shapes needed; member access on aggregate temporaries (`holder_of(9).kind`); and
  indexing slice temporaries (`arr[lo..][0]`).

  **The honest denominator, measured: the corpus is 83 positive + 188 negative
  fixtures. 74 of 83 positive modules lower fully, and every one of the 9 that
  remain is inline `asm` or `@naked` — both stage 5 by design (the raw all-dirs
  number is 142/271, but most of that denominator is compile-fail fixtures that can
  never lower).** Zero verifier failures.

  Stage 2 is done, pending only the asm-adjacent tail that cannot exist before the
  stage-5 encoder. Next: stage 3 (`promote_slots` + peephole), and the differential
  harness (`examples_smoke_test.py --backend=`) BEFORE the x86-64 work, per
  `docs/backend.md`'s validation plan.

- **Stage 3, first half — `promote_slots`** (`src/compiler/mir_passes.cpp`). The
  lazy SSA-construction algorithm (Braun et al.) adapted to block parameters: a
  non-escaping slot accessed only by full-width same-type loads/stores becomes a
  value; merge points become join-block parameters; branch/switch edges into a
  parameterized join are split through jump-only trampolines (those terminators
  cannot carry block arguments — the verifier's own rule). Two documented v1
  simplifications: no trivial-parameter elimination, and a store-free path reads a
  zero constant (locals are zero-valued; `undefined` reads are unspecified anyway).
  `--mir-opt` runs it on the `--emit-mir` path with a mandatory re-verify.
  Validated three ways: 5 new ctest cases (straight-line, diamond, loop back-edge,
  branch-edge splitting, refusal on escaping/mixed-width slots), a mirgen_test case
  reading the optimized output, and a corpus sweep — all 142 lowering modules run
  the pass with the verifier green. `x = 40; x += 2; return x` is now three
  instructions and zero slots.

  Remaining in stage 3: the peephole pass (const-fold, identity ops, trivial-param
  folding). Then the differential harness, then stage 4.

- **Stage 3, second half — `peephole`.** Four local cleanups to a fixpoint:
  width-correct integer constant folding (operands truncated to the type's bits,
  sign-extended only where the OP is signed; division by zero and oversized shifts
  are never folded — that would delete runtime behavior), identity simplification
  (`x+0`, `x*1`, `ptr.add p,0`, ...) via one remap sweep, trivial/unused
  block-parameter removal (undoing `promote_slots`' deliberate redundancy, argument
  columns removed from every predecessor jump), and dead pure-instruction
  elimination (which deletes the orphaned operand chains promotion leaves).
  `--mir-opt` now runs both passes plus the mandatory re-verify. Three new ctest
  cases (fold/identity/DCE combined, division-by-zero preservation, trivial-param
  removal); the corpus sweep stays green across all 142 lowering modules. The
  `x += 2` loop probe now emits `const.int 42` and a single-parameter loop header.

  **Stage 3 is complete.** Next per `docs/backend.md`: the differential harness
  (`examples_smoke_test.py --backend=...`) BEFORE stage 4, then x86-64
  (legalize → ISel → trivial regalloc → frame → encoder → ELF).

- **The differential harness, written before stage 4 as the plan requires.**
  `--backend=llvm|native` is a real driver flag: native runs the full pipeline as it
  exists (mirgen → promote_slots → peephole → verify) and then refuses with a fixed
  stage-4 message the harness keys its skip-detection on.
  `tests/backend_differential_test.py` compiles (and runs, where runnable) every
  positive fixture under both backends and compares exit code and stdout, with four
  outcomes: match (the only passing state once stage 4 lands), MISMATCH (always a
  failure), awaiting-stage-4 (counted, shrinks as object generation lands), and
  named-refusal (mirgen's loud `cannot lower X yet` — today exactly the 9 asm/naked
  fixtures, listed by name so growth is visible as a regression). Current audit:
  74 positive fixtures = 65 awaiting stage 4 + 9 named asm refusals + 0 anything
  else — stage 2's completeness claim, machine-checked on every `just test` run.
  `examples_smoke_test.py` also grew the `--backend` pass-through the plan names.

  Next: stage 4 — x86-64 legalize → ISel → TRIVIAL regalloc → frame → encoder →
  ELF, in that order; `examples/start` runs first, then this harness's
  awaiting-stage-4 count starts falling.

## Stage 4 — x86-64: **the native backend produces working executables**

Built bottom-up in one pass: encoder, ELF writer, trivial-regalloc ISel, entry glue,
driver wiring.

- **`src/compiler/x86_encoder.{hpp,cpp}`** — byte-exact instruction encoding for
  exactly what the ISel emits, no more. Expected bytes cross-checked against GNU `as`
  (validation #3); label-fixup rel32 jumps; `Call32`/`Rip32` relocations against
  caller-owned symbol indices. 44 ctest assertions.
- **`src/compiler/elf_writer.{hpp,cpp}`** — ELF64 relocatable objects: fixed section
  order, locals-before-globals symbol ordering with `sh_info` set correctly, `.rela`
  sections per target section.
- **`src/compiler/backend_x86.{hpp,cpp}`** — the TRIVIAL allocator, as the plan
  demands before linear scan: every MIR value owns an 8-byte frame slot, every
  instruction loads operands into fixed scratch registers and spills its result.
  Block parameters get a staging slot plus a canonical slot, making the
  swap/rotation hazard impossible by construction rather than by analysis. System V
  calls (6 int + 8 SSE registers, stack tail, `AL` = vector count). Entry glue
  matching codegen's `_start` for all three `main` shapes.
- **`--backend=native` is now a real pipeline**: mirgen → promote_slots → peephole →
  verify → ISel → ELF → the SAME link/run tail the LLVM path uses.

**Differential result: 65 of 65 non-asm positive fixtures produce identical exit
codes and stdout under both backends.** (One, `example_fnptr3`, is compared on exit
code only — its own comment documents that it prints an unspecified value on the
failed path; LLVM leaves stack garbage there, native leaves a deterministic zero.)

The harness found seven real bugs on its first runs, every one of them a silent
miscompile that no existing test could see, since nothing had executed MIR before:
an indirect call through a struct FIELD called the field's address; a by-value
aggregate `for-in` binding stored the element's address instead of copying it;
`!err` took an error aggregate's address down the pointer path and was always false;
scalar global initializers were dropped (`const alignment := 8` became 0, so every
native allocation divided by zero); `++`/`--` returned the new value in postfix
position, shifting every byte-copy loop; `cast(any, *T)` yielded the blob address
instead of the data word; and int↔float casts fell through to `Bitcast`, printing
3.14159 as 4614256650576692846.

## Stage 5 — inline `asm`: **the whole corpus now matches, 74 of 74**

The plan predicted this would be *simpler* without LLVM, and it was. With no
register allocator there is no constraint model: every variable operand is simply
its frame slot, so `mov &fd, eax` encodes directly as `mov [rbp-off], eax`. Sema had
already done the hard part (mnemonic validation, operand directions, the clobber
set), so MIR carries a resolved `AsmBlock` — instructions with register/immediate/
variable operands, where a variable operand indexes the instruction's argument list
and each argument is a pointer to that variable's storage. mirgen pins those slots
against `promote_slots`; the backend resolves each back to its `rbp` offset and
encodes. The `asm -> reg` expression form stores that register at block exit.

The encoder grew the forms hand-written asm needs but ISel never emits: memory
operands across the ALU/mov family, `movzx` from memory, the unary `/digit` group,
`push`/`pop` on memory, and `nop`/`syscall`/`cpuid`. An instruction it cannot render
is a NAMED error, never a silent drop.

Found by this work: `for_each_value_operand` reached `Op::Asm` through its default
case, which would have handed the MIR passes an asm BLOCK INDEX as a value id — and
worse, let `promote_slots` believe an asm-referenced slot was unused.

**Differential result: 74 of 74 positive fixtures — the entire corpus — produce
identical exit codes and stdout under `--backend=llvm` and `--backend=native`.**
`mirgen_test`'s refuse-loudly probe moved from `asm` to `stackalloc`, which is now
one of only three constructs left unlowered (with `hook` macros in one fixture and
forwarded multi-returns needing slot coercions).

Closing the last three unlowered constructs found two more silent miscompiles, both
of the same shape as the earlier ones — something that *looked* lowered but wasn't:

- **Aggregate global initializers were dropped entirely.** `const S: [3]i32 = {7,8,9}`
  became zeros, so a fixture reading `S[0]` printed 0 and still "passed" everything
  except the differential run. Globals now fold through one recursive
  `constant_blob` (scalars via sema's own evaluator, structs and arrays spliced at
  sema's offsets, strings as a relocation plus length, `type_of(T)` as its interned
  id), which also replaced the ad-hoc scalar/string special cases.
- **A forwarded multi-return with a dropped `?error` slot** returned the first value
  instead of the blob; the drop wrapper now yields the blob whenever two or more
  values survive, as codegen does, and forwarding rebuilds slot by slot when the
  layouts genuinely differ (`[]i32` → `*i32`).

Plus function-pointer GLOBALS as callees (`hook()`, `mod.hook()`), which had no
route at all.

**`stackalloc` closed the list: mirgen now lowers every construct the corpus
contains.** It is a dynamic frame extension (`Op::StackAlloc`), not a slot — the size
is a runtime value — so the backend rounds it up, subtracts from `rsp`, and returns
the new top; the epilogue's `mov rsp, rbp` frees it. With nothing left to refuse,
`mirgen_test`'s probe changed shape rather than target: it now compiles a program
using every construct that was once hardest (defer, multi-return, tagged unions,
traits, generics, `stackalloc`, `asm`) and asserts the coverage summary is EMPTY —
the same property the probe always tested, stated from the other side.

Docs brought current per the definition of done: `docs/backend.md`'s status and
stage list (2–5 marked done, with how each actually landed and which predictions
held), the validation section (the differential harness's four-way report and what
it caught), and `README.md`'s backend paragraph and flag list.

- **Validation #2 — `mirage test` on both backends — done, and it found the gap it
  exists to find.** `mirage test --backend=native` was silently producing a
  *crashing* binary: the test-runner entry point is synthesized by the backend, and
  mirgen had none, so a module with no `main` linked with no `_start`. mirgen now
  synthesizes the same shape codegen does — one wrapper per discovered `@test`
  (calling it and reporting Ok from the error blob's tag), the `Test_Info` descriptor
  built with the same `ConstBlob` machinery reflection uses, and an entry that calls
  `core/testing`'s `_run_tests` instead of `main` (which is compiled but never
  invoked, so an ordinary program can be tested without restructuring). `@test`
  functions are kept rather than skipped under test mode, and `$rtti_enabled` — the
  one construct `tests/mir/generics` needed — lowers as the constant sema fixed
  before any source was read.

  **All 35 assertion-carrying tests in `tests/mir/` now pass under both backends**,
  and `mir_suite_test.py` runs both, so it stays true. This matters more than the
  differential run for one reason: a native miscompile surfaces here as a *named
  failing test*, where the differential harness can only report a diverging exit
  code.

Next per `docs/backend.md`: stage 6's linear-scan allocator plus a machine-level
verifier, differential-tested against this trivial allocator — which stays
permanently as `--regalloc=trivial`, the standing triage tool.

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
