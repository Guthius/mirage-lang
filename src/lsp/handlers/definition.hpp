#pragma once

#include "../analysis.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace lsp::handlers {
    // Handles textDocument/definition. `line`/`column` are 1-based, matching
    // SourceLocation. Returns an LSP Location object, or JSON null if the
    // token isn't an identifier or nothing resolves.
    auto handle_definition(analysis::ProgramResult &result, const std::string &module_path,
                            const std::string &path, size_t line, size_t column) -> nlohmann::json;
}
