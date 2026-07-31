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

    auto discover_module_dirs(const std::string &workspace_root) -> std::vector<std::string> {
        std::vector<std::string> dirs;
        if (workspace_root.empty()) return dirs;

        std::error_code ec;
        auto it = std::filesystem::recursive_directory_iterator(
            workspace_root, std::filesystem::directory_options::skip_permission_denied, ec);
        if (ec) return dirs;

        std::set<std::string> seen;
        for (const auto end = std::filesystem::recursive_directory_iterator{}; it != end; it.increment(ec)) {
            if (ec) break;
            const auto &entry = *it;
            if (entry.is_directory(ec)) {
                const auto name = entry.path().filename().string();
                // Build trees and VCS/tooling directories hold no source worth searching, and
                // a build tree in particular can be enormous.
                if (name.starts_with(".") || name.starts_with("build") || name == "node_modules") {
                    it.disable_recursion_pending();
                }
                continue;
            }
            if (entry.path().extension() != ".mir") continue;
            seen.insert(ast::canonicalize(entry.path().parent_path().string()));
        }

        dirs.assign(seen.begin(), seen.end());
        return dirs;
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

    void DocumentStore::index_bundle(const std::string &key) {
        const auto it = module_results_.find(key);
        if (it == module_results_.end()) return;
        for (const auto &dir : it->second.result.ast_program.modules | std::views::keys) {
            bundle_of_dir_[dir].insert(key);
        }
    }

    void DocumentStore::unindex_bundle(const std::string &key) {
        const auto it = module_results_.find(key);
        if (it == module_results_.end()) return;
        for (const auto &dir : it->second.result.ast_program.modules | std::views::keys) {
            const auto entry = bundle_of_dir_.find(dir);
            if (entry == bundle_of_dir_.end()) continue;
            entry->second.erase(key);
            if (entry->second.empty()) bundle_of_dir_.erase(entry);
        }
    }

    void DocumentStore::invalidate(const std::string &canonical_path) {
        const auto dir = std::filesystem::path(canonical_path).parent_path().string();

        const auto entry = bundle_of_dir_.find(dir);
        if (entry == bundle_of_dir_.end()) return;

        // Copied: unindex_bundle mutates bundle_of_dir_, including this very entry.
        const auto keys = entry->second;
        for (const auto &key : keys) {
            unindex_bundle(key);
            module_results_.erase(key);
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
                unindex_bundle(it->first);
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
        unindex_bundle(oldest->first);
        module_results_.erase(oldest);
    }

    auto DocumentStore::analyse_uncached(const std::string &dir) const -> ProgramResult {
        return analyse(dir, open_texts_);
    }

    auto DocumentStore::ensure_analysed(const std::string &canonical_path) -> ProgramResult & {
        const auto dir = ast::canonicalize(std::filesystem::path(canonical_path).parent_path().string());

        // Any bundle whose closure already contains this directory will do — they analysed
        // the same sources. The old linear scan likewise took whichever came first in
        // unordered iteration order.
        if (const auto entry = bundle_of_dir_.find(dir); entry != bundle_of_dir_.end() && !entry->second.empty()) {
            auto &bundle = module_results_.at(*entry->second.begin());
            bundle.last_access = ++access_clock_;
            return bundle.result;
        }

        // insert_or_assign may replace an existing bundle under this key, so drop its index
        // entries first.
        unindex_bundle(dir);
        auto [it, inserted] = module_results_.insert_or_assign(
            dir, CachedBundle{.result = analyse(dir, open_texts_), .last_access = ++access_clock_});
        index_bundle(dir);
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
