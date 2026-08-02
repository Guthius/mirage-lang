#!/usr/bin/env python3
"""Diagnostics and codegen for the attributes added alongside the self-hosting work:
'@no_discard', '@export', '@callconv', '@cdecl', '@import'.

The ABI behaviour of '@cdecl' lives in tests/cdecl_abi_test.py, which links against C.
This suite covers everything that is decided in the front end: where each attribute is
legal, what it rejects, and (for '@export') what symbol actually comes out.

Not wired into ctest (same posture as the other .py suites). Run manually:

    just build
    python3 tests/attribute_new_test.py
"""

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


def compile_source(source: str, *extra):
    """Builds 'source' as a one-file module; returns the CompletedProcess."""
    with tempfile.TemporaryDirectory() as tmp:
        (Path(tmp) / "main.mir").write_text(source)
        return subprocess.run(
            [str(MIRAGE), "build", tmp, "-o", "/dev/null", *extra],
            capture_output=True, text=True, timeout=60, cwd=REPO_ROOT,
        )


def emit_ir(source: str, *extra):
    with tempfile.TemporaryDirectory() as tmp:
        (Path(tmp) / "main.mir").write_text(source)
        return subprocess.run(
            [str(MIRAGE), "build", tmp, "--emit-ir", *extra],
            capture_output=True, text=True, timeout=60, cwd=REPO_ROOT,
        )


def expect_error(source: str, fragment: str, message: str, *extra) -> None:
    r = compile_source(source, *extra)
    check(r.returncode != 0 and fragment in r.stderr,
          f"{message} (got: {r.stderr.strip().splitlines()[0][:130] if r.stderr.strip() else 'no diagnostic'})")


def expect_ok(source: str, message: str, *extra) -> None:
    r = compile_source(source, *extra)
    check(r.returncode == 0, f"{message} ({r.stderr.strip().splitlines()[0][:130] if r.returncode else ''})")


# ---------------------------------------------------------------- parser

def case_parser():
    expect_error("@bogus\npub fn f() -> i32 { return 1 }\npub fn main() -> i32 { return f() }\n",
                 "unknown attribute '@bogus'", "an unknown attribute name is rejected")
    expect_error("@bogus\npub fn f() -> i32 { return 1 }\npub fn main() -> i32 { return f() }\n",
                 "no_discard, export, callconv, cdecl, import",
                 "the unknown-attribute message lists the new names")
    # Grouped form still works, and still refuses per-member arguments.
    expect_ok("@(no_discard, always_inline)\nfn f() -> i32 { return 1 }\n"
              "pub fn main() -> i32 { const _ := f()  return 0 }\n",
              "grouped '@(no_discard, always_inline)' is accepted")
    expect_error("@(export(\"x\"))\npub fn f() -> i32 { return 1 }\npub fn main() -> i32 { return f() }\n",
                 "grouped attribute", "a grouped member still cannot take arguments")
    # 'import' is a reserved keyword, so the attribute-name position has to accept it
    # specially -- without that, '@import(...)' does not parse at all.
    expect_ok('@import("env", "host_now")\next fn host_now() -> i64\n'
              "pub fn main() -> i32 { return 0 }\n",
              "'@import' parses despite 'import' being a keyword")


# ---------------------------------------------------------------- @no_discard

def case_no_discard():
    dropped = "@no_discard\nfn f() -> i32 { return 1 }\npub fn main() -> i32 { f()  return 0 }\n"
    expect_error(dropped, "must be used ('@no_discard')", "a dropped '@no_discard' result is an error")
    expect_error(dropped, "assign to '_'", "the error names the '_' opt-out")

    expect_ok("@no_discard\nfn f() -> i32 { return 1 }\n"
              "pub fn main() -> i32 { const _ := f()  return 0 }\n",
              "'const _ := f()' silences '@no_discard'")
    expect_ok("@no_discard\nfn f() -> i32 { return 1 }\n"
              "pub fn main() -> i32 { return f() }\n",
              "using the result normally is fine")

    expect_error("@no_discard\nfn f() { }\npub fn main() -> i32 { return 0 }\n",
                 "has no effect", "'@no_discard' on a void function is an error")
    expect_error("@no_discard(1)\nfn f() -> i32 { return 1 }\npub fn main() -> i32 { return f() }\n",
                 "takes no arguments", "'@no_discard' takes no arguments")

    # Methods carry it too, and the diagnostic names the method.
    expect_error("pub type T = struct { x: i32 }\n"
                 "impl T {\n  @no_discard\n  pub fn get(self) -> i32 { return self.x }\n}\n"
                 "pub fn main() -> i32 { mut t: T = default  t.get()  return 0 }\n",
                 "return value of 'get' must be used", "'@no_discard' works on an impl method")


# ---------------------------------------------------------------- @export

def case_export():
    ir = emit_ir("@export\npub fn exported() -> i32 { return 1 }\n"
                 "pub fn main() -> i32 { return exported() }\n")
    check("define i32 @exported()" in ir.stdout,
          "bare '@export' emits the declaration's own name, unmangled")

    ir = emit_ir('@export("custom_name")\npub fn renamed() -> i32 { return 1 }\n'
                 "pub fn main() -> i32 { return renamed() }\n")
    check("define i32 @custom_name()" in ir.stdout, "'@export(\"name\")' emits that name")
    check("@renamed" not in ir.stdout, "the original name is not also emitted")

    # A non-pub function is normally internal; '@export' forces external linkage, or the
    # symbol would not be visible to a linker at all.
    ir = emit_ir("@export\nfn private_but_exported() -> i32 { return 1 }\n"
                 "pub fn main() -> i32 { return private_but_exported() }\n")
    check("define i32 @private_but_exported()" in ir.stdout,
          "'@export' on a non-pub function still gets external linkage")

    expect_error('@export("a")\npub fn f() -> i32 { return 1 }\n'
                 '@export("a")\npub fn g() -> i32 { return 2 }\n'
                 "pub fn main() -> i32 { return f() + g() }\n",
                 "duplicate export name 'a'", "two declarations exporting one name is an error")

    expect_error('ext fn puts(s: *u8) -> i32\n@export("puts")\npub fn mine() -> i32 { return 1 }\n'
                 "pub fn main() -> i32 { return mine() }\n",
                 "duplicate export name 'puts'", "an export colliding with an 'ext fn' is an error")

    expect_error("@export\npub fn f[T: type](v: T) -> i32 { return 1 }\n"
                 "pub fn main() -> i32 { return f(1) }\n",
                 "not allowed on a generic function", "'@export' on a generic is an error")

    expect_error('@export("bad name")\npub fn f() -> i32 { return 1 }\n'
                 "pub fn main() -> i32 { return f() }\n",
                 "is not a valid symbol name", "an export name with a space is rejected")

    # Orthogonality warning: '@export' alone on an aggregate-by-value signature.
    r = compile_source("pub type P = struct { x: i32  y: i32 }\n@export\n"
                       "pub fn f(p: P) -> i32 { return p.x }\n"
                       "pub fn main() -> i32 { mut p: P = default  return f(p) }\n")
    check(r.returncode == 0 and "does not declare '@cdecl'" in r.stderr,
          "'@export' with an aggregate parameter and no '@cdecl' warns (but still compiles)")

    r2 = compile_source("@export\npub fn f(a: i32) -> i32 { return a }\n"
                        "pub fn main() -> i32 { return f(1) }\n")
    check("does not declare '@cdecl'" not in r2.stderr,
          "a scalar-only '@export' signature does not warn (the conventions coincide)")

    # Methods: '@export' changes only the symbol's name and linkage, so it IS honoured
    # there (a codegen path that was silently ignoring it until this was checked).
    ir = emit_ir("pub type T = struct { x: i32 }\n"
                 "impl T {\n  @export(\"method_get\")\n  pub fn get(self) -> i32 { return self.x }\n}\n"
                 "pub fn main() -> i32 { mut t: T = default  return t.get() }\n")
    check("define i32 @method_get(ptr" in ir.stdout, "'@export' on an impl method emits that symbol")

    ir2 = emit_ir("pub type Shape = trait { fn area(self) -> i32 }\n"
                  "pub type Sq = struct { s: i32 }\n"
                  "impl Shape for Sq {\n  @export(\"trait_area\")\n  fn area(self) -> i32 { return self.s * self.s }\n}\n"
                  "pub fn main() -> i32 { mut q: Sq = default  const h: Shape = &q  return h.area() }\n")
    check("define i32 @trait_area(ptr" in ir2.stdout, "'@export' on a trait-impl method emits that symbol")

    expect_error("pub type T = struct { x: i32 }\n"
                 "impl T {\n  @export(\"dup\")\n  pub fn get(self) -> i32 { return self.x }\n}\n"
                 "@export(\"dup\")\npub fn other() -> i32 { return 1 }\n"
                 "pub fn main() -> i32 { mut t: T = default  return t.get() + other() }\n",
                 "duplicate export name 'dup'", "a method and a function cannot claim one export name")


# ---------------------------------------------------------------- @callconv / @cdecl

def case_callconv():
    expect_ok("@cdecl\npub fn f(a: i32) -> i32 { return a }\n"
              "pub fn main() -> i32 { return f(1) }\n", "'@cdecl' is accepted")
    expect_ok('@callconv("c")\npub fn f(a: i32) -> i32 { return a }\n'
              "pub fn main() -> i32 { return f(1) }\n", "'@callconv(\"c\")' is accepted")
    expect_ok('@callconv("mirage")\npub fn f(a: i32) -> i32 { return a }\n'
              "pub fn main() -> i32 { return f(1) }\n", "'@callconv(\"mirage\")' is the default and accepted")

    expect_error('@callconv("pascal")\npub fn f() -> i32 { return 1 }\n'
                 "pub fn main() -> i32 { return f() }\n",
                 "unknown calling convention 'pascal'", "an unknown convention is rejected")
    expect_error('@callconv("win64")\npub fn f() -> i32 { return 1 }\n'
                 "pub fn main() -> i32 { return f() }\n",
                 "not supported in v1", "a real-but-unsupported convention gets its own message")
    expect_error("@callconv\npub fn f() -> i32 { return 1 }\npub fn main() -> i32 { return f() }\n",
                 "requires exactly one string argument", "'@callconv' requires its argument")

    expect_error("@cdecl\npub fn f() -> (i32, i32) { return 1, 2 }\n"
                 "pub fn main() -> i32 { const a, b := f()  return a + b }\n",
                 "multi-return", "'@cdecl' on a multi-return function is an error")

    expect_error("@(cdecl, naked)\npub fn f() -> i32 { asm { ret } }\n"
                 "pub fn main() -> i32 { return f() }\n",
                 "no compiler-generated prologue", "'@cdecl' + '@naked' is an error")

    # D2: function-pointer types carry no convention, so the address is refused rather than
    # silently producing a pointer that callers marshal wrongly.
    expect_error("@cdecl\nfn f() -> i32 { return 1 }\n"
                 "pub fn main() -> i32 { const p: fn() -> i32 = f  return p() }\n",
                 "cannot take the address of '@callconv(\"c\")' function",
                 "taking the address of a '@cdecl' function is an error")
    expect_ok("@cdecl\nfn f() -> i32 { return 1 }\npub fn main() -> i32 { return f() }\n",
              "calling a '@cdecl' function directly is still fine")

    expect_error("@cdecl\npub fn f[T: type](v: T) -> i32 { return 1 }\n"
                 "pub fn main() -> i32 { return f(1) }\n",
                 "not allowed on a generic function", "'@cdecl' on a generic is an error")

    # A method's receiver would also have to cross the C boundary, and method call sites do
    # not go through the C-ABI path -- so this is refused rather than accepted and ignored.
    expect_error("pub type T = struct { x: i32 }\n"
                 "impl T {\n  @cdecl\n  pub fn get(self) -> i32 { return self.x }\n}\n"
                 "pub fn main() -> i32 { mut t: T = default  return t.get() }\n",
                 "not supported on impl methods in v1", "'@callconv' on an impl method is rejected")


# ---------------------------------------------------------------- @import

def case_import():
    expect_ok('@import("wasi_snapshot_preview1", "fd_write")\n'
              "ext fn fd_write(fd: i32, iovs: anyptr, n: i32, written: *i32) -> i32\n"
              "pub fn main() -> i32 { return 0 }\n",
              "'@import' with an explicit module and name")
    expect_ok('@import("env")\next fn host_log(p: *u8, n: i32)\n'
              "pub fn main() -> i32 { return 0 }\n",
              "'@import' with only a module (the name defaults to the declaration's)")

    expect_error('@import("env")\npub fn f() -> i32 { return 1 }\n'
                 "pub fn main() -> i32 { return f() }\n",
                 "only allowed on an 'ext fn'", "'@import' on a 'fn' is an error")
    expect_error("@import()\next fn f() -> i32\npub fn main() -> i32 { return f() }\n",
                 "one or two string arguments", "'@import' with no arguments is an error")

    # The carve-out is exactly one attribute wide: everything else stays banned on 'ext fn'.
    expect_error("@naked\next fn puts(s: *u8) -> i32\npub fn main() -> i32 { return 0 }\n",
                 "'@naked' is not allowed on an 'ext fn'", "other attributes on 'ext fn' are still rejected")
    expect_error("@no_discard\next fn puts(s: *u8) -> i32\npub fn main() -> i32 { return 0 }\n",
                 "'@no_discard' is not allowed on an 'ext fn'", "'@no_discard' on 'ext fn' is rejected too")

    # Inert on a native target: one stdlib source file carries the annotation for every target.
    expect_ok('@import("env")\next fn host_log(p: *u8, n: i32)\n'
              "pub fn main() -> i32 { return 0 }\n",
              "'@import' compiles inertly on x86-64")


def main() -> int:
    if not MIRAGE.exists():
        print(f"FAIL: {MIRAGE} not found; run 'just build' first")
        return 1

    case_parser()
    case_no_discard()
    case_export()
    case_callconv()
    case_import()

    print()
    if failures:
        print(f"{failures} failure(s)")
        return 1
    print("all new-attribute tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
