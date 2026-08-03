#!/usr/bin/env python3
"""Runs every Mirage-language test module under tests/mir/ with 'mirage test'.

This is the migrated half of the corpus. Positive fixtures — the ones that compile and RUN
— live here as '@test' functions, where an assertion says what it expected instead of
encoding it in a process exit code. Negative fixtures, the ones whose whole point is a
specific diagnostic, stay in 'examples/' pinned by tests/examples_expected.json: a program
that does not compile cannot be a test function, so '@test' cannot express them.

This script is a thin driver — it only decides WHICH modules to run and reports the
aggregate. The assertions are Mirage source; add tests by adding '@test' functions to a
module under tests/mir/, or a new directory there.

Needs the standard library for 'core/testing'. Set MIRAGE_STD if it is not beside this
repo.

Not wired into ctest (same posture as the other .py suites). Run manually:

    just build
    python3 tests/mir_suite_test.py
"""

import os
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
MIRAGE = REPO_ROOT / "build" / "mirage"
SUITE_ROOT = REPO_ROOT / "tests" / "mir"
STD = Path(os.environ.get("MIRAGE_STD", REPO_ROOT.parent / "Mirage"))

failures = 0


def main() -> int:
    global failures

    if not MIRAGE.exists():
        print(f"FAIL: {MIRAGE} not found; run 'just build' first")
        return 1
    if not (STD / "core" / "testing").is_dir():
        print(f"FAIL: {STD}/core/testing not found; set MIRAGE_STD to a stdlib checkout")
        return 1

    modules = sorted(d for d in SUITE_ROOT.iterdir() if d.is_dir())
    if not modules:
        print(f"FAIL: no test modules found under {SUITE_ROOT}")
        return 1

    # Both backends (docs/backend.md, validation #2): 'tests/mir' carries real
    # assertions rather than exit-code comparisons, which is exactly what a backend
    # swap needs -- a native miscompile shows up here as a FAILED TEST naming itself,
    # where the differential harness would only report a diverging exit code.
    # The native backend runs under both register allocators (docs/backend.md
    # stage 6): linear is the one under test, trivial the triage baseline.
    variants = (
        ("llvm", []),
        ("native/linear", ["--backend=native", "--regalloc=linear"]),
        ("native/trivial", ["--backend=native", "--regalloc=trivial"]),
    )
    for label, extra in variants:
        for module in modules:
            argv = [str(MIRAGE), "test", str(module), f"--std={STD}"]
            argv.extend(extra if extra else ["--backend=llvm"])
            result = subprocess.run(
                argv, capture_output=True, text=True, timeout=300, cwd=REPO_ROOT,
            )
            # The harness's own table is the useful output; pass it through verbatim
            # rather than re-summarizing it less well.
            sys.stdout.write(result.stdout)
            if result.returncode != 0:
                failures += 1
                print(f"FAIL: {module.name} under {label} "
                      f"exited {result.returncode}")
                # Compilation errors land on stderr and would otherwise be invisible.
                for line in result.stderr.splitlines():
                    if "error:" in line or "warning:" in line:
                        print(f"  {line}")

    print()
    if failures:
        print(f"{failures} module/backend combination(s) with failures")
        return 1
    print(f"all {len(modules)} Mirage test module(s) passed on llvm, native/linear "
          f"and native/trivial")
    return 0


if __name__ == "__main__":
    sys.exit(main())
