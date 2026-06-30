#include "sema.hpp"

#include <format>
#include <unordered_set>

namespace sema {
    namespace {
        auto is_constant_expr_impl(const ast::Expr &expr, const std::string &module_path, const Program &program, const std::unordered_set<std::string> &treated_as_const) -> bool {
            return std::visit(
                [&]<typename T>(const T &v) -> bool {
                    using V = std::decay_t<T>;

                    if constexpr (std::is_same_v<V, ast::LiteralIntegerExpr> ||
                                  std::is_same_v<V, ast::LiteralFloatExpr> ||
                                  std::is_same_v<V, ast::LiteralStringExpr> ||
                                  std::is_same_v<V, ast::LiteralBoolExpr> ||
                                  std::is_same_v<V, ast::LiteralNilExpr>) {
                        return true;
                    }

                    if constexpr (std::is_same_v<V, ast::IdentExpr>) {
                        if (treated_as_const.count(v.name)) {
                            return true;
                        }

                        const auto mod_it = program.modules.find(module_path);
                        if (mod_it == program.modules.end()) {
                            return false;
                        }

                        const auto sym_it = mod_it->second.symbols.find(v.name);
                        if (sym_it == mod_it->second.symbols.end()) {
                            return false;
                        }

                        if (const auto *g = std::get_if<GlobalSymbol>(&sym_it->second)) {
                            return !g->is_mut && g->is_resolved;
                        }

                        return false;
                    }

                    if constexpr (std::is_same_v<V, std::unique_ptr<ast::SizeOfExpr>>) {
                        return true;
                    }

                    if constexpr (std::is_same_v<V, ast::IotaExpr>) {
                        return true;
                    }

                    if constexpr (std::is_same_v<V, ast::DotIdentExpr>) {
                        return true;
                    }

                    if constexpr (std::is_same_v<V, std::unique_ptr<ast::UnaryExpr>>) {
                        if (v->op == ast::UnaryOp::AddressOf || v->op == ast::UnaryOp::Deref) {
                            return false;
                        }

                        return is_constant_expr_impl(v->operand, module_path, program, treated_as_const);
                    }

                    if constexpr (std::is_same_v<V, std::unique_ptr<ast::BinaryExpr>>) {
                        return is_constant_expr_impl(v->lhs, module_path, program, treated_as_const) &&
                               is_constant_expr_impl(v->rhs, module_path, program, treated_as_const);
                    }

                    if constexpr (std::is_same_v<V, std::unique_ptr<ast::TernaryExpr>>) {
                        return is_constant_expr_impl(v->condition, module_path, program, treated_as_const) &&
                               is_constant_expr_impl(v->then_expr, module_path, program, treated_as_const) &&
                               is_constant_expr_impl(v->else_expr, module_path, program, treated_as_const);
                    }

                    if constexpr (std::is_same_v<V, std::unique_ptr<ast::CastExpr>>) {
                        return is_constant_expr_impl(v->value, module_path, program, treated_as_const);
                    }

                    if constexpr (std::is_same_v<V, std::unique_ptr<ast::CallExpr>>) {
                        const auto *callee_ident = std::get_if<ast::IdentExpr>(&v->callee);
                        if (!callee_ident) {
                            return false;
                        }

                        const auto mod_it = program.modules.find(module_path);
                        if (mod_it == program.modules.end()) {
                            return false;
                        }

                        const auto sym_it = mod_it->second.symbols.find(callee_ident->name);
                        if (sym_it == mod_it->second.symbols.end()) {
                            return false;
                        }

                        const auto *macro = std::get_if<MacroSymbol>(&sym_it->second);
                        if (!macro || !macro->is_resolved) {
                            return false;
                        }

                        for (const auto &arg : v->args) {
                            if (!is_constant_expr_impl(arg, module_path, program, treated_as_const)) {
                                return false;
                            }
                        }

                        std::unordered_set<std::string> extended = treated_as_const;
                        for (const auto &p : macro->decl->params) {
                            extended.insert(p.name);
                        }

                        return is_constant_expr_impl(macro->decl->expr_template, module_path, program, extended);
                    }

                    return false;
                },
                expr);
        }

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

        if (g->decl->init) {
            if (!is_constant_expr_impl(*g->decl->init, module_path, program, {})) {
                diag.report_error(
                    DiagnosticStage::Sema, g->decl->location,
                    "global variable initializer must be a compile-time constant expression");
            }
        }

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

    auto is_constant_expr(const ast::Expr &expr, const std::string &module_path, const Program &program) -> bool {
        return is_constant_expr_impl(expr, module_path, program, {});
    }
}
