#!/usr/bin/env python3
"""LSP server robustness against malformed input.

tests/lsp_smoke_test.py drives the server the way a well-behaved client would. This one
drives it the way a buggy or hostile one might: requests missing required fields, a
didChange with no content changes, and headers claiming implausible sizes.

The bar is that the server survives. A language server that exits takes every open file's
diagnostics, hover and completion with it, not just the one bad request.

Not wired into ctest (see asm_diagnostics_test.py for the same posture). Run it manually
after building:

    just build
    python3 tests/lsp_robustness_test.py
"""

import json
import os
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
LSP_BINARY = REPO_ROOT / "build" / "mirage-lsp"
FIXTURE = REPO_ROOT / "tests" / "lsp_fixtures" / "error_fixture" / "main.mir"

failures = 0


def check(condition: bool, message: str) -> None:
    global failures
    if not condition:
        failures += 1
        print(f"FAIL: {message}")
    else:
        print(f"ok: {message}")


def frame(obj: dict) -> bytes:
    body = json.dumps(obj).encode()
    return b"Content-Length: %d\r\n\r\n" % len(body) + body


def drive(messages: bytes, timeout: int = 90) -> subprocess.CompletedProcess:
    return subprocess.run([str(LSP_BINARY)], input=messages, capture_output=True, timeout=timeout)


def responses(stdout: bytes) -> dict:
    """Maps JSON-RPC id -> 'result' or 'error' for every response in the stream."""
    found = {}
    for part in stdout.split(b"Content-Length:"):
        if b"\r\n\r\n" not in part:
            continue
        try:
            obj = json.loads(part.split(b"\r\n\r\n", 1)[1].decode(errors="replace"))
        except Exception:
            continue
        if isinstance(obj, dict) and "id" in obj:
            found[obj["id"]] = "error" if "error" in obj else "result"
    return found


def session(*middle: bytes) -> bytes:
    uri = "file://" + str(FIXTURE)
    out = frame({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                 "params": {"processId": None, "rootUri": "file://" + str(REPO_ROOT), "capabilities": {}}})
    out += frame({"jsonrpc": "2.0", "method": "initialized", "params": {}})
    out += frame({"jsonrpc": "2.0", "method": "textDocument/didOpen",
                  "params": {"textDocument": {"uri": uri, "languageId": "mirage", "version": 1,
                                              "text": "pub fn main() -> i32 { return 0 }\n"}}})
    for m in middle:
        out += m
    # A well-formed request last. Note it may or may not be ANSWERED before 'exit' arrives:
    # hover is handled on the analysis worker thread, so the response races the shutdown.
    # Only assertions about main-thread behavior (error responses, exit code, stderr) are
    # deterministic here, which is what the checks below rely on.
    out += frame({"jsonrpc": "2.0", "id": 99, "method": "textDocument/hover",
                  "params": {"textDocument": {"uri": uri}, "position": {"line": 0, "character": 8}}})
    out += frame({"jsonrpc": "2.0", "id": 100, "method": "shutdown"})
    out += frame({"jsonrpc": "2.0", "method": "exit"})
    return out


def test_malformed_requests() -> None:
    uri = "file://" + str(FIXTURE)
    result = drive(session(
        # Hover with no "position". nlohmann auto-vivifies the missing key to null and
        # .get<size_t>() on null throws; nothing caught it, so this killed the server.
        frame({"jsonrpc": "2.0", "id": 2, "method": "textDocument/hover",
               "params": {"textDocument": {"uri": uri}}}),
        # Definition with no "textDocument" either.
        frame({"jsonrpc": "2.0", "id": 3, "method": "textDocument/definition", "params": {}}),
        # A malformed *notification*: no one to answer, so it must be logged and dropped.
        frame({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {}}),
    ))

    check(result.returncode == 0, f"server exits cleanly after malformed requests (got {result.returncode})")
    seen = responses(result.stdout)
    check(seen.get(2) == "error", "hover without a position gets a JSON-RPC error, not a crash")
    check(seen.get(3) == "error", "definition without a textDocument gets a JSON-RPC error")
    # shutdown is answered on the main thread, after every malformed message above, so
    # receiving its response proves dispatch survived all of them.
    check(seen.get(100) == "result", "shutdown is still answered after the malformed requests")


def test_empty_content_changes() -> None:
    uri = "file://" + str(FIXTURE)
    # nlohmann's back() on an empty array decrements end() with no bounds check and does not
    # throw: this was a SIGSEGV that no try/catch could have caught.
    result = drive(session(
        frame({"jsonrpc": "2.0", "method": "textDocument/didChange",
               "params": {"textDocument": {"uri": uri, "version": 2}, "contentChanges": []}}),
    ))
    check(result.returncode == 0, f"didChange with empty contentChanges survives (got {result.returncode})")
    check(b"no content changes" in result.stderr, "the empty didChange is reported and ignored")
    check(responses(result.stdout).get(100) == "result", "server still answers after an empty didChange")


def test_oversized_headers() -> None:
    # Content-Length far beyond any real document reached 'std::string body(n, ...)' and died
    # with an uncaught std::bad_alloc.
    # A framing error desynchronizes the stream -- there is no reliable way to find the next
    # message boundary -- so the server exits rather than continuing. Exit 1 is the correct
    # outcome; what matters is that it is a clean exit and not a signal death (a negative
    # returncode) or an abort from an uncaught std::bad_alloc (134).
    result = drive(b"Content-Length: 100000000000\r\n\r\n", timeout=60)
    check(result.returncode == 1, f"absurd Content-Length exits cleanly, not by abort (got {result.returncode})")
    check(b"exceeds" in result.stderr, "oversized Content-Length is reported")

    # A header line that never terminates grew unboundedly under std::getline.
    result = drive(b"X" * (2 * 1024 * 1024), timeout=60)
    check(result.returncode == 1, f"unterminated header line exits cleanly (got {result.returncode})")
    check(b"header line exceeds" in result.stderr, "oversized header line is reported")


def main() -> int:
    if not LSP_BINARY.exists():
        print(f"error: {LSP_BINARY} not found — run 'just build' first", file=sys.stderr)
        return 1

    test_malformed_requests()
    test_empty_content_changes()
    test_oversized_headers()

    print()
    if failures:
        print(f"{failures} failure(s)")
        return 1
    print("all LSP robustness tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
