#!/usr/bin/env python3
"""'#compile_only_if' file-level conditional compilation: the feature's definition of done.

examples/example_compile_only_if is a module with two platform files declaring the same
'pub fn platform_name()' under opposite '#compile_only_if' conditions. This suite pins:

  - '--opt build/target_os=...' selects which file's symbols exist: the program exits
    with len(platform_name()) — 5 ("linux") vs 4 ("wasm");
  - an excluded file contributes no '#link' directives;
  - an excluded file's symbols and strings never reach the emitted IR;
  - an excluded file is STILL fully type-checked — a deliberate type error injected
    into it fails the build even when the build excludes it (the anti-rot design);
  - an excluded file's trait impls are registered while IT is checked, so a platform
    file can return its own type as the trait it implements (example_..._trait);
  - the directive's own error shapes: duplicate directive, statement position,
    non-constant condition, 'pub' on the directive.

Not wired into ctest (see asm_diagnostics_test.py for the same posture). Run it
manually after building:

    just build
    python3 tests/compile_only_if_test.py
"""

import shutil
import subprocess
import sys
import tempfile
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


def mirage(action: str, directory: Path, *extra: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [str(MIRAGE_BINARY), action, str(directory), *extra],
        capture_output=True,
        text=True,
        timeout=60,
        cwd=REPO_ROOT,
    )


def test_target_selects_platform_file() -> None:
    example = EXAMPLES / "example_compile_only_if"

    result = mirage("run", example)
    check(result.returncode == 5, f"default (Linux) build runs linux_only's platform_name (exit {result.returncode} == 5)")

    result = mirage("run", example, "--opt", "build/target_os=Wasm32")
    check(result.returncode == 4, f"Wasm32 build runs wasm_only's platform_name (exit {result.returncode} == 4)")


def test_excluded_file_contributes_no_links() -> None:
    example = EXAMPLES / "example_compile_only_if"

    result = mirage("build", example, "--print-link-directives")
    check(result.returncode == 0, "Linux build lists link directives cleanly")
    check("system m" in result.stdout, "Linux build collects linux_only's '#link(system, m)'")
    check("pthread" not in result.stdout, "Linux build does NOT collect the excluded wasm file's '#link'")

    result = mirage("build", example, "--print-link-directives", "--opt", "build/target_os=Wasm32")
    check("pthread" in result.stdout, "Wasm32 build collects wasm_only's '#link(system, pthread)'")
    check("system m" not in result.stdout, "Wasm32 build does NOT collect the excluded linux file's '#link'")


def test_excluded_file_absent_from_ir() -> None:
    result = mirage("build", EXAMPLES / "example_compile_only_if", "--emit-ir")
    check(result.returncode == 0, "Linux build emits IR cleanly (llvm::verifyModule runs inside codegen)")
    check("linux" in result.stdout, "IR contains the included file's 'linux' string")
    check("wasm" not in result.stdout and "js_console_log" not in result.stdout,
          "IR contains nothing from the excluded wasm file (no symbols, no strings, no ext decls)")


def test_excluded_file_is_still_type_checked() -> None:
    # The permanent always-excluded fixture.
    result = mirage("build", EXAMPLES / "example_compile_only_if_excluded_error", "-o", "/dev/null")
    check(result.returncode != 0, "type error in an always-excluded file fails the build")
    check("unknown identifier 'undefined_identifier'" in result.stderr
          and "excluded.mir" in result.stderr,
          "the diagnostic names the excluded file's undefined identifier")

    # The definition-of-done shape: inject a deliberate error into wasm_only.mir and
    # build for Linux (which excludes it) — the diagnostic must still appear.
    with tempfile.TemporaryDirectory() as tmp:
        copy = Path(tmp) / "example"
        shutil.copytree(EXAMPLES / "example_compile_only_if", copy)
        with open(copy / "wasm_only.mir", "a", encoding="utf-8") as f:
            f.write("\npub fn rotted() -> i32 {\n    return undefined_identifier_xyz\n}\n")
        result = mirage("build", copy, "-o", "/dev/null")
        check(result.returncode != 0, "injected type error in the excluded wasm file fails a Linux build")
        check("undefined_identifier_xyz" in result.stderr, "the injected error's identifier is named")


def test_excluded_file_trait_impl() -> None:
    """example_compile_only_if_trait: each platform file implements one trait for its own
    type and returns that type AS the trait. Both directions must build, which is only
    possible if the EXCLUDED file's impl is registered while that file is type-checked —
    it is what its own 'return &platform_reporter' coerces through."""
    example = EXAMPLES / "example_compile_only_if_trait"

    result = mirage("run", example)
    check(result.returncode == 5, f"Linux build runs linux_impl's Reporter (exit {result.returncode} == 5)")

    result = mirage("run", example, "--opt", "build/target_os=Wasm32")
    check(result.returncode == 4, f"Wasm32 build runs wasm_impl's Reporter (exit {result.returncode} == 4)")

    result = mirage("build", example, "--emit-ir")
    check(result.returncode == 0, "Linux build emits IR cleanly")
    check("Wasm_Reporter" not in result.stdout,
          "the excluded file's impl reaches no vtable or symbol in the emitted IR")


def test_directive_error_shapes() -> None:
    result = mirage("build", EXAMPLES / "example_compile_only_if_duplicate", "-o", "/dev/null")
    check(result.returncode != 0 and "a file may only have one '#compile_only_if' directive." in result.stderr,
          "two directives in one file: sema error")

    result = mirage("build", EXAMPLES / "example_compile_only_if_stmt", "-o", "/dev/null")
    check(result.returncode != 0
          and "'#compile_only_if' is a file-level directive and may only appear at module scope." in result.stderr,
          "directive in a function body: parse error")

    result = mirage("build", EXAMPLES / "example_compile_only_if_nonconst", "-o", "/dev/null")
    check(result.returncode != 0
          and "'#compile_only_if' condition must be a compile-time constant expression." in result.stderr,
          "non-constant condition: sema error")

    result = mirage("build", EXAMPLES / "example_compile_only_if_nonbool", "-o", "/dev/null")
    check(result.returncode != 0
          and "'#compile_only_if' condition must be a 'bool' expression." in result.stderr,
          "non-bool constant condition: sema error")

    result = mirage("build", EXAMPLES / "example_compile_only_if_pub", "-o", "/dev/null")
    check(result.returncode != 0 and "'#compile_only_if' directives cannot be 'pub'" in result.stderr,
          "'pub #compile_only_if': parse error")


def main() -> int:
    if not MIRAGE_BINARY.exists():
        print(f"error: {MIRAGE_BINARY} not found — run 'just build' first", file=sys.stderr)
        return 1

    test_target_selects_platform_file()
    test_excluded_file_contributes_no_links()
    test_excluded_file_absent_from_ir()
    test_excluded_file_is_still_type_checked()
    test_excluded_file_trait_impl()
    test_directive_error_shapes()

    print()
    if failures:
        print(f"{failures} failure(s)")
        return 1
    print("all #compile_only_if tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
