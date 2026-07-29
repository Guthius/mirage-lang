# Mirage Compiler & LSP — Full Source Review Findings

Full review of everything under `src/` (compiler + LSP), performed 2026-07-29 across 9
parallel subsystem passes reading every file in full. This document is written as a
work order: each finding has a stable ID, severity, category, exact location, a
concrete failure scenario, and a suggested fix direction. Nothing in the codebase was
changed while producing this document.

## How to work through this

1. Fix in severity order: **HIGH** first (crashes, miscompiles, memory-safety, silent
   data corruption), then **MEDIUM** (real but narrower correctness/UX bugs, partial
   implementations with a clear user-visible gap), then **LOW** (refactors,
   consistency, polish).
2. Several HIGH findings were **verified against the actual `build/mirage` binary**
   with a minimal repro (marked "Verified" below) — trust these; reproduce them
   yourself before fixing to get a regression test out of the same repro.
3. Findings marked "plausible/unverified" were found by careful reading but not
   executed — confirm the failure scenario before investing in a fix, but don't
   dismiss them without checking.
4. Several findings recur across sections as the *same underlying bug class* hit in
   multiple places (e.g. "missing `TypeKind` case in a switch", "operator location
   captured after `advance()`", "duplicated logic drifting apart"). Where that
   happens it's called out explicitly — fixing the class, not just the instance, is
   usually the better investment.
5. After fixing anything here, run the existing test suite (`tests/`) and the
   `examples/` fixtures, and add a regression test/example for the specific bug fixed
   (this project keeps one `.mir` fixture per fixed bug — follow that convention).
6. Do not fix by deleting functionality or adding broad try/catch "just in case" —
   several findings below are exactly about *silent* failure paths; the fix is
   almost always to make the failure loud (a diagnostic) or correct (compute the
   right answer), not to swallow it further.

---

## Priority index — all HIGH severity findings

| ID | Area | One-line summary | Verified? |
|----|------|-------------------|-----------|
| [LEX-1](#lex-1) | Lexer | Unrecognized byte fakes an EOF token and truncates the rest of the file from tokenization | Yes |
| [LEX-2](#lex-2) | Lexer | Multi-line string/char literals compute wrong line and **underflow** column (unsigned wraparound) | Yes |
| [LEX-3](#lex-3) | Lexer | `default`/`undefined` missing from ASI-trigger list → silently merges with next line | Yes |
| [DIAG-1](#diag-1) | Diagnostics | Caret-padding loop has no bounds check → OOB read/crash on any diagnostic with a bad column (e.g. triggered by LEX-2) | Yes |
| [PARSE-1](#parse-1) | Parser | 8 binary/ternary/when parse functions capture `location` *after* consuming the operator → wrong diagnostic underline/span for `&`, `^`, `\|`, `in`, `&&`, `\|\|`, `?:`, `when...else` | Plausible, same class as project's known fixed `BinaryExpr` bug |
| [PARSE-2](#parse-2) | Parser | `{ .Variant(expr), ... }` inside an array/collection literal misparses as a bitset literal | Plausible |
| [SEMA-1](#sema-1) | Sema core | `check_struct_field_defaults_for_module` holds a reference into `Program::structs` across a call that can reallocate that vector (generic instantiation) → use-after-free | Plausible, high-confidence given generics shipped this session |
| [SEMA-2](#sema-2) | Sema declare | Duplicate bare `impl TYPE { fn f() }` method names silently overwrite each other with no diagnostic; earlier body is dropped, never checked/codegen'd | Plausible |
| [CHECK-1](#check-1) | Sema check | Ternary/`when` expressions don't apply literal-coercion ordering that `BinaryExpr` has → order-dependent spurious type errors | **Verified** |
| [CHECK-2](#check-2) | Sema check | No integer-literal range/overflow check against a narrower target type anywhere in the compiler → silent truncation, zero diagnostic | **Verified** |
| [CHECK-3](#check-3) | Sema check | `size_of`/`type_of` on a generic type instantiation (`size_of(List[i32])`) unconditionally errors "not yet supported" — stale pre-generics TODO | **Verified** |
| [TYPE-1](#type-1) | Value resolver | `INT64_MIN / -1` (and `% -1`) is unguarded in both `int64_t` const-fold evaluators → SIGFPE crash on a `const` expression or match/switch case label | Plausible, straightforward repro given |
| [CODEGEN-1](#codegen-1) | Codegen | `match`/`switch` by-ref capture (`&payload`) stores a pointer into a non-entry-hoisted alloca → every loop iteration's captured reference silently aliases the same stack slot | Plausible, mechanism fully traced |
| [CODEGEN-2](#codegen-2) | Codegen | `ext fn` SysV ABI struct-by-value coercion (fixed once for `Struct`) never extended to `Array`/`Union`/`Bitset`-by-value — same corruption class, left uncovered | Plausible |
| [CODEGEN-3](#codegen-3) | Codegen | Global `const` initializers can't be a tagged-union payload constructor or payload-free variant — hits an internal "unsupported global constant initializer" error | Plausible |
| [CLI-1](#cli-1) | main.cpp | Temp object/exe filenames built from unseeded `std::rand()` → deterministic, identical across every process run; concurrent builds race and can delete each other's files | Plausible, easy to verify |
| [CLI-2](#cli-2) | main.cpp | `parse_options` silently drops malformed CLI input (missing `-o` value, stray positional token) instead of erroring — later flags silently never parsed | Plausible |
| [MODRES-1](#modres-1) | module_resolver | Local `import(...)` path resolution has no containment/absolute-path check, unlike the stdlib-fallback branch and `resolve_contained_path` — `import("/etc")` or `import("../../../x")` escapes the module tree | Plausible |
| [LSPCORE-1](#lspcore-1) | LSP server | No try/catch around JSON-RPC message dispatch or worker-thread task execution → any malformed/incomplete request field throws and kills the entire LSP process | Plausible, multiple concrete trigger sites given |
| [LSPCORE-2](#lspcore-2) | LSP server | `contentChanges.back()` on an empty array is UB (not even a catchable exception) | Plausible |
| [LSPCORE-3](#lspcore-3) | LSP transport | No upper bound on `Content-Length` before allocating the body buffer → multi-GB allocation from a single malformed header | Plausible |
| [LSPH-1](#lsph-1) | LSP handlers | Trait dynamic-dispatch method calls (`shape.draw()`) never resolve in hover/completion/definition/references — the LSP has no equivalent of sema's "Tier-3" trait-handle dispatch path | Plausible, mechanism fully traced against sema_check.cpp |
| [LSPH-2](#lsph-2) | LSP handlers | Bitset flag members (`.Write`, `modes += .Close`) have zero resolution support anywhere in the LSP — `TypeKind::Bitset` missing from `match_enum_or_variant` | Plausible |
| [LSPH-3](#lsph-3) | LSP handlers | `find_enclosing_function` has no `TraitImplDecl` branch → hover/completion/go-to-def/references/inlay-hints all break for `self`, params, and locals inside **every** `impl Trait for Type { ... }` method body | Plausible, high-confidence (sibling functions in the same file do have the branch) |
| [LSPH-4](#lsph-4) | LSP handlers | "Find All References" walker never inspects `DotIdentExpr`/`TaggedVariantExpr` → misses virtually all idiomatic bare `.Variant`/`.Flag` usages | Plausible |

---

## 1. Lexer / Tokenizer / Source Manager / Diagnostics

Files: `src/compiler/lexer.{hpp,cpp}`, `token.{hpp,cpp}`, `source_manager.{hpp,cpp}`,
`source_location.hpp`, `diagnostic_engine.{hpp,cpp}`, `asm_lexer.{hpp,cpp}`,
`asm_registers.hpp`.

### LEX-1
**Severity:** High · **Category:** Bug · **File:** `src/compiler/lexer.cpp:604-616`, interacting with the main loop at `lexer.cpp:193-195` · **Verified**

`lex_symbol`'s default case (an unrecognized byte) reports an error and returns a
synthesized `TokenKind::Eof` token. `tokenize()`'s main loop treats any `Eof`-kind
token as "stop lexing" and `break`s. Result: **one stray/unsupported byte anywhere in
the source truncates the entire rest of the file from tokenization** — everything
after it is silently never parsed, never type-checked, never compiled, with no
indication beyond the single "unrecognized character" diagnostic.

Confirmed: a file with a lone `` ` `` on line 2 followed by three valid statements
produces exactly one diagnostic; lines 3-6 are never even looked at.

**Fix:** skip the bad byte (already consumed via `advance()`) and `continue` the main
loop instead of fabricating an `Eof`; only emit a genuine `Eof` at true end-of-input.
Also stop the fabricated `Eof` from carrying a non-empty `.lexeme` / updating
`last_real_kind_`/`last_token_is_asi_trigger_` (the real EOF path at `make_eof()`,
lines 229-235, deliberately leaves that state alone).

### LEX-2
**Severity:** High · **Category:** Bug · **File:** `src/compiler/lexer.cpp:210-217` (`make_location_from_offset`) interacting with `lex_string`/`lex_char` at `lexer.cpp:485-537` · **Verified**

`make_location_from_offset` assumes a token starts and ends on the same source line
(`column = col_ - (pos_ - offset)`). But `lex_string`/`lex_char` never reject an
embedded raw `\n`, so both can produce genuinely multi-line tokens — most realistically
a forgotten closing quote that isn't found until several lines later. For such a
token, `.line` ends up as the token's *ending* line (not its start), and `.column`
**underflows** (unsigned wraparound, since `size_t` is unsigned).

Verified via a standalone probe calling `lexer::tokenize()` directly: a 2-line string
literal produced `line=3 col=18446744073709551612` (should be `line=2`); a
newline-containing char literal produced `col=18446744073709551615`.

Note `lex_asm_block` (lexer.cpp:619-683) already solves the identical problem
correctly by snapshotting `brace_line`/`brace_col` *before* scanning the raw body —
that pattern was simply never applied to `lex_string`/`lex_char`.

This directly feeds **DIAG-1** below (the corrupted column crashes the diagnostic
printer) — verified end-to-end as one causal chain.

**Fix:** snapshot start line/column before the scan loop in both `lex_string` and
`lex_char`, mirroring `lex_asm_block`. Separately worth considering: reject a raw
`\n` inside `lex_string` outright ("unterminated string literal" reported at the
newline), which is what most C-family languages do and avoids the multi-line-token
case existing at all.

### LEX-3
**Severity:** High · **Category:** Bug · **File:** `src/compiler/lexer.cpp:85-108` (`is_asi_trigger`) · **Verified**

`TokenKind::KwDefault` and `TokenKind::KwUndefined` are missing from the ASI-trigger
switch, even though both are complete terminal expressions exactly like
`KwTrue`/`KwFalse`/`KwNil`, which *are* listed. Verified by building:

```mirage
mut x: Foo = default
(get())
```

parses as the single expression `default(get())` (a postfix call on `default`),
producing three cascading, confusing errors instead of two independent statements.
Same repro for `undefined`.

`tests/lexer_asi_test.cpp:81-89` tests this exact pattern for `KwTrue`/`KwFalse`/`KwNil`
but has no case for `KwDefault`/`KwUndefined` — consistent with this being an
oversight from when those keywords were added.

**Fix:** add `KwDefault`/`KwUndefined` to the `is_asi_trigger` switch, plus matching
test cases in `lexer_asi_test.cpp`.

### DIAG-1
**Severity:** High · **Category:** Bug / crash · **File:** `src/compiler/diagnostic_engine.cpp:60-67` (`print_diagnostic`) · **Verified**

The caret-padding loop —
`for (uint32_t i = 1; i < diagnostic.location.column; ++i) out << source_line[i-1]...`
— has **no bounds check** against `source_line.size()`. Only the separate
`caret_count` computation a few lines later attempts a clamp, and only for the caret
run itself, not the padding loop. When `location.column` is anomalously large (as
produced by LEX-2, but reachable in principle from any stage that reports a bad
location), this indexes far past the `string_view`'s buffer via `operator[]` (UB).

**Confirmed by direct test:** reporting a synthetic diagnostic at the corrupted
string-literal location from LEX-2 crashes immediately with an assertion failure in
this build (`std::string_view::operator[]` `__pos < this->_M_len`), i.e. real OOB
memory access in a release build without assertions. This makes the diagnostic
printer itself a crash vector for *any* compiler stage that reports a diagnostic at a
bad/oversized column — independent of fixing LEX-2, this should be hardened on its
own.

**Fix:** clamp the padding loop's index (and ideally validate/clamp
`location.column`/`length` in general) before indexing into `source_line`.

### Other lexer/diagnostics findings

- **LEX-4** (Medium, robustness) — `lexer.cpp:77-80`: `is_digit`/`is_hex_digit`/`is_alpha`/`is_alpha_numeric` pass a plain `char` straight into `std::isdigit`/`isxdigit`/`isalpha`/`isalnum`. Per the standard this is UB unless the value is representable as `unsigned char` (or EOF) — any source byte with the high bit set (non-ASCII text, stray UTF-8/Latin-1 bytes, a UTF-8 BOM at file start) triggers it. `asm_lexer.cpp` already does this correctly everywhere (`static_cast<unsigned char>(ch)` — lines 12, 16, 21, 125, 129, 217-218). **Fix:** apply the same cast in the four main-lexer helpers.
- **LEX-5** (Low, refactor) — `lexer.cpp:115-116` vs `asm_lexer.cpp:31-32`: `SourceLocation::line`/`column` are `size_t`, but the main lexer tracks `line_`/`col_` as `uint32_t` while `asm_lexer` tracks the same concept as `size_t`. Harmless today but an inconsistent width for the same quantity across two lexers in the same file set.
- **DIAG-2** (Low, dead code) — `diagnostic_engine.hpp:26` and every diagnostic call site: `Diagnostic::stage` (Lexer/Parser/Sema/Codegen) is set on every diagnostic but **never read anywhere** (not in `print_diagnostic`, not in the LSP). Either wire it up (e.g. a `[sema]`-style prefix, or surface it as the LSP diagnostic's `source`/`code`) or remove the field.
- **DIAG-3** (Low, behavior confirmation needed) — `diagnostic_engine.cpp:8-11`: once `error_count_ >= MAX_ERRORS` (20), `report()` returns before distinguishing level, so warnings emitted afterward are silently dropped too, not just errors. Confirm this is deliberate; if so, comment it.
- **SRCMGR-1** (Medium, robustness/API hazard) — `source_manager.cpp:30-34` (`set_source`): `insert_or_assign` on an existing key move-assigns into the stored `std::string`, freeing/replacing its old heap buffer. Any previously-returned `SourceFile.text` (`string_view`) for that path becomes dangling the instant `set_source` is called again for the same path on the same instance. Not currently triggered (the one call site, `lsp/analysis.cpp:14`, builds a fresh `SourceManager` per `analyse()` call) but an unguarded footgun for any future incremental/long-lived use. **Fix:** document the invalidation contract explicitly, or return something with a safer lifetime than a raw `string_view` into a mutable map.
- **SRCMGR-2** (Low, refactor) — `source_manager.cpp:36-67` (`get_source_line`): builds a temporary `std::string(filename)` on every diagnostic printed just to probe an `unordered_map<std::string,std::string>`; a transparent hasher/comparator would avoid the per-call allocation.
- **SRCMGR-3** (Low, robustness) — `source_manager.cpp:51-64`: doesn't strip a trailing `\r` for CRLF-terminated files; the returned line (and its `.size()`) includes the `\r`, skewing `print_diagnostic`'s `max_length`/`caret_count` clamp by one character on CRLF lines.
- **ASMREG-1** (Medium, partial-impl) — `asm_registers.hpp:19-68`: the classic 8-bit high-byte registers `ah`/`bh`/`ch`/`dh` are present in neither `registers` nor `unsupported_registers` (their low-byte siblings `al`/`bl`/`cl`/`dl` and the REX-only `dil`/`sil`/`spl`/`bpl` *are* listed). `asm { mov ah, 1 }` gets a confusing "undefined variable 'ah'" (treated as a Mirage identifier) instead of a clear "register not supported in v1" diagnostic like `xmm*`/`cr*`/`dr*` get. **Fix:** add them to `unsupported_registers` for a clear diagnostic (or to `registers` if actually supporting them).

---

## 2. AST & Parser

Files: `src/compiler/ast.{hpp,cpp}`, `ast_parser.{hpp,cpp}`, `asm_parser.{hpp,cpp}`.

### PARSE-1
**Severity:** High · **Category:** Bug · **File:** `src/compiler/ast.cpp`, 8 functions · **Plausible — same class as a bug the project has already hunted and fixed once**

Every left-associative binary-operator parse loop from `parse_multiplicative` through
`parse_equality` follows the pattern `location = current_location(); advance();`
(captures the operator's own location, *then* consumes it). Eight later functions
invert the order — they consume the operator first, *then* read `current_location()`,
so `location` ends up pointing at the start of the RHS operand instead of the
operator:

- `parse_bitwise_and` — ast.cpp:1807-1810
- `parse_bitwise_xor` — ast.cpp:1832-1834
- `parse_bitwise_or` — ast.cpp:1850-1853
- `parse_in_expr` — ast.cpp:1875-1876
- `parse_logical_and` — ast.cpp:1892-1893
- `parse_logical_or` — ast.cpp:1909-1910
- `parse_ternary_expr` — ast.cpp:1926-1927
- `parse_when_expr` — ast.cpp:1956-1957

`sema.hpp`'s `get_expr_location` (line 710) returns this `.location` verbatim for
`BinaryExpr`/`TernaryExpr`/`WhenExpr`, and it's used directly for diagnostic
underlines throughout `sema_check.cpp`. Concretely: `a & b` underlines wherever `b`
starts, not the `&` token; same for `^`, `~` (xor), `|`, `in`, `&&`, `||`, `?:`, and
`when...else`.

Project memory documents having hunted and fixed "the same class of bug" for
`BinaryExpr::location` and for `get_expr_location` on `CallExpr`/`MemberExpr` — these
8 sites are apparently the still-outstanding instances of that same class.

**Fix:** capture `location` via `current_location()` *before* the consuming
`advance()`/`match()` call in all 8 functions, matching the pattern already used in
`parse_multiplicative`/`parse_additive`/`parse_shift`/`parse_relational`/`parse_equality`
and `parse_assign_expr`.

### PARSE-2
**Severity:** High · **Category:** Bug · **File:** `src/compiler/ast.cpp:701-758` (`parse_braced_initializer`) · **Plausible, no existing fixture exercises this shape**

`parse_braced_initializer` disambiguates a leading `.identifier` inside `{...}` by
checking `peek_next().kind != TokenKind::LBrace` then whether `peek_next() == Equal`
to choose `StructExpr` vs `BitsetExpr`. It never checks for `LParen`, which is the
token shape of the `.variant(expr)` payload-construction sugar (ast.hpp:1304-1312,
added generally for tagged unions). Concretely:

```mirage
type Option = union(enum) { Some: i32, None }
const arr: [2]Option = {.Some(5), .None}
```

At `{`: current is `.`, peek is `Some`, peek_next is `(`. The disambiguation
condition treats this as struct/bitset territory (only `LBrace` is excluded), and
since peek_next isn't `Equal` either, it falls into the `BitsetExpr` member-list loop,
which consumes `.Some` as a bitset member name then chokes on the following `(` with
"expected ',', got '('".

Grepped the repo — no `.mir` fixture exercises this shape. Looks like a genuine gap
between the `.variant(expr)` sugar (added 2026-07-24) and the bitset-literal
disambiguation added later (2026-07-26), which never accounted for it.

**Fix:** also treat `peek_next().kind == TokenKind::LParen` as "not struct/bitset
here", falling through to ordinary array-element parsing (where `parse_primary`'s own
`.` handling already parses `.Variant(expr)` correctly). Add an example fixture once
fixed (`example_tagged_union_array_literal` or similar), matching this project's
convention of one `.mir` regression fixture per fixed bug.

### Other parser findings

- **PARSE-3** (Medium, bug/error-recovery) — `ast.cpp:958-979` (`parse_grouped_attributes`): for `@(section("x"))`, when a grouped-attribute member is followed by `(`, the code reports "a grouped attribute cannot take arguments" but never skips the offending `(...)` span. The recovery loop then exits, `expect(RParen, ...)` fails again, and control returns to `parse_decl` still sitting on `(`, which then trips "attributes are only allowed on 'fn' declarations" and finally a generic "expected declaration" fallback — one malformed input produces 3-4 unrelated-looking diagnostics instead of one clear one. **Fix:** on this error, skip a balanced `(...)` span (mirror `asm_parser.cpp`'s `skip_bracketed_span` or the existing `LoopProgressGuard` pattern at ast.cpp:54-137).
- **PARSE-4** (Low-Medium, consistency/bug) — `ast.cpp:2072-2096` (`parse_var_decl_group_stmt`): the comma-scanning loop silently accepts an *empty* name slot whenever two commas appear back-to-back or a trailing comma precedes `:=`. `sema_check.cpp` treats an empty name exactly like `_` (the documented discard mechanism), so `a,, b := f()` and `a, := f()` are silently accepted as if written with an explicit `_`. This second, undocumented discard spelling isn't mentioned in `grammar.md`/`spec.md` anywhere and is more likely to be a typo (accidental double comma) than a deliberate discard. **Fix:** either document it explicitly, or require `_` and treat a bare comma-gap as a parse error.
- **PARSE-5..9** (Medium, refactor — duplicated/drifted parsing logic):
  - Tagged-variant braced-payload parsing (`.field = expr` loop building `StructExpr::Field`s) is copy-pasted almost byte-for-byte three times: contextual `.variant{...}` in `parse_primary` (ast.cpp:1280-1301), qualified `Type.variant{...}` in `parse_postfix` (ast.cpp:1566-1590), and the `{.field = expr, ...}` branch of `parse_braced_initializer` (ast.cpp:708-729). Extract a shared `parse_dot_field_list(Parser&) -> vector<StructExpr::Field>`.
  - The match-arm-pattern lambda (`.name` / `.name(capture)` / `.name(&capture)` / `_` / literal) is duplicated verbatim between `parse_primary`'s `KwMatch` handling (ast.cpp:1360-1388) and `parse_switch_stmt` (ast.cpp:3026-3051). Extract `parse_match_arm_pattern(Parser&) -> MatchExpr::ArmPattern`.
  - Parameter-list parsing (`mut`? name `:`/`:=`, type-or-default, `...` variadic tracking) is independently implemented three times with subtle divergence: `parse_function_params` (ast.cpp:2354-2418), the inline loop in `parse_impl_method` (ast.cpp:3237-3280), and `parse_trait_method_decl` (ast.cpp:2493-2525) — the trait-method copy doesn't track `seen_variadic`/reject-position the same way the other two do, evidence the three copies have already started drifting. A shared parameterized helper (leading-comma-required flag, variadic-policy enum) would prevent further divergence.
  - `parse_when_stmt` (ast.cpp:2144-2167) and `parse_when_decl` (ast.cpp:3399-3421) are structurally identical (condition, forced-block then-branch, optional `else`/`else when` chain), differing only in Stmt-body vs Decl-list body.
  - `parse_generic_args`'s comma loop (ast.cpp:162-179) is re-implemented inline in `parse_index_or_slice_expr` (ast.cpp:1455-1470) for the "remaining args after the first" case.
- **PARSE-10** (Low, consistency) — `#link`'s lookahead is written out verbatim at both call sites (ast.cpp:3136, 3491) instead of sharing a helper the way `#error`/`#warn` do via `peek_diagnostic_directive_kind` (ast.cpp:910-915). Add a `peek_link_decl(parser)` mirroring it.
- **PARSE-11** (Low, consistency) — `SliceExpr` (ast.hpp:562-567) requires both `start`/`end` explicitly, unlike `RangeExpr::lower` which is `optional<Expr>` ("nullopt means implicit 0", exercised by `for x in ..upper`). `arr[..5]` (implicit-zero start) isn't reachable even though the conceptually identical range form is. May be intentional scope — flagged as an unexplained asymmetry, not a confirmed bug.
- **ASMPARSE-1** (Low, bug/error-recovery) — `asm_parser.cpp:102-105` (`parse_operand`'s default branch): on an unexpected operand token (e.g. a stray `,` or `]`), reports "expected an operand..." but doesn't consume the token, causing `parse_instruction`'s operand loop to immediately break and fire a second, redundant "expected ',' or end of instruction" diagnostic for the same malformed operand. **Fix:** advance past the offending token in the default case, mirroring every other branch of `parse_operand`.

Files with no notable findings: `ast_parser.hpp`/`ast_parser.cpp` (bounds-checked
`peek()`/`peek_next()`, correct EOF clamping), `asm_parser.hpp`.

---

## 3. Semantic Analysis — Core / Declare / Attributes

Files: `src/compiler/sema.{hpp,cpp}`, `sema_declare.cpp`, `sema_attributes.cpp`.

### SEMA-1
**Severity:** High · **Category:** Bug — memory safety (use-after-free) · **File:** `src/compiler/sema.cpp:399-410` (`check_struct_field_defaults_for_module`) · **Plausible, high-confidence given generics shipped this session**

```cpp
const auto *struct_info_ptr = program.struct_at(ts->resolved->struct_index);
const auto &struct_info = *struct_info_ptr;   // reference into Program::structs (plain std::vector<StructInfo>)
...
for (size_t i = 0; ...) {
    const auto &field_type = struct_info.fields[i].type;
    const auto init_ty = check_expr(*field.init, empty, module_path, program, diag, field_type, 0);
    if (!is_assignable(init_ty, field_type)) { ... }   // struct_info/field_type re-read after the call
}
```

`struct_info` is a reference into `Program::structs`, a plain `std::vector<StructInfo>`
(not the node-stable containers `generic_fn_instances` uses specifically to avoid this
hazard — see `sema.hpp:614-621` and its `vector<unique_ptr<...>>` design). `check_expr`
is called with a mutable `Program&`, and per `type_resolver.cpp` (`instantiate_generic_type`,
lines ~1113/1124/1220/1556), a field-default expression that is the *first* use of
some generic struct/enum/union/bitset instantiation will `program.structs.push_back(...)`,
which can reallocate the vector and invalidate `struct_info_ptr`/`struct_info`/`field_type`
mid-loop.

Concretely:
```mirage
type List[T: type] = struct { ptr: *T = nil, len: usize = 0 }
type Foo = struct { items: List[i32] = List[i32]{} }
```
If `List[i32]` hasn't been instantiated earlier in the pipeline, evaluating `Foo.items`'s
default reallocates `program.structs`, and the immediately-following `is_assignable`
call on the same iteration reads freed memory. With 2+ defaulted fields, the second
field's read is corrupted even more directly.

**Fix:** capture the needed field type by value before calling `check_expr`, or
re-fetch `program.struct_at(...)` fresh on each iteration instead of holding a `const&`
across the call.

### SEMA-2
**Severity:** High · **Category:** Bug / consistency · **File:** `src/compiler/sema_declare.cpp:645-659` (`declare_one_decl`, `ImplDecl` case) · **Plausible**

```cpp
for (auto &fn : v.functions) {
    if (find_attribute(fn.attributes, "init")) { ... }
    module.methods[type_name][fn.name] = MethodInfo{ .decl = &fn, ... };  // unconditional overwrite
}
```

Unlike every other symbol kind (`declare_symbol` reports "redefinition of 'x'") and
unlike trait-impl method registration (`register_trait_impls_for_program`,
sema_declare.cpp:858-897, which explicitly checks
`impl_info.methods.contains(fn.name)` and reports "duplicate method"), a bare
`impl TYPE { ... }` method whose name collides — within one impl block, across
multiple `impl TYPE {}` blocks for the same type, or across the multiple `.mir` files
merged into one module directory — silently overwrites the prior `MethodInfo` with no
diagnostic. The earlier method's body is never type-checked or codegen'd; calls
silently resolve to whichever definition was declared last in file-merge order.

Concrete failure: two files in the same module both define
`impl Point { fn distance(...) {...} }` with different bodies; compiles cleanly, one
implementation silently vanishes.

**Fix:** mirror the trait-impl duplicate check —
`if (module.methods[type_name].contains(fn.name)) diag.report_error(...)` before
inserting.

### Other sema core/declare/attributes findings

- **SEMA-3** (Medium, partial-impl) — `sema_declare.cpp:605-606` (`declare_one_decl`, `FunctionDecl` case): `validate_generic_param_types` is documented (sema_declare.cpp:66-69, sema.hpp:809-811) as "shared by...all four decl kinds that can carry generic_params (type/fn/impl/trait-impl)" but is **never actually called** for a bare generic free function. Confirmed via grep: called only from `declare_type`, the `ImplDecl` path, and `register_trait_impls_for_program` — never for `fn foo[T: SomeIllegalType](...)`. **Fix:** add the missing call in the `FunctionDecl` branch.
- **SEMA-4** (Medium, consistency/latent bug) — `sema_declare.cpp:423-477` (`ensure_condition_modules_declared`): this function exists specifically to force-declare a module referenced from a `when`-condition before folding (the fix for the documented "cross-module unordered_map iteration order" bug). It handles `MemberExpr`/`BinaryExpr`/`UnaryExpr`/`CastExpr`/`WhenExpr`/`IdentExpr` explicitly but falls through silently for everything else, with **no exhaustiveness guard** — unlike the near-identical `walk_expr_for_foreign_refs` in `sema_attributes.cpp:268-381`, which uses `static_assert(!sizeof(V), ...)` specifically so a new `ast::Expr` variant can't slip through unhandled. A `when` condition like `size_of(mod.SomeType) > 4` (a `BinaryExpr` whose lhs is a `SizeOfExpr` wrapping a cross-module member access) isn't recursed into, silently reintroducing the exact iteration-order bug class this function exists to prevent, just for a different expression shape. **Fix:** give this function the same exhaustive/`static_assert` treatment as `walk_expr_for_foreign_refs`, or unify the two into one shared exhaustive walker parameterized by callback.
- **SEMA-5** (Medium, partial-impl) — `sema_attributes.cpp:125-140` (`validate_init_structural`): never checks `decl.generic_params.empty()`. `@init` is rejected on impl/trait-impl methods at declare time, but nothing stops `@init pub fn setup[T: type]() { ... }` — a generic function has no single concrete body/signature to call implicitly, which is nonsensical for `@init`. `validate_init_dependencies_for_program` (sema_attributes.cpp:468-549) will schedule it into `Program::init_call_order` regardless. **Fix:** reject `@init` on any decl with non-empty `generic_params`.
- **SEMA-6** (Low, cosmetic) — `sema_declare.cpp:687-692`: bare-import cycle diagnostic is reported with a default-constructed `SourceLocation{}` instead of the real location of the triggering import/when-decl, unlike essentially every other diagnostic in these files.
- **SEMA-7** (Low, cosmetic cascade) — `sema_declare.cpp:685-724`/`266-299`: when a bare-import cycle is detected, the reentrant module's symbol table is only partially built at that point, so the other side of the cycle sees a partial `pub` symbol set — layers extra confusing "unknown identifier" diagnostics on top of the real cycle error, order-dependent on where the cyclic import sits in source.
- **SEMA-8** (Low, refactor) — `sema.cpp:156-175` (`resolve_signatures_for_module`): this type-forcing loop appears to be dead weight — `check_program` already calls `ensure_module_declared` for every module before this runs, and `ensure_module_declared` already force-layouts everything (with `layout_done` guards making repeats a no-op). Looks like a leftover from before `ensure_module_declared` was generalized. Worth deleting to shrink the function to just its function/ext-fn signature loop — verify with a targeted diff + full test run before removing.
- **SEMA-9** (Low, refactor) — `sema_declare.cpp:81-158` (`declare_type`): five near-identical struct/enum/union/trait/bitset slot-allocation blocks. Defensible today, but the next new declarable `TypeKind` is one copy-paste-and-forget away from an inconsistency.
- **SEMA-10** (Low, refactor) — `sema_attributes.cpp:113-123` / `164-181`: `find_attribute(attrs, "naked"/"always_inline"/"no_return")` is looked up once inside `validate_common_attributes`, then re-looked-up again immediately after for the `@init`-combination checks. Bind once, reuse.
- **SEMA-11** (Low, refactor) — `sema.cpp:418-479` (`check_bodies_for_module`) vs `sema.cpp:485-515` (`check_trait_impl_bodies_for_program`): both build an identical `LocalScope` (globals + `self` + params) and call `check_stmt`, differing only in where the method table/module context comes from. Extract a shared `check_method_body(MethodInfo&, module_path, program, diag)`.
- **SEMA-12** (Low, refactor) — `sema.cpp:229-278` (`resolve_impl_signatures_for_module`) vs `sema.cpp:286-324` (`resolve_trait_impl_signatures_for_program`): near-identical param/return/variadic resolution loops, differing mainly in defaults policy. Factor the resolution loop out, leave the defaults-policy divergent.
- **SEMA-13** (Low, refactor) — `sema_declare.cpp:483-517` (`declare_link_decl`) vs `sema_declare.cpp:525-556` (`declare_diagnostic_decl`): both do check_expr against `[]u8` → is_assignable → is_constant_expr → evaluate_const_value → extract string, with an internal-error fallback. Extract `resolve_constant_u8_slice_arg(...)` — this project's own history (`@env` added next to `@option`, `#warn` added next to `#error`) shows this exact block is likely to keep getting re-copied for the next directive.

---

## 4. Semantic Analysis — Type Checking (`sema_check.cpp`)

File: `src/compiler/sema_check.cpp` (~4400 lines). All bugs in this section were
**verified against the actual `build/mirage` binary** with minimal repros.

### CHECK-1
**Severity:** High · **Category:** Bug · **File:** `sema_check.cpp`, `TernaryExpr` (1790-1797) / `WhenExpr` (1805-1811) vs `BinaryExpr` (1764) · **Verified**

`BinaryExpr`'s case special-cases `is_coercible_literal` to decide which operand to
check first, so a literal on either side unifies against the other side's real type.
`TernaryExpr`/`WhenExpr` never do this — they always check `then_expr` first with the
*outer* `expected` (often `nullopt`), then check `else_expr` with
`expected = then_ty`.

Verified repro:
```mirage
mut x: u8
x = 3
const y := cond ? 5 : x    // error: ternary branches have different types
const y := cond ? x : 5    // compiles fine — same values, operands swapped
```
`then_ty` defaults literal `5` to `i32` (no expected type available); `x` (u8) is then
checked with `expected=i32`, but `IdentExpr` ignores `expected`, so `i32 != u8` →
error. Swapping operand order avoids it entirely.

**Fix:** apply the same `is_coercible_literal(then)/is_coercible_literal(else)`
ordering swap used in `BinaryExpr` to `TernaryExpr`/`WhenExpr`.

### CHECK-2
**Severity:** High · **Category:** Bug · **File:** `sema_check.cpp:1608` (`LiteralIntegerExpr` case) · **Verified**

`if (expected && expected->is_integer()) return *expected;` — no bounds check at all.
Grepped the whole compiler: no integer-literal range/overflow check exists at any
stage.

Verified repro: `mut x: u8; x = 300;` compiles cleanly (exit 0), silently
truncates/UB at codegen. Same for negative literals into unsigned types.

**Fix:** when `expected` is narrower than the literal's natural width, evaluate the
constant and reject/warn if it doesn't fit `expected`'s range (signed/unsigned
aware).

### CHECK-3
**Severity:** High · **Category:** Partial-impl · **File:** `sema_check.cpp`, `IndexOrInstantiateExpr` case (2378-2389), interacting with `SizeOfExpr`/`AlignOfExpr`/`TypeOfExpr` (2175, 2198, 2226) · **Verified**

The `IndexOrInstantiateExpr` case still carries the comment "TODO(generics): full
IndexOrInstantiateExpr classification...lands with the rest of the generics sema
support" and unconditionally errors "generic-argument instantiation is not yet
supported here" for anything but a single ordinary index.
`SizeOfExpr`/`AlignOfExpr`/`TypeOfExpr` only special-case `IdentExpr`/`MemberExpr`
operand shapes before falling back to generic `check_expr`, which hits this stub.

Verified repro (both fail identically):
```mirage
size_of(List[i32])   // error: generic-argument instantiation is not yet supported here
type_of(List[i32])   // same error
```

Given generics shipped 2026-07-29 per project history, this is an integration gap —
the generics work updated call-callee and lvalue-target classification of
`IndexOrInstantiateExpr` but not the plain-value-position case, nor
`size_of`/`align_of`/`type_of`'s type-name fast paths. `resolve_lvalue`'s own
`IndexOrInstantiateExpr` case (lines 1064-1092) duplicates this same "not yet
supported" gate near-verbatim (see CHECK-8) and would need the identical fix.

**Fix:** extend `SizeOfExpr`/`AlignOfExpr`/`TypeOfExpr`'s ident/member fast-path to
also recognize `IndexOrInstantiateExpr` naming a generic decl (reuse `resolve_type`'s
existing generic-instantiation path for `NamedType`), or give `IndexOrInstantiateExpr`'s
generic classification (already implemented for `CallExpr` callees) a home reachable
from plain expression position.

### Other sema_check.cpp findings

- **CHECK-4** (Medium, bug) — `binary_op_result`'s `Add`/`Sub` case (196-200) only special-cases `TypeKind::Anyptr + integer`; `TypeKind::Pointer` falls through to the generic "types must be exactly equal" path and errors. Meanwhile `IncrDecrExpr` (2152) explicitly permits `TypeKind::Pointer` alongside `Anyptr`. Verified: `p = p + 1` on `*i32` → "operand type mismatch"; `p++` on the same variable → compiles fine. Pick one: support typed-pointer `+`/`-` (mirroring `++`/`--`), or restrict `++`/`--` to `anyptr` to match.
- **CHECK-5** (Medium, partial-impl) — `TernaryExpr`/`WhenExpr` never call `compute_condition_narrowing` (unlike `IfStmt`/`WhileStmt` in `check_stmt`, lines 3789/3834) — the typed-error system's typestate narrowing is invisible in expression position. Verified: `if e { match e {...} }` compiles; `e ? match e {...} : 0` fails with "cannot match on an error value of unknown state" for the semantically identical condition.
- **CHECK-6** (documented-scope gap, low-medium) — `compute_condition_narrowing` (3353-3392) only recognizes the error variable as the leftmost operand of a compound condition, one level deep. `x && err`, `err1 && err2`, or deeper nesting silently fail to narrow with no diagnostic explaining why. Consistent with the file's other "narrow v1 subset" admissions (generics inference) — worth confirming this is the intended v1 boundary before investing effort widening it.
- **CHECK-7** (Medium, refactor) — `check_group_call_returns` (469-660) and `check_expr`'s `CallExpr` case (1897-2148) are ~90% duplicated: explicit generic-function instantiation, member-call namespace resolution, trait-handle dynamic dispatch, generic-method instantiation, struct-field-as-function-pointer calls, and the final Function/ExtFunction/Macro symbol dispatch — reimplemented independently, differing only in single-return vs multi-return. Any future fix (e.g. CHECK-3) has to be applied in both places or drifts. Extract a shared "resolve call target + check args" helper parameterized on return arity.
- **CHECK-8** (Medium, refactor) — `resolve_lvalue`'s `IndexOrInstantiateExpr` case (1064-1092) duplicates `check_expr`'s own `IndexOrInstantiateExpr` case (2378-2405) near-verbatim, including the CHECK-3 "not yet supported" gate. Both need updating together.
- **CHECK-9** (Medium, refactor) — `check_expr` is a single ~1450-line function (1603-3057) — one `std::visit` handling ~40 distinct expression kinds inline (the `MatchExpr` case alone is ~300 lines). `check_stmt` is similarly ~625 lines (3774-4399), with `SwitchStmt`'s scalar/union/enum handling re-deriving `MatchExpr`'s logic a second time. Both are an order of magnitude over the project's own ~150-line guideline. Split per-node-kind into named helpers (`check_call_expr`, `check_match_expr`, `check_switch_stmt`, `check_braced_initializer_expr`, etc.); `MatchExpr`/`SwitchStmt` could likely share one core dispatcher parameterized on statement-vs-expression-arm-body.
- **CHECK-10** (minor consistency, not a bug) — `switch` statements never check exhaustiveness for tagged-union/enum operands with no `_` arm (verified compiles silently). Appears intentional per an in-code comment at line 4081 for the bool case, but the same reasoning isn't documented near the tagged-union/enum `switch` branches, and the asymmetry with `match`'s headline exhaustiveness feature is worth an explicit comment even if the behavior is correct as-is.

---

## 5. Type Resolver / Value Resolver / Symbol Table

Files: `src/compiler/resolved_type.hpp`, `symbol_table.hpp`, `type_resolver.cpp`,
`value_resolver.cpp`. `resolved_type.hpp`/`symbol_table.hpp` had no findings.

### TYPE-1
**Severity:** High · **Category:** Bug — crash · **File:** `value_resolver.cpp:585-586` (`evaluate_integer_constant`) and `value_resolver.cpp:940-941` (`evaluate_const_value`) · **Plausible, straightforward repro**

Both const-fold evaluators guard division/modulo against `rhs == 0` but **not**
against `lhs == INT64_MIN && rhs == -1`, which is unguarded signed-overflow UB (and
typically raises SIGFPE on x86-64).

Repro: `const X = -9223372036854775808 / -1;`, or any `match`/`switch` case-label
expression of that shape — `evaluate_integer_constant` is reachable from case-pattern
folding (`sema_check.cpp:1474/2558/4065`, `codegen.cpp:4475/4621`). On this platform
this crashes the compiler process outright — not a diagnostic, a hard crash.

Note the sibling `uint64_t`-based `eval_integer_const_expr` in `type_resolver.cpp`
(871-872) doesn't have this specific issue (unsigned division has no overflow UB),
which is itself evidence the three const-folders (see TYPE-6) have drifted apart in
care level.

**Fix:** add an `lhs == INT64_MIN && rhs == -1` check before dividing/modding in both
evaluators, alongside the existing `rhs == 0` check. Add a regression test/example.

### Other type/value resolver findings

- **TYPE-2** (Medium, bug) — `value_resolver.cpp:590-591` (`evaluate_integer_constant`) and `value_resolver.cpp:945-946` (`evaluate_const_value`): shift amount (`<<`/`>>`) is unbounded, unlike the sibling `eval_integer_const_expr` in `type_resolver.cpp:876-877`, which explicitly guards `*rhs >= 64 ? nullopt : ...`. Shift amount ≥ 64 (or negative) is UB regardless of standard version. Repro: `const X = 1 << 100;`, or any `@warn`/`@link`-data/`when`-condition of that shape. **Fix:** mirror the `type_resolver.cpp` guard in both evaluators.
- **TYPE-3** (Medium, exception-safety) — every cycle guard across `type_resolver.cpp`/`value_resolver.cpp` is a manual `insert(key); ...; erase(key)` (or `push_back`/`pop_back`) pair with no RAII wrapper: `alias_resolving` (481/484), `struct_resolving` (518/521), `union_resolving` (548/551), `bitset_resolving` (567/570), `trait_resolving` (591/594), `trait_composition_stack` (1425/1513), `generic_type_resolving` (1768/1771), `value_resolving` (value_resolver.cpp 350/379, 473/498), `fn_signature_resolving` (value_resolver.cpp 411/438), and `Program::active_generic_env_stack` (pushed/popped in sema_check.cpp/codegen.cpp, read via `.back()` in type_resolver.cpp/value_resolver.cpp). Any exception thrown between insert and erase (an `.at()` miss, a bad `std::get`, an uncaught `std::stoll`) permanently marks that key "resolving," turning every later real error into a spurious "circular dependency" report — or, for the generic-env stack, leaves a **dangling pointer** to a stack-local `env` that a later `.back()` read then dereferences as UB. There's no try/catch anywhere around `sema::check_program`/`ast::resolve` in `lsp/server.cpp` (see LSPCORE-1), so such an exception during an LSP request would propagate out of the worker thread uncaught and kill the process. **Fix:** wrap each insert/erase and push/pop pair in a small RAII scope guard.
- **TYPE-4** (Medium, dead code / doc drift) — `Program::generic_type_instances_needed` (sema.hpp:614-621) is documented as mirroring `generic_fn_instances_needed`, with codegen's instantiation loop supposedly walking only these. Written to at `type_resolver.cpp:1796`, but grep shows **zero reads** anywhere in `codegen.cpp` — `declare_structs()`/`declare_unions()` unconditionally iterate all of `program.structs`/`unions`/`enums`/`bitsets`. Not a correctness bug (over-inclusive codegen is safe) but either dead bookkeeping or an unfinished liveness-gating feature — worth a decision either way.
- **TYPE-5** (Medium, refactor) — non-struct tagged-union-payload wrapping is implemented twice: `layout_union`'s tagged branch (~1116-1142) is a near-verbatim copy of `wrap_payload_in_struct` (1214-1236), added later for the error-union feature but never backported to replace the original. A future alignment fix to one won't automatically apply to the other.
- **TYPE-6** (Medium, refactor) — tagged-union size/align/payload-offset math (`payload_offset = align_up(TAG_SIZE, align)`, etc.) is independently computed three times: `layout_union`'s tagged branch (~1157-1166), `synthesize_error_union`'s inner union (~1275-1281), and its outer union (~1311-1317). Extract `compute_tagged_union_layout(payload_size, payload_align) -> {offset, align, size}`.
- **TYPE-7** (Low-Medium, refactor/consistency) — `size_of`/`align_of` logic exists in three independent copies: `Resolver::size_of`/`align_of` (673-693, used during layout), free functions `resolved_type_size`/`resolved_type_align` (1866-1890, used by LSP hover), and codegen.cpp's own separate copy. All three agree today, but the Trait/Any "primitive_align would wrongly forward to primitive_size's 16" special case already had to be hand-copied into two of the three (687-688, 1884-1885) — a new `TypeKind` or bugfix is one missed copy-paste from LSP hover disagreeing with what codegen actually emits.
- **TYPE-8** (Low, bug — silent wrong answer) — `TypeKind::Namespace` (produced for a bare imported-module identifier used as a value — `sema_check.cpp:1649,2158`) isn't handled by `Resolver::size_of`/`align_of` (673-693), `resolved_type_size`/`align` (1866-1890), or `primitive_size`/`primitive_align` (127-163). Falls through every switch's `default:` and returns `0` with **no diagnostic**. `size_of(my_import)` silently evaluates to `0` instead of reporting "not a value type". **Fix:** add an explicit `TypeKind::Namespace` case that reports an error in all three.
- **TYPE-9** (Low, consistency) — `resolve_type_impl`'s visit over `ast::Type` (1531-1610) ends in a silent `else { return ResolvedType{.kind = TypeKind::Invalid}; }` with no diagnostic and no `static_assert`. All 13 current alternatives are handled today, but nothing forces a future 14th to be. **Fix:** replace the catch-all with a `static_assert(!sizeof(V), ...)` (matching the pattern already used elsewhere, e.g. `walk_expr_for_foreign_refs`) so a missing case becomes a compile error, not a silent `Invalid`.
- **TYPE-10** (verified clean, no action) — grepped both files for direct iteration over `program.modules` (the known iteration-order bug class); every access is via `.find()`/`.at()`. The two direct-iteration sites present (`program.error_unions`, `program.generic_type_instance_lookup`) are `std::vector`s with deterministic order, not susceptible. `walk_namespace_chain`/`resolve_final_shallow`/`resolve_final_full` all consistently thread `ast_program` through per the documented prior fix.
- **TYPE-11** (Low, refactor) — three independently-maintained recursive const-folders (`evaluate_integer_constant`, `evaluate_const_value` here, plus `eval_integer_const_expr` in `type_resolver.cpp`) walk overlapping AST shapes with hand-synced logic that has already drifted (this is the root cause of TYPE-1 and TYPE-2). Consolidate into one evaluator parameterized by desired signedness/width.
- **TYPE-12** (Low, minor robustness) — `intern_error_union` (`type_resolver.cpp:1326-1353`) reports "error(...) member types must be distinct" for `error(A|A)` but doesn't drop the duplicate from `sorted_members` before calling `synthesize_error_union` — produces a broken two-variant union even though a hard error was already reported. No valid program observes this (error already reported, `ok=false`), but a `std::ranges::unique` after sorting would make the error-recovery path internally consistent.

---

## 6. Code Generation (LLVM backend)

Files: `src/compiler/codegen.{hpp,cpp}` (~6000 lines).

### CODEGEN-1
**Severity:** High · **Category:** Bug — memory aliasing · **File:** `codegen.cpp:4378` (`emit_match`) / `codegen.cpp:4544` (`emit_switch_stmt`) / `codegen.cpp:4321-4325` (`emit_variant_capture`) · **Plausible, mechanism fully traced**

`emit_match`/`emit_switch_stmt` each allocate their scratch union slot via
`builder_.CreateAlloca(union_ll_ty, nullptr, "match.union")` **at the current
insertion point** rather than through the file's own `create_entry_alloca` hoisting
helper (used everywhere else for locals/params/scratch that must survive the current
statement — see codegen.cpp:2625-2628). `emit_variant_capture`'s by-ref branch then
stores a GEP'd pointer *into that same non-hoisted alloca* into a captured local:

```cpp
auto *ref_slot = create_entry_alloca(fn, llvm::PointerType::getUnqual(*context_), *vp.capture_name);
builder_.CreateStore(payload_ptr, ref_slot);   // payload_ptr points INTO union_slot
```

Because `union_slot`'s `AllocaInst` is textually inside the loop body, LLVM's default
backend allocates **one static stack slot** per static `alloca` regardless of its
position in the CFG — every dynamic execution of that same instruction returns the
same address. If a `switch`/`match` arm's `&payload`-captured reference is stored
anywhere that outlives the current loop iteration (pushed into a list, returned via
an out-param, stashed in a struct field — all idiomatic uses of capture-by-ref), every
iteration's capture silently aliases the same memory, and after the loop every stored
pointer reads back only the *last* iteration's payload.

Concrete scenario: `while (queue.pop_ref(&msg)) { switch (msg) { .Event(&e) =>
log.push(&e) }; }` — every entry in `log` ends up pointing at the same stack slot
holding the final message's payload.

**Fix:** route `union_slot` (and the related scratch allocas in CODEGEN-14 below)
through `create_entry_alloca`.

### CODEGEN-2
**Severity:** High · **Category:** Partial-impl / bug · **File:** `codegen.cpp:778-790` (`ext_abi_param_type`), `792-823` (return handling), `3025-3061` (`emit_ext_call_arg`) · **Plausible**

All three only special-case `sema::TypeKind::Struct` for SysV x86-64
eightbyte-classification ABI coercion. This is precisely the bug class the project
already fixed once for `Struct` ("ext fn struct-by-value ABI mismatch," 2026-07-16) —
but a small fixed-size `Array` (e.g. `[2]f32` matching a C `float[2]`/vector typedef),
`Union`, or `Bitset` passed by value across an `ext fn` boundary gets none of the
coercion, so LLVM's default aggregate-splitting lowering won't match what a real C
callee reads. Same failure mode as the already-fixed struct case, just left uncovered
for the other by-value-aggregate kinds.

**Fix:** generalize `classify_struct_eightbytes`/`ext_abi_param_type`/`emit_ext_call_arg`
to operate on any aggregate `ResolvedType` (or explicitly extend to cover `Array` at
minimum, since `Union`/`Bitset` by-value FFI is rarer).

### CODEGEN-3
**Severity:** High · **Category:** Partial-impl · **File:** `codegen.cpp:2890-5177` (`emit_const_or_runtime`) · **Plausible (may currently be unreachable if sema's `is_constant_expr_impl` doesn't allow this shape — verify that first)**

`emit_const_or_runtime` (used for every module-level `const`/global initializer) has
**no case at all** for `ast::TaggedVariantExpr` (only `emit_expr`, the runtime path,
handles it), and its `ast::DotIdentExpr` arm (4934-4953) explicitly only handles
`Enum`/`Bitset`; for a payload-free tagged-union variant (`ty.kind == Union`) it falls
through with only a comment to the final:

```cpp
report_codegen_error(diag_, {}, "unsupported global constant initializer");
```

It also never consults `expr_variant_coercions`/`expr_trait_coercions` the way
`emit_value_as` does, so an implicit scalar→tagged-union coercion in a global
initializer has the same gap. A payload-free variant is trivially const-foldable (just
the tag word, rest zero — no alloca needed), so this looks like a straightforward
omission rather than a fundamental limitation.

**Fix:** first confirm whether sema's `is_constant_expr_impl` currently allows a
`TaggedVariantExpr`/union-`DotIdentExpr` as a "constant" expression at all; if it
does (or once findings elsewhere make it reachable), add the missing cases to
`emit_const_or_runtime` — payload-free case as a direct `ConstantDataArray` of tag
bytes + zero padding, payload-bearing case via the existing `build_struct_constant`-style
approach used elsewhere in this function.

### Other codegen findings

- **CODEGEN-4** (Medium, bug risk) — `classify_struct_eightbytes` (694-757): `collect_abi_leaves` produces flat leaves, and the per-slot classification loop computes `float_bytes`/`all_float` by intersecting each leaf against `[slot_start, slot_end)` independently, per slot. A field that spans two 8-byte slots (possible for a `@packed` struct — `is_packed` is a real field) gets split across both slots' classification instead of forcing MEMORY class per real SysV rules, potentially selecting the wrong coerced register type for real C interop (raylib/libc). Not confirmed reachable without checking whether Mirage's field-offset algorithm can produce such straddling for `@packed` — worth a defensive check either way given this is exactly the ABI bug class already hit once.
- **CODEGEN-5** (Medium, consistency/fragility) — `declare_vtables()` computes the vtable "shape" independently in two places: `codegen.cpp:1650` (sizing each family member's array type: `fm.info->methods.size() + fm.info->component_traits.size()`) and `codegen.cpp:1673` (filling it: same formula recomputed independently). A future edit to one and not the other silently desyncs the array type's declared length from the constant initializer's element count, corrupting every method-slot/back-pointer-slot index used by `emit_trait_handle_dispatch`/`emit_trait_handle_coercion`. Extract a single `vtable_slot_count(const TraitInfo&)` helper.
- **CODEGEN-6** (Medium, refactor) — `emit_call` (3398-3624) and `call_return_types` (3626-3715) are ~300-line near-duplicates of the same callee-resolution decision tree (generic-instance lookup, IdentExpr local fn-ptr, MemberExpr namespace/trait/method/struct-field-fn-ptr dispatch, symbol-table FunctionSymbol/ExtFunctionSymbol branch). The file's own comment on `call_return_types` warns the generic-instance check "must be checked first or...would compute the wrong return arity" — exactly the kind of invariant fragile across two independently-maintained copies. Factor a single `resolve_callee(...)` returning a tagged result that both functions switch over.
- **CODEGEN-7** (Medium, refactor) — `sizeof_operand`/`align_of_operand`/`type_of_operand` (4777-4868): three ~35-line functions with byte-identical structure (IdentExpr → module-symbol TypeSymbol lookup → generic-env-stack fallback → MemberExpr namespace-chain lookup → `expr_types` fallback), differing only in the final per-type computation. Parametrize on a callback.
- **CODEGEN-8** (Medium, refactor) — struct/array constant-building is duplicated ~4x between the runtime path (`emit_struct_expr_value`/`emit_positional_struct_expr_value`, 1125-1198) and the const-fold path (inlined inside `emit_const_or_runtime`'s `BracedInitializerExpr` visitor, 5046-5146) — same field-tracking, same default-init fallback, duplicated for named and positional init. `emit_default_value`/`emit_const_default_value` (993-1081) are likewise a near-verbatim pair differing only in `emit_expr` vs `emit_const_or_runtime` recursion. A shared implementation parametrized on the "evaluate a sub-expression" step would collapse ~250 lines to under 100.
- **CODEGEN-9** (Medium, robustness) — `resolve_asm_variable` (5423-5436) returns a default-constructed `AsmVarRef{}` (null `ptr`/`storage_type`) on a lookup miss, relying entirely on sema having already validated the name resolves. Callers immediately `CreateLoad(ref.storage_type, ref.ptr)` (5491, 5612) with no null check — a null here is either an immediate LLVM crash or, worse, a silent miscompile of a raw asm memory operand address if ever reached with wrong-but-non-null values. Given this touches inline-asm addressing directly, add a `report_codegen_error` + safe bail-out.
- **CODEGEN-10** (Low, dead code) — `emit_tagged_variant_expr` (1290-1296) computes `struct_ll_ty` and immediately discards it (`(void)struct_ll_ty;`) — the subsequent store relies on `struct_val`'s own type. Remove.
- **CODEGEN-11** (Low, consistency) — `emit_variant_capture`'s by-ref branch (4321-4330) sets `type_module` to `*current_module_path_` (the match site's module) while the by-value branch correctly uses `struct_module` (the payload struct's declaring module). No observed effect today since aggregate `ResolvedType`s resolve via global index rather than `(module_path, name)` lookup, but it's an inconsistency and a latent trap if any future codegen path starts trusting `LocalValue::type_module` for name-based resolution.
- **CODEGEN-12** (Low, refactor) — bitset `+=`/`-=`/binary `+`/`-` arithmetic (`Add→Or`, `Sub→And-Not`) is independently re-implemented three times: `emit_binary_expr` (2860-2874), `emit_assign` (4734-4743), `const_binary` (5203-5212). Extract `emit_bitset_binary(op, lhs, rhs)`.
- **CODEGEN-13** (Low, refactor, author-acknowledged) — `emit_asm_stmt`/`emit_asm_expr` (5452-5676) have ~120 lines of duplication, called out in-code as "a deliberate, small (~60 line) duplication rather than touching emit_asm_stmt." The operand-rendering loop and constraint-string-building are structurally identical modulo output-index offset; a shared helper parametrized on leading-constraint-count could cut ~100 lines while preserving the `$N` numbering difference the comment cares about.
- **CODEGEN-14** (Low, consistency) — beyond the match/switch case in CODEGEN-1, plain (non-hoisted) `CreateAlloca` appears in several other helpers where the pointer is immediately loaded/discarded within the same expression (so aliasing is harmless today, but inconsistent with the file's own hoisting discipline): `emit_union_expr_value` (1239), `emit_tagged_variant_expr` (1259), `emit_variant_coercion` (1308), `extract_error_tag` (3722), `unwrap_failed_error_value` (3738), `build_error_failed_value` (3755, 3774), `translate_error_value` (3815, 3839), `emit_member_lvalue`'s temporary-spill path (3222), `emit_init`/`emit_start`'s tag-extraction slots (2560, 2613). Worth an audit pass routing these through `create_entry_alloca` for consistency, especially since CODEGEN-1 shows this exact pattern turning into a real bug once a construct captures the address.

**Positive note:** newer features (trait composition, generics, inline asm) were
checked specifically for parallel/duplicate mechanisms vs. established patterns and
came back mostly clean — trait composition's `component_vtables_` reuses the same
`build_trait_handle_value`/`method_fn_key_for` machinery as ordinary trait dispatch;
generics' `active_generic_env_stack` push/pop is scoped correctly with no leak path
(always reaches its `pop_back()`); `emit_method`/`emit_generic_function_instance`/`emit_function`
share boilerplate but for a legitimately different arg-list shape each. CODEGEN-5 and
CODEGEN-1/14 are the real divergences found.

---

## 7. Module Resolver & CLI (`main.cpp`)

Files: `src/compiler/module_resolver.{hpp,cpp}`, `src/main.cpp`.

### MODRES-1
**Severity:** High · **Category:** Bug / security · **File:** `module_resolver.cpp:72-76` (`resolve_import_path`, local-import branch) · **Plausible**

The local-import candidate — `candidate = fs::path(importer_path) / import_path` —
is, if a directory, immediately canonicalized and returned with **no**
`is_contained_in` check and no rejection of absolute paths. Contrast with:
- the `mirage_path` fallback branch just below (82-94), which explicitly calls
  `is_contained_in(mirage_path, canonical_fallback)` to stop stdlib-relative imports
  from escaping the stdlib root;
- `resolve_contained_path` (128-142), a helper written specifically to reject absolute
  paths and directory escapes, which **is** used elsewhere (`type_resolver.cpp:1835`,
  `codegen.cpp:4874`) to sandbox `import_bin(path)`.

Per `fs::path::operator/` semantics, if `import_path` is absolute, the join
**discards `importer_path` entirely** — `import("/etc")` resolves straight to `/etc`
(or any other absolute path the process can read). Unrestricted relative `..`
traversal (`import("../../../../wherever")`) is also completely unbounded for the
local branch, unlike the stdlib fallback.

Note: unrestricted `..` traversal for local sibling-package imports may be
intentional (multi-directory local projects) — the **absolute-path bypass** is the
part that looks like an unambiguous oversight given the codebase's own established
`resolve_contained_path` pattern for exactly this problem class.

**Fix:** route the local candidate through the same kind of check
`resolve_contained_path` provides (reject an absolute `import_path` at minimum), or
explicitly decide and document whether project-root escape via `import()` is
supposed to be allowed.

### CLI-1
**Severity:** High · **Category:** Bug / security · **File:** `main.cpp:459, 467` · **Plausible, easy to verify**

Temp object/executable filenames are built via
`fs::temp_directory_path() / std::format("mirage-{}.o", std::rand())`. `std::srand()`
is never called anywhere in the codebase (verified via grep), so `std::rand()` starts
from the same default seed on every process invocation — the "random" suffixes are
**deterministic and identical across every separate run of the compiler**.

Concrete failure: two concurrent `mirage build`/`mirage run` invocations (parallel CI
jobs, a test runner, two terminals) generate the exact same
`/tmp/mirage-<N>.o`/`/tmp/mirage-<N>` paths, race on writing/linking/executing them,
and one process's cleanup (`fs::remove` at lines 473/480/510) can delete the file out
from under the other mid-flight. Also a predictable-temp-name symlink-attack surface
in a shared `/tmp`.

**Fix:** use a proper unique/secure temp-name mechanism (`mkstemp`/`mkostemp`, or seed
with `std::random_device` + PID + high-res clock).

### CLI-2
**Severity:** High · **Category:** Bug · **File:** `main.cpp` (`parse_options`), lines 84-86, 95-97, 102-104, 120 · **Plausible**

Missing-value cases for `--opt`, `-o`/`--output`, and `-l` all do a bare
`return options;` with **no error message** (contrast with `--opt`'s own
"requires 'key=value'" case a few lines below, which does print one). Separately, any
unrecognized trailing positional token hits `else { break; }` (line 120), which
silently stops parsing the rest of `argv` entirely — any flags after that token are
never processed. `main()`'s only post-parse validation (line 386,
`action == Action::None || module_path.empty()`) doesn't catch either failure mode.

Concrete scenarios:
- `mirage build myproj -o` (trailing `-o` with no filename — e.g. a shell-quoting
  mistake) silently proceeds and writes to the default `a.out`.
- `mirage build myproj extra_token -o out.exe` — the stray `extra_token` triggers
  `break`, so `-o out.exe` is never parsed; output silently goes to `a.out`.
- Same class of silent failure for a dropped `--opt key=value` (a compile-time config
  option silently not applied) or a dropped `-l lib` (later manifests as a confusing
  "undefined reference" linker error with no indication the flag was ever swallowed).

**Fix:** make `parse_options` return/signal an explicit parse-failure (e.g.
`std::optional<Options>` or an error-message field) on any malformed input, and check
that in `main()`.

### Other module resolver / CLI findings

- **MODRES-2** (Medium, UX) — `module_resolver.cpp:170-187`: an invalid `--std=` path is a hard error, but an invalid `MIRAGE_PATH` env var is silently ignored (`mirage_path` just stays empty) — every subsequent `import("std")` then fails with a generic "cannot resolve import path" with no hint the real cause is a stale/typo'd `MIRAGE_PATH`. **Fix:** emit a warning/error for an invalid `MIRAGE_PATH`, mirroring `--std=`.
- **MODRES-3** (Low, error handling) — `module_resolver.cpp:32-33` (`load_and_parse`): a file that failed to load (I/O error) and a legitimately empty `.mir` file are treated identically (`continue`, no flag, no count increment) — an unreadable module file can vanish from the build with no diagnostic at all, if `SourceManager::load` doesn't itself always emit one (check that invariant).
- **MODRES-4** (Low, refactor) — `module_resolver.cpp:100-107` (`visit`): `if (program.modules.contains(path)) return;` immediately followed by `try_emplace(path)`, which already returns `inserted=false` for the same condition. Redundant lookup — drop the `contains` check.
- **CLI-3** (Low-Medium, bug) — `main.cpp:380` vs `70-72`: `main()` gates on `argc < 3` and returns exit code 1 *before* calling `parse_options` at all — so `mirage --help` alone (`argc == 2`) never reaches the `--help` branch (which does `std::exit(0)`); it prints the same usage text but exits 1. Any tooling checking for exit code 0 on `--help` sees a false failure. **Fix:** call `parse_options` (or at least check for `--help`/`-h`) before the `argc` gate, or lower the gate to `argc < 2`.
- **CLI-4** (Medium-High, bug) — `main.cpp:446-457`: the "Processed N file(s)..." stats banner is written to `llvm::outs()` unconditionally, *before* the `--emit-ir` check. `mirage build proj --emit-ir > out.ll` (the natural way to capture IR for `opt`/`llc`/FileCheck) gets a human-readable banner prepended before the actual `; ModuleID = ...` text, making the file invalid LLVM IR until manually stripped. **Fix:** print the stats banner to `llvm::errs()` instead, or move the print after the `emit_ir` early-return.
- **CLI-5** (Medium, configurability) — `main.cpp:208`: linker driver hardcoded as `"clang"` with no override mechanism, unlike `--std=`/`MIRAGE_PATH` for the stdlib path. **Fix:** add a `--cc=`/`MIRAGE_CC` override.
- **CLI-6** (Medium, refactor/security posture) — `main.cpp:127-138, 206-252` (`link_executable`): assembles a shell command string via `shell_quote` and calls `std::system`. `shell_quote`'s escaping does look correct for the inputs it currently receives, but this is inconsistent with the `run` action just below, which correctly uses `fork`+`execv` with an argv array and no shell at all (496-509). **Fix:** replace with `fork`/`execvp("clang", argv)` (or `posix_spawn`) using the existing `args` vector directly, removing the shell dependency and the quoting-correctness question entirely.
- **CLI-7** (Low, error handling) — `main.cpp:474`: link failure only prints "mirage: linker failed" — doesn't surface the actual command or clang's exit status, making failures harder to debug.
- **CLI-8** (Low, resource leak) — `main.cpp:461-463`: if `emit_object_file` fails, `main` returns directly without removing the possibly-partial temp `.o`, unlike the link-failure and success paths just below, which both clean up.
- **CLI-9** (Medium, UX) — `main.cpp:511-516`: a child process killed by a signal (e.g. the compiled program SIGSEGVs) is never distinguished (`WIFSIGNALED` is never checked) — `mirage run` just reports exit code 1 with no "process was killed by signal N" message, which is especially unhelpful for surfacing codegen or user-program crashes. **Fix:** check `WIFSIGNALED(status)` and print the signal name via `strsignal`.
- **CLI-10** (Low-Medium, security/consistency — needs sema-side verification) — `main.cpp:226-238`: `#link(lib, ...)` path directives for `LinkCategory::Lib` get no containment check (same `operator/` absolute-override hazard as MODRES-1); `LinkCategory::Flag` passes `link.data` straight through as a raw clang argument. The `Flag` case looks like an intentional escape hatch but is worth flagging as an explicit trust-boundary decision; confirm whether `sema_check.cpp` validates `LinkDirective::data` before this point (main.cpp itself does not).
- **CLI-11** (Low, robustness) — `main.cpp:459, 467`: `fs::temp_directory_path()` uses the throwing overload with no surrounding try/catch anywhere in `main` — a broken/missing temp-dir configuration produces an unhandled `std::terminate` instead of a clean diagnostic, inconsistent with the file's otherwise careful use of `error_code` overloads elsewhere.
- **CLI-12** (Low, error handling) — `main.cpp:178`: `pass_manager.run(module)`'s return value is discarded; a defective codegen pass could silently produce a broken `.o` with no diagnostic before it reaches the linker.

---

## 8. LSP Core Infrastructure

Files: `src/lsp/main.cpp`, `server.{hpp,cpp}`, `transport.{hpp,cpp}`,
`analysis.{hpp,cpp}`, `type_printer.{hpp,cpp}`, `uri.{hpp,cpp}`, `task_queue.hpp`.

### LSPCORE-1
**Severity:** High · **Category:** Robustness · **File:** `src/lsp/server.cpp:259-386` (message dispatch), plus the worker-thread `compute()` lambdas · **Plausible, multiple concrete trigger sites**

Only the initial `json::parse` call (252-257) is wrapped in try/catch. Every handler
after that does unchecked chained access, e.g. `message.value("method", ...)` (259,
throws `json::type_error` if the top-level message isn't a JSON object), or
`doc["uri"].get<std::string>()` (303), `message["params"]["position"]["line"].get<size_t>()`
(332/351/368). nlohmann's non-const `operator[]` auto-vivifies missing keys to `null`,
so `.get<...>()` on that `null` throws — and none of this is caught anywhere between
here and `main()`.

Concrete scenario: a `textDocument/hover` request missing the `position` field throws,
unwinds out of `Server::run`/`main`, and the process terminates — killing the LSP for
every open file, not just failing the one request.

The same applies on the **worker thread**: `Worker::do_publish_diagnostics` and the
`compute()` lambdas passed to `run_cancellable_request` (339-380) call into
`analyse()`/`sema::check_program()`/handlers with no try/catch. Since this runs inside
a `std::jthread`, an escaping exception calls `std::terminate()` directly. This is
also where TYPE-3's exception-safety gaps would surface if ever triggered.

**Fix:** wrap per-message dispatch in try/catch (turn exceptions into a JSON-RPC error
response when an id is present, log+drop for notifications); wrap `task->run()`
similarly in `run_worker_loop`.

### LSPCORE-2
**Severity:** High · **Category:** Bug — crash (UB, not even catchable) · **File:** `src/lsp/server.cpp:313` · **Plausible**

`message["params"]["contentChanges"].back()["text"]`. nlohmann's `back()` decrements
`end()`, which is UB on an empty JSON array (no bounds check, no throw) — a `didChange`
with `contentChanges: []` (malformed but not something a try/catch fix would rescue)
crashes the process outright.

**Fix:** explicit `.empty()` check before dereferencing.

### LSPCORE-3
**Severity:** High · **Category:** Bug · **File:** `src/lsp/transport.cpp:61` (`std::stoul` on `Content-Length`), `76` (body allocation) · **Plausible**

No upper bound on the parsed `Content-Length` before allocating
`std::string body(content_length, '\0')`. A very large but numerically valid header
(several GB) passes `stoul` and triggers a multi-GB allocation that can throw
`std::length_error`/`std::bad_alloc`, uncaught anywhere in the chain →
`std::terminate`. Same issue for `read_line`'s unbounded header-line `std::getline`.

**Fix:** clamp `content_length` (and header line length) to a sane maximum and
reject/log rather than allocate blindly.

### Other LSP core findings

- **LSPCORE-4** (Medium, bug) — `server.cpp:193-195` (`run_worker_loop`): when a task is cancelled before the worker dequeues it, the `continue` for that branch never calls `completed.mark_done(id_key)` — `in_flight_` entries are only erased via `completed.drain()`, which is only ever populated from inside `run_cancellable_request`, which never runs for a task cancelled at pop time. A request cancelled before the worker reaches it (fast cursor movement during hover, for example) leaks its `in_flight_` entry forever, growing unboundedly over a long session — exactly the growth pattern `CompletedRequests`'s own doc comment says it exists to prevent. **Fix:** call `completed.mark_done(id_key)` in this branch too.
- **LSPCORE-5** (Medium, partial-impl) — no `InvalidRequest` handling for requests arriving after `shutdown`. Per the LSP spec, once `shutdown` is received, further requests (other than `exit`) should error with `InvalidRequest`; `shutdown_received` (set at line 290) is never consulted by dispatch — only `state` (Uninitialized/Running) gates anything.
- **LSPCORE-6** (Medium, robustness) — `canonical_path_of` (71-73) calls `ast::canonicalize(uri_to_path(uri))` without checking for `uri_to_path` returning `""` on an unsupported scheme (e.g. `untitled:` — some editors do send this for unsaved buffers). Feeding `""` into `ast::canonicalize`/`fs::path(path).parent_path()` etc. could throw `filesystem_error`, uncaught per LSPCORE-1.
- **LSPCORE-7** (Low, refactor/UX) — `server.cpp:318-327` (`didClose`): unconditionally publishes an empty diagnostics list for the closed file, bypassing `do_publish_diagnostics`/`files_that_became_clean`. If the file still has genuine on-disk errors and remains part of an open module's import closure, its squiggles vanish on close and won't reappear until an unrelated edit re-touches the closure. Also desyncs `last_published_nonempty_diag_files_` bookkeeping.
- **LSPCORE-8** (Low) — `in_flight_[id_key] = cancelled;` (336, 355, 372) overwrites any prior entry for the same id_key with no uniqueness check; a client reusing an id while an older request with that id is still in flight (protocol violation, not validated) causes cancellation misattribution.
- **LSPCORE-9** (Low, refactor) — `uri.cpp:34-59` (`percent_encode`): doesn't escape a literal `%` — a path containing `%41` won't round-trip correctly through `path_to_uri`→`uri_to_path`. Encode `%`→`%25` first.
- **LSPCORE-10** (Low) — `uri.cpp:62-68` (`uri_to_path`): no handling of a non-empty URI authority (`file://host/path` mis-treated as path `"host/path"`). Likely an intentional Linux-only simplification — worth an explicit guard/comment.
- **LSPCORE-11** (Medium, robustness) — `analysis.cpp:42-56`: cached module analysis is only invalidated from `open`/`update`/`close`, all driven by editor-side LSP notifications. A dependency file that's imported but never opened in the editor (edited externally, `git pull`) never triggers invalidation — cached diagnostics/hover/definition for it stay stale indefinitely, bounded only by the unrelated LRU cap (`MAX_CACHED_MODULES = 32`). No `workspace/didChangeWatchedFiles` registration exists to catch this. Worth registering for it if editor-external edits to dependencies are expected to be common.
- **LSPCORE-12** (Low, refactor) — `analysis.cpp:108-122` (`ensure_analysed`): O(n) linear scan over all cached bundles per request. Bounded by `MAX_CACHED_MODULES=32` today (not a real cost), but a `dir -> bundle key` index would make it O(1) if the cap is ever raised.
- **LSPCORE-13** (Low-confidence, worth a fuzz pass) — `analysis.cpp:27` forces `ast_program.ok = true` before sema, assuming every parser error-recovery path leaves a structurally well-formed (if incomplete) tree. If any recovery path violates that, sema could crash rather than degrade — and per LSPCORE-1, any such crash currently takes down the whole server.
- **LSPCORE-14** (Low-Medium, bug) — `type_printer.cpp:34-48` (`generic_args_suffix`, specifically 41-45): only special-cases `std::get_if<int64_t>(&arg.value_arg)` for const-generic values; per project history, const-generic value params also include `bool`. Hovering `Foo[true]` renders `Foo[?]` instead of `Foo[true]`. **Fix:** add a `bool` branch.
- **LSPCORE-15** (Low, fragility) — `type_printer.cpp:170-175` (trait hover in `describe_type_definition`): zips `method.decl->params` (AST, for names) and `method.params` (resolved types) by index with no length assertion. If these ever desynchronize, hover text silently prints mismatched/missing param types rather than failing loudly.
- **LSPCORE-16** (Low, robustness) — `task_queue.hpp:35-41` (`TaskQueue::push`): no backpressure/capacity bound — a client flooding the server faster than the single worker thread can drain has no cap or coalescing beyond the diagnostics-specific debounce. Not a currently-observed hang/crash risk, but worth a defensive limit.

**Verified correct (no action needed):** `TaskQueue` push/pop synchronization
(mutex+condvar, no races/deadlocks found); `DocumentStore`'s single-threaded-by-design
invariant is respected everywhere; `ensure_analysed`'s LRU eviction cannot evict the
entry it just inserted/returned; `uri.cpp`'s percent-decode bounds check is correct;
`type_printer.cpp`'s `TypeKind::Type` case is present and correct (previously-fixed
bug per project memory, confirmed still fixed).

---

## 9. LSP Request Handlers

Files: `src/lsp/handlers/{common,ast_walker,hover,completion,definition,references,inlay_hint,diagnostics}.{hpp,cpp}`.

These four are the most impactful findings in this section — they each disable a
whole class of LSP functionality for a specific (and in three of four cases, the
*idiomatic*) language construct.

### LSPH-1
**Severity:** High · **Category:** Bug + consistency · **Files:** `common.cpp:172-199` (`resolve_member`), `completion.cpp:101-167` (`add_type_members`) · **Plausible, mechanism fully traced against sema_check.cpp**

The canonical trait-dispatch pattern — `shape.draw()` where `shape: Drawable` — is a
`MemberExpr` whose object type is `TypeKind::Trait`. `resolve_member` dereferences
pointers, checks struct/union fields, enum/variant, then falls to
`sema::find_method(type, member, program)`. `find_method` (sema.cpp:703) only has two
tiers: a bare `impl TYPE {}` block, and `trait_impls_by_type` keyed by the *concrete
implementing type* (e.g. `"Circle"`). A trait's own declared dispatch methods
(`fn draw(self)` inside `type Drawable = trait {...}`) live only in
`program.trait_at(trait_index)->methods`, which `sema_check.cpp`'s
`try_trait_handle_dispatch` (430-464) consults as a distinct "Tier-3" path that the
LSP never mirrors.

**Failure scenario:** hover, go-to-definition, find-references, and completion
(`shape.<complete>`) on `.draw()` at a trait-handle call site all return
nothing/empty — for the single most central trait-dispatch use case in the language
(see `examples/example_traits/main.mir:34-36`).

**Fix:** teach `resolve_member` (and `completion.cpp`'s `add_type_members`) to
special-case `type.kind == TypeKind::Trait` by checking
`program.trait_at(type.trait_index)->methods` before/alongside `find_method`,
mirroring `try_trait_handle_dispatch`.

### LSPH-2
**Severity:** High · **Category:** Bug + consistency · **File:** `common.cpp:131-163` (`match_enum_or_variant`) · **Plausible**

Only handles `TypeKind::Enum` and `TypeKind::Union` — `TypeKind::Bitset` is absent,
the same class of bug project memory describes as already fixed in `type_to_string`,
recurring here in the member-resolution dispatch. Both the fully-qualified
(`sema_check.cpp:931-944`) and the dominant bare `.Flag` contextual form
(`sema_check.cpp:2439-2455`, used throughout `examples/example_bitset/main.mir` —
`modes += .Write`, `{.Close, .Flush}`) route through `match_enum_or_variant`.

**Failure scenario:** hover/go-to-def/find-references/completion on any
`.Write`/`.Close`-style bitset flag reference — the idiomatic form per the compiler's
own example — silently returns nothing.

**Fix:** add a `TypeKind::Bitset` branch to `match_enum_or_variant` that walks
`program.bitset_at(type.bitset_index)->member_enum_type`'s enum fields, returning a
Resolution rooted at the bitset type. Also add the corresponding branch to
`completion.cpp`'s `add_type_members`.

### LSPH-3
**Severity:** High · **Category:** Bug + consistency · **File:** `common.cpp:521-689` (`find_enclosing_function`) · **Plausible, high-confidence — sibling code in the same file has the branch this one lacks**

The decl loop handles `FunctionDecl`, `ExtFunctionDecl`, `MacroDecl`, `ImplDecl`,
`TypeDecl` — never `ast::TraitImplDecl` (the node for `impl TRAIT for TYPE {...}`,
distinct from plain `ImplDecl`). `walk_module_bodies` (`ast_walker.cpp:154-158`) and
`references.cpp:221-228` **both already handle** `TraitImplDecl` — direct evidence
this is an oversight in `find_enclosing_function` specifically, not deliberate
scope-narrowing.

**Failure scenario:** for ANY `impl Drawable for Circle { fn draw(self) {...} }`
method body (a common, real pattern — `examples/example_traits/main.mir`),
`find_enclosing_function` returns an empty `EnclosingFunction{}` for any cursor line
inside it. This breaks:
- hover/completion/go-to-def on `self`, any param, or any local variable declared
  inside the method body,
- `size_of`/`align_of`/`len` hover inside such a body,
- asm-operand hover inside such a body (gated on `enclosing.body`),
- inlay-hint parameter-name hints for calls inside such a body needing `self`/local
  resolution (e.g. `self.other_method(x)`),
- "Find All References" *initiated* by right-clicking a local/param/`self` inside
  such a body (`references.cpp:128`'s `resolve_at()` fails to produce a target at
  all, even though the separate reference-collection loop would find real usages
  fine if given a valid target).

**Fix:** add an `ast::TraitImplDecl` branch to the loop in `find_enclosing_function`,
paralleling the existing `ImplDecl` branch — build `self`/param `ParamInfo`s (self
type resolvable via `program.trait_impls_by_type` or by resolving
`trait_impl->type_name` directly) and pass `&fn.body`.

### LSPH-4
**Severity:** High · **Category:** Partial-impl · **File:** `references.cpp:85-123` (`collect_references_in_scope`) · **Plausible**

The `visitor.on_expr` lambda only handles `ast::IdentExpr` and `ast::MemberExpr`. It
never checks `ast::DotIdentExpr` (bare `.Variant`/`.Flag`) or `ast::TaggedVariantExpr`
(`.Variant{...}`/`.Variant(...)`) — both of which `common.cpp`'s own
`find_expr_by_location`/`resolve_at_tokens` already treat as first-class reference
sites.

**Failure scenario:** "Find All References" on an enum field or tagged-union variant
finds only explicit `Type.Variant`-qualified `MemberExpr` usages (rare in practice)
and misses every idiomatic bare `.Variant` usage (the dominant form per every
enum/union/match example in the codebase) — typically returning zero or near-zero
results for a target that's used pervasively. Compounds with LSPH-2 once bitset
support is added (bare `.Flag` usages need the same handling).

**Fix:** extend the visitor to also match `DotIdentExpr`/`TaggedVariantExpr` nodes,
resolving their cached `expr_types` entry through `match_enum_or_variant` (same
pattern `resolve_at_tokens` already uses), comparing against `target` via
`same_declaration`.

### Other LSP handler findings

- **LSPH-5** (Medium, bug/consistency) — `references.cpp:221-228` (the `TraitImplDecl` branch): unlike the `ImplDecl` branch just above it (205-220), which looks up the method's own `MethodInfo` and pushes `fn.location` when `include_declaration` and the target matches, this branch does neither. "Find All References" with `includeDeclaration: true` on a trait-impl method never includes the `fn` declaration line itself. **Fix:** look up the `MethodInfo` via `program.trait_impls_by_type[{mod_path, trait_impl->type_name.name}]` and push `fn.location`, mirroring the `ImplDecl` branch.
- **LSPH-6** (Low-Medium, bug) — `common.cpp:803-812` (`find_expr_by_location`, `IndexOrInstantiateExpr` case): unlike sibling cases (`SizeOfExpr`/`AlignOfExpr`/`LenExpr`/`MemberExpr`), never checks `location_matches(node->location, target)` on the node itself before recursing into `operand`/`args` — a cursor positioned exactly on an explicit generic-instantiation's own defining token can't be found as its own node. Limited impact since most callers reach the callee identifier first, but an asymmetry worth normalizing.
- **LSPH-7** (Low, follow-on from LSPH-2) — `common.cpp:1090-1123` (`resolve_type_decl_field_at`): no `BitsetType` branch (a bitset's own flags are declared on its backing enum's separate `TypeDecl`, so this may be fine for the enum's own declaration site, but reinforces that flag identifiers referenced *through* the bitset type never resolve regardless of where declared).
- **LSPH-8** (Low, refactor) — `common.cpp`'s `find_expr_by_location`, `walk_stmt_for_locals`/`collect_stmt_locals`, and `ast_walker.cpp`'s generic walker are three separate recursive `std::visit` traversals over the same Stmt/Expr shape, each with different per-node payloads (the file's own comments acknowledge this: "Mirrors the dispatch shape of check_expr/check_stmt...just recursing instead"). Not urgent — each has genuinely different early-exit/collection semantics — but a shared walker with generalized callback shapes ("stop as soon as found" / "collect all names before line X") would remove ~150 lines of near-identical dispatch.
- **LSPH-9** (Low, partial-impl) — `completion.cpp`: no completion support inside explicit generic-instantiation argument lists (`List[<cursor>]` — offering type names). Likely rare enough not to prioritize.
- **LSPH-10** (Low, refactor) — `completion.cpp:201-229` (`chain_anchor`/cursor-classification): a hand-rolled token scan duplicated in spirit (not verbatim) from `token_at`/`chain_prefix` in `common.cpp`. The "cursor mid-token vs. right-after-token-with-nothing-typed" distinction it needs is genuinely completion-specific, so this may not be worth unifying — noted for awareness.
- **LSPH-11** (Low, minor inefficiency) — `references.cpp`: `AsmRegister` targets aren't rejected at the same entry guard that rejects `None`/`Builtin` (`handle_references`, line 129) — falls through, walks every module doing real work, then returns empty via `same_declaration`'s `default: return false;`. Harmless but wasted work; add `AsmRegister` to the entry-guard rejection.

**Verified clean, no action needed:** `ast_walker.cpp` is exhaustive with every
excluded node kind explicitly commented, and correctly includes `TraitImplDecl`
(the direct evidence LSPH-3 is an oversight elsewhere, not a pattern). `hover.cpp`'s
`describe_symbol` is exhaustive over all 6 `sema::Symbol` variants (no 7th kind
exists) and delegates type-string formatting entirely to `type_printer.cpp`, itself
verified complete over `TypeKind`. `definition.cpp` is exhaustive over
`Resolution::Kind` in the ways that matter. `diagnostics.cpp`/`.hpp` — `DiagnosticLevel`
genuinely has only `Error`/`Warning`, so its severity mapping is complete, not
truncated; range computation is correct.

---

## Cross-cutting bug classes (fix the class, not just the instance)

A few of the findings above are one instance of a recurring pattern in this codebase.
Worth keeping in mind while fixing individual items, since a targeted fix to one
instance often leaves siblings unfixed:

1. **"Missing `TypeKind`/AST-node-kind case in a switch/dispatch chain."** Seen in:
   LEX-3 (ASI triggers), TYPE-8 (`Namespace` in size_of/align_of), TYPE-9 (silent
   `Invalid` catch-all), LSPH-2 (`Bitset` in `match_enum_or_variant`), LSPH-3
   (`TraitImplDecl` in `find_enclosing_function`), LSPH-4 (`DotIdentExpr`/`TaggedVariantExpr`
   in reference collection), LSPCORE-14 (`bool` in const-generic hover). Project
   memory independently documents several *already-fixed* instances of this same
   class in the LSP hover/inlay-hint code. When fixing any one of these, it's worth
   grepping for other `switch`/`if constexpr` chains over the same enum/variant in
   the same file to check for siblings.
2. **"Operator/expression location captured after the token is consumed."** PARSE-1
   is 8 fresh instances of a bug class the project has already hunted and fixed once
   for `BinaryExpr` and for `get_expr_location` on `CallExpr`/`MemberExpr`.
3. **"A helper exists to solve exactly this problem elsewhere in the file, but this
   call site doesn't use it."** MODRES-1 (`resolve_contained_path` exists, unused for
   local imports), CODEGEN-1/14 (`create_entry_alloca` exists, unused for match/switch
   scratch allocas), CLI-6 (the safe `fork`+`execv` pattern exists two functions away
   from the `std::system` one).
4. **"N near-identical implementations of the same logic that have already started to
   drift."** CHECK-7/CHECK-8 (call resolution), CODEGEN-6/7/8/12/13, TYPE-5/6/7/11,
   PARSE-5..9, SEMA-11/12/13. Each is individually low-to-medium severity, but
   collectively they're the reason bugs like CHECK-1 (ternary missing what `BinaryExpr`
   has) and TYPE-1/TYPE-2 (one evaluator missing a guard the sibling has) exist in the
   first place — consolidating reduces the rate of *future* drift-bugs, not just fixes
   a past one.
