#!/usr/bin/env python3
"""Diagnostic/behavior smoke test for ignorable errors ('?error(...)') — the
examples/example_optional_error* fixtures.

Shells out to the built 'mirage' binary against each fixture and asserts on exit code +
stderr/stdout content — same shape as tests/attribute_diagnostics_test.py. What
tests/examples_expected.json cannot pin, and this can, is the TEXT: the synthesized panic
message names the failing variant and the call site, and each of the three placement rules
has its own diagnostic. Not wired into ctest. Run it manually after building:

    just build
    python3 tests/error_optional_test.py
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
# stderr, required substrings in stdout). 'build' only compiles; 'run' compiles and
# executes. The panic fixture is expected to DIE, hence exit 101.
CASES = [
    (
        "example_optional_error_panic",
        "run",
        101,
        # The variant name is resolved at runtime out of the error value; the location is
        # baked in per call site. Only the basename is printed, so this is stable across
        # machines even though SourceLocation carries an absolute path.
        ["panic: unhandled Alloc_Error.Out_Of_Memory at main.mir:"],
        ["small alloc survived"],
    ),
    (
        "example_optional_error_not_last",
        "build",
        1,
        ["'?' may only mark the LAST return type"],
        [],
    ),
    (
        "example_optional_error_non_error",
        "build",
        1,
        ["'?' requires an error type", "got 'i32'", "got 'Point'"],
        [],
    ),
    (
        "example_optional_error_position",
        "build",
        1,
        ["'?' is only allowed on a function's last return type"],
        [],
    ),
    (
        "example_optional_error_trait_mismatch",
        "build",
        1,
        # '?' is type identity, so this falls out of the ordinary signature comparison —
        # the printed types must show the marker for the message to make any sense.
        ["does not match trait", "?error(Alloc_Error)"],
        [],
    ),
    (
        "example_optional_error",
        "run",
        0,
        [],
        [
            "w=3 h=4",
            # The '_' opt-out really is silent: this call failed and the program lived.
            "ignored a failure and kept going",
            # A lone '?error(...)' in a value context is captured, not dropped.
            "touch reported a failure, as expected",
            "relay succeeded",
        ],
    ),
    (
        "example_optional_error_alias",
        "run",
        0,
        [],
        ["all conversions ok", "write error"],
    ),
    (
        "example_optional_error_trait",
        "run",
        0,
        [],
        ["static dispatch ok", "dynamic dispatch ok", "handled the failure explicitly"],
    ),
    (
        "example_optional_error_fnptr",
        "run",
        0,
        [],
        ["local fn ptr: p is nil = 1", "struct field: q is nil = 1", "done"],
    ),
]


def main() -> int:
    if not MIRAGE_BINARY.exists():
        print(f"error: {MIRAGE_BINARY} not found — run 'just build' first", file=sys.stderr)
        return 1

    for example_dir, action, expected_exit, stderr_substrings, stdout_substrings in CASES:
        result = run_mirage(action, example_dir)
        check(
            result.returncode == expected_exit,
            f"{example_dir}: exit code {result.returncode} == {expected_exit}",
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

    # The panic must abort the program, not merely report: nothing after the failing call
    # may run.
    result = run_mirage("run", "example_optional_error_panic")
    check(
        "unreachable" not in result.stdout,
        "example_optional_error_panic: execution stops at the panic",
    )

    # One panic helper per error union TYPE, not per call site — example_optional_error
    # drops the same '?Alloc_Error' at five different places.
    ir = subprocess.run(
        [str(MIRAGE_BINARY), "build", str(EXAMPLES / "example_optional_error"), "--emit-mir"],
        capture_output=True,
        text=True,
        timeout=30,
    )
    helper_definitions = [
        line for line in ir.stdout.splitlines()
        if line.startswith("fn ") and "__mirage_panic_unhandled_error" in line
    ]
    check(
        len(helper_definitions) == 1,
        f"example_optional_error: exactly one panic helper emitted (got {len(helper_definitions)})",
    )

    # Freestanding builds link -nostdlib, so the panic path must reach the kernel
    # itself rather than calling libc write()/exit(). The native backend emits the
    # syscall sequence as inline asm; a libc call here would fail to link, which is
    # exactly the bug this pins (it was live until the LLVM removal forced the
    # native path to own the freestanding case too).
    freestanding = subprocess.run(
        [str(MIRAGE_BINARY), "build", str(EXAMPLES / "example_optional_error"),
         "--emit-mir", "--freestanding"],
        capture_output=True,
        text=True,
        timeout=30,
    )
    panic_body = freestanding.stdout.split("__mirage_panic_unhandled_error", 1)
    panic_body = panic_body[1] if len(panic_body) > 1 else ""
    check(
        "call @write" not in panic_body and "call @exit" not in panic_body,
        "example_optional_error: freestanding panic path does not call libc write()/exit()",
    )
    check(
        "asm" in panic_body and "syscall.arg" in freestanding.stdout,
        "example_optional_error: freestanding panic path emits syscalls instead",
    )

    print()
    print(f"{failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
