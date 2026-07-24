#pragma once

#include "compiler/module_resolver.hpp"
#include "compiler/sema.hpp"
#include "compiler/source_manager.hpp"

#include <cstdint>
#include <memory>
#include <set>
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
    auto analyse(const std::string &root_module_path,
                 const std::unordered_map<std::string, std::string> &open_buffers) -> ProgramResult;

    // One cached analysis bundle plus the bookkeeping needed to evict it: `last_access`
    // is a logical clock (see DocumentStore::access_clock_) bumped on every cache hit or
    // insert, used to find the least-recently-used bundle once the cache is over capacity.
    struct CachedBundle {
        ProgramResult result;
        uint64_t last_access = 0;
    };

    // Tracks open documents (by canonical filesystem path) and caches one
    // ProgramResult per module root directory, reusing a cached bundle for
    // any file that's a transitive import of an already-analysed root.
    //
    // Every method here is intended to run on a single thread (the analysis
    // worker thread - see Server): none of DocumentStore's internals are
    // synchronized, by design, since a mutex here would let the worker's
    // in-progress analysis block the main stdin-reading thread, reintroducing
    // the stall this split was meant to eliminate.
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

        // Given `closure_dirs` (the module directory set this analysis round's bundle
        // covers - i.e. its ast_program.modules) and the set of files this round found to
        // have non-empty diagnostics, returns every file that had non-empty diagnostics in
        // a *previous* round but isn't in that set now - i.e. files whose squiggles would
        // otherwise never be cleared because they're no longer part of any round's
        // touched-file set (group_diagnostics_by_file only produces an entry for a file
        // that actually has a diagnostic - a file whose only error was just fixed simply
        // has no entry at all, in a bundle that may itself have been fully recreated by
        // the edit that fixed it, since editing any file in a closure invalidates the
        // whole bundle). Also updates the tracked set for the next call.
        //
        // This tracking deliberately lives here (DocumentStore-level, surviving bundle
        // invalidation/recreation) rather than on ProgramResult itself: a fix made to file
        // B is often only discovered via re-analysing after an edit to file A in the same
        // closure, which throws away and rebuilds the whole bundle - tracking scoped to
        // the ephemeral bundle would reset to empty on exactly the edit that needs it.
        // `closure_dirs` is what keeps two unrelated module closures (e.g. two different
        // fixtures/projects touched in the same session) from cross-contaminating each
        // other's bookkeeping: only entries whose directory is in `closure_dirs` are ever
        // read or written by a given call.
        auto files_that_became_clean(const std::set<std::string> &closure_dirs,
                                      const std::set<std::string> &current_nonempty_files) -> std::vector<std::string>;

      private:
        void invalidate(const std::string &canonical_path);

        // Erases any cached bundle that isn't reachable from any file still open in
        // `open_texts_`. Handles the common case (closing every open file in a module
        // eventually frees its bundle); does NOT by itself bound cache size for a
        // session that navigates (e.g. go-to-definition) into many directories
        // without ever opening a buffer there - see evict_lru_if_over_capacity().
        void evict_unreferenced_bundles();

        // Backstop for the case sweep-on-close can't reach: navigating into many
        // module directories over a long session without ever opening a buffer in
        // them means nothing ever calls invalidate()/evict_unreferenced_bundles()
        // for those bundles. Caps module_results_ at MAX_CACHED_MODULES, evicting
        // the least-recently-accessed bundle when over capacity.
        void evict_lru_if_over_capacity();

        std::unordered_map<std::string, std::string> open_texts_;        // canonical file path -> buffer text
        std::unordered_map<std::string, CachedBundle> module_results_;   // module root dir -> last analysis
        std::set<std::string> last_published_nonempty_diag_files_;       // see files_that_became_clean()
        uint64_t access_clock_ = 0;
    };
}
