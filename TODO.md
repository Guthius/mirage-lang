# Outstanding issues and gaps

A review of the compiler as of the `feat/pre-self-hosting` branch. Each item has a repro or
a file reference where one exists. Ordered by severity within each section; the sections
themselves are roughly ordered by urgency.

Nothing here is speculative — every behavioural claim was reproduced against
`build/mirage` at commit `2ece065`.

---

## 1. Correctness bugs

### 1.1 `@no_discard` on a generic function is rejected with a wrong diagnostic — **blocks legitimate code**

```mirage
@no_discard
fn checked[T: type](v: T) -> T { return v }
pub fn main() -> i32 { return checked(1) }
```
```
error: '@no_discard' on a function with no return value has no effect
```

The function plainly returns a value. A generic's `FunctionSymbol::return_types` is never
populated (a template has one signature per instantiation, none of which exists yet), so
`validate_no_discard_attribute`'s `return_types.empty()` check reads "no return value".

This is the same root cause as the generic-`@test` double-diagnostic fixed in
`9540176` — that fix returned early for generics, and `@no_discard` needed the same guard
and did not get it. **Fix:** skip the return-shape check for a generic declaration, exactly
as `validate_test_structural` does.

Same root cause, milder symptom: `@no_return` on a generic returning a value silently skips
its "will never return its value" warning (`is_void_or_error_return` answers true for an
empty list). A false negative rather than a false positive.

*`src/compiler/sema_attributes.cpp`, `validate_no_discard_attribute` /
`validate_no_return_attribute`.*

### 1.2 `@export` on a method of a *generic type* is silently ignored

```mirage
pub type Box[T: type] = struct { v: T }
impl Box[T: type] {
    @export("boxget")
    pub fn get(self) -> T { return self.v }
}
```
Emits `define internal i32 @"__mir_..._Box__i32::get"` — no `boxget`, and no diagnostic.

`validate_method_attributes_for_module` skips methods with `!info.is_resolved`, and a
generic type's method template is never resolved (each instantiation resolves separately).
So the attribute is neither validated (it should be the "not allowed on a generic function"
error) nor honoured.

This is the same *validated-but-ignored* class as the bare-impl/trait-impl case fixed in
`b432963`; that fix reached the resolved-method paths but not the generic-template one.
The right answer is almost certainly to reject it, as for generic free functions.

### 1.3 An `@export` name can collide with the compiler's own mangling

`@export("__mir_something")` is accepted. `is_plausible_symbol_name` is deliberately
permissive about characters, but nothing reserves the `__mir_` / `__mirage_` prefixes that
`symbol_name()` and the test-wrapper synthesis generate. A crafted export name could shadow
a real mangled symbol.

Low likelihood, trivial fix: reject those two prefixes with a message saying they are
compiler-reserved.

*`src/compiler/sema_attributes.cpp`, `is_plausible_symbol_name`.*

---

## 2. Features that are accepted but wholly or partly inert

### 2.1 `@import` is validated and then never read — **inert on every target**

`ExtFunctionSymbol::import_module` / `import_name` are written by
`validate_ext_function_attributes_for_module` and read by nothing:

```
$ grep -rn 'import_module' src/ --include='*.cpp' | grep -v sema_attributes
(nothing)
```

The attribute exists for the native wasm backend, which does not exist yet (§3). On the
current emscripten path, imports are resolved by `emcc`, so `@import` changes nothing at
all. This is by design *for now*, but a user writing `@import("wasi_snapshot_preview1", …)`
today gets silence rather than an effect.

Options: leave it (documented in spec §21 as target-conditional and currently a no-op —
which the spec already says), or emit a warning on non-wasm targets. The spec deliberately
argues against the warning, so the honest gap is just that the wasm consumer is missing.

### 2.2 `@export` does not produce a wasm export

`@export` sets the symbol name and external linkage. On wasm, appearing in the module's
**export section** is a separate act that the LLVM/emscripten path leaves to `emcc`'s
`-sEXPORTED_FUNCTIONS`. So `@export` on a wasm target does not do what its name implies.

Blocked on the native wasm backend (§3, stage 7), which is where export-section emission
belongs. Worth a spec note in the meantime.

### 2.3 `@no_discard` fires on only three of the call paths

Recorded at exactly three sites in `resolve_call_returns`: cross-module free function,
same-module free function, and direct method call. Not recorded for:

- **generic function instantiations** (the `record_instance` path) — so even after §1.1 is
  fixed, `@no_discard` on a generic would still never fire at a call site;
- **trait-dispatched calls** — arguably moot, since trait method *declarations* cannot carry
  attributes at all (`pub type T = trait { @no_discard fn f(self) -> i32 }` is a parse
  error);
- **calls through a function pointer** — silently accepted:
  ```mirage
  @no_discard
  fn f() -> i32 { return 1 }
  pub fn main() -> i32 { const p: fn() -> i32 = f  p()  return 0 }   // no diagnostic
  ```

The function-pointer case is defensible (the attribute is a property of the declaration, not
of the pointer type — the same reasoning as D2's convention-free function pointers) but it
is undocumented. The generic case is a genuine hole.

### 2.4 `@callconv("c")` is unsupported on impl methods

A deliberate v1 limitation with a clear error (`b432963`), not a silent gap — listed here
because it is a real capability hole: a method cannot be exposed to C. Lifting it means
marshalling the receiver and routing method call sites through the C-ABI path.

### 2.5 Attributes are not permitted on globals or trait method declarations

`@export` on a `pub mut` global is rejected with "attributes are only allowed on 'fn' and
'ext fn' declarations". Exporting a global is a natural C-interop expectation and is
currently impossible. Likewise attributes inside `type X = trait { … }` are a parse error,
which is what makes §2.3's trait case moot.

Both are defensible v1 scope; neither is stated as a limitation anywhere.

---

## 3. The backend (item 1 of the self-hosting work) — largest outstanding gap

Only the IR foundation exists: `src/compiler/mir.{hpp,cpp}` with `tests/mir_test.cpp` in
ctest. **Nothing consumes it. LLVM remains the only code path.**

`docs/backend.md` is the full design record — remaining stages 2–10, their sequencing and
the reasons for it, the validation strategy, and decisions D1–D8. Summarised here only so
this file is a complete inventory:

- **Stage 2** port `codegen.cpp` → `mirgen.cpp` (the large mechanical step; aggregates are
  the one non-mechanical part)
- **Stage 3** `promote_slots` + peephole
- **Stage 4–6** x86-64: legalize → ISel → trivial regalloc → frame → encoder → ELF, then
  inline asm through that encoder, then linear-scan regalloc + machine verifier
- **Stage 7–9** wasm: standalone `.wasm`, then the relocatable form emscripten needs (D5),
  then Relooper
- **Stage 10** flip `--backend=native`, delete `codegen.cpp` and the LLVM dependency

Sub-items with no home in that document:

- **MIR has no driver surface.** `--emit-mir` does not exist and `mir::verify` is called
  only from tests. Both arrive with stage 2, but until then the IR is unreachable from the
  CLI.
- **`mir::Builder` lifetime footgun.** It holds `Module&` plus an index and re-resolves on
  each use (correct), but a caller that holds `auto &fn = b.function()` across a
  `module.functions.push_back(...)` gets a dangling reference. `mirgen` will build many
  functions; worth an assertion or a comment before that happens.
- **Function-pointer ↔ `anyptr` casts must become a target-conditional sema error on wasm**
  (a wasm funcref is a table index, not an address). Already noted as *pending* in
  `docs/backend.md`; it needs doing when stage 7 lands.

---

## 4. Documentation and spec gaps

### 4.1 Broken internal link in `spec.md`

`docs/spec.md:652` links `[Compile-Time Configuration](#compile-time-configuration)`. The
heading is `## 12. Compile-Time Configuration`, so the anchor is
`#12-compile-time-configuration` — which is how the other four references to it are spelled
(lines 943, 1739, 1901, 2963). One-character fix.

(Two other anchors flagged by a naive checker — `#ignorable-errors-` and `#when-statement-1`
— are **correct**: GitHub strips punctuation and suffixes duplicate headings. No action.)

### 4.2 `mirage test` flag interactions are undocumented

- `mirage test -o <path>` silently ignores `-o`. The test binary always goes to a temp file.
  Either honour it or reject it; currently it looks like it worked.
- `mirage test --emit-ir` emits IR and never runs the tests. Reasonable, undocumented.

Neither appears in `spec.md` §23 or the README.

### 4.3 Undocumented limitations

None of these is stated anywhere, and each is surprising:

- `@no_discard` does not fire through a function pointer (§2.3).
- `@export` on a wasm target does not create a wasm export (§2.2).
- `@export`/attributes are not available on globals or trait method declarations (§2.5).
- Union types are rejected in `ext fn` signatures (`sema.cpp:147,164`,
  "union types are not yet supported in extern function signatures") — a real ABI hole with
  no spec mention.
- Multi-byte character literals are rejected (`lexer.cpp:662`); the spec's Literals section
  does not say so.
- "multi-value capture is not yet supported here" (`sema_check.cpp:4392`) — unclear which
  construct this covers; needs either a spec note or removal if unreachable.

### 4.4 `docs/spec.md` has no changelog or version marker

The spec is now ~4,300 lines describing 23 sections and has changed substantially. There is
no way to tell which compiler revision a given spec statement corresponds to. A short
"last updated against commit X" line, or a changelog section, would make drift detectable.

---

## 5. Tooling and infrastructure

### 5.1 Editor tooling is stale — **will actively mis-highlight new code**

`editors/vscode/syntaxes/mirage.tmLanguage.json`:
- line 180 and 195: attribute pattern is `@(no_return|naked|always_inline|section|init)` —
  missing all six of `no_discard`, `export`, `callconv`, `cdecl`, `import`, `test`.
- line 168: `\$(option|env)` — missing `rtti_enabled`.

`editors/zed/languages/mirage/highlights.scm` appears to have no attribute rule at all.
The tree-sitter grammar lives in a separate repo (`../tree-sitter-mirage`) and was not
reviewed here — it likely needs the same additions.

`tests/editor_grammar_test.py` passes, so it does not check attribute-set completeness
against the parser. That is the actual defect: the test cannot catch this class of drift.

### 5.2 No `just test` recipe, and no CI

The full battery is now ctest (6 targets) **plus 25 Python suites**, two of which need
`--std` pointed at the stdlib checkout. Running it correctly is entirely manual and
undocumented outside a memory note. There is no `.github/workflows/` in either this repo or
the stdlib repo.

At minimum: a `just test` recipe that runs ctest and every `tests/*_test.py`, failing on the
first non-zero exit. That is a ~10-line addition and would make "no regressions" checkable
by someone who did not write the suites.

### 5.3 Two Python suites silently no-op without the stdlib

`test_action_test.py` and `mir_suite_test.py` need `core/testing`. They fail with a clear
message if `MIRAGE_STD` is wrong — good — but nothing in the repo documents that
requirement outside those files' own docstrings.

---

## 6. Test-suite hygiene

### 6.1 A stale known-bug annotation in the corpus gate

`tests/examples_smoke_test.py`'s `SPECIAL_CASES` pins `"lexer"` with `timeout: 5` and this
comment:

> Parsing 'examples/lexer/token.mir' never terminates: line 16 uses a postfix
> `kind match { ... }` form that grammar.md does not define … Drop the timeout override once
> the parser rejects the form.

**The hang no longer reproduces.** `mirage build examples/lexer --emit-ir --freestanding`
completes in ~9 ms, and the described construct is not at `token.mir:16` any more. Either the
parser was fixed or the fixture was edited; either way the override and its comment are
misleading, and the comment asserts a live bug that no longer exists.

Verify, then remove the override — or, if the underlying parser weakness is real but merely
no longer triggered, write a fixture that does trigger it.

### 6.2 `examples/` is now 270 directories, ~110 of them negative fixtures

Per decision D7 the compile-fail fixtures stay in `examples/` pinned by
`examples_expected.json`, which is correct — and the harness already guards both
directions (`examples_smoke_test.py:243-246` reports both "present in examples/ but has no
baseline entry" and "stale baseline entry, directory no longer exists"), so nothing rots
silently.

The remaining concern is only size: a 270-entry corpus where roughly 140 entries are
positive fixtures that D7 says *should* migrate to `tests/mir/`. Only 4 modules / 35 tests
have migrated so far. Continuing that migration is the outstanding work; the gate itself is
sound.

### 6.3 `examples/example_raylib_link` can never link here

Its `#link(lib, "linux/libraylib.a")` names an archive that is not vendored, so it is pinned
as `link-directives` only. Fine, but it means the `#link(lib, …)` *path resolution* is
tested while actual linking of a `lib` directive is not tested anywhere.

---

## 7. Smaller observations

- **189 `.at()` calls in `codegen.cpp`** and 68 "internal error" reports across the front
  end. The `.at()` calls abort rather than diagnose; the `@test` body-emission crash fixed
  in `9540176` was exactly this shape (an `out_of_range` abort where a skipped type-check
  left a table empty). Worth a sweep for the ones reachable from a *legal* program state
  under a new driver action.
- **`mirage test` does not validate `main`'s signature.** `validate_hosted_main` is skipped
  in test mode (correct — `main` is optional there), so a `main` with a bad entry-point
  signature compiles silently under `test` and fails only under `build`. Consistent with
  "main is never called", but a user testing a program with a malformed `main` gets no
  warning.
- **Module iteration order is stable but not meaningful.** `codegen.cpp` walks
  `sema_program_.modules` (an `unordered_map`) in five places to emit declarations,
  functions and globals. Builds *are* reproducible — emitted IR is byte-identical across
  repeated runs, for both `example_reflection` and the 90-file `mirage303`, because the hash
  order is a deterministic function of the same insertion sequence. So this is **not** a
  correctness or reproducibility bug.

  What it does mean is that emission order is hash-derived rather than source-derived, so an
  unrelated change (adding a module, renaming a path) can reshuffle the whole output and make
  IR diffs between two builds uninformative. `ast::Program::module_order` now exists and
  gives a meaningful order, but has exactly one consumer (`@test` discovery). Worth adopting
  in codegen when the backend port lands — diffable output is the primary debugging surface
  for that work, per `docs/backend.md`.

  Related and already handled: the `@export` collision check sorts by source position before
  reporting precisely because hash order decided *which* of two colliding declarations was
  blamed. Other whole-program passes that report diagnostics from an `unordered_map` walk
  are worth auditing for the same issue.
