#!/usr/bin/env python3
"""Differential test: native x86-64 vs native wasm through the EMSCRIPTEN link
(stage 8, docs/backend.md).

Where wasm_differential_test.py runs the standalone module under a minimal node
host, this one exercises the RELOCATABLE object: wasm-ld consumes the linking
section and relocations, emscripten's real libc satisfies printf/malloc/write,
and the module runs inside emscripten's own runtime. Anything wrong with the
object format fails the link loudly; anything wrong with the C-ABI lowering
diverges at run time against clang-compiled code.

Two parts:

  1. The positive corpus, compiled for wasm32-unknown-emscripten with
     '--backend=native' and compared against the native x86-64 build (exit code
     and stdout). Named refusals (inline asm, funcptr↔anyptr) and fixtures that
     need the host filesystem (emscripten's MEMFS is empty) are counted, never
     silently passed.
  2. The ext-fn ABI fixture linked against helper.c compiled BY EMCC — the wasm
     C ABI checked against the other compiler's idea of it, mirroring what the
     x86 run does with clang.

Skips cleanly when the emsdk (or node) is absent. Not wired into ctest. Run:

    just build
    python3 tests/wasm_emscripten_test.py
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
MIRAGE = REPO_ROOT / "build" / "mirage"
EXAMPLES = REPO_ROOT / "examples"
BASELINE = Path(__file__).resolve().parent / "examples_expected.json"
ABI_FIXTURE = Path(__file__).resolve().parent / "ext_abi_fixture"
ABI_EXPECTED = (34 + 123 + 45 + 2006) % 256
EMSDK = Path(os.environ.get("EMSDK", "/mnt/projects/Projects/Mirage/emsdk"))

# Deliberately-unspecified stdout (see backend_differential_test.py).
EXIT_CODE_ONLY = {"example_fnptr3"}
# Fixtures whose RESULT legitimately depends on the target: '#compile_only_if'
# selects different platform files under a wasm target_os, and 'size_of' of
# pointer-bearing shapes answers 4 where x86-64 answers 8. Compiled and run,
# but not compared across targets.
TARGET_DEPENDENT = {
    "example_compile_only_if",
    "example_compile_only_if_trait",
    "example_generics_size_of",
    "example_when_size_of",
    "example_sibling_type_layout",
    "example_generics_default_size_of",
}
# These read the host filesystem at run time; emscripten's MEMFS has no files.
NEEDS_HOST_FS = {"test_io", "compiler"}

failures = 0


def fail(message: str) -> None:
    global failures
    failures += 1
    print(f"FAIL: {message}")


def refused_for_wasm(stderr: str) -> bool:
    markers = (
        "cannot be compiled for a wasm target",
        "cannot call a C-variadic function",
        "cannot lower",
        "error:",
    )
    return any(marker in stderr for marker in markers)


def main() -> int:
    if shutil.which("node") is None:
        print("node not found; skipping the emscripten differential")
        return 0
    emcc = EMSDK / "upstream" / "emscripten" / "emcc"
    if not emcc.exists():
        print(f"emsdk not found at {EMSDK}; skipping the emscripten differential")
        return 0
    env = dict(os.environ)
    env["PATH"] = f"{emcc.parent}:{EMSDK / 'upstream' / 'bin'}:{env['PATH']}"

    baseline = json.loads(BASELINE.read_text())
    # Every runnable POSITIVE fixture — nonzero expected exits included. The
    # original exit==0 filter silently excluded half the runnable corpus, and
    # the stage-10 flip found eight real divergences hiding in that half.
    candidates = sorted(
        name for name, spec in baseline.items()
        if spec.get("action") in ("run", "build") and "diag" not in spec
    )

    matched = 0
    no_wasm: list[str] = []
    skipped_fs: list[str] = []
    with tempfile.TemporaryDirectory() as tmp:
        for name in candidates:
            if name in TARGET_DEPENDENT:
                continue
            if name in NEEDS_HOST_FS:
                skipped_fs.append(name)
                continue
            spec = baseline[name]
            directory = EXAMPLES / name
            timeout = spec.get("timeout", 120)

            x86_bin = Path(tmp) / f"{name}.x86"
            em_js = Path(tmp) / f"{name}.js"

            build_x86 = subprocess.run(
                [str(MIRAGE), "build", str(directory), "--backend=native",
                 "-o", str(x86_bin)],
                capture_output=True, text=True, timeout=timeout, cwd=REPO_ROOT)
            if build_x86.returncode != 0:
                fail(f"{name}: x86 native build failed\n  {build_x86.stderr.strip()[:300]}")
                continue

            build_em = subprocess.run(
                [str(MIRAGE), "build", str(directory), "--backend=native",
                 "--target=wasm32-unknown-emscripten", "-o", str(em_js)],
                capture_output=True, text=True, timeout=timeout, cwd=REPO_ROOT, env=env)
            if build_em.returncode != 0:
                if refused_for_wasm(build_em.stderr):
                    no_wasm.append(name)
                    continue
                fail(f"{name}: emscripten build failed WITHOUT naming a reason\n"
                     f"  {build_em.stderr.strip()[:300]}")
                continue

            try:
                run_x86 = subprocess.run([str(x86_bin)], capture_output=True,
                                          text=True, timeout=timeout)
                run_em = subprocess.run(["node", str(em_js)], capture_output=True,
                                         text=True, timeout=timeout)
            except subprocess.TimeoutExpired:
                fail(f"{name}: timed out")
                continue

            compare_stdout = name not in EXIT_CODE_ONLY
            if run_x86.returncode == run_em.returncode and (
                    not compare_stdout or run_x86.stdout == run_em.stdout):
                matched += 1
                print(f"ok: {name}: targets agree (exit {run_x86.returncode})"
                      + ("" if compare_stdout else " [exit code only]"))
                continue
            fail(f"{name}: x86 (exit {run_x86.returncode}) vs emscripten "
                 f"(exit {run_em.returncode}) diverge"
                 + (f"\n  x86 stdout: {run_x86.stdout!r}\n  em stdout:  {run_em.stdout!r}"
                    if run_x86.stdout != run_em.stdout else "")
                 + (f"\n  em stderr:  {run_em.stderr.strip()[:300]}"
                    if run_em.stderr.strip() else ""))

        # ---- part 2: the C ABI against emcc-compiled C -----------------------
        abi_dir = Path(tmp) / "abi"
        abi_dir.mkdir()
        shutil.copy(ABI_FIXTURE / "main.mir", abi_dir / "main.mir")
        helper = subprocess.run(
            [str(emcc), "-c", "-O1", "-o", str(abi_dir / "helper.o"),
             str(ABI_FIXTURE / "helper.c")],
            capture_output=True, text=True, timeout=120, env=env)
        if helper.returncode != 0:
            fail(f"emcc could not compile the ABI helper\n  {helper.stderr.strip()[:300]}")
        else:
            abi_js = Path(tmp) / "abi.js"
            build_abi = subprocess.run(
                [str(MIRAGE), "build", str(abi_dir), "--backend=native",
                 "--target=wasm32-unknown-emscripten", "-o", str(abi_js)],
                capture_output=True, text=True, timeout=120, cwd=REPO_ROOT, env=env)
            if build_abi.returncode != 0:
                fail(f"ABI fixture emscripten build failed\n  {build_abi.stderr.strip()[:300]}")
            else:
                run_abi = subprocess.run(["node", str(abi_js)], capture_output=True,
                                          text=True, timeout=120)
                if run_abi.returncode == ABI_EXPECTED:
                    print(f"ok: ext-fn aggregates cross the emcc boundary intact "
                          f"(exit {run_abi.returncode})")
                else:
                    fail(f"ABI fixture: exit {run_abi.returncode} != {ABI_EXPECTED}"
                         + (f"\n  stderr: {run_abi.stderr.strip()[:300]}"
                            if run_abi.stderr.strip() else ""))

    total = len(candidates)
    print(f"\n{total} positive fixtures: {matched} matched, {failures} mismatched/failed, "
          f"{len(no_wasm)} with no wasm form, {len(skipped_fs)} need the host filesystem "
          f"({', '.join(skipped_fs) or 'none'})")
    if failures:
        return 1
    print("all emscripten differential checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
