#!/usr/bin/env python3
"""Pins '--nortti' and the '$rtti_enabled' compile-time constant.

The corpus gate (examples_smoke_test.py) pins each fixture under ONE configuration, and
every claim here is about the DIFFERENCE between two: the same source compiled with and
without '--nortti'. That is why these live in their own suite.

The load-bearing case is the interaction with 'when'. 'when' type-checks both branches --
a deliberate language rule, unlike generics -- so a 'type_info_of' call in the branch
written specifically to be dead under '--nortti' would still be checked, and would still
hit the "reflection is disabled" error, making the feature unusable in a single file. The
error is suppressed in a statically-dead branch, narrowly: same flag, same diagnostic, and
nothing else about the branch goes unchecked.

Not wired into ctest (same posture as the other .py suites). Run manually:

    just build
    python3 tests/nortti_test.py
"""

import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
MIRAGE = REPO_ROOT / "build" / "mirage"

failures = 0


def check(condition: bool, message: str) -> None:
    global failures
    if not condition:
        failures += 1
        print(f"FAIL: {message}")
    else:
        print(f"ok: {message}")


def run(args, timeout=120):
    return subprocess.run(
        [str(MIRAGE)] + args, cwd=str(REPO_ROOT),
        capture_output=True, text=True, timeout=timeout,
    )


def build(example, *extra):
    return run(["build", f"examples/{example}", "-o", "/dev/null", *extra])


def emit_ir(example, *extra):
    return run(["build", f"examples/{example}", "--emit-ir", *extra])


def execute(example, *extra):
    return run(["run", f"examples/{example}", *extra])


def case_guarded_when_compiles_both_ways():
    ok = execute("example_nortti_guarded")
    check(ok.returncode == 4,
          f"guarded 'when': rtti branch runs by default (exit {ok.returncode}, want 4)")

    off = execute("example_nortti_guarded", "--nortti")
    check(off.returncode == 0,
          f"guarded 'when': --nortti takes the reflection-free branch (exit {off.returncode}, want 0)")
    check("requires runtime type information" not in off.stderr,
          "guarded 'when': --nortti does not report the disabled-reflection error for the dead branch")


def case_unguarded_call_is_an_error():
    ok = build("example_nortti_error")
    check(ok.returncode == 0, "unguarded 'type_info_of' builds by default")

    off = build("example_nortti_error", "--nortti")
    check(off.returncode != 0, "unguarded 'type_info_of' under --nortti is an error")
    check("requires runtime type information" in off.stderr,
          "the --nortti error names runtime type information as the missing thing")
    check("$rtti_enabled" in off.stderr,
          "the --nortti error points at '$rtti_enabled' as the way to write a guarded variant")


def case_type_info_globals_are_suppressed():
    """The point of the flag: no Type_Info constants and no lookup table in the binary.

    Checked on emitted IR rather than on behaviour, because a program can take the
    reflection-free path at RUNTIME while the compiler still emitted every Type_Info
    global -- which is exactly the bug this test was written after finding. Sema's three
    types_needing_info registration sites ('type_of', 'type_info_of(type_of(T))', and
    'any' coercion) all gate on the same helper for that reason.
    """
    symbols = ("@.type_info", "@__mirage_type_info_table")

    on = emit_ir("example_nortti_guarded")
    check(any(s in on.stdout for s in symbols),
          "with rtti: the fixture really does emit Type_Info globals (guards the negative below)")

    off = emit_ir("example_nortti_guarded", "--nortti")
    check(off.returncode == 0, "--nortti IR emission succeeds")
    for s in symbols:
        check(s not in off.stdout, f"--nortti emits no '{s}'")


def case_any_still_works_without_rtti():
    """'any' is not RTTI: the value carries a bare u64 id and a cast compares ids.

    'any' coercion is one of the three types_needing_info registration sites, so gating it
    on the flag could plausibly have broken 'any' itself. This pins that it did not.
    """
    for example in ("example_any_cast", "example_any_basic"):
        if not (REPO_ROOT / "examples" / example).is_dir():
            continue
        on = execute(example)
        off = execute(example, "--nortti")
        check(on.returncode == off.returncode,
              f"{example}: --nortti does not change behaviour (exit {on.returncode} vs {off.returncode})")


def case_rtti_enabled_is_an_ordinary_constant():
    on = execute("example_nortti_const")
    check(on.returncode == 1, f"'$rtti_enabled' folds to true by default (exit {on.returncode}, want 1)")
    off = execute("example_nortti_const", "--nortti")
    check(off.returncode == 0, f"'$rtti_enabled' folds to false under --nortti (exit {off.returncode}, want 0)")


def case_type_info_module_not_required():
    """Under --nortti a program need not have 'runtime/type_info' in its import graph."""
    ok = build("example_nortti_no_import")
    check(ok.returncode != 0 and "Type_Info" in ok.stderr,
          "without --nortti the guarded branch is live and demands 'Type_Info' (guards the positive below)")

    off = build("example_nortti_no_import", "--nortti")
    check(off.returncode == 0,
          f"--nortti compiles with no 'runtime/type_info' anywhere in the graph ({off.stderr.strip()[:150]})")


def case_parser_diagnostics():
    """'$rtti_enabled' is nullary, and '$' still rejects an unknown name."""
    import tempfile, os

    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "main.mir"
        src.write_text("pub fn main() -> i32 { return $rtti_enabled(1) ? 1 : 0 }\n")
        r = run(["build", tmp, "-o", "/dev/null"])
        check(r.returncode != 0 and "'$rtti_enabled' takes no arguments" in r.stderr,
              "'$rtti_enabled(...)' is rejected with a message about arguments")

        src.write_text("pub fn main() -> i32 { return $nonsense ? 1 : 0 }\n")
        r2 = run(["build", tmp, "-o", "/dev/null"])
        check(r2.returncode != 0 and "expected 'option', 'env' or 'rtti_enabled' after '$'" in r2.stderr,
              "an unknown '$' name lists all three accepted spellings")


def main() -> int:
    if not MIRAGE.exists():
        print(f"FAIL: {MIRAGE} not found; run 'just build' first")
        return 1

    case_guarded_when_compiles_both_ways()
    case_unguarded_call_is_an_error()
    case_type_info_globals_are_suppressed()
    case_any_still_works_without_rtti()
    case_rtti_enabled_is_an_ordinary_constant()
    case_type_info_module_not_required()
    case_parser_diagnostics()

    print()
    if failures:
        print(f"{failures} failure(s)")
        return 1
    print("all --nortti / $rtti_enabled tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
