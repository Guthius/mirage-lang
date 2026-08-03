#!/usr/bin/env python3
"""'@test', the 'mirage test' driver action, '--load' forced modules, and 'core/testing'.

Most of this suite is about a DIFFERENCE between two driver actions -- what 'mirage build'
accepts that 'mirage test' rejects, and vice versa -- which the corpus gate cannot express,
since it pins each fixture under exactly one action.

Needs the standard library (for 'core/testing'), so every invocation passes '--std'. Point
MIRAGE_STD at a checkout if it is not beside this repo.

Not wired into ctest (same posture as the other .py suites). Run manually:

    just build
    python3 tests/test_action_test.py
"""

import os
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
MIRAGE = REPO_ROOT / "build" / "mirage"
STD = Path(os.environ.get("MIRAGE_STD", REPO_ROOT.parent / "Mirage"))

failures = 0

# Every fixture needs an error type to return; kept in one place so the cases below stay
# about the thing they are testing.
PRELUDE = "pub type E = enum(i32) { Bad = 1 }\n"


def check(condition: bool, message: str) -> None:
    global failures
    if not condition:
        failures += 1
        print(f"FAIL: {message}")
    else:
        print(f"ok: {message}")


def run(action: str, files: dict[str, str], *extra, timeout=120):
    """Writes 'files' (relative path -> contents) into a temp module and runs 'action'."""
    with tempfile.TemporaryDirectory() as tmp:
        for rel, text in files.items():
            path = Path(tmp) / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
        args = [str(MIRAGE), action, tmp, f"--std={STD}"]
        if action == "build":
            args += ["-o", "/dev/null"]
        return subprocess.run(args + list(extra), capture_output=True, text=True,
                              timeout=timeout, cwd=REPO_ROOT)


def one(source: str) -> dict[str, str]:
    return {"main.mir": PRELUDE + source}


# ------------------------------------------------------------ signature restrictions
# These are checked in EVERY action: a malformed '@test' declaration is always an error, so
# switching actions never changes whether the declaration itself is valid.

def case_signature_restrictions():
    cases = [
        ("@test\nfn t(a: i32) -> error(E) { return_ok }\npub fn main() -> i32 { return 0 }\n",
         "may not take parameters", "parameters"),
        ("@test\nfn t() { }\npub fn main() -> i32 { return 0 }\n",
         "must return exactly 'error(...)'", "a void return"),
        ("@test\nfn t() -> i32 { return 0 }\npub fn main() -> i32 { return 0 }\n",
         "must return exactly 'error(...)'", "a plain value return"),
        ("@test\nfn t[T: type]() -> error(E) { return_ok }\npub fn main() -> i32 { return 0 }\n",
         "not allowed on a generic function", "generic parameters"),
        ("pub type S = struct { x: i32 }\nimpl S {\n  @test\n  fn t(self) -> error(E) { return_ok }\n}\n"
         "pub fn main() -> i32 { return 0 }\n",
         "not allowed on impl methods", "an impl method"),
    ]
    for source, fragment, what in cases:
        for action in ("build", "test"):
            r = run(action, one(source))
            check(r.returncode != 0 and fragment in r.stderr,
                  f"'@test' with {what} is rejected under '{action}'")

    # A union of error types is explicitly allowed — a deliberate widening relative to
    # '@init', which restricts to 'enum(i32)'.
    r = run("test", one("pub type F = enum(i32) { Other = 1 }\n"
                        "@test\nfn t() -> error(E | F) { return_ok }\n"))
    check(r.returncode == 0, f"'@test' returning 'error(A | B)' is accepted ({r.stderr.strip()[:120]})")

    # Exactly one spurious-diagnostic guard: a generic '@test' must not ALSO be told its
    # return type is wrong (a template's return types are never resolved).
    r = run("build", one("@test\nfn t[T: type]() -> error(E) { return_ok }\npub fn main() -> i32 { return 0 }\n"))
    check(r.stderr.count("must return exactly") == 0,
          "a generic '@test' does not also report a bogus return-type error")


def case_conflicting_attributes():
    for attr, fragment in [
        ("naked", "'@test' and '@naked' cannot be combined"),
        ("no_return", "'@test' and '@no_return' cannot be combined"),
        ("init", "'@test' and '@init' cannot be combined"),
        ("export", "'@test' and '@export' cannot be combined"),
        ("cdecl", "'@test' and '@callconv' cannot be combined"),
    ]:
        body = "asm { ret }" if attr == "naked" else "return_ok"
        r = run("build", one(f"@(test, {attr})\nfn t() -> error(E) {{ {body} }}\npub fn main() -> i32 {{ return 0 }}\n"))
        check(r.returncode != 0 and fragment in r.stderr, f"'@test' + '@{attr}' is rejected")

    # Deliberately legal: nothing prevents a test body from being inlined into its
    # synthesized wrapper's call site.
    r = run("test", one("@(test, always_inline)\nfn t() -> error(E) { return_ok }\n"))
    check(r.returncode == 0, f"'@test' + '@always_inline' is accepted ({r.stderr.strip()[:120]})")


# ------------------------------------------------------------ mode-dependent behaviour

def case_body_checking_is_mode_dependent():
    """A '@test' body is type-checked only under 'mirage test'.

    Deliberately mirrors "an unreached generic instantiation is never type-checked", NOT
    'when''s "both branches always checked". Documented as an asymmetry in the spec.
    """
    broken = one("@test\nfn t() -> error(E) {\n  const x := nonexistent_thing.field\n  return_ok\n}\n"
                 "pub fn main() -> i32 { return 0 }\n")
    b = run("build", broken)
    check(b.returncode == 0, f"a broken '@test' body compiles clean under 'build' ({b.stderr.strip()[:120]})")
    tst = run("test", broken)
    check(tst.returncode != 0 and "unknown identifier 'nonexistent_thing'" in tst.stderr,
          "the same broken body fails under 'test'")


def case_call_site_diagnostic():
    called = "@test\nfn t1() -> error(E) { return_ok }\n"
    b = run("build", one(called + "pub fn main() -> i32 { const e := t1()  return 0 }\n"))
    check(b.returncode != 0 and "cannot call '@test' function 't1' outside of 'mirage test'" in b.stderr,
          "calling a '@test' function under 'build' is a hard error")

    t = run("test", one(called + "@test\nfn t2() -> error(E) { const e := t1()  return_ok }\n"))
    check(t.returncode == 0 and "tests should not call other tests" in t.stderr,
          f"calling a '@test' function under 'test' is a warning, and the run still succeeds ({t.returncode})")

    # Taking the address triggers the same diagnostic as a direct call, in both modes.
    addr = one(called + "pub fn main() -> i32 { const p: fn() -> error(E) = t1  return 0 }\n")
    b2 = run("build", addr)
    check(b2.returncode != 0 and "'@test' function 't1'" in b2.stderr,
          "taking a '@test' function's address is diagnosed like a call")


# ------------------------------------------------------------ forced modules

FORCED_HELPER = {
    "main.mir": PRELUDE + "@test\nfn t_root() -> error(E) { return_ok }\n",
    "helper/helper.mir": "pub type HE = enum(i32) { Nope = 1 }\n"
                          "pub const marker: i32 = 42\n"
                          "@test\nfn t_forced() -> error(HE) { return_ok }\n",
}


def case_forced_modules():
    r = run("test", FORCED_HELPER, "--load", "helper")
    check(r.returncode == 0, f"a '--load'ed module compiles and its tests run ({r.stderr.strip()[:120]})")
    check("t_forced" in r.stdout, "a '@test' in a forced module is discovered")
    # Order: normal graph first, forced modules appended.
    check(r.stdout.index("t_root") < r.stdout.index("t_forced"),
          "forced-module tests are listed after the normal graph's")

    # Without '--load' the module is simply not part of the program.
    r2 = run("test", FORCED_HELPER)
    check("t_forced" not in r2.stdout, "without '--load' the module is not compiled at all")

    # THE guarantee: forcing creates no binding, so nothing can name the module.
    named = {
        "main.mir": PRELUDE + "@test\nfn t() -> error(E) {\n  const x := helper.marker\n  return_ok\n}\n",
        "helper/helper.mir": "pub const marker: i32 = 42\n",
    }
    r3 = run("test", named, "--load", "helper")
    check(r3.returncode != 0 and "unknown identifier 'helper'" in r3.stderr,
          "a forced module's symbols are unreachable by name (no accessible alias)")

    # Already-loaded paths are a no-op, not an error: normally imported, and the root itself.
    imported = {
        "main.mir": PRELUDE + 'const h := import("helper")\n'
                              "@test\nfn t() -> error(E) { const x := h.marker  return_ok }\n",
        "helper/helper.mir": "pub const marker: i32 = 42\n",
    }
    r4 = run("test", imported, "--load", "helper")
    check(r4.returncode == 0, f"'--load' of an already-imported module is a no-op ({r4.stderr.strip()[:120]})")

    r5 = run("test", one("@test\nfn t() -> error(E) { return_ok }\n"), "--load", ".")
    check(r5.returncode == 0, f"'--load' of the root module itself is a no-op ({r5.stderr.strip()[:120]})")

    r6 = run("test", one("@test\nfn t() -> error(E) { return_ok }\n"), "--load", "no/such/module")
    check(r6.returncode != 0 and "cannot resolve forced module path" in r6.stderr,
          "an unresolvable '--load' path is a clear error")


def case_forced_module_participates_fully():
    """A forced module is compiled on identical terms to a normally-reached one."""
    files = {
        "main.mir": PRELUDE + "@test\nfn t() -> error(E) { return_ok }\n",
        "broken/broken.mir": "pub fn f() -> i32 { return no_such_thing }\n",
    }
    r = run("test", files, "--load", "broken")
    check(r.returncode != 0 and "no_such_thing" in r.stderr,
          "a forced module's declarations are type-checked like any other")

    # '@init' in a forced module participates in the ordering graph and actually runs.
    init_files = {
        "main.mir": PRELUDE + "@test\nfn t() -> error(E) { return_ok }\n",
        "reg/reg.mir": "ext fn write(fd: i32, buf: *u8, count: usize) -> i64\n"
                        "@init\nfn announce() {\n"
                        '  const s := "forced-init-ran\\n"\n'
                        "  write(1, &s[0], len(s))\n"
                        "}\n",
    }
    r2 = run("test", init_files, "--load", "reg")
    check(r2.returncode == 0 and "forced-init-ran" in r2.stdout,
          "a forced module's '@init' runs (it participates in the ordering graph)")


# ------------------------------------------------------------ driver

def case_driver():
    r = run("test", one("@test\nfn t() -> error(E) { return_ok }\n"), "--freestanding")
    check(r.returncode != 0 and "not supported with '--freestanding'" in r.stderr,
          "'mirage test --freestanding' is refused up front")

    r2 = run("test", one("@test\nfn t() -> error(E) { return_ok }\n"),
             "--target=wasm32-unknown-emscripten")
    check(r2.returncode != 0 and "not supported for target" in r2.stderr,
          "'mirage test' is refused for a wasm target")

    # A 'main' in the root module is legal under 'test' and simply never called.
    with_main = one("ext fn write(fd: i32, buf: *u8, count: usize) -> i64\n"
                    "@test\nfn t() -> error(E) { return_ok }\n"
                    "pub fn main() -> i32 {\n"
                    '  const s := "MAIN-RAN\\n"\n'
                    "  write(1, &s[0], len(s))\n"
                    "  return 0\n"
                    "}\n")
    r3 = run("test", with_main)
    check(r3.returncode == 0, f"a root-module 'main' is legal under 'test' ({r3.stderr.strip()[:120]})")
    check("MAIN-RAN" not in r3.stdout, "that 'main' is never called")

    # A 'main' that IS present is validated under 'test' too -- compiling clean here only
    # to fail under 'build' would be a trap.
    r_bad = run("test", one("@test\nfn t() -> error(E) { return_ok }\n"
                            "fn main() -> i32 { return 0 }\n"))
    check(r_bad.returncode != 0 and "must be declared 'pub fn main'" in r_bad.stderr,
          "a malformed 'main' is reported under 'test', not silently accepted")

    # And a module with NO main is fine under 'test', though not under 'build'.
    r4 = run("test", one("@test\nfn t() -> error(E) { return_ok }\n"))
    check(r4.returncode == 0, "a module with no 'main' is legal under 'test'")
    r5 = run("build", one("@test\nfn t() -> error(E) { return_ok }\n"))
    check(r5.returncode != 0 and "requires 'pub fn main" in r5.stderr,
          "the same module still needs a 'main' under 'build'")

    # '--noinit' is honored as-is.
    noinit = one("ext fn write(fd: i32, buf: *u8, count: usize) -> i64\n"
                 "@init\nfn setup() {\n"
                 '  const s := "INIT-RAN\\n"\n'
                 "  write(1, &s[0], len(s))\n"
                 "}\n"
                 "@test\nfn t() -> error(E) { return_ok }\n")
    check("INIT-RAN" in run("test", noinit).stdout, "'@init' runs under 'test'")
    check("INIT-RAN" not in run("test", noinit, "--noinit").stdout,
          "'--noinit' suppresses it under 'test' too")

    r6 = subprocess.run([str(MIRAGE), "frobnicate", "x"], capture_output=True, text=True, timeout=30)
    check("expected 'build', 'run' or 'test'" in r6.stderr, "the unknown-action message lists 'test'")

    # '-o' has nothing to name under 'test' -- the binary is a temporary that is run and
    # deleted. Rejected rather than ignored, which looked like it had worked.
    r7 = run("test", one("@test\nfn t() -> error(E) { return_ok }\n"), "-o", "/tmp/mirage-test-out")
    check(r7.returncode != 0 and "'-o' is not supported with 'mirage test'" in r7.stderr,
          "'-o' is rejected under 'test' rather than silently ignored")

    # '--emit-mir' is how you inspect the generated wrappers; it prints and does not run.
    r8 = run("test", one("@test\nfn t() -> error(E) { return_ok }\n"), "--emit-mir")
    check(r8.returncode == 0 and "__mirage_test_wrapper_0" in r8.stdout,
          "'--emit-mir' under 'test' shows the synthesized per-test wrapper")
    check("running 1 test" not in r8.stdout, "and does not run the tests")


def case_missing_contract():
    """A missing or reshaped 'core/testing' is a driver error, not a codegen failure."""
    with tempfile.TemporaryDirectory() as fake_std:
        # A std root with no 'core/testing' at all.
        r = subprocess.run(
            [str(MIRAGE), "test", "/dev/null", f"--std={fake_std}"],
            capture_output=True, text=True, timeout=60, cwd=REPO_ROOT,
        )
        check(r.returncode != 0, "'mirage test' fails when 'core/testing' cannot be resolved")

        # Present but not exposing the contract.
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / "main.mir").write_text(PRELUDE + "@test\nfn t() -> error(E) { return_ok }\n")
            broken_std = Path(fake_std) / "core" / "testing"
            broken_std.mkdir(parents=True)
            (broken_std / "testing.mir").write_text("pub const unrelated: i32 = 1\n")
            r2 = subprocess.run(
                [str(MIRAGE), "test", tmp, f"--std={fake_std}"],
                capture_output=True, text=True, timeout=60, cwd=REPO_ROOT,
            )
            check(r2.returncode != 0 and "does not expose the expected" in r2.stderr,
                  "a 'core/testing' missing the contract gives the driver-level error")


# ------------------------------------------------------------ end to end

def case_end_to_end():
    files = one("@test\nfn t_passes() -> error(E) { return_ok }\n"
                "@test\nfn t_fails() -> error(E) { return .Bad }\n"
                "@test\nfn t_crashes() -> error(E) {\n  mut p: *i32 = nil\n  p.* = 1\n  return_ok\n}\n")
    r = run("test", files)
    check(r.returncode != 0, "a run with failures exits non-zero")
    check("3 tests" in r.stdout, "the header counts every discovered test")
    check("1 passed, 1 failed, 1 crashed" in r.stdout,
          f"outcomes are classified into three states, not two ({r.stdout.strip().splitlines()[-1:]})")
    # Crash isolation: the segfaulting test must not take the run down with it.
    check("t_passes" in r.stdout and "t_crashes" in r.stdout,
          "a crashing test does not abort the run (fork-per-case isolation)")

    all_pass = run("test", one("@test\nfn t() -> error(E) { return_ok }\n"))
    check(all_pass.returncode == 0, "a run with no failures exits 0")

    # Two tests with DIFFERENT error unions compile together — the point of the per-test
    # wrapper, since function-pointer types are structurally exact.
    two_unions = one("pub type F = enum(i32) { Other = 1 }\n"
                     "@test\nfn a() -> error(E) { return_ok }\n"
                     "@test\nfn b() -> error(F) { return_ok }\n")
    r2 = run("test", two_unions)
    check(r2.returncode == 0, f"tests with differing 'error(...)' unions coexist ({r2.stderr.strip()[:120]})")


def main() -> int:
    if not MIRAGE.exists():
        print(f"FAIL: {MIRAGE} not found; run 'just build' first")
        return 1
    if not (STD / "core" / "testing").is_dir():
        print(f"FAIL: {STD}/core/testing not found; set MIRAGE_STD to a stdlib checkout")
        return 1

    case_signature_restrictions()
    case_conflicting_attributes()
    case_body_checking_is_mode_dependent()
    case_call_site_diagnostic()
    case_forced_modules()
    case_forced_module_participates_fully()
    case_driver()
    case_missing_contract()
    case_end_to_end()

    print()
    if failures:
        print(f"{failures} failure(s)")
        return 1
    print("all 'mirage test' / '@test' tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
