#pragma once

#include "../uri.hpp"
#include "compiler/source_location.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>

// The one place LSP position/range/Location JSON is built — definition,
// references, inlay hints and diagnostics all emit through here instead of
// re-implementing the 1-based-to-0-based conversion each. Internal lines and
// columns are 1-based byte positions; byte columns are converted to the
// negotiated wire encoding at the server boundary (see position_encoding.hpp).
namespace lsp::handlers {
    inline auto to_zero_based(const size_t one_based) -> size_t {
        return one_based == 0 ? 0 : one_based - 1;
    }

    inline auto position_json(const size_t line, const size_t column) -> nlohmann::json {
        return {{"line", to_zero_based(line)}, {"character", to_zero_based(column)}};
    }

    // A single-line range spanning `length` bytes — minimum 1, so clients always
    // have something to highlight rather than a zero-width caret.
    inline auto range_json(const size_t line, const size_t column, const size_t length) -> nlohmann::json {
        const auto start_character = to_zero_based(column);
        return {
            {"start", {{"line", to_zero_based(line)}, {"character", start_character}}},
            {"end", {{"line", to_zero_based(line)}, {"character", start_character + std::max<size_t>(length, 1)}}},
        };
    }

    // A Location covering the token at `loc`, using its recorded length. Null for
    // a location with no file (synthesized/builtin).
    inline auto location_json(const SourceLocation &loc) -> nlohmann::json {
        if (loc.filename.empty()) return nullptr;
        return {
            {"uri", path_to_uri(std::string(loc.filename))},
            {"range", range_json(loc.line, loc.column, loc.length)},
        };
    }
}
