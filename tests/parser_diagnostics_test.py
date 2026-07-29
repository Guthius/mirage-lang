#!/usr/bin/env python3
"""Parser diagnostic-location tests.

Asserts that diagnostics anchored on an operator underline the operator itself rather
than whatever follows it. Eight expression-parsing functions in ast.cpp used to read
current_location() *after* consuming the operator, so the reported column was the start
of the right-hand operand.

Not wired into ctest (see asm_diagnostics_test.py for the same posture). Run it manually
after building:

    just build
    python3 tests/parser_diagnostics_test.py
"""

import re
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


# examples/example_parse_operator_location/main.mir pads every line so that the operator
# under test begins at column 19. Each entry is (line number, operator) for the assertion
# message; the expected column is 19 for all of them.
OPERATOR_LINES = [
    (21, "&"),
    (22, "^"),
    (23, "|"),
    (24, "&&"),
    (25, "||"),
    (26, "in"),
    (27, "?:"),
    (28, "when...else"),
]

EXPECTED_COLUMN = 19


def main() -> int:
    if not MIRAGE_BINARY.exists():
        print(f"error: {MIRAGE_BINARY} not found — run 'just build' first", file=sys.stderr)
        return 1

    example = EXAMPLES / "example_parse_operator_location"
    result = subprocess.run(
        [str(MIRAGE_BINARY), "build", str(example), "-o", "/dev/null"],
        capture_output=True,
        text=True,
        timeout=30,
        cwd=REPO_ROOT,
    )
    check(result.returncode != 0, "example_parse_operator_location fails to build")

    # Collect "<path>:<line>:<col>: error:" occurrences keyed by line.
    reported: dict[int, int] = {}
    for match in re.finditer(r"main\.mir:(\d+):(\d+): .*error", result.stderr):
        line, column = int(match.group(1)), int(match.group(2))
        reported.setdefault(line, column)

    for line, operator in OPERATOR_LINES:
        column = reported.get(line)
        check(
            column == EXPECTED_COLUMN,
            f"'{operator}' on line {line} underlines the operator "
            f"(column {column} == {EXPECTED_COLUMN})",
        )

    print()
    if failures:
        print(f"{failures} failure(s)")
        return 1
    print("all parser diagnostic tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
