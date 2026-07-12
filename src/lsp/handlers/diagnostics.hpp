#pragma once

#include "compiler/diagnostic_engine.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace lsp::handlers {
    // Converts one compiler Diagnostic into an LSP Diagnostic JSON object:
    // 0-based line/character (SourceLocation is 1-based), severity (1=Error,
    // 2=Warning). Diagnostic carries no token/length, so there's no real end
    // position to recover - the range is widened to one character
    // (character+1) rather than left zero-width, so editors reliably render
    // a visible squiggle.
    auto to_lsp_diagnostic(const Diagnostic &diagnostic) -> nlohmann::json;

    // Groups `diagnostics` by location.filename into ready-to-publish LSP
    // Diagnostic arrays. Diagnostics with an empty filename (structural
    // errors with no attributable line, e.g. "cannot resolve import path")
    // have nothing to anchor a squiggle to and are dropped.
    auto group_diagnostics_by_file(const std::vector<Diagnostic> &diagnostics)
        -> std::unordered_map<std::string, std::vector<nlohmann::json>>;
}
