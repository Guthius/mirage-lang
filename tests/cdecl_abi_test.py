#!/usr/bin/env python3
"""Callee-side ABI test for '@callconv("c")' / '@cdecl' — the mirror of ext_abi_test.py.

ext_abi_test.py checks Mirage-as-caller against a C callee. This one checks
Mirage-as-CALLEE: the fixture's functions are all '@(export, cdecl)', C calls into them,
and clang produces the argument marshalling. A callee-side coercion bug is invisible to
ext_abi_test.py, because there Mirage never has to take a coerced aggregate apart —
it only ever builds one.

The struct shapes hit each tier of the System V x86-64 classification (one INTEGER
eightbyte, one SSE eightbyte, two eightbytes, and MEMORY class), in both directions, since
passing and returning use different machinery (byval vs sret, coerce-in vs coerce-out).

Not wired into ctest (see ext_abi_test.py for the same posture). Run it manually after
building:

    just build
    python3 tests/cdecl_abi_test.py
"""

import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
MIRAGE_BINARY = REPO_ROOT / "build" / "mirage"
FIXTURE = Path(__file__).resolve().parent / "cdecl_abi_fixture"

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

    # '#link(lib, "helper.o")' resolves relative to the module directory, so the object has
    # to land next to main.mir.
    helper_o = FIXTURE / "helper.o"
    build = subprocess.run(
        ["clang", "-c", "-O1", "-o", str(helper_o), str(FIXTURE / "helper.c")],
        capture_output=True, text=True, timeout=60,
    )
    check(build.returncode == 0, f"C helper compiles ({build.stderr.strip()[:80]})")
    if build.returncode != 0:
        return 1

    try:
        ir = subprocess.run(
            [str(MIRAGE_BINARY), "build", str(FIXTURE), "--emit-ir"],
            capture_output=True, text=True, timeout=60, cwd=REPO_ROOT,
        )
        check(ir.returncode == 0, "the '@cdecl' fixture emits IR cleanly")

        # The DEFINITIONS must carry the C signature, not the raw Mirage one. Checking the
        # emitted shapes as well as the runtime behaviour catches the case where two
        # symmetric bugs cancel out and the program still produces the right answer.
        check("define i32 @mir_pair_sum(i64" in ir.stdout,
              "a one-INTEGER-eightbyte struct parameter is coerced to i64")
        check("define <2 x float> @mir_vec2_make(float" in ir.stdout,
              "a one-SSE-eightbyte struct return is coerced to <2 x float>")
        check("@mir_big_sum(ptr byval" in ir.stdout,
              "a >16-byte struct parameter is passed byval (MEMORY class)")
        check("@mir_big_make(ptr sret" in ir.stdout,
              "a >16-byte struct return uses a hidden sret pointer")
        # '@export' means the mangled module-path name is gone entirely.
        check("__mir_" not in ir.stdout.split("define")[1] if "define" in ir.stdout else False,
              "'@export' emits the bare name, not the module-mangled one")

        # The actual round trip. Mirage's 'main' forwards the C side's failure count, so a
        # non-zero exit is the number of mismatched checks and stderr names each one.
        result = subprocess.run(
            [str(MIRAGE_BINARY), "run", str(FIXTURE)],
            capture_output=True, text=True, timeout=120, cwd=REPO_ROOT,
        )
        detail = "\n".join(line for line in result.stderr.splitlines() if "cdecl ABI FAIL" in line)
        check(result.returncode == 0,
              f"C calls into '@cdecl' Mirage functions and every value survives (exit {result.returncode})"
              + (f"\n{detail}" if detail else ""))
    finally:
        helper_o.unlink(missing_ok=True)

    print()
    if failures:
        print(f"{failures} failure(s)")
        return 1
    print("all '@cdecl' callee-side ABI tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
