#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace lsp::encoding {
    // Column conversions between the two position encodings in play: the compiler
    // counts BYTE columns (the lexer bumps col_ once per byte), while LSP's default
    // wire encoding is UTF-16 code units. Both offsets are 0-based within one line
    // of text. An offset past the end of the line carries over 1:1, so synthesized
    // end-of-range positions (diagnostic start+length) and a failed line lookup
    // (empty text) both degrade to pass-through, never to a clamp at column 0.
    auto utf16_to_byte_column(std::string_view line, size_t utf16_col) -> size_t;
    auto byte_to_utf16_column(std::string_view line, size_t byte_col) -> size_t;

    // Produces the text of 1-based `line` in `file` for the conversions above.
    // Returning "" means the line cannot be produced and positions pass through
    // unchanged.
    using LineLookup = std::function<std::string(const std::string &file, size_t line)>;

    // Rewrites every {"line","character"} position object in `value` (a handler's
    // outgoing result) from byte columns to UTF-16 code units. Positions nested
    // under an object carrying a sibling "uri" (the Location shape) are resolved
    // against THAT uri's document; everything else against `default_file`.
    void positions_to_utf16(nlohmann::json &value, const std::string &default_file, const LineLookup &line_of);

    // The 1-based line's text out of a whole buffer, without its terminator and
    // without the '\r' of a CRLF pair (mirrors SourceManager::get_source_line).
    auto line_from_text(std::string_view text, size_t line) -> std::string_view;
}
