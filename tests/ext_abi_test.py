#!/usr/bin/env python3
"""SysV x86-64 ABI test for by-value aggregates crossing an 'ext fn' boundary.

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

# 34 from sum_arr(3.0, 4.0), 123 from sum_big(1, 2, 3), 45 from sum_two_words(4, 5).
EXPECTED_EXIT = 34 + 123 + 45

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
        # The declaration must be SysV-coerced, not passed as a raw aggregate. A by-value
        # '[2]f32' is one SSE eightbyte and must lower exactly like 'struct { float x, y; }'.
        ir = subprocess.run(
            [str(MIRAGE_BINARY), "build", str(FIXTURE), "--emit-ir"],
            capture_output=True, text=True, timeout=60, cwd=REPO_ROOT,
        )
        check("declare float @sum_arr(<2 x float>)" in ir.stdout,
              "by-value [2]f32 is coerced to <2 x float>, not passed as [2 x float]")

        result = subprocess.run(
            [str(MIRAGE_BINARY), "run", str(FIXTURE)],
            capture_output=True, text=True, timeout=120, cwd=REPO_ROOT,
        )
        check(result.returncode == EXPECTED_EXIT,
              f"aggregates cross the C boundary intact: exit {result.returncode} == {EXPECTED_EXIT}")
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
