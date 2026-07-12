#include "analysis.hpp"

#include <filesystem>

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
        ast_program.ok = true;
        auto sema_program = sema::check_program(ast_program, diag);

        return ProgramResult{
            .source_manager = std::move(source_manager),
            .ast_program = std::move(ast_program),
            .sema_program = std::move(sema_program),
            .diagnostics = diag.diagnostics(),
        };
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
    }

    auto DocumentStore::text_of(const std::string &canonical_path) const -> const std::string * {
        const auto it = open_texts_.find(canonical_path);
        return it == open_texts_.end() ? nullptr : &it->second;
    }

    void DocumentStore::invalidate(const std::string &canonical_path) {
        const auto dir = std::filesystem::path(canonical_path).parent_path().string();

        for (auto it = module_results_.begin(); it != module_results_.end();) {
            if (it->second.ast_program.modules.contains(dir)) {
                it = module_results_.erase(it);
            } else {
                ++it;
            }
        }
    }

    auto DocumentStore::ensure_analysed(const std::string &canonical_path) -> ProgramResult & {
        const auto dir = ast::canonicalize(std::filesystem::path(canonical_path).parent_path().string());

        for (auto &[root, result] : module_results_) {
            if (result.ast_program.modules.contains(dir)) {
                return result;
            }
        }

        auto [it, inserted] = module_results_.insert_or_assign(dir, analyse(dir, open_texts_));
        return it->second;
    }
}
