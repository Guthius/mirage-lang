#pragma once

#include "compiler/module_resolver.hpp"
#include "compiler/sema.hpp"
#include "compiler/source_manager.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace lsp::analysis {
    // The result of running the compiler pipeline once for a module root.
    // `source_manager` owns all backing source text; `ast_program` (locations
    // are string_views into `source_manager`) and `sema_program` (expr_types
    // is keyed by ast_program's node addresses) both point into it and into
    // each other, so all three must always be replaced together as one unit,
    // never mutated independently once returned.
    struct ProgramResult {
        std::unique_ptr<SourceManager> source_manager;
        ast::Program ast_program;
        sema::Program sema_program;
        std::vector<Diagnostic> diagnostics;
    };

    // Runs lex -> parse -> resolve imports -> sema on the module directory at
    // `root_module_path`. `open_buffers` maps canonical file paths to their
    // current (possibly unsaved) editor contents; every one of them is seeded
    // into the fresh SourceManager before resolving, so files open in the
    // editor are analysed using live buffer text rather than what's on disk.
    //
    // Sema is run even if lexing/parsing reported errors (see analysis.cpp)
    // so hover/definition/diagnostics stay useful on a broken-but-partial
    // file, matching the rest of the compiler's "partial results are better
    // than none" tolerance for LSP use.
    //
    // Known limitation (not fixed here - see plan): module_resolver aborts
    // loading further files in a directory the moment any error has been
    // recorded anywhere in the whole resolve() call, so a syntax error in one
    // file of a multi-file module can cause sibling files in that directory
    // to be skipped, producing spurious "undefined symbol" diagnostics for
    // things those siblings define.
    auto analyse(const std::string &root_module_path,
                 const std::unordered_map<std::string, std::string> &open_buffers) -> ProgramResult;

    // Tracks open documents (by canonical filesystem path) and caches one
    // ProgramResult per module root directory, reusing a cached bundle for
    // any file that's a transitive import of an already-analysed root.
    class DocumentStore {
      public:
        void open(const std::string &canonical_path, std::string text);
        void update(const std::string &canonical_path, std::string text);
        void close(const std::string &canonical_path);

        [[nodiscard]] auto text_of(const std::string &canonical_path) const -> const std::string *;

        // Returns the cached (or freshly computed) ProgramResult that covers
        // `canonical_path`'s containing module directory. Non-const: hover's
        // local-variable-type resolution needs to call sema::resolve_type()
        // (which takes a mutable sema::Program&) against the cached bundle.
        auto ensure_analysed(const std::string &canonical_path) -> ProgramResult &;

      private:
        void invalidate(const std::string &canonical_path);

        std::unordered_map<std::string, std::string> open_texts_;      // canonical file path -> buffer text
        std::unordered_map<std::string, ProgramResult> module_results_; // module root dir -> last analysis
    };
}
