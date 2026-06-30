#include "sema.hpp"

#include <format>

namespace sema {
    namespace {
        auto error_invalid(DiagnosticEngine &diag, const SourceLocation &loc, std::string msg) -> ResolvedType {
            diag.report_error(DiagnosticStage::Sema, loc, std::move(msg));
            return ResolvedType{
                .kind = TypeKind::Invalid,
            };
        }
    }

    auto resolve_global_symbol(const std::string &module_path, const std::string &name, Program &program, DiagnosticEngine &diag, const SourceLocation &loc) -> ResolvedType {
        const auto mod_it = program.modules.find(module_path);
        if (mod_it == program.modules.end()) {
            return error_invalid(diag, loc, std::format("internal error: module '{}' not found", module_path));
        }

        const auto sym_it = mod_it->second.symbols.find(name);
        if (sym_it == mod_it->second.symbols.end()) {
            return error_invalid(diag, loc, std::format("unknown identifier '{}'", name));
        }

        auto *g = std::get_if<GlobalSymbol>(&sym_it->second);
        if (!g) {
            return error_invalid(diag, loc, std::format("'{}' is not a global value", name));
        }

        if (g->is_resolved) {
            return g->type;
        }

        const auto key = std::make_pair(module_path, name);
        if (program.resolve_state.value_resolving.contains(key)) {
            return error_invalid(diag, loc, std::format("circular dependency detected resolving '{}'", name));
        }

        program.resolve_state.value_resolving.insert(key);

        ResolvedType declared_ty{.kind = TypeKind::Void};

        bool has_declared_ty = false;
        if (g->decl->type) {
            declared_ty = resolve_type(*g->decl->type, module_path, program, diag);
            has_declared_ty = true;
        }

        if (g->decl->init) {
            LocalScope no_locals;
            const auto init_ty = check_expr(*g->decl->init, no_locals, module_path, program, diag, has_declared_ty ? std::optional(declared_ty) : std::nullopt, 0);
            if (has_declared_ty) {
                if (!is_assignable(init_ty, declared_ty)) {
                    diag.report_error(DiagnosticStage::Sema, g->decl->location, "type mismatch in variable declaration");
                }
                g->type = declared_ty;
            } else {
                g->type = init_ty;
            }
        } else {
            if (!g->decl->is_mut) {
                diag.report_error(DiagnosticStage::Sema, g->decl->location, "'const' requires an initializer");
            }
            g->type = declared_ty;
        }

        g->is_resolved = true;
        program.resolve_state.value_resolving.erase(key);
        return g->type;
    }

    auto resolve_macro_symbol(const std::string &module_path, const std::string &name, Program &program, DiagnosticEngine &diag, const SourceLocation &loc) -> MacroSymbol & {
        static MacroSymbol invalid_sentinel{};

        const auto mod_it = program.modules.find(module_path);
        if (mod_it == program.modules.end()) {
            diag.report_error(DiagnosticStage::Sema, loc, std::format("internal error: module '{}' not found", module_path));
            return invalid_sentinel;
        }

        const auto sym_it = mod_it->second.symbols.find(name);
        if (sym_it == mod_it->second.symbols.end()) {
            diag.report_error(DiagnosticStage::Sema, loc, std::format("unknown identifier '{}'", name));
            return invalid_sentinel;
        }

        auto *m = std::get_if<MacroSymbol>(&sym_it->second);
        if (!m) {
            diag.report_error(DiagnosticStage::Sema, loc, std::format("'{}' is not a macro", name));
            return invalid_sentinel;
        }

        if (m->is_resolved) {
            return *m;
        }

        const auto key = std::make_pair(module_path, name);
        if (program.resolve_state.value_resolving.contains(key)) {
            diag.report_error(DiagnosticStage::Sema, loc, std::format("circular dependency detected resolving '{}'", name));
            return *m;
        }

        program.resolve_state.value_resolving.insert(key);

        for (auto &p : m->decl->params) {
            m->params.push_back(resolve_type(p.type, module_path, program, diag));
        }

        LocalScope macro_scope;
        for (size_t i = 0; i < m->decl->params.size(); ++i) {
            macro_scope[m->decl->params[i].name] = LocalBinding{.type = m->params[i], .is_mut = false};
        }

        m->result_type = check_expr(m->decl->expr_template, macro_scope, module_path, program, diag, std::nullopt, 0);
        m->is_resolved = true;

        program.resolve_state.value_resolving.erase(key);
        return *m;
    }
}
