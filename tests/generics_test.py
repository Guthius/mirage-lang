#!/usr/bin/env python3
"""Diagnostic/behavior smoke test for parametric generics ('examples/example_generics_*').

Shells out to the built 'mirage' binary against each fixture and asserts on exit code +
stdout/stderr content. Not wired into ctest (see asm_diagnostics_test.py for the same
posture). Run it manually after building:

    just build
    python3 tests/generics_test.py
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
# cases, where we only care about the diagnostic text); 'run' compiles and executes.
CASES = [
    # Positive-path cases — cover deliverable #5's required coverage list.
    ("example_generics_basic", "run", 30, ["sum = 30"], []),
    ("example_generics_const", "run", 12, ["a[0]=5 b[0]=7"], []),
    ("example_generics_mixed", "run", 10, ["sum = 10"], []),
    ("example_generics_self_instantiation", "run", 42, ["value = 42"], []),
    ("example_generics_infer_arg_unify", "run", 9, ["x = 9"], []),
    (
        "example_generics_rtti",
        "run",
        0,
        ["box: is_generic=1 args=1", "plain: is_generic=0 args=0"],
        [],
    ),
    # Generic functions as values + generic params in scope inside default parameter values
    # (spec.md §22). Each of these exercised a distinct failure before: a generic instantiation
    # in value position, a bare function name decaying with no expected type, and the enclosing
    # declaration's own generic parameters being visible to a default expression.
    (
        "example_generics_default_fn_value",
        "run",
        7,
        ["h=4 h_wide=8 h_inferred=4 eq_yes=1 eq_no=0"],
        [],
    ),
    (
        "example_generics_default_type_param",
        "run",
        12,
        ["zeroes[0]=0 copied[0]=1 from_default=6 from_arg=6"],
        [],
    ),
    (
        "example_generics_default_size_of",
        "run",
        21,
        ["w8=8 w4=4 w_explicit=1 a8=8"],
        [],
    ),
    # A monomorphized generic struct's field defaults on codegen's runtime braced-initializer
    # path — these used to lower to a zero-width 'i0' and fail LLVM verification.
    (
        "example_generics_struct_field_default",
        "run",
        12,
        ["plain: cap=0 count=0 sealed=0 slot0=7", "sized: cap=4 count=0"],
        [],
    ),
    # Non-generic half of function-name decay.
    (
        "example_fnptr_inferred",
        "run",
        25,
        ["via_local=10 defaulted=6 overridden=-3 twice=12"],
        [],
    ),
    # A generic instantiated behind a type alias whose declaring module is reached only through
    # a NAMED import, so it is not force-declared. find_type_decl_for read "module not declared
    # yet" as "not a generic decl" and laid the template body out unbound, reporting
    # "unknown type 'T'" against the declaration and caching the wreckage.
    (
        "example_generics_cross_module_alias",
        "run",
        30,
        ["a=10 b=20"],
        [],
    ),
    # Rejection cases — each new restriction/coherence error from spec.md §22.
    (
        "example_generics_orphan_impl",
        "build",
        1,
        [],
        ["impl generic parameter lists must match the target type's own arity exactly"],
    ),
    (
        "example_generics_impl_arity_mismatch",
        "build",
        1,
        [],
        ["impl generic parameter lists must match the target type's own arity exactly"],
    ),
    (
        "example_generics_bad_param_type",
        "build",
        1,
        [],
        # A NAMED type in this position is now accepted on shape and then checked for
        # trait-ness, so the message names the actual problem instead of restating the rule.
        # Both the type decl's 'E' and the fn decl's 'F' are reported.
        [
            "generic parameter 'E' is bound by 'Color', which is not a trait",
            "generic parameter 'F' is bound by 'Color', which is not a trait",
        ],
    ),
    # --- Eager checking of generic declarations -------------------------------------
    # A generic body is type-checked once at its declaration, with parameters bound to
    # Opaque, so a mistake surfaces there rather than at the first (or no) instantiation.
    (
        "example_generics_eager_unknown_call",
        "build",
        1,
        [],
        ["unknown function 'nonexistent_helper'"],
    ),
    # The other half: an UNCONSTRAINED parameter must not be reported for anything that
    # depends on it. Regression guard against the eager pass flagging correct code.
    ("example_generics_eager_unbound_ok", "run", 7, [], []),
    (
        "example_generics_eager_impl_method",
        "build",
        1,
        [],
        ["unknown function 'missing_helper'"],
    ),
    # A realistic generic container whose impl methods route everything through 'self'.
    # Every operation here was a false positive from the eager pass at some point.
    ("example_generics_eager_container", "run", 0, [], []),
    # One generic at two distinct argument sets — per-instance expression records.
    ("example_generics_two_instances", "run", 30, [], []),
    # --- Trait bounds '[T: Hashable]' -----------------------------------------------
    ("example_generics_bound_ok", "run", 42, [], []),
    (
        "example_generics_bound_method_missing",
        "build",
        1,
        [],
        ["no method 'size' on a generic parameter bound by 'Hashable'"],
    ),
    (
        "example_generics_bound_arith_rejected",
        "build",
        1,
        [],
        ["cannot apply arithmetic to a generic parameter bound by 'Hashable'"],
    ),
    (
        "example_generics_bound_not_impl",
        "build",
        1,
        [],
        ["'Plain' does not implement trait 'Hashable', required by generic parameter 'T'"],
    ),
    (
        "example_generics_bound_not_a_trait",
        "build",
        1,
        [],
        ["is bound by 'Point', which is not a trait"],
    ),
    (
        "example_generics_missing_args",
        "build",
        1,
        [],
        ["used without generic arguments"],
    ),
    (
        "example_generics_infer_fail",
        "build",
        1,
        [],
        ["could not infer generic parameter"],
    ),
    # A generic function named as a value needs explicit generic arguments — there is nothing to
    # infer from in value position. Also pins the absence of the phantom "unknown type 'T'"
    # cascade this mistake used to emit against the callee's own (correct) declaration.
    (
        "example_generics_fn_value_missing_args",
        "build",
        1,
        [],
        ["is a generic function; supply its generic arguments"],
    ),
    (
        "example_generics_fn_value_variadic",
        "build",
        1,
        [],
        ["cannot take the address of variadic function"],
    ),
    # Pins the diagnostic that REPLACED "did you mean to call it?" once function names started
    # decaying to function pointers.
    (
        "example_fnptr_bad_context",
        "build",
        1,
        [],
        ["arithmetic is not allowed on function pointer types"],
    ),
    # A compiler diagnostic must SPELL an unbound generic parameter, not print "<generic>".
    # Asserted as two separate substrings so a regression that reverts describe_type's Opaque
    # arm fails on the second even if some other type in the message happens to contain "T".
    #
    # Hard to reach on purpose: TypeKind::Opaque is deliberately permissive, so it suppresses
    # nearly every type mismatch that would otherwise render it — '?T' is one of the few
    # rejections that survives. Verified by instrumenting describe_type and sweeping the whole
    # corpus: no other fixture reaches it with a named Opaque.
    (
        "example_generics_param_name_in_diagnostic",
        "build",
        1,
        [],
        ["'?' requires an error type", "got 'T'"],
    ),
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

    print()
    if failures:
        print(f"{failures} failure(s)")
        return 1
    print("all generics tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
