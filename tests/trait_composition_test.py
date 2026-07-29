#!/usr/bin/env python3
"""Diagnostic/behavior smoke test for trait composition ('examples/example_trait_composition_*').

Shells out to the built 'mirage' binary against each fixture and asserts on exit code +
stdout/stderr content. Not wired into ctest (see asm_diagnostics_test.py for the same
posture). Run it manually after building:

    just build
    python3 tests/trait_composition_test.py
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
# stdout, required substrings in stderr). 'build' only compiles (used for the rejection
# cases and warning-only cases, where we only care about diagnostic text); 'run' compiles
# and executes, checking the compiled program's own exit code.
CASES = [
    # --- Parser: bare trait (regression guard), composition-list arities/bodies ---
    ("example_trait_composition_bare", "run", 9, [], []),
    ("example_trait_composition_single_no_body", "run", 5, [], []),
    ("example_trait_composition_multi_with_body", "run", 0, [], []),
    ("example_trait_composition_multi_no_body", "run", 0, [], []),
    (
        "example_trait_composition_empty_body_rejected",
        "build",
        1,
        [],
        ["trait must declare at least one method"],
    ),
    # --- Sema: cycle detection ---
    (
        "example_trait_composition_cycle_2",
        "build",
        1,
        [],
        ["circular trait composition: 'B' composes 'A', which composes 'B'"],
    ),
    (
        "example_trait_composition_cycle_self",
        "build",
        1,
        [],
        ["circular trait composition: 'A' composes 'A'"],
    ),
    (
        "example_trait_composition_cycle_transitive",
        "build",
        1,
        [],
        ["circular trait composition: 'B' composes 'C', which composes 'A', which composes 'B'"],
    ),
    # --- Sema: diamond merge (positive — no error, dispatch still correct) ---
    ("example_trait_composition_diamond", "run", 0, [], []),
    # --- Sema: signature-collision errors ---
    (
        "example_trait_composition_collision",
        "build",
        1,
        [],
        [
            "trait 'Z' composes both 'X' and 'Y', which each declare 'run' with incompatible "
            "signatures ('fn(self) -> i32' vs 'fn(self) -> bool'). Rename one of them to disambiguate."
        ],
    ),
    (
        "example_trait_composition_collision_own",
        "build",
        1,
        [],
        [
            "trait 'Z' declares 'run' itself and also composes 'X', which declares 'run' with an "
            "incompatible signature ('fn(self) -> bool' vs 'fn(self) -> i32'). Rename one of them to disambiguate."
        ],
    ),
    # --- Sema: redundant single-composition warning firing / not firing ---
    (
        "example_trait_composition_redundant_warn",
        "run",
        0,
        [],
        [
            "trait 'X' composes only 'Y' and declares no methods of its own, making it identical "
            "to 'Y'. Either remove 'X' and use 'Y' directly, or declare it as a type alias:",
            "pub type X = Y",
        ],
    ),
    (
        "example_trait_composition_redundant_not_fired_two_composed",
        "run",
        0,
        [],
        [],
    ),
    (
        "example_trait_composition_redundant_not_fired_own_method",
        "run",
        0,
        [],
        [],
    ),
    # --- Coherence/orphan rules still enforced on a composed trait ---
    (
        "example_trait_composition_orphan_still_enforced",
        "build",
        1,
        [],
        ["orphan impl: 'impl Composed for Impl' must be declared in the module that defines 'Composed' or the module that defines 'Impl'"],
    ),
    (
        "example_trait_composition_coherence_still_enforced",
        "build",
        1,
        [],
        ["duplicate impl of trait 'Composed' for type 'Impl'"],
    ),
    # --- Codegen/runtime: Stream/Reader/Writer/Seekable — direct pointer-to-component
    # coercion, handle-to-handle narrowing, correct dispatch per component, data-pointer
    # identity preserved across coercion (mutation visible through the original struct).
    (
        "example_trait_composition_stream",
        "run",
        0,
        [
            "direct read: 100",
            "narrowed read: 100",
            "after write, through original struct: 200",
            "seek result: 42, through original struct: 42",
        ],
        [],
    ),
]

# Warning text that must NOT appear for the two "not fired" redundancy cases above — checked
# separately from CASES' required-substrings list, which only supports positive assertions.
NOT_FIRED_CASES = [
    ("example_trait_composition_redundant_not_fired_two_composed", "composes only"),
    ("example_trait_composition_redundant_not_fired_own_method", "composes only"),
]


def main() -> int:
    if not MIRAGE_BINARY.exists():
        print(f"error: {MIRAGE_BINARY} not found — run 'just build' first", file=sys.stderr)
        return 1

    for example_dir, action, expected_code, stdout_substrings, stderr_substrings in CASES:
        result = run_mirage(action, example_dir)
        check(
            result.returncode == expected_code,
            f"{example_dir}: exit code {result.returncode} == {expected_code}",
        )
        for s in stdout_substrings:
            check(s in result.stdout, f"{example_dir}: stdout contains {s!r}")
        for s in stderr_substrings:
            check(s in result.stderr, f"{example_dir}: stderr contains {s!r}")

    for example_dir, absent_substring in NOT_FIRED_CASES:
        result = run_mirage("run", example_dir)
        check(
            absent_substring not in result.stderr,
            f"{example_dir}: stderr does NOT contain {absent_substring!r} (redundancy warning must not fire)",
        )

    print()
    if failures:
        print(f"{failures} failure(s)")
        return 1
    print("all trait composition tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
