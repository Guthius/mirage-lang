#!/usr/bin/env python3
"""The in-repo VS Code TextMate grammar tracks the language's actual syntax.

The VS Code grammar is the only editor integration this repo owns. (A tree-sitter grammar
exists in a separate tree-sitter-mirage repository for other editors; nothing here checks
it.)

Two kinds of check:

  - Fixed regressions, pinned so they cannot come back: '#' comments were replaced by '//'
    and '/* */' on 2026-07-27, and the '?' ignorable-error return marker landed on
    2026-07-30.

  - DRIFT checks, which derive the expected set from the compiler's own parser rather than
    restating it. The attribute set and the '$' directive set both live as literal lists in
    src/compiler/ast.cpp; a grammar listing a different set is stale by construction. This
    is the check that was missing when six attributes were added and the grammar kept
    highlighting five.

Not wired into ctest (it needs no build). Run it manually:

    python3 tests/editor_grammar_test.py
"""

import json
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
GRAMMAR = REPO_ROOT / "editors" / "vscode" / "syntaxes" / "mirage.tmLanguage.json"
AST_PARSER = REPO_ROOT / "src" / "compiler" / "ast.cpp"

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


def compiler_attribute_names() -> set[str]:
    """The attribute names the parser accepts, read out of check_known_attribute_name.

    Derived rather than restated: a literal list here would drift exactly as the grammar
    did. The function is a chain of 'name == "x"' comparisons, so the set is every string
    literal compared against 'name' inside it.
    """
    source = AST_PARSER.read_text()
    start = source.index("void check_known_attribute_name")
    end = source.index("\n        }", start)
    body = source[start:end]
    return set(re.findall(r'name == "(\w+)"', body))


def compiler_dollar_directives() -> set[str]:
    """The names accepted after the '$' sigil.

    Scoped to parse_primary's 'Dollar' dispatch block: the same
    'parser.peek().lexeme == "x"' shape is used for the '#' directives, so an unscoped
    search would also pick up link/warn/compile_only_if.
    """
    source = AST_PARSER.read_text()
    start = source.index("if (parser.check(TokenKind::Dollar))")
    block = source[start:source.index("}", source.index("return parse_option_expr"))]
    names = set(re.findall(r'parser\.peek\(\)\.lexeme == "(\w+)"', block))
    # 'option' is the fall-through case, named only in parse_option_expr's own diagnostic.
    if "return parse_option_expr" in block:
        names.add("option")
    return names


def alternatives(pattern: str) -> set[str]:
    """The alternation members of a '(a|b|c)' group in a TextMate regex."""
    match = re.search(r"\(([\w|]+)\)", pattern)
    return set(match.group(1).split("|")) if match else set()


def case_attribute_set_matches_the_parser():
    expected = compiler_attribute_names()
    check(len(expected) >= 5, f"the parser's attribute set was located ({len(expected)} names)")

    grammar = json.loads(GRAMMAR.read_text())
    patterns = rule_patterns(grammar, "attributes")
    listed = set()
    for pattern in patterns:
        listed |= alternatives(pattern)

    missing = expected - listed
    extra = listed - expected
    check(not missing, f"the VS Code grammar highlights every attribute the parser accepts (missing: {sorted(missing)})")
    check(not extra, f"and highlights nothing the parser rejects (extra: {sorted(extra)})")


def case_dollar_directives_match_the_parser():
    expected = compiler_dollar_directives()
    check("rtti_enabled" in expected and "option" in expected and "env" in expected,
          f"the parser's '$' directive set was located ({sorted(expected)})")

    grammar = json.loads(GRAMMAR.read_text())
    listed = set()
    for pattern in rule_patterns(grammar, "directives"):
        if pattern.startswith("\\$"):
            listed |= alternatives(pattern)
    missing = expected - listed
    check(not missing, f"the VS Code grammar highlights every '$' directive (missing: {sorted(missing)})")


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

    case_attribute_set_matches_the_parser()
    case_dollar_directives_match_the_parser()

    if failures:
        print(f"\n{failures} failure(s)")
        return 1
    print("\nall editor grammar tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
