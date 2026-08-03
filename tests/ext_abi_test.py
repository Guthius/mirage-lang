#!/usr/bin/env python3
"""Per-target ABI test for by-value aggregates crossing an 'ext fn' boundary.

Unlike the other suites, this one links Mirage code against C compiled by the system clang,
so the values the callee sees are produced by an independent implementation of the ABI rather
than by Mirage agreeing with itself. That is the only way to catch a coercion bug: an
incorrect but self-consistent lowering passes every Mirage-only test.

Lives here rather than in examples/ because it needs a C compilation step, which the corpus
runner (tests/examples_smoke_test.py) has no way to perform.

Not wired into ctest (see asm_diagnostics_test.py for the same posture). Run it manually
after building:

    just build
    python3 tests/ext_abi_test.py
"""

import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
MIRAGE_BINARY = REPO_ROOT / "build" / "mirage"
FIXTURE = Path(__file__).resolve().parent / "ext_abi_fixture"

# 34 from sum_arr(3.0, 4.0), 123 from sum_big(1, 2, 3), 45 from sum_two_words(4, 5),
# 2006 from sum_straddle(2, 6). Taken mod 256 by the shell on the way out.
EXPECTED_EXIT = (34 + 123 + 45 + 2006) % 256

failures = 0


def check(condition: bool, message: str) -> None:
    global failures
    if not condition:
        failures += 1
        print(f"FAIL: {message}")
    else:
        print(f"ok: {message}")


def main() -> int:
    if not MIRAGE_BINARY.exists():
        print(f"error: {MIRAGE_BINARY} not found — run 'just build' first", file=sys.stderr)
        return 1
    if shutil.which("clang") is None:
        print("error: clang not found; this test needs it to build the C side", file=sys.stderr)
        return 1

    # '#link(lib, "helper.o")' resolves relative to the module directory, so the object has to
    # land next to main.mir.
    helper_o = FIXTURE / "helper.o"
    build = subprocess.run(
        ["clang", "-c", "-O1", "-o", str(helper_o), str(FIXTURE / "helper.c")],
        capture_output=True, text=True, timeout=60,
    )
    check(build.returncode == 0, f"C helper compiles ({build.stderr.strip()[:80]})")
    if build.returncode != 0:
        return 1

    try:
        # The declaration must be SysV-classified, not passed as a raw aggregate.
        # Asserted on MIR, which now records the ABI decision directly (the LLVM
        # IR this used to grep is gone): a by-value '[2]f32' is ONE SSE eightbyte
        # and must lower exactly like 'struct { float x, y; }'.
        ir = subprocess.run(
            [str(MIRAGE_BINARY), "build", str(FIXTURE), "--emit-mir"],
            capture_output=True, text=True, timeout=60, cwd=REPO_ROOT,
        )
        check("@sum_arr(f64)" in ir.stdout,
              "by-value [2]f32 is classified as one SSE eightbyte, not passed indirectly")
        check("@sum_two_words(i64, i64)" in ir.stdout,
              "a two-eightbyte struct passes as two INTEGER words")
        # A packed field straddling an eightbyte boundary forces MEMORY class; classifying the
        # two halves independently would put it in registers instead.
        check("@sum_straddle(ptr byval(" in ir.stdout,
              "straddling packed struct is passed byval (MEMORY class), as clang does")
        check("@sum_big(ptr byval(24, align 8))" in ir.stdout,
              "a >16-byte struct is passed byval at its own size and alignment")

        # Both allocators: the native path implements SysV aggregate
        # classification itself (mirgen's C lowering), and this run pins it
        # against C code compiled by clang.
        for label, extra in (
            ("linear", ["--regalloc=linear"]),
            ("trivial", ["--regalloc=trivial"]),
        ):
            result = subprocess.run(
                [str(MIRAGE_BINARY), "run", str(FIXTURE), *extra],
                capture_output=True, text=True, timeout=120, cwd=REPO_ROOT,
            )
            check(result.returncode == EXPECTED_EXIT,
                  f"[{label}] aggregates cross the C boundary intact: "
                  f"exit {result.returncode} == {EXPECTED_EXIT}")

        # The same fixture under the WebAssembly ABI, which has no register-packing
        # tier: every one of these is a multi-element aggregate, so all four go
        # indirectly (clang's wasm rule — a recursively-single-scalar aggregate
        # would pass directly). The contrast with the x86 shapes above is the whole
        # point; '--emit-mir' stops before the link, so the x86 helper.o is
        # irrelevant here.
        wasm_ir = subprocess.run(
            [str(MIRAGE_BINARY), "build", str(FIXTURE), "--target=wasm32-unknown-emscripten", "--emit-mir"],
            capture_output=True, text=True, timeout=60, cwd=REPO_ROOT,
        )
        check(wasm_ir.returncode == 0, "wasm32 build of the ABI fixture emits MIR cleanly")
        check("@sum_arr(ptr)" in wasm_ir.stdout,
              "wasm32: by-value [2]f32 goes indirectly, NOT packed into one word as on x86-64")
        check("@sum_big(ptr)" in wasm_ir.stdout and "@sum_two_words(ptr)" in wasm_ir.stdout,
              "wasm32: multi-field structs pass by reference")
        check("byval(" not in wasm_ir.stdout,
              "wasm32: no byval stack copies — the pointer itself IS the argument")
    finally:
        helper_o.unlink(missing_ok=True)

    print()
    if failures:
        print(f"{failures} failure(s)")
        return 1
    print("all ext-fn ABI tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
