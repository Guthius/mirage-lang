#!/usr/bin/env python3
"""Pins the five-root module search order.

'import("...")' is tried against five roots, first hit wins:

    1. the importing module's directory   (no containment check -- '..' is allowed)
    2. the root module's directory
    3. the current working directory
    4. the compiler executable's directory, then <exe dir>/../lib/mirage
    5. --std=<path>, else $MIRAGE_MODULES_ROOT

Roots 2-5 additionally reject any path that canonicalizes OUTSIDE the root it was found
under; root 1 deliberately does not, because sibling-module imports in the corpus
('import("../../runtime/type_info")', 'import("..")') depend on upward traversal.

Every case below builds its own throwaway tree under a temp directory and controls cwd
and the environment explicitly, because that is the only way to tell the roots apart --
in the repo itself several of them coincide.

Not wired into ctest (same posture as the other .py suites). Run manually:

    just build
    python3 tests/module_resolution_test.py
"""

import os
import shutil
import subprocess
import sys
import tempfile
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


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def run(args, cwd, env_overrides=None):
    env = dict(os.environ)
    # Neither variable may leak in from the developer's shell: MIRAGE_MODULES_ROOT would
    # silently satisfy imports these cases expect to fail, and MIRAGE_PATH would make the
    # deprecation-note assertions pass for the wrong reason.
    env.pop("MIRAGE_MODULES_ROOT", None)
    env.pop("MIRAGE_PATH", None)
    env.update(env_overrides or {})
    return subprocess.run(
        [str(MIRAGE)] + args,
        cwd=str(cwd),
        env=env,
        capture_output=True,
        text=True,
        timeout=60,
    )


# A module that imports 'dep' and does nothing else, plus the 'dep' module itself.
CONSUMER = 'const dep := import("dep")\npub fn main() -> i32 { return dep.value }\n'
DEP = "pub const value: i32 = 7\n"


def case_root1_importing_module():
    """Root 1: 'dep' sits beside the importing module."""
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        write(tmp / "proj" / "main.mir", CONSUMER)
        write(tmp / "proj" / "dep" / "dep.mir", DEP)
        r = run(["build", "proj", "-o", str(tmp / "out")], cwd=tmp)
        check(r.returncode == 0, f"root 1: import beside the importing module resolves ({r.stderr.strip()[:120]})")


def case_root1_allows_upward_traversal():
    """Root 1 skips the containment check, so '..' still works. Corpus depends on this."""
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        write(tmp / "proj" / "main.mir", 'const dep := import("../shared")\npub fn main() -> i32 { return dep.value }\n')
        write(tmp / "shared" / "shared.mir", DEP)
        r = run(["build", "proj", "-o", str(tmp / "out")], cwd=tmp)
        check(r.returncode == 0, f"root 1: upward '..' traversal is still allowed ({r.stderr.strip()[:120]})")


def case_root2_root_module_dir():
    """Root 2: a nested module imports something that only exists beside the ROOT module."""
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        # Root module imports 'nested'; 'nested' imports 'dep', which exists only at the
        # root module's level -- so root 1 misses and root 2 must supply it.
        write(tmp / "proj" / "main.mir", 'const n := import("nested")\npub fn main() -> i32 { return n.value }\n')
        write(tmp / "proj" / "nested" / "nested.mir", 'const dep := import("dep")\npub const value: i32 = dep.value\n')
        write(tmp / "proj" / "dep" / "dep.mir", DEP)
        r = run(["build", "proj", "-o", str(tmp / "out")], cwd=tmp)
        check(r.returncode == 0, f"root 2: import resolves against the root module directory ({r.stderr.strip()[:120]})")

        trace = run(["build", "proj", "--print-module-search"], cwd=tmp)
        check(
            "[root module directory]" in trace.stdout,
            "root 2: --print-module-search attributes it to the root module directory",
        )


def case_root3_cwd():
    """Root 3: 'dep' exists only in the working directory the compiler was invoked from."""
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        write(tmp / "proj" / "main.mir", CONSUMER)
        write(tmp / "elsewhere" / "dep" / "dep.mir", DEP)
        # cwd is 'elsewhere', the module is addressed by an absolute path, so roots 1 and 2
        # both point at 'proj' and miss.
        r = run(["build", str(tmp / "proj"), "-o", str(tmp / "out")], cwd=tmp / "elsewhere")
        check(r.returncode == 0, f"root 3: import resolves against the cwd ({r.stderr.strip()[:120]})")

        trace = run(["build", str(tmp / "proj"), "--print-module-search"], cwd=tmp / "elsewhere")
        check(
            "[current working directory]" in trace.stdout,
            "root 3: --print-module-search attributes it to the cwd",
        )


def case_root4_compiler_dir():
    """Root 4a: 'dep' sits beside the compiler executable."""
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        bindir = tmp / "install" / "bin"
        bindir.mkdir(parents=True)
        shutil.copy2(MIRAGE, bindir / "mirage")
        write(tmp / "proj" / "main.mir", CONSUMER)
        write(bindir / "dep" / "dep.mir", DEP)
        # cwd is a directory with no 'dep', so roots 1-3 all miss.
        (tmp / "empty").mkdir()
        env = dict(os.environ)
        env.pop("MIRAGE_MODULES_ROOT", None)
        env.pop("MIRAGE_PATH", None)
        r = subprocess.run(
            [str(bindir / "mirage"), "build", str(tmp / "proj"), "-o", str(tmp / "out")],
            cwd=str(tmp / "empty"), env=env, capture_output=True, text=True, timeout=60,
        )
        check(r.returncode == 0, f"root 4a: import resolves beside the compiler executable ({r.stderr.strip()[:120]})")


def case_root4_lib_mirage():
    """Root 4b: the bin/ + lib/mirage/ install layout, with no environment variable set."""
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        bindir = tmp / "install" / "bin"
        bindir.mkdir(parents=True)
        shutil.copy2(MIRAGE, bindir / "mirage")
        write(tmp / "install" / "lib" / "mirage" / "dep" / "dep.mir", DEP)
        write(tmp / "proj" / "main.mir", CONSUMER)
        (tmp / "empty").mkdir()
        env = dict(os.environ)
        env.pop("MIRAGE_MODULES_ROOT", None)
        env.pop("MIRAGE_PATH", None)
        r = subprocess.run(
            [str(bindir / "mirage"), "build", str(tmp / "proj"), "-o", str(tmp / "out")],
            cwd=str(tmp / "empty"), env=env, capture_output=True, text=True, timeout=60,
        )
        check(r.returncode == 0, f"root 4b: bin/ + lib/mirage/ install layout resolves with no env var ({r.stderr.strip()[:120]})")


def case_root5_env_and_std():
    """Root 5: $MIRAGE_MODULES_ROOT, and --std= taking precedence over it."""
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        write(tmp / "proj" / "main.mir", CONSUMER)
        write(tmp / "modroot" / "dep" / "dep.mir", "pub const value: i32 = 7\n")
        write(tmp / "otherroot" / "dep" / "dep.mir", "pub const value: i32 = 9\n")
        (tmp / "empty").mkdir()

        r = run(["build", str(tmp / "proj"), "-o", str(tmp / "out")], cwd=tmp / "empty",
                env_overrides={"MIRAGE_MODULES_ROOT": str(tmp / "modroot")})
        check(r.returncode == 0, f"root 5: $MIRAGE_MODULES_ROOT resolves ({r.stderr.strip()[:120]})")

        # --std= must win over the environment: point the env at 'modroot' and the flag at
        # 'otherroot', then check the trace resolved through the flag's directory.
        trace = run(["build", str(tmp / "proj"), "--print-module-search", f"--std={tmp / 'otherroot'}"],
                    cwd=tmp / "empty", env_overrides={"MIRAGE_MODULES_ROOT": str(tmp / "modroot")})
        check("otherroot" in trace.stdout and "modroot" not in trace.stdout,
              "root 5: '--std=' takes precedence over $MIRAGE_MODULES_ROOT")


def case_root5_containment():
    """Roots 2-5 reject a path escaping their own root; root 1 does not."""
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        # 'escape' resolves to tmp/outside, which is NOT under tmp/modroot.
        write(tmp / "proj" / "main.mir", 'const dep := import("../outside")\npub fn main() -> i32 { return dep.value }\n')
        write(tmp / "outside" / "outside.mir", DEP)
        (tmp / "modroot").mkdir()
        (tmp / "empty").mkdir()
        # cwd is 'empty' and the module is absolute, so root 1 is tmp/proj -- from there
        # '../outside' DOES exist, and root 1 is unguarded, so this must succeed.
        r = run(["build", str(tmp / "proj"), "-o", str(tmp / "out")], cwd=tmp / "empty",
                env_overrides={"MIRAGE_MODULES_ROOT": str(tmp / "modroot")})
        check(r.returncode == 0, "containment: root 1 still permits '..' out of the module")

        # Now the same escape shape, but only reachable via root 5: put the importer
        # somewhere with no sibling to find, and make the escape only satisfiable from
        # modroot. It must be rejected rather than resolving to tmp/outside.
        write(tmp / "proj2" / "main.mir", 'const dep := import("sub/../../outside")\npub fn main() -> i32 { return dep.value }\n')
        (tmp / "modroot" / "sub").mkdir(parents=True, exist_ok=True)
        r2 = run(["build", str(tmp / "proj2"), "-o", str(tmp / "out2")], cwd=tmp / "empty",
                 env_overrides={"MIRAGE_MODULES_ROOT": str(tmp / "modroot")})
        check(r2.returncode != 0 and "cannot resolve import path" in r2.stderr,
              "containment: root 5 rejects a path that escapes the module root")


def case_absolute_import_rejected():
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        write(tmp / "proj" / "main.mir", 'const dep := import("/etc")\npub fn main() -> i32 { return 0 }\n')
        r = run(["build", "proj", "-o", str(tmp / "out")], cwd=tmp)
        check(r.returncode != 0 and "cannot resolve import path" in r.stderr,
              "an absolute import path is rejected")


def case_failure_diagnostic_lists_roots():
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        write(tmp / "proj" / "main.mir", 'const dep := import("nowhere")\npub fn main() -> i32 { return 0 }\n')
        r = run(["build", "proj", "-o", str(tmp / "out")], cwd=tmp)
        for label in ("importing module", "root module directory", "current working directory",
                      "compiler directory", "MIRAGE_MODULES_ROOT"):
            check(label in r.stderr, f"failure diagnostic names the '{label}' root")


def case_legacy_mirage_path_note():
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        write(tmp / "proj" / "main.mir", 'const dep := import("nowhere")\npub fn main() -> i32 { return 0 }\n')

        # Set: the note fires, but resolution is NOT attempted there -- 'dep' does exist
        # under the MIRAGE_PATH directory and must still fail to resolve.
        write(tmp / "legacy" / "nowhere" / "n.mir", "pub const value: i32 = 1\n")
        r = run(["build", "proj", "-o", str(tmp / "out")], cwd=tmp,
                env_overrides={"MIRAGE_PATH": str(tmp / "legacy")})
        check(r.returncode != 0, "MIRAGE_PATH is not consulted as a search root")
        check("MIRAGE_PATH is set but is no longer consulted" in r.stderr,
              "MIRAGE_PATH set: the deprecation note appears on the failure")

        # Unset: no note.
        r2 = run(["build", "proj", "-o", str(tmp / "out")], cwd=tmp)
        check("no longer consulted" not in r2.stderr,
              "MIRAGE_PATH unset: no deprecation note")

        # Set alongside MIRAGE_MODULES_ROOT: no note, the new variable is authoritative.
        (tmp / "modroot").mkdir(exist_ok=True)
        r3 = run(["build", "proj", "-o", str(tmp / "out")], cwd=tmp,
                 env_overrides={"MIRAGE_PATH": str(tmp / "legacy"),
                                "MIRAGE_MODULES_ROOT": str(tmp / "modroot")})
        check("no longer consulted" not in r3.stderr,
              "MIRAGE_PATH set with MIRAGE_MODULES_ROOT: no deprecation note")


def case_bad_modules_root_warns():
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        write(tmp / "proj" / "main.mir", "pub fn main() -> i32 { return 0 }\n")
        r = run(["build", "proj", "-o", str(tmp / "out")], cwd=tmp,
                env_overrides={"MIRAGE_MODULES_ROOT": str(tmp / "does-not-exist")})
        check(r.returncode == 0, "a bad MIRAGE_MODULES_ROOT does not fail a build that does not need it")
        check("MIRAGE_MODULES_ROOT is set to" in r.stderr, "a bad MIRAGE_MODULES_ROOT warns")

        # '--std=' naming a bad directory is an error, not a warning.
        r2 = run(["build", "proj", f"--std={tmp / 'does-not-exist'}", "-o", str(tmp / "out")], cwd=tmp)
        check(r2.returncode != 0 and "is not a valid standard library directory" in r2.stderr,
              "a bad '--std=' is an error")


def main() -> int:
    if not MIRAGE.exists():
        print(f"FAIL: {MIRAGE} not found; run 'just build' first")
        return 1

    case_root1_importing_module()
    case_root1_allows_upward_traversal()
    case_root2_root_module_dir()
    case_root3_cwd()
    case_root4_compiler_dir()
    case_root4_lib_mirage()
    case_root5_env_and_std()
    case_root5_containment()
    case_absolute_import_rejected()
    case_failure_diagnostic_lists_roots()
    case_legacy_mirage_path_note()
    case_bad_modules_root_warns()

    print()
    if failures:
        print(f"{failures} failure(s)")
        return 1
    print("all module resolution tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
