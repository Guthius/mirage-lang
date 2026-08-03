# Backend: replacing LLVM

The compiler currently generates code through LLVM (`src/compiler/codegen.cpp`, ~6,900
lines, plus the object-emission and target-selection code in `src/main.cpp`). It is being
replaced by a Mirage-specific IR and native object generation for `x86_64` and `wasm`, so
that the compiler is standalone — no LLVM, no external toolchain beyond a linker.

**Status (2026-08-03).** Stages 1–7 are done. `--backend=native` is a complete pipeline —
sema → MIR (`mirgen.cpp`) → `promote_slots` + `peephole` (`mir_passes.cpp`) → verify →
then per target: x86-64 through linear-scan register allocation (`x86_regalloc.cpp`,
with its machine-level interference verifier) → emission (`backend_x86.cpp`) → machine
code (`x86_encoder.cpp`) → a relocatable object (`elf_writer.cpp`) → the same linker
invocation the LLVM path uses; or (`--target=wasm32-unknown-unknown`) a finished
standalone `.wasm` module (`backend_wasm.cpp` → `wasm_encoder.cpp`) written straight to
the output — whole-program compilation leaves nothing for a linker to do there.

**`tests/backend_differential_test.py` reports 74 of 74 positive corpus fixtures
producing identical exit codes and stdout under both backends — with the native side run
under BOTH register allocators** — and `tests/wasm_differential_test.py` reports 64 of
those 74 producing identical results between native x86-64 and native wasm under node,
with the other 10 refused BY NAME (the nine inline-asm/`@naked` fixtures, which are
x86-only by definition, and one exercising the funcptr↔anyptr cast that is now a
target-conditional sema error on wasm). `--regalloc=trivial` keeps the stage-4/5
discipline alive as the standing triage tool.

LLVM remains the default (`--backend=llvm`) and the only path for wasm-emscripten. What
remains is stages 8–10 below: the relocatable wasm form, the Relooper, and the
flip-and-delete.

The per-increment history, including the fifteen silent miscompiles the differential
harness caught, is in `TODO.md`.

This document is the design record for that work. It exists because the reasoning behind
the IR's shape is not recoverable from the code, and because the remaining stages have a
sequencing that matters.

---

## Why the IR looks the way it does

Two facts about this compiler decide almost every choice in `mir.hpp`, and neither is
obvious from outside.

**Sema already owns layout.** `sema::resolved_type_size` / `resolved_type_align` and
`StructInfo::fields[].offset` are computed in the front end from `Options::pointer_size`;
`codegen::size_of`/`align_of` are thin forwarders into sema, and no data-layout query
influences a single layout decision. So MIR needs no structural type system and no layout
engine — a type is a size, an alignment and a scalar kind. This is why `mir::Ty` is eight
scalars, and why there is no `getelementptr`: every address computation is plain pointer
arithmetic (`ptr.add.const` / `ptr.add`), which is a genuine simplification rather than a
workaround.

**The front end emits memory form, not SSA.** Every local is a stack slot written and read
through load/store; the LLVM emitter needed `phi` in exactly three places. So no SSA
construction is required, and a straightforward register allocator suffices. Those three
cases use **block parameters** instead of phi — simpler to allocate registers for, and
closer to what wasm's structured control flow wants.

**Aggregates are memory; values are scalars.** A MIR value is always one machine scalar.
This keeps instruction selection, register allocation and the wasm encoder simple, and it
costs nothing in expressiveness given the layout fact above.

**Coding style is a constraint, not a preference.** This compiler is going to be rewritten
in Mirage, so MIR is data-oriented and index-based — flat vectors with `u32` handles, no
inheritance, no variant-of-`unique_ptr`, no templates — to make that port a
transliteration rather than a redesign. `sema::Program`'s parallel arenas are the model.

**The builder mirrors `llvm::IRBuilder`'s method surface deliberately**, so porting the
6,900-line emitter is a mechanical rename rather than a rewrite. The three places where it
*cannot* be mechanical — GEP, aggregates, and inline asm — are exactly the three worth
concentrating review on.

### What the LLVM path actually uses

Worth knowing before porting, because it is much less than LLVM offers: about 70 distinct
`IRBuilder` operations across all 6,900 lines — scalar arithmetic, load/store, GEP,
insert/extractvalue, branches, calls, inline asm. No vector types except `<2 x float>`
(produced solely by System V eightbyte classification), no atomics, no intrinsics beyond
`memcpy`/`memset`, **no debug info at all** (zero `DIBuilder` references — DWARF is not a
porting obligation), and no IR optimization passes are ever run.

---

## Stages

Each is independently verifiable. Do not start the next until the current one is green.
Stages 2–5 are DONE; their entries below are kept as the design record (the reasoning is
what future changes must not contradict), annotated with how they actually landed.

**2. Port `codegen.cpp` → `mirgen.cpp`** against `mir::Builder` — DONE. Validated by
*reading* MIR for the corpus, then (from stage 4 on) by running it. The
mechanical-but-large step, and the one that produced the most latent bugs: a lowering can
be type-correct, pass the verifier, and still be wrong, which is why the differential
harness exists.

The one part that is not mechanical is aggregates. `codegen.cpp` uses
`insertvalue`/`extractvalue` on first-class aggregates in ~67 places (struct literals,
slices, trait handles, `any`, error unions, multi-return). Mitigate with a value wrapper in
*mirgen*, not in MIR: an aggregate-typed `Val` carries the slot's base pointer, and
`insert_value(agg, offset, scalar)` mutates in place when the slot was created by this
builder and has not yet been read, otherwise copies first. The in-place fast path covers
the whole `null → insert → insert → insert` construction pattern that produces nearly all
of those sites; the copy path is slower but always correct.

**3. `promote_slots` + peephole** — DONE (`mir_passes.cpp`; `--mir-opt` runs both).
`promote_slots` is a mem2reg-lite: a slot whose address
never escapes, accessed only by full-width loads/stores of one scalar type, becomes a
value. Because the front end puts *every* local in a slot, this is what recovers most of
what LLVM's -O0 pipeline was giving for free. Target-independent, benefits both backends.
`Slot::address_escapes` already exists for it.

**4. x86-64: legalize → ISel → *trivial* regalloc → frame layout → encoder → ELF.** — DONE.
Built bottom-up: encoder first (byte-checked against GNU `as`), then the ELF writer, then
selection. The trivial allocator's discipline is one paragraph — every MIR value owns an
8-byte frame slot; every instruction loads its operands into fixed scratch registers and
spills its result — and block parameters get a staging slot plus a canonical slot, which
makes the swap/rotation hazard impossible by construction rather than by analysis.

Build **trivial** register allocation first — spill every value to a frame slot, reload
around every use. It is slow and enormous but almost impossible to get wrong, and it lets
ISel, frame layout, the encoder and the ELF writer all be validated end-to-end before a
single line of linear scan exists.

**5. Inline asm through that encoder.** — DONE, and the prediction below held exactly:
owning the allocator removed the constraint model entirely. A variable operand is simply
its frame slot, so `mov &fd, eax` encodes as `mov [rbp-off], eax` with no marshalling.
MIR carries a resolved `AsmBlock`; the encoder gained the memory-operand forms
hand-written asm needs and refuses anything else BY NAME.

Without LLVM there is no integrated assembler.
Fortunately the accepted language is already tiny and enumerated: the 30 Tier-1 mnemonics
(`sema_check.cpp`'s `asm_tier1_directions`), the 64 GPRs in `asm_registers.hpp`, and
register/immediate/variable/simple-memory operands — SSE, x87, segment and control
registers are already rejected as "not supported in v1". Only one stdlib file uses `asm`
(`core/sys/linux/syscalls.mir`).

This also *simplifies* the operand model: today operands are LLVM constraint strings and
LLVM allocates. With our own allocator an asm block becomes a machine instruction with
pre-coloured operands and an explicit clobber set — which sema already computes. The three
compiler-internal asm blobs (freestanding `exit` and `write` syscalls, `_start`'s stack
realign) stop being asm at all and become machine instructions directly.

**6. Linear-scan register allocator + machine verifier.** — DONE (`x86_regalloc.cpp`).
Differential-tested against trivial on every harness run. The hard requirements all hold:
14 allocatable GPRs (`rsp`/`rbp` reserved) and 16 XMMs as separate classes;
caller/callee-saved split; fixed-register constraints (`div`/`idiv` clobber `rdx:rax`,
variable shifts need `cl`, System V argument and return registers); spilling with
save/restore splitting around calls.

How it actually landed, and the one deliberate deviation: there is NO separate machine
IR. MIR is already SSA-shaped (one def per value, block parameters instead of phi), so
MIR values ARE the virtual registers, and the allocator assigns each an interval-long
location — register or 8-byte frame area — that ONE emission engine in `backend_x86.cpp`
reads operands from. `--regalloc=trivial` is the degenerate assignment (everything
spilled) through the SAME templates, which is what keeps it meaningful as a triage tool:
a bug that reproduces under trivial is in the shared engine, one that does not is in the
allocator. Fixed-register needs are modelled as per-position KILL RANGES the allocator
must route around; call clobbers are kill ranges an interval may overlap only with
`save_around_calls` — the emitter parks it in its save area across exactly the calls it
crosses, which is this design's interval-splitting form. Intervals are conservative
single ranges `[start, end]`; holes cost register pressure, never correctness.

The predicted subtle-miscompile risk was real, twice, both caught by the differential
harness within one run: a liveness bug (values defined mid-block stretched back to block
start, letting a call result "cross" its own defining call and be clobbered by its own
save-around restore), and an ENCODER bug the trivial allocator could never reach ('xor
sil, 1' encoding as 'xor dh, 1' — byte ops on SPL/BPL/SIL/DIL need a forced REX prefix,
and the trivial scratch set RAX/RCX/RDX never touches those encodings). The machine
verifier re-checks pairwise interference and kill-range violations after every
allocation and aborts the compile on any finding; `tests/x86_regalloc_test.cpp` pins the
constraint properties in ctest.

**7. wasm, standalone.** — DONE (`wasm_encoder.cpp` + `backend_wasm.cpp`), exactly as
planned below: the dispatch loop shipped first (Relooper deferred to stage 9), every MIR
value is a typed wasm local, aggregates live on a `__stack_pointer` shadow stack, a
function pointer is a funcref table index (slot 0 reserved so null traps), and
global-initializer relocations resolve at layout time — function targets to table
indices, global targets to absolute addresses — because the final `.wasm` is emitted
directly with nothing left for a linker. `@import` now binds import module/name
(closing TODO §2.1) and External linkage exports every `@export`ed definition (§2.2);
the funcptr↔anyptr cast became the promised target-conditional sema error. Two
things the plan did not predict: C-variadic imports (printf) are reachable after all —
the tail spills to a shadow-stack buffer passed as one trailing pointer, emscripten's
own convention, so stage 8 will link real libc unchanged — and the first 32-bit
lowering exposed a latent mirgen bug (the `any` blob's data word was placed at
pointer_bytes() instead of 8, overwriting the u64 id's upper half; invisible on every
64-bit run). Semantics that differ between wasm and x86 by instruction-set accident
(shift counts mod 32 vs 64, `i32.div_s` trapping on INT32_MIN/-1, `f.ne` being
unordered, trapping float→int) are widened to i64 or recomposed so both backends
compute identical bits; `tests/wasm_differential_test.py` runs the corpus under node
(`tests/wasm_host.js` is the minimal embedder: write/exit/sbrk/malloc-family/printf-
family/file handles) against native x86-64.

The original sizing rationale, kept as the design record:

*Easier:* no register allocation at all — wasm functions have unlimited typed locals, so
each MIR value becomes a local. And opcodes are one or two bytes with LEB128 immediates.

*Harder:* wasm has `block`/`loop`/`if`/`br`/`br_table` and **no** `goto`, while MIR has an
arbitrary CFG. Ship the **dispatch loop** first: wrap the function in
`loop { block { … br_table $state } }`, one arm per basic block, every branch assigning
`$state`. It works for any CFG including irreducible ones, is ~400 lines, and is trivially
checkable. Do *not* start with Relooper — a CFG-structuring bug and a codegen bug look
identical from outside, and you want one new variable at a time.

Also needed: a shadow stack (a mutable `__stack_pointer` global, since wasm's operand stack
is not addressable, for any slot that survives `promote_slots`); data segments for
`.rodata`/`.data` with `.bss` needing none (linear memory is zero-initialized); and a
`funcref` table, because **a wasm function reference is a table index, not an address**.
That last one has consequences: `call_indirect` needs a *type index* (hence
`Module::signatures` interning), trait vtables become constant arrays of `i32` table
indices rather than pointers, and casting between a function pointer and `anyptr` cannot
work on wasm. Make that a target-conditional sema error rather than a silent miscompile,
and record it in the spec — it is a real semantic difference between the two targets.

Target `wasm32-unknown-unknown` first: no libc, all host interaction via `ext fn` imports
(`@import` already names the import module), so the backend is the only variable. Then
`wasm32-wasi`, which needs a `core/sys/wasi/` stdlib backend beside the existing
`core/sys/linux/` — roughly the same surface, and mechanically simpler since every WASI
call is an ordinary import rather than inline asm.

Because Mirage compiles whole-program into a single object, emit the **final `.wasm`
directly**: no relocatable-object format, no linking section, no `wasm-ld`.

**8. wasm, relocatable** (for emscripten). — DONE, exactly as the sizing predicted: a
second output shape over the SAME encoder (`wasm::ObjectModule` beside `wasm::Module`,
one serializer each; the instruction translation in `backend_wasm.cpp` is shared, with
an object-mode flag at the six emission points where an address or index becomes a
relocation). Memory and the funcref table are imports (`env.__linear_memory`,
`env.__indirect_function_table`), the shadow-stack pointer is the imported global
`env.__stack_pointer`, and layout belongs to `wasm-ld`. The section encodings were
decoded from a reference object emcc's own clang produced rather than recalled from the
spec — which is why the first link attempt succeeded. The entry glue defines the C
`main(argc, argv)` emscripten's runtime calls (the user's main keeps its body under
`__original_main`), and libc symbols — printf, malloc, write, fmod — resolve at link
time against emscripten's real libc, which is also what vindicated lowering C-variadic
tails via emscripten's buffer convention in stage 7. `tests/wasm_emscripten_test.py`
sweeps the corpus (62 of 74 matching the native x86-64 build through the full emcc
link; the others are the named refusals plus two fixtures that read the host
filesystem, which MEMFS does not have) and links the ext-fn ABI fixture against
emcc-compiled C — the wasm C ABI checked against the other compiler's idea of it.

Built *second*, deliberately: attempting it alongside stage 7 means debugging CFG
structuring and relocation bugs through the same symptom.

**9. Relooper.** Recover natural `if`/`loop`/`block` structure for reducible regions
(essentially all of them, since Mirage has no `goto`), falling back to dispatch for
anything that resists. Purely an optimization; ship stages 7–8 without it.

**10. Flip the default** to `--backend=native`, run everything, delete `codegen.cpp` and
the LLVM dependency. `emcc` stays in the matrix as a linker for stage 8. Removing
`find_package(LLVM)` should collapse build time and dependency footprint measurably — worth
recording.

---

## Driver changes

- `--backend=llvm|native`, defaulting to `llvm` until stage 10. Keeping both alive is what
  makes differential testing possible; it is the primary validation mechanism for the
  whole effort.
- `--emit-mir` replaces `--emit-ir` on the native path. Eight corpus fixtures pin
  `emit-ir`; re-baseline them when the flag lands.
- `--emit-asm` (native x86-64, debug): textual assembly from machine IR. The fastest way to
  eyeball a suspected encoder bug.
- `--regalloc=trivial|linear` (debug), per stage 6.
- Target selection stops going through `llvm::Triple`. Replace with
  `Target { X86_64_Linux, Wasm32_Unknown, Wasm32_Wasi, Wasm32_Emscripten }` and a parser for
  the `--target=` spellings currently accepted.

  **Keep the `$option` value strings byte-identical** (`"Linux"`, `"Wasm32"`, `"X86_64"`, …).
  `default_target_os`/`default_target_arch` feed `build/target_os` and `build/target_arch`,
  which every `#compile_only_if` in the standard library switches on; drifting them would
  silently change which platform files are included. Pin them in a test.

  Note `Wasm32_Unknown` (the *target* `wasm32-unknown-unknown`) is a different axis from the
  existing `--freestanding` flag (which means "no Mirage stdlib, hand-written `_start`").
  Keep the two spellings distinct in help text and diagnostics.

---

## Validation

This is what determines whether the effort succeeds.

1. **Differential compilation** — DONE, and it paid for itself immediately.
   `tests/backend_differential_test.py` builds (and runs) every positive fixture under
   both backends and compares exit code and stdout; `examples_smoke_test.py` also takes
   `--backend`. Written BEFORE the backend, exactly as this list demanded, which is why
   the very first native run had a harness waiting for it. It has since caught fifteen
   silent miscompiles — every one type-correct, verifier-clean, and invisible to all 27
   other suites, because nothing else ever EXECUTED the lowered code.

   Its report is deliberately four-way: match, mismatch, awaiting-stage-4, and
   refused-by-name. The last two are tolerated but COUNTED and listed, so the suite is
   green while incomplete yet any regression in coverage reads as one.
2. **`mirage test`** on both backends — DONE. `tests/mir/` is a real assertion-carrying
   suite rather than an exit-code comparison, which is exactly what a backend swap needs:
   a native miscompile surfaces as a NAMED failing test, where the differential harness
   can only report a diverging exit code. `mir_suite_test.py` runs every module under
   both backends; all 35 tests pass under each. Test mode needs its own synthesized
   entry (wrappers + `Test_Info` + `_run_tests`), which is why running it caught that
   `--backend=native` had been emitting a crashing test binary.
3. **Encoder differential** — DONE. `tests/x86_encoder_test.cpp` pins every emitter's
   bytes and, with `--dump`, emits each form's AT&T spelling beside them;
   `tests/x86_encoder_differential_test.py` assembles those with `as` and compares.
   Where the two differ, the comparison is SEMANTIC rather than whitelisted: both byte
   sequences are disassembled by `objdump` and their instruction text compared, so this
   encoder's deliberate uniform disp32/imm32 forms pass while a wrong opcode, register
   or displacement fails. Verified to work by injecting a swapped ModRM reg/rm field:
   21 of 40 instructions failed. Skips cleanly when binutils is absent.
4. **Machine-level verifier** after register allocation, checking interference.
5. **`mirage303`** (90 files, ~2s, links raylib) as the integration smoke test. It
   exercises `ext fn` struct ABI, function pointers, traits and `#link` together, which no
   fixture does.
6. **`--regalloc=trivial` bisection** as the standing triage tool.

---

## Standalone wasm without emscripten

The current emscripten dependency is not fundamental. It exists because LLVM emits a
*relocatable* wasm object that something else must link, and because the wasm stdlib was
written against emscripten's libc.

| What emscripten supplies | Replacement | Cost |
|---|---|---|
| Linking | Emit the final `.wasm` directly — whole-program compile, nothing to link | Free; *less* work than a relocatable object |
| Memory, `__stack_pointer`, data segments, `__heap_base` | We own the module; declare them | ~100 lines |
| Entry glue | Export `_start` (WASI) or `main`; optionally emit a small `.js`/`.html` loader | Small |
| libc | `wasm32-unknown-unknown` needs none; `wasm32-wasi` needs a `core/sys/wasi/` backend | Real, but stdlib work rather than compiler work |
| SDL/GL/filesystem ports | **Not replaced** | See below |

**What is genuinely lost:** prebuilt C library ports. raylib's web build depends on
emscripten's GL/SDL/HTML5 glue and its asyncify main-loop transform. Nothing here
reproduces that, and nothing should try — which is why stage 8 keeps the emscripten target
alive rather than dropping it. `mirage303`-on-web needs `emcc` for as long as it needs
raylib.

`setjmp`/`longjmp`, C++ exceptions, dynamic linking and threads are also unavailable —
none of which Mirage has.

---

## Decisions already settled

Recorded so they are not silently re-litigated. Each is implemented and documented in
`spec.md` unless marked as pending.

| # | Decision |
|---|---|
| **D1** | `@export` and `@callconv` stay **orthogonal**; `@export` does not imply `@cdecl`. An exported symbol is also how two separately-compiled Mirage objects will find each other, and that path must keep the Mirage ABI. A warning covers the easy mistake (aggregate-by-value in an `@export` signature with no `@cdecl`). |
| **D2** | Taking the **address of a `@callconv("c")` function is an error** in v1; function-pointer types carry no convention. Direct calls remain legal. Lifting this means adding a convention field to function types and to §15's structural-equality rule. |
| **D3** | Under `--nortti`, the `type_info_of` error **does not fire in a statically-dead `when` branch**. Deliberately narrow: one diagnostic, one flag, and it must also skip `type_ids` registration or the flag fails to shrink the binary. |
| **D4** | wasm import modules are named by `@import("module", "name")` on `ext fn`, defaulting to `("env", <decl name>)`. |
| **D5** | **Emscripten is retained** as a relocatable-object target after `codegen.cpp` is deleted, preserving the raylib web build — hence stage 8. |
| **D6** | Search root 4 probes both `<exe_dir>` and `<exe_dir>/../lib/mirage`. |
| **D7** | Negative compile-fail fixtures **keep `examples_expected.json`**; only positive fixtures migrate to `@test` (`tests/mir/`). A program that does not compile cannot be a test function, so `@test` cannot express them. |
| **D8** | **`MIRAGE_PATH` is dropped outright** — not consulted, no fallback. A note appears on an unresolved import when it is set and `MIRAGE_MODULES_ROOT` is not, purely for discoverability. |
| **pending** | Function-pointer ↔ `anyptr` casts must become a target-conditional sema error on wasm (stage 7). |

---

## Rough size

MIR core (done) ~1.5k; passes ~0.5k; x86 legalize/ISel ~1.5k; regalloc ~1.2k; frame +
encoder ~2.5k; ELF ~0.8k; wasm standalone ~2k; wasm relocatable ~0.9k; driver rework ~0.4k.
Call it **11–13k lines of new code**, plus rewriting `codegen.cpp`'s 6.9k into `mirgen.cpp`
— comparable in scale to the existing sema layer.
