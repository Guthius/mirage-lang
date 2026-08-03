#!/usr/bin/env python3
"""Back-end-visible effects of the declaration attributes.

This used to assert on LLVM IR text ('define ... noreturn', 'section ".text.hot"').
With LLVM removed there is no IR to grep, so each case is re-expressed against what
the native backend actually does — emitted machine code where the attribute has a
representation, and a NAMED REFUSAL where it has none:

  @naked          no compiler prologue: the function's first bytes are its own
                  body, not 'push rbp; mov rbp, rsp'.
  @always_inline  no native representation — it was an LLVM optimization HINT with
                  no semantic content, and this backend does not inline. Sema still
                  validates it (including the address-taken warning), which
                  attribute_diagnostics_test.py covers.
  @no_return      likewise a hint; sema's diagnostics are the whole contract.
  @section        REFUSED BY NAME: elf_writer emits the fixed System V section set,
                  so a named section cannot be honored. It worked on the LLVM path,
                  so losing it is the one feature cost of the removal — the refusal
                  is what keeps that honest instead of silently ignoring it.

Not wired into ctest — same manual-run convention as tests/asm_diagnostics_test.py:

    just build
    python3 tests/attribute_codegen_test.py
"""

import shutil
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


def build(example_dir: str, *extra: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [str(MIRAGE_BINARY), "build", str(EXAMPLES / example_dir), *extra],
        capture_output=True, text=True, timeout=60, cwd=REPO_ROOT)


def function_disassembly(binary: Path, name_substring: str) -> str:
    """The objdump block for the first function whose label contains the substring."""
    dump = subprocess.run(["objdump", "-d", str(binary)], capture_output=True,
                          text=True, timeout=60).stdout
    for block in dump.split("\n\n"):
        header = block.split("\n", 1)[0]
        if name_substring in header and header.rstrip().endswith(":"):
            return block
    return ""


def main() -> int:
    if not MIRAGE_BINARY.exists():
        print(f"FAIL: {MIRAGE_BINARY} does not exist — run 'just build' first")
        return 1

    # ---- @naked: the body IS the function, with no compiler prologue ---------
    out = REPO_ROOT / "build" / "attr_naked_probe"
    result = build("example_attr_naked", "-o", str(out))
    check(result.returncode == 0, "example_attr_naked: builds natively")
    if result.returncode == 0 and shutil.which("objdump") is not None:
        naked = function_disassembly(out, "raw_add_one")
        check(bool(naked), "example_attr_naked: 'raw_add_one' appears in the disassembly")
        if naked:
            # The two instructions every ordinary function opens with.
            opening = "\n".join(naked.split("\n")[1:3])
            check("push" not in opening and "mov    %rsp,%rbp" not in opening,
                  "example_attr_naked: no 'push rbp / mov rbp, rsp' prologue")
        # A non-naked function in the same binary must still HAVE the prologue,
        # or the check above passes for the wrong reason.
        ordinary = function_disassembly(out, "<main>")
        if ordinary:
            check("push" in "\n".join(ordinary.split("\n")[1:3]),
                  "control: an ordinary function still emits its prologue")
    elif shutil.which("objdump") is None:
        print("note: objdump not found; skipping the @naked prologue check")
    out.unlink(missing_ok=True)

    # ---- @section: refused by name, never silently dropped -------------------
    for fixture in ("example_attr_section", "example_attr_section_method",
                    "example_attr_section_trait_method"):
        result = build(fixture, "-o", str(REPO_ROOT / "build" / "attr_section_probe"))
        check(result.returncode != 0, f"{fixture}: '@section' fails the build")
        check("'@section'" in result.stderr,
              f"{fixture}: the failure NAMES '@section' rather than ignoring it")
    (REPO_ROOT / "build" / "attr_section_probe").unlink(missing_ok=True)

    print(f"\n{failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
