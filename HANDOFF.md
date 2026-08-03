# Handoff: finish the Mirage self-hosting prerequisites

You are picking up an in-progress body of work on the Mirage compiler. This document is the
complete brief: what was done, what remains, how to verify it, and the conventions and
traps that matter.

**Where it stands (updated 2026-08-03, end of session):** ALL TEN STAGES ARE DONE.
`--backend=native` is the DEFAULT on x86_64-linux and wasm32 (standalone and
emscripten); `--backend=llvm` stays selectable through the soak period, after which
`codegen.cpp` and the LLVM dependency get deleted. What remains is the soak itself,
the deferred corpus migration (§6.2, decision D7), and CI's first real push.

**Read `TODO.md` and `docs/backend.md` before writing any code.** This file tells you *how*
to work on the repo; those two tell you *what* is left and *why the design is the way it
is*. Neither is optional. `docs/backend.md`'s stage ordering in particular exists for
reasons that are easy to override without it — and every one of its predictions that has
been tested so far turned out to be right, including that inline `asm` would get SIMPLER
once we owned the register allocator.

---

## 0. Repository layout

Three repositories, all on disk as siblings:

| Path | What it is |
|---|---|
| `/mnt/projects/Projects/Mirage/Mirage-Cpp` | The compiler (C++23). Branch `fix/todo-resolution`. |
| `/mnt/projects/Projects/Mirage/Mirage` | The standard library (Mirage source). |
| `/mnt/projects/Projects/mirage303` | A 90-file VB6→Mirage game port; the real-world consumer. |

The compiler is `mirec`/`mirage`: lexer → parser → module resolver → multi-phase sema →
code generation. There are now TWO code generators behind `--backend=`: `codegen.cpp`
(LLVM, the default) and the native path — `mirgen.cpp` → `mir_passes.cpp` →
`backend_x86.cpp` → `x86_encoder.cpp` → `elf_writer.cpp`. `mirage_core` (everything
except `codegen.cpp` and `main.cpp`) links no LLVM, which is why the whole native path
lives there; `mirage-lsp` links only `mirage_core`.

### Build and test

```sh
cd /mnt/projects/Projects/Mirage/Mirage-Cpp
just build            # configure + build → build/mirage, build/mirage-lsp
just test             # ctest (7 targets) + every tests/*_test.py
```

`just test` defaults `MIRAGE_STD` to `../Mirage`; override it if your stdlib is elsewhere.
It runs every suite even after one fails and lists them all at the end. The backend's own
nets are `backend_differential_test.py` (both backends over the corpus),
`mir_suite_test.py` (the assertion suite, both backends) and
`x86_encoder_differential_test.py` (our encoder vs GNU `as`); the last skips cleanly if
binutils is absent.

**Check `cmake --build`'s EXIT STATUS, not its output.** This cost real time once: a
root-owned `build/.ninja_log` made ninja bail with `Error writing to build log`, which does
not match an `error:` grep, so a "verified" fix was actually tested against a stale binary.
If you hit it: `rm -f build/.ninja_log`.

---

## 1. FIRST STEP: validate the existing work

Do not start new work until this passes. The point is to confirm the baseline is real, and
to build your own understanding of the code you are about to extend.

1. **Full battery is green.**
   ```sh
   just test
   ```
   Expect ctest (7 targets) plus every `tests/*_test.py`, all passing. Two of those are
   the backend's own safety nets and take the longest: `backend_differential_test.py`
   and `mir_suite_test.py`.

2. **The two backends agree on the whole corpus.**
   ```sh
   python3 tests/backend_differential_test.py
   ```
   Expect **74 of 74 positive fixtures matched, 0 mismatched, 0 refused**. This is the
   single most important number in the project: it says the native backend compiles and
   runs every program the LLVM one does, with identical results.

3. **The assertion suite passes natively.**
   ```sh
   python3 tests/mir_suite_test.py
   ```
   Expect all 4 modules (35 tests) green under BOTH backends. A native miscompile shows
   up here as a named failing test, which the differential run cannot give you.

4. **The real-world consumer still builds.**
   ```sh
   cd /mnt/projects/Projects/mirage303
   mirage build game -l raylib --std=/mnt/projects/Projects/Mirage/Mirage -o /tmp/Client
   ```
   ~2.3s, 90 files, LLVM backend. The only thing exercising `ext fn` struct ABI, function
   pointers, traits and `#link` together.

5. **Nothing is unlowered.**
   ```sh
   for d in examples/*/; do ./build/mirage build "$d" --emit-mir 2>&1 >/dev/null; done \
     | grep -c 'cannot lower'
   ```
   Should print **0**. If it does not, a construct regressed — `mirgen`'s coverage report
   is the instrument this project is steered by.

6. **Read the commit log** (`git log --oneline 2ece065..HEAD`). Each message explains
   *why*, not just what, and several document bugs found along the way — those
   explanations are the best available description of the traps in this codebase.

7. **Spot-check a claim.** The messages assert specific things: that `postfix ++` returns
   the old value, that `cast(3.14, u64)` converts rather than reinterprets, that an
   asm-referenced local is pinned against `promote_slots`. Verify a few directly with
   `--emit-mir --mir-opt`. If any claim is false, that is a bug and the first thing to
   report.

Report anything that fails validation *before* doing new work.

## 2. What was already done

Five original work items; four landed early, and the fifth — the backend — is now
complete through stage 5.

| Item | Status |
|---|---|
| Five-root module resolution; `MIRAGE_PATH` retired | done |
| `--nortti` and `$rtti_enabled` | done |
| `@no_discard` / `@export` / `@callconv` / `@cdecl` / `@import` | done |
| `@test`, `mirage test`, `--load` forced modules, `core/testing` | done |
| **Custom IR + native x86-64/wasm object generation** | **all ten stages done; LLVM deletion post-soak** |

`--backend=native` is a complete pipeline: sema → MIR (`mirgen.cpp`) → `promote_slots` +
`peephole` (`mir_passes.cpp`) → verify → x86-64 selection with the trivial allocator
(`backend_x86.cpp`) → machine code (`x86_encoder.cpp`) → a relocatable object
(`elf_writer.cpp`) → the same linker invocation the LLVM path uses. It matches LLVM on
all 74 positive corpus fixtures and runs the whole assertion suite. LLVM remains the
default and the only wasm path.

`TODO.md` has the per-increment log, including the ~17 silent miscompiles the
differential harness caught. Read `docs/backend.md` first regardless: it is the design
record, and its stage ordering exists for reasons that are easy to override without it.

Also done early: the Zed editor extension was **deleted** (unused). Do not re-add it. The
VS Code grammar stays and is drift-checked against the parser by
`tests/editor_grammar_test.py`.

## 3. What remains

### 3.1 Backend — stages 6 to 10

`docs/backend.md` is the authoritative design record and is CURRENT (status, stage list
and validation section all reflect reality). Do not deviate from its ordering without a
reason you can state.

**Stage 6 — linear-scan register allocation + a machine-level verifier.** The next step,
and the one most likely to harbour a subtle miscompile: code that runs and is wrong.

Today `backend_x86.cpp` emits straight from MIR to bytes with the TRIVIAL allocator —
every value in a frame slot, every instruction loading operands into fixed scratch
registers. Linear scan needs a machine-IR layer in between (virtual registers, then
assignment), which is a real refactor of that file rather than an addition to it.

Two things make that safe to attempt: the differential harness will tell you within one
run whether you broke anything, and the trivial allocator must SURVIVE as
`--regalloc=trivial`. That flag is not a legacy option — it is the standing triage tool
("if it also misbehaves under trivial, the bug is not in the allocator"), and the plan's
requirements are concrete: 14 allocatable GPRs (`rsp`/`rbp` reserved) and 16 XMMs as
separate classes, the caller/callee-saved split, fixed-register constraints (`div`/`idiv`
clobber `rdx:rax`, variable shifts need `cl`, the System V argument and return
registers), and spilling with interval splitting around calls.

**Stages 7–9 — wasm.** Dispatch loop before Relooper, standalone `.wasm` before the
relocatable form emscripten needs. Both orderings are load-bearing: a CFG-structuring bug
and a codegen bug look identical from outside, and you want one new variable at a time.
Two known sema-side prerequisites: `@import` (`TODO.md` §2.1) and `@export`'s wasm export
section (§2.2) are both validated-but-inert today, waiting on this stage; and
function-pointer ↔ `anyptr` casts must become a target-conditional error, because a wasm
funcref is a table index, not an address.

**Stage 10 — flip `--backend=native` to the default, soak, then delete `codegen.cpp` and
LLVM.** Note the revised decision: the deletion happens AFTER a soak period, because the
differential test is most valuable exactly when the new backend is newest.

### 3.2 Corpus migration (`TODO.md` §6.2)

**83** positive fixtures in `examples/` are pinned by exit code in `examples_expected.json`
(the other 188 entries are negative, compile-fail fixtures). Decision D7 says the positive
ones should become `@test` functions in `tests/mir/` with real assertions. Negative
fixtures stay in `examples/` — a program that does not compile cannot be a test function.
Four modules / 35 tests have migrated so far as the pattern.

The 83/188 split is worth internalizing before reading any coverage claim about this
corpus. "159 of 271 directories emit MIR cleanly" and "74 of 74 positive fixtures match
under both backends" describe the same tree: most of the 271 are compile-fail fixtures
that can never lower, and the ones that do lower but are not in the 74 are negative
fixtures whose *sema* stage fails later. Prefer the 74/74 figure — it is the one tied to
observable behaviour.

Delete each migrated fixture's baseline entry in the same commit; the harness reports
orphaned entries in both directions (`examples_smoke_test.py:243-246`).

### 3.3 CI has never run

`.github/workflows/ci.yml` was written without a runner to verify against. The YAML is
syntax-checked; nothing else about it is. Expect the first push to need adjustment.

---

## 4. Design invariants — do not violate these

These are why the code looks the way it does. Breaking them will look like it works and
fail later.

**Sema owns layout.** `resolved_type_size`/`resolved_type_align` and
`StructInfo::fields[].offset` come from the front end. No backend code computes layout;
every offset is read from sema. This is why MIR needs no structural type system.

**MIR values are machine scalars; aggregates live in memory.** An aggregate expression's
"value" IS its address. `default` is a memset, copy is a memcpy, an aggregate return goes
through a caller-owned sret pointer. Returning the address of a callee slot dangles — that
was a real bug, fixed by sret.

**MIR integers are sign-agnostic.** `I8` is eight bits, not `i8` or `u8`. The *source*
language type decides sext vs zext. Getting this wrong silently miscompiled every unsigned
widening (`cast(u8(200), i64)` produced −56). `coerce_to` takes `source_is_signed` for
exactly this reason — always pass it truthfully.

**Block parameters, not phi.** The front end emits memory form, so phi was needed in only
three places; block params are simpler to allocate registers for and to lower to wasm.

**Symbol mangling must match `codegen.cpp` exactly** (`__mir_<module>_<name>`,
`Type::method`, `Type::Trait::method`). Both backends coexist until stage 10, so a
program's symbols must not change when the flag flips: anything linking against a Mirage
object — C code, a second Mirage object, an `@export`ed name — would otherwise break on
a backend switch alone. (The differential harness compares exit codes and stdout, not
symbols; this invariant is not enforced by a test, which is exactly why it is written
down here.)

**Data-oriented, index-based, no templates/inheritance/variant-of-unique_ptr in MIR.** This
compiler gets rewritten in Mirage; the port should be a transliteration, not a redesign.

**Unsupported constructs are diagnosed by name, never skipped.** `mirgen`'s coverage report
is the instrument the work is steered by. It was fixed twice for being vague ("the
expression form at this position" ×472; "an unnamed statement" ×112). Keep it accurate — if
you add a construct, remove it from `expr_kind_name`/`stmt_kind_name`'s fallback.

**`mir::verify` runs on every lowering, but is skipped once anything is diagnosed.**
Emission bails out of what it cannot lower, so a module built in an error state is expected
to be incomplete; verifying it buries the real diagnostic under consequences.

---

## 5. Settled decisions (do not re-litigate)

Recorded in `docs/backend.md` §"Decisions already settled" as D1–D8. In brief:

- **D1** `@export` and `@callconv` stay orthogonal; `@export` does not imply `@cdecl`.
- **D2** Taking the address of a `@callconv("c")` function is an error in v1.
- **D3** Under `--nortti`, the `type_info_of` error does not fire in a statically-dead
  `when` branch — deliberately narrow.
- **D4** wasm imports are named by `@import("module", "name")` on `ext fn`.
- **D5** Emscripten is **retained** as a relocatable-object target.
- **D6** Search root 4 probes `<exe_dir>` and `<exe_dir>/../lib/mirage`.
- **D7** Negative fixtures keep `examples_expected.json`; only positive ones migrate.
- **D8** `MIRAGE_PATH` is dropped outright.
- **Revised this session:** delete `codegen.cpp`/LLVM only *after* a soak period with
  `--backend=native` as the default — the differential test is most valuable exactly when
  the new backend is newest.

Also pinned in `~/.claude` memory and honored throughout: `match`/`switch` arm loops stay
separate; `&` on a `const` yields a mutable pointer; `#compile_only_if`'s condition must
fold to `bool` (stricter than `when`); `ast::Module` stays per-file.

---

## 6. Working conventions

**Per issue: branch is already created (`fix/todo-resolution`) — work on it. For each
discrete change: implement → build (check exit status) → probe with a hand-written program
→ `just test` → add tests → commit.** Do not batch unrelated changes into one commit.

**Commit messages explain WHY.** Look at the existing 22 for the register. State the
problem, the mechanism, the reasoning behind non-obvious choices, and any bug found along
the way. End with:
```
Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
```

**Baseline discipline.** `tests/examples_expected.json` is re-blessed with
`python3 tests/examples_smoke_test.py --update-baseline` **in the same commit as the fix
that changed behavior** — never as a standalone commit — and the JSON hunk is reviewed like
any other diff.

**Tests.** C++ unit tests go in ctest; everything else is a `tests/*_test.py` run directly.
`mirgen_test.py` has an "unsupported construct" probe that is deliberately pointed at
something *not yet lowered* — when you lower it, repoint the probe rather than deleting the
test. The property under test is that mirgen refuses loudly.

**Docs are part of the change.** `docs/spec.md` and `docs/grammar.md` are updated alongside
any language-visible change. spec.md sections are appended, never renumbered, so existing
anchor links keep working.

---

## 7. Traps that have already bitten

Each of these was a real bug found during the work. They are the shapes to watch for.

1. **`cmake --build` exit status, not grep** — see §0.
2. **A template's `return_types` is never populated.** Any check reading the return shape
   must skip generics or it reads "returns nothing" and reports nonsense.
3. **A generic type's methods are never signature-resolved** (`is_resolved` stays false),
   so passes gated on `is_resolved` skip them entirely. `@export` there was neither
   validated nor honored.
4. **Assignment targets have no recorded `expr_type`.** sema records types for values; a
   target is not one. `mirgen::lvalue_type` derives it from the target's shape and must
   mirror `emit_address` exactly.
5. **`validate_hosted_main` accepted declarations codegen never emits** (a generic `main`, a
   `@test` `main`), and `functions_.at()` then aborted with no diagnostic. `codegen.cpp` has
   ~189 `.at()` calls; any new driver action can reach a fresh one.
6. **`continue` must target a `for` loop's STEP block, not its condition** — otherwise the
   index never advances. Invisible to any test that does not actually iterate.
7. **Diagnostics that report through an `unordered_map` walk are nondeterministic.** The
   `@export` collision check sorts by source position before reporting for this reason.
   Other whole-program passes may still have this.
8. **`!ptr` and pointer conditions are null comparisons**, not integer ones. Building a
   `const.int` typed `ptr` is ill-typed; the MIR verifier caught it.
9. **Block labels repeat** (every nested `if` produces `if.end`), so MIR block references
   carry the block index.

---

### 7b. Traps the native backend found

These are different in kind from the list above: every one was a lowering that was
type-correct, passed `mir::verify`, and produced WRONG CODE. None was visible to any
suite until `--backend=native` actually executed the result — which is the entire
argument for having written the differential harness before the backend.

If you touch `mirgen.cpp`, these are the shapes to re-check:

1. **Size an sret slot from the CALLEE's sema return list, never `expr_type`.** A
   multi-return call expression has no recorded type at all.
2. **Sema's per-call side tables are keyed TWO ways** — by the `Expr` variant-slot
   address for a value call, by the `CallExpr` address for group/forwarded/`try` calls.
   Consult both, and route group declarations AROUND the value-position dropped-error
   wrapper or it destructures the blob before the names can.
3. **Aggregates bind BY VALUE**: parameters, `for-in` bindings and `any` casts must copy
   bytes, never store the source address, because every reader treats an aggregate
   local's slot as holding the aggregate.
4. **An indirect callee needs `emit_address` + load**, not `emit_expr`: sema records no
   type for a call's callee, so a member-expression callee falls into the aggregate path
   and yields the field's address.
5. **`!x` must route through `emit_condition`** — the one place that knows every
   truthiness shape, including an error value's Ok/Failed tag.
6. **Postfix `++`/`--` returns the OLD value.** `mem.mir`'s `d++.* = s++.*` depends on it.
7. **int ↔ float casts are conversions, not `Bitcast`**, and float→int reads the TARGET's
   signedness to pick the rounding form.
8. **Global initializers must actually be emitted** — scalar and aggregate alike. Zeroing
   them instead is silent: `const alignment := 8` became 0 and every allocation divided
   by zero.
9. **Trait-impl methods live only in `trait_impls_by_type`**, never `ProgramModule::methods`,
   and their defaulted arguments live on the TRAIT's method declaration.
10. **`Op::Asm`'s `a` is a block index, not a value.** A pass that treats it as one hands
    itself a bogus `ValueId` and lets `promote_slots` believe an asm-referenced slot is
    unused.
11. **Test mode needs its own synthesized entry.** Without it, `mirage test` links a
    module with no `main` and no `_start` and jumps into `argc`.

---

## 8. Definition of done

- Every item in `TODO.md` resolved or explicitly re-scoped with a stated reason.
- `just test` green; `mirage303` builds and runs. — HOLDS TODAY.
- Backend: the corpus compiles and runs correctly under `--backend=native`,
  differential-tested against `--backend=llvm`. — HOLDS TODAY for x86-64: 74/74 fixtures
  match and the assertion suite passes both ways. The harness was written before the
  backend, as this line demanded, and that is why every miscompile in §7b was found
  within minutes of being written rather than months later.
- `docs/spec.md`, `docs/grammar.md`, `docs/backend.md` and `README.md` current. —
  `backend.md` and `README.md` were brought current with the native backend; spec and
  grammar were never invalidated by it (nothing language-visible changed).
- `TODO.md` and this file deleted once genuinely complete. — NOT YET: the soak period
  and the post-soak LLVM deletion remain,
  and `TODO.md` §2.1/§2.2 (wasm-blocked attributes) and §6.2 (corpus migration, deferred
  by decision) are still open.

**If you cannot finish, say so plainly and leave `TODO.md` accurate.** An honest inventory
with measured scope is worth more than an optimistic one — the previous sessions ended that
way deliberately, and it is why this handoff is possible.
