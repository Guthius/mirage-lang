#!/usr/bin/env python3
"""Position-encoding negotiation test for mirage-lsp (LSP-1).

The compiler counts byte columns; the LSP default wire encoding is UTF-16 code
units. This drives the same fixture through two sessions:

  1. a default client (no positionEncodings offered) — the server must convert
     incoming characters to byte columns and outgoing ones back to UTF-16;
  2. a client offering "utf-8" — the server must advertise
     positionEncoding "utf-8" and pass byte columns through untouched.

The fixture line `    width = len("héé") + width` puts two 2-byte codepoints
before the trailing `width`, so its byte column and UTF-16 column differ by 2.

Run manually after building:

    python3 tests/lsp_position_encoding_test.py
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from lsp_smoke_test import Client, LSP_BINARY, FIXTURES, uri_for, check
import lsp_smoke_test

FIXTURE = FIXTURES / "unicode_fixture" / "main.mir"

# 0-based positions in the fixture. Line 3 (index 2) is
#     width = len("héé") + width
# The trailing 'width' starts at codepoint/UTF-16 index 25 and byte index 27
# (each 'é' is one UTF-16 unit but two UTF-8 bytes).
USE_LINE = 2
USE_COL_UTF16 = 25
USE_COL_BYTES = 27
DECL_LINE = 1
DECL_COL = 8       # 'width' in '    mut width: usize = 3' — pure ASCII, same in both
DECL_STMT_COL = 4  # a local's declaration location is the statement start ('mut')


def open_fixture(client: Client) -> str:
    uri = uri_for(FIXTURE)
    client.notify("textDocument/didOpen", {
        "textDocument": {"uri": uri, "languageId": "mirage", "version": 1, "text": FIXTURE.read_text()},
    })
    diag = client.read()
    check(diag["method"] == "textDocument/publishDiagnostics", "didOpen publishes diagnostics")
    check(diag["params"]["diagnostics"] == [], "unicode fixture is clean")
    return uri


def run_session(label: str, offered: dict, use_col: int, expect_utf8: bool) -> None:
    line3 = FIXTURE.read_text().splitlines()[USE_LINE]
    assert line3.index("width", 10) == USE_COL_UTF16
    assert len(line3[:USE_COL_UTF16].encode()) == USE_COL_BYTES

    client = Client(LSP_BINARY)
    init = client.request("initialize", {"processId": None, "rootUri": None, "capabilities": offered})
    caps = init["result"]["capabilities"]
    if expect_utf8:
        check(caps.get("positionEncoding") == "utf-8",
              f"{label}: initialize advertises positionEncoding utf-8, got {caps.get('positionEncoding')}")
    else:
        check("positionEncoding" not in caps,
              f"{label}: no positionEncoding advertised (spec default utf-16), got {caps.get('positionEncoding')}")
    client.notify("initialized", {})
    uri = open_fixture(client)

    # Incoming conversion: the cursor on the trailing 'width' must resolve to the
    # local's declaration. With the wrong encoding the cursor lands inside ' + '
    # and resolves nothing.
    resp = client.request("textDocument/definition", {
        "textDocument": {"uri": uri}, "position": {"line": USE_LINE, "character": use_col},
    })
    result = resp["result"]
    check(result is not None, f"{label}: definition after the non-ASCII string resolves")
    if result:
        check(result["range"]["start"]["line"] == DECL_LINE and result["range"]["start"]["character"] == DECL_STMT_COL,
              f"{label}: definition points at the 'width' declaration, got {result['range']['start']}")

    hover = client.request("textDocument/hover", {
        "textDocument": {"uri": uri}, "position": {"line": USE_LINE, "character": use_col},
    })
    check(hover["result"] is not None, f"{label}: hover after the non-ASCII string resolves")

    # Outgoing conversion: references from the declaration must report the
    # trailing use at the client's column for it.
    refs = client.request("textDocument/references", {
        "textDocument": {"uri": uri},
        "position": {"line": DECL_LINE, "character": DECL_COL},
        "context": {"includeDeclaration": True},
    })
    locations = refs["result"] or []
    starts = [(loc["range"]["start"]["line"], loc["range"]["start"]["character"]) for loc in locations]
    check((USE_LINE, use_col) in starts,
          f"{label}: references report the use after the non-ASCII string at character {use_col}, got {starts}")

    client.close()


def main() -> int:
    if not LSP_BINARY.exists():
        print(f"FAIL: {LSP_BINARY} not found - run `just build` first")
        return 1

    run_session("utf-16 client", {}, USE_COL_UTF16, expect_utf8=False)
    run_session("utf-8 client", {"general": {"positionEncodings": ["utf-8", "utf-16"]}},
                USE_COL_BYTES, expect_utf8=True)

    if lsp_smoke_test.failures:
        print(f"\n{lsp_smoke_test.failures} failure(s)")
        return 1
    print("\nall position-encoding checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
