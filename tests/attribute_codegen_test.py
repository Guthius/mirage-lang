#!/usr/bin/env python3
"""LLVM-IR-level codegen test for the declaration-attribute system: confirms each of the four
codegen-affecting attributes ('@no_return', '@naked', '@always_inline', '@section') actually
attaches the LLVM attribute/section it's supposed to, and that the module still passes
llvm::verifyModule (already wired into every 'build' invocation — a build that fails with
"LLVM module verification failed" instead of the compile succeeding would itself indicate a
malformed-IR regression).

Not wired into ctest — same manual-run convention as tests/asm_diagnostics_test.py:

    just build
    python3 tests/attribute_codegen_test.py
"""

import re
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


def emit_ir(example_dir: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [str(MIRAGE_BINARY), "build", str(EXAMPLES / example_dir), "--emit-ir"],
        capture_output=True,
        text=True,
        timeout=30,
    )


# Each case: (example dir, mangled-name substring identifying the function, expected LLVM
# function attribute OR section string to find attached to it).
ATTR_CASES = [
    ("example_attr_no_return", "suspicious", "attr", "noreturn"),
    ("example_attr_naked", "raw_add_one", "attr", "naked"),
    ("example_attr_always_inline_address_taken", "_add", "attr", "alwaysinline"),
    ("example_attr_section", "hot_path", "section", '".text.hot"'),
    ("example_attr_section_method", "hot_path", "section", '".text.hot"'),
    ("example_attr_section_trait_method", "draw", "section", '".text.hot"'),
]


def main() -> int:
    if not MIRAGE_BINARY.exists():
        print(f"FAIL: {MIRAGE_BINARY} does not exist — run 'just build' first")
        return 1

    for example_dir, name_substr, kind, expected in ATTR_CASES:
        result = emit_ir(example_dir)
        check(result.returncode == 0, f"{example_dir}: --emit-ir succeeds")
        check(
            "LLVM module verification failed" not in result.stderr,
            f"{example_dir}: llvm::verifyModule passes (no verification-failure diagnostic)",
        )

        # Find the 'define ...' line for the function, then (for attributes) resolve its
        # '#N' attribute-group reference against the module's 'attributes #N = { ... }' line.
        define_line = next(
            (line for line in result.stdout.splitlines() if line.startswith("define") and name_substr in line),
            None,
        )
        check(define_line is not None, f"{example_dir}: found a 'define' line containing '{name_substr}'")
        if define_line is None:
            continue

        if kind == "section":
            check(f"section {expected}" in define_line, f"{example_dir}: define line has 'section {expected}'")
        else:
            group_match = re.search(r"#(\d+)\s*\{", define_line) or re.search(r"#(\d+)\s*$", define_line.strip())
            check(group_match is not None, f"{example_dir}: define line references an attribute group")
            if group_match is not None:
                group_id = group_match.group(1)
                group_line = next(
                    (line for line in result.stdout.splitlines() if line.startswith(f"attributes #{group_id} ")),
                    None,
                )
                check(group_line is not None, f"{example_dir}: found 'attributes #{group_id}' definition")
                if group_line is not None:
                    check(expected in group_line, f"{example_dir}: attribute group #{group_id} contains '{expected}'")

    print(f"\n{failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
