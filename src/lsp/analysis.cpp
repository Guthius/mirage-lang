#include "analysis.hpp"

#include <algorithm>
#include <filesystem>
#include <ranges>

namespace lsp::analysis {
    auto analyse(const std::string &root_module_path,
                 const std::unordered_map<std::string, std::string> &open_buffers) -> ProgramResult {
        auto source_manager = std::make_unique<SourceManager>();
        DiagnosticEngine diag(*source_manager);

        for (const auto &[path, text] : open_buffers) {
            source_manager->set_source(path, text);
        }

        auto ast_program = ast::resolve(root_module_path, *source_manager, diag);

        // sema::check_program() short-circuits when ast_program.ok is false.
        // We want hover/definition/diagnostics to keep working on whatever
        // partial AST got produced even when lexing/parsing hit an error, so
        // force it to run - mutating ast_program in place (not a copy) is
        // required here: sema captures pointers into ast_program's Decl/Expr
        // nodes (expr_types keys, Symbol::decl), and those would dangle if we
        // fed sema a temporary copy instead of the object we're about to
        // return.
        // LSPCORE-13 asks whether every parser error-recovery path leaves a tree structurally
        // well-formed enough for sema to walk without crashing, since forcing ok=true here
        // means sema runs on input it would normally refuse. Probed with 100 mutation trials
        // over four sources chosen for sema complexity (traits, generics, error typestate,
        // trait composition), each randomly corrupted with 1-8 edits and then driven through
        // didOpen plus several hovers: no crashes, hangs or non-zero exits. Not a proof, but
        // the assumption held everywhere it was pushed. Note this became far less dangerous
        // once the worker thread got its own try/catch (LSPCORE-1): an exception escaping sema
        // now fails one request instead of calling std::terminate.
        ast_program.ok = true;
        auto sema_program = sema::check_program(ast_program, diag);

        return ProgramResult{
            .source_manager = std::move(source_manager),
            .ast_program = std::move(ast_program),
            .sema_program = std::move(sema_program),
            .diagnostics = diag.diagnostics(),
        };
    }

    namespace {
        constexpr size_t MAX_CACHED_MODULES = 32;
    }

    void DocumentStore::open(const std::string &canonical_path, std::string text) {
        open_texts_[canonical_path] = std::move(text);
        invalidate(canonical_path);
    }

    void DocumentStore::update(const std::string &canonical_path, std::string text) {
        open_texts_[canonical_path] = std::move(text);
        invalidate(canonical_path);
    }

    void DocumentStore::close(const std::string &canonical_path) {
        open_texts_.erase(canonical_path);
        invalidate(canonical_path);
        evict_unreferenced_bundles();
    }

    void DocumentStore::invalidate_external(const std::string &canonical_path) {
        // Deliberately does not touch open_texts_. An externally changed file may have no
        // buffer at all; and where it does, the buffer is still what the editor believes the
        // file to be, so overwriting or dropping it here would fight the client. Only the
        // cached ANALYSIS is stale, and invalidate() already evicts by module directory,
        // which is exactly the granularity a dependency change needs.
        invalidate(canonical_path);
    }

    auto DocumentStore::open_paths() const -> std::vector<std::string> {
        std::vector<std::string> paths;
        paths.reserve(open_texts_.size());
        for (const auto &path : open_texts_ | std::views::keys) paths.push_back(path);
        return paths;
    }

    auto DocumentStore::text_of(const std::string &canonical_path) const -> const std::string * {
        const auto it = open_texts_.find(canonical_path);
        return it == open_texts_.end() ? nullptr : &it->second;
    }

    void DocumentStore::invalidate(const std::string &canonical_path) {
        const auto dir = std::filesystem::path(canonical_path).parent_path().string();

        for (auto it = module_results_.begin(); it != module_results_.end();) {
            if (it->second.result.ast_program.modules.contains(dir)) {
                it = module_results_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void DocumentStore::evict_unreferenced_bundles() {
        std::set<std::string> open_dirs;
        for (const auto &path : open_texts_ | std::views::keys) {
            open_dirs.insert(std::filesystem::path(path).parent_path().string());
        }

        for (auto it = module_results_.begin(); it != module_results_.end();) {
            const auto &modules = it->second.result.ast_program.modules;
            const bool still_referenced =
                std::ranges::any_of(open_dirs, [&](const auto &dir) { return modules.contains(dir); });

            if (!still_referenced) {
                it = module_results_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void DocumentStore::evict_lru_if_over_capacity() {
        if (module_results_.size() <= MAX_CACHED_MODULES) {
            return;
        }

        auto oldest = module_results_.begin();
        for (auto it = module_results_.begin(); it != module_results_.end(); ++it) {
            if (it->second.last_access < oldest->second.last_access) {
                oldest = it;
            }
        }
        module_results_.erase(oldest);
    }

    auto DocumentStore::ensure_analysed(const std::string &canonical_path) -> ProgramResult & {
        const auto dir = ast::canonicalize(std::filesystem::path(canonical_path).parent_path().string());

        for (auto &bundle : module_results_ | std::views::values) {
            if (bundle.result.ast_program.modules.contains(dir)) {
                bundle.last_access = ++access_clock_;
                return bundle.result;
            }
        }

        auto [it, inserted] = module_results_.insert_or_assign(
            dir, CachedBundle{.result = analyse(dir, open_texts_), .last_access = ++access_clock_});
        evict_lru_if_over_capacity();
        return it->second.result;
    }

    auto DocumentStore::files_that_became_clean(const std::set<std::string> &closure_dirs,
                                                 const std::set<std::string> &current_nonempty_files)
        -> std::vector<std::string> {
        std::vector<std::string> became_clean;

        for (auto it = last_published_nonempty_diag_files_.begin(); it != last_published_nonempty_diag_files_.end();) {
            const auto dir = std::filesystem::path(*it).parent_path().string();
            if (!closure_dirs.contains(dir)) {
                ++it; // not part of this closure - leave untouched, some other bundle owns it
                continue;
            }

            if (current_nonempty_files.contains(*it)) {
                ++it;
            } else {
                became_clean.push_back(*it);
                it = last_published_nonempty_diag_files_.erase(it);
            }
        }

        for (const auto &file : current_nonempty_files) {
            last_published_nonempty_diag_files_.insert(file);
        }

        return became_clean;
    }
}
