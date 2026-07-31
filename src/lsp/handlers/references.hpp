#pragma once

#include "../analysis.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <vector>

namespace lsp::handlers {
    // How to reach modules the analysed import closure does not contain.
    //
    // Passing this is what makes the search workspace-wide; omitting it (nullptr) keeps the
    // import-closure-only behaviour, which is what happens when the client sent no workspace
    // root to search.
    struct WorkspaceSearch {
        std::vector<std::string> module_dirs;
        // Analyses one module directory into its OWN ProgramResult. Deliberately a callback:
        // this must not go through the LRU document cache, or a single search could evict
        // every bundle the user actually has open (see DocumentStore::analyse_uncached).
        std::function<analysis::ProgramResult(const std::string &)> analyse;
    };

    // Computes textDocument/references for 1-based (line, column) in `path`: resolves the
    // symbol under the cursor (the same resolve_at() hover and definition use), then walks
    // every module looking for occurrences that resolve to the same declaration.
    // `include_declaration` optionally adds the declaration's own site.
    //
    // With `workspace` non-null the search covers every module in it, not just the analysed
    // module's import closure — so a reference from a file that does not import the target is
    // found. Locals and params are function-scoped and are never searched beyond the closure.
    auto handle_references(analysis::ProgramResult &result, const std::string &module_path,
                            const std::string &path, size_t line, size_t column,
                            bool include_declaration,
                            const WorkspaceSearch *workspace = nullptr) -> nlohmann::json;
}
