#pragma once

#include "compiler/diagnostic_engine.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace lsp::handlers {
    // Converts one compiler Diagnostic into an LSP Diagnostic JSON object:
    // 0-based line/character (SourceLocation is 1-based), severity (1=Error,
    // 2=Warning). The range end is start + SourceLocation::length (the
    // originating token's lexeme length, at minimum 1), so the squiggle
    // spans the whole token rather than a single character.
    auto to_lsp_diagnostic(const Diagnostic &diagnostic) -> nlohmann::json;

    // Groups `diagnostics` by location.filename into ready-to-publish LSP
    // Diagnostic arrays. Diagnostics with an empty filename (structural
    // errors with no attributable line, e.g. "cannot resolve import path")
    // have nothing to anchor a squiggle to and are dropped.
    auto group_diagnostics_by_file(const std::vector<Diagnostic> &diagnostics)
        -> std::unordered_map<std::string, std::vector<nlohmann::json>>;
}
