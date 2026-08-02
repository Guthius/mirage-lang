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

    // Where an import was found, for '--print-module-search'. One record per resolved
    // import edge, in resolution order.
    struct ModuleSearchRecord {
        std::string importer;      // canonical directory of the module doing the import
        std::string import_path;   // the string as written in 'import("...")'
        std::string resolved_path; // canonical directory it resolved to
        std::string root_label;    // which search root satisfied it (see SearchRoots)
    };

    struct Program {
        std::unordered_map<std::string, Module> modules;
        std::unordered_map<std::string, std::unordered_map<std::string, std::string>> module_imports;
        std::string root_module_path;
        std::vector<ModuleSearchRecord> module_search_trace;
        size_t file_count = 0;
        size_t token_count = 0;
        bool ok = false;
    };

    // The five roots an 'import("...")' path is tried against, in this order. Root 1 (the
    // importing module's own directory) is not stored here — it varies per import and is
    // passed separately.
    //
    // Every root below is checked for CONTAINMENT: a path that canonicalizes outside its
    // own root is rejected rather than accepted. Root 1 deliberately is not, because
    // upward '..' traversal is how the corpus's sibling-module imports work
    // ('import("../../runtime/type_info")', 'import("..")'). That asymmetry is a language
    // decision, not an oversight — see resolve_import_path.
    struct SearchRoots {
        std::string root_module;       // 2. directory of the root module being compiled
        std::string cwd;               // 3. current working directory
        std::string compiler_dir;      // 4a. directory holding the 'mirage' executable
        std::string compiler_lib_dir;  // 4b. <compiler_dir>/../lib/mirage, for bin+lib installs
        std::string modules_root;      // 5. '--std=' if given, else $MIRAGE_MODULES_ROOT

        // $MIRAGE_PATH was the pre-2026-08 spelling of root 5 and is no longer consulted at
        // all. Recorded only so a failed resolution can point out that the variable someone
        // still has exported is being ignored — a note on an error, never a fallback.
        bool legacy_mirage_path_set = false;
    };

    // Options the driver supplies to resolve(). Every field may be empty: an embedder that
    // supplies nothing (the LSP's default construction) still gets roots 1, 2, 3 and the
    // $MIRAGE_MODULES_ROOT half of root 5.
    struct ResolveOptions {
        std::string std_path_override; // '--std=<path>'; takes precedence over $MIRAGE_MODULES_ROOT
        std::string compiler_dir;      // directory of the running executable; see executable_directory()
    };

    auto canonicalize(const std::string &path) -> std::string;

    // Directory holding the currently-running executable, or an empty string if it cannot
    // be determined. Reads /proc/self/exe, falling back to resolving 'argv0' against $PATH
    // when it contains no separator.
    //
    // Lives here so 'mirage' and 'mirage-lsp' share one implementation, but is NOT called
    // from resolve(): which directory counts as "the compiler" is the caller's fact, and
    // module resolution stays free of process introspection.
    auto executable_directory(const char *argv0) -> std::string;

    auto resolve(const std::string &root_module_path, SourceManager &source_manager, DiagnosticEngine &diagnostics,
                 const ResolveOptions &options = {}) -> Program;

    // Resolves 'relative_path' against 'base_dir', rejecting absolute paths and any path
    // that escapes 'base_dir' (e.g. via '..'). Returns the canonical absolute path on success,
    // or an empty string on failure. Does not check whether the path actually exists.
    auto resolve_contained_path(const std::string &base_dir, const std::string &relative_path) -> std::string;
}
