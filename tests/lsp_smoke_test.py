#!/usr/bin/env python3
"""Scripted stdio JSON-RPC smoke test for mirage-lsp.

Not wired into ctest (it spawns a stateful subprocess and drives a multi-step
protocol conversation, a different shape of test than lexer_asi_test). Run it
manually after building:

    just build
    python3 tests/lsp_smoke_test.py
"""

import json
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
LSP_BINARY = REPO_ROOT / "build" / "mirage-lsp"
FIXTURES = Path(__file__).resolve().parent / "lsp_fixtures"

failures = 0


def check(condition: bool, message: str) -> None:
    global failures
    if not condition:
        failures += 1
        print(f"FAIL: {message}")
    else:
        print(f"ok: {message}")


class Client:
    def __init__(self, binary: Path):
        self.proc = subprocess.Popen(
            [str(binary)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=sys.stderr,
        )
        self._next_id = 1

    def send(self, message: dict) -> None:
        body = json.dumps(message).encode()
        self.proc.stdin.write(f"Content-Length: {len(body)}\r\n\r\n".encode() + body)
        self.proc.stdin.flush()

    def read(self) -> dict:
        headers = {}
        while True:
            line = self.proc.stdout.readline()
            if line in (b"\r\n", b"\n", b""):
                break
            key, _, value = line.decode().partition(":")
            headers[key.strip().lower()] = value.strip()
        length = int(headers["content-length"])
        return json.loads(self.proc.stdout.read(length))

    def request(self, method: str, params: dict) -> dict:
        req_id = self._next_id
        self._next_id += 1
        self.send({"jsonrpc": "2.0", "id": req_id, "method": method, "params": params})
        return self.read()

    def notify(self, method: str, params: dict) -> None:
        self.send({"jsonrpc": "2.0", "method": method, "params": params})

    def close(self) -> int:
        self.request_no_response("shutdown")
        self.notify("exit", {})
        self.proc.stdin.close()
        return self.proc.wait(timeout=5)

    def request_no_response(self, method: str) -> dict:
        req_id = self._next_id
        self._next_id += 1
        self.send({"jsonrpc": "2.0", "id": req_id, "method": method})
        return self.read()


def uri_for(path: Path) -> str:
    return f"file://{path}"


def main() -> int:
    if not LSP_BINARY.exists():
        print(f"FAIL: {LSP_BINARY} not found - run `just build` first")
        return 1

    client = Client(LSP_BINARY)

    # --- initialize: capability scope boundary ---
    init = client.request("initialize", {"processId": None, "rootUri": None, "capabilities": {}})
    caps = init["result"]["capabilities"]
    check(caps.get("hoverProvider") is True, "initialize advertises hoverProvider")
    check(caps.get("definitionProvider") is True, "initialize advertises definitionProvider")
    check("completionProvider" not in caps, "initialize does NOT advertise completionProvider")
    check("referencesProvider" not in caps, "initialize does NOT advertise referencesProvider")
    check("renameProvider" not in caps, "initialize does NOT advertise renameProvider")
    client.notify("initialized", {})

    # --- diagnostics on the error fixture ---
    error_main = FIXTURES / "error_fixture" / "main.mir"
    error_uri = uri_for(error_main)
    client.notify("textDocument/didOpen", {
        "textDocument": {"uri": error_uri, "languageId": "mirage", "version": 1, "text": error_main.read_text()},
    })
    diag_msg = client.read()
    check(diag_msg["method"] == "textDocument/publishDiagnostics", "didOpen triggers publishDiagnostics")
    diags = diag_msg["params"]["diagnostics"]
    check(len(diags) >= 1, "error fixture produces at least one diagnostic")
    if diags:
        d = diags[0]
        check(d["severity"] == 1, "diagnostic severity is Error")
        check(d["range"]["start"]["line"] == 1, f"diagnostic on line 2 (0-based line 1), got {d['range']['start']['line']}")
        check(d["range"]["start"]["character"] == 4, f"diagnostic at column 5 (0-based char 4), got {d['range']['start']['character']}")

    # --- definition + hover on the multi-module fixture ---
    multi_main = FIXTURES / "multi_fixture" / "main.mir"
    multi_uri = uri_for(multi_main)
    text = multi_main.read_text()
    lines = text.splitlines()
    client.notify("textDocument/didOpen", {
        "textDocument": {"uri": multi_uri, "languageId": "mirage", "version": 1, "text": text},
    })
    client.read()  # publishDiagnostics (expected empty)

    def pos_of(line_1based: int, substr: str) -> tuple[int, int]:
        line = lines[line_1based - 1]
        return line_1based - 1, line.index(substr)

    # Cross-module call: greet.hello(name) on line 5.
    l, c = pos_of(5, "hello")
    resp = client.request("textDocument/definition", {"textDocument": {"uri": multi_uri}, "position": {"line": l, "character": c}})
    result = resp["result"]
    check(result is not None, "definition on cross-module call resolves")
    if result:
        check(result["uri"].endswith("greet/greet.mir"), f"cross-module definition points at greet.mir, got {result['uri']}")
        check(result["range"]["start"]["line"] == 0, "cross-module definition on hello's decl line")

    # Local variable: `name` used as an argument on line 5, declared on line 4.
    l, c = pos_of(5, "name")
    resp = client.request("textDocument/definition", {"textDocument": {"uri": multi_uri}, "position": {"line": l, "character": c}})
    result = resp["result"]
    check(result is not None, "definition on local variable use resolves")
    if result:
        check(result["range"]["start"]["line"] == 3, f"local variable definition on its declaration line, got {result['range']['start']['line']}")

    # Hover on the cross-module call: expect the callee's resolved signature.
    l, c = pos_of(5, "hello")
    resp = client.request("textDocument/hover", {"textDocument": {"uri": multi_uri}, "position": {"line": l, "character": c}})
    result = resp["result"]
    check(result is not None, "hover on cross-module call resolves")
    if result:
        value = result["contents"]["value"]
        check("usize" in value and "hello" in value, f"hover shows hello's signature, got {value!r}")

    # Hover on a typed local: expect its type in the tooltip.
    l, c = pos_of(4, "name")
    resp = client.request("textDocument/hover", {"textDocument": {"uri": multi_uri}, "position": {"line": l, "character": c}})
    result = resp["result"]
    check(result is not None, "hover on local variable resolves")
    if result:
        value = result["contents"]["value"]
        check("u8" in value, f"hover shows local's slice-of-u8 type, got {value!r}")

    # --- regression fixture: pointer-receiver methods, `self`, and
    # struct-literal field designators (hover/definition gaps found while
    # investigating "hover only works for some things") ---
    reg_main = FIXTURES / "regression_fixture" / "main.mir"
    reg_uri = uri_for(reg_main)
    reg_text = reg_main.read_text()
    reg_lines = reg_text.splitlines()
    client.notify("textDocument/didOpen", {
        "textDocument": {"uri": reg_uri, "languageId": "mirage", "version": 1, "text": reg_text},
    })
    diag_msg = client.read()
    check(diag_msg["params"]["diagnostics"] == [], "regression fixture compiles with no diagnostics")

    def reg_pos_of(line_1based: int, substr: str) -> tuple[int, int]:
        line = reg_lines[line_1based - 1]
        return line_1based - 1, line.index(substr)

    def reg_hover(line_1based: int, substr: str) -> dict | None:
        l, c = reg_pos_of(line_1based, substr)
        resp = client.request("textDocument/hover", {"textDocument": {"uri": reg_uri}, "position": {"line": l, "character": c}})
        return resp["result"]

    def reg_definition(line_1based: int, substr: str) -> dict | None:
        l, c = reg_pos_of(line_1based, substr)
        resp = client.request("textDocument/definition", {"textDocument": {"uri": reg_uri}, "position": {"line": l, "character": c}})
        return resp["result"]

    # Bug 1: method call through a pointer receiver (`p: *Parser`).
    result = reg_hover(35, "current_location")
    check(result is not None, "hover resolves a method called through a pointer receiver")
    if result:
        value = result["contents"]["value"]
        check("current_location" in value, f"hover shows the method's signature, got {value!r}")
    result = reg_definition(35, "current_location")
    check(result is not None, "definition resolves a method called through a pointer receiver")
    if result:
        check(result["range"]["start"]["line"] == 16, f"definition points at the method's decl line, got {result['range']['start']['line']}")

    # Bug 2: `self` itself, and a `self.field` chain.
    result = reg_hover(17, "self")
    check(result is not None, "hover resolves the `self` parameter itself")
    if result:
        check("Parser" in result["contents"]["value"], f"hover shows self's type, got {result['contents']['value']!r}")
    result = reg_hover(18, "self")
    check(result is not None, "hover resolves `self` used in `self.pos`")
    result = reg_hover(18, "pos")
    check(result is not None, "hover resolves the field in `self.pos`")
    result = reg_definition(18, "pos")
    check(result is not None, "definition resolves the field in `self.pos`")
    if result:
        check(result["range"]["start"]["line"] == 12, f"definition points at Parser.pos's decl line, got {result['range']['start']['line']}")

    # Bug 3: struct-literal field designators, including one that collides
    # with an in-scope local of the same name (`name`, declared line 36) -
    # must resolve to the struct field, not the shadowing local.
    result = reg_hover(45, "member")
    check(result is not None, "hover resolves a struct-literal field designator with no name collision")
    result = reg_definition(44, "name")
    check(result is not None, "definition resolves a struct-literal field designator that collides with a local")
    if result:
        check(result["uri"] == reg_uri, "shadowed field designator definition stays in the same file")
        check(result["range"]["start"]["line"] == 1, f"shadowed field designator resolves to NamedType.name's decl line (1), not the local's (35), got {result['range']['start']['line']}")

    # Sanity: local variable hover away from its declaration site still works.
    result = reg_hover(41, "member_slot")
    check(result is not None, "hover resolves a local variable used away from its declaration")

    client.notify("textDocument/didClose", {"textDocument": {"uri": reg_uri}})
    client.read()  # publishDiagnostics

    # --- bad_import fixture: a failed import(...) must not cascade into
    # "unknown identifier" for every downstream qualified-name use ---
    bad_import_main = FIXTURES / "bad_import_fixture" / "main.mir"
    bad_import_uri = uri_for(bad_import_main)
    client.notify("textDocument/didOpen", {
        "textDocument": {"uri": bad_import_uri, "languageId": "mirage", "version": 1, "text": bad_import_main.read_text()},
    })
    diag_msg = client.read()
    diags = diag_msg["params"]["diagnostics"]
    messages = [d["message"] for d in diags]
    check(sum("unknown identifier" in m for m in messages) == 0, f"failed import does not cascade into 'unknown identifier', got {messages}")
    check(any("cannot resolve import" in m for m in messages), f"the import itself is still diagnosed, got {messages}")
    check(any("Decl" in m for m in messages), f"downstream usage 'ast.Decl' gets its own diagnostic, got {messages}")
    check(any("NamedType" in m for m in messages), f"downstream usage 'ast.NamedType' gets its own diagnostic, got {messages}")
    client.notify("textDocument/didClose", {"textDocument": {"uri": bad_import_uri}})
    client.read()  # publishDiagnostics

    # --- rapid/malformed edits: server must stay alive and keep answering ---
    for i in range(5):
        garbled = text[: max(0, len(text) - i * 3)] + "{{{ garbage )))" * i
        client.notify("textDocument/didChange", {
            "textDocument": {"uri": multi_uri, "version": i + 2},
            "contentChanges": [{"text": garbled}],
        })
        client.read()  # publishDiagnostics for each rapid edit
    client.notify("textDocument/didChange", {
        "textDocument": {"uri": multi_uri, "version": 99},
        "contentChanges": [{"text": text}],
    })
    client.read()
    resp = client.request("textDocument/hover", {"textDocument": {"uri": multi_uri}, "position": {"line": 0, "character": 8}})
    check("result" in resp, "server still answers requests after rapid/malformed edits")
    check(client.proc.poll() is None, "server process is still alive after rapid/malformed edits")

    # --- shutdown / exit ---
    client.notify("textDocument/didClose", {"textDocument": {"uri": multi_uri}})
    close_notification = client.read()
    check(close_notification["params"]["diagnostics"] == [], "didClose publishes empty diagnostics")

    exit_code = client.close()
    check(exit_code == 0, f"process exits 0 after shutdown+exit, got {exit_code}")

    print()
    if failures:
        print(f"{failures} check(s) FAILED")
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
