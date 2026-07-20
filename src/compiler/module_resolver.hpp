#pragma once

#include "ast.hpp"
#include "diagnostic_engine.hpp"

#include <unordered_map>

namespace ast {
    using Module = std::vector<Decl>;

    struct Program {
        std::unordered_map<std::string, Module> modules;
        std::unordered_map<std::string, std::unordered_map<std::string, std::string>> module_imports;
        std::string root_module_path;
        size_t file_count = 0;
        size_t token_count = 0;
        bool ok = false;
    };

    auto canonicalize(const std::string &path) -> std::string;
    auto resolve(const std::string &root_module_path, SourceManager &source_manager, DiagnosticEngine &diagnostics) -> Program;

    // Resolves 'relative_path' against 'base_dir', rejecting absolute paths and any path
    // that escapes 'base_dir' (e.g. via '..'). Returns the canonical absolute path on success,
    // or an empty string on failure. Does not check whether the path actually exists.
    auto resolve_contained_path(const std::string &base_dir, const std::string &relative_path) -> std::string;
}
