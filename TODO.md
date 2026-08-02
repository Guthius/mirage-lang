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

  **Coverage today: 18 of 271 corpus modules lower fully.** That number is the stage-2
  progress metric.

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
