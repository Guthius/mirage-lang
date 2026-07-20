#include "sema.hpp"

#include <algorithm>
#include <format>
#include <unordered_set>

namespace sema {
    namespace {
        auto is_constant_expr_impl(const ast::Expr &expr, const std::string &module_path, const Program &program, const std::unordered_set<std::string> &treated_as_const) -> bool {
            if (const auto mod_it = program.modules.find(module_path); mod_it != program.modules.end()) {
                if (mod_it->second.expr_variant_coercions.contains(get_expr_key(expr))) {
                    return false; // implicit tagged-union coercion has runtime stores
                }
                if (mod_it->second.expr_trait_coercions.contains(get_expr_key(expr))) {
                    return false; // implicit pointer-to-trait-handle coercion has runtime stores
                }
            }

            return std::visit(
                [&]<typename T>(const T &v) -> bool {
                    using V = std::decay_t<T>;

                    if constexpr (std::is_same_v<V, ast::LiteralIntegerExpr> ||
                                  std::is_same_v<V, ast::LiteralFloatExpr> ||
                                  std::is_same_v<V, ast::LiteralStringExpr> ||
                                  std::is_same_v<V, ast::LiteralCharExpr> ||
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

                    if constexpr (std::is_same_v<V, ast::ImportBinExpr>) {
                        return true;
                    }

                    if constexpr (std::is_same_v<V, ast::IotaExpr>) {
                        return true;
                    }

                    if constexpr (std::is_same_v<V, ast::DotIdentExpr>) {
                        return true;
                    }

                    if constexpr (std::is_same_v<V, std::unique_ptr<ast::TaggedVariantExpr>>) {
                        return false; // has runtime stores
                    }

                    if constexpr (std::is_same_v<V, std::unique_ptr<ast::BracedInitializerExpr>>) {
                        return std::visit(
                            [&]<typename BV>(const BV &bv) -> bool {
                                using BVT = std::decay_t<BV>;

                                if constexpr (std::is_same_v<BVT, ast::EmptyExpr>) {
                                    return true;
                                } else if constexpr (std::is_same_v<BVT, ast::ArrayExpr>) {
                                    for (const auto &elem : bv.values) {
                                        if (std::holds_alternative<ast::UndefinedExpr>(elem)) continue;
                                        if (!is_constant_expr_impl(elem, module_path, program, treated_as_const)) {
                                            return false;
                                        }
                                    }
                                    return true;
                                } else { // ast::StructExpr
                                    const auto mod_it = program.modules.find(module_path);
                                    if (mod_it == program.modules.end()) {
                                        return false;
                                    }

                                    const auto ty_it = mod_it->second.expr_types.find(get_expr_key(expr));
                                    if (ty_it == mod_it->second.expr_types.end()) {
                                        return false;
                                    }

                                    const auto &ty = ty_it->second;
                                    if (ty.kind == TypeKind::Union) {
                                        return false; // union member construction has runtime stores
                                    }

                                    std::unordered_set<std::string> provided;
                                    for (const auto &sf : bv.fields) {
                                        if (std::holds_alternative<ast::UndefinedExpr>(sf.expr)) {
                                            provided.insert(sf.name);
                                            continue;
                                        }
                                        if (!is_constant_expr_impl(sf.expr, module_path, program, treated_as_const)) {
                                            return false;
                                        }

                                        // Reject values that require a runtime lvalue-based coercion
                                        // (array<->slice, array/slice->pointer) that only emit_value_as
                                        // can perform; the constant codegen path can't do this.
                                        const auto field_ty_it = mod_it->second.expr_types.find(get_expr_key(sf.expr));
                                        if (const auto *info = ty.kind == TypeKind::Struct ? program.struct_at(ty.struct_index) : nullptr;
                                            info && field_ty_it != mod_it->second.expr_types.end()) {
                                            const auto field_it = std::ranges::find(info->fields, sf.name, &StructField::name);
                                            if (field_it != info->fields.end()) {
                                                const auto &from = field_ty_it->second;
                                                const auto &target = field_it->type;
                                                const bool needs_runtime_coercion =
                                                    (from.kind == TypeKind::Array && target.kind == TypeKind::Slice) ||
                                                    (from.kind == TypeKind::Slice && target.kind == TypeKind::Array) ||
                                                    (from.kind == TypeKind::Array && (target.kind == TypeKind::Pointer || target.kind == TypeKind::Anyptr)) ||
                                                    (from.kind == TypeKind::Slice && (target.kind == TypeKind::Pointer || target.kind == TypeKind::Anyptr));
                                                if (needs_runtime_coercion) {
                                                    return false;
                                                }
                                            }
                                        }

                                        provided.insert(sf.name);
                                    }

                                    if (const auto *info = ty.kind == TypeKind::Struct ? program.struct_at(ty.struct_index) : nullptr) {
                                        for (const auto &field : info->fields) {
                                            if (provided.contains(field.name) || !field.init_expr) {
                                                continue;
                                            }
                                            if (!is_constant_expr_impl(*field.init_expr, module_path, program, treated_as_const)) {
                                                return false;
                                            }
                                        }
                                    }

                                    return true;
                                }
                            },
                            *v);
                    }

                    if constexpr (std::is_same_v<V, ast::DefaultExpr>) {
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
        if (const auto resolved = resolve_declared_type(g->decl->type, g->decl->init, module_path, program, diag, g->decl->location)) {
            declared_ty = *resolved;
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

        if (m->decl->result_type) {
            const auto declared_ty = resolve_type(*m->decl->result_type, module_path, program, diag);
            const auto actual_ty = check_expr(m->decl->expr_template, macro_scope, module_path, program, diag, declared_ty, 0);
            if (!is_assignable(actual_ty, declared_ty)) {
                diag.report_error(DiagnosticStage::Sema, m->decl->location,
                    "macro body type does not match declared result type");
            }
            m->result_type = declared_ty;
            m->has_declared_result_type = true;
        } else {
            m->result_type = check_expr(m->decl->expr_template, macro_scope, module_path, program, diag, std::nullopt, 0);
        }
        m->is_resolved = true;

        program.resolve_state.value_resolving.erase(key);
        return *m;
    }

    auto is_constant_expr(const ast::Expr &expr, const std::string &module_path, const Program &program) -> bool {
        return is_constant_expr_impl(expr, module_path, program, {});
    }

    auto evaluate_integer_constant(const ast::Expr &expr, const std::string &module_path, const Program &program) -> std::optional<int64_t> {
        return std::visit(
            [&]<typename T>(const T &v) -> std::optional<int64_t> {
                using V = std::decay_t<T>;

                if constexpr (std::is_same_v<V, ast::LiteralIntegerExpr>) {
                    return static_cast<int64_t>(v.value);
                }

                if constexpr (std::is_same_v<V, ast::LiteralBoolExpr>) {
                    return v.value ? int64_t{1} : int64_t{0};
                }

                if constexpr (std::is_same_v<V, ast::LiteralCharExpr>) {
                    return static_cast<int64_t>(v.value);
                }

                if constexpr (std::is_same_v<V, ast::IdentExpr>) {
                    const auto mod_it = program.modules.find(module_path);
                    if (mod_it == program.modules.end()) return std::nullopt;
                    const auto sym_it = mod_it->second.symbols.find(v.name);
                    if (sym_it == mod_it->second.symbols.end()) return std::nullopt;
                    const auto *g = std::get_if<GlobalSymbol>(&sym_it->second);
                    if (!g || g->is_mut || !g->decl->init) return std::nullopt;
                    return evaluate_integer_constant(*g->decl->init, module_path, program);
                }

                if constexpr (std::is_same_v<V, std::unique_ptr<ast::UnaryExpr>>) {
                    if (v->op == ast::UnaryOp::Negate) {
                        auto inner = evaluate_integer_constant(v->operand, module_path, program);
                        if (inner) return -(*inner);
                    }
                    if (v->op == ast::UnaryOp::BitwiseNot) {
                        auto inner = evaluate_integer_constant(v->operand, module_path, program);
                        if (inner) return ~(*inner);
                    }
                    return std::nullopt;
                }

                if constexpr (std::is_same_v<V, std::unique_ptr<ast::BinaryExpr>>) {
                    auto lhs = evaluate_integer_constant(v->lhs, module_path, program);
                    auto rhs = evaluate_integer_constant(v->rhs, module_path, program);
                    if (!lhs || !rhs) return std::nullopt;
                    switch (v->op) {
                    case ast::BinaryOp::Add:        return *lhs + *rhs;
                    case ast::BinaryOp::Sub:        return *lhs - *rhs;
                    case ast::BinaryOp::Mul:        return *lhs * *rhs;
                    case ast::BinaryOp::Div:        return *rhs != 0 ? std::optional<int64_t>{*lhs / *rhs} : std::nullopt;
                    case ast::BinaryOp::Mod:        return *rhs != 0 ? std::optional<int64_t>{*lhs % *rhs} : std::nullopt;
                    case ast::BinaryOp::BitwiseAnd: return *lhs & *rhs;
                    case ast::BinaryOp::BitwiseOr:  return *lhs | *rhs;
                    case ast::BinaryOp::BitwiseXor: return *lhs ^ *rhs;
                    case ast::BinaryOp::ShiftLeft:  return *lhs << *rhs;
                    case ast::BinaryOp::ShiftRight: return *lhs >> *rhs;
                    default:                        return std::nullopt;
                    }
                }

                if constexpr (std::is_same_v<V, std::unique_ptr<ast::CastExpr>>) {
                    return evaluate_integer_constant(v->value, module_path, program);
                }

                return std::nullopt;
            },
            expr);
    }
}
