#!/usr/bin/env python3
"""Diagnostic/behavior smoke test for inline asm ('asm { ... }') examples.

Shells out to the built 'mirage' binary against each examples/example_asm_* fixture and
asserts on exit code + stderr/stdout content. Not wired into ctest (it exercises whole
example programs end-to-end via subprocess, a different shape of test than
lexer_asi_test/ast_parser_progress_test). Run it manually after building:

    just build
    python3 tests/asm_diagnostics_test.py
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


# Each case: (example dir, mirage action, expected success, required substrings in
# stderr, required substrings in stdout). 'build' only compiles; 'run' compiles and
# executes — used for the happy-path cases where we also want to check program output.
CASES = [
    (
        "example_asm_syscall",
        "run",
        True,
        [],
        ["open succeeded"],
    ),
    (
        "example_asm_movzx",
        "run",
        True,
        [],
        ["widened: 200"],
    ),
    (
        "example_asm_movzx_bad_src_width",
        "build",
        False,
        ["'movzx' source must be 8 or 16 bits (got 32 bits)"],
        [],
    ),
    (
        "example_asm_zero_operand",
        "run",
        True,
        [],
        [],
    ),
    (
        "example_asm_unknown_mnemonic",
        "build",
        True,
        ["unknown mnemonic 'cpuid'"],
        [],
    ),
    (
        "example_asm_width_mismatch",
        "build",
        True,
        ["asm operand 'fd' has type 'i32' (32 bits) but register 'rax' is 64 bits", "Consider using 'eax' instead"],
        [],
    ),
    (
        "example_asm_unknown_var",
        "build",
        False,
        ["unknown identifier 'unknown_name' in asm block"],
        [],
    ),
    (
        "example_asm_struct_operand",
        "build",
        False,
        ["only scalar and pointer types are supported in asm operands"],
        [],
    ),
    (
        "example_asm_module_scope",
        "build",
        False,
        ["asm blocks are only legal inside function bodies"],
        [],
    ),
    (
        "example_asm_sse_register",
        "build",
        False,
        ["register 'xmm0' is not supported in inline asm (v1)"],
        [],
    ),
    (
        "example_asm_memory_operand",
        "build",
        False,
        ["memory operands with displacement/scale syntax", "not supported in inline asm (v1)"],
        [],
    ),
    # 'asm -> reg [: type] { ... }' — the expression form (see docs/spec.md's
    # "Asm Expression" subsection).
    (
        "example_asm_expr_open_forms",
        "run",
        True,
        [],
        ["all four asm-expr forms succeeded"],
    ),
    (
        "example_asm_expr_cannot_infer",
        "build",
        False,
        ["cannot infer result type for 'asm -> eax'"],
        [],
    ),
    (
        "example_asm_expr_width_mismatch",
        "build",
        True,
        ["asm result register 'rax' is 64 bits but result type 'i32' is 32 bits", "Consider using 'eax' to avoid implicit truncation"],
        [],
    ),
    (
        "example_asm_expr_unknown_register",
        "build",
        False,
        ["expected a register name after 'asm ->'"],
        [],
    ),
    (
        "example_asm_expr_sse_register",
        "build",
        False,
        ["register 'xmm0' is not supported in inline asm (v1)"],
        [],
    ),
    (
        "example_asm_expr_mixed_output",
        "run",
        True,
        [],
        ["mixed output ok: 42 42"],
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

    print(f"\n{failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
