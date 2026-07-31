#!/usr/bin/env python3
"""Scripted stdio JSON-RPC smoke test for mirage-lsp.

Not wired into ctest (it spawns a stateful subprocess and drives a multi-step
protocol conversation, a different shape of test than lexer_asi_test). Run it
manually after building:

    just build
    python3 tests/lsp_smoke_test.py
"""

import json
import queue
import subprocess
import sys
import threading
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
        # A single persistent background thread owns proc.stdout exclusively and pushes
        # every parsed message onto this queue - read()/read_with_timeout() only ever pull
        # from the queue, never touch the pipe directly. This matters now that timeouts are
        # a normal occurrence (the parser-hang/cancellation tests expect them): an earlier
        # version spawned a fresh reader thread per read_with_timeout() call, and a timed-out
        # call left its thread still blocked on the pipe - the next call's new thread then
        # raced it for the same bytes, corrupting the Content-Length framing and hanging the
        # whole test. One long-lived reader avoids that by construction.
        self._queue: "queue.Queue[dict | None]" = queue.Queue()
        self._reader_thread = threading.Thread(target=self._reader_loop, daemon=True)
        self._reader_thread.start()

    def _reader_loop(self) -> None:
        while True:
            headers = {}
            while True:
                line = self.proc.stdout.readline()
                if line in (b"\r\n", b"\n", b""):
                    break
                key, _, value = line.decode().partition(":")
                headers[key.strip().lower()] = value.strip()
            if "content-length" not in headers:
                self._queue.put(None)
                return
            length = int(headers["content-length"])
            body = self.proc.stdout.read(length)
            self._queue.put(json.loads(body))

    def send(self, message: dict) -> None:
        body = json.dumps(message).encode()
        self.proc.stdin.write(f"Content-Length: {len(body)}\r\n\r\n".encode() + body)
        self.proc.stdin.flush()

    def read(self) -> dict:
        msg = self._queue.get()
        if msg is None:
            raise EOFError("server closed its output stream")
        return msg

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

    def read_with_timeout(self, timeout: float) -> dict | None:
        """Like read(), but returns None instead of hanging forever if the
        server is stuck (e.g. a parser infinite-loop regression) rather than
        crashed - a plain read() would just block the whole test process."""
        try:
            return self._queue.get(timeout=timeout)
        except queue.Empty:
            return None

    def request_with_timeout(self, method: str, params: dict, timeout: float) -> dict | None:
        req_id = self._next_id
        self._next_id += 1
        self.send({"jsonrpc": "2.0", "id": req_id, "method": method, "params": params})
        return self.read_with_timeout(timeout)


def uri_for(path: Path) -> str:
    return f"file://{path}"


def main() -> int:
    if not LSP_BINARY.exists():
        print(f"FAIL: {LSP_BINARY} not found - run `just build` first")
        return 1

    client = Client(LSP_BINARY)

    # --- initialize: capability scope boundary ---
    # Declares dynamicRegistration so the server registers its '**/*.mir' watcher below.
    # Without it the server skips registration entirely and never receives
    # didChangeWatchedFiles, which is the degraded-but-working path for simpler clients.
    init = client.request("initialize", {
        "processId": None,
        "rootUri": None,
        "capabilities": {"workspace": {"didChangeWatchedFiles": {"dynamicRegistration": True}}},
    })
    caps = init["result"]["capabilities"]
    check(caps.get("hoverProvider") is True, "initialize advertises hoverProvider")
    check(caps.get("definitionProvider") is True, "initialize advertises definitionProvider")
    check(caps.get("completionProvider", {}).get("triggerCharacters") == ["."],
          f"initialize advertises completionProvider with '.' trigger, got {caps.get('completionProvider')}")
    check(caps.get("referencesProvider") is True, "initialize advertises referencesProvider")
    check("renameProvider" not in caps, "initialize does NOT advertise renameProvider")
    client.notify("initialized", {})

    # The server's one outbound request: a file watcher, registered after 'initialized'
    # because that is the point the client is ready to receive server-initiated requests.
    registration = client.read_with_timeout(5)
    check(registration is not None and registration.get("method") == "client/registerCapability",
          f"server registers a file watcher after 'initialized', got {registration}")
    if registration and registration.get("method") == "client/registerCapability":
        regs = registration["params"]["registrations"]
        check(any(r["method"] == "workspace/didChangeWatchedFiles" for r in regs),
              f"the registration is for didChangeWatchedFiles, got {regs}")
        check(any("**/*.mir" in json.dumps(r.get("registerOptions", {})) for r in regs),
              f"the watcher matches '.mir' files, got {regs}")
        # Answer it, as a real client would.
        client.send({"jsonrpc": "2.0", "id": registration["id"], "result": None})

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

    # --- completion ---
    # Member completion right after 'greet.' (cursor sits exactly where 'hello' starts, i.e.
    # nothing typed after the dot yet) must offer the imported module's exported function.
    l, c = pos_of(5, "hello")
    resp = client.request("textDocument/completion", {"textDocument": {"uri": multi_uri}, "position": {"line": l, "character": c}})
    labels = [it["label"] for it in resp["result"]]
    check("hello" in labels, f"completion right after 'greet.' offers the module's hello function, got {labels}")

    # General completion (no '.' trigger) inside main()'s body must offer: locals declared
    # before the cursor (name, count), module-level symbols (greet, main), and keywords.
    l, c = pos_of(6, "return")
    resp = client.request("textDocument/completion", {"textDocument": {"uri": multi_uri}, "position": {"line": l, "character": c}})
    labels = [it["label"] for it in resp["result"]]
    check("name" in labels, f"general completion inside main() offers the local 'name', got {labels}")
    check("count" in labels, f"general completion inside main() offers the local 'count', got {labels}")
    check("greet" in labels, f"general completion inside main() offers the module symbol 'greet', got {labels}")
    check("return" in labels, f"general completion inside main() offers the 'return' keyword, got {labels}")

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

    def reg_completion(line_1based: int, substr: str) -> list:
        l, c = reg_pos_of(line_1based, substr)
        resp = client.request("textDocument/completion", {"textDocument": {"uri": reg_uri}, "position": {"line": l, "character": c}})
        return [it["label"] for it in resp["result"]]

    # Member completion right after 'self.' (Parser { data: Arena, pos: u32 }, with an
    # impl-block method current_location) must offer both fields and the method.
    labels = reg_completion(18, "pos")
    check("pos" in labels, f"member completion after 'self.' offers the struct field 'pos', got {labels}")
    check("data" in labels, f"member completion after 'self.' offers the struct field 'data', got {labels}")
    check("current_location" in labels, f"member completion after 'self.' offers the impl method, got {labels}")

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

    # Bug 4: optional argument names in function-pointer types (fn(item: *T) -> bool)
    # must appear in hover text.
    result = reg_hover(64, "visit")
    check(result is not None, "hover resolves a local of a named function-pointer type")
    if result:
        value = result["contents"]["value"]
        check("item: *NamedType" in value, f"hover shows the fn-pointer-type's argument name, got {value!r}")

    client.notify("textDocument/didClose", {"textDocument": {"uri": reg_uri}})
    client.read()  # publishDiagnostics

    # --- optional_error fixture: ignorable errors ('?error(...)'). The '?' introduces a
    # new ast::Type alternative, and the LSP's own ast::Type printer ends in a
    # non-exhaustive `else { return "<?>"; }` — a missed case degrades silently rather
    # than failing to compile, so it is pinned here. Lines are located by searching for
    # each declaration's name so the fixture can be reordered freely. ---
    opt_main = FIXTURES / "optional_error_fixture" / "main.mir"
    opt_uri = uri_for(opt_main)
    opt_text = opt_main.read_text()
    opt_lines = opt_text.splitlines()
    client.notify("textDocument/didOpen", {
        "textDocument": {"uri": opt_uri, "languageId": "mirage", "version": 1, "text": opt_text},
    })
    diag_msg = client.read()
    check(diag_msg["params"]["diagnostics"] == [], "optional_error fixture compiles with no diagnostics")

    def opt_pos(anchor: str, substr: str | None = None) -> tuple[int, int]:
        """Line containing 'anchor'; column of 'substr' (default: 'anchor') within it."""
        line_idx = next(i for i, line in enumerate(opt_lines) if anchor in line)
        return line_idx, opt_lines[line_idx].index(substr if substr is not None else anchor)

    def opt_hover(anchor: str, substr: str | None = None) -> str | None:
        l, c = opt_pos(anchor, substr)
        resp = client.request("textDocument/hover", {"textDocument": {"uri": opt_uri}, "position": {"line": l, "character": c}})
        result = resp["result"]
        return result["contents"]["value"] if result else None

    # The marking must survive into hover text — a signature rendered without it would
    # claim the caller has to handle an error the compiler lets them drop.
    value = opt_hover("fn alloc_sugar", "alloc_sugar")
    check(value is not None and "?error(Alloc_Error)" in value,
          f"hover on a '?T'-returning fn shows '?error(...)', got {value!r}")

    value = opt_hover("fn touch_explicit", "touch_explicit")
    check(value is not None and "-> ?error(Alloc_Error)" in value,
          f"hover on a lone '?error(...)' return shows the marking, got {value!r}")

    # ...and must NOT leak onto the non-optional counterpart, which is the failure mode if
    # the printer keyed off anything other than the union's own is_optional.
    value = opt_hover("fn alloc_plain", "alloc_plain")
    check(value is not None and "error(Alloc_Error)" in value and "?error" not in value,
          f"hover on the non-optional counterpart shows no '?', got {value!r}")

    value = opt_hover("pub type Allocator", "Allocator")
    check(value is not None and "?error(Alloc_Error)" in value,
          f"hover on a trait shows '?' on its method's return type, got {value!r}")

    # Go-to-definition and hover must see THROUGH the '?' to the type it marks. The trait
    # case is the one that matters: trait signatures resolve during layout_trait, before
    # the enum it names has been laid out, so a shallow named-type resolution fails here
    # while still working in the free function above.
    for anchor, label in (("fn alloc_sugar", "free function"),
                          ("fn alloc(self", "trait method"),
                          ("type Alloc_Fn", "fn-pointer type")):
        l, c = opt_pos(anchor, "?Alloc_Error")
        pos = {"line": l, "character": c + 1}  # the identifier, just past the '?'
        resp = client.request("textDocument/hover", {"textDocument": {"uri": opt_uri}, "position": pos})
        value = resp["result"]["contents"]["value"] if resp["result"] else None
        check(value is not None and "enum Alloc_Error" in value,
              f"hover reaches the type inside '?Alloc_Error' in a {label}, got {value!r}")
        resp = client.request("textDocument/definition", {"textDocument": {"uri": opt_uri}, "position": pos})
        check(resp["result"] is not None,
              f"go-to-definition reaches the type inside '?Alloc_Error' in a {label}")

    # A dropped trailing error slot leaves the call yielding one value, so the local's
    # hover must be that value's type — not a tuple, and not the error union.
    value = opt_hover("const dropped", "dropped")
    check(value is not None and "anyptr" in value and "error" not in value,
          f"hover on a var bound from a dropped-error call shows the surviving value's type, got {value!r}")

    client.notify("textDocument/didClose", {"textDocument": {"uri": opt_uri}})
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

    # --- parser progress-guard regression: a stray token in a parameter list
    # used to spin the parser forever (see ast.cpp's LoopProgressGuard), which
    # hung the single request thread. '&x: i32' is the easiest reproduction:
    # none of expect_identifier()/expect(Colon)/parse_type() can consume '&'.
    # None of the pre-existing malformed-edit garbage above reproduces this,
    # since '{', '}', ')' are all consumed by the loops' own check(RBrace/RParen)
    # guards and 'garbage' is a plain Identifier consumable by expect_identifier().
    hang_main = FIXTURES / "hang_fixture" / "main.mir"
    hang_uri = uri_for(hang_main)
    client.notify("textDocument/didOpen", {
        "textDocument": {"uri": hang_uri, "languageId": "mirage", "version": 1, "text": hang_main.read_text()},
    })
    diag_msg = client.read_with_timeout(5)
    check(diag_msg is not None, "server responds to didOpen on the parser-hang reproducer within 5s (not stuck spinning)")

    resp = client.request_with_timeout(
        "textDocument/hover", {"textDocument": {"uri": hang_uri}, "position": {"line": 0, "character": 3}}, timeout=5)
    check(resp is not None, "server still answers a request after parsing the hang reproducer")
    check(client.proc.poll() is None, "server process is still alive after the parser-hang reproducer")

    client.notify("textDocument/didClose", {"textDocument": {"uri": hang_uri}})
    client.read_with_timeout(5)

    # --- debounce coalescing: a burst of didChange for the same file sent back-to-back
    # (no reads in between, so the worker can't have started analysing any of the earlier
    # ones yet) must still result in the LATEST buffer content being used once a request
    # actually runs - not some earlier edit in the burst that got coalesced away for
    # diagnostics purposes.
    for i in range(5):
        client.notify("textDocument/didChange", {
            "textDocument": {"uri": multi_uri, "version": 200 + i},
            "contentChanges": [{"text": text + f"// burst {i}\n"}],
        })
    client.notify("textDocument/didChange", {
        "textDocument": {"uri": multi_uri, "version": 210},
        "contentChanges": [{"text": text + "fn zzz_debounce_marker_zzz() {}\n"}],
    })
    marker_line = len(text.splitlines())
    resp = client.request_with_timeout(
        "textDocument/hover",
        {"textDocument": {"uri": multi_uri}, "position": {"line": marker_line, "character": 4}},
        timeout=5)
    check(resp is not None, "server answers a request sent right after a rapid didChange burst")
    if resp is not None:
        result = resp.get("result")
        check(result is not None and "zzz_debounce_marker_zzz" in result["contents"]["value"],
              f"hover after a didChange burst reflects the LATEST edit, not an earlier coalesced one, got {resp}")
    # Drain the eventually-fired debounced publishDiagnostics notification(s) for the burst
    # so they don't desynchronize the message stream for later checks.
    while (msg := client.read_with_timeout(1)) is not None and "method" in msg:
        pass

    # --- $/cancelRequest: a request cancelled immediately must never receive a response
    # for that id, and the server must stay fully responsive afterward. Best-effort timing:
    # queue up a backlog of didChange tasks first so the worker is still busy with them by
    # the time the cancellation arrives, biasing (not guaranteeing - this is a cooperative,
    # coarse cancellation model) toward the request not having started yet.
    for i in range(20):
        client.notify("textDocument/didChange", {
            "textDocument": {"uri": multi_uri, "version": 300 + i},
            "contentChanges": [{"text": text + f"// cancel-backlog {i}\n"}],
        })
    cancel_id = client._next_id
    client.send({"jsonrpc": "2.0", "id": cancel_id, "method": "textDocument/hover",
                 "params": {"textDocument": {"uri": multi_uri}, "position": {"line": 0, "character": 8}}})
    client._next_id += 1
    client.notify("$/cancelRequest", {"id": cancel_id})

    got_cancelled_response = False
    while (msg := client.read_with_timeout(2)) is not None:
        if msg.get("id") == cancel_id and ("result" in msg or "error" in msg):
            got_cancelled_response = True
            break
    check(not got_cancelled_response, "a request cancelled immediately after sending gets no response")

    resp = client.request_with_timeout(
        "textDocument/hover", {"textDocument": {"uri": multi_uri}, "position": {"line": 0, "character": 8}}, timeout=5)
    check(resp is not None, "server still answers requests after a cancellation")
    check(client.proc.poll() is None, "server process is still alive after a cancellation")

    # Restore multi_uri to its clean original text before the remaining checks.
    client.notify("textDocument/didChange", {
        "textDocument": {"uri": multi_uri, "version": 999},
        "contentChanges": [{"text": text}],
    })
    client.read_with_timeout(5)

    # --- diagnostics staleness: fixing an error in file B must clear B's squiggles even
    # when the re-analysis that discovers the fix was triggered by editing a DIFFERENT file
    # (A) in the same module closure - group_diagnostics_by_file only ever produces an
    # entry for a file that currently has a diagnostic, so B (now clean, and not itself the
    # edited file) would otherwise never be told to clear its old squiggles.
    stale_dir = FIXTURES / "diagnostics_staleness_fixture"
    a_path, b_path = stale_dir / "a.mir", stale_dir / "b.mir"
    a_uri, b_uri = uri_for(a_path), uri_for(b_path)
    original_b_text = b_path.read_text()
    try:
        client.notify("textDocument/didOpen", {
            "textDocument": {"uri": a_uri, "languageId": "mirage", "version": 1, "text": a_path.read_text()},
        })
        # b.mir isn't open, so its on-disk (erroring) content is read fresh; both a.mir
        # (clean) and b.mir (the error) get published this round, in some order.
        diags_by_file = {}
        for _ in range(2):
            msg = client.read_with_timeout(5)
            check(msg is not None, "diagnostics published for both files in the staleness fixture's closure")
            if msg:
                diags_by_file[msg["params"]["uri"]] = msg["params"]["diagnostics"]
        check(len(diags_by_file.get(b_uri, [])) >= 1, f"b.mir starts with a real diagnostic, got {diags_by_file}")

        # Fix b.mir on disk directly (simulating a fix made outside this session, e.g. in
        # another editor, or a file nobody has open as a buffer here) - NOT via didOpen/
        # didChange, so the only way the server can ever notice is by re-analysing the
        # closure for an unrelated reason.
        b_path.write_text("pub fn bar() {\n    mut x: u32 = 0\n}\n")

        # Edit A, not B, and confirm B's now-stale error gets explicitly cleared anyway.
        client.notify("textDocument/didChange", {
            "textDocument": {"uri": a_uri, "version": 2},
            "contentChanges": [{"text": a_path.read_text() + "\npub fn foo2() {}\n"}],
        })
        cleared_b = False
        for _ in range(3):
            msg = client.read_with_timeout(5)
            if msg is None:
                break
            if msg["params"]["uri"] == b_uri and msg["params"]["diagnostics"] == []:
                cleared_b = True
            if msg["params"]["uri"] != b_uri:
                continue
        check(cleared_b, "editing A publishes an empty-array clearing notification for B once B's fix is discovered")
    finally:
        b_path.write_text(original_b_text)

    client.notify("textDocument/didClose", {"textDocument": {"uri": a_uri}})
    client.read_with_timeout(5)

    # --- didChangeWatchedFiles: an external change alone must refresh analysis ---
    #
    # The block above needed an unrelated EDIT to A before the server noticed B's on-disk
    # fix. That is the bug: cached analysis was invalidated only by editor notifications, so
    # a dependency changed by a git pull kept stale diagnostics indefinitely, bounded only
    # by the unrelated MAX_CACHED_MODULES LRU. Here nothing is edited - only the watched-file
    # notification arrives.
    original_b_text = b_path.read_text()
    try:
        client.notify("textDocument/didOpen", {
            "textDocument": {"uri": a_uri, "languageId": "mirage", "version": 1, "text": a_path.read_text()},
        })
        watched_initial = {}
        for _ in range(2):
            msg = client.read_with_timeout(5)
            if msg is None:
                break
            watched_initial[msg["params"]["uri"]] = msg["params"]["diagnostics"]
        check(len(watched_initial.get(b_uri, [])) >= 1,
              f"b.mir starts with a real diagnostic again, got {watched_initial}")

        b_path.write_text("pub fn bar() {\n    mut x: u32 = 0\n}\n")

        # No didOpen, no didChange - just the notification a file watcher would send.
        client.notify("workspace/didChangeWatchedFiles", {
            "changes": [{"uri": b_uri, "type": 2}],  # 2 = Changed
        })

        cleared_externally = False
        for _ in range(4):
            msg = client.read_with_timeout(5)
            if msg is None:
                break
            if msg["params"]["uri"] == b_uri and msg["params"]["diagnostics"] == []:
                cleared_externally = True
        check(cleared_externally,
              "an external change to B clears B's diagnostics with no editor edit")
    finally:
        b_path.write_text(original_b_text)

    client.notify("textDocument/didClose", {"textDocument": {"uri": a_uri}})
    client.read_with_timeout(5)

    # --- didClose keeps diagnostics a still-open closure is responsible for ---
    #
    # Clearing unconditionally is right for diagnostics derived from the closed BUFFER and
    # wrong for the rest: b.mir has an on-disk error and sits in the same module directory
    # as a.mir, so with a.mir still open it is still being analysed every round. Only its
    # squiggles used to disappear, until some unrelated edit re-touched the closure.
    client.notify("textDocument/didOpen", {
        "textDocument": {"uri": a_uri, "languageId": "mirage", "version": 1, "text": a_path.read_text()},
    })
    for _ in range(2):
        if client.read_with_timeout(5) is None:
            break
    client.notify("textDocument/didOpen", {
        "textDocument": {"uri": b_uri, "languageId": "mirage", "version": 1, "text": b_path.read_text()},
    })
    for _ in range(2):
        if client.read_with_timeout(5) is None:
            break

    # Close B while A -- in the same closure -- stays open.
    client.notify("textDocument/didClose", {"textDocument": {"uri": b_uri}})
    b_after_close = None
    for _ in range(3):
        msg = client.read_with_timeout(5)
        if msg is None:
            break
        if msg["params"]["uri"] == b_uri:
            b_after_close = msg["params"]["diagnostics"]
            break
    check(b_after_close is not None and len(b_after_close) >= 1,
          f"closing a file still inside an open closure keeps its on-disk diagnostics, got {b_after_close}")

    # With nothing else open, the same close clears -- the ordinary case, and what the spec
    # asks for.
    client.notify("textDocument/didClose", {"textDocument": {"uri": a_uri}})
    client.read_with_timeout(5)
    client.notify("textDocument/didOpen", {
        "textDocument": {"uri": b_uri, "languageId": "mirage", "version": 1, "text": b_path.read_text()},
    })
    client.read_with_timeout(5)
    client.notify("textDocument/didClose", {"textDocument": {"uri": b_uri}})
    b_alone = None
    for _ in range(3):
        msg = client.read_with_timeout(5)
        if msg is None:
            break
        if msg["params"]["uri"] == b_uri:
            b_alone = msg["params"]["diagnostics"]
            break
    check(b_alone == [],
          f"closing the last file covering an error still clears its diagnostics, got {b_alone}")

    # A malformed or empty notification must be ignored, not crash the server.
    client.notify("workspace/didChangeWatchedFiles", {"changes": []})
    client.notify("workspace/didChangeWatchedFiles", {"changes": [{"type": 2}]})
    client.notify("workspace/didChangeWatchedFiles", {})
    still_alive = client.request("initialize", {"processId": None, "rootUri": None, "capabilities": {}})
    check("result" in still_alive or "error" in still_alive,
          "server survives malformed didChangeWatchedFiles notifications")

    # --- Find All References ---
    #
    # The two shapes that motivated enabling this at all are match-arm variant patterns and
    # bitset literal flags. Neither is an expression node, so a walker driven only by
    # on_expr saw neither — and match arms are where variants are most used, so a references
    # request on a variant used to report close to nothing.
    sym_main = FIXTURES / "symbols_fixture" / "main.mir"
    sym_uri = uri_for(sym_main)
    sym_text = sym_main.read_text()
    sym_lines = sym_text.splitlines()
    client.notify("textDocument/didOpen", {
        "textDocument": {"uri": sym_uri, "languageId": "mirage", "version": 1, "text": sym_text},
    })
    sym_diag = client.read()
    check(sym_diag["params"]["diagnostics"] == [], "symbols fixture compiles with no diagnostics")

    def sym_pos(anchor: str, substr: str | None = None) -> tuple[int, int]:
        """Line containing 'anchor'; column of 'substr' (default: 'anchor') within it."""
        line_idx = next(i for i, line in enumerate(sym_lines) if anchor in line)
        return line_idx, sym_lines[line_idx].index(substr if substr is not None else anchor)

    def sym_references(anchor: str, substr: str | None = None, include_declaration: bool = True) -> list:
        l, c = sym_pos(anchor, substr)
        resp = client.request("textDocument/references", {
            "textDocument": {"uri": sym_uri},
            "position": {"line": l, "character": c},
            "context": {"includeDeclaration": include_declaration},
        })
        return resp["result"] or []

    def ref_lines(refs: list) -> list[int]:
        return sorted(r["range"]["start"]["line"] + 1 for r in refs)

    # A module-scope function: declaration plus three call sites.
    double_refs = sym_references("pub fn double", "double")
    check(len(double_refs) >= 4,
          f"references on a function find its declaration and all call sites, got {ref_lines(double_refs)}")
    decl_line = sym_pos("pub fn double", "double")[0] + 1
    check(decl_line in ref_lines(double_refs),
          "references include the declaration when includeDeclaration is true")

    without_decl = sym_references("pub fn double", "double", include_declaration=False)
    check(len(without_decl) < len(double_refs),
          f"includeDeclaration=false returns fewer results ({len(without_decl)} vs {len(double_refs)})")

    # A local: uses stay inside the one function.
    counter_refs = sym_references("mut counter: i32", "counter")
    check(len(counter_refs) >= 4,
          f"references on a local find all of its uses, got {ref_lines(counter_refs)}")

    # A variant used only in match/switch arm patterns and one construction site. This is
    # the case that found nothing before VariantPattern carried a location.
    opened_refs = sym_references("    Opened: struct", "Opened")
    opened_at = ref_lines(opened_refs)
    match_arm_line = sym_pos("        .Opened(p): p.id", ".Opened")[0] + 1
    switch_arm_line = sym_pos("        .Opened(p): { return p.id }", ".Opened")[0] + 1
    check(match_arm_line in opened_at,
          f"references on a variant find its use in a MATCH arm pattern (line {match_arm_line}, got {opened_at})")
    check(switch_arm_line in opened_at,
          f"references on a variant find its use in a SWITCH arm pattern (line {switch_arm_line}, got {opened_at})")

    # A bitset flag written inside a '{.A, .B}' literal: stored as a name, not an Expr.
    close_refs = sym_references("    Close", "Close")
    close_at = ref_lines(close_refs)
    literal_line = sym_pos("{.Close, .Flush}", ".Close")[0] + 1
    check(literal_line in close_at,
          f"references on a bitset flag find its use in a '{{.A, .B}}' literal (line {literal_line}, got {close_at})")

    # '.Read' is used via '+=', which IS an expression - so it was already found. Pinned so
    # the new pattern/literal handling cannot regress the path that already worked.
    read_refs = sym_references("    Read", "Read")
    compound_line = sym_pos("modes += .Read", ".Read")[0] + 1
    check(compound_line in ref_lines(read_refs),
          f"references on a bitset flag still find compound-assignment uses (line {compound_line})")

    # A trait method called through a handle. Anchored on the CALL, not on the declaration
    # inside 'type Shape = trait {...}': a position inside a type declaration is not a
    # resolvable reference target, so anchoring there returns nothing.
    area_refs = sym_references("return shape.area()", "area")
    area_at = ref_lines(area_refs)
    call_line = sym_pos("return shape.area()", "area")[0] + 1
    check(call_line in area_at,
          f"references on a trait-handle method call find the call site, got {area_at}")

    # A position that resolves to nothing must answer, not error.
    blank = client.request("textDocument/references", {
        "textDocument": {"uri": sym_uri},
        "position": {"line": 0, "character": 0},
        "context": {"includeDeclaration": True},
    })
    check("error" not in blank and blank["result"] == [],
          f"references on a comment returns an empty list, got {blank.get('result')}")

    # Omitting 'context' entirely must not throw - some clients do.
    no_context_line, no_context_col = sym_pos("pub fn double", "double")
    no_context = client.request("textDocument/references", {
        "textDocument": {"uri": sym_uri},
        "position": {"line": no_context_line, "character": no_context_col},
    })
    check("error" not in no_context and no_context["result"],
          "references without a 'context' field is tolerated")

    # --- member completion on a trait handle and a bitset ---
    #
    # Both branches of add_type_members were added without end-to-end coverage: completion
    # has its own cursor-classification step that hover and go-to-definition do not share,
    # so being right by symmetry with a verified fix proved nothing about this path.
    #
    # A trait handle's methods live on the trait, not on any implementing type, so the
    # generic method sweep never finds them. A bitset declares no members of its own, so
    # its completions have to come from its member enum.
    def sym_completion(anchor: str, substr: str) -> list:
        l, c = sym_pos(anchor, substr)
        resp = client.request("textDocument/completion", {
            "textDocument": {"uri": sym_uri},
            "position": {"line": l, "character": c},
        })
        return resp["result"] or []

    # Cursor immediately after 'shape.' — i.e. at the start of the method name.
    shape_items = sym_completion("return shape.area()", "area")
    shape_labels = [it["label"] for it in shape_items]
    check("area" in shape_labels and "perimeter" in shape_labels,
          f"completion on a trait handle offers the trait's methods, got {shape_labels}")
    check(any(it["label"] == "area" and it.get("detail") == "trait method" for it in shape_items),
          f"trait-handle completions are labelled 'trait method', got {shape_items}")

    # --- contextual '.Name' completion (DEFERRED §3) ---
    #
    # A bitset flag, an enum field and a tagged-union variant are only ever written
    # contextually, as a bare '.Name' taking its meaning from the EXPECTED type. That form has
    # no receiver chain, so completion used to fall through to the no-receiver path and offer
    # keywords and locals — and add_type_members' Bitset arm was unreachable outright.
    #
    # Each assertion below exercises a different way the expected type is found: see
    # expected_type_at. The first two are answered by sema (the buffer parses, so the
    # DotIdentExpr carries its own resolved type); the rest are the token-level fallbacks that
    # matter when it does not.

    # Inside a bitset literal, against the declared type of the variable being initialised.
    flag_items = sym_completion("{.Close, .Flush}", "Close")
    flag_labels = [it["label"] for it in flag_items]
    check("Close" in flag_labels and "Flush" in flag_labels and "Read" in flag_labels,
          f"contextual '.flag' inside a bitset literal offers the member enum's fields, got {flag_labels}")
    check(any(it["label"] == "Close" and it.get("detail") == "bitset flag" for it in flag_items),
          f"bitset flag completions are labelled 'bitset flag', got {flag_items}")
    check("if" not in flag_labels and "counter" not in flag_labels,
          f"contextual '.flag' offers ONLY flags, not keywords or locals, got {flag_labels}")

    # 'modes += .Read' — compound assignment to an already-typed local.
    compound_items = sym_completion("modes += .Read", "Read")
    compound_labels = [it["label"] for it in compound_items]
    check("Read" in compound_labels and "Close" in compound_labels,
          f"contextual '.flag' after '+=' offers the bitset's flags, got {compound_labels}")

    # A match arm pattern: '.Opened' takes its meaning from the scrutinee's type.
    arm_items = sym_completion(".Opened(p): p.id,", "Opened")
    arm_labels = [it["label"] for it in arm_items]
    check("Opened" in arm_labels and "Closed" in arm_labels,
          f"contextual '.Variant' in a match arm offers the scrutinee's variants, got {arm_labels}")

    # And in a switch arm, which reaches the same code by a different route.
    switch_arm_items = sym_completion(".Opened(p): { return p.id }", "Opened")
    switch_arm_labels = [it["label"] for it in switch_arm_items]
    check("Opened" in switch_arm_labels and "Closed" in switch_arm_labels,
          f"contextual '.Variant' in a switch arm offers the scrutinee's variants, got {switch_arm_labels}")

    # A contextual variant used as a call ARGUMENT — the shape only sema can answer, since the
    # expected type comes from the callee's parameter list.
    call_arg_items = sym_completion("describe(.Opened{.id = 1})", "Opened")
    call_arg_labels = [it["label"] for it in call_arg_items]
    check("Opened" in call_arg_labels and "Closed" in call_arg_labels,
          f"contextual '.Variant' as a call argument offers the parameter's variants, got {call_arg_labels}")

    # --- the two cursor conventions must stay different (LSPH-10) ---
    #
    # completion asks "did the cursor just FOLLOW this token" (half-open at the LEFT), while
    # common.cpp's token_at asks "is the cursor ON this token" (half-open at the RIGHT). The
    # inversion is what lets completion tell "mid-token, filter by the typed prefix" from
    # "immediately after a token, nothing typed yet".
    #
    # The isolating position is the END of an identifier that is immediately followed by '.':
    # completion reads that as still being on 'shape' (offer values, filtered by "shape"),
    # while token_at's convention would read it as being on the '.' (offer shape's MEMBERS).
    # Pinned because the two conventions look like an inconsistency and are one edit from
    # being "fixed" into agreement.
    end_line, end_col = sym_pos("return shape.area()", "shape")
    at_ident_end = client.request("textDocument/completion", {
        "textDocument": {"uri": sym_uri},
        "position": {"line": end_line, "character": end_col + len("shape")},
    })["result"] or []
    end_labels = [it["label"] for it in at_ident_end]
    check("shape" in end_labels,
          f"cursor at the end of 'shape' completes the identifier itself, got {end_labels}")
    check("area" not in end_labels,
          "cursor at the end of 'shape' must NOT offer members - that is token_at's "
          f"cursor convention, not completion's, got {end_labels}")
    # The load-bearing assertion: the result must be FILTERED by the typed prefix. Under
    # token_at's convention the cursor at this column lands on the '.' instead, which is not
    # an identifier and (being half-open at the right) never satisfies 'column == end' either
    # - so no anchor is set at all and the handler falls through to offering every keyword,
    # local and module symbol unfiltered. That is the failure this pin exists to catch.
    check("if" not in end_labels and "struct" not in end_labels,
          "completion at the end of 'shape' must be filtered by that prefix, not the "
          f"unfiltered keyword+symbol list, got {len(end_labels)} items")

    # --- completion inside a generic instantiation's argument list (LSPH-9) ---
    #
    # 'Bucket[i32]' and 'arr[idx]' are the same production to the parser, told apart in sema
    # once the identifier's declaration is known -- and completion has no parse at all, since
    # it tokenizes a buffer that is usually mid-edit. So the bracket's owner is found by
    # walking back over balanced brackets, and classified by what that identifier names.
    generic_items = sym_completion("mut b: Bucket[i32] = default", "i32")
    generic_labels = [it["label"] for it in generic_items]
    check("Square" in generic_labels and "Bucket" in generic_labels,
          f"completion inside 'Bucket[...]' offers declared type names, got {generic_labels}")
    check("i32" in generic_labels and "bool" in generic_labels,
          f"completion inside 'Bucket[...]' offers builtin type names, got {generic_labels}")
    check("double" not in generic_labels and "counter" not in generic_labels,
          f"completion inside 'Bucket[...]' offers no functions or locals, got {generic_labels}")

    # An ordinary index must be unaffected: 'arr' is not a generic type, so this is the
    # value-completion path exactly as before.
    index_items = sym_completion("return b.value + arr[idx]", "idx")
    index_labels = [it["label"] for it in index_items]
    check("idx" in index_labels,
          f"completion inside 'arr[...]' still offers locals, got {index_labels}")
    check("if" in index_labels,
          f"completion inside 'arr[...]' still offers keywords, got {index_labels}")

    client.notify("textDocument/didClose", {"textDocument": {"uri": sym_uri}})
    client.read_with_timeout(5)

    # --- inlay hints (and hover) inside uninstantiated generic bodies ---
    #
    # Sema type-checks every generic declaration's body once with its parameters bound to
    # Opaque, but records the results in that declaration's own template instance rather than
    # in the module's tables. The LSP read only the module's tables, so every expression in
    # every generic body missed and fell through to a fallback that binds each 'T' to a
    # placeholder 'u8'. The observable result was a hint that confidently lied (': u8' for a
    # value of type 'T') or, where the fallback also failed, no useful hint at all.
    gen_main = FIXTURES / "generic_body_fixture" / "main.mir"
    gen_uri = uri_for(gen_main)
    gen_text = gen_main.read_text()
    gen_lines = gen_text.splitlines()
    client.notify("textDocument/didOpen", {
        "textDocument": {"uri": gen_uri, "languageId": "mirage", "version": 1, "text": gen_text},
    })
    gen_diag = client.read()
    check(gen_diag["params"]["diagnostics"] == [], "generic-body fixture compiles with no diagnostics")

    def gen_line(anchor: str) -> int:
        """0-based line containing 'anchor'. Comment lines are skipped deliberately: the
        fixture documents the shapes it pins, and a comment quoting one verbatim would
        otherwise be matched instead of the code, silently turning every assertion below
        into 'no hint on a comment line'."""
        return next(i for i, line in enumerate(gen_lines)
                    if anchor in line and not line.lstrip().startswith("//"))

    def gen_hints() -> dict[int, list[str]]:
        """0-based line -> every inlay-hint label on it, for the whole file."""
        resp = client.request("textDocument/inlayHint", {
            "textDocument": {"uri": gen_uri},
            "range": {"start": {"line": 0, "character": 0},
                      "end": {"line": len(gen_lines), "character": 0}},
        })
        by_line: dict[int, list[str]] = {}
        for hint in resp["result"] or []:
            by_line.setdefault(hint["position"]["line"], []).append(hint["label"])
        return by_line

    def check_hint(hints: dict[int, list[str]], anchor: str, expected: str) -> None:
        got = hints.get(gen_line(anchor), [])
        check(expected in got, f"inlay hint on '{anchor}' is '{expected}', got {got}")

    hints = gen_hints()

    # A bare parameter typed 'T'. This is the one that used to read ': u8'.
    check_hint(hints, "mut n := value", ": T")
    # Both operands are locals the placeholder fallback never had in scope, and '%' routes
    # through opaque_binary_op_result - so this also pins the name propagating through an
    # operator rather than being dropped.
    check_hint(hints, "mut digit := n % base_t", ": T")
    check_hint(hints, "const base_t := cast(base, T)", ": T")
    # Concrete type, generic body: nothing here depends on 'T' at all, and the hint was
    # missing purely because the lookup never reached the template tables.
    check_hint(hints, "mut i := count", ": usize")

    # A generic METHOD: a different lookup key, filled in by a different sema pass.
    check_hint(hints, "mut span := self.length", ": usize")
    check_hint(hints, "mut doubled := span * 2", ": usize")
    check_hint(hints, "mut slot := self.items[self.head]", ": T")

    # The second of two type parameters, so a wrong answer cannot coincidentally match.
    check_hint(hints, "mut paired := v", ": V")

    # The control: ordinary non-generic code must be untouched by any of this.
    check_hint(hints, "mut z := 1 + 2", ": i32")

    # The ordering pin. The placeholder fallback used to run without pushing an
    # ExprSideTables of its own, so its 'u8'-flavoured results were written into the MODULE's
    # tables - permanently, for AST nodes shared by every instantiation. A hover was enough to
    # do it, and every later request for those nodes then read the poisoned entry back. Only
    # meaningful in this order: hover FIRST, then ask for the hints again.
    n_line = gen_line("mut n := value")
    hover_n = client.request("textDocument/hover", {
        "textDocument": {"uri": gen_uri},
        "position": {"line": n_line, "character": gen_lines[n_line].index("n :=")},
    })
    hover_text = (hover_n["result"] or {}).get("contents", {}).get("value", "")
    check(": T" in hover_text, f"hover on a 'T'-typed generic local reports 'T', got {hover_text!r}")

    check_hint(gen_hints(), "mut n := value", ": T")

    hover_i_line = gen_line("mut i := count")
    hover_i = client.request("textDocument/hover", {
        "textDocument": {"uri": gen_uri},
        "position": {"line": hover_i_line, "character": gen_lines[hover_i_line].index("i :=")},
    })
    hover_i_text = (hover_i["result"] or {}).get("contents", {}).get("value", "")
    check(": usize" in hover_i_text,
          f"hover on a concretely-typed local in a generic body reports 'usize', got {hover_i_text!r}")

    # Go-to-definition on a field access through 'self' inside a generic method - the same
    # module-tables-only lookup powered this, so it was broken in exactly the same way.
    head_line = gen_line("mut slot := self.items[self.head]")
    head_def = client.request("textDocument/definition", {
        "textDocument": {"uri": gen_uri},
        "position": {"line": head_line, "character": gen_lines[head_line].index("head]")},
    })
    head_result = head_def.get("result")
    check(bool(head_result),
          f"go-to-definition on 'self.head' inside a generic method resolves, got {head_result}")
    if head_result:
        target = head_result[0] if isinstance(head_result, list) else head_result
        check(target["range"]["start"]["line"] == gen_line("    head: usize = 0"),
              f"'self.head' resolves to the field declaration, got line {target['range']['start']['line'] + 1}")

    client.notify("textDocument/didClose", {"textDocument": {"uri": gen_uri}})
    client.read_with_timeout(5)

    # --- workspace-wide Find All References (DEFERRED §4) ---
    #
    # Needs its own session: the main one deliberately sends 'rootUri: None', which keeps the
    # search scoped to the analysed module's import closure. That is still the right
    # behaviour when a client supplies no root, and every reference assertion above depends
    # on it.
    #
    # The fixture is three sibling modules. 'lib' declares the target and imports nothing;
    # 'app' and 'unrelated' both import lib. Opening lib/main.mir analyses lib and its
    # imports — which is nothing — so NEITHER importer is in that closure. Finding their uses
    # is precisely what the workspace sweep adds.
    ws_root = FIXTURES / "workspace_fixture"
    ws_client = Client(LSP_BINARY)
    ws_client.request("initialize", {
        "processId": None,
        "rootUri": uri_for(ws_root),
        "capabilities": {},
    })
    ws_client.notify("initialized", {})

    ws_lib = ws_root / "lib" / "main.mir"
    ws_lib_uri = uri_for(ws_lib)
    ws_lib_text = ws_lib.read_text()
    ws_client.notify("textDocument/didOpen", {
        "textDocument": {"uri": ws_lib_uri, "languageId": "mirage", "version": 1, "text": ws_lib_text},
    })
    ws_client.read_with_timeout(5)

    ws_lines = ws_lib_text.splitlines()
    helper_line = next(i for i, l in enumerate(ws_lines) if "pub fn shared_helper" in l)
    ws_refs = ws_client.request("textDocument/references", {
        "textDocument": {"uri": ws_lib_uri},
        "position": {"line": helper_line, "character": ws_lines[helper_line].index("shared_helper")},
        "context": {"includeDeclaration": True},
    })["result"] or []

    ws_files = sorted({Path(r["uri"].removeprefix("file://")).parent.name for r in ws_refs})
    check(ws_files == ["app", "lib", "unrelated"],
          f"references on 'shared_helper' reach modules that the analysed closure never loads, got {ws_files}")
    # 'unrelated' uses it twice on one line; both must be reported, not just the first.
    unrelated_hits = [r for r in ws_refs if Path(r["uri"].removeprefix("file://")).parent.name == "unrelated"]
    check(len(unrelated_hits) == 2,
          f"both uses in the non-importing module are found, got {len(unrelated_hits)}")

    # A LOCAL is function-scoped, so the sweep must not run for it at all — the result stays
    # confined to the declaring file no matter how large the workspace is.
    app_main = ws_root / "app" / "main.mir"
    app_uri = uri_for(app_main)
    app_text = app_main.read_text()
    ws_client.notify("textDocument/didOpen", {
        "textDocument": {"uri": app_uri, "languageId": "mirage", "version": 1, "text": app_text},
    })
    ws_client.read_with_timeout(5)
    app_lines = app_text.splitlines()
    lib_alias_line = next(i for i, l in enumerate(app_lines) if "const lib := import" in l)
    alias_refs = ws_client.request("textDocument/references", {
        "textDocument": {"uri": app_uri},
        "position": {"line": lib_alias_line, "character": app_lines[lib_alias_line].index("lib")},
        "context": {"includeDeclaration": True},
    })["result"] or []
    alias_files = {Path(r["uri"].removeprefix("file://")).parent.name for r in alias_refs}
    check(alias_files <= {"app"},
          f"a module alias local to one file stays confined to it, got {sorted(alias_files)}")

    ws_exit = ws_client.close()
    check(ws_exit == 0, f"workspace session exits 0, got {ws_exit}")

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
