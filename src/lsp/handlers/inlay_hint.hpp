#pragma once

#include "../analysis.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace lsp::handlers {
    // Handles textDocument/inlayHint. `start_line`/`end_line` are 1-based (matching
    // SourceLocation) and inclusive, converted from the request's 0-based half-open range by
    // the caller. Returns an LSP InlayHint[] array (possibly empty), never null - an empty
    // range simply has no hints, which isn't an error.
    //
    // Two kinds of hint are produced:
    //   - Type hints, after a ':='-inferred variable's name (e.g. 'mut x := f()' -> 'x: i32 := f()').
    //   - Parameter-name hints, before each positional call argument whose callee resolves to a
    //     known function/method (e.g. 'foo(1, 2)' -> 'foo(a: 1, b: 2)') - skipped when the
    //     argument is itself a plain identifier already spelling the parameter's name, to match
    //     the noise-reduction convention other editors' inlay hints use.
    auto handle_inlay_hint(analysis::ProgramResult &result, const std::string &module_path,
                            const std::string &path, size_t start_line, size_t end_line) -> nlohmann::json;
}
