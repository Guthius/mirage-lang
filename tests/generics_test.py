#!/usr/bin/env python3
"""Diagnostic/behavior smoke test for parametric generics ('examples/example_generics_*').

Shells out to the built 'mirage' binary against each fixture and asserts on exit code +
stdout/stderr content. Not wired into ctest (see asm_diagnostics_test.py for the same
posture). Run it manually after building:

    just build
    python3 tests/generics_test.py
"""

import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
MIRAGE_BINARY = REPO_ROOT / "build" / "mirage"
EXAMPLES = REPO_ROOT / "examples"

failures = 0


def check(condition: bool, message: str) -> None:
    global failures
    if not condition:
        failures += 1
        print(f"FAIL: {message}")
    else:
        print(f"ok: {message}")


def run_mirage(action: str, example_dir: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [str(MIRAGE_BINARY), action, str(EXAMPLES / example_dir)],
        capture_output=True,
        text=True,
        timeout=30,
    )


# Each case: (example dir, mirage action, expected exit code, required substrings in
# stdout, required substrings in stderr). 'build' only compiles (used for the rejection
# cases, where we only care about the diagnostic text); 'run' compiles and executes.
CASES = [
    # Positive-path cases — cover deliverable #5's required coverage list.
    ("example_generics_basic", "run", 30, ["sum = 30"], []),
    ("example_generics_const", "run", 12, ["a[0]=5 b[0]=7"], []),
    ("example_generics_mixed", "run", 10, ["sum = 10"], []),
    ("example_generics_self_instantiation", "run", 42, ["value = 42"], []),
    ("example_generics_infer_arg_unify", "run", 9, ["x = 9"], []),
    (
        "example_generics_rtti",
        "run",
        0,
        ["box: is_generic=1 args=1", "plain: is_generic=0 args=0"],
        [],
    ),
    # Rejection cases — each new restriction/coherence error from spec.md §22.
    (
        "example_generics_orphan_impl",
        "build",
        1,
        [],
        ["impl generic parameter lists must match the target type's own arity exactly"],
    ),
    (
        "example_generics_impl_arity_mismatch",
        "build",
        1,
        [],
        ["impl generic parameter lists must match the target type's own arity exactly"],
    ),
    (
        "example_generics_bad_param_type",
        "build",
        1,
        [],
        ["declared type must be 'type' or a builtin scalar type"],
    ),
    (
        "example_generics_missing_args",
        "build",
        1,
        [],
        ["used without generic arguments"],
    ),
    (
        "example_generics_infer_fail",
        "build",
        1,
        [],
        ["could not infer generic parameter"],
    ),
]


def main() -> int:
    if not MIRAGE_BINARY.exists():
        print(f"error: {MIRAGE_BINARY} not found — run 'just build' first", file=sys.stderr)
        return 1

    for example_dir, action, expected_code, stdout_substrings, stderr_substrings in CASES:
        result = run_mirage(action, example_dir)
        check(
            result.returncode == expected_code,
            f"{example_dir}: exit code {result.returncode} == {expected_code}",
        )
        for s in stdout_substrings:
            check(s in result.stdout, f"{example_dir}: stdout contains {s!r}")
        for s in stderr_substrings:
            check(s in result.stderr, f"{example_dir}: stderr contains {s!r}")

    print()
    if failures:
        print(f"{failures} failure(s)")
        return 1
    print("all generics tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
