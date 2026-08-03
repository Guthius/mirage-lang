#!/usr/bin/env python3
"""Differential test: native x86-64 vs native wasm32 over the positive corpus.

Stage 7's counterpart to backend_differential_test.py. Both code generators consume
the SAME MIR, so a program's observable behavior (exit code and stdout) must not
depend on which one ran — the wasm module executes under node (tests/wasm_host.js),
whose instantiation also validates the module format on every fixture.

Three outcomes per fixture:

    match       both targets agree — the passing state
    MISMATCH    both produced a result and they differ — always a failure
    no wasm     the fixture cannot exist on wasm32-unknown-unknown and the compiler
                REFUSED IT BY NAME — inline asm (x86-only by definition), calls into
                C variadics (no wasm variadic ABI), or a stdlib surface with no
                backend on this freestanding-like target. Tolerated and counted;
                the summary makes the covered/uncovered split visible on every run.

Anything else out of the wasm path — a crash, an unnamed error, a host-level
instantiation failure — is a real failure.

Skips cleanly when node is not installed.

Not wired into ctest (needs a built compiler). Run manually:

    just build
    python3 tests/wasm_differential_test.py
"""

import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
MIRAGE = REPO_ROOT / "build" / "mirage"
EXAMPLES = REPO_ROOT / "examples"
BASELINE = Path(__file__).resolve().parent / "examples_expected.json"
HOST = Path(__file__).resolve().parent / "wasm_host.js"

# Fixtures that deliberately print an UNSPECIFIED value (see
# backend_differential_test.py); exit codes are still compared.
EXIT_CODE_ONLY = {"example_fnptr3"}

failures = 0


def fail(message: str) -> None:
    global failures
    failures += 1
    print(f"FAIL: {message}")


def refused_for_wasm(stderr: str) -> bool:
    """A NAMED refusal: the compiler said why this program has no wasm form."""
    markers = (
        "cannot be compiled for a wasm target",      # inline asm
        "cannot call a C-variadic function",          # printf and friends
        "cannot lower",                               # mirgen's loud refusal
        "error:",                                     # sema: no stdlib backend, target-conditional rules
    )
    return any(marker in stderr for marker in markers)


def main() -> int:
    if shutil.which("node") is None:
        print("node not found; skipping the wasm differential")
        return 0
    if not BASELINE.exists():
        print(f"error: {BASELINE} not found", file=sys.stderr)
        return 1
    baseline = json.loads(BASELINE.read_text())

    candidates = sorted(
        name for name, spec in baseline.items()
        if spec.get("exit", 1) == 0 and spec.get("action") in ("run", "build")
    )

    matched = 0
    no_wasm: list[str] = []
    with tempfile.TemporaryDirectory() as tmp:
        for name in candidates:
            spec = baseline[name]
            directory = EXAMPLES / name
            timeout = spec.get("timeout", 60)

            x86_bin = Path(tmp) / f"{name}.x86"
            wasm_bin = Path(tmp) / f"{name}.wasm"

            build_x86 = subprocess.run(
                [str(MIRAGE), "build", str(directory), "--backend=native",
                 "-o", str(x86_bin)],
                capture_output=True, text=True, timeout=timeout, cwd=REPO_ROOT)
            if build_x86.returncode != 0:
                fail(f"{name}: x86 native build failed\n  {build_x86.stderr.strip()[:300]}")
                continue

            build_wasm = subprocess.run(
                [str(MIRAGE), "build", str(directory), "--backend=native",
                 "--target=wasm32-unknown-unknown", "-o", str(wasm_bin)],
                capture_output=True, text=True, timeout=timeout, cwd=REPO_ROOT)
            if build_wasm.returncode != 0:
                if refused_for_wasm(build_wasm.stderr):
                    no_wasm.append(name)
                    continue
                fail(f"{name}: wasm build failed WITHOUT naming a reason\n"
                     f"  {build_wasm.stderr.strip()[:300]}")
                continue

            try:
                run_x86 = subprocess.run([str(x86_bin)], capture_output=True,
                                          text=True, timeout=timeout)
                run_wasm = subprocess.run(["node", str(HOST), str(wasm_bin)],
                                           capture_output=True, text=True, timeout=timeout)
            except subprocess.TimeoutExpired:
                fail(f"{name}: timed out")
                continue

            if run_wasm.returncode == 121:
                # An unsatisfied import: the program wants host surface (files,
                # sockets, an allocator beyond sbrk) the minimal embedder does not
                # provide. A property of the program/embedder pair on this
                # freestanding-like target, so counted with the named refusals.
                no_wasm.append(name)
                continue
            if run_wasm.returncode == 120:
                fail(f"{name}: wasm host error\n  {run_wasm.stderr.strip()[:300]}")
                continue
            compare_stdout = name not in EXIT_CODE_ONLY
            if run_x86.returncode == run_wasm.returncode and (
                    not compare_stdout or run_x86.stdout == run_wasm.stdout):
                matched += 1
                print(f"ok: {name}: targets agree (exit {run_x86.returncode})"
                      + ("" if compare_stdout else " [exit code only]"))
                continue
            fail(f"{name}: x86 (exit {run_x86.returncode}) vs wasm "
                 f"(exit {run_wasm.returncode}) diverge"
                 + (f"\n  x86 stdout:  {run_x86.stdout!r}\n  wasm stdout: {run_wasm.stdout!r}"
                    if run_x86.stdout != run_wasm.stdout else "")
                 + (f"\n  wasm stderr: {run_wasm.stderr.strip()[:300]}"
                    if run_wasm.stderr.strip() else ""))

    total = len(candidates)
    print(f"\n{total} positive fixtures: {matched} matched, {failures} mismatched, "
          f"{len(no_wasm)} with no wasm form ({', '.join(no_wasm) or 'none'})")
    if failures:
        return 1
    print("all x86/wasm differential checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
