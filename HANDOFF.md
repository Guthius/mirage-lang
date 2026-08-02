# Handoff: finish the Mirage self-hosting prerequisites

You are picking up an in-progress body of work on the Mirage compiler. This document is the
complete brief: what was done, what remains, how to verify it, and the conventions and
traps that matter.

**Read `TODO.md` and `docs/backend.md` before writing any code.** This file tells you *how*
to work on the repo; those two tell you *what* is left and *why the design is the way it
is*. Neither is optional.

---

## 0. Repository layout

Three repositories, all on disk as siblings:

| Path | What it is |
|---|---|
| `/mnt/projects/Projects/Mirage/Mirage-Cpp` | The compiler (C++23). Branch `fix/todo-resolution`. |
| `/mnt/projects/Projects/Mirage/Mirage` | The standard library (Mirage source). |
| `/mnt/projects/Projects/mirage303` | A 90-file VB6→Mirage game port; the real-world consumer. |

The compiler is `mirec`/`mirage`: lexer → parser → module resolver → multi-phase sema →
codegen. `mirage_core` (everything except `codegen.cpp` and `main.cpp`) links no LLVM;
`mirage-lsp` links only `mirage_core`.

### Build and test

```sh
cd /mnt/projects/Projects/Mirage/Mirage-Cpp
just build            # configure + build → build/mirage, build/mirage-lsp
just test             # ctest (6 targets) + every tests/*_test.py
```

`just test` defaults `MIRAGE_STD` to `../Mirage`; override it if your stdlib is elsewhere.
It runs every suite even after one fails and lists them all at the end.

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
   Expect ctest (6) plus 26 Python suites, all passing.

2. **The real-world consumer still builds.**
   ```sh
   cd /mnt/projects/Projects/mirage303
   mirage build game -l raylib --std=/mnt/projects/Projects/Mirage/Mirage -o /tmp/Client
   ```
   ~2.3s, 90 files. This is the only thing exercising `ext fn` struct ABI, function
   pointers, traits and `#link` together.

3. **Backend coverage is where `TODO.md` claims.**
   ```sh
   ok=0; for d in examples/*/; do
     ./build/mirage build "$d" --emit-mir >/dev/null 2>&1 && ok=$((ok+1)); done
   echo "$ok / $(ls -d examples/*/ | wc -l)"
   ```
   Should print **32 / 271**. If it differs, find out why before proceeding.

4. **Read the coverage report** — this is your work queue for the rest of stage 2:
   ```sh
   for d in examples/*/; do ./build/mirage build "$d" --emit-mir 2>&1 >/dev/null; done \
     | grep -oE 'cannot lower .* yet' | sed 's/cannot lower //; s/ yet//' \
     | sort | uniq -c | sort -rn
   ```

5. **Review the 22 commits on the branch** (`git log --oneline 2ece065..HEAD`). Each commit
   message explains *why*, not just what. Several document bugs that were found and fixed;
   those explanations are the best available description of the traps in this codebase.

6. **Spot-check the claims.** The commit messages assert specific things — that `@export` on
   a generic type's method is rejected, that `cast(u8(200), i64)` zero-extends, that `when`
   emits only the live branch. Verify a few directly. If any claim is false, that is a bug
   to fix and the first thing to report.

Report anything that fails validation *before* doing new work.

---

## 2. What was already done

Five original work items, of which four are complete and one (the backend) is partial.

| Item | Status |
|---|---|
| Five-root module resolution; `MIRAGE_PATH` retired | ✅ done |
| `--nortti` and `$rtti_enabled` | ✅ done |
| `@no_discard` / `@export` / `@callconv` / `@cdecl` / `@import` | ✅ done |
| `@test`, `mirage test`, `--load` forced modules, `core/testing` | ✅ done |
| **Custom IR + native x86-64/wasm object generation** | ⚠️ **stage 2 of 10, partial** |

Plus a follow-up review pass (`TODO.md`) whose sections 1, 4, 5, 6.1 and 7 are fully
resolved, and two features added from it (`@export` on globals, `@no_discard` on trait
methods).

Also done: the Zed editor extension was **deleted** (unused). Do not re-add it. The
VS Code grammar stays and is drift-checked against the parser by
`tests/editor_grammar_test.py`.

---

## 3. What remains

### 3.1 Backend — the large item

`docs/backend.md` is the authoritative design record. It explains stages 2–10, the
sequencing and **why the ordering matters**. Do not deviate from that ordering without a
reason you can state.

**Stage 2 (in progress) — finish lowering sema to MIR in `src/compiler/mirgen.cpp`.**

Already lowered: scalar functions, `ext fn`, globals, locals as slots, arithmetic with
correct signedness, comparisons, conversions, casts, assignment, lvalues (struct fields,
indexing, pointer auto-deref, address-of), aggregates as memory (memset/memcpy), string
literals, `len`, direct + cross-module + method + indirect calls, braced initializers,
`if`/`while`/`for-in`/`switch`/`when`, `break`/`continue`, slice expressions, sret returns,
`return_ok`/`return_err`, enum variants, `type_of`, character literals.

Remaining, ranked by the coverage report (recheck — this list is from the last measurement):

1. **Multi-return** (~99 occurrences across three spellings: `-> (T, error(E))` returns,
   `return_ok` with value slots, and group declarations `const a, b := f()`). One lowering
   covers all three. Highest value by a wide margin. The sret machinery already exists —
   `returns_via_sret()` and the sret parameter binding — so this is mostly writing the
   multiple values into the sret slot at their offsets and destructuring at the call site.
2. **Tagged-union `switch`/`match`** (~80). Switch on the u32 tag at offset 0; payload
   captures read from `UnionInfo::payload_offset`.
3. **`try`** (~31). Error propagation as control flow: check the tag, and on failure copy
   the error into the caller's own error slot and return early.
4. **Trait-handle method calls** (~26). Indirect call through a vtable slot. Needs vtable
   globals, which need global initializers with relocations (`mir::Relocation` exists).
5. Then: `defer` (scope tracking, emitted at every exit path — including `return`, `break`,
   `continue`), generic instantiations, inline `asm`, global initializers, `match`
   expressions.

**Stages 3–10** — see `docs/backend.md`. Summary: `promote_slots` + peephole; x86-64
(legalize → ISel → **trivial** regalloc → frame → encoder → ELF); inline asm through that
encoder; linear-scan regalloc + machine verifier; wasm standalone; wasm relocatable
(emscripten); flip `--backend=native`, soak, then delete `codegen.cpp` and LLVM.

Two orderings in that document are load-bearing and easy to get wrong:
- **Build trivial register allocation before linear scan.** It validates ISel, the frame
  layout, the encoder and the ELF writer end to end before the allocator exists, and it
  stays permanently as the triage tool ("if it also misbehaves under `--regalloc=trivial`,
  the bug is not in the allocator").
- **Build the wasm dispatch loop before Relooper**, and the standalone `.wasm` writer
  before the relocatable one. A CFG-structuring bug and a codegen bug look identical from
  outside; you want one new variable at a time.

### 3.2 Corpus migration (`TODO.md` §6.2)

~145 positive fixtures in `examples/` are pinned by exit code in `examples_expected.json`.
Decision D7 says they should become `@test` functions in `tests/mir/` with real assertions.
Negative (compile-fail) fixtures stay in `examples/` — a program that does not compile
cannot be a test function. Four modules / 35 tests have migrated so far as the pattern.

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
`Type::method`, `Type::Trait::method`). Both backends coexist until stage 10, and the
differential test compares their symbols directly.

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

## 8. Definition of done

- Every item in `TODO.md` resolved or explicitly re-scoped with a stated reason.
- `just test` green; `mirage303` builds and runs.
- Backend: the corpus compiles and runs correctly under `--backend=native`, differential-
  tested against `--backend=llvm` (build every fixture both ways, compare exit code and
  stdout — write that harness *before* the x86-64 backend, it is the primary safety net).
- `docs/spec.md`, `docs/grammar.md`, `docs/backend.md` and `README.md` current.
- `TODO.md` and this file deleted once genuinely complete.

**If you cannot finish, say so plainly and leave `TODO.md` accurate.** An honest inventory
with measured scope is worth more than an optimistic one — the previous sessions ended that
way deliberately, and it is why this handoff is possible.
