#!/usr/bin/env python3
"""By-ref payload captures ('.Variant(&v):') that outlive their arm.

Codegen emits the match/switch operand as a VALUE into one scratch slot per function, so
'&v' points at a compiler temporary rather than the original object, and the next match
through that slot overwrites it. Sema has no lifetime machinery, so this is diagnosed by a
conservative syntactic check -- and emitted as a WARNING, since it over-reports.

Two things matter and neither is pinnable by tests/examples_expected.json (which keys on
'error:'): that every escape route is caught, and that reading THROUGH the pointer while
the arm is live is not flagged. A check that warns on everything would satisfy the first
alone.

Not wired into ctest. Run it manually after building:

    just build
    python3 tests/payload_capture_test.py
"""

import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
MIRAGE_BINARY = REPO_ROOT / "build" / "mirage"
EXAMPLES = REPO_ROOT / "examples"

ESCAPE_FIXTURE = "example_payload_capture_escape"
SAFE_FIXTURE = "example_tagged_union_scalar_payload"

failures = 0


def check(condition: bool, message: str) -> None:
    global failures
    if not condition:
        failures += 1
        print(f"FAIL: {message}")
    else:
        print(f"ok: {message}")


def build(example_dir: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [str(MIRAGE_BINARY), "build", str(EXAMPLES / example_dir)],
        capture_output=True,
        text=True,
        timeout=60,
    )


def warned_lines(stderr: str) -> set[int]:
    return {
        int(m.group(1))
        for m in re.finditer(r"main\.mir:(\d+):\d+: warning: by-ref payload capture", stderr)
    }


def test_every_escape_route_is_caught() -> None:
    result = build(ESCAPE_FIXTURE)
    check(result.returncode == 0, "an escaping capture is a warning, not an error")

    stderr = result.stderr
    for route in ("an assignment", "a call argument", "a struct field", "a return"):
        check(
            f"escapes its arm via {route}" in stderr,
            f"escape via {route} is reported",
        )

    # Two distinct assignment escapes (a local and a global), the three other direct routes,
    # and the two the check used to miss: one via a local BOUND to the capture, one via a
    # tagged-variant payload.
    check(
        len(warned_lines(stderr)) == 7,
        f"all seven escaping arms are reported, once each (got {sorted(warned_lines(stderr))})",
    )

    check(
        "points at a compiler temporary" in stderr,
        "the warning explains the mechanism rather than just naming the rule",
    )

    check(
        "a variant payload" in stderr,
        "escape via a tagged-variant payload is reported (the arm that was missing)",
    )


def test_the_pointer_is_followed_through_a_local() -> None:
    """The under-report DEFERRED named: 'const p := v' then letting 'p' escape.

    Binding the capture to a local is not itself an escape -- 'p' is arm-local too -- so what
    is checked is that the pointer is FOLLOWED there, not that the binding is flagged. A
    warning must land in escape_via_alias, and none in safe_shadowed_alias, where a later
    declaration of the same name rebinds it to something else.
    """
    result = build("example_payload_capture_escape")
    source = (EXAMPLES / "example_payload_capture_escape" / "main.mir").read_text().splitlines()

    def function_of(line_no: int) -> str:
        for i in range(line_no - 1, -1, -1):
            if source[i].startswith("pub fn "):
                return source[i].split("(")[0].removeprefix("pub fn ").strip()
        return "?"

    warned_functions = {function_of(n) for n in warned_lines(result.stderr)}
    check(
        "escape_via_alias" in warned_functions,
        f"an escape through a local bound to the capture is reported, got {sorted(warned_functions)}",
    )
    check(
        "safe_shadowed_alias" not in warned_functions,
        "a name rebound to something else stops being tracked, so shadowing does not "
        f"produce a false positive, got {sorted(warned_functions)}",
    )


def test_reading_through_the_pointer_is_not_flagged() -> None:
    """'got = v.*' copies out of the slot while the arm is still live: safe, and common.

    A check that flagged this would be unusable -- reading through the pointer is the
    entire reason to capture by reference.
    """
    result = build(ESCAPE_FIXTURE)
    lines = warned_lines(result.stderr)

    source = (EXAMPLES / ESCAPE_FIXTURE / "main.mir").read_text().splitlines()
    safe_line = next(
        i + 1 for i, line in enumerate(source) if "got = v.*" in line
    )
    check(
        safe_line not in lines,
        f"reading through the pointer (line {safe_line}) is not flagged",
    )


def test_existing_corpus_fixture_stays_clean() -> None:
    """The one pre-existing by-ref capture in examples/ does not escape.

    A false positive here would mean the check is too coarse to ship.
    """
    result = build(SAFE_FIXTURE)
    check(result.returncode == 0, f"{SAFE_FIXTURE} still builds")
    check(
        "by-ref payload capture" not in result.stderr,
        f"{SAFE_FIXTURE} produces no escape warning",
    )


def main() -> int:
    if not MIRAGE_BINARY.exists():
        print(f"error: {MIRAGE_BINARY} not found - run 'just build' first", file=sys.stderr)
        return 1

    test_every_escape_route_is_caught()
    test_the_pointer_is_followed_through_a_local()
    test_reading_through_the_pointer_is_not_flagged()
    test_existing_corpus_fixture_stays_clean()

    if failures:
        print(f"\n{failures} failure(s)")
        return 1
    print("\nall payload capture tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
