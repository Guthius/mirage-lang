#pragma once

#include "ast.hpp"
#include "diagnostic_engine.hpp"

#include <ranges>
#include <unordered_map>

namespace ast {
    // One parsed '.mir' file. A module is a directory of these; the resolver keeps the
    // per-file structure (rather than merging every file's declarations into one flat
    // list) because '#compile_only_if' gates inclusion per FILE — sema needs to know
    // which declarations came from which file to exclude a file's symbols while still
    // type-checking its contents.
    struct FileAST {
        std::string file_path;                // canonical path, same string the file's token locations carry
        std::vector<Decl> declarations;
        SourceLocation location;              // start of the file (line 1, column 1)
    };

    using Module = std::vector<FileAST>;

    // Flat view over every declaration in a module, in file order (files are sorted by
    // path; declarations keep source order). For consumers that don't care about file
    // boundaries — symbol walks, searches — so the per-file split doesn't force nested
    // loops everywhere.
    inline auto all_decls(const Module &module) {
        return module | std::views::transform([](const FileAST &f) -> const std::vector<Decl> & { return f.declarations; }) |
               std::views::join;
    }

    struct Program {
        std::unordered_map<std::string, Module> modules;
        std::unordered_map<std::string, std::unordered_map<std::string, std::string>> module_imports;
        std::string root_module_path;
        size_t file_count = 0;
        size_t token_count = 0;
        bool ok = false;
    };

    auto canonicalize(const std::string &path) -> std::string;
    auto resolve(const std::string &root_module_path, SourceManager &source_manager, DiagnosticEngine &diagnostics,
                 const std::string &std_path_override = {}) -> Program;

    // Resolves 'relative_path' against 'base_dir', rejecting absolute paths and any path
    // that escapes 'base_dir' (e.g. via '..'). Returns the canonical absolute path on success,
    // or an empty string on failure. Does not check whether the path actually exists.
    auto resolve_contained_path(const std::string &base_dir, const std::string &relative_path) -> std::string;
}
