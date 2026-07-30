#!/usr/bin/env python3
"""The in-repo VS Code TextMate grammar tracks the language's actual syntax.

Only the VS Code grammar lives here. Zed's highlighting comes from the separate
tree-sitter-mirage repository, pinned by commit SHA in editors/zed/extension.toml and not
checked out — so a syntax change landed in this repo leaves Zed stale until a PR is opened
there. See editors/zed/README.md. Nothing in this file can check that.

What it does check is that the grammar this repo DOES own has not silently drifted from the
syntax: '#' comments were replaced by '//' and '/* */' on 2026-07-27, and the '?' ignorable-
error return marker landed on 2026-07-30.

Not wired into ctest (it needs no build). Run it manually:

    python3 tests/editor_grammar_test.py
"""

import json
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
GRAMMAR = REPO_ROOT / "editors" / "vscode" / "syntaxes" / "mirage.tmLanguage.json"

failures = 0


def check(condition: bool, message: str) -> None:
    global failures
    if not condition:
        failures += 1
        print(f"FAIL: {message}")
    else:
        print(f"ok: {message}")


def rule_patterns(grammar: dict, name: str) -> list[str]:
    """Every 'match'/'begin' regex belonging to a named repository rule."""
    out = []
    for pattern in grammar["repository"][name]["patterns"]:
        expr = pattern.get("match") or pattern.get("begin")
        if expr:
            out.append(expr)
    return out


def matches_any(patterns: list[str], text: str) -> bool:
    return any(re.search(p, text) for p in patterns)


def main() -> int:
    if not GRAMMAR.exists():
        print(f"error: {GRAMMAR} not found", file=sys.stderr)
        return 1

    grammar = json.loads(GRAMMAR.read_text())
    top_level = json.dumps(grammar["patterns"])

    # A rule that exists but is never referenced from the top level highlights nothing.
    check("#comments" in top_level, "the comments rule is reachable from the top-level patterns")
    check("optional-error" in top_level, "the optional-error rule is reachable from the top-level patterns")

    comments = rule_patterns(grammar, "comments")
    check(matches_any(comments, "// a line comment"), "'//' starts a line comment")
    check(matches_any(comments, "/* a block comment */"), "'/*' starts a block comment")
    check(not matches_any(comments, "# an old-style comment"),
          "'#' is NOT a comment (that syntax was replaced on 2026-07-27)")

    optional = rule_patterns(grammar, "optional-error")
    # Every spelling of the marker that appears in the corpus and the spec.
    for source in (
        "fn alloc(size: usize) -> (anyptr, ?Allocator_Error)",
        "fn touch(size: usize) -> ?error(Alloc_Error)",
        "type Alloc_Fn = fn(usize) -> (anyptr, ?Alloc_Error)",
    ):
        check(matches_any(optional, source), f"'?' return marker is highlighted in: {source}")

    # The ternary uses the same character and must not be caught by it.
    check(not matches_any(optional, "const x := cond ? a : b"),
          "a ternary '?' is not mistaken for the return marker")

    if failures:
        print(f"\n{failures} failure(s)")
        return 1
    print("\nall editor grammar tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
