#!/usr/bin/env python3
"""Command-line argument handling.

Covers the cases where the driver used to accept malformed input silently: a flag missing
its value, or a stray positional token that stopped argument parsing so every later flag was
ignored. Both produced a build that looked successful but did not do what was asked.

Also pins '--help' exiting 0, which it did not when the argc gate ran before parsing.

Not wired into ctest (see asm_diagnostics_test.py for the same posture). Run it manually
after building:

    just build
    python3 tests/cli_test.py
"""

import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
MIRAGE_BINARY = REPO_ROOT / "build" / "mirage"
START_EXAMPLE = REPO_ROOT / "examples" / "start"

failures = 0


def check(condition: bool, message: str) -> None:
    global failures
    if not condition:
        failures += 1
        print(f"FAIL: {message}")
    else:
        print(f"ok: {message}")


def run(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [str(MIRAGE_BINARY), *args],
        capture_output=True, text=True, timeout=120, cwd=REPO_ROOT,
    )


# (argv, expected exit code, substring expected in stderr). Every malformed form must be
# rejected loudly rather than proceeding with a default.
MALFORMED = [
    (["build", str(START_EXAMPLE), "-o"], "requires an output filename"),
    (["build", str(START_EXAMPLE), "--output"], "requires an output filename"),
    (["build", str(START_EXAMPLE), "-l"], "requires a library name"),
    (["build", str(START_EXAMPLE), "--opt"], "requires an argument"),
    (["build", str(START_EXAMPLE), "--opt", "novalue"], "key=value"),
    (["build", str(START_EXAMPLE), "extra", "-o", "/tmp/mirage-cli-test"], "unexpected argument"),
    (["badaction", str(START_EXAMPLE)], "unknown action"),
]


def main() -> int:
    if not MIRAGE_BINARY.exists():
        print(f"error: {MIRAGE_BINARY} not found — run 'just build' first", file=sys.stderr)
        return 1

    for argv, expected_text in MALFORMED:
        result = run(*argv)
        label = " ".join(a.replace(str(REPO_ROOT), ".") for a in argv)
        check(result.returncode == 1, f"'{label}' exits 1 (got {result.returncode})")
        check(expected_text in result.stderr, f"'{label}' explains why ({expected_text!r})")

    # --help must exit 0 even as the only argument. The argc gate used to run before
    # parse_options, so this printed usage and exited 1.
    for flag in ("--help", "-h"):
        result = run(flag)
        check(result.returncode == 0, f"'{flag}' alone exits 0 (got {result.returncode})")

    # No arguments at all is still a usage error.
    check(run().returncode == 1, "no arguments exits 1")

    # A valid invocation is unaffected; examples/start returns 42.
    result = run("run", str(START_EXAMPLE))
    check(result.returncode == 42, f"'run examples/start' still exits 42 (got {result.returncode})")

    print()
    if failures:
        print(f"{failures} failure(s)")
        return 1
    print("all CLI tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
