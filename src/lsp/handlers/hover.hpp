#pragma once

#include "../analysis.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace lsp::handlers {
    // Handles textDocument/hover. `line`/`column` are 1-based, matching
    // SourceLocation. Returns an LSP Hover object, or JSON null if there's
    // nothing useful to say (keywords, punctuation, unresolved identifiers).
    auto handle_hover(analysis::ProgramResult &result, const std::string &module_path,
                       const std::string &path, size_t line, size_t column) -> nlohmann::json;
}
