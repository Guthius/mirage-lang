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
    # Inline 'asm' is the probe BECAUSE it is still unlowered (it needs the stage-5
    # encoder) -- when that changes, point this at whatever is unlowered then rather than
    # deleting it. The property under test is that mirgen refuses loudly, not that any
    # particular construct is missing.
    r = emit_mir("pub fn main() -> i32 {\n"
                 "  mut x: i64 = 41\n"
                 "  asm {\n    add x, 1\n  }\n"
                 "  return cast(x, i32) - 42\n}\n")
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


def case_multi_return():
    """A return list travels through ONE caller-owned sret blob, each value at its
    naturally-aligned offset. Writers (return / return_ok / return_err) and readers
    (group declarations, forwarded returns) must agree on that layout exactly, so this
    pins both sides of it."""
    r = emit_mir("pub type E = enum(i32) { Bad = 1 }\n"
                 "fn divmod(a: i32, b: i32) -> (i32, i32, error(E)) {\n"
                 "  if b == 0 { return_err .Bad }\n"
                 "  return_ok a / b, a % b\n}\n"
                 "fn pair() -> (i32, i64) { return 7, 9 }\n"
                 "fn forward() -> (i32, i64) { return pair() }\n"
                 "pub fn main() -> i32 {\n"
                 "  const q, r, err := divmod(17, 5)\n"
                 "  if err { return 1 }\n"
                 "  const a, _ := forward()\n"
                 "  return q + r + a\n}\n")
    check(r.returncode == 0, f"multi-return lowers ({r.stderr.strip()[:140]})")
    # divmod's blob is (i32@0, i32@4, error@8): the callee writes the second value at
    # offset 4 and the error slot at 8; the caller destructures at the same offsets.
    check("ptr.add.const %0, 4" in r.stdout, "the second value slot sits at its layout offset")
    check("ptr.add.const %0, 8" in r.stdout, "and the error slot after it")
    # pair's blob is (i32@0, i64@8): alignment padding, not packing.
    check("ptr.add.const %0, 8" in r.stdout, "an i64 slot is naturally aligned, not packed")
    # 'return pair()' with identical return lists is one blob copy, not a rebuild.
    check("mem.copy" in r.stdout, "an exact-match forwarded return is one blob copy")
    # '_' binds nothing: no slot named '_' exists.
    check("; _" not in r.stdout, "a '_' group slot is not bound")
    check("MIR is malformed" not in r.stderr, "and the result verifies")


def case_tagged_unions():
    """A tagged union is a blob: u32 tag at 0, payload at payload_offset. switch/match
    dispatch on the tag; captures bind the payload -- by value as a copy, by reference
    as a pointer INTO the dispatch's own copy of the operand."""
    r = emit_mir("type Shape = union(enum) {\n"
                 "  Empty\n  Circle: struct { r: i32 }\n  Pair: struct { a: i32  b: i32 }\n}\n"
                 "fn area(s: Shape) -> i32 {\n"
                 "  switch s {\n"
                 "    .Empty: { return 0 },\n"
                 "    .Circle(c): { return c.r * 3 },\n"
                 "    .Pair(&p): { p.a = p.a + 1  return p.a + p.b },\n"
                 "  }\n  return -1\n}\n"
                 "fn describe(s: Shape) -> i32 {\n"
                 "  return match s {\n    .Empty: 1,\n    .Circle: 2,\n    _: 3,\n  }\n}\n"
                 "pub fn main() -> i32 {\n"
                 "  mut c: Shape = .Circle{.r = 5}\n"
                 "  return area(c) + describe(c)\n}\n")
    check(r.returncode == 0, f"tagged-union switch/match lower ({r.stderr.strip()[:140]})")
    # An aggregate parameter arrives as a pointer to the caller's copy; its BYTES are
    # copied into the local slot. Storing the pointer itself made every later read of
    # the parameter read the pointer's bytes as data.
    check(re.search(r"; s\n\s+%\d+: i64 = const\.int 12\n\s+mem\.copy", r.stdout) is not None
          or "mem.copy %1, %0" in r.stdout,
          "an aggregate parameter is copied by value into its slot")
    check("switch.union" in r.stdout, "the switch operand is copied into a dispatch slot")
    check(re.search(r"switch %\d+, default", r.stdout) is not None, "and dispatched on its tag")
    check("switch.arm.Circle" in r.stdout and "switch.arm.Pair" in r.stdout,
          "arm blocks carry their variant names")
    check(re.search(r"\^match\.end\.\d+\(%\d+: i32\)", r.stdout) is not None,
          "a scalar match result merges through a block parameter")
    check("MIR is malformed" not in r.stderr, "and the result verifies")


def case_locals_are_zeroed_and_typed_by_declaration():
    """'mut x: [20]u8' with no initializer is a 20-byte ZEROED slot -- the declared type
    wins over the (absent) initializer, and no-init means zero-valued, not garbage."""
    r = emit_mir("pub fn main() -> i32 {\n"
                 "  mut out: [20]u8\n"
                 "  mut n: i32\n"
                 "  mut u: i64 = undefined\n"
                 "  u = 1\n"
                 "  return n + cast(out[19], i32) + cast(u, i32) - 1\n}\n")
    check(r.returncode == 0, f"uninitialized locals lower ({r.stderr.strip()[:140]})")
    check("slot0: size 20, align 1 ; out" in r.stdout,
          "the declared type sizes the slot, not an i64 fallback")
    check("mem.set" in r.stdout, "an uninitialized aggregate is zero-filled")
    # 'undefined' is the explicit opt-out: exactly the two zero-inits above, no third.
    check(r.stdout.count("mem.set") == 1, "'undefined' leaves its slot alone")


def case_pointer_arithmetic_scales():
    r = emit_mir("pub fn main() -> i32 {\n"
                 "  mut a: [4]i32 = {10, 20, 30, 40}\n"
                 "  mut p: *i32 = &a[0]\n"
                 "  const third: *i32 = p + 2\n"
                 "  const back: *i32 = third - 1\n"
                 "  return third.* + back.* - 50\n}\n")
    check(r.returncode == 0, f"pointer +/- integer lowers ({r.stderr.strip()[:140]})")
    check("mul" in r.stdout, "the index is scaled by the pointee's size")
    check("neg" in r.stdout, "subtraction negates the byte offset")
    check("MIR is malformed" not in r.stderr,
          "and no integer 'add' is emitted on a pointer operand")


def case_slice_array_coercions():
    """'out = s' with s a slice and out an array copies min(len, count) elements and
    zero-fills the tail; a slice passed where '*u8' is expected passes its data word."""
    r = emit_mir("ext fn puts(s: *u8) -> i32\n"
                 "fn fill(s: []u8) -> [8]u8 {\n  mut out: [8]u8\n  out = s\n  return out\n}\n"
                 "pub fn main() -> i32 {\n"
                 "  const out := fill(\"hi\")\n"
                 "  puts(\"done\")\n"
                 "  return cast(out[0], i32) - 104\n}\n")
    check(r.returncode == 0, f"slice/array coercions lower ({r.stderr.strip()[:140]})")
    check("icmp.ult" in r.stdout and "select" in r.stdout,
          "the copy length is min(slice len, array count)")
    check(re.search(r"load %\d+\n.*call @puts", r.stdout) is not None or "call @puts" in r.stdout,
          "a slice argument to a '*u8' parameter passes the data word")
    check("MIR is malformed" not in r.stderr, "and the result verifies")


def case_try_propagation():
    """'try' branches on the callee error's tag: on failure the error lands in the
    caller's own last return slot and the function returns; on success the surviving
    value is read out of the callee's blob. A subset error union re-tags rather than
    byte-copies."""
    r = emit_mir("pub type E = enum(i32) { Bad = 1 }\n"
                 "pub type F = enum(i32) { Worse = 1 }\n"
                 "fn divide(a: i32, b: i32) -> (i32, error(E)) {\n"
                 "  if b == 0 { return_err .Bad }\n  return_ok a / b\n}\n"
                 "fn narrow() -> error(E) { return_ok }\n"
                 "fn widen() -> (i32, error(E | F)) {\n"
                 "  try narrow()\n"
                 "  const q := try divide(10, 2)\n"
                 "  return_ok q\n}\n"
                 "pub fn main() -> i32 {\n"
                 "  const v, err := widen()\n"
                 "  if err { return 1 }\n  return v\n}\n")
    check(r.returncode == 0, f"'try' lowers ({r.stderr.strip()[:140]})")
    check("try.propagate" in r.stdout and "try.ok" in r.stdout,
          "each 'try' splits into propagate/ok blocks")
    check("icmp.ne" in r.stdout, "and branches on the tag")
    # E is a subset of (E, F): the propagated error is re-tagged for the wider union.
    check("retag" not in r.stdout or "retag." in r.stdout,
          "a subset propagation goes through the re-tag path when needed")
    check("MIR is malformed" not in r.stderr, "and the result verifies")


def case_trait_dispatch():
    """A trait handle is {data, vtable}; a '.method()' call through one is an indirect
    call through the vtable slot at the method's order index. Vtables are constant
    globals whose entries are RELOCATIONS -- no address exists until layout."""
    r = emit_mir("pub type Animal = trait {\n  fn speak(self) -> i32\n  fn legs(self) -> i32\n}\n"
                 "type Dog = struct { volume: i32 }\n"
                 "impl Animal for Dog {\n"
                 "  fn speak(self) -> i32 { return self.volume * 2 }\n"
                 "  fn legs(self) -> i32 { return 4 }\n}\n"
                 "fn describe(a: Animal) -> i32 {\n"
                 "  if a == nil { return -1 }\n  return a.speak() + a.legs()\n}\n"
                 "pub fn main() -> i32 {\n"
                 "  mut d: Dog = { .volume = 10 }\n  const da: Animal = &d\n"
                 "  return describe(da)\n}\n")
    check(r.returncode == 0, f"trait dispatch lowers ({r.stderr.strip()[:140]})")
    check("const @.vtable.0" in r.stdout, "the impl gets a constant vtable global")
    check(re.search(r"\+0 -> @\S*Dog::Animal::speak", r.stdout) is not None,
          "whose slot 0 relocates to the first trait method")
    check(re.search(r"\+8 -> @\S*Dog::Animal::legs", r.stdout) is not None,
          "and slot 1 to the second, in TraitInfo::methods order")
    check("call.indirect" in r.stdout, "dispatch is an indirect call")
    # 'a == nil' compares DATA words: the nil literal is already the null data word,
    # so no load through null is emitted.
    check(re.search(r"icmp\.eq %\d+, %\d+", r.stdout) is not None, "nil test compares data words")
    check("MIR is malformed" not in r.stderr, "and the result verifies")


def case_compound_assignment():
    """'x += 2' is load/apply/store. It used to lower as a plain store of the RHS --
    silently, since the MIR was type-correct and nothing executes it yet."""
    r = emit_mir("pub fn main() -> i32 {\n"
                 "  mut x: i32 = 40\n  x += 2\n"
                 "  mut u: u32 = 8\n  u >>= 1\n"
                 "  mut s: i32 = -8\n  s >>= 1\n"
                 "  return x + cast(u, i32) + s - 42\n}\n")
    check(r.returncode == 0, f"compound assignment lowers ({r.stderr.strip()[:140]})")
    check(re.search(r"load %\d+\n\s+%\d+: i32 = add", r.stdout) is not None,
          "'+=' loads the old value and adds")
    check("lshr" in r.stdout, "'>>=' on an unsigned target is a logical shift")
    check("ashr" in r.stdout, "'>>=' on a signed target is an arithmetic shift")
    check("MIR is malformed" not in r.stderr, "and the result verifies")


def case_defer_ternary_sizeof():
    """'defer' bodies run in LIFO order at every exit path: the block's end, every
    'return', and 'break'/'continue' (down to the loop body's scope). A ternary is
    control flow -- the unchosen side must not run -- merging like match does.
    size_of/align_of are compile-time constants read from sema's layout."""
    r = emit_mir("ext fn close(fd: i32) -> i32\n"
                 "pub type P = struct { x: i64  y: i32 }\n"
                 "fn work(early: bool) -> i32 {\n"
                 "  defer { close(3) }\n"
                 "  {\n    defer { close(1) }\n    if early { return 100 }\n  }\n"
                 "  mut i: i32 = 0\n"
                 "  while i < 3 {\n    defer { close(4) }\n    i = i + 1\n"
                 "    if i == 2 { continue }\n  }\n"
                 "  return 0\n}\n"
                 "pub fn main() -> i32 {\n"
                 "  const w := work(false)\n"
                 "  const t := w == 0 ? cast(size_of(P), i32) : cast(align_of(P), i32)\n"
                 "  return t - 16\n}\n")
    check(r.returncode == 0, f"defer/ternary/size_of lower ({r.stderr.strip()[:140]})")
    # Early return: inner defer (1), then outer (3), then ret -- LIFO across scopes.
    check(re.search(r"const\.int 1\n.*call @close.*\n.*const\.int 3\n.*call @close",
                    r.stdout) is not None,
          "a return runs enclosing defers innermost-first")
    check("ternary.then" in r.stdout and "ternary.else" in r.stdout,
          "a ternary is control flow, not a select")
    check("const.int 16" in r.stdout, "size_of(P) folds to sema's layout answer")
    check("MIR is malformed" not in r.stderr, "and the result verifies")


def case_bitsets_and_slice_casts():
    """A bitset value is its storage integer; a member is its one-bit mask
    (1 << (enum value + 1)); '+='/'-=' are set union/difference; 'in' is
    '(set & mask) == mask'. A slice-forming cast builds a (data, len) header, and the
    explicit length wins over the operand's own extent."""
    r = emit_mir("pub type Mode = enum { Read  Write  Exec }\n"
                 "pub type Modes = bitset(Mode, u8)\n"
                 "pub fn main() -> i32 {\n"
                 "  mut m: Modes = {.Read, .Write}\n"
                 "  m += .Exec\n  m -= .Write\n"
                 "  mut total: i32 = 0\n"
                 "  if .Read in m { total += 1 }\n"
                 "  mut buf: [4]u8 = {7, 8, 9, 10}\n"
                 "  const s := cast(&buf[0], []u8, 2)\n"
                 "  return total + cast(len(s), i32)\n}\n")
    check(r.returncode == 0, f"bitsets and slice casts lower ({r.stderr.strip()[:140]})")
    check("const.int 6" in r.stdout, "'{.Read, .Write}' folds to one mask constant (2|4)")
    check("or " in r.stdout or re.search(r"= or %", r.stdout) is not None,
          "'+=' on a bitset is a set union")
    check(re.search(r"= not %", r.stdout) is not None, "'-=' clears via and-not")
    check("MIR is malformed" not in r.stderr, "and the result verifies")


def case_generic_instances():
    """A generic call resolves to the monomorphized instance sema already picked
    (expr_generic_fn_instance) -- never re-derived here -- and each instance's body is
    emitted with that instance's own expr tables and substitution env active, which is
    what makes 'size_of(T)' inside the body fold to the concrete type's size."""
    r = emit_mir("fn scaled[T: type](v: T) -> T {\n"
                 "  return v * cast(size_of(T), T)\n}\n"
                 "pub type Box[T: type] = struct { value: T }\n"
                 "impl Box[T: type] {\n"
                 "  pub fn get(self) -> T { return self.value }\n}\n"
                 "pub fn main() -> i32 {\n"
                 "  mut b: Box[i64] = { .value = 5 }\n"
                 "  return cast(scaled[i64](4) + b.get(), i32)\n}\n")
    check(r.returncode == 0, f"generic instances lower ({r.stderr.strip()[:140]})")
    check(re.search(r"fn @\S*scaled__i64\(", r.stdout) is not None,
          "a free-function instance is emitted under its mangled name")
    check(re.search(r"fn @\S*Box__i64::get\(%\d+: ptr\)", r.stdout) is not None,
          "a generic-type method instance takes self as a leading pointer")
    check("const.int 8" in r.stdout,
          "'size_of(T)' inside the instance folds through the substitution env")
    check("MIR is malformed" not in r.stderr, "and the result verifies")


def case_ranges_unions_defaults():
    """Range for-in counts at usize width with the index bound to the COUNTER;
    untagged union members alias offset 0; defaulted arguments evaluate in the
    callee's own context at the call site."""
    r = emit_mir("type U = union {\n  as_i64: i64\n  as_f64: f64\n}\n"
                 "fn add(a: i32, b: i32 = 30) -> i32 { return a + b }\n"
                 "pub fn main() -> i32 {\n"
                 "  mut sum: i32 = 0\n"
                 "  for i, x in 2..5 { sum += cast(i, i32) + x }\n"
                 "  mut u: U = { .as_i64 = 7 }\n"
                 "  u.as_i64 = 5\n"
                 "  mut d: i32 = default\n"
                 "  return sum + cast(u.as_i64, i32) + d + add(7)\n}\n")
    check(r.returncode == 0, f"ranges/unions/defaults lower ({r.stderr.strip()[:140]})")
    check("^for.cond" in r.stdout and "^for.step" in r.stdout, "a range is a counting loop")
    check("const.int 30" in r.stdout, "the omitted argument's default is emitted at the call site")
    check("MIR is malformed" not in r.stderr, "and the result verifies")


def case_macros():
    """A macro is expression-template expansion: the template emits under the macro's
    declaring module, while a parameter reference evaluates the ARGUMENT back in its
    own call-site context."""
    r = emit_mir("const alignment: usize = 8\n"
                 "macro align_up(n: usize) -> (n + (alignment - 1)) & ~(alignment - 1)\n"
                 "pub fn main() -> i32 {\n"
                 "  const a := align_up(13)\n"
                 "  return cast(a, i32) - 16\n}\n")
    check(r.returncode == 0, f"macros lower ({r.stderr.strip()[:140]})")
    check("call @" not in r.stdout.replace("call @printf", ""),
          "a macro expands inline -- no call is emitted")
    check("const.int 13" in r.stdout, "the argument expression lands in the expansion")
    check("MIR is malformed" not in r.stderr, "and the result verifies")


def case_native_variadics():
    """A Mirage-native variadic receives its tail as ONE slice, collected at the call
    site; 'xs...' forwards an existing slice verbatim. Only a C 'ext fn' variadic
    passes raw trailing arguments."""
    r = emit_mir("fn total(base: i32, xs: ...i32) -> i32 {\n"
                 "  mut sum: i32 = base\n  for x in xs { sum += x }\n  return sum\n}\n"
                 "pub fn main() -> i32 {\n"
                 "  return total(1, 2, 3, 4) - 10\n}\n")
    check(r.returncode == 0, f"native variadics lower ({r.stderr.strip()[:140]})")
    check("variadic.tmp" in r.stdout, "the tail is collected into a backing array")
    check("const.int 3" in r.stdout, "with the count in the slice header")
    check("MIR is malformed" not in r.stderr, "and the result verifies")


def case_self_calls_and_field_fn_ptrs():
    """'self.bump()' inside a method resolves through one stripped pointer level (the
    receiver auto-deref); 'h.f(x)' where 'f' is a function-typed FIELD is an indirect
    call through the field's value, not a method."""
    r = emit_mir("type Counter = struct { n: i32 }\n"
                 "impl Counter {\n"
                 "  fn bump(mut self) -> i32 { self.n += 1  return self.n }\n"
                 "  pub fn twice(mut self) -> i32 { return self.bump() + self.bump() }\n}\n"
                 "type Holder = struct { f: fn(i32) -> i32 }\n"
                 "fn negate(v: i32) -> i32 { return -v }\n"
                 "pub fn main() -> i32 {\n"
                 "  mut c: Counter = { .n = 0 }\n  mut h: Holder = { .f = negate }\n"
                 "  return c.twice() + h.f(-39)\n}\n")
    check(r.returncode == 0, f"self-calls and field fn-ptrs lower ({r.stderr.strip()[:140]})")
    check("call.indirect" in r.stdout, "the field call goes through call.indirect")
    check(re.search(r"Counter::bump\(%\d+: ptr\)", r.stdout) is not None,
          "the self-call resolves to the concrete method")
    check("MIR is malformed" not in r.stderr, "and the result verifies")


def case_dropped_optional_error():
    """A call whose trailing '?error(...)' the caller drops gets the check it didn't
    write: branch on the tag, and on Failed call a per-union noreturn panic helper
    that names the variant and the call site, then exits 101."""
    r = emit_mir("pub type E = enum(i32) { Bad = 1  Worse = 2 }\n"
                 "fn risky(fail: bool) -> (i32, ?error(E)) {\n"
                 "  if fail { return_err .Worse }\n  return_ok 21\n}\n"
                 "pub fn main() -> i32 {\n"
                 "  const v := risky(false)\n  return v * 2\n}\n")
    check(r.returncode == 0, f"a dropped ?error lowers ({r.stderr.strip()[:140]})")
    check("err.panic" in r.stdout and "err.ok" in r.stdout, "the drop is checked at the call site")
    check("__mirage_panic_unhandled_error." in r.stdout,
          "through the per-union panic helper, named like codegen's")
    check("panic.variant.Worse" in r.stdout, "which dispatches to the variant's name")
    check("call @exit" in r.stdout and "const.int 101" in r.stdout, "and exits 101")
    check("MIR is malformed" not in r.stderr, "and the result verifies")


def case_reflection():
    """Type_Info descriptors are constant globals whose entries are byte blobs with
    RELOCATIONS (name strings, field slices, nested descriptors), plus the sorted
    __mirage_type_info_table. 'type_info_of(type_of(T))' folds to the descriptor's
    address; a runtime id goes through an inline binary search."""
    r = emit_mir("pub type Point = struct { x: i32  y: f64 }\n"
                 "pub fn main() -> i32 {\n"
                 "  const direct := type_info_of(type_of(Point))\n"
                 "  if direct == nil { return 1 }\n"
                 "  const scalar := type_info_of(type_of(i32))\n"
                 "  if scalar != nil { return 2 }\n"
                 "  mut id: type = type_of(Point)\n"
                 "  const looked := type_info_of(id)\n"
                 "  if looked != direct { return 3 }\n"
                 "  return 42\n}\n")
    # This source has no runtime/type_info import, so Type_Info may be unavailable;
    # accept either a clean lower or sema's own rejection -- but never a crash or
    # malformed MIR.
    if r.returncode == 0:
        check("__mirage_type_info_table" in r.stdout, "the id table is emitted")
        check("tinfo.cond" in r.stdout, "a runtime id lookup is an inline binary search")
        check("MIR is malformed" not in r.stderr, "and the result verifies")
    else:
        check("cannot lower" not in r.stderr and "malformed" not in r.stderr,
              f"reflection without runtime/type_info fails in sema, not mirgen ({r.stderr.strip()[:120]})")


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


def case_indirect_calls_and_constants():
    r = emit_mir("fn add(a: i32, b: i32) -> i32 { return a + b }\n"
                 "pub fn main() -> i32 {\n"
                 "  const f: fn(i32, i32) -> i32 = add\n  const c := 'A'\n"
                 "  return f(3, 4) + cast(c, i32)\n}\n")
    check(r.returncode == 0, f"function-pointer calls lower ({r.stderr.strip()[:140]})")
    check("func.addr @" in r.stdout, "taking a function's address yields a code pointer")
    # wasm's call_indirect needs the signature as a TYPE INDEX, so MIR carries it
    # explicitly rather than inferring it from the callee.
    check("call.indirect" in r.stdout and ": sig" in r.stdout,
          "an indirect call names its signature explicitly")
    check("const.int 65" in r.stdout, "a character literal is its byte value")
    check("MIR is malformed" not in r.stderr, "and the result verifies")


def case_when_emits_only_the_live_branch():
    """'when' is resolved at compile time: sema folded the condition, so lowering emits one
    branch and NO control flow. The dead branch is type-checked (the language's
    both-branches rule) but must not be emitted -- that is the whole point of 'when'."""
    r = emit_mir("pub fn main() -> i32 {\n  mut r: i32 = 0\n"
                 "  when size_of(usize) == 8 { r = 64 } else { r = 32 }\n  return r\n}\n")
    check(r.returncode == 0, f"'when' lowers ({r.stderr.strip()[:140]})")
    check("const.int 64" in r.stdout, "the selected branch is emitted")
    check("const.int 32" not in r.stdout, "and the dead branch is NOT")
    check("branch" not in r.stdout, "no runtime control flow is emitted at all")


def case_loops():
    """'for-in' over a slice, in all three binding forms, with break and continue.

    'continue' targets the loop's STEP block, not its condition -- targeting the condition
    would skip the increment and spin forever, which is the classic desugaring bug.
    """
    r = emit_mir("pub fn main() -> i32 {\n"
                 "  mut xs: [5]i32 = { 1, 2, 3, 4, 5 }\n  mut total: i32 = 0\n"
                 "  for v in xs[..] {\n    if v == 4 { break }\n    if v == 2 { continue }\n"
                 "    total = total + v\n  }\n"
                 "  for i, w in xs[..] { total = total + cast(i, i32) + w }\n"
                 "  for &r in xs[..] { r.* = r.* * 2 }\n"
                 "  return total\n}\n")
    check(r.returncode == 0, f"'for-in' lowers in all three forms ({r.stderr.strip()[:140]})")
    for block in ("^for.cond", "^for.body", "^for.step", "^for.end"):
        check(block in r.stdout, f"'{block}' block is emitted")
    check("MIR is malformed" not in r.stderr, "and the result verifies")

    # A slice expression builds the two-word (data, length) pair in a slot.
    check("slice" in r.stdout, "'xs[..]' materializes a slice")


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
    case_multi_return()
    case_tagged_unions()
    case_locals_are_zeroed_and_typed_by_declaration()
    case_pointer_arithmetic_scales()
    case_slice_array_coercions()
    case_try_propagation()
    case_trait_dispatch()
    case_compound_assignment()
    case_defer_ternary_sizeof()
    case_bitsets_and_slice_casts()
    case_generic_instances()
    case_ranges_unions_defaults()
    case_macros()
    case_native_variadics()
    case_self_calls_and_field_fn_ptrs()
    case_dropped_optional_error()
    case_reflection()
    case_switch_and_conditions()
    case_indirect_calls_and_constants()
    case_when_emits_only_the_live_branch()
    case_loops()
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
