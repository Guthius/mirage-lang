#!/usr/bin/env python3
"""'--noinit' behavior test for the '@init' declaration attribute.

Builds examples/example_attr_init_basic both normally and with '--noinit', then runs each
resulting binary and diffs the output — proving '--noinit' actually suppresses the
synthesized '_init' call (the normal build prints "both ready"; the '--noinit' build must
NOT, since neither module's '@init' function ever runs). Needs a non-default CLI invocation
the plain examples/example_attr_* convention (see tests/attribute_diagnostics_test.py) can't
express, hence a dedicated script. Not wired into ctest — run manually after building:

    just build
    python3 tests/attribute_init_test.py
"""

import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
MIRAGE_BINARY = REPO_ROOT / "build" / "mirage"
FIXTURE = REPO_ROOT / "examples" / "example_attr_init_basic"

failures = 0


def check(condition: bool, message: str) -> None:
    global failures
    if not condition:
        failures += 1
        print(f"FAIL: {message}")
    else:
        print(f"ok: {message}")


def build_and_run(extra_args: list[str], out_path: Path) -> subprocess.CompletedProcess:
    build = subprocess.run(
        [str(MIRAGE_BINARY), "build", str(FIXTURE), "-o", str(out_path), *extra_args],
        capture_output=True,
        text=True,
        timeout=30,
    )
    if build.returncode != 0:
        return build
    return subprocess.run([str(out_path)], capture_output=True, text=True, timeout=30)


def main() -> int:
    if not MIRAGE_BINARY.exists():
        print(f"FAIL: {MIRAGE_BINARY} does not exist — run 'just build' first")
        return 1

    with tempfile.TemporaryDirectory() as tmp:
        normal_out = Path(tmp) / "normal.out"
        noinit_out = Path(tmp) / "noinit.out"

        normal = build_and_run([], normal_out)
        check(normal.returncode == 0, f"normal build+run succeeds, got exit {normal.returncode}")
        check("both ready" in normal.stdout, f"normal build prints 'both ready', got {normal.stdout!r}")

        noinit = build_and_run(["--noinit"], noinit_out)
        check(noinit.returncode == 0, f"--noinit build+run succeeds, got exit {noinit.returncode}")
        check("both ready" not in noinit.stdout, f"--noinit build does NOT print 'both ready', got {noinit.stdout!r}")
        check("NOT ready" in noinit.stdout, f"--noinit build prints 'NOT ready' instead, got {noinit.stdout!r}")

    print(f"\n{failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
