#pragma once

#include "../analysis.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace lsp::handlers {
    // Computes textDocument/references for 1-based (line, column) in `path`: resolves the
    // symbol under the cursor (same resolve_at() used by hover/definition), then walks every
    // module in the loaded import closure (result.ast_program.modules - NOT a workspace-wide
    // scan) looking for identifier/member-access occurrences that resolve to the same
    // declaration. `include_declaration` optionally adds the declaration's own site.
    auto handle_references(analysis::ProgramResult &result, const std::string &module_path,
                            const std::string &path, size_t line, size_t column,
                            bool include_declaration) -> nlohmann::json;
}
