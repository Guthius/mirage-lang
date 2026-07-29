#include "source_manager.hpp"

#include "diagnostic_engine.hpp"

#include <fstream>
#include <sstream>

auto SourceManager::load(const std::string &canonical_path, DiagnosticEngine &diagnostics) -> SourceFile {
    if (const auto it = sources_.find(canonical_path); it != sources_.end()) {
        return {it->first, it->second};
    }

    std::ifstream file(canonical_path);
    if (!file.is_open()) {
        diagnostics.report_error(
            DiagnosticStage::Parser, {},
            std::format("cannot open module file '{}'", canonical_path));

        return {};
    }

    std::ostringstream buf;
    buf << file.rdbuf();

    auto [it, inserted] = sources_.emplace(canonical_path, buf.str());

    return {it->first, it->second};
}

auto SourceManager::set_source(const std::string &canonical_path, std::string text) -> SourceFile {
    auto [it, inserted] = sources_.insert_or_assign(canonical_path, std::move(text));

    return {it->first, it->second};
}

auto SourceManager::get_source_line(std::string_view filename, size_t line) -> std::string_view {
    if (line == 0) {
        return {};
    }

    auto it = sources_.find(std::string(filename));
    if (it == sources_.end()) {
        return {};
    }

    const auto &source = it->second;

    uint32_t current_line = 1;
    size_t line_start = 0;

    for (size_t i = 0; i < source.size(); ++i) {
        if (current_line == line) {
            line_start = i;
            while (i < source.size() && source[i] != '\n') {
                ++i;
            }

            // Drop the '\r' of a CRLF terminator. It is not part of the line's text, and
            // leaving it in inflates .size() by one, which skews the caret clamping in
            // DiagnosticEngine::print_diagnostic on every line of a CRLF file.
            auto end = i;
            if (end > line_start && source[end - 1] == '\r') {
                --end;
            }

            return std::string_view(source).substr(line_start, end - line_start);
        }

        if (source[i] == '\n') {
            ++current_line;
        }
    }

    return {};
}
