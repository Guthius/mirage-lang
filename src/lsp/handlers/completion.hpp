#pragma once

#include "../analysis.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace lsp::handlers {
    // Computes textDocument/completion candidates for 1-based (line, column) in `path`, a
    // file within `module_path`'s module directory: keywords, locals/params in scope,
    // module-level symbols, and (when the cursor sits right after a '.') the receiver's
    // struct/union/enum fields, tagged-union variants, and methods (including trait methods).
    auto handle_completion(analysis::ProgramResult &result, const std::string &module_path,
                            const std::string &path, size_t line, size_t column) -> nlohmann::json;
}
