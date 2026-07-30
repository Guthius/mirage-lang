#!/usr/bin/env python3
"""Whole-corpus regression gate over every fixture directory in 'examples/'.

The other test scripts in this directory each pin a hand-picked handful of fixtures.
This one pins *all* of them: it walks 'examples/*/' (one level only) and compares each
directory's outcome against a checked-in baseline, 'tests/examples_expected.json'.
That baseline is what makes "no regressions" a checkable claim rather than a hope --
roughly a third of the corpus is referenced by no other test at all.

Not wired into ctest (see asm_diagnostics_test.py for the same posture). Run it
manually after building:

    just build
    python3 tests/examples_smoke_test.py

Flags:
    --strict            treat a changed diagnostic message as a failure, not a warning
    --update-baseline   re-derive tests/examples_expected.json from current behavior
    --only NAME         restrict the run to a single example directory (repeatable)

Re-blessing the baseline is never a standalone commit: run --update-baseline as part
of the fix that changed the behavior, and review the resulting JSON hunk like any
other diff.
"""

import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
MIRAGE_BINARY = REPO_ROOT / "build" / "mirage"
EXAMPLES = REPO_ROOT / "examples"
BASELINE = Path(__file__).resolve().parent / "examples_expected.json"

DEFAULT_TIMEOUT = 60

failures = 0
warnings = 0


def check(condition: bool, message: str) -> None:
    global failures
    if not condition:
        failures += 1
        print(f"FAIL: {message}")
    else:
        print(f"ok: {message}")


def warn(message: str) -> None:
    global warnings
    warnings += 1
    print(f"warn: {message}")


# Directories whose action cannot be inferred from the source alone. Each entry may set
# "action", "extra_args" and "timeout"; everything else is auto-derived as usual.
#
# 'example_raylib_link' emits '#link(lib, "linux/libraylib.a")', which the driver
# resolves relative to the example directory (src/main.cpp, LinkCategory::Lib). That
# archive is not vendored here, so the example can never link in this repo -- extra
# '-l' flags do not help, because the unresolved path is passed to clang positionally
# regardless. Its own header comment names --print-link-directives as the intended way
# to verify it, so that is what we pin.
SPECIAL_CASES: dict[str, dict] = {
    "example_raylib_link": {"action": "link-directives"},
    # Build-only by design: its own header says "Deliberately not run (only built) — actually
    # executing this would perform an 8-byte write into a 4-byte stack slot". Auto-derivation
    # cannot see that (it compiles and has a 'fn main'), so it ran the program and pinned the
    # resulting crash as a plain exit code. It really does die with SIGSEGV.
    "example_asm_width_mismatch": {"action": "build"},
    # Parsing 'examples/lexer/token.mir' never terminates: line 16 uses a postfix
    # 'kind match { ... }' form that docs/grammar.md:646 does not define (match_expr is
    # prefix-only), and the parser spins on it instead of reporting a syntax error.
    # Pinned as a timeout with a short clock so the suite does not burn a full minute
    # on it every run. Drop the timeout override once the parser rejects the form.
    "lexer": {"action": "emit-ir", "timeout": 5},
}

# Actions, in the order auto-derivation tries them:
#   "emit-ir"         mirage build <dir> --emit-ir --freestanding
#   "link-directives" mirage build <dir> --print-link-directives
#   "build"           mirage build <dir> -o <tmp>       (compile + link only)
#   "run"             mirage run <dir>                  (compile, link, execute)
ACTIONS = ("emit-ir", "link-directives", "build", "run")


def example_dirs() -> list[Path]:
    return sorted(p for p in EXAMPLES.iterdir() if p.is_dir())


def has_main(directory: Path) -> bool:
    """True if any .mir file directly in this directory declares 'fn main'.

    Submodule subdirectories are deliberately not searched: a module is exactly the
    .mir files in one directory, so a 'fn main' in a subdirectory belongs to a
    different module and would not satisfy the linker for this one.
    """
    for mir in directory.glob("*.mir"):
        text = mir.read_text(encoding="utf-8", errors="replace")
        if "fn main" in text:
            return True
    return False


def run_action(directory: Path, action: str, extra_args: list[str], timeout: int):
    """Invoke the compiler for one example. Returns (exit_code, stdout, stderr).

    cwd is always REPO_ROOT: examples/test_io opens "Justfile" by relative path, and
    pinning the cwd keeps every fixture's behavior independent of where this script is
    invoked from.
    """
    with tempfile.TemporaryDirectory() as tmp:
        if action == "emit-ir":
            # --freestanding suppresses codegen.cpp's "hosted build requires
            # 'pub fn main()'" check, which fires for library-style modules even under
            # --emit-ir. Without it every such module pins at exit 1, which would mask
            # a real regression behind an already-failing exit code; with it they pin
            # at exit 0 and any new front-end or codegen error flips them red.
            argv = [str(MIRAGE_BINARY), "build", str(directory), "--emit-ir", "--freestanding"]
        elif action == "link-directives":
            argv = [str(MIRAGE_BINARY), "build", str(directory), "--print-link-directives"]
        elif action == "build":
            argv = [str(MIRAGE_BINARY), "build", str(directory), "-o", str(Path(tmp) / "out")]
        elif action == "run":
            argv = [str(MIRAGE_BINARY), "run", str(directory)]
        else:
            raise ValueError(f"unknown action {action!r}")
        argv.extend(extra_args)
        try:
            result = subprocess.run(
                argv,
                capture_output=True,
                text=True,
                timeout=timeout,
                cwd=REPO_ROOT,
            )
        except subprocess.TimeoutExpired:
            return "timeout", "", ""
    return result.returncode, result.stdout, result.stderr


def first_diagnostic(stderr: str) -> str | None:
    """The first 'error:' line from stderr, made machine-independent.

    The line:col is deliberately kept. Changes that move a diagnostic's underline are
    common (operator diagnostics moved onto the operator token; bitset flag errors moved
    from the whole literal onto the offending flag), and the whole point of pinning this
    text is that such a move shows up as a reviewable diff instead of passing silently.
    """
    prefix = f"{REPO_ROOT}/"
    for line in stderr.splitlines():
        if "error:" in line:
            return line.replace(prefix, "").strip()
    return None


def derive_entry(directory: Path) -> dict:
    """Work out how this example should be exercised, and what it currently does."""
    name = directory.name
    special = SPECIAL_CASES.get(name, {})
    extra_args = list(special.get("extra_args", []))
    timeout = special.get("timeout", DEFAULT_TIMEOUT)

    if "action" in special:
        action = special["action"]
    elif not has_main(directory):
        # A library-style module: exercise parse + sema + codegen, but stop short of
        # the link, which would fail on an undefined 'main'.
        action = "emit-ir"
    else:
        code, _, stderr = run_action(directory, "build", extra_args, timeout)
        if code != 0:
            # An intentional-rejection fixture (or genuinely broken). Pin the failure.
            entry = {"action": "build", "exit": code}
            diag = first_diagnostic(stderr)
            if diag:
                entry["diag"] = diag
            if extra_args:
                entry["extra_args"] = extra_args
            if timeout != DEFAULT_TIMEOUT:
                entry["timeout"] = timeout
            return entry
        action = "run"

    code, _, stderr = run_action(directory, action, extra_args, timeout)
    entry = {"action": action, "exit": code}
    diag = first_diagnostic(stderr)
    if diag:
        entry["diag"] = diag
    if extra_args:
        entry["extra_args"] = extra_args
    if timeout != DEFAULT_TIMEOUT:
        entry["timeout"] = timeout
    return entry


def update_baseline(only: list[str]) -> int:
    existing = json.loads(BASELINE.read_text()) if BASELINE.exists() else {}
    dirs = example_dirs()
    if only:
        dirs = [d for d in dirs if d.name in only]
    for directory in dirs:
        entry = derive_entry(directory)
        # 'known_broken' is a human annotation recording that a fixture was already
        # failing before this campaign started. Never derived, never dropped.
        previous = existing.get(directory.name, {})
        if "known_broken" in previous:
            entry["known_broken"] = previous["known_broken"]
        existing[directory.name] = entry
        print(f"{directory.name}: {entry['action']} exit={entry['exit']}")
    BASELINE.write_text(json.dumps(existing, indent=2, sort_keys=True) + "\n")
    print(f"\nwrote {BASELINE.relative_to(REPO_ROOT)} ({len(existing)} entries)")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--strict", action="store_true", help="diagnostic drift fails")
    parser.add_argument("--update-baseline", action="store_true")
    parser.add_argument("--only", action="append", default=[], metavar="NAME")
    args = parser.parse_args()

    if not MIRAGE_BINARY.exists():
        print(f"error: {MIRAGE_BINARY} not found — run 'just build' first", file=sys.stderr)
        return 1

    if args.update_baseline:
        return update_baseline(args.only)

    if not BASELINE.exists():
        print(f"error: {BASELINE} not found — run with --update-baseline first", file=sys.stderr)
        return 1

    expected = json.loads(BASELINE.read_text())
    dirs = example_dirs()
    on_disk = {d.name for d in dirs}

    if not args.only:
        for name in sorted(on_disk - expected.keys()):
            check(False, f"{name}: present in examples/ but has no baseline entry")
        for name in sorted(expected.keys() - on_disk):
            check(False, f"{name}: stale baseline entry, directory no longer exists")

    for directory in dirs:
        name = directory.name
        if args.only and name not in args.only:
            continue
        entry = expected.get(name)
        if entry is None:
            continue

        action = entry["action"]
        extra_args = list(entry.get("extra_args", []))
        timeout = entry.get("timeout", DEFAULT_TIMEOUT)
        code, _, stderr = run_action(directory, action, extra_args, timeout)

        label = f"{name} [{action}]"
        if entry.get("known_broken"):
            label += " (known-broken)"
        check(code == entry["exit"], f"{label}: exit {code} == {entry['exit']}")

        expected_diag = entry.get("diag")
        actual_diag = first_diagnostic(stderr)
        if expected_diag != actual_diag:
            message = f"{label}: diagnostic changed\n    was: {expected_diag}\n    now: {actual_diag}"
            if args.strict:
                check(False, message)
            else:
                warn(message)

    print()
    if warnings:
        print(f"{warnings} diagnostic change(s) — re-run with --strict to fail on these,")
        print("or with --update-baseline to re-bless them as part of your fix commit.")
    if failures:
        print(f"{failures} failure(s)")
        return 1
    print("all example fixtures match the baseline")
    return 0


if __name__ == "__main__":
    sys.exit(main())
