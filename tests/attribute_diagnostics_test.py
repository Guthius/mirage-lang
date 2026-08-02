#!/usr/bin/env python3
"""Diagnostic/behavior smoke test for the declaration-attribute system's examples/example_attr_*
fixtures ('@no_return', '@naked', '@always_inline', '@section', '@init').

Shells out to the built 'mirage' binary against each fixture and asserts on exit code +
stderr/stdout content — same shape as tests/asm_diagnostics_test.py. Not wired into ctest. Run
it manually after building:

    just build
    python3 tests/attribute_diagnostics_test.py
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


# Each case: (example dir, mirage action, expected success, required substrings in stderr,
# required substrings in stdout). 'build' only compiles; 'run' compiles and executes — used
# for the happy-path/runtime-behavior cases where we also want to check program output.
CASES = [
    (
        "example_attr_no_return",
        "build",
        True,
        ["a '@no_return' function with a value return type will never return its value"],
        [],
    ),
    (
        "example_attr_naked",
        "run",
        True,
        [],
        [],
    ),
    (
        "example_attr_naked_non_asm_body",
        "build",
        True,
        ["'@naked' function contains a non-'asm' statement"],
        [],
    ),
    (
        "example_attr_always_inline_address_taken",
        "build",
        True,
        ["taking the address of '@always_inline' function 'add'"],
        [],
    ),
    (
        "example_attr_section",
        "build",
        True,
        [],
        [],
    ),
    (
        "example_attr_section_empty",
        "build",
        False,
        ["'@section' argument must not be an empty string"],
        [],
    ),
    (
        "example_attr_unknown",
        "build",
        False,
        ["unknown attribute '@frobulate'. Known attributes: no_return, naked, always_inline, section, init, no_discard, export, callconv, cdecl, import, test."],
        [],
    ),
    (
        "example_attr_conflict_init_no_return",
        "build",
        False,
        ["'@init' and '@no_return' cannot be combined"],
        [],
    ),
    (
        "example_attr_init_basic",
        "run",
        True,
        [],
        ["both ready"],
    ),
    (
        "example_attr_init_params",
        "build",
        False,
        ["'@init' functions may not take parameters"],
        [],
    ),
    (
        "example_attr_init_private",
        "run",
        True,
        [],
        ["private init ran"],
    ),
    (
        "example_attr_init_bad_return",
        "build",
        False,
        ["'@init' functions must return nothing or 'error(...)'"],
        [],
    ),
    (
        "example_attr_init_on_method",
        "build",
        False,
        ["'@init' is not allowed on impl methods"],
        [],
    ),
    (
        "example_attr_naked_method",
        "build",
        False,
        ["cannot use argument of naked function"],
        [],
    ),
    (
        "example_attr_naked_method_non_asm_body",
        "build",
        False,
        [
            "'@naked' function contains a non-'asm' statement",
            "cannot use argument of naked function",
        ],
        [],
    ),
    (
        "example_attr_section_method",
        "build",
        True,
        [],
        [],
    ),
    (
        "example_attr_section_method_empty",
        "build",
        False,
        ["'@section' argument must not be an empty string"],
        [],
    ),
    (
        "example_attr_section_trait_method",
        "build",
        True,
        [],
        [],
    ),
    (
        "example_attr_naked_always_inline_method",
        "build",
        False,
        ["'@naked' and '@always_inline' cannot be combined"],
        [],
    ),
    (
        "example_attr_init_error_return",
        "run",
        False,
        [],
        [],
    ),
    (
        "example_attr_init_cycle",
        "build",
        False,
        ["circular '@init' dependency"],
        [],
    ),
]


def main() -> int:
    if not MIRAGE_BINARY.exists():
        print(f"FAIL: {MIRAGE_BINARY} does not exist — run 'just build' first")
        return 1

    for example_dir, action, expect_success, stderr_substrings, stdout_substrings in CASES:
        result = run_mirage(action, example_dir)
        actual_success = result.returncode == 0

        check(
            actual_success == expect_success,
            f"{example_dir}: expected {'success' if expect_success else 'failure'} "
            f"(exit {result.returncode}, action={action})",
        )
        for substring in stderr_substrings:
            check(
                substring in result.stderr,
                f"{example_dir}: stderr contains {substring!r}",
            )
        for substring in stdout_substrings:
            check(
                substring in result.stdout,
                f"{example_dir}: stdout contains {substring!r}",
            )

    # example_attr_init_error_return additionally must exit with exactly code 1 (the fixed
    # sentinel a failing '@init' terminates with) and must never reach 'main' — if it did,
    # "main ran" would appear in stdout.
    result = run_mirage("run", "example_attr_init_error_return")
    check(result.returncode == 1, f"example_attr_init_error_return: exits with code 1, got {result.returncode}")
    check("main ran" not in result.stdout, "example_attr_init_error_return: 'main' never ran")

    print(f"\n{failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
