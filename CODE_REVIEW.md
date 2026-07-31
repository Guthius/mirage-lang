# Mirage — Code Review Action Plan

**Date:** 2026-07-31 · **Scope:** everything under `src/` (compiler + LSP), ~34k lines, all files read in full.
**Method:** five parallel deep-review passes (frontend, sema core, `sema_check.cpp`, codegen + driver, LSP). Every High finding in the sema-core section was **reproduced against the built compiler**; other findings were verified by reading the complete relevant code paths, not just the flagged line.

Finding IDs are new for this round (`FE-` frontend, `SEMA-` sema core, `CHECK-` sema_check.cpp, `CG-` codegen/driver, `LSP-`). They do not continue the ID series from the retired `DEFERRED.md`.

---

## How to attack this

| Phase | What | Items |
|---|---|---|
| **0 — Crashes & miscompiles** | Wrong code or a dead compiler on plausible input | FE-1, FE-2, SEMA-1, SEMA-2, SEMA-3, SEMA-4, SEMA-5, CHECK-1, CG-1, CG-2 (+CHECK-7), LSP-2, LSP-3, LSP-4 |
| **1 — User-visible correctness** | Wrong answers, missed errors, confusing diagnostics | FE-3…FE-8, FE-11, SEMA-6…SEMA-9, CHECK-2…CHECK-6, CHECK-8, CHECK-9, CG-3…CG-7, LSP-1, LSP-5…LSP-7, LSP-12, LSP-13 |
| **2 — Duplication & structure** | Factor before the copies drift further | FE-9, SEMA-10…SEMA-14, CHECK-10, CHECK-11, CG-8…CG-10, LSP-8…LSP-11 |
| **3 — Polish** | All Low items | FE-12…FE-22, SEMA-15…SEMA-18, CHECK-12…CHECK-18, CG-11…CG-18, LSP-14…LSP-20 |

Every Phase-0 item should land with a regression test (repro snippets are inline below).

### Cross-cutting themes

1. **"Couldn't fold" is indistinguishable from "folded to false."** SEMA-1, SEMA-2 and SEMA-10 share one root: `is_constant_expr` accepts shapes `evaluate_const_value` can't fold, and every `when` consumer treats nullopt as `false` silently. Fixing the plumbing (hard-error when a passed-`is_constant_expr` expression fails to fold) fixes the whole family and prevents recurrence.
2. **Sema-permits / codegen-can't pairs.** CHECK-1 (arithmetic on aggregates), CG-2 + CHECK-7 (multi-return forwarding), SEMA-3 (`is_assignable` element-blind) are all cases where the two phases disagree about what is legal, and the failure surfaces as an LLVM assert or a location-free "module verification failed". Whenever one of these is fixed, add the corresponding check to the *other* phase as an internal error with a location.
3. **Eager-generic (Opaque) tolerance is asymmetric.** CHECK-5 and CHECK-6 list expression forms whose siblings have the deliberate `TypeKind::Opaque` early-out but which themselves hard-error on valid generic code. Sweep for `Opaque` handling once, as a family.
4. **Two type printers, both claiming to mirror the other.** SEMA-15 and LSP-10 are the same problem from both ends; fix together.
5. **Comments promise more than the code enforces.** Several bugs hide behind comments asserting an invariant no caller upholds (LSP-4's empty-URI guard, LSP-10's "must agree" printers, FE-10's "absolute location" claim). When touching these, make the invariant structural (API/type), not textual.

### Deliberate decisions to respect (from the retired DEFERRED.md, commit `7ab9415`)

- **match/switch arm loops in sema are intentionally not unified** (old CHECK-9b): `match` unifies arm types and requires exhaustiveness, `switch` does neither; a boolean-flag merge was rejected. CHECK-10 below stays within that decision — it proposes extracting only the semantics-shared *per-arm pattern* helpers (variant lookup, duplicate detection, capture binding), not merging the loops.
- **`find_expr_by_location` is intentionally separate from `ast_walker`** (old LSPH-8). No finding below re-proposes merging them.
- **The two LSP cursor-boundary conventions are intentionally different and pinned** (old LSPH-10). Untouched below.

---

## Phase 0 detail — crashes & miscompiles

### FE-1 · High · bug — Float literals with `_` silently decode wrong
`src/compiler/lexer.cpp:333-337` + `src/compiler/ast.cpp:1147`. `skip_digits()` accepts `_` in float parts, so `1_000.5` lexes as one FloatLiteral; `std::stod` stops at the first `_` and yields `1.0` — wrong constant, no diagnostic. (grammar.md:715 defines FLOAT_LITERAL with no underscores.)
**Fix:** strip `_` before conversion and extend the grammar, or reject `_` in float literals at lex time.

### FE-2 · High · bug — Out-of-range float literal crashes the compiler
`src/compiler/ast.cpp:1147`. `std::stod("1e999")` throws `std::out_of_range`; nothing catches it — compiler terminates. The lexer's exponent path (lexer.cpp:434-446) happily produces the token. `1e+` also decodes silently as `1.0`.
**Fix:** use `std::from_chars` and diagnose both out-of-range and incomplete exponents.

### SEMA-1 · High · bug (reproduced) — `when` with a macro call silently compiles the wrong branch
`src/compiler/value_resolver.cpp:800-1017`; consumers `sema_check.cpp:5205-5216`, `sema_check.cpp:4130-4136`, `sema_declare.cpp:701-712`. `is_constant_expr_impl` accepts macro `CallExpr` as constant (value_resolver.cpp:367-400) but `evaluate_const_value` has no CallExpr case → fold returns nullopt → consumers default to `selected = false` with no diagnostic. Repro: `macro four() -> 4` + `when four() == 4 { … } else { … }` prints the **else** branch. The comment at value_resolver.cpp:963-969 warns about exactly this.
**Fix:** add a macro-expansion case to `evaluate_const_value` (mirror `eval_integer_const_expr`'s), **and** make `when` consumers hard-error when `is_constant_expr` passed but the fold failed (see theme 1).

### SEMA-2 · High · bug (reproduced) — `when N > …` on a generic value param always takes the else branch
`src/compiler/value_resolver.cpp:818-827`, contradicting the contract at `sema.hpp:518-522`. `is_constant_expr` merges generic env value-param names (value_resolver.cpp:609-616), but `evaluate_const_value` never consults `program.active_generic_env_stack`, so the condition folds to false for every instantiation. Repro: `fn report[N: usize]() { when N > 2 {…} else {…} }` prints the else branch for both `report[8]()` and `report[1]()`.
**Fix:** in `evaluate_const_value`'s IdentExpr case, check the top of `active_generic_env_stack` before the module-symbol lookup (as `eval_integer_const_expr` does at type_resolver.cpp:988-1002).

### SEMA-3 · High · bug (reproduced) — `is_assignable` is element-type-blind; ill-typed globals reach the LLVM verifier
`src/compiler/type_resolver.cpp:2016-2020`; unguarded callers at `value_resolver.cpp:454` (global initializers), `sema.cpp:478` (field defaults), `value_resolver.cpp:589` (macro result type). `Array→Slice`, `Slice→Array`, `Slice→Pointer`, `Slice→Anyptr` return true with no element/pointee comparison; `assignable_in_module` patches only some rows, and the sites above call the raw function. Repro: `const S: []u8 = A` with `A: [3]i32` → no sema diagnostic, dies with "LLVM module verification failed". `Slice→Array` and `Slice→Pointer` are also absent from the spec's assignability table (docs/spec.md §15).
**Fix:** move element/pointee comparisons into `is_assignable` itself, or delete the element-blind rows and require callers to use `assignable_in_module`.

### SEMA-4 · High · bug (reproduced) — `std::bad_variant_access` abort on illegal generic value-param type
`src/compiler/type_resolver.cpp:518` — `std::get<ast::BuiltinType>(param.type)` in the value-arg branch. `validate_generic_param_types` only *reports* an illegal param type (e.g. `[N: *i32]`); the declaration stays registered, and the first instantiation hits the unchecked `std::get`. Repro: `type Foo[N: *i32] = …` + `Foo[3]` → correct error, then abort (exit 134). Also kills the LSP worker thread (runs sema with no catch, per sema.hpp:627-629).
**Fix:** `std::get_if` and return `Invalid` when the param type isn't a builtin scalar.

### SEMA-5 · High · bug (reproduced) — OOB `v->args[i]` on macro arity mismatch in const contexts; no recursion guard
`src/compiler/type_resolver.cpp:1047-1061` — `eval_integer_const_expr`'s CallExpr case iterates `params.size()` and indexes `v->args[i]` unchecked. Repro: `const buf: [twice()]u8` where `twice` takes 1 param → correct arity error, then libstdc++ assertion abort. The path also skips `resolved_macro.is_resolved` and has no self-recursion guard for macros in type contexts.
**Fix:** bail to nullopt on `args.size() != params.size()` or `!is_resolved`; add a depth/visiting guard.

### CHECK-1 · High · bug — Arithmetic/bitwise/shift accept any two operands of identical non-numeric type
`src/compiler/sema_check.cpp:345-356` (`binary_op_result`). `Mul/Div/Mod/BitwiseAnd/Or/Xor/ShiftLeft/ShiftRight` (and the `Add/Sub` fallthrough) only check `lhs != rhs`, so `struct + struct`, `"a" + "b"`, `enum | enum`, `bool * bool`, `ptr * ptr`, `f32 << f32` all pass sema and reach LLVM `CreateAdd`/`CreateAnd` on aggregate values (assert or garbage). Comparisons were already hardened for exactly this (`is_aggregate_no_cmp`, line 364, with a comment saying aggregates "must be rejected in sema").
**Fix:** after the pointer-arithmetic special case, require numeric operands — integers for `%`/bitwise/shifts, integer-or-float for `+ - * /`; reject Bool/Enum/Pointer/aggregates.

### CG-1 · High · bug — Range-loop index: 8-byte load from a smaller slot (miscompile)
`src/compiler/codegen.cpp:6474-6487` (`emit_for_in`, range path). The index slot is allocated with the upper bound's LLVM type (e.g. `i32` — bare literals default to `i32`, sema_check.cpp:3978) but registered as `.type = USize`; sema binds the index as `usize` (sema_check.cpp:3893), so any body read does `CreateLoad(i64, <i32 alloca>)` — 4 bytes of stack garbage in the high half. Reachable with `for i, x in 0..4 { arr[i] }`. The only existing example never reads `i`, so tests miss it.
**Fix:** allocate the index slot as i64/USize; keep the bound comparison in the bound's type by widening as needed.

### CG-2 (+ CHECK-7) · High · bug — Multi-return forwarding `return f()` skips per-slot coercion, and can't drop a trailing `?error`
Two halves of the same path:
- `src/compiler/codegen.cpp:6614-6621` vs `sema_check.cpp:5607-5620`: the `return f()` fast path does `CreateRet(emit_call(...))` verbatim while sema validated each slot with `assignable_in_module` (permits array→slice, array→pointer, bitset→storage). `fn g() -> ([]i32, error(E)) { return f() }` with `f() -> ([4]i32, error(E))` passes sema, then dies with the location-free "LLVM module verification failed".
- `sema_check.cpp:5590-5605` vs `5566-5570` (CHECK-7): the forwarding path compares exact arity only, so `return f()` from a `-> (T, U)` function where `f -> (T, U, ?E)` reports an arity mismatch — while group declaration (`const a, b := f()`) and single-value contexts both drop and record the trailing `?error`.
**Fix:** in codegen, extract slots and re-insert via the existing coercion machinery when types aren't exact; in sema, teach the forwarding path the same dropped-`?error` recording its siblings have. (Alternative: restrict sema to exact types for the forwarding form — but that removes an accepted feature.)

### LSP-2 · High · bug — Non-string `"method"` crashes the whole server
`src/lsp/server.cpp:331`: `message.value("method", std::string{})` throws nlohmann `type_error.302` for `{"method": 42}` or `"method": null` (present in responses). The line runs *outside* the `try` starting at :375 and nothing in main catches → `std::terminate`. Not covered by `tests/lsp_robustness_test.py`.
**Fix:** hoist extraction inside the try, or guard with `is_string()`.

### LSP-3 · High · bug — A request task that throws never answers the client
`src/lsp/server.cpp:254-266`: when `task->run()` throws (documented as reachable — any `.at()` miss in the front end), the catch only logs and `mark_done`s; no JSON-RPC error response is sent, so the editor's promise hangs forever. The task already carries `id_key`.
**Fix:** for tasks with a non-empty `id_key`, send an `INTERNAL_ERROR` response from the catch blocks. Do the same for cancelled requests (LSP-13).

### LSP-4 · High · bug — `canonical_path_of`'s empty-return contract enforced by no caller
`src/lsp/server.cpp:85-92` returns `""` for unsupported URIs, but all eight call sites (didOpen :459, didChange :466, didChangeWatchedFiles :498, didClose :531, hover/definition :543, completion :579, references :613, inlayHint :656) use the result unconditionally. An `untitled:` buffer opens a phantom `""` document, runs `analyse("")` per request, seeds `set_source("", text)` into later analyses, and publishes diagnostics for `file://`.
**Fix:** guard at each site (or once in a shared request-decode helper — see LSP-8).

---

## Phase 1 detail — user-visible correctness

### Frontend

**FE-3 · High · bug — `can_start_expr` missing CharLiteral, `$`, prefix `++`/`--`.** `ast.cpp:455-489`, used at :2411/:2448. `return 'x'`, `return $option("k")`, `return ++i` parse as *bare* `return` + separate statement — no parse error, confusing downstream sema error or silent dead code. Fix: add `CharLiteral`, `Dollar`, `PlusPlus`, `MinusMinus` to the switch (also improves `is_asi_gotcha_candidate`).

**FE-4 · Medium · bug — `match_identifier` consumes a non-matching identifier.** `ast_parser.cpp:86-91`, call site `ast.cpp:236`. `struct(align) { … }` consumes `align`, sets `is_packed = false`, matches `)` — unknown qualifier silently ignored (`struct()` also accepted). API is a landmine for future callers. Fix: only advance on lexeme match; report unknown struct qualifiers.

**FE-5 · Medium · bug — Integer literals ≥ 2^64 wrap silently; empty base prefixes lex.** `lexer.cpp:471-499` + `ast.cpp:501-553`. `0x1_0000_0000_0000_0000` wraps to `0` before sema's range check sees it; `0x`/`0b`/`0o` with zero digits decode as `0` (`0xG` → `0`, identifier `G`). Fix: overflow-check during accumulation; require ≥1 digit after a base prefix.

**FE-6 · Medium · bug — `AsmBlock` is not an ASI trigger.** `lexer.cpp:90-117, 165-167, 202-204`. After `x := asm -> rax { … }`, a following line starting with `-`/`(`/`[` is glued into the expression — the exact failure class ASI exists to prevent. Fix: treat `AsmBlock` as a trigger.

**FE-7 · Medium · bug — Failed `expect(AsmBlock)` still runs the asm pipeline on the wrong token.** `ast.cpp:1334-1336` and `:3256-3259`. `asm rax` (missing block) tokenizes an unrelated token's lexeme as asm → cascading bogus diagnostics at a misleading location. Fix: bail out of the asm path when the expect fails.

**FE-8 · Medium · incomplete-feature — Register-name case differs between asm bodies and asm-expression headers.** `ast.cpp:1312-1313` vs `asm_lexer.cpp:82-90`. `MOV RAX, 5` works in bodies; `asm -> RAX { … }` is rejected. Fix: lowercase the header register before lookup and store the normalized name (as `AsmRegisterOperand.name`'s doc comment already claims).

**FE-11 · Medium · bug — Variadic-must-be-last check skipped when the next param uses `:=`.** `ast.cpp:2511-2513`. The check lives inside the `match(Colon)` branch, so `fn f(xs: ...i32, y := 0)` parses without diagnostic. Fix: check `seen_variadic` before branching on `:` vs `:=`.

### Sema core

**SEMA-6 · Medium · incomplete-feature — `impl TRAIT for T` accepted for pointer/function/slice aliases.** `sema_declare.cpp:971-975` rejects only `TypeKind::Trait`. Verified: `type P = *i32; impl Show for P { … }` compiles; downstream machinery (vtables, `find_method` full-scan) was never designed for it. Fix: whitelist Struct/Enum/Union (+ Bitset if intended).

**SEMA-7 · Medium · bug — Generic value args not range-checked against the declared scalar type.** `type_resolver.cpp:509-532`. `type Buf[N: u8]` + `Buf[300]` compiles; `len(b.data)` is 300, and the declared type participates in the cache key/mangling. Same gap in `coerce_option_string` (value_resolver.cpp:658-671) for `$option`/`$env` integer targets. Fix: range-check the folded value against the declared width/signedness at both sites.

**SEMA-8 · Medium · bug — `@init` dependency graph misses bare-import references.** `sema_attributes.cpp:298-322`. Only `mod.symbol` accesses create edges; bare-imported symbols are plain IdentExprs → no edge → `init_call_order` can run initializers in the wrong order silently. Fix: in the IdentExpr case, consult `module.bare_import_origins` and emit the edge.

**SEMA-9 · Medium · bug — Declare-phase namespace walk skips the `is_pub` check its model performs.** `sema_declare.cpp:893-919` vs `type_resolver.cpp:327-330`. `impl a.b.Trait for a.b.Type` can traverse a non-pub re-export that ordinary resolution rejects — same chain resolves inconsistently by phase. Fix: add the crossed/is_pub check — or better, fix via SEMA-11 (single shared walker).

### sema_check.cpp

**CHECK-2 · Medium · bug — Assigning to an error local invalidates narrowed state *before* the RHS is checked.** `sema_check.cpp:3548-3556`. `err = match err { … }` inside `if err { }` is spuriously rejected ("unknown state"); runtime order is RHS-first. `err_unknown_reason` also isn't reset (stale advice text). Fix: check value first, then invalidate; reset the reason.

**CHECK-3 · Medium · bug — Contextual-literal coercion is one-directional in comparisons.** `sema_check.cpp:4005-4012` + `:396-411`. `color == .Red` works; `.Red == color` fails (LHS checked against the outer expected type, usually Bool). Same for `{.A} == modes`. Fix: extend `is_coercible_literal`/the swap to `DotIdentExpr` and `BracedInitializerExpr`; don't forward a comparison's outer expected type into operands.

**CHECK-4 · Medium · bug — Range `for-in`: literal upper bound defeats a typed lower bound.** `sema_check.cpp:3875-3889`. `for i in start..10` with `start: usize` errors (upper checked first, resolves `10` to i32) while `for i in 0..len` compiles. Fix: apply the same non-literal-side-first ordering BinaryExpr/Ternary use.

**CHECK-5 · Medium · incomplete-feature — Missing Opaque allowance in `len()`, SliceExpr, `stackalloc`, match/switch operands.** `sema_check.cpp:4308-4313`, `4380-4401`, `4316-4319`, `2925-2927`/`3146-3148`. Siblings (indexing, for-in, negate, deref, member access, `try`) deliberately stay silent on `TypeKind::Opaque` during the eager generic pass; these paths hard-error on possibly-correct generic code (`len(self.data)`, `self.data[1..n]`, `stackalloc(N)`, `match self.tag`). Fix: add the same early-outs (sweep as a family — see theme 3).

**CHECK-6 · Medium · incomplete-feature — `try` in a group declaration missing the Opaque tolerance the expression form has.** `sema_check.cpp:5535-5553` vs `4444-4448`. `try a, b := self.helper()` fails the eager pass while `const x := try self.helper()` passes. Fix: mirror the expression form's `Opaque` acceptance.

**CHECK-8 · Medium · smell/bug — Assignment-target member resolution re-resolves the object chain recursively.** `sema_check.cpp:1282-1289` + `:1235`. `a.b.c.d = x` re-checks each prefix at every level (exponential in chain depth), re-writes `expr_types`, and `f().field = x` produces two stacked diagnostics, the second misleading (:3545-3547). Fix: compute writability from a single lvalue resolution of the object instead of check-then-probe.

**CHECK-9 · Medium · incomplete-feature — Mutation through `&const_local` is unchecked.** `sema_check.cpp:3511-3524` (AddressOf ignores `lv.writable`) + `:1426-1450` (deref lvalue always writable). `const x := 5; const p := &x; p.* = 7` passes. With no const-pointer type, `&` silently launders const-ness. Fix: reject (or warn on) `&` of a non-`mut` binding — or document the gap as a language decision in the spec.

### Codegen & driver

**CG-3 · Medium · bug — Alignment never propagated to allocas/globals of packed-struct and byte-array types.** `codegen.cpp:714` (packed `setBody`), `:884` (unions as `[N x i8]`) have ABI align 1, but `create_entry_alloca` (:3337) and globals (:1011) never `setAlignment` from sema's `align_of`, while loads/stores of i64/f64 fields get implicit `align 8`. Benign on today's x86-64 pipeline; UB-by-construction once optimization passes or another target arrive. Fix: `setAlignment(align_of(...))` on every aggregate alloca/global. Pair with CG-16 (DataLayout set only at emission).

**CG-4 · Medium · incomplete-feature — Codegen hardcodes x86-64 while the driver targets the host triple.** `emit_process_exit` (:2992-2997), `emit_write_stderr` (:3035), `_start`'s `and $$-16, %rsp` (:3293), SysV eightbyte classifier — vs `main.cpp:183` using `getDefaultTargetTriple()`; `default_target_arch` even advertises Arm64/Wasm names. On aarch64, hosted builds emit x86 asm and fail at assembly time. Fix: gate on the triple and report "unsupported target" cleanly, or thread the triple into `codegen::Options`.

**CG-5 · Medium · incomplete-feature — README Usage doesn't match the CLI.** `README.md:41-46` shows `mirage source.mir` / `-o` forms; `main.cpp:488` *requires* a `build`/`run` action, so every README invocation exits 1. README omits `run`, `-l`, `--std=`, `--cc=`, `--opt`, `--noinit`, `--print-link-directives`, `--dump-ast`, `--no-eager-generic-check`. Fix: rewrite the Usage block from `print_usage`.

**CG-6 · Medium · bug — `run` leaks the temp executable on link failure.** `main.cpp:583-598`: only `object_path` is removed; the `mkstemp` exe placeholder is orphaned in `$TMPDIR`. Also a redundant "mirage: linker failed" after `link_executable`'s own diagnostic. Fix: remove `exe_path` on that path; drop the duplicate message.

**CG-7 · Medium · bug/polish — User-reachable diagnostics with empty source locations.** Of the 23 `report_codegen_error(diag_, {}, …)` sites, these are user-visible with a real location in scope: `:5909`/`:5972` "unsupported global constant initializer" (use `global->decl->location` via `emit_global_initializers` :1681); `:2949` "generic instantiation … may fall through" (`instance.decl->location`); `:3481` "unsupported scalar cast" (caller at :4838 has `v->location`); `:1635` "hosted build requires 'pub fn main()'" (name the root module). Fix: thread the locations through.

### LSP

**LSP-1 · High · bug — No UTF-16 position handling anywhere; all positions are byte columns.** `src/compiler/lexer.cpp:278` (`++col_` per byte), consumed raw at `server.cpp:544-545`, `handlers/common.cpp:1117-1119`, `handlers/diagnostics.cpp:26-32`, `handlers/inlay_hint.cpp:212`, `handlers/references.cpp:14-16`. The server never reads `general.positionEncodings` nor advertises `positionEncoding` (server.cpp:409-419), implicitly promising UTF-16 while speaking UTF-8 bytes. Any non-ASCII on a line misplaces everything after it; `completion.cpp:458` can slice mid-codepoint. Fix: negotiate `positionEncoding: "utf-8"` when offered; otherwise convert at the JSON boundary (one line-indexed UTF-8↔UTF-16 helper each way).

**LSP-5 · Medium · incomplete-feature — Find References never searches type positions.** `handlers/ast_walker.cpp` walks only Expr/Stmt; type annotations (`x: MyType`, param/return/field types, impl targets, generic args) are never visited — references on a type returns near-zero results with no indication. Fix: add an `on_type` callback to the walker and resolve `NamedType` occurrences.

**LSP-6 · Medium · incomplete-feature — `includeDeclaration` dropped for type and ext-fn targets.** `handlers/references.cpp:341-441`: declaration sites emitted for FunctionSymbol/GlobalSymbol/MacroSymbol/methods, never for TypeSymbol or ExtFunctionSymbol (both carry `location`/`decl`). Fix: add the two branches.

**LSP-7 · Medium · bug — Module-scope initializer / macro reference walk is crippled, plus one dead call.** `handlers/references.cpp:355-356`: `collect_references_in_scope({.params = {}, .body = nullptr}, …)` is a no-op (returns immediately, :304). The hand-rolled visitors after it (:358-366, :379-385) match only IdentExpr, so `mod.symbol`, `.Variant`, member and bitset-flag references inside module-scope const initializers and macro templates are silently missed. Fix: delete the dead call; reuse the full `on_expr` visitor.

**LSP-12 · Medium · bug — Scope-blind, line-based local shadowing gives wrong answers.** `handlers/common.cpp:496-548`: "last declaration with `line <= cursor` wins" ignores block scope — `mut y := 1; if c { mut y := "s"; }` then `y` after the `if` resolves to the *inner* `y`. Fix: track block extents during the walk (the bracket index exists) and discard candidates whose block closed before the cursor.

**LSP-13 · Medium · bug — Cancelled requests get no response at all.** `server.cpp:237-247, 281-291`: spec says respond with `ErrorCodes.RequestCancelled` (-32800); stricter clients keep the promise pending. Same fix path as LSP-3.

---

## Phase 2 detail — duplication & structure

**FE-9 · Medium — `parse_var_decl_stmt` / `parse_var_decl` near-verbatim copies.** `ast.cpp:2200-2239` vs `2955-2992`; only the group-decl branch and `is_pub` differ. Share a helper returning the common fields.

**SEMA-10 · Medium · smell — Silent-false fold pattern on `$option` failure paths.** `sema_declare.cpp:706-710`, `sema_check.cpp:5210-5214`: a string-valued fold for a `when` condition leaves `selected` false silently. Covered by the theme-1 plumbing fix: once `is_constant_expr` passes, a failed/mistyped fold must be a hard error.

**SEMA-11 · Medium · duplication — Four (plus one) namespace-chain walkers.** `type_resolver.cpp:299`, `sema_declare.cpp:893`, `sema_attributes.cpp:259`, `value_resolver.cpp:108`, and `try_resolve_namespace_chain` in sema_check.cpp. SEMA-9 is the concrete cost of the drift. Fix: one shared walker with a `check_pub` flag, external linkage.

**SEMA-12 · Medium · duplication — `resolve_option_expr` / `resolve_env_expr` ~40 near-identical lines.** `value_resolver.cpp:715-798`. Extract a shared implementation taking a value-source callback (as `coerce_option_string` already did).

**SEMA-13 · Medium · duplication — `:=`-param/variadic signature resolution exists three times.** `sema.cpp:289-314`, `value_resolver.cpp:516-536`, and the instance-signature path in `instantiate_generic_function`. Any new param modifier must land in three places.

**SEMA-14 · Medium · smell — `Program` god-object + ambient mutable stacks.** `sema.hpp:690-942`: 25+ fields; `active_expr_tables`/`active_generic_env_stack` are raw-pointer stacks pointing at stack-local envs, with correctness resting on comment-documented invariants (e.g. the four-way ordering constraints on `check_generic_templates_for_program`, sema.cpp:707-717). Minimum: split `ResolveState` + generic-instantiation machinery into their own headers; wrap the two ambient stacks in accessor types that enforce the invariant by API.

**CHECK-10 · Medium · duplication — match/switch arm dispatch ~400 near-duplicate lines.** `sema_check.cpp:2724-2993` vs `3014-3191`. *Within the bounds of the pinned CHECK-9b decision* (do not unify the loops): extract per-arm pattern helpers — variant lookup, duplicate-arm detection, coverage vectors, capture binding incl. the by-ref escape check, unreachable-default logic. The "all match arms must have the same type" accumulator block also repeats 8× inside `check_match_expr` alone; the pair has already drifted once (comment at :2630).

**CHECK-11 · Medium · duplication — Verbatim duplicated blocks worth helpers.**
- `SizeOfExpr` vs `AlignOfExpr`: `sema_check.cpp:4177-4223` / `4225-4262` — byte-identical except the noun.
- Function-name→fn-pointer decay (variadic reject + `@always_inline` warning + signature compare) ×4: `:3392-3450`, `:3703-3740`, `:2420-2432`.
- `TernaryExpr` vs `WhenExpr` branch checking: `4032-4073` / `4081-4124`.
- `instantiate_generic_function` vs `instantiate_generic_method`: `1865-2014` / `2016-2122` — cache-hit marking, suppression, param/return loops are near-clones.

**CG-8 · Medium · duplication — `emit_match` vs `emit_switch_stmt` ~350 near-duplicate lines.** `codegen.cpp:5136-5322` / `5324-5473`: three-way operand split, variant/tag lookups, error-unwrap preamble (marked "identical comment"), default-arm handling; the merge/PHI epilogue repeats 3× inside `emit_match` (5225-5231, 5265-5271, 5315-5321). The entry-block-alloca fix already had to land twice (:5186/:5371). A shared "classify operand + iterate arms" skeleton parameterized on arm emission (value vs statement) is fine here — the CHECK-9b decision was about *sema* semantics, which codegen doesn't share.

**CG-9 · Medium · duplication — Function-prologue parameter binding ×4.** `emit_function` (3355-3369), `emit_method` (2799-2813), both branches of `emit_generic_function_instance` (2912-2925, 2928-2941), plus the 8-line state-reset block ×3. Extract `begin_function_body(fn, params, param_types, module_path)`.

**CG-10 · Medium · smell — `emit_try_propagation`'s `loc` parameter is dead; one caller fabricates `{}`.** `codegen.cpp:4643`; `emit_stmt` group-decl path (:6119) passes `{}` while `call_ptr->location` is at hand. Delete or use it.

**LSP-8 · Medium · smell — Request-dispatch boilerplate copy-pasted ×4.** `server.cpp:540-575, 576-609, 610-652, 653-686`: id/`id_key`/`cancelled` setup, in-flight warning, queue-full rejection, `run_cancellable_request` wrapping (~40 lines each). One `enqueue_request(method, params_parser, handler)` helper — also the natural home for the LSP-4 empty-path guard.

**LSP-9 · Medium · duplication — `resolve_base_name` + dotted-chain walk triplicated.** `common.cpp:1489-1511` / `completion.cpp:483-498` / `references.cpp:169-185`; chain-stepping at `common.cpp:1604-1618`, `completion.cpp:517-527`, and inside `expected_type_at` (`completion.cpp:382-393`). Already drifted: the completion/references copies drop `display_override`. Fix: shared `resolve_base_name(...)` and `walk_chain(...)` in common.cpp.

**LSP-10 (+ SEMA-15) · Medium · duplication — Two type printers, already diverged.** `lsp/type_printer.cpp:212-330` and `compiler/sema.cpp:20-85` both render `ResolvedType`; each carries a comment claiming they mirror the other (type_printer.cpp:317, sema.cpp:74). They disagree today: `describe_type` renders Bitset/Type/Any/Namespace as `"<type>"`, never module-qualifies, no generic-args suffix — a diagnostic and a hover can spell the same type differently. Fix: one printer with an options struct (qualify?, expand-generics?), or at minimum a unit test locking the shared subset.

**LSP-11 · Medium · smell — References sweep: full recompile per request, no mid-sweep cancellation.** `references.cpp:477-488` + `server.cpp:645-649`: every request re-walks the filesystem and `analyse_uncached`s each candidate module serially on the single worker thread; `$/cancelRequest` is only honored before/after the whole compute. Fix: check the cancelled flag between modules (thread the `shared_ptr<atomic<bool>>` into `WorkspaceSearch`); consider a per-generation sweep cache.

---

## Phase 3 detail — polish (Low)

### Frontend
- **FE-12** `ast.cpp:1902, 1945` — vacuous `check(Ampersand) && !check(AmpAmp)` conditions (lexer already fused `&&`); delete.
- **FE-13** `lexer.cpp:260-269` — `peek()` returns `'\n'` at EOF, `peek_next()` returns `'\0'`; paths silently rely on the `'\n'` sentinel. Comment at minimum.
- **FE-14** `lexer.cpp:345-352` + `ast.cpp:120-137` — virtual semicolons alias the next token's offset; a loop iteration consuming only a virtual `;` looks like "no progress" and `LoopProgressGuard` force-advances past a real token during recovery. Locate the virtual `;` at the previous line's end.
- **FE-15** `lexer.cpp:568-575` + `ast.cpp:645-652, 686-689` — duplicate diagnostics for malformed char/string literals (lexer + parser both fire).
- **FE-16** `lexer.cpp:594-599` — `'é'` reports "unterminated character literal" instead of "multi-byte character literals are not supported".
- **FE-17** `ast.cpp:3546, 3563` — `parse_decl`'s `top_level` param is dead, and `!top_level || …` would make every decl implicitly `pub` if ever passed `false`. Remove or fix.
- **FE-18** `ast.hpp:659-664` — `TaggedVariantExpr.payload == nullopt` never constructed (all three sites set it; qualified payload-free variants are MemberExpr). Stale comment/dead state.
- **FE-19** `ast.cpp:1195-1259` — `size_of`/`align_of`/`type_of` operand disambiguation ×3; also `parse_return_stmt`/`parse_return_ok_stmt` (2404-2463), `parse_option_expr`/`parse_env_expr`, and the return-type list loops are clone pairs. Each is one helper away from single-sourced.
- **FE-20** `asm_lexer.cpp:19-24` / `asm_parser.cpp:16-21` — `to_lower` defined twice; belongs in `asm_registers.hpp`.
- **FE-21** `asm_lexer.cpp:150-154` — out-of-range asm immediates conflated with "malformed"; distinguish "does not fit in 64 bits".
- **FE-22** `lexer.cpp:133-134, 211-213` — `asm_header_budget_ = 16` magic number; after a malformed `asm ->` header, a `{` within 16 tokens is raw-captured as an asm body and ASI is suppressed in the window. Name the constant, comment the failure mode.
- **FE-10** (Medium, latent — parked here because nothing consumes it today) `asm_lexer.cpp:43-45` — asm token `SourceLocation.offset` is relative to the block text while line/col are absolute; `asm_parser.hpp:19-21` claims "absolute … no offsetting needed", and the LSP *does* use `.location.offset` on main tokens (`common.cpp:1458-1461`). Add the block's base offset; note `length` stays 1 (one-char carets under multi-char operands).

### Sema core
- **SEMA-15** `sema.cpp:82` — `describe_type` has no Any/Type/Namespace cases → `"<type>"` in user diagnostics. Subsumed by LSP-10's unified printer.
- **SEMA-16** `type_resolver.cpp:1767, 2259-2261` — array byte-size arithmetic truncates into `ArrayInfo::size : uint32_t` with no overflow check; a >4 GiB array type wraps silently. Reject or widen.
- **SEMA-17** `sema.cpp:216`, `value_resolver.cpp:488` — unchecked `.at()`/`std::get` chains on bare-import alias resolution; a miss terminates the LSP thread. Prefer `find`/`get_if` + internal-error diagnostic, consistent with sema.hpp:906-914's own philosophy.
- **SEMA-18** `type_resolver.cpp:1125` — `layout_enum` holds an unused `auto &mod`; delete or comment if it's an intentional throw-if-absent probe.

### sema_check.cpp
- **CHECK-12** `:2847-2849` — tagged-union match default arm mutates shared `locals` (scalar path copies at :2740); error-state side effects leak into later arms.
- **CHECK-13** `:2760, :3036` — scalar match/switch never validates the pattern's *type* against the operand; a `u64` global constant is accepted as a pattern in a `match` on `u8` (literals are caught, non-literal constants are not).
- **CHECK-14** `:1008, :1035, :1072` — `check_call_args` reports every mismatch at the whole call's location; `get_expr_location(args[i])` is available.
- **CHECK-15** `:5472-5473` — `asm -> reg` result type from surrounding context skips the scalar check the explicit `: type` path has (:5467-5470); `const s: SomeStruct = asm -> rax { … }` reaches codegen.
- **CHECK-16** `:2299, :2320` — `template_check_depth` is the last bare set/reset counter bracketing recursive checking; everything else here got RAII (`ScopedGenericScope`).
- **CHECK-17** `:1894-1904, :2050-2058` — `generic_fn_instance_lookup` is a linear scan per call site; O(instances × call-sites). Hash map keyed on mangled name.
- **CHECK-18** `:2695-2703` — duplicate `'_'` arm updates `out_default_arm_idx` to the later duplicate; "unreachable default" then points at the wrong arm.

### Codegen & driver
- **CG-11** `:4874-4885, :4917-4928, :5764-5780` — enum/bitset constant lookup ×3; fn-symbol→pointer lookup ×4 (`:4719-4727, :4862-4871, :5754-5761`). One helper each.
- **CG-12** `:3262-3264, :3315-3317` — `emit_init`/`emit_start` re-implement `extract_error_tag` (:4462).
- **CG-13** `:4984` — string literals never interned; no `unnamed_addr` (linker can't merge either); built byte-by-byte instead of `ConstantDataArray::getString`; every panic site re-creates "panic: unhandled ".
- **CG-14** `:2770, :6557-6560` — `find_self_ptr_type` / for-in by-ref silently fall back to `pointee_index = 0` (wrong type, no diagnostic) when the pointee was never interned; make it an internal error.
- **CG-15** `:3549-3550` — pointer arithmetic lowered as ptrtoint/add/inttoptr instead of `getelementptr i8`; discards provenance for every `p + n`, pessimizing any future optimization pipeline.
- **CG-16** `main.cpp:201` — DataLayout installed only at emission; all implicit alignments/folds during codegen used the default. Coincides with x86-64 today; compounds CG-3/CG-4. Set it on the module before `codegen::generate`.
- **CG-17** `codegen.cpp:262-264` — `retained_contexts` function-local static parks every `LLVMContext` forever; not thread-safe, accumulates across in-process reuse. Return a `{context, module}` pair (or `ThreadSafeModule`).
- **CG-18** `:6688` — `emit_import_bin` doesn't check the stream opened; a file deleted between sema and codegen silently becomes an empty `[]u8`. One `if (!file)` check.

### LSP
- **LSP-14** `definition.cpp:14-22`, `references.cpp:15-23` — zero-width ranges (`start == end`), so clients can't highlight the target; token length is available. `location_json`/`position_json`/`to_zero_based` also re-implemented in definition.cpp, references.cpp, inlay_hint.cpp:19, diagnostics.cpp:7 — one shared helper fixes both.
- **LSP-15** `completion.cpp:54-57` — `BUILTIN_TYPE_NAMES` includes `"isize"`, which does not exist anywhere in the compiler; accepting the completion inserts invalid code.
- **LSP-16** `completion.cpp:305-310` — `expected_type_at`'s `line` and `module_path` params unused.
- **LSP-17** `uri.cpp:75-86` — `file://localhost/…` (valid per RFC 8089) rejected; scheme match case-sensitive; `path_to_uri` leaves non-ASCII unencoded while VS Code percent-encodes, so server-minted URIs may not string-compare equal to client URIs. (Windows drive letters are documented out of scope — not a bug.)
- **LSP-18** `transport.cpp:44-46` — `read_line`'s EOF path returns the line before the `\r` strip.
- **LSP-19** `analysis.cpp:212-237` — `last_published_nonempty_diag_files_` never shrinks for abandoned closures; session-long growth.
- **LSP-20** `server.cpp:407` — `workspace_root` written unsynchronized on the main thread; a protocol-violating repeat `initialize` would race worker-thread reads (:646). Cheap guard: ignore repeat initialize.

---

## Verified non-issues

Checked and deliberately **not** flagged (so nobody "fixes" them later):

- **Frontend:** no-separator statements are documented grammar (grammar.md note 1); group declarations being statement-only matches grammar.md:399-402; the ASI trigger list is otherwise sound (`return_err` exclusion, `.*` Dot-lookback, `KwReturnOk`/`KwDefault`/`KwUndefined`); `LoopProgressGuard` covers all list loops — no non-terminating recovery path found; SourceManager view-invalidation, CRLF handling, and caret clamping are correct.
- **Sema:** `resolve_final_shallow/full` writing `ts->resolved` across recursion is safe (unordered_map reference stability); `intern_error_union` dedupe, tagged-union layout unification, trait-composition cycle stack, and the ScopedResolveMark RAII conversion are sound; diagnostic-engine cap/dedupe correct (dedupe set bounded in practice by the 20-error cap); module_resolver's absolute-import rejection and stdlib containment are correct.
- **sema_check:** switch non-exhaustiveness is documented (:2996-3006); `!`/condition operands are deliberately coerced in codegen (:167-172); underfilled array initializers default-fill (confirmed against codegen:1357-1379); runtime `when`-expressions are backed by a real runtime path (codegen:5501/5537); `opaque_param_index` exclusion from equality/hash is static_assert-guarded.
- **Codegen/driver:** C-variadic args are promotion-checked in sema (`is_valid_variadic_arg`); the panic helper's i32 tag load is sound (`is_valid_error_member` requires `enum(i32)`); the dual keying of `expr_generic_fn_instance` is consistent on both sides incl. `try`; `mkstemp`/`mkstemps` are race-safe and link/run use `fork`+`execvp` with argv arrays (no shell-injection surface); error-union forwarding is guarded by `error_unions_interchangeable`.
- **LSP:** the threading model is sound (single-owner DocumentStore on the worker, OutputChannel mutex, FIFO ordering of didChange before reads); transport hardening (header caps, empty contentChanges) is real and tested; capability registration matches implemented handlers exactly; 1-based/0-based conversions are consistently correct in both directions.
