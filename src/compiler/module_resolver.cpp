#include "module_resolver.hpp"

#include "lexer.hpp"
#include "source_manager.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>

namespace ast {
    namespace {
        auto load_and_parse(const std::string &canonical_path, SourceManager &source_manager, DiagnosticEngine &diagnostics, Program &program) -> Module {
            std::error_code ec;
            std::filesystem::directory_iterator dir(canonical_path, ec);
            if (ec) {
                diagnostics.report_error(
                    DiagnosticStage::Parser, {},
                    std::format("cannot read module directory '{}'", canonical_path));
                return {};
            }

            std::vector<std::filesystem::path> files;
            for (const auto &entry : dir) {
                if (entry.is_regular_file() && entry.path().extension() == ".mir") {
                    files.push_back(entry.path());
                }
            }
            std::sort(files.begin(), files.end());

            Module combined;
            for (const auto &file : files) {
                const auto source_file = source_manager.load(file.string(), diagnostics);
                if (source_file.text.empty()) continue;
                const auto errors_before = diagnostics.error_count();
                auto tokens = lexer::tokenize(source_file.text, source_file.filename, diagnostics);
                if (diagnostics.error_count() > errors_before) continue;
                program.file_count += 1;
                program.token_count += tokens.size();
                auto decls = parse(tokens, diagnostics);
                combined.insert(combined.end(), std::make_move_iterator(decls.begin()), std::make_move_iterator(decls.end()));
            }
            return combined;
        }

        auto find_import_strings(const Module &module) -> std::vector<std::pair<std::string, SourceLocation>> {
            std::vector<std::pair<std::string, SourceLocation>> found;

            for (auto &decl : module) {
                if (auto *var_decl = std::get_if<VarDecl>(&decl); var_decl && var_decl->init) {
                    if (auto *import_stmt = std::get_if<ImportExpr>(&*var_decl->init)) {
                        found.emplace_back(import_stmt->module_name, import_stmt->location);
                    }
                }
            }

            return found;
        }

        auto is_contained_in(const std::filesystem::path &base, const std::filesystem::path &candidate) -> bool {
            auto base_it = base.begin();
            auto cand_it = candidate.begin();
            for (; base_it != base.end(); ++base_it, ++cand_it) {
                if (cand_it == candidate.end() || *cand_it != *base_it) {
                    return false;
                }
            }
            return true;
        }

        auto resolve_import_path(const std::string &importer_path, const std::string &import_path, const std::string &mirage_path) -> std::string {
            auto candidate = std::filesystem::path(importer_path) / import_path;
            if (std::filesystem::is_directory(candidate)) {
                return canonicalize(candidate.string());
            }

            if (mirage_path.empty()) {
                return {};
            }

            auto fallback_candidate = std::filesystem::path(mirage_path) / import_path;
            if (!std::filesystem::is_directory(fallback_candidate)) {
                return {};
            }

            auto canonical_fallback = canonicalize(fallback_candidate.string());
            if (canonical_fallback.empty()) {
                return {};
            }

            if (!is_contained_in(std::filesystem::path(mirage_path), std::filesystem::path(canonical_fallback))) {
                return {};
            }

            return canonical_fallback;
        }

        void visit(const std::string &path, Program &program, SourceManager &source_manager, DiagnosticEngine &diagnostics, const std::string &mirage_path) {
            if (program.modules.contains(path)) {
                return;
            }

            auto [it, inserted] = program.modules.try_emplace(path);
            if (!inserted) {
                return;
            }

            it->second = load_and_parse(it->first, source_manager, diagnostics, program);

            for (auto &[import_str, import_location] : find_import_strings(program.modules[path])) {
                auto resolved_path = resolve_import_path(path, import_str, mirage_path);
                if (resolved_path.empty()) {
                    diagnostics.report_error(
                        DiagnosticStage::Parser, import_location,
                        std::format("cannot resolve import path '{}' from '{}'", import_str, path));

                    continue;
                }

                program.module_imports[path][import_str] = resolved_path;

                visit(resolved_path, program, source_manager, diagnostics, mirage_path);
            }
        }
    }

    auto resolve_contained_path(const std::string &base_dir, const std::string &relative_path) -> std::string {
        const std::filesystem::path rel(relative_path);
        if (rel.is_absolute()) {
            return {};
        }

        std::error_code ec;
        const auto candidate = std::filesystem::path(base_dir) / rel;
        const auto canonical_candidate = std::filesystem::weakly_canonical(candidate, ec);
        if (ec || !is_contained_in(std::filesystem::path(base_dir), canonical_candidate)) {
            return {};
        }

        return canonical_candidate.string();
    }

    auto canonicalize(const std::string &path) -> std::string {
        std::error_code error;

        const auto canonical_path = std::filesystem::weakly_canonical(path, error);
        if (error) {
            return {};
        }

        return canonical_path.string();
    }

    auto resolve(const std::string &root_module_path, SourceManager &source_manager, DiagnosticEngine &diagnostics) -> Program {
        Program program;

        const auto canonical = canonicalize(root_module_path);
        if (canonical.empty() || !std::filesystem::is_directory(canonical)) {
            diagnostics.report_error(
                DiagnosticStage::Parser, {},
                std::format("'{}' is not a valid module directory", root_module_path));

            return program;
        }

        program.root_module_path = canonical;

        std::string mirage_path;
        if (const char *env_value = std::getenv("MIRAGE_PATH"); env_value != nullptr) {
            auto candidate = canonicalize(env_value);
            if (!candidate.empty() && std::filesystem::is_directory(candidate)) {
                mirage_path = candidate;
            }
        }

        visit(canonical, program, source_manager, diagnostics, mirage_path);

        program.ok = !diagnostics.has_errors();

        return program;
    }
}
