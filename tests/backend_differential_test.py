#!/usr/bin/env python3
"""Differential test: '--backend=llvm' vs '--backend=native' over the positive corpus.

This is the primary safety net for the whole LLVM-replacement effort
(docs/backend.md, "Validation" #1), written BEFORE the native object path exists —
deliberately, because the harness is most valuable exactly when the new backend is
newest. For every positive fixture in tests/examples_expected.json it compiles (and,
for runnable fixtures, executes) the program under both backends and compares exit
code and stdout.

Four outcomes per fixture:

    match         both backends agree — the only passing state once stage 4 lands
    MISMATCH      both produced a result and they differ — always a failure
    native n/a    the native backend refused with its own stage-4 message — tolerated
                  and counted, so this suite is green today and tightens automatically
                  as object generation lands; the summary line makes the remaining
                  distance visible on every run
    not lowered   mirgen refused with a NAMED 'cannot lower X yet' diagnostic — the
                  loud-refusal contract working as designed (today that is exactly
                  the inline-asm fixtures, which need the stage-5 encoder); tolerated
                  and counted separately so growth here is visible as a regression

A fixture where the native backend fails with anything else (a verifier complaint, a
crash, an unnamed error) is a failure: stage 2 claims every other positive program
lowers, and this is where that claim gets audited against the whole corpus.

Not wired into ctest (needs a built compiler). Run manually:

    just build
    python3 tests/backend_differential_test.py
"""

import json
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
MIRAGE = REPO_ROOT / "build" / "mirage"
EXAMPLES = REPO_ROOT / "examples"
BASELINE = Path(__file__).resolve().parent / "examples_expected.json"

# The exact refusal emitted while object generation is unimplemented (src/main.cpp's
# '--backend=native' block). Anything else out of the native backend is a real result
# or a real bug, never a skip.
NATIVE_UNAVAILABLE = "the native backend cannot produce objects yet"

# Fixtures that deliberately print an UNSPECIFIED value, so the two backends may
# legitimately disagree on stdout. Each entry names the line in the fixture that says
# so; exit codes are still compared.
EXIT_CODE_ONLY = {
    # "val2 is undefined on the Failed path" — its own comment. LLVM leaves stack
    # garbage there, the native backend leaves a deterministic zero; both are valid
    # instances of unspecified.
    "example_fnptr3",
}

failures = 0


def fail(message: str) -> None:
    global failures
    failures += 1
    print(f"FAIL: {message}")


def run_backend(directory: Path, action: str, backend: str, timeout: int,
                regalloc: str | None = None):
    extra = [f"--regalloc={regalloc}"] if regalloc else []
    with tempfile.TemporaryDirectory() as tmp:
        if action == "run":
            argv = [str(MIRAGE), "run", str(directory), f"--backend={backend}", *extra]
        else:
            argv = [str(MIRAGE), "build", str(directory), "-o", str(Path(tmp) / "out"),
                    f"--backend={backend}", *extra]
        try:
            result = subprocess.run(argv, capture_output=True, text=True,
                                    timeout=timeout, cwd=REPO_ROOT)
        except subprocess.TimeoutExpired:
            return "timeout", "", ""
    return result.returncode, result.stdout, result.stderr


def main() -> int:
    if not BASELINE.exists():
        print(f"error: {BASELINE} not found", file=sys.stderr)
        return 1
    baseline = json.loads(BASELINE.read_text())

    # Positive run/build fixtures only: emit-ir and link-directives have no
    # backend-differential meaning, and negative fixtures never reach codegen.
    candidates = sorted(
        name for name, spec in baseline.items()
        if spec.get("exit", 1) == 0 and spec.get("action") in ("run", "build")
    )

    matched = 0
    unavailable = 0
    not_lowered: list[str] = []
    for name in candidates:
        spec = baseline[name]
        directory = EXAMPLES / name
        action = spec["action"]
        timeout = spec.get("timeout", 60)

        llvm_code, llvm_out, _ = run_backend(directory, action, "llvm", timeout)

        # Both register allocators (docs/backend.md stage 6): linear is the one
        # under test; trivial is the standing triage baseline. A fixture passes
        # only when BOTH agree with LLVM, so a linear-scan miscompile and a
        # shared-emission miscompile are distinguishable from one run's output.
        fixture_ok = True
        skipped = False
        for regalloc in ("linear", "trivial"):
            native_code, native_out, native_err = run_backend(
                directory, action, "native", timeout, regalloc)
            if NATIVE_UNAVAILABLE in native_err:
                unavailable += 1
                skipped = True
                break
            if native_code != 0 and "cannot lower" in native_err:
                not_lowered.append(name)
                skipped = True
                break
            compare_stdout = name not in EXIT_CODE_ONLY
            if llvm_code == native_code and (not compare_stdout or llvm_out == native_out):
                continue
            fixture_ok = False
            fail(f"{name}: llvm (exit {llvm_code}) vs native/{regalloc} "
                 f"(exit {native_code}) diverge"
                 + (f"\n  llvm stdout:   {llvm_out!r}\n  native stdout: {native_out!r}"
                    if llvm_out != native_out else "")
                 + (f"\n  native stderr: {native_err.strip()[:300]}" if native_err.strip() else ""))
        if skipped or not fixture_ok:
            continue
        matched += 1
        print(f"ok: {name}: backends agree (exit {llvm_code})"
              + ("" if name not in EXIT_CODE_ONLY else " [exit code only]"))

    total = len(candidates)
    print(f"\n{total} positive fixtures: {matched} matched, {failures} mismatched, "
          f"{unavailable} awaiting native object generation (stage 4), "
          f"{len(not_lowered)} refused by name ({', '.join(not_lowered) or 'none'})")
    if failures:
        return 1
    print("all backend differential checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
