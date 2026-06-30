#include "module_resolver.hpp"

#include "lexer.hpp"
#include "source_manager.hpp"

#include <filesystem>
#include <fstream>

namespace ast {
    namespace {
        auto load_and_parse(const std::string &canonical_path, SourceManager &source_manager, DiagnosticEngine &diagnostics) -> Module {
            const auto source = source_manager.load(canonical_path, diagnostics);
            if (source.empty()) {
                return {};
            }

            auto tokens = lexer::tokenize(source, canonical_path, diagnostics);
            if (diagnostics.has_errors()) {
                return {};
            }

            return parse(tokens, diagnostics);
        }

        auto find_import_strings(const Module &module) -> std::vector<std::string> {
            std::vector<std::string> found;

            for (auto &decl : module) {
                if (auto *var_decl = std::get_if<VarDecl>(&decl); var_decl && var_decl->init) {
                    if (auto *import_stmt = std::get_if<ImportExpr>(&*var_decl->init)) {
                        found.push_back(import_stmt->module_name);
                    }
                }
            }

            return found;
        }

        auto resolve_import_path(const std::string &importer_path, const std::string &import_path) -> std::string {
            auto parent_path = std::filesystem::path(importer_path).parent_path();
            auto candidate_path = parent_path / import_path;
            auto candidate_path_with_ext = candidate_path;

            candidate_path_with_ext += ".mir";
            if (std::filesystem::exists(candidate_path_with_ext)) {
                return canonicalize(candidate_path_with_ext);
            }

            return {};
        }

        void visit(const std::string &path, Program &program, SourceManager &source_manager, DiagnosticEngine &diagnostics) {
            if (program.modules.contains(path)) {
                return;
            }

            auto [it, inserted] = program.modules.try_emplace(path);
            if (!inserted) {
                return;
            }

            it->second = load_and_parse(it->first, source_manager, diagnostics);

            for (auto &import_str : find_import_strings(program.modules[path])) {
                auto resolved_path = resolve_import_path(path, import_str);
                if (resolved_path.empty()) {
                    diagnostics.report_error(
                        DiagnosticStage::Parser, {},
                        std::format("cannot resolve import path '{}' from '{}'", import_str, path));

                    continue;
                }

                program.module_imports[path][import_str] = resolved_path;

                visit(resolved_path, program, source_manager, diagnostics);
            }
        }
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
        if (canonical.empty()) {
            diagnostics.report_error(
                DiagnosticStage::Parser, {},
                std::format("cannot resolve root module path '{}'", root_module_path));

            return program;
        }

        program.root_module_path = canonical;

        visit(canonical, program, source_manager, diagnostics);

        program.ok = !diagnostics.has_errors();

        return program;
    }
}
