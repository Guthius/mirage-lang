#!/usr/bin/env python3
"""Error-typestate narrowing: which condition shapes narrow, and what the rejection says.

tests/examples_expected.json pins these two fixtures by exit code and first diagnostic.
What it cannot pin, and this can, is that each of the three causes of "unknown state"
produces its OWN explanation — they used to share one generic "check it first", which is
the thing the user has usually already done.

Not wired into ctest. Run it manually after building:

    just build
    python3 tests/error_narrowing_test.py
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
        timeout=60,
    )


def test_and_chain_narrows() -> None:
    """Every bare 'err'/'!err' operand of a '&&' chain narrows that variable.

    Before this, only the whole condition or the LEFTMOST operand of a single '&&'/'||'
    was recognized, so 'flag && err' and 'err1 && err2' narrowed nothing and the following
    'match err' was rejected. The fixture returns the number of the first failing check.
    """
    result = run_mirage("run", "example_error_narrow_and_chain")
    check(
        result.returncode == 0,
        f"example_error_narrow_and_chain runs clean (exit {result.returncode}, "
        f"stderr: {result.stderr.strip()[:400]})",
    )


def test_unnarrowable_shapes_are_explained() -> None:
    """Each cause of "unknown state" names itself.

    All three are in one fixture, so a regression that collapses them back to a single
    generic message fails here rather than silently passing the corpus harness (which only
    pins the FIRST diagnostic).
    """
    result = run_mirage("build", "example_error_narrow_unknown")
    check(result.returncode != 0, "example_error_narrow_unknown is rejected")

    stderr = result.stderr

    check(
        "'||' proves nothing about either side" in stderr,
        "a '||' condition explains that it narrows neither operand",
    )
    check(
        "only 'err', '!err', or an operand of a '&&' chain" in stderr,
        "the condition-shape diagnostic names the shapes that DO narrow",
    )
    check(
        "its address was taken" in stderr,
        "'&err' explains that taking the address invalidated the state",
    )
    check(
        "left it in different states" in stderr,
        "a disagreeing join explains that neither branch's state survived",
    )

    # The three must be distinct: a regression that reports one cause for all of them
    # would still satisfy the substring checks above if they collapsed onto one message.
    unknown_state_lines = {
        line.split("unknown state", 1)[1]
        for line in stderr.splitlines()
        if "unknown state" in line
    }
    check(
        len(unknown_state_lines) == 3,
        f"the three causes produce three distinct explanations (got {len(unknown_state_lines)})",
    )


def test_or_still_does_not_narrow() -> None:
    """'||' is deliberately NOT widened.

    An operand of a '||' being true says nothing about any other operand, so the
    then-branch proves nothing -- including about the leftmost one, which earlier versions
    did report a (Unknown/Unknown) narrowing for. That entry is not a no-op: it DEGRADES a
    previously-known state, which is the conservative and correct thing to do.
    """
    result = run_mirage("build", "example_error_narrow_unknown")
    check(
        "main.mir:23" in result.stderr,
        "the '||' case is still rejected (at its own line)",
    )


def main() -> int:
    if not MIRAGE_BINARY.exists():
        print(f"error: {MIRAGE_BINARY} not found - run 'just build' first", file=sys.stderr)
        return 1

    test_and_chain_narrows()
    test_unnarrowable_shapes_are_explained()
    test_or_still_does_not_narrow()

    if failures:
        print(f"\n{failures} failure(s)")
        return 1
    print("\nall error narrowing tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
