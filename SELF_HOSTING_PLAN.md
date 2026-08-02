# Pre-Self-Hosting Implementation Plan

Everything that must land in the C++ compiler (`Mirage-Cpp`) before the compiler is
rewritten in Mirage. Five work items, plus a design answer for standalone wasm.

Nothing here changes the language's surface semantics except where explicitly stated
(§2 attributes, §3 `$rtti_enabled`, §4 module search order, §5 `@test`).

---

## 0. Summary and recommended sequencing

| # | Item | Size | Depends on |
|---|------|------|-----------|
| 4 | Module resolution order | S | — |
| 3 | `--nortti` + `$rtti_enabled` | S | — |
| 2 | `@no_discard` / `@export` / `@callconv` / `@cdecl` / `@import` | M | — |
| 5 | `@test` + `mirage test` + `core/testing` + corpus migration | L | 4 (`--load` resolution) |
| 1 | Mirage IR + native x86-64 / wasm object generation | XL | 2 (`@callconv` feeds the ABI layer; `@import` names wasm imports) |

`@import` is not in the original brief — it is the mechanism chosen in decision **D4** for
naming a wasm import's module, and it lands in item 2 because it is an attribute. All eight
open design questions are settled; see **§9** for the authoritative list.

**Do 4 → 3 → 2 → 5 → 1, in that order.** The reason is not effort ordering, it is
validation: item 5 gives you `mirage test` and a corpus expressed *in Mirage*, and that
corpus is the differential harness you need to prove the new backend (item 1) against the
LLVM one. Doing the backend first means validating 12k lines of new code against a Python
script that only compares exit codes.

Items 4, 3 and the sema half of item 2 are pure front-end work in `mirage_core` and are
independent of each other; they can be done in one sitting each.

**The LLVM backend stays alive throughout item 1** behind `--backend=llvm|native`
(default flips to `native` at the end), so every fixture can be compiled both ways and
diffed. Retire `codegen.cpp` only once the corpus is green on `native` for both targets.

---

## 1. Current state — what the port actually has to move

Facts established by reading the tree, because they decide the design:

**The front end is already LLVM-free.** `mirage_core` (lexer, parser, module resolver,
sema, type/value resolvers — ~22k lines) links no LLVM. Only `src/compiler/codegen.cpp`
(6,943 lines) and `src/main.cpp` (722 lines) include LLVM headers. `mirage-lsp` links
`mirage_core` only, so **the LSP is untouched by item 1** — it cannot break as long as
the `mirage_core` public headers keep their shape.

**Sema owns layout, not LLVM.** `sema::resolved_type_size` / `resolved_type_align` /
`StructInfo::fields[].offset` are computed in the front end from
`sema::Options::pointer_size`. `declare_structs()` then emits every struct as a *packed*
LLVM struct with explicit `[N x i8]` padding elements at the sema-computed offsets, and
`codegen::size_of/align_of` are thin forwarders into sema. **No DataLayout query
influences a single layout decision.** This is the single most important fact in this
document: the new backend needs no structural type system and no data-layout engine — a
type is a size, an alignment, and a scalar kind, and everything else is a byte offset that
sema already computed.

**The instruction surface is tiny.** The full set of `IRBuilder` operations used across
6,943 lines:

- memory: `alloca` (entry-block only), `load`, `store`, `memcpy`, `memset`
- address: `inbounds gep` (const-offset and dynamic-index forms only), `struct gep`
- integer: `add sub mul udiv sdiv urem srem and or xor shl lshr ashr not neg`
- float: `fadd fsub fmul fdiv frem fneg`
- compare: `icmp` (all 10 predicates), `fcmp` (ordered predicates only)
- convert: `trunc zext sext fptrunc fpext fptosi fptoui sitofp uitofp ptrtoint inttoptr`
- aggregate: `insertvalue`, `extractvalue` (single-level indices only)
- control: `br`, `condbr`, `switch`, `ret`, `retvoid`, `unreachable`, `phi` (3 sites), `select` (1 site)
- calls: direct, indirect through a function pointer, and inline asm

No vector types except `<2 x float>` (produced solely by SysV eightbyte classification),
no atomics, no exceptions/landing pads, no intrinsics beyond `memcpy`/`memset`, **no debug
info at all** (zero `DIBuilder` references — DWARF is not a porting obligation), and no IR
optimization passes are ever run (`main.cpp` builds a `TargetMachine` and calls
`addPassesToEmitFile` directly; the module goes to the object writer unoptimized).

**The emitter is memory-form, not SSA-form.** Every local is an `alloca` written and read
through `load`/`store`; `phi` appears in exactly three places. **No SSA construction is
needed in the new backend**, and a straightforward register allocator suffices.

**Target-specific logic is already isolated.** `is_wasm_target()` gates exactly three
things: entry glue (`emit_wasm_main` vs `emit_start`), the `ext fn` ABI
(`classify_aggregate` → `classify_aggregate_wasm` vs `classify_aggregate_eightbytes`), and
the freestanding refusal. `run()` refuses every triple that is not x86-64 Linux or wasm32.

**Inline asm is a bounded x86 subset.** `asm_registers.hpp` enumerates the 64 supported
GPRs (and explicitly rejects SSE/x87/segment/control registers as "not in v1"); the Tier-1
mnemonic table in `sema_check.cpp:5338` lists 30 mnemonics, and the parser accepts only
register / immediate / Mirage-variable / simple-memory operands. Only one file in the
whole standard library uses `asm` (`core/sys/linux/syscalls.mir`). Codegen itself emits
three internal asm blobs: the freestanding `exit` syscall, the freestanding `write`
syscall, and `_start`'s `and $-16, %rsp`.

**Corpus.** 259 fixtures in `examples/`, pinned by `tests/examples_expected.json`:
140 `run`, 110 `build`, 8 `emit-ir`, 1 `link-directives`. Roughly 110 of them are
*negative* fixtures whose expected outcome is a specific diagnostic — this matters for
item 5 (see §5.4).

---

## 2. Item 4 — module resolution order

*(Sequenced first: it is small, and item 5's `--load` flag must resolve by the new rule.)*

### 2.1 Target behaviour

`resolve_import_path()` (`module_resolver.cpp:84`) currently tries two roots. Replace with
five, in order, first hit wins:

1. the **current module's** directory (`importer_path`) — unchanged, still allows `..`
2. the **root module's** directory (`Program::root_module_path`)
3. the **current working directory**
4. the **compiler executable's** directory
5. `$MIRAGE_MODULES_ROOT`

### 2.2 Implementation

**`module_resolver.hpp`** — replace the single `std::string mirage_path` parameter that
threads through `visit`/`resolve_import_path` with a small value type:

```cpp
struct SearchRoots {
    std::string root_module;      // filled by resolve() after canonicalizing the root
    std::string cwd;              // std::filesystem::current_path(), canonicalized once
    std::string compiler_dir;     // see 2.3
    std::string modules_root;     // --std= override, else $MIRAGE_MODULES_ROOT
};
```

Build it once in `resolve()` and pass by const-ref. Each field is empty when unavailable;
`resolve_import_path` skips empty roots. Keep `resolve()`'s existing
`std_path_override` parameter — it now overrides root 5 only.

**Containment.** Today `is_contained_in` guards only the `MIRAGE_PATH` root. Apply it
uniformly to roots 2–5 (a stdlib-relative `import("../../../etc")` should not escape its
root), and keep root 1 unguarded — the corpus depends on `..` traversal from the importing
module (`examples/example_reflection` reaches `runtime/type_info` that way, and
`example_attr_init_cycle/b` uses `import("..")`). Pin this asymmetry with a comment; it is
a deliberate decision, not an oversight.

**Absolute-path rejection** stays as-is and applies before any root is tried.

### 2.3 Locating the compiler executable

`/proc/self/exe` via `std::filesystem::read_symlink` on Linux. Fall back to `argv[0]`
resolved against `$PATH` when it contains no separator. The driver computes this once and
hands it to `ast::resolve()` — do **not** call `getenv`/`/proc` from inside
`module_resolver.cpp`, which is shared with the LSP and must stay side-effect-light.
Because the LSP also calls `ast::resolve()`, give `SearchRoots` sane empty defaults so an
LSP that does not supply a compiler path still resolves roots 1–3 and 5.

Root 4 probes **both `<exe_dir>` and `<exe_dir>/../lib/mirage`, in that order** (decision
D6), so a `/usr/local/bin/mirage` + `/usr/local/lib/mirage/core/...` install layout resolves
stdlib imports with no environment variable set at all — which is the main reason this root
exists. `just install` places the binary in `<prefix>/bin`, so the second probe is the one
that will actually fire in a real install. Both probes are reported by the failure
diagnostic in §2.4 so the two-directory behaviour is discoverable.

### 2.4 Diagnostics and migration

- On failure, list every root that was tried:
  ```
  error: cannot resolve import path 'core/io' from '<module>'
    searched: <importer dir>
              <root module dir>
              <cwd>
              <compiler dir>
              <compiler dir>/../lib/mirage
              $MIRAGE_MODULES_ROOT (<value> | not set)
  ```
  Today the message names neither the roots nor why they failed; that has already cost
  debugging time once (see the `MIRAGE_PATH` warning added in `resolve()`).
- Keep the "`$X` is set but is not a directory" warning, retargeted at
  `MIRAGE_MODULES_ROOT`.
- **`MIRAGE_PATH` is dropped outright** (decision D8) — it is not consulted, not a
  fallback, and does not participate in resolution. One concession to discoverability,
  which is *not* a fallback: if resolution fails **and** `MIRAGE_PATH` is set **and**
  `MIRAGE_MODULES_ROOT` is not, append a single note to the diagnostic above:
  ```
  note: MIRAGE_PATH is set but is no longer consulted; use MIRAGE_MODULES_ROOT
  ```
  This costs nothing, changes no resolution behaviour, and prevents a silently baffling
  failure in an environment that has been working for months. `mirage303`'s build line uses
  `--std=`, which is unaffected. Announce the removal in the commit message and README.
- Add `--print-module-search` (debug-only, like `--dump-ast`): prints each import and the
  root that satisfied it, then exits. Ambiguity is resolved silently by order; this flag is
  how you see it.

### 2.5 Test impact

Adding roots is additive — every currently-resolving import still resolves at the same or
an earlier root. The one new risk is **cwd (root 3) newly satisfying an import that used to
fail**, which would flip a negative fixture to passing. Run `examples_smoke_test.py` from
the repo root and review; add fixtures for: resolution from root 2 when root 1 misses,
resolution from root 4, containment rejection at root 5, and the five-root failure
diagnostic.

---

## 3. Item 3 — `--nortti` and `$rtti_enabled`

### 3.1 What RTTI actually costs today

`type_info_of` support (`codegen.cpp:2121` onward, ~700 lines) emits, per program:

- one `Type_Info` constant global per distinct `ResolvedType` ever reflected on, built
  against the *user-declared* `runtime/type_info` contract (the compiler reads that
  module's struct/union declarations by name and fills them field-by-field), and
- `type_info_table_global_`, the id→`*Type_Info` table that a runtime
  `type_info_of(some_any)` binary-searches.

`any` itself is **not** RTTI: an `any` value is `{u64 type id, ptr}` and an `any` cast is
an integer comparison. Disabling RTTI must not disable `any`.

### 3.2 `--nortti`

New driver flag. Threaded to sema as `sema::Options::rtti_enabled` (default `true`) and to
codegen via `codegen::Options::rtti_enabled`.

Effects, in order of where they bite:

1. **Sema**: `type_info_of(...)` is a hard error —
   `error: 'type_info_of' requires runtime type information; this build was compiled with '--nortti'`
   with a note pointing at `$rtti_enabled`. Nothing else about reflection changes;
   `type_of`, `size_of`, `any` and `any` casts all keep working.
2. **Sema**: stop force-resolving `runtime/type_info` (the existing
   `find_type_info_union` seeding). A `--nortti` program that never imports
   `runtime/type_info` must compile with that module absent from the graph entirely.
3. **Codegen**: `declare_type_info_globals()` becomes a no-op; no `Type_Info` globals, no
   id table. Expect a measurable size win on `mirage303`-scale programs — worth recording
   a before/after number in the commit.

### 3.3 `$rtti_enabled`

A new `$`-sigil compile-time constant expression folding to `bool`. Parsed exactly where
`$option` / `$env` are, in `parse_primary`'s `Dollar` dispatch (`ast.cpp:920`), which
already peeks the identifier after `$` and reports
`expected 'option' or 'env' after '$'` — extend that set and that message.

- **AST**: a new `struct RttiEnabledExpr { SourceLocation location; }` in the `Expr`
  variant. Give it its own node rather than desugaring to `$option("build/rtti")`: the LSP
  and `--dump-ast` should be able to name it, and a user-writable `--opt build/rtti=...`
  would otherwise be able to lie about the compiler's actual behaviour.
- **Sema**: `check_expr` returns `Bool`; `evaluate_const_value` returns
  `Options::rtti_enabled`. It is a constant expression everywhere `$option` is, so it works
  in `when`, in a `const` initializer, and — importantly — in `#compile_only_if`, whose
  condition must fold to exactly `bool` (a pinned decision), which this does natively.
- **Grammar/spec**: add to §12 "Compile-Time Configuration" next to `$option`/`$env`, and
  note it is not a reserved word (same posture as `option`/`env`).

Worked pattern this enables, which is the point of the feature:

```mirage
pub fn describe[T: type](value: T) -> []u8 {
    when $rtti_enabled {
        return format_from(type_info_of(type_of(T)))
    } else {
        return "<no rtti>"
    }
}
```

**The interaction that makes or breaks this feature:** `when` type-checks *both* branches
(a pinned decision, deliberately unlike generics). So the `type_info_of` call above is
still *checked* under `--nortti`, and §3.2's hard error would fire in the branch that was
written specifically to be dead.

**Decision D3: under `--nortti`, the `type_info_of` error does not fire inside a
statically-dead `when` branch.** A ~20-line change in `check_expr`'s `TypeInfoOfExpr`
case, conditioned on the dead-branch state `when` already tracks. Everything else about
the branch is still checked — arity, argument types, the shape of the surrounding
expression — so this suppresses exactly one diagnostic and nothing else.

Three things to get right, since this is a deliberate hole in a pinned rule:

- It is scoped to `--nortti` **and** `type_info_of` **and** a dead `when` branch. It is
  not a general "dead `when` branches are unchecked" relaxation — that would silently
  reverse the pinned decision. Assert the narrowness in the code comment.
- The dead branch's `type_info_of` still needs a *type* for the surrounding expression to
  check against. Return the declared result type (`*Type_Info`) as normal and suppress only
  the diagnostic; do not return `Invalid`, which would cascade into unrelated errors in the
  same branch.
- It must **not** cause a `Type_Info` global to be emitted for the reflected type. Codegen
  never runs on a dead `when` branch, but sema's `type_ids` bookkeeping (the
  `type_info_of(type_of(T))` fast path records an id that `declare_type_info_globals`
  later walks) does. Skip the id registration on the suppressed path or the flag will
  fail to shrink the binary, which is its whole purpose.

Record D3 in spec §12 alongside `$rtti_enabled`, and cross-reference it from §21's `when`
prose so the exception is discoverable from the rule it excepts.

### 3.4 Tests

`examples/example_nortti_*`: `type_info_of` under `--nortti` errors; the same program
compiles under `--nortti` when the call sits in a dead `when` branch; **and that program
emits no `Type_Info` global** (assert via `--emit-ir` / later `--emit-mir`, not just exit
code — this is the third bullet of D3 and an exit-code check cannot see it);
`$rtti_enabled` folds to `true`/`false` per flag; `$rtti_enabled` in `#compile_only_if`
selects a file; `--nortti` program with no `runtime/type_info` in the graph builds; `any`
casts still work under `--nortti`; the *live* branch under `--nortti` still errors (proving
D3 didn't over-suppress). The smoke harness needs an `extra_args` entry per fixture (the
`SPECIAL_CASES` mechanism already supports this).

---

## 4. Item 2 — `@no_discard`, `@export`, `@callconv`, `@cdecl`, `@import`

`@import` was not in the original brief; it is the mechanism decided in D4 for naming a
wasm import's module, and it belongs here rather than in item 1 because it is an attribute
and this is the attribute item. Its parser/sema half lands now; its codegen half is inert
until the wasm backend exists (§6.3).

### 4.1 Parser (all five)

`ast.cpp:1075` holds the fixed known-attribute set
(`no_return | naked | always_inline | section | init`). Add `no_discard`, `export`,
`callconv`, `cdecl`, `import` (and `test` from item 5). Argument shapes:

| Attribute | Form |
|---|---|
| `@no_discard` | bare, no args |
| `@export` | bare, **or** `@export("name")` — one string argument |
| `@callconv` | `@callconv("c")` — exactly one string argument, required |
| `@cdecl` | bare, no args |
| `@import` | `@import("module")` **or** `@import("module", "name")` |

`@export`'s optional argument is the one new parse shape; the existing
`attribute_clause` already parses `@name(args)`, so this is an arity/type check in sema,
not new grammar. The grouped form `@(export, no_discard)` must keep working, and per the
existing rule a grouped member takes no argument list — so `@(export("x"), ...)` stays a
parse error and `@export("x")` must be its own clause. Document that.

**One real grammar change, needed only by `@import`.** Spec §21 currently states
attributes are legal on `fn` and on methods inside `impl` blocks, and explicitly *not* on
`ext fn`. `@import` applies to `ext fn` and nothing else, so the parser must begin
accepting an `attribute_clause` before an `ext fn` declaration. Do this as a narrow
change, not a general relaxation:

- parse an attribute clause before `ext fn` and attach it to `ExtFunctionDecl` (which gains
  a `std::vector<Attribute> attributes` field);
- in sema, reject **every** attribute on an `ext fn` except `@import`, with a message
  naming the offending attribute — so the existing prohibition stays in force for
  `@naked`/`@section`/`@init`/etc. and only `@import` is carved out;
- update grammar.md's `ext_fn_decl` production and spec §21's "not `ext fn`" sentence to
  state the single exception.

This is the only production-level change in item 2; everything else is a name added to a
known-attribute set.

### 4.2 `@no_discard`

**Sema.** Store `bool no_discard` on `FunctionSymbol` and `MethodInfo` (set in
`sema_declare.cpp` where attributes are already walked). Validate in
`sema_attributes.cpp`: no arguments; not combinable with a void-returning declaration
(`error: '@no_discard' on a function with no return value has no effect`).

**Check site.** `check_stmt`'s `ExprStmt` case (`sema_check.cpp:5911`) already exists and
already does exactly this shape of analysis for ignored error unions. Add: if the
expression is a `CallExpr` whose resolved callee carries `no_discard`, report

```
error: return value of '<name>' must be used ('@no_discard')
note: assign to '_' to discard it deliberately
```

Also fire on a *method* call and on a call inside a `defer` body. Do **not** fire when the
call is the operand of `try` (the value is consumed) or an assignment target.

**Opt-out.** `_ := f()`. `_` is already an accepted discard binding name in group
declarations and `for-in` (`sema_check.cpp:5657, 5704`); confirm the single-name
`VarDeclStmt` path accepts it too and add a fixture if not. This is a one-line
documentation addition to spec §21, not a new mechanism.

**Codegen.** None. `@no_discard` is a pure sema attribute.

### 4.3 `@export`

**Semantics.** Sets the symbol's external linkage name. Without an argument the export
name is the declaration's own unqualified name; with one, that string.

Today `symbol_name(module_path, name)` (`codegen.cpp:120`) mangles every symbol from
`(module path, name)`, with one existing escape hatch: the root module's `main` is emitted
unmangled as an entry symbol. `@export` generalizes that hatch.

**Sema validation** (`sema_attributes.cpp`):
- at most one string argument, must fold to a `[]u8` constant (reuse `@section`'s
  argument-folding path verbatim);
- the name must be a valid C identifier-ish symbol (non-empty, no NUL, no whitespace) —
  reject early with a clear message rather than producing a broken object;
- **not allowed on a generic function** (`error: '@export' is not allowed on a generic
  function; each instantiation would need a distinct export name`) — same rationale as
  `@test`'s and `ext fn`'s generic ban;
- **program-wide duplicate detection**: two declarations exporting the same name is an
  error naming both locations. This needs a new whole-program pass, sitting next to the
  existing `@init` collection in `sema_attributes.cpp` (which already walks every module
  post-load). Also collide-check against `ext fn` names, which occupy the same flat symbol
  namespace.
- legal on methods (`impl` blocks) — the export name is then required if you want it to be
  spellable, but a bare `@export` on a method is fine and uses the method's own name.

**Codegen.** In `declare_globals_and_functions` / `declare_methods` /
`declare_trait_methods`, if the decl carries `@export`, use the export name instead of
`symbol_name(...)` and force `ExternalLinkage`. `apply_function_attributes` is the natural
home for the linkage/name decision — extend it to return the chosen name rather than only
setting attributes.

On wasm: additionally emit an entry in the module's **export section** (§7.4) — that is
what "export" means there, and it is the whole reason the attribute matters for the wasm
story.

**Interaction with dead-code elimination:** an `@export`ed function must never be dropped.
The current backend emits everything reachable from the module walk, so this is free today
— but write the fixture now so a future DCE pass can't silently break it.

### 4.4 `@callconv` and `@cdecl`

**Semantics.** `@callconv("c")` makes the function use the platform C ABI instead of
Mirage's internal one. `@cdecl` is exactly `@callconv("c")`.

This is not cosmetic. The compiler deliberately has **two** ABIs today (see
`ext_function_type`'s header comment): Mirage-to-Mirage calls pass aggregates raw because
the compiler owns both sides, while `ext fn` calls go through `classify_aggregate` (SysV
eightbytes natively, clang's WebAssembly rule on wasm). `@callconv("c")` moves a *Mirage*
function onto the second path — which is exactly the machinery `ext_function_type` /
`ext_abi_param_type` already implement, so this is reuse, not new ABI code.

**Sema:**
- exactly one string argument, folded like `@section`; recognized values: `"c"` and
  `"mirage"` (the default). Anything else:
  `error: unknown calling convention '<x>'; expected "c" or "mirage"`. Reserve `"sysv"`,
  `"win64"`, `"fastcall"` as *known-but-unsupported* names with a distinct message, the
  same courtesy `asm_registers.hpp` extends to SSE registers.
- `@callconv` and `@cdecl` together → error (redundant/ambiguous).
- `@callconv("c")` + `@naked` → error (a naked function has no compiler-generated
  prologue to implement a convention with).
- not allowed on a generic function (each instantiation would need its own signature
  lowering; no reason to allow it in v1).
- Store as an enum `CallConv { Mirage, C }` on `FunctionSymbol`/`MethodInfo`.

**Sema, the part that is easy to miss:** a function pointer's type must carry its
convention. `fn() -> i32` and a `@cdecl fn() -> i32` are **not** interchangeable — taking
the address of a `@cdecl` function and calling it through a plain `fn` pointer type would
miscompile silently.

**Decision D2: taking the address of a `@callconv("c")` function is an error in v1.**

```
error: cannot take the address of a '@callconv("c")' function
note: function pointers do not carry a calling convention in v1
```

Trigger on the same conditions as `@test`'s existing call-site/address-of check
(§5.2) — the name used as a value, assigned to a function-pointer-typed binding, or passed
as a callback — so the two share one helper rather than growing two near-identical walks.
Direct calls remain legal, which is the entire use case.

The rejected alternative (a convention field on `FunctionTypeInfo`, participating in the
structural-equality rule of §15) is correct and complete but touches type identity,
generics instantiation and the LSP type printer. Record D2 in the spec as a v1 limitation
in the same style as §22's "No Bounds in v1", and note the alternative there so the
upgrade path is written down.

**Codegen.** Route the function's declaration through `ext_function_type` instead of
`function_type`, and every *call site* through the same `ext fn` argument-marshalling path
(sret slot, `byval` copies, eightbyte packing/unpacking). That call-site marshalling
already exists for `ext fn` calls; the change is choosing it based on the callee's
`CallConv` rather than on "is this an `ExtFunctionSymbol`". Factor that predicate into one
helper so the two callers cannot drift.

**Decision D1: `@export` and `@callconv` stay orthogonal.** `@export` does *not* imply
`@cdecl`. An exported symbol is also how two separately-compiled Mirage objects will find
each other once the linker story matures, and that path must keep the Mirage ABI with its
raw aggregate passing; folding the convention into `@export` would leave no way to express
it. The C-facing idiom is therefore explicit, and should appear verbatim in spec §21:

```mirage
@(export, cdecl)
pub fn mirage_add(a: i32, b: i32) -> i32 { return a + b }
```

Because this makes a one-word mistake (`@export` alone) produce a symbol that C callers
will silently mis-marshal for any struct-by-value parameter, add a **warning**: an
`@export` function whose signature passes or returns an aggregate by value, without
`@cdecl`, warns that C callers will not see the C ABI, and names `@cdecl` as the fix.
Scalar-only signatures — where the two conventions coincide — stay silent.

### 4.5 `@import` (wasm import module)

`@import("module")` / `@import("module", "name")` on an `ext fn` names the wasm import it
binds to. The second argument defaults to the declaration's own name.

**Sema:** one or two string arguments, folded like `@section`; both non-empty. Legal
**only** on `ext fn` (§4.1) — on a `fn` it is an error naming `ext fn` as the correct
placement. Store `{ import_module, import_name }` on `ExtFunctionSymbol`. Duplicate
`(module, name)` pairs across two different `ext fn` declarations are an error, for the
same reason duplicate `@export` names are.

**Non-wasm targets:** `@import` is accepted and validated but has no effect — an ELF
`ext fn` is resolved by the linker from its bare name, and there is no import-module
concept. Do not make it an error on x86-64: the whole point is that one stdlib source file
carries the annotation and compiles for every target. Emit no warning either; a
`#compile_only_if`-gated WASI backend would otherwise warn on every native build.

**Codegen (item 1, §6.3):** the import's `(module, name)` pair for the wasm import section.
Absent `@import`, the module defaults to `"env"` and the name to the declaration name —
which is what today's emscripten path effectively assumes.

Worked shape, from the `core/sys/wasi/` backend this exists to enable:

```mirage
@import("wasi_snapshot_preview1", "fd_write")
ext fn fd_write(fd: i32, iovs: *Iovec, iovs_len: i32, written: *i32) -> i32

@import("env")   // name defaults to 'console_log'
ext fn console_log(ptr: *u8, len: i32)
```

### 4.6 Tests and docs

Fixtures for: each attribute accepted bare and grouped; `@export("custom")` emits that
symbol (verify with `--emit-ir`, later `--emit-mir`, or `nm` on the object);
duplicate export names error naming both sites; `@export` on a generic errors;
`@export` with an aggregate-by-value parameter and no `@cdecl` warns; `@no_discard` result
ignored errors and `_ := f()` silences it; `@no_discard` on a void function errors;
`@cdecl` struct-by-value round-trips through the existing `tests/ext_abi_fixture` harness
(extend `ext_abi_test.py` — it already compiles a C counterpart and compares field-by-field,
which is exactly the check `@cdecl` needs); unknown `@callconv` string errors; address-of a
`@cdecl` function errors; `@import` on a `fn` errors; a non-`@import` attribute on an
`ext fn` still errors; `@import` compiles inertly on x86-64.

Docs: spec §21 gains five subsections plus the D1 idiom and the D2 v1 limitation;
grammar.md's known-attribute list gains five names and its `ext_fn_decl` production gains
the attribute clause; spec §19 gains a sentence confirming none of the five are reserved
words.

---

## 5. Item 5 — `@test`, `mirage test`, and corpus migration

`TEST_DESIGN.md` is already a complete, well-reasoned specification for the compiler side.
**Implement it as written**, with the four corrections below, then do the two things it
declares out of scope (the `core/testing` implementation, and the migration).

### 5.1 Corrections to `TEST_DESIGN.md`

1. **§1.2 / §2.4 resolution rule.** It says `--load` and forced-module resolution use
   "root-module-relative first, then `MIRAGE_PATH`/`--std`". That is the *old* rule.
   Retarget both to item 4's five-root order. Update the prose in both sections.
2. **wasm.** The document forbids `mirage test --freestanding` (fork/waitpid are POSIX)
   but says nothing about wasm, which has the same problem and more. Add the same
   driver-level refusal for any wasm target:
   `error: 'mirage test' is not supported for target '<triple>'`.
3. **§3.3 exit path.** It specifies test-mode `_start` calls `_run_tests` and takes its
   exit code. On the native path `_start` must still perform the `and $-16, %rsp`
   realignment before the first call (`codegen.cpp:3494`) — reuse `emit_start`'s prologue
   rather than writing a second one, or the harness's own `fork` will crash on a
   misaligned stack in a way that looks like a test failure.
4. **`@test` + `@export` / `@callconv`.** Add both to the conflicting-attribute list in
   §2.2. A test is invoked only through its synthesized wrapper; exporting it or changing
   its convention is meaningless and the wrapper's `fn() -> bool` assumption would break.

### 5.2 Compiler-side work (per `TEST_DESIGN.md`)

Unchanged from the document, summarized so the plan is self-contained:

- parser: `test` joins the known-attribute set;
- sema: `@test` signature restrictions (no params, no generics, return type exactly
  `error(...)`, not on a method), conflicting-attribute rules, mode-dependent body
  checking, and the call-site/address-of diagnostic (error under `build`/`run`, warning
  under `test`);
- sema: the **forced-module list** mechanism — modules loaded and fully checked but bound
  to no identifier in any symbol table, reached by codegen through a fixed
  `(module path, decl name)` mangled handle. This is the genuinely new mechanism and the
  one worth reviewing hardest; its "no accessible alias" guarantee is a property that a
  shortcut implementation would silently lose;
- codegen: one `fn() -> bool` wrapper per discovered `@test`, the `Test_Info` constant, and
  a test-mode `_start`;
- driver: the `test` action, repeatable `--load <path>`, the `--freestanding` and wasm
  refusals, and the early `core/testing` contract validation.

### 5.3 `core/testing` (out of scope in the design doc, in scope here)

Lives in the stdlib repo (`/mnt/projects/Projects/Mirage/Mirage`), implementing the fixed
contract in `TEST_DESIGN.md` §7: `Test_Function`, `Test_Case`, `Test_Info`, `_run_tests`.
Per-test `fork`/`waitpid` isolation, three outcome states (ok / failed / crashed-with-
signal-name), parent-side timing via `clock_gettime`, a module/function/result/elapsed
table in `Test_Info` order, and a fixed non-zero exit sentinel on any failure.

It needs `fork`, `waitpid`, `exit`, `clock_gettime` as `ext fn` declarations — no compiler
support beyond what `ext fn` already provides. Note it will be the first stdlib module
whose *correctness* nothing else can test; keep it small and verify it with a handful of
deliberately-failing and deliberately-crashing fixtures.

### 5.4 Migrating the corpus — the honest scope

This is where the work is, and where `TEST_DESIGN.md` does not help, because **`@test`
cannot express a compile-failure test.** Of 259 fixtures, roughly 110 exist precisely to
assert that a program *does not compile* and produces an exact diagnostic. Those cannot be
Mirage functions in a Mirage program.

Split the corpus three ways:

**(a) Positive runtime fixtures (~140, the `run` entries).** Migrate to `@test` functions.
Most already encode their assertion in the process exit code; converting means turning
`return 3` into `return .Failed(...)`/`return .Ok`. Group related fixtures into a few
module-per-feature test modules (`tests/mir/errors/`, `tests/mir/generics/`,
`tests/mir/traits/`, …) rather than 140 single-function modules — the harness reports
module + function, so grouping costs nothing in legibility and saves 140 directory loads
per run. Delete the corresponding `examples_expected.json` entries as each group lands.

**(b) Negative compile-fail fixtures (~110, the `build` entries with a `diag`).**

**Decision D7: these stay as they are** — `examples/example_*/main.mir` pinned by
`tests/examples_expected.json`, gated by `tests/examples_smoke_test.py`, re-blessed with
`--update-baseline` as part of whatever fix changed the behaviour. No new compiler feature.

The consequence to plan around, stated plainly: **`examples_smoke_test.py` and its 37 KB
JSON baseline survive item 5, survive item 1, and survive into the self-hosted era.** They
are Python, so they will still run against a self-hosted `mirage` binary unchanged — the
harness only shells out to the CLI and compares exit codes and diagnostic text. That is
genuinely fine and is the reason this is a defensible choice; it just means the corpus
never becomes fully self-hosted, and the existing baseline-review discipline (never re-bless
in a standalone commit; review the JSON hunk like any other diff) stays load-bearing rather
than becoming vestigial.

Two things worth doing anyway, both cheap and neither requiring a compiler change:

- As positive fixtures migrate out to `@test` (class (a)), their `examples_expected.json`
  entries must be deleted in the same commit. The harness fails on an unknown directory but
  is silent about a baseline entry with no directory, so a stale entry would rot unnoticed.
  Add that check to `examples_smoke_test.py` — ~10 lines, and it is what keeps the shrinking
  baseline honest.
- Keep `--strict` (treat a changed diagnostic message as a failure, not a warning) on in CI.
  With the JSON baseline retained, that flag is the only thing standing between a reworded
  diagnostic and a silently-accepted regression.

**(c) Driver / toolchain fixtures.** `cli_test.py`, `editor_grammar_test.py`, the
`--print-link-directives` fixture, the wasm cross-compile check, and all six LSP test
scripts test the *tooling*, not the language, and stay in Python. The LSP ones die with the
LSP rewrite regardless.

The five C++ unit tests (`lexer_asi`, `lexer_robustness`, `diagnostic_engine`,
`ast_parser_progress`, `type_printer`) are testing C++ internals that the self-hosted
compiler will replace wholesale. Leave them; they retire with `Mirage-Cpp`.

**Sequencing within item 5:** compiler support → `core/testing` → migrate class (a) group
by group, deleting baseline entries as you go. Class (b) is not migrated. Each group is
independently committable and independently green. Expect `examples_expected.json` to end
up at roughly 110–120 entries, all negative, all `build`.

---

## 6. Item 1 — Mirage IR and native object generation

The large item. Structure it as five separable layers so that each is testable before the
next exists.

```
sema::Program
    │
    ├── mirgen/        (replaces codegen.cpp's LLVM calls, target-independent)
    ▼
  MIR                  scalar 3-address IR, virtual registers, explicit stack slots
    │
    ├── mir/passes/    verifier, printer, promote-allocas, peephole
    ▼
  MIR (legalized)
    │
    ├── x86_64/        legalize → isel → regalloc → frame → encode
    │       ▼
    │     ELF64 writer  →  .o
    │
    └── wasm/          legalize → structurize → encode
            ▼
          wasm module writer  →  .wasm
```

### 6.1 MIR design

**Design constraint that should govern every choice here: this code gets rewritten in
Mirage.** Keep it data-oriented and index-based — flat `std::vector` arenas with `u32`
handles, no inheritance, no `std::variant` of `unique_ptr`, no templates. The existing
`sema::Program` (parallel vectors indexed by `struct_index`, `union_index`, …) is the
model; MIR should look like more of the same. A `std::unique_ptr`-heavy IR would have to be
redesigned during the self-hosting port instead of transliterated.

```cpp
namespace mir {
    enum class Ty : uint8_t { I1, I8, I16, I32, I64, F32, F64, Ptr };  // scalars only

    struct Inst {
        Op       op;
        Ty       type;      // type of the defined value; Void ops define nothing
        uint32_t a, b, c;   // operand value ids / immediates / block ids, per-op meaning
    };

    struct Block { uint32_t first_inst, inst_count; std::vector<uint32_t> params; };

    struct Function {
        uint32_t             symbol;       // index into Module::symbols
        std::vector<Ty>      param_types;
        std::vector<Ty>      result_types;
        std::vector<Inst>    insts;
        std::vector<Block>   blocks;
        std::vector<Slot>    slots;        // stack slots: {size, align, is_addr_taken}
        CallConv             conv;
    };

    struct Module {
        std::vector<Function>  functions;
        std::vector<Global>    globals;    // {symbol, section, size, align, init bytes, relocs}
        std::vector<Symbol>    symbols;    // {name, linkage, kind}
        std::vector<Signature> signatures; // deduplicated; wasm needs type indices
    };
}
```

**Three decisions to make deliberately:**

**(1) Aggregates are memory, values are scalars.** A MIR value is always one machine
scalar. This is what lets ISel, register allocation and the wasm encoder all stay simple —
and it costs nothing in expressiveness because sema already computed every offset.

But `codegen.cpp` uses `insertvalue`/`extractvalue` on first-class aggregates in 67 places
(struct literals, slices, trait handles, `any`, error unions, multi-return). Porting those
by hand is the largest single source of risk in this item. Mitigate with a **value wrapper
in the builder, not in MIR**:

```cpp
struct Val {                    // what mirgen manipulates, mirroring llvm::Value*
    enum { Scalar, Aggregate } kind;
    uint32_t id;                // vreg id, or the slot's base pointer vreg
    uint32_t type_size;         // aggregates only
};
```

`build_insert_value(agg, byte_offset, scalar)` on an Aggregate whose slot was created by
this builder *and has not yet been read* mutates in place (a plain `store`); otherwise it
copies the slot first and then stores. `build_extract_value(agg, offset, ty)` is a `load`.
The in-place fast path covers the entire
`null → insert → insert → insert` construction pattern that produces nearly all of the 67
sites, so the copy path should almost never fire in practice — and when it does, it is
still correct.

**(2) Control flow: basic blocks with block parameters, not phi.** Block params are
simpler to allocate registers for and simpler to lower to wasm than phi nodes, and there
are only *three* `CreatePHI` sites to convert. Everything else in `codegen.cpp` is
already block-and-branch.

**(3) The builder mirrors `llvm::IRBuilder`'s method names.** `build_load`, `build_store`,
`build_add`, `build_icmp_ne`, `build_cond_br`, … one per LLVM op actually used. This makes
the `codegen.cpp` port a mostly-mechanical, reviewable rename of 6,900 lines instead of a
rewrite. The three places where it *cannot* be mechanical — GEP, aggregates, and inline asm
— are exactly the three places to concentrate review.

**GEP replacement.** LLVM's typed `getelementptr` disappears. Because layout comes from
sema, every GEP in the file is one of two shapes:

- `CreateStructGEP` / `CreateConstInBoundsGEP1_64` → `build_ptr_add_const(base, offset)`
- `CreateInBoundsGEP` with a dynamic index → `build_ptr_add(base, mul(index, elem_size))`

Both become plain pointer arithmetic. This is a genuine simplification, not a workaround.

**Supporting infrastructure to build first, before any backend:**

- **`mir::print` / `--emit-mir`** — a textual form, stable and diffable. This is the
  primary debugging surface for the entire item, and the replacement for `--emit-ir`
  (which 8 fixtures pin — re-baseline those to `--emit-mir` when the flag lands).
- **`mir::verify`** — the analogue of `llvm::verifyModule`, which `run()` currently calls
  and which has been catching real bugs (the `create_entry_alloca` comment documents one).
  Check: every value defined before use, every block terminated exactly once, branch
  targets in range, operand types match the op, block-param arity matches every
  predecessor's branch. Run it under a debug build on every compile.
- **`promote_slots`** — a mem2reg-lite: a stack slot whose address never escapes (never
  passed to a call, never stored, never `ptrtoint`'d) and whose accesses are all
  full-width loads/stores of one scalar type becomes a vreg. Because the front end puts
  *every* local in a slot, this pass is what recovers most of the performance LLVM's -O0
  pipeline was giving for free. Target-independent, benefits both backends, ~300 lines.
  Build it early; it also shrinks the input the register allocator has to chew on.
- **A peephole pass** — constant folding, `add x, 0`, redundant `zext`/`trunc` pairs,
  branch-to-branch. ~200 lines, meaningful payoff.

### 6.2 x86-64 backend

**Legalize.** `i1` is stored as `i8` (loads zero-extend, stores truncate); MIR `select`
expands to a diamond or `cmov`; `memcpy`/`memset` of a constant size below a threshold
expand to load/store sequences, otherwise call libc (hosted) or an emitted intrinsic
(freestanding).

**Instruction selection.** Macro expansion, one MIR op at a time — no tree matching, no
BURS. Each MIR op maps to one to three `MachineInst`s. Enough addressing-mode folding to
turn `ptr_add_const` + `load` into a single `mov reg, [base+disp]`, which is the one
peephole that matters for output size. `MachineInst` carries virtual registers plus
*fixed-register constraints* where the ISA demands them.

**Register allocation.** Linear scan over live intervals in reverse-post-order block
layout. The hard requirements, all of which the current program shapes actually exercise:

- 14 allocatable GPRs (`rsp`/`rbp` reserved) and 16 XMMs, allocated from separate classes;
- caller-saved / callee-saved split, with callee-saved registers saved in the prologue only
  when used;
- **fixed-register constraints**: `div`/`idiv` clobber and define `rdx:rax`, variable
  shifts need the count in `cl`, SysV argument registers `rdi rsi rdx rcx r8 r9` +
  `xmm0-7`, returns in `rax`/`rdx`/`xmm0`/`xmm1`;
- spilling to frame slots, and interval splitting around calls.

This is the component most likely to harbour a subtle miscompile. Two defences: (i) the
MIR verifier plus a *machine-level* verifier that re-checks live-range interference after
allocation; (ii) a `--regalloc=trivial` mode that spills every value to a frame slot and
reloads around every use. Trivial mode is slow and enormous but almost impossible to get
wrong — when a program misbehaves, if it also misbehaves under `--regalloc=trivial` the bug
is not in the allocator. Build trivial mode *first*; it lets you validate ISel, the frame
layout, the encoder and the ELF writer end-to-end before writing a single line of linear
scan.

**Frame layout and prologue/epilogue.** Slot assignment with alignment; `rsp` maintained so
that it is 16-byte aligned *at every `call`* (SysV); callee-saved save/restore; and the
`_start` special case, which enters with a 16-byte-aligned `rsp` rather than the
`rsp % 16 == 8` that a normal function sees. The existing `and $-16, %rsp` asm blob becomes
a frame-layout flag on the synthesized `_start` rather than an inline-asm call.

**Encoder.** REX / ModRM / SIB / displacement / immediate encoding for the selected opcode
subset (roughly: `mov movzx movsx lea add sub imul idiv div and or xor not neg shl shr sar
cmp test set<cc> j<cc> jmp call ret push pop cdq cqo` plus the SSE scalar set
`movss movsd addss addsd subss subsd mulss mulsd divss divsd ucomiss ucomisd cvtsi2ss
cvtsi2sd cvtss2sd cvtsd2ss cvttss2si cvttsd2si xorps xorpd`). Bounded and mechanical, but
the place where a wrong bit produces a program that runs and gives the wrong answer —
so: **table-driven encoding plus a differential test that assembles every table entry with
both this encoder and `as`, and byte-compares.** That harness pays for itself within a day.

**Inline asm.** Without LLVM there is no integrated assembler, so `asm { … }` blocks must be
encoded by the same encoder. Fortunately the accepted language is already tiny and
enumerated: the 30 Tier-1 mnemonics (`sema_check.cpp:5338`), the 64 GPRs in
`asm_registers.hpp`, and register/immediate/variable/simple-memory operands. SSE, x87,
segment and control registers are already rejected as "not supported in v1".

This also *simplifies* the operand model: today operands are LLVM constraint strings
(`"={rax},{rax},{rdi},…"`) and LLVM does the allocation. With our own allocator, an asm
block becomes a `MachineInst` with pre-coloured operands and an explicit clobber set —
which the sema layer already computes (`asm_implicit_clobbers`, `asm_tier1_directions`).
Roughly the same amount of code, less indirection.

The three compiler-internal asm blobs (`syscall` for freestanding exit and write, and
`_start`'s stack realign) stop being asm entirely: emit them as MIR/machine instructions
directly.

**ELF64 writer.** Sections `.text`, `.rodata`, `.data`, `.bss`, plus arbitrary named
sections for `@section`. Symbol table with local/global binding, `.rela.text` /
`.rela.data` relocations — `R_X86_64_PC32` for calls and RIP-relative data references,
`R_X86_64_64` for absolute pointers in initialized data (vtables, `Type_Info` tables,
string-literal slice constants all need this), `R_X86_64_PLT32` for calls to `ext fn`
symbols. Section alignment and `.symtab`/`.strtab`/`.shstrtab` layout. ~800 lines, and
verifiable with `readelf`/`objdump` plus the existing linker step, which stays unchanged.

### 6.3 wasm backend

Structurally easier than x86-64 in two ways and harder in one.

**Easier — no register allocation.** wasm functions have unlimited typed locals. Every MIR
vreg becomes a wasm local; the value stack is used only within an expression. The entire
regalloc/spill/frame-pointer layer disappears.

**Easier — no instruction encoding subtleties.** wasm opcodes are one or two bytes with
LEB128 immediates.

**Harder — structured control flow.** wasm has `block`/`loop`/`if`/`br`/`br_table` and *no*
`goto`. MIR has an arbitrary CFG. Two strategies:

- **Stage 1 (correctness): the dispatch loop.** Wrap the whole function in
  `loop { block { … block { br_table $state } … } }`, one arm per basic block, with a
  `$state` local holding the next block index; every MIR branch assigns `$state` and
  `br` to the loop header. Works for *any* CFG including irreducible ones, is ~400 lines,
  and is trivially checkable. Slower and larger than structured output.
- **Stage 2 (quality): Relooper.** Recover natural `if`/`loop`/`block` structure for the
  reducible regions (which is essentially all of them, since Mirage has no `goto`),
  falling back to dispatch for anything that resists. ~800 more lines.

Do stage 1 first and ship it; do stage 2 as an optimization once the corpus is green. Do
not start with Relooper — a CFG-structuring bug and a codegen bug look identical from the
outside, and you want only one new variable at a time.

**Shadow stack.** wasm's operand stack is not addressable, so any MIR slot that survives
`promote_slots` (i.e. whose address is taken) lives in linear memory. Reserve a mutable
global `__stack_pointer` (i32) initialized just below `__heap_base`; each function's
prologue subtracts its frame size and the epilogue restores. Standard, well-trodden.

**Function pointers.** A wasm function reference is a **table index**, not an address.
`call_indirect` takes a *type index*, so `mir::Module::signatures` must be deduplicated and
each indirect call must name its signature. Concretely:

- every address-taken function gets an entry in the module's `funcref` table;
- a Mirage function-pointer value is that i32 index;
- **trait vtables** — currently constant arrays of function pointers — become constant
  arrays of i32 table indices, and `emit_trait_handle_dispatch`'s
  "load a pointer from the vtable, call it" becomes "load an i32, `call_indirect`".

**Consequence to document:** on wasm a function pointer is not in the same address space as
data, so casting between a function pointer and `anyptr` cannot work. Check whether the
language currently permits it; if so, make it a target-conditional sema error rather than a
silent miscompile. This is a real semantic difference between the two targets and belongs
in the spec.

**Data and memory.** `.rodata`/`.data` become one active data segment at a fixed offset;
`.bss` needs no segment at all (linear memory is zero-initialized) — just reserve the
address range. `__heap_base` is placed after the data. Declare the memory with an initial
page count derived from data size + a configurable stack size.

**Imports and exports.** Every `ext fn` becomes an import, its `(module, name)` pair taken
from `@import` (§4.5, decision D4) and defaulting to `("env", <decl name>)`. Every
`@export` function goes in the export section. The entry point is exported as `_start`
(WASI convention) or `main`, per flavour.

**Output — two shapes, per decision D5.** For `wasm32-unknown-unknown` and `wasm32-wasi`,
emit the **final `.wasm` module directly**: Mirage already compiles whole-program into a
single object, so there is nothing to link, and this is strictly less work than the ELF
path. For `wasm32-unknown-emscripten`, emit a **relocatable wasm object** with `linking`
and `reloc.*` sections for `emcc`/`wasm-ld` — see §7.4 for the full delta and the reason to
build it *second*, over the same encoder, once the standalone path is green.

### 6.4 Driver changes

- `--backend=llvm|native` (default `llvm`, flipping to `native` at the end of the item).
  Keeps `codegen.cpp` and its `emit_object_file` path intact for differential testing.
- `--emit-mir` replaces `--emit-ir` on the native path; `--emit-ir` remains meaningful only
  under `--backend=llvm`.
- `--emit-asm` (native x86-64 only): textual assembly from the machine IR. Not required,
  but the fastest way to eyeball a suspected encoder bug.
- `--regalloc=trivial|linear` (debug).
- Target selection stops going through `llvm::Triple`. Replace with a small internal enum:
  ```
  Target { X86_64_Linux, Wasm32_Unknown, Wasm32_Wasi, Wasm32_Emscripten }
  ```
  All four are permanent (D5 keeps emscripten). Note `Wasm32_Unknown` is the *target*
  spelled `wasm32-unknown-unknown`; it is a different axis from the existing
  `--freestanding` flag (which means "no Mirage stdlib, hand-written `_start`"). Keep the
  two spellings distinct in help text and diagnostics — conflating them is the most likely
  source of a confusing error message in this area.
  and a parser for the `--target=` spellings currently accepted. `default_target_os` /
  `default_target_arch` (which feed `$option build/target_os` / `build/target_arch`, and
  therefore every `#compile_only_if` in the stdlib) switch on that enum. **Keep the
  `$option` value strings byte-identical** (`"Linux"`, `"Wasm32"`, `"X86_64"`, …) or every
  platform-split stdlib file silently changes inclusion.
- `sema::Options::pointer_size` continues to come from the selected target, unchanged.
- Once `--backend=native` is the default and `codegen.cpp` is deleted, `find_package(LLVM)`
  and every `${LLVM_LIBS}` reference come out of `CMakeLists.txt`. **Build time and
  dependency footprint collapse**, which is a real secondary benefit worth measuring and
  recording.

### 6.5 Validation strategy

This is the part that determines whether the item succeeds.

1. **Differential compilation.** For every fixture, build under `--backend=llvm` and
   `--backend=native` and compare exit code and stdout. `examples_smoke_test.py` already
   walks the corpus and can take a `--backend` argument; extend it to run both and diff.
   This is the single highest-value harness in the item — write it before the backend.
2. **`mirage test` (item 5) on both backends.** The migrated corpus becomes a real,
   assertion-carrying test suite rather than an exit-code comparison.
3. **Encoder differential.** Every table entry assembled by both this encoder and `as`,
   byte-compared. Catches the class of bug that produces working-but-wrong code.
4. **Machine-level verifier** after register allocation, checking interference.
5. **`mirage303`** (90 files, ~2s, links raylib) as the integration smoke test —
   `--backend=native` must produce a working game client. It exercises `ext fn` struct
   ABI, function pointers, traits and the `#link` path all at once, which no fixture does.
6. **`--regalloc=trivial` bisection** as the standing triage tool for "the native build
   misbehaves".

### 6.6 Order of work within item 1

Each step is independently verifiable; do not start the next until the current one is
green.

1. MIR data structures, builder, printer (`--emit-mir`), verifier. No backend.
2. Port `codegen.cpp` → `mirgen.cpp` against the builder. Validate by *reading* MIR for
   the corpus — no object is produced yet. This is the mechanical-but-large step.
3. `promote_slots` + peephole. Validate: MIR still verifies, output visibly shrinks.
4. x86-64: legalize → ISel → **trivial** regalloc → frame → encoder → ELF. First goal:
   `examples/start` runs. Then the whole corpus under differential test.
5. x86-64 inline asm through the encoder; freestanding syscalls and `_start` as machine
   instructions. Validate against `core/sys/linux/syscalls.mir` and the freestanding
   fixtures.
6. Linear-scan register allocator + machine verifier. Differential-test against trivial.
7. wasm, standalone: locals, shadow stack, data/memory, imports/exports, dispatch-loop
   control flow, `call_indirect` + table for function pointers and vtables. Target
   `wasm32-unknown-unknown` first (no libc dependency at all, so the backend is the only
   variable), then `wasm32-wasi` once `core/sys/wasi/` exists.
8. wasm, relocatable (D5): `linking` + `reloc.*` sections, `R_WASM_*` relocations, deferred
   memory layout. Reuses step 7's encoder and structurizer wholesale. Validate by building
   an existing wasm fixture through `emcc` and diffing behaviour against the current
   LLVM-backend emscripten output.
9. Relooper.
10. Flip the default to `--backend=native`; run everything; delete `codegen.cpp` and the
    LLVM dependency. `emcc` stays in the matrix as a linker for step 8.

### 6.7 Rough size

New C++, order of magnitude: MIR core + builder + printer + verifier ~1.5k; passes ~0.5k;
x86 legalize/ISel ~1.5k; regalloc ~1.2k; frame + encoder ~2.5k; ELF ~0.8k; wasm standalone
~2k; wasm relocatable ~0.9k (D5); driver rework ~0.4k. Call it **11–13k lines of new
code**, plus a near-total rewrite of `codegen.cpp`'s 6.9k lines into `mirgen.cpp`.
Comparable in scale to the existing sema layer. Steps 1–6 are the bulk; step 7 is smaller
than it looks because there is no allocator and no relocation format, and step 8 is
precisely the part where that second clause stops being true.

---

## 7. wasm without emscripten — the design answer

**Yes, and it is one of the cleaner parts of item 1.** The dependency on emscripten today
is not fundamental; it exists because LLVM emits a *relocatable wasm object* that something
else must turn into a module, and because the wasm stdlib was written against emscripten's
libc.

### 7.1 What emscripten currently supplies

1. **Linking.** LLVM produces a wasm object with a linking section; `wasm-ld` (wrapped by
   `emcc`) resolves it into a module.
2. **libc.** A musl port. `core/sys/wasm/` binds to it, and — per the pinned notes —
   emscripten's errno numbers are WASI's, not Linux's.
3. **Runtime setup.** Memory declaration, `__stack_pointer`, data segment placement,
   `__heap_base`, the JS glue that instantiates the module and calls `main`.
4. **Ports.** SDL/GL/filesystem shims, and the raylib web build.

### 7.2 What replaces each

| Emscripten role | Replacement | Cost |
|---|---|---|
| Linking | Emit the final `.wasm` directly. Mirage already compiles whole-program into one object, so there is nothing to link. | **Free** — strictly less work than emitting a relocatable object. |
| Memory / `__stack_pointer` / segments / `__heap_base` | We own the module; declare them ourselves. | ~100 lines, §6.3. |
| Entry glue | Export `_start` (WASI) or `main`; drop `emit_wasm_main`'s C-`main` shape. Optionally emit a ~30-line `.html`/`.js` loader for browser use. | Small. |
| libc | Two flavours, below. | The real work, and it is stdlib work, not compiler work. |
| Ports (raylib/SDL/GL) | **Not replaced.** | See §7.4. |

### 7.3 The two standalone flavours to offer

**`wasm32-freestanding`** (`wasm32-unknown-unknown`). No libc, no host assumptions. All
host interaction is a user-declared `ext fn`, which becomes a wasm import the embedder
supplies. This is the *purest* target and needs essentially zero stdlib work to be
"supported" — it just supports very little. Ideal for embedding Mirage in a JS/host
application, and the right first wasm target to bring up because it removes every variable
except the backend itself. Note that `--freestanding` (the existing flag, meaning "no
Mirage stdlib, hand-written `_start`") and `wasm32-freestanding` are different axes —
pick distinct spellings to avoid confusion, e.g. keep `--freestanding` and name the target
`wasm32-unknown-unknown`.

**`wasm32-wasi`.** Imports from `wasi_snapshot_preview1`. The stdlib gains a
`core/sys/wasi/` backend beside the existing `core/sys/linux/` and `core/sys/wasm/`,
implemented with the same `#compile_only_if(target_os == .Wasi)` + `_target.mir` pattern
already established. The surface needed to match what `core/sys/linux` provides:

```
fd_write  fd_read  fd_close  fd_seek  path_open  fd_prestat_get  fd_prestat_dir_name
clock_time_get  random_get  proc_exit  environ_get  environ_sizes_get  args_get  args_sizes_get
sock_accept  sock_recv  sock_send  sock_shutdown        (preview1 sockets — limited)
```

That is comparable in size to `core/sys/linux/syscalls.mir` and mechanically simpler (no
inline asm — every WASI call is an ordinary import). Errno numbers are already WASI's in
the existing wasm path, so `core/os`'s error mapping largely carries over. Runs unmodified
on wasmtime, wasmer, node, and in a browser behind a small JS WASI shim. **Recommend this
as the default wasm target** once it exists: it is standalone, it is a real specification
rather than a vendor runtime, and it gives files/clock/args/env, which is what the corpus
and the test harness actually need.

Networking is where WASI preview1 is genuinely thin; `core/net` on WASI will be partial.
Say so in the docs rather than shipping something that compiles and fails at runtime.

### 7.4 Emscripten is retained (decision D5)

**Prebuilt C library ports are the one thing nothing here reproduces.** raylib's web build
depends on emscripten's GL/SDL/HTML5 glue and its asyncify main-loop transform. Nothing in
this plan replaces that, and nothing should try — so `mirage303`-on-web needs `emcc` for as
long as it needs raylib.

D5 keeps `--target=wasm32-unknown-emscripten` working after `codegen.cpp` is deleted. This
does not compromise the standalone goal — the compiler still emits everything itself and
`emcc` is only a linker, which the brief explicitly allows to stay external — but it is
**not free**, and the cost lands entirely in item 1:

> The native wasm backend must emit **two** output shapes: a finished `.wasm` module for
> the two standalone flavours, and a **relocatable wasm object** for emscripten.

The relocatable path additionally needs:

- the `linking` custom section (symbol table: function/data/global symbols, their flags,
  and segment info);
- `reloc.CODE` / `reloc.DATA` custom sections with the `R_WASM_*` relocation kinds actually
  used — `FUNCTION_INDEX_LEB`, `TABLE_INDEX_SLEB`, `MEMORY_ADDR_LEB`, `MEMORY_ADDR_SLEB`,
  `MEMORY_ADDR_I32`, `TYPE_INDEX_LEB`, `GLOBAL_INDEX_LEB`;
- unresolved-symbol placeholders where the standalone path resolves indices directly, which
  means the encoder cannot assume it knows a function's final index at emit time — every
  index that could be an import must go through a relocation slot;
- deferring memory layout, `__stack_pointer`, `__heap_base` and the data-segment offset to
  `wasm-ld` rather than choosing them ourselves (§6.3's "we own the module" simplification
  is exactly what does *not* apply here).

Practically: **build the standalone `.wasm` writer first and get the corpus green on it,
then add the relocatable writer as a second backend over the same encoder.** The
instruction encoding, control-flow structuring, shadow stack and `call_indirect` work are
shared; only module assembly and index resolution differ. Attempting both at once means
debugging CFG structuring and relocation bugs through the same symptom.

Estimated additional cost: **~800–1,000 lines** on top of the standalone wasm writer, and
it keeps `emcc` in the test matrix (the emsdk at `/mnt/projects/Projects/Mirage/emsdk`
stays a build dependency for the wasm fixtures).

Retire this path only if raylib-on-web stops mattering; if that happens, deleting it is a
clean subtraction, since nothing else depends on the relocatable format.

### 7.5 Also lost, and worth stating

`setjmp`/`longjmp`, C++ exceptions, and dynamic linking — none of which Mirage has. Threads
(wasm shared memory / atomics) — not currently supported on any target. So the practical
loss list is exactly §7.4.

---

## 8. Risk register

| Risk | Where | Mitigation |
|---|---|---|
| Register-allocation miscompile — code that runs and is wrong | §6.2 | `--regalloc=trivial` built first and kept; post-allocation interference verifier; differential corpus run |
| Instruction-encoding bug — same failure mode | §6.2 | Table-driven encoder differential-tested against `as`, every entry |
| The 6.9k-line `codegen.cpp` port | §6.1 | Builder API mirrors `IRBuilder` method-for-method; only GEP / aggregates / inline asm are non-mechanical; MIR verifier on every compile |
| Aggregate-value semantics change during the port | §6.1 | `Val` wrapper preserves the value-semantics call shape; in-place fast path; copy fallback is always correct |
| wasm CFG structuring | §6.3 | Dispatch loop first (any CFG, ~400 lines); Relooper strictly as a later optimization |
| wasm relocation bugs debugged simultaneously with CFG bugs (D5) | §7.4 | Standalone `.wasm` writer green on the whole corpus *before* the relocatable writer starts; both share one encoder |
| Silent `$option` string drift when `llvm::Triple` is replaced | §6.4 | Pin `build/target_os` / `build/target_arch` strings in a test; the whole stdlib platform split depends on them |
| `@callconv` function pointers miscompiling | §4.4 | D2 forbids taking the address of a `@callconv("c")` function |
| `@export` without `@cdecl` silently mis-marshalling structs to C (D1) | §4.4 | Warning on any `@export` function with an aggregate-by-value parameter or return and no `@cdecl` |
| Stale `examples_expected.json` entries as positive fixtures migrate out (D7) | §5.4 | Harness gains an orphaned-baseline-entry check; `--strict` stays on in CI |
| Corpus never becomes fully self-hosted (D7) | §5.4 | Accepted: the Python harness runs unchanged against a self-hosted binary, since it only shells out to the CLI |
| `when` type-checks both branches, so `$rtti_enabled` guards don't suppress the RTTI error | §3.3 | D3: dead-`when`-branch exemption, deliberately narrow — and it must also skip `type_ids` registration, or `--nortti` fails to shrink the binary |
| Attributes on `ext fn` were previously a hard prohibition (D4) | §4.1 | `@import` is carved out one attribute at a time; every other attribute on an `ext fn` still errors by name |
| LSP breakage | all | `mirage_core`'s public headers are the contract; only `module_resolver.hpp` (item 4) and `ast.hpp`/`sema.hpp` field additions (items 2/3/5) touch it. Additive changes only; run the LSP test scripts after each item. `codegen.cpp` is not linked into the LSP at all, so item 1 cannot affect it. |

## 9. Settled decisions

All eight open questions are decided. Recorded here as the authoritative list; each is
also written into the section that implements it.

| # | Decision | Where |
|---|----------|-------|
| **D1** | `@export` and `@callconv` stay **orthogonal** — `@export` does not imply `@cdecl`. The C-facing idiom is `@(export, cdecl)`. A warning covers the easy mistake (aggregate-by-value in an `@export` signature with no `@cdecl`). | §4.4 |
| **D2** | Taking the **address of a `@callconv("c")` function is an error** in v1; function-pointer types carry no convention. Direct calls remain legal. Recorded in the spec as a v1 limitation with the upgrade path noted. | §4.4 |
| **D3** | Under `--nortti`, the `type_info_of` error **does not fire in a statically-dead `when` branch**. Deliberately narrow: this one diagnostic, this one flag, and it must also skip `type_ids` registration. | §3.3 |
| **D4** | wasm import modules are named by a new **`@import("module", "name")` attribute** on `ext fn`, defaulting to `("env", <decl name>)`. Requires accepting an attribute clause before `ext fn` — a real grammar change, carved out for `@import` only. | §4.1, §4.5, §6.3 |
| **D5** | **Emscripten is retained** as a relocatable-object target after `codegen.cpp` is deleted, preserving the raylib web build. Adds the `linking`/`reloc.*` sections and `R_WASM_*` relocations to item 1 — ~900 lines, built *after* the standalone `.wasm` writer is green. | §6.3, §6.6 step 8, §7.4 |
| **D6** | Search root 4 probes **both `<exe_dir>` and `<exe_dir>/../lib/mirage`**, in that order, so a `bin/` + `lib/mirage/` install needs no environment variable. | §2.3 |
| **D7** | Negative compile-fail fixtures **keep `examples_expected.json`**; no `--expect` comment mode. Only the ~140 positive fixtures migrate to `@test`. The Python harness survives into the self-hosted era, which is fine — it only shells out to the CLI. Gains an orphaned-entry check; `--strict` stays on in CI. | §5.4 |
| **D8** | **`MIRAGE_PATH` is dropped outright** — not consulted, no fallback. One note appended to the *failure* diagnostic when it is set and `MIRAGE_MODULES_ROOT` is not, purely for discoverability. | §2.4 |

### Net effect on scope

Six decisions matched the recommendation and change nothing. Two moved scope:

- **D5 grew item 1** by roughly 900 lines and one new output format, and keeps `emcc` and
  the emsdk in the build matrix indefinitely. Sequencing absorbs most of the risk: the
  relocatable writer is step 8, over an encoder already proven by step 7.
- **D7 shrank item 5** by ~200 lines of driver work and one design, at the cost of the
  corpus never becoming fully self-hosted. The migration is now purely additive — write
  `@test` modules, delete baseline entries — with no compiler feature gating it.

**D8 is the only decision with a compatibility cost**, and it is deliberate: any shell or
CI job exporting `MIRAGE_PATH` loses stdlib resolution the moment item 4 lands. The
failure is a clear diagnostic with a named fix, not a mystery, but it is a hard break —
worth a line in the commit message and the README rather than discovering it from a red
build.
