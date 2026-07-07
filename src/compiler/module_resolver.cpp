#include "module_resolver.hpp"

#include "lexer.hpp"
#include "source_manager.hpp"

#include <algorithm>
#include <filesystem>

namespace ast {
    namespace {
        auto load_and_parse(const std::string &canonical_path, SourceManager &source_manager, DiagnosticEngine &diagnostics) -> Module {
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
                auto tokens = lexer::tokenize(source_file.text, source_file.filename, diagnostics);
                if (diagnostics.has_errors()) return {};
                auto decls = parse(tokens, diagnostics);
                combined.insert(combined.end(), std::make_move_iterator(decls.begin()), std::make_move_iterator(decls.end()));
            }
            return combined;
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
            auto candidate = std::filesystem::path(importer_path) / import_path;
            if (!std::filesystem::is_directory(candidate)) {
                return {};
            }
            return canonicalize(candidate.string());
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
        if (canonical.empty() || !std::filesystem::is_directory(canonical)) {
            diagnostics.report_error(
                DiagnosticStage::Parser, {},
                std::format("'{}' is not a valid module directory", root_module_path));

            return program;
        }

        program.root_module_path = canonical;

        visit(canonical, program, source_manager, diagnostics);

        program.ok = !diagnostics.has_errors();

        return program;
    }
}
