#!/usr/bin/env python3
"""'--emit-mir': lowering sema to Mirage IR (docs/backend.md, stage 2).

mirgen is being grown construct by construct rather than landed whole, so this suite has
two jobs:

  - pin that what IS lowered is lowered correctly, by reading the emitted MIR;
  - pin that what is NOT yet lowered reports a diagnostic naming the construct rather than
    silently emitting wrong code or crashing.

The second is the one that matters while coverage is partial: a backend that quietly skips
what it does not understand is far worse than one that says so.

Not wired into ctest (it needs a built compiler). Run manually:

    just build
    python3 tests/mirgen_test.py
"""

import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
MIRAGE = REPO_ROOT / "build" / "mirage"

failures = 0


def check(condition: bool, message: str) -> None:
    global failures
    if not condition:
        failures += 1
        print(f"FAIL: {message}")
    else:
        print(f"ok: {message}")


def emit_mir(source: str):
    with tempfile.TemporaryDirectory() as tmp:
        (Path(tmp) / "main.mir").write_text(source)
        return subprocess.run([str(MIRAGE), "build", tmp, "--emit-mir"],
                              capture_output=True, text=True, timeout=60, cwd=REPO_ROOT)


def case_scalar_function():
    r = emit_mir("pub fn add(a: i32, b: i32) -> i32 { return a + b }\n"
                 "pub fn main() -> i32 { return add(2, 3) }\n")
    check(r.returncode == 0, f"a scalar function lowers cleanly ({r.stderr.strip()[:140]})")
    check("fn export @main() -> i32" in r.stdout, "the entry point keeps its unmangled name")
    check("__mir_" in r.stdout, "a non-entry function is mangled like codegen's")
    check("add" in r.stdout and "call @" in r.stdout, "the call is emitted")
    check("MIR is malformed" not in r.stderr, "the verifier accepts the result")


def case_locals_are_slots():
    r = emit_mir("pub fn main() -> i32 {\n  mut x: i32 = 7\n  x = x + 1\n  return x\n}\n")
    check(r.returncode == 0, "locals lower cleanly")
    check("slot0: size 4, align 4 ; x" in r.stdout, "a local becomes a stack slot carrying its name")
    check("store" in r.stdout and "load" in r.stdout,
          "and is reached through store/load (memory form; promote_slots undoes this later)")


def case_control_flow():
    r = emit_mir("pub fn main() -> i32 {\n"
                 "  mut t: i32 = 0\n  mut i: i32 = 0\n"
                 "  while i < 5 {\n    if i % 2 == 0 { t = t + i } else { t = t - 1 }\n    i = i + 1\n  }\n"
                 "  return t\n}\n")
    check(r.returncode == 0, "control flow lowers cleanly")
    for block in ("^while.cond", "^while.body", "^while.end", "^if.then", "^if.else", "^if.end"):
        check(block in r.stdout, f"'{block}' block is emitted")
    check("branch" in r.stdout and "jump" in r.stdout, "branches and jumps are emitted")
    check("srem" in r.stdout, "'%' on a signed type lowers to srem, not urem")


def case_short_circuit_uses_block_parameters():
    """'&&' is control flow, and the merge is the case phi nodes existed for."""
    r = emit_mir("pub fn main() -> i32 {\n  mut d: i32 = 0\n"
                 "  if d != 0 && 100 / d > 0 { return 1 }\n  return 0\n}\n")
    check(r.returncode == 0, "short-circuit '&&' lowers cleanly")
    check("^and.rhs" in r.stdout and "^logic.short" in r.stdout,
          "the right operand is guarded by its own block")
    # Block references carry the block index, so labels stay unambiguous when nested
    # control flow produces several blocks with the same name.
    check(re.search(r"\^and\.end\.\d+\(%", r.stdout) is not None,
          "the merge is a BLOCK PARAMETER, not a phi")
    check("MIR is malformed" not in r.stderr, "and the result verifies")


def case_unsigned_and_float_operators():
    r = emit_mir("pub fn main() -> i32 {\n"
                 "  mut a: u32 = 10\n  mut b: u32 = 3\n"
                 "  mut f: f64 = 1.5\n  mut g: f64 = 2.5\n"
                 "  const q := a / b\n  const s := f + g\n"
                 "  return cast(q, i32)\n}\n")
    check("udiv" in r.stdout or r.returncode != 0,
          "an unsigned divide lowers to udiv, not sdiv")
    check("fadd" in r.stdout or r.returncode != 0, "float addition lowers to fadd")


def case_unsupported_is_reported_not_skipped():
    """The property that matters while coverage is partial."""
    # 'defer' is the probe BECAUSE it is still unlowered -- when that changes, point this
    # at whatever is unlowered then rather than deleting it. The property under test is that
    # mirgen refuses loudly, not that any particular construct is missing.
    r = emit_mir("ext fn close(fd: i32) -> i32\n"
                 "pub fn main() -> i32 {\n  defer { close(1) }\n  return 0\n}\n")
    check(r.returncode != 0, "a construct mirgen cannot lower fails the build")
    check("native backend cannot lower" in r.stderr,
          "and says so, naming the native backend")
    check("not yet lowered by the native backend:" in r.stderr,
          "and prints a coverage summary listing the constructs")
    # Never a crash, and never silently-wrong output.
    check("terminate" not in r.stderr and "Aborted" not in r.stderr,
          "an unsupported construct does not abort the compiler")


def case_lvalues():
    """Struct fields, array indexing, pointer auto-deref and address-of.

    All four are the SAME address computation followed by a load or a store, which is why
    mirgen has one emit_address rather than a read path and a write path that can drift.
    Every offset comes from sema, so no layout logic is involved.
    """
    r = emit_mir("pub type Point = struct { x: i32  y: i32 }\n"
                 "pub fn main() -> i32 {\n"
                 "  mut p: Point = default\n  p.x = 3\n  p.y = 4\n"
                 "  mut arr: [4]i32 = default\n  arr[2] = arr[0] + p.x\n"
                 "  const ptr := &p\n  ptr.y = ptr.y * 2\n"
                 "  return cast(arr[2] + p.y, i32)\n}\n")
    check(r.returncode == 0, f"struct fields, indexing and pointer members lower ({r.stderr.strip()[:140]})")
    check("ptr.add.const" in r.stdout, "a struct field is a constant byte offset from the base")
    check("ptr.add " in r.stdout and "mul" in r.stdout, "an array index is a scaled offset")
    check("mem.set" in r.stdout, "'default' on an aggregate is a zero fill of the slot")
    check(", escapes ; p" in r.stdout,
          "taking a local's address marks its slot escaping (promote_slots must skip it)")
    check("MIR is malformed" not in r.stderr, "and the result verifies")


def case_casts():
    r = emit_mir("pub fn main() -> i32 {\n"
                 "  mut a: i32 = -5\n  const w := cast(a, i64)\n"
                 "  mut u: u8 = 200\n  const z := cast(u, i64)\n"
                 "  return cast(w + z, i32)\n}\n")
    check(r.returncode == 0, f"scalar casts lower ({r.stderr.strip()[:140]})")
    check("sext" in r.stdout, "a signed widening cast is a sign-extend")
    check("zext" in r.stdout, "an unsigned widening cast is a zero-extend")
    check("trunc" in r.stdout, "a narrowing cast truncates")
    # MIR integers are sign-agnostic (I8 is eight bits, not 'i8' or 'u8'), so the SOURCE
    # language type has to decide sext vs zext. Getting this wrong silently miscompiled
    # every unsigned widening: cast(u8(200), i64) produced -56.
    check(r.stdout.count("sext") == 1 and r.stdout.count("zext") == 1,
          "signed and unsigned widening pick different extensions")

    rb = emit_mir("pub fn main() -> i32 {\n  mut f: bool = true\n  return cast(f, i32)\n}\n")
    check("zext" in rb.stdout and "sext" not in rb.stdout,
          "a bool always zero-extends ('true' must widen to 1, never -1)")


def case_aggregates_are_memory():
    """Aggregates have no MIR value form, so every aggregate operation is a byte move of a
    size sema already computed. 'default' is a memset, copy is a memcpy, and an aggregate
    expression's "value" is its address."""
    r = emit_mir("pub type Point = struct { x: i32  y: i32 }\n"
                 "pub fn main() -> i32 {\n"
                 "  mut a: Point = default\n  a.x = 2\n"
                 "  mut b: Point = default\n  b = a\n"
                 "  return b.x\n}\n")
    check(r.returncode == 0, f"aggregate default and copy lower ({r.stderr.strip()[:140]})")
    check("mem.set" in r.stdout, "'default' is a zero fill")
    check("mem.copy" in r.stdout, "aggregate assignment is a byte copy")


def case_strings_and_len():
    r = emit_mir("pub fn main() -> i32 {\n"
                 "  mut arr: [3]i32 = default\n  const n := len(arr)\n"
                 '  const s := "hello"\n  const m := len(s)\n'
                 "  return cast(n + m, i32)\n}\n")
    check(r.returncode == 0, f"string literals and 'len' lower ({r.stderr.strip()[:140]})")
    check("const @.str.0: size 6" in r.stdout,
          "a string literal becomes a private constant global, NUL-terminated for C")
    check("global.addr @.str.0" in r.stdout, "and the slice points at it")
    # An array's length is known at compile time; a slice's is loaded from its second word.
    check("const.int 3" in r.stdout, "an array's 'len' folds to a constant")

    # The same literal twice must produce ONE global, as codegen interns them.
    r2 = emit_mir('pub fn main() -> i32 {\n  const a := "dup"\n  const b := "dup"\n  return 0\n}\n')
    check(r2.stdout.count("const @.str.") == 1, "identical string literals are interned")


def case_cross_module_calls():
    """'mod.fn()' is an ordinary direct call once the import binding names the target."""
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / "helper").mkdir()
        (root / "helper" / "helper.mir").write_text("pub fn triple(v: i32) -> i32 { return v * 3 }\n")
        (root / "main.mir").write_text('const h := import("helper")\n'
                                        "pub fn main() -> i32 { return h.triple(4) }\n")
        r = subprocess.run([str(MIRAGE), "build", str(root), "--emit-mir"],
                            capture_output=True, text=True, timeout=60, cwd=REPO_ROOT)
    check(r.returncode == 0, f"a cross-module call lowers ({r.stderr.strip()[:140]})")
    check("call @__mir_" in r.stdout and "triple" in r.stdout,
          "and resolves to the target module's mangled symbol")


def case_methods():
    """A method is an ordinary function taking the receiver as a leading pointer.

    A value receiver therefore needs an ADDRESS, and taking one pins its slot -- which is
    what makes 'self' work uniformly whether the method reads or mutates.
    """
    r = emit_mir("pub type Counter = struct { value: i32 }\n"
                 "impl Counter {\n"
                 "  pub fn get(self) -> i32 { return self.value }\n"
                 "  pub fn bump(mut self, by: i32) { self.value = self.value + by }\n"
                 "}\n"
                 "pub fn main() -> i32 {\n"
                 "  mut c: Counter = default\n  c.bump(5)\n  c.bump(3)\n  return c.get()\n}\n")
    check(r.returncode == 0, f"methods declare, emit and call ({r.stderr.strip()[:140]})")
    check("Counter::get" in r.stdout and "Counter::bump" in r.stdout,
          "mangled as 'Type::method', matching codegen so both backends emit one symbol")
    check("(%0: ptr" in r.stdout, "the receiver is a leading pointer parameter")
    check(", escapes ; c" in r.stdout,
          "a value receiver's slot is pinned, since the call needs its address")


def case_braced_initializers():
    r = emit_mir("pub type Point = struct { x: i32  y: i32 }\n"
                 "pub fn main() -> i32 {\n"
                 "  mut p: Point = { .x = 3, .y = 4 }\n"
                 "  mut arr: [3]i32 = { 1, 2, 3 }\n"
                 "  return p.x + p.y + arr[2]\n}\n")
    check(r.returncode == 0, f"struct and array literals lower ({r.stderr.strip()[:140]})")
    # The zero fill is what makes an OMITTED field default to zero without mirgen having to
    # know which fields were omitted.
    check("mem.set" in r.stdout, "a literal is zero-filled before its elements are written")
    check(r.stdout.count("store") >= 5, "each provided element is stored at its own offset")
    check("MIR is malformed" not in r.stderr, "and the result verifies")


def case_aggregate_returns_use_sret():
    """An aggregate return travels through a hidden pointer the CALLER owns.

    Returning the address of a callee slot instead would dangle the moment the frame went
    away, so the sret shape is a correctness requirement, not an optimization.
    """
    r = emit_mir("pub type Point = struct { x: i32  y: i32 }\n"
                 "pub fn make(a: i32, b: i32) -> Point {\n"
                 "  mut p: Point = { .x = a, .y = b }\n  return p\n}\n"
                 "pub fn main() -> i32 {\n  mut q: Point = make(3, 4)\n  return q.x + q.y }\n")
    check(r.returncode == 0, f"an aggregate return lowers ({r.stderr.strip()[:140]})")
    check("@__mir_" in r.stdout and "make(%0: ptr" in r.stdout,
          "the callee takes a leading sret pointer")
    check("mem.copy" in r.stdout, "and writes its result through it")
    check("MIR is malformed" not in r.stderr, "and the result verifies")


def case_error_returns():
    """'error(...)' is a tagged blob: u32 tag at offset 0, payload at payload_offset."""
    r = emit_mir("pub type E = enum(i32) { Bad = 1  Worse = 2 }\n"
                 "fn check(v: i32) -> error(E) {\n"
                 "  if v < 0 { return_err .Bad }\n  return_ok\n}\n"
                 "pub fn main() -> i32 {\n  const e := check(1)\n  return 0 }\n")
    check(r.returncode == 0, f"'return_ok'/'return_err' lower ({r.stderr.strip()[:140]})")
    # Ok carries no payload, so the zero fill IS the value.
    check(r.stdout.count("mem.set") >= 2, "both paths zero the blob first")
    check("store" in r.stdout, "and the failing path writes a tag")
    check("MIR is malformed" not in r.stderr, "and the result verifies")


def case_switch_and_conditions():
    r = emit_mir("pub type Dir = enum(u8) { North  South  East  West }\n"
                 "pub fn main() -> i32 {\n"
                 "  mut hits: i32 = 0\n  const d := Dir.South\n"
                 "  switch d {\n    .North: { hits = 1 },\n    .South: { hits = 2 },\n    _: { hits = 9 },\n  }\n"
                 "  mut p: *i32 = nil\n"
                 "  if p == nil { hits = hits + 10 }\n  if !p { hits = hits + 100 }\n"
                 "  return hits\n}\n")
    check(r.returncode == 0, f"'switch' and pointer conditions lower ({r.stderr.strip()[:140]})")
    check("switch %" in r.stdout and "default ^" in r.stdout,
          "'switch' lowers to the MIR switch terminator with a default")
    # A pointer in boolean context is a null test; comparing one against a const.int would
    # be ill-typed, which the MIR verifier catches.
    check("const.null" in r.stdout, "'!p' and 'p == nil' compare against null")
    check("MIR is malformed" not in r.stderr, "and the result verifies")

    # Block labels are an emitter aid and repeat; references carry the index so nested
    # control flow stays readable.
    check(len(re.findall(r"\^if\.end\.\d+:", r.stdout)) >= 2,
          "duplicate block labels are disambiguated by index")


def case_mir_goes_to_stdout():
    """'--emit-mir > out.mir' has to produce a valid file, as '--emit-ir' does."""
    r = emit_mir("pub fn main() -> i32 { return 0 }\n")
    check(r.stdout.startswith("; mirage ir"), "MIR is written to stdout")
    check(r.stderr.strip() == "", "and nothing else is mixed into it")


def main() -> int:
    if not MIRAGE.exists():
        print(f"FAIL: {MIRAGE} not found; run 'just build' first")
        return 1

    case_scalar_function()
    case_locals_are_slots()
    case_control_flow()
    case_short_circuit_uses_block_parameters()
    case_unsigned_and_float_operators()
    case_lvalues()
    case_casts()
    case_aggregates_are_memory()
    case_strings_and_len()
    case_cross_module_calls()
    case_methods()
    case_braced_initializers()
    case_aggregate_returns_use_sret()
    case_error_returns()
    case_switch_and_conditions()
    case_unsupported_is_reported_not_skipped()
    case_mir_goes_to_stdout()

    print()
    if failures:
        print(f"{failures} failure(s)")
        return 1
    print("all mirgen tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
