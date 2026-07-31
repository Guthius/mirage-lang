#include "position_encoding.hpp"

#include "uri.hpp"

namespace lsp::encoding {
    namespace {
        // One UTF-8 sequence: its byte length and how many UTF-16 code units it
        // decodes to (astral-plane codepoints take a surrogate pair). A malformed
        // lead byte counts as one byte / one unit so broken input degrades to
        // pass-through instead of desyncing the walk.
        struct Utf8Step {
            size_t bytes;
            size_t utf16_units;
        };

        auto step_at(const unsigned char c) -> Utf8Step {
            if (c < 0x80) return {1, 1};
            if ((c & 0xE0) == 0xC0) return {2, 1};
            if ((c & 0xF0) == 0xE0) return {3, 1};
            if ((c & 0xF8) == 0xF0) return {4, 2};
            return {1, 1};
        }
    }

    auto utf16_to_byte_column(const std::string_view line, const size_t utf16_col) -> size_t {
        size_t units = 0;
        size_t i = 0;
        while (i < line.size() && units < utf16_col) {
            const auto s = step_at(static_cast<unsigned char>(line[i]));
            units += s.utf16_units;
            i += s.bytes;
        }
        if (i > line.size()) i = line.size(); // truncated trailing sequence
        return utf16_col > units ? i + (utf16_col - units) : i;
    }

    auto byte_to_utf16_column(const std::string_view line, const size_t byte_col) -> size_t {
        size_t units = 0;
        size_t i = 0;
        while (i < line.size() && i < byte_col) {
            const auto s = step_at(static_cast<unsigned char>(line[i]));
            units += s.utf16_units;
            i += s.bytes;
        }
        return byte_col > i ? units + (byte_col - i) : units;
    }

    namespace {
        void walk(nlohmann::json &value, const std::string &file, const LineLookup &line_of) {
            if (value.is_array()) {
                for (auto &item : value) walk(item, file, line_of);
                return;
            }
            if (!value.is_object()) return;

            // A "uri" sibling (the Location shape) rebinds every position in this
            // subtree to that document; workspace-wide reference results point into
            // files other than the requested one.
            std::string here = file;
            if (const auto it = value.find("uri"); it != value.end() && it->is_string()) {
                if (auto path = uri_to_path(it->get<std::string>()); !path.empty()) here = std::move(path);
            }

            const auto line_it = value.find("line");
            const auto char_it = value.find("character");
            if (line_it != value.end() && char_it != value.end() &&
                line_it->is_number_integer() && char_it->is_number_integer()) {
                // Wire positions are 0-based; the lookup takes a 1-based line.
                const auto line = line_it->get<size_t>();
                *char_it = byte_to_utf16_column(line_of(here, line + 1), char_it->get<size_t>());
                return;
            }

            for (auto &item : value.items()) {
                walk(item.value(), here, line_of);
            }
        }
    }

    void positions_to_utf16(nlohmann::json &value, const std::string &default_file, const LineLookup &line_of) {
        walk(value, default_file, line_of);
    }

    auto line_from_text(const std::string_view text, const size_t line) -> std::string_view {
        if (line == 0) return {};
        size_t current = 1;
        size_t start = 0;
        for (size_t i = 0; i <= text.size(); ++i) {
            if (i == text.size() || text[i] == '\n') {
                if (current == line) {
                    auto end = i;
                    if (end > start && text[end - 1] == '\r') --end;
                    return text.substr(start, end - start);
                }
                if (i == text.size()) break;
                ++current;
                start = i + 1;
            }
        }
        return {};
    }
}
