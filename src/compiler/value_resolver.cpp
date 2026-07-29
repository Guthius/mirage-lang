#include "sema.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <format>
#include <limits>
#include <unordered_set>

namespace sema {
    namespace {
        // Guarded signed-integer folding, shared by this file's two evaluators.
        //
        // Division and modulo overflow for INT64_MIN / -1, which is undefined behaviour and
        // traps with SIGFPE on x86-64 -- a hard compiler crash, not a diagnostic. A shift by a
        // negative amount, or by at least the operand width, is likewise undefined; in
        // practice x86 masks the count, so '1 << 64' silently folded to 1 and a case label
        // written that way matched 1.
        //
        // Returning nullopt makes the expression "not a constant expression", which callers
        // already handle and report. The uint64_t sibling in type_resolver.cpp
        // (eval_integer_const_expr) has always guarded its shifts this way; unsigned division
        // cannot overflow, which is why only these two needed the division guard.
        auto fold_div(const int64_t lhs, const int64_t rhs) -> std::optional<int64_t> {
            if (rhs == 0 || (lhs == std::numeric_limits<int64_t>::min() && rhs == -1)) return std::nullopt;
            return lhs / rhs;
        }

        auto fold_mod(const int64_t lhs, const int64_t rhs) -> std::optional<int64_t> {
            if (rhs == 0 || (lhs == std::numeric_limits<int64_t>::min() && rhs == -1)) return std::nullopt;
            return lhs % rhs;
        }

        auto fold_shift_left(const int64_t lhs, const int64_t rhs) -> std::optional<int64_t> {
            if (rhs < 0 || rhs >= 64) return std::nullopt;
            return static_cast<int64_t>(static_cast<uint64_t>(lhs) << static_cast<uint64_t>(rhs));
        }

        auto fold_shift_right(const int64_t lhs, const int64_t rhs) -> std::optional<int64_t> {
            if (rhs < 0 || rhs >= 64) return std::nullopt;
            return lhs >> rhs;
        }

        // Resolves the module a MemberExpr's `object` refers to, for the two shapes cross-
        // module qualified const access can take: a plain identifier bound to an
        // ImportSymbol (`opts.target_os`, opts := import(...)) or an inline import used
        // directly as the object (`import("...").target_os`) — mirrors
        // try_resolve_namespace_chain's IdentExpr/ImportExpr base cases (sema_check.cpp),
        // duplicated here since is_constant_expr_impl/evaluate_const_value run without a
        // LocalScope and need to be checkable before/without a full check_expr pass.
        auto resolve_member_object_import_path(const ast::Expr &object, const std::string &module_path, const Program &program) -> std::optional<std::string> {
            if (const auto *obj_ident = std::get_if<ast::IdentExpr>(&object)) {
                const auto mod_it = program.modules.find(module_path);
                if (mod_it == program.modules.end()) return std::nullopt;
                const auto sym_it = mod_it->second.symbols.find(obj_ident->name);
                if (sym_it == mod_it->second.symbols.end()) return std::nullopt;
                const auto *imp = std::get_if<ImportSymbol>(&sym_it->second);
                if (!imp) return std::nullopt;
                return imp->module_path;
            }
            if (const auto *obj_import = std::get_if<ast::ImportExpr>(&object)) {
                const auto mod_it = program.modules.find(module_path);
                if (mod_it == program.modules.end()) return std::nullopt;
                const auto path_it = mod_it->second.inline_import_paths.find(obj_import);
                if (path_it == mod_it->second.inline_import_paths.end()) return std::nullopt;
                return path_it->second;
            }
            return std::nullopt;
        }

        auto is_constant_expr_impl(const ast::Expr &expr, const std::string &module_path, const Program &program, const std::unordered_set<std::string> &treated_as_const) -> bool {
            if (const auto mod_it = program.modules.find(module_path); mod_it != program.modules.end()) {
                if (mod_it->second.expr_variant_coercions.contains(get_expr_key(expr))) {
                    return false; // implicit tagged-union coercion has runtime stores
                }
                if (mod_it->second.expr_trait_coercions.contains(get_expr_key(expr))) {
                    return false; // implicit pointer-to-trait-handle coercion has runtime stores
                }
                if (mod_it->second.expr_trait_handle_coercions.contains(get_expr_key(expr))) {
                    return false; // implicit handle-to-handle trait narrowing has a runtime load + store
                }
                if (mod_it->second.expr_any_coercions.contains(get_expr_key(expr))) {
                    return false; // implicit any-coercion materializes a runtime {id, data} value
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

                    // Cross-module qualified const access, e.g. 'opts.target_os' — the
                    // direct-object-is-an-IdentExpr shape mirrors evaluate_const_value's
                    // MemberExpr case, which is what actually folds this expression's value;
                    // this only decides whether it's ELIGIBLE to be folded at all.
                    if constexpr (std::is_same_v<V, std::unique_ptr<ast::MemberExpr>>) {
                        const auto other_module_path = resolve_member_object_import_path(v->object, module_path, program);
                        if (!other_module_path) return false;

                        const auto other_mod_it = program.modules.find(*other_module_path);
                        if (other_mod_it == program.modules.end()) return false;
                        const auto other_sym_it = other_mod_it->second.symbols.find(v->member);
                        if (other_sym_it == other_mod_it->second.symbols.end()) return false;
                        const auto *g = std::get_if<GlobalSymbol>(&other_sym_it->second);
                        return g && !g->is_mut && g->is_resolved;
                    }

                    if constexpr (std::is_same_v<V, std::unique_ptr<ast::SizeOfExpr>>) {
                        return true;
                    }

                    if constexpr (std::is_same_v<V, std::unique_ptr<ast::AlignOfExpr>>) {
                        return true;
                    }

                    // Compile-time constant for every operand EXCEPT one whose resolved type is
                    // 'any' — that case lowers to a runtime read of the any value's type id
                    // (see codegen.cpp's TypeOfExpr case).
                    if constexpr (std::is_same_v<V, std::unique_ptr<ast::TypeOfExpr>>) {
                        if (const auto mod_it = program.modules.find(module_path); mod_it != program.modules.end()) {
                            if (const auto ty_it = mod_it->second.expr_types.find(get_expr_key(v->operand)); ty_it != mod_it->second.expr_types.end()) {
                                return ty_it->second.kind != TypeKind::Any;
                            }
                        }
                        return true;
                    }

                    // Never a compile-time constant: always materializes to a pointer value at
                    // codegen time (either the address of a specific '__type_info_<id>' global,
                    // or the result of a runtime binary-search table lookup).
                    if constexpr (std::is_same_v<V, std::unique_ptr<ast::TypeInfoOfExpr>>) {
                        return false;
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
                                } else if constexpr (std::is_same_v<BVT, ast::BitsetExpr>) {
                                    return true; // member names are plain strings, always compile-time constant
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

                    if constexpr (std::is_same_v<V, std::unique_ptr<ast::WhenExpr>>) {
                        return is_constant_expr_impl(v->condition, module_path, program, treated_as_const) &&
                               is_constant_expr_impl(v->then_expr, module_path, program, treated_as_const) &&
                               is_constant_expr_impl(v->else_expr, module_path, program, treated_as_const);
                    }

                    // Always compile-time evaluable (like SizeOfExpr/ImportBinExpr above) — its
                    // value comes from '--opt' or a constant default, never a runtime store.
                    if constexpr (std::is_same_v<V, std::unique_ptr<ast::OptionExpr>>) {
                        return true;
                    }

                    // Same as OptionExpr above, but the value comes from an environment
                    // variable or a constant default — still never a runtime store.
                    if constexpr (std::is_same_v<V, std::unique_ptr<ast::EnvExpr>>) {
                        return true;
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
            const auto init_ty = check_expr(*g->decl->init, no_locals, module_path, program, diag, has_declared_ty ? std::optional(declared_ty) : std::nullopt, 0, -1, nullptr);
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

    // Lazily/reentrantly resolves a free function's signature — mirrors
    // resolve_global_symbol above. Required because a default parameter
    // expression may call another function, and a ':=' inferred-type parameter's
    // type is itself derived from checking its default expression: one function's
    // signature can therefore depend on another's, regardless of Program::modules'
    // unordered iteration order.
    auto ensure_function_signature_resolved(const std::string &module_path, const std::string &name, Program &program, DiagnosticEngine &diag) -> FunctionSymbol & {
        auto &sym = std::get<FunctionSymbol>(program.modules.at(module_path).symbols.at(name));
        if (sym.is_resolved) {
            return sym;
        }

        const auto key = std::make_pair(module_path, name);
        if (program.resolve_state.fn_signature_resolving.contains(key)) {
            diag.report_error(DiagnosticStage::Sema, sym.decl->location,
                std::format("circular dependency detected resolving default value(s) of function '{}'", name));
            sym.is_resolved = true; // avoid infinite recursion on repeated lookups of this cycle
            return sym;
        }
        program.resolve_state.fn_signature_resolving.insert(key);

        for (auto &p : sym.decl->params) {
            ResolvedType pt;
            if (p.type) {
                pt = resolve_type(*p.type, module_path, program, diag);
            } else {
                // ':=' inferred-type param — infer from the (parser-guaranteed) default expr,
                // checked in an empty (module-scope) LocalScope, no caller in scope.
                LocalScope empty;
                pt = check_expr(*p.default_value, empty, module_path, program, diag, std::nullopt, 0);
            }
            if (p.is_variadic) {
                sym.is_variadic = true;
                sym.variadic_element_type = pt;
                sym.params.push_back(intern_slice(program, pt));
            } else {
                sym.params.push_back(pt);
            }
        }
        for (auto &rt : sym.decl->return_types) {
            sym.return_types.push_back(resolve_type(rt, module_path, program, diag));
        }

        check_param_defaults(sym.decl->params, sym.params, sym.required_params, sym.param_default_is_const, module_path, program, diag);

        sym.is_resolved = true;
        program.resolve_state.fn_signature_resolving.erase(key);
        return sym;
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

    auto is_constant_expr(const ast::Expr &expr, const std::string &module_path, const Program &program,
                           const std::unordered_set<std::string> &extra_const_names) -> bool {
        // Merge in the ambient 'currently checking this generic instance's body' env's value
        // generic-param names too (see Program::active_generic_env_stack), so e.g. 'const x
        // := N * 2' inside a generic body recognizes 'N' as constant without every caller
        // needing to pass it explicitly.
        if (program.active_generic_env_stack.empty()) {
            return is_constant_expr_impl(expr, module_path, program, extra_const_names);
        }
        auto names = extra_const_names;
        for (const auto &binding : *program.active_generic_env_stack.back()) {
            if (!binding.is_type) names.insert(binding.param_name);
        }
        return is_constant_expr_impl(expr, module_path, program, names);
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

                if constexpr (std::is_same_v<V, std::unique_ptr<ast::MemberExpr>>) {
                    // Cross-module qualified const access, e.g. 'linux.EINVAL' or
                    // 'import("...").EINVAL' — mirrors evaluate_const_value's identically-
                    // shaped MemberExpr case (this evaluator has no DiagnosticEngine to call
                    // resolve_global_symbol with, but by the time a match/switch arm pattern
                    // reaches here it's already been through check_expr, which resolves the
                    // referenced const's value as an ordinary side effect of type-checking it —
                    // same reasoning as the IdentExpr case above not calling it either).
                    const auto other_module_path = resolve_member_object_import_path(v->object, module_path, program);
                    if (!other_module_path) return std::nullopt;

                    const auto other_mod_it = program.modules.find(*other_module_path);
                    if (other_mod_it == program.modules.end()) return std::nullopt;
                    const auto other_sym_it = other_mod_it->second.symbols.find(v->member);
                    if (other_sym_it == other_mod_it->second.symbols.end()) return std::nullopt;
                    const auto *g = std::get_if<GlobalSymbol>(&other_sym_it->second);
                    if (!g || g->is_mut || !g->decl->init) return std::nullopt;
                    return evaluate_integer_constant(*g->decl->init, *other_module_path, program);
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
                    case ast::BinaryOp::Div:        return fold_div(*lhs, *rhs);
                    case ast::BinaryOp::Mod:        return fold_mod(*lhs, *rhs);
                    case ast::BinaryOp::BitwiseAnd: return *lhs & *rhs;
                    case ast::BinaryOp::BitwiseOr:  return *lhs | *rhs;
                    case ast::BinaryOp::BitwiseXor: return *lhs ^ *rhs;
                    case ast::BinaryOp::ShiftLeft:  return fold_shift_left(*lhs, *rhs);
                    case ast::BinaryOp::ShiftRight: return fold_shift_right(*lhs, *rhs);
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

    auto find_enum_field_by_name(const EnumInfo &info, const std::string_view name) -> const EnumFieldInfo * {
        const auto it = std::ranges::find(info.fields, name, &EnumFieldInfo::name);
        return it != info.fields.end() ? &*it : nullptr;
    }

    auto find_enum_field_by_value(const EnumInfo &info, const int64_t value) -> const EnumFieldInfo * {
        const auto it = std::ranges::find(info.fields, value, &EnumFieldInfo::value);
        return it != info.fields.end() ? &*it : nullptr;
    }

    namespace {
        // Coerces a raw '--opt key=value' (or '$env' environment-variable) string against
        // $option's/$env's resolved target type. Reports its own diagnostic (naming 'key')
        // and returns nullopt on failure. 'directive' ("$option"/"$env") and 'noun'
        // ("option"/"environment variable") let the two callers share this logic while
        // keeping diagnostics accurate about where the value actually came from.
        auto coerce_option_string(const std::string &raw, const ResolvedType &target, const Program &program,
                                   DiagnosticEngine &diag, const SourceLocation &loc, const std::string &key,
                                   const char *directive, const char *noun) -> std::optional<ConstFoldValue> {
            switch (target.kind) {
            case TypeKind::Bool: {
                std::string lower = raw;
                std::ranges::transform(lower, lower.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (lower == "true" || lower == "1") return int64_t{1};
                if (lower == "false" || lower == "0") return int64_t{0};
                diag.report_error(DiagnosticStage::Sema, loc,
                    std::format("{} '{}' expects a boolean value ('true'/'false'/'1'/'0'), got '{}'", noun, key, raw));
                return std::nullopt;
            }
            case TypeKind::I8: case TypeKind::I16: case TypeKind::I32: case TypeKind::I64:
            case TypeKind::U8: case TypeKind::U16: case TypeKind::U32: case TypeKind::U64:
            case TypeKind::USize: {
                try {
                    size_t pos = 0;
                    const long long parsed = std::stoll(raw, &pos, 10);
                    if (pos != raw.size()) throw std::invalid_argument("trailing characters");
                    return static_cast<int64_t>(parsed);
                } catch (...) {
                    diag.report_error(DiagnosticStage::Sema, loc,
                        std::format("{} '{}' expects an integer value, got '{}'", noun, key, raw));
                    return std::nullopt;
                }
            }
            case TypeKind::Slice: {
                if (const auto *slice_info = program.slice_at(target.slice_index); slice_info && slice_info->element_type.kind == TypeKind::U8) {
                    return raw;
                }
                diag.report_error(DiagnosticStage::Sema, loc,
                    std::format("'{}' does not support this target type for {} '{}'", directive, noun, key));
                return std::nullopt;
            }
            case TypeKind::Enum: {
                const auto *einfo = program.enum_at(target.enum_index);
                if (!einfo) return std::nullopt;

                if (const auto *by_name = find_enum_field_by_name(*einfo, raw)) {
                    return by_name->value;
                }

                try {
                    size_t pos = 0;
                    const long long parsed = std::stoll(raw, &pos, 10);
                    if (pos == raw.size()) {
                        if (const auto *by_value = find_enum_field_by_value(*einfo, parsed)) {
                            return by_value->value;
                        }
                    }
                } catch (...) {}

                std::string valid_names;
                for (const auto &field : einfo->fields) {
                    if (!valid_names.empty()) valid_names += ", ";
                    valid_names += field.name;
                }
                diag.report_error(DiagnosticStage::Sema, loc,
                    std::format("{} '{}' value '{}' does not match any variant of this enum; valid values: {}", noun, key, raw, valid_names));
                return std::nullopt;
            }
            default:
                diag.report_error(DiagnosticStage::Sema, loc,
                    std::format("'{}' does not support this target type for {} '{}'", directive, noun, key));
                return std::nullopt;
            }
        }
    }

    auto resolve_option_expr(const void *expr_key, const ast::OptionExpr &opt, std::optional<ResolvedType> expected,
                              const std::string &module_path, Program &program, DiagnosticEngine &diag) -> ResolvedType {
        ResolvedType target;
        if (expected) {
            target = *expected;
            // Still check the default against the known target — needed both to catch a
            // type mismatch and (for e.g. a bare '.Variant' enum literal default, which is
            // otherwise contextless) to populate expr_types so evaluate_const_value below
            // can resolve it.
            if (opt.default_value) {
                LocalScope empty;
                check_expr(*opt.default_value, empty, module_path, program, diag, target, 0);
            }
        } else if (opt.default_value) {
            LocalScope empty;
            target = check_expr(*opt.default_value, empty, module_path, program, diag, std::nullopt, 0);
        } else {
            target = intern_slice(program, ResolvedType{.kind = TypeKind::U8});
        }

        std::optional<ConstFoldValue> resolved_value;

        const auto opt_it = program.options.opt_values.find(opt.key);
        if (opt_it != program.options.opt_values.end()) {
            resolved_value = coerce_option_string(opt_it->second, target, program, diag, opt.location, opt.key, "$option", "option");
        } else if (opt.default_value) {
            resolved_value = evaluate_const_value(*opt.default_value, module_path, program, diag);
            if (!resolved_value) {
                diag.report_error(DiagnosticStage::Sema, opt.location,
                    std::format("'$option' default value for '{}' is not a compile-time constant", opt.key));
            }
        } else {
            diag.report_error(DiagnosticStage::Sema, opt.location,
                std::format("required option '{}' was not provided.\n       Pass it with: --opt {}=<value>", opt.key, opt.key));
        }

        if (resolved_value) {
            if (const auto mod_it = program.modules.find(module_path); mod_it != program.modules.end()) {
                mod_it->second.expr_option_values[expr_key] = *resolved_value;
            }
        }

        return target;
    }

    auto resolve_env_expr(const void *expr_key, const ast::EnvExpr &opt, std::optional<ResolvedType> expected,
                           const std::string &module_path, Program &program, DiagnosticEngine &diag) -> ResolvedType {
        ResolvedType target;
        if (expected) {
            target = *expected;
            if (opt.default_value) {
                LocalScope empty;
                check_expr(*opt.default_value, empty, module_path, program, diag, target, 0);
            }
        } else if (opt.default_value) {
            LocalScope empty;
            target = check_expr(*opt.default_value, empty, module_path, program, diag, std::nullopt, 0);
        } else {
            target = intern_slice(program, ResolvedType{.kind = TypeKind::U8});
        }

        std::optional<ConstFoldValue> resolved_value;

        if (const char *env_value = std::getenv(opt.key.c_str()); env_value != nullptr) {
            resolved_value = coerce_option_string(env_value, target, program, diag, opt.location, opt.key, "$env", "environment variable");
        } else if (opt.default_value) {
            resolved_value = evaluate_const_value(*opt.default_value, module_path, program, diag);
            if (!resolved_value) {
                diag.report_error(DiagnosticStage::Sema, opt.location,
                    std::format("'$env' default value for '{}' is not a compile-time constant", opt.key));
            }
        } else {
            diag.report_error(DiagnosticStage::Sema, opt.location,
                std::format("required environment variable '{}' was not set.\n       Set it with: {}=<value>", opt.key, opt.key));
        }

        if (resolved_value) {
            if (const auto mod_it = program.modules.find(module_path); mod_it != program.modules.end()) {
                mod_it->second.expr_option_values[expr_key] = *resolved_value;
            }
        }

        return target;
    }

    auto evaluate_const_value(const ast::Expr &expr, const std::string &module_path, Program &program, DiagnosticEngine &diag) -> std::optional<ConstFoldValue> {
        return std::visit(
            [&]<typename T>(const T &v) -> std::optional<ConstFoldValue> {
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
                if constexpr (std::is_same_v<V, ast::LiteralStringExpr>) {
                    return v.value;
                }

                if constexpr (std::is_same_v<V, ast::IdentExpr>) {
                    const auto mod_it = program.modules.find(module_path);
                    if (mod_it == program.modules.end()) return std::nullopt;
                    const auto sym_it = mod_it->second.symbols.find(v.name);
                    if (sym_it == mod_it->second.symbols.end()) return std::nullopt;
                    const auto *g = std::get_if<GlobalSymbol>(&sym_it->second);
                    if (!g || g->is_mut || !g->decl->init) return std::nullopt;
                    resolve_global_symbol(module_path, v.name, program, diag, g->decl->location);
                    return evaluate_const_value(*g->decl->init, module_path, program, diag);
                }

                if constexpr (std::is_same_v<V, std::unique_ptr<ast::MemberExpr>>) {
                    // Cross-module qualified const access, e.g. 'opts.target_os' or
                    // 'import("opts").target_os'.
                    const auto other_module_path = resolve_member_object_import_path(v->object, module_path, program);
                    if (!other_module_path) return std::nullopt;

                    const auto other_mod_it = program.modules.find(*other_module_path);
                    if (other_mod_it == program.modules.end()) return std::nullopt;
                    const auto other_sym_it = other_mod_it->second.symbols.find(v->member);
                    if (other_sym_it == other_mod_it->second.symbols.end()) return std::nullopt;
                    const auto *g = std::get_if<GlobalSymbol>(&other_sym_it->second);
                    if (!g || g->is_mut || !g->decl->init) return std::nullopt;
                    resolve_global_symbol(*other_module_path, v->member, program, diag, g->decl->location);
                    return evaluate_const_value(*g->decl->init, *other_module_path, program, diag);
                }

                if constexpr (std::is_same_v<V, ast::DotIdentExpr>) {
                    // A bare '.Variant' enum literal has no fixed value on its own — it only
                    // means something once check_expr has resolved it against an expected
                    // enum type, recorded in ITS OWN expr_types entry (populated whenever
                    // check_expr visited this exact node, e.g. because it's a '$option'
                    // default checked directly against the declared target type, or an
                    // operand of '==' checked against the other side's type — see
                    // resolve_option_expr and check_expr's BinaryExpr case respectively).
                    // If nothing ever check_expr'd this exact node, this legitimately can't
                    // be folded (the BinaryExpr Equal/NotEqual case below has its own
                    // sibling-type fallback for the one shape that can still route around
                    // that: a dot-ident used as the LHS of a comparison the outer context
                    // didn't already give an enum-typed 'expected').
                    const auto mod_it = program.modules.find(module_path);
                    if (mod_it == program.modules.end()) return std::nullopt;
                    const auto ty_it = mod_it->second.expr_types.find(get_expr_key(expr));
                    if (ty_it == mod_it->second.expr_types.end()) return std::nullopt;
                    if (ty_it->second.kind == TypeKind::Bitset) {
                        // A '.Member' resolved against a bitset-expected type folds to its
                        // one-bit MASK (1 << (value+1)), not the raw enum ordinal — see
                        // BitsetInfo's bit-index formula.
                        const auto *binfo = program.bitset_at(ty_it->second.bitset_index);
                        if (!binfo) return std::nullopt;
                        const auto *einfo = program.enum_at(binfo->member_enum_type.enum_index);
                        if (!einfo) return std::nullopt;
                        if (const auto *field = find_enum_field_by_name(*einfo, v.name)) {
                            return int64_t{1} << (field->value + 1);
                        }
                        return std::nullopt;
                    }
                    if (ty_it->second.kind != TypeKind::Enum) return std::nullopt;
                    const auto *einfo = program.enum_at(ty_it->second.enum_index);
                    if (!einfo) return std::nullopt;
                    if (const auto *field = find_enum_field_by_name(*einfo, v.name)) return field->value;
                    return std::nullopt;
                }

                if constexpr (std::is_same_v<V, std::unique_ptr<ast::BracedInitializerExpr>>) {
                    // Only a bitset literal folds here; Empty/Struct/Array literals have no
                    // pure-scalar constant form and are left to codegen's own emitters.
                    const auto *bitset_expr = std::get_if<ast::BitsetExpr>(v.get());
                    if (!bitset_expr) return std::nullopt;
                    const auto mod_it = program.modules.find(module_path);
                    if (mod_it == program.modules.end()) return std::nullopt;
                    const auto ty_it = mod_it->second.expr_types.find(get_expr_key(expr));
                    if (ty_it == mod_it->second.expr_types.end() || ty_it->second.kind != TypeKind::Bitset) return std::nullopt;
                    const auto *binfo = program.bitset_at(ty_it->second.bitset_index);
                    if (!binfo) return std::nullopt;
                    const auto *einfo = program.enum_at(binfo->member_enum_type.enum_index);
                    if (!einfo) return std::nullopt;
                    int64_t folded = 0;
                    for (const auto &name : bitset_expr->members) {
                        const auto *field = find_enum_field_by_name(*einfo, name);
                        if (!field) return std::nullopt;
                        folded |= int64_t{1} << (field->value + 1);
                    }
                    return folded;
                }

                if constexpr (std::is_same_v<V, std::unique_ptr<ast::UnaryExpr>>) {
                    if (v->op == ast::UnaryOp::AddressOf || v->op == ast::UnaryOp::Deref) return std::nullopt;
                    const auto inner = evaluate_const_value(v->operand, module_path, program, diag);
                    const auto *iv = inner ? std::get_if<int64_t>(&*inner) : nullptr;
                    if (!iv) return std::nullopt;
                    switch (v->op) {
                    case ast::UnaryOp::LogicalNot: return int64_t{*iv == 0 ? 1 : 0};
                    case ast::UnaryOp::Negate:     return int64_t{-*iv};
                    case ast::UnaryOp::BitwiseNot: return int64_t{~*iv};
                    default:                       return std::nullopt;
                    }
                }

                if constexpr (std::is_same_v<V, std::unique_ptr<ast::BinaryExpr>>) {
                    if (v->op == ast::BinaryOp::LogicalAnd || v->op == ast::BinaryOp::LogicalOr) {
                        const auto lhs = evaluate_const_value(v->lhs, module_path, program, diag);
                        const auto rhs = evaluate_const_value(v->rhs, module_path, program, diag);
                        const auto *li = lhs ? std::get_if<int64_t>(&*lhs) : nullptr;
                        const auto *ri = rhs ? std::get_if<int64_t>(&*rhs) : nullptr;
                        if (!li || !ri) return std::nullopt;
                        const bool lb = *li != 0, rb = *ri != 0;
                        return int64_t{(v->op == ast::BinaryOp::LogicalAnd ? (lb && rb) : (lb || rb)) ? 1 : 0};
                    }

                    if (v->op == ast::BinaryOp::Equal || v->op == ast::BinaryOp::NotEqual) {
                        // Handles the common 'mod.CONST == .EnumVariant' shape: a bare '.Variant'
                        // enum literal has no fixed value on its own (DotIdentExpr is contextual),
                        // so its value is resolved via the OTHER side's already-check_expr'd type
                        // (ProgramModule::expr_types), not by recursing into it directly.
                        const auto resolve_side = [&](const ast::Expr &side, const ast::Expr &other) -> std::optional<int64_t> {
                            if (const auto folded = evaluate_const_value(side, module_path, program, diag)) {
                                if (const auto *iv = std::get_if<int64_t>(&*folded)) return *iv;
                                return std::nullopt;
                            }
                            if (const auto *dot = std::get_if<ast::DotIdentExpr>(&side)) {
                                const auto mod_it = program.modules.find(module_path);
                                if (mod_it == program.modules.end()) return std::nullopt;
                                const auto ty_it = mod_it->second.expr_types.find(get_expr_key(other));
                                if (ty_it == mod_it->second.expr_types.end() || ty_it->second.kind != TypeKind::Enum) return std::nullopt;
                                const auto *einfo = program.enum_at(ty_it->second.enum_index);
                                if (!einfo) return std::nullopt;
                                if (const auto *field = find_enum_field_by_name(*einfo, dot->name)) return field->value;
                            }
                            return std::nullopt;
                        };
                        const auto lhs = resolve_side(v->lhs, v->rhs);
                        const auto rhs = resolve_side(v->rhs, v->lhs);
                        if (!lhs || !rhs) return std::nullopt;
                        return int64_t{(v->op == ast::BinaryOp::Equal ? (*lhs == *rhs) : (*lhs != *rhs)) ? 1 : 0};
                    }

                    const auto lhs = evaluate_const_value(v->lhs, module_path, program, diag);
                    const auto rhs = evaluate_const_value(v->rhs, module_path, program, diag);
                    const auto *li = lhs ? std::get_if<int64_t>(&*lhs) : nullptr;
                    const auto *ri = rhs ? std::get_if<int64_t>(&*rhs) : nullptr;
                    if (!li || !ri) return std::nullopt;
                    switch (v->op) {
                    case ast::BinaryOp::Add:          return *li + *ri;
                    case ast::BinaryOp::Sub:          return *li - *ri;
                    case ast::BinaryOp::Mul:          return *li * *ri;
                    case ast::BinaryOp::Div:          return fold_div(*li, *ri);
                    case ast::BinaryOp::Mod:          return fold_mod(*li, *ri);
                    case ast::BinaryOp::BitwiseAnd:   return *li & *ri;
                    case ast::BinaryOp::BitwiseOr:    return *li | *ri;
                    case ast::BinaryOp::BitwiseXor:   return *li ^ *ri;
                    case ast::BinaryOp::ShiftLeft:    return fold_shift_left(*li, *ri);
                    case ast::BinaryOp::ShiftRight:   return fold_shift_right(*li, *ri);
                    case ast::BinaryOp::Less:         return int64_t{*li < *ri ? 1 : 0};
                    case ast::BinaryOp::Greater:      return int64_t{*li > *ri ? 1 : 0};
                    case ast::BinaryOp::LessEqual:    return int64_t{*li <= *ri ? 1 : 0};
                    case ast::BinaryOp::GreaterEqual: return int64_t{*li >= *ri ? 1 : 0};
                    default:                          return std::nullopt;
                    }
                }

                if constexpr (std::is_same_v<V, std::unique_ptr<ast::CastExpr>>) {
                    return evaluate_const_value(v->value, module_path, program, diag);
                }

                if constexpr (std::is_same_v<V, std::unique_ptr<ast::OptionExpr>>) {
                    const auto mod_it = program.modules.find(module_path);
                    if (mod_it == program.modules.end()) return std::nullopt;
                    const auto it = mod_it->second.expr_option_values.find(get_expr_key(expr));
                    if (it == mod_it->second.expr_option_values.end()) return std::nullopt;
                    return it->second;
                }

                if constexpr (std::is_same_v<V, std::unique_ptr<ast::EnvExpr>>) {
                    const auto mod_it = program.modules.find(module_path);
                    if (mod_it == program.modules.end()) return std::nullopt;
                    const auto it = mod_it->second.expr_option_values.find(get_expr_key(expr));
                    if (it == mod_it->second.expr_option_values.end()) return std::nullopt;
                    return it->second;
                }

                if constexpr (std::is_same_v<V, std::unique_ptr<ast::TernaryExpr>>) {
                    // Mirrors WhenExpr below - is_constant_expr_impl already treats a
                    // ternary-of-constants as eligible (same shape as WhenExpr's check), so
                    // this fold must exist too or that eligibility check is a dead end: any
                    // constant-only context (e.g. '#link's data argument) that accepts a
                    // ternary here would hit an "internal error: could not resolve" instead
                    // of the correct diagnostic or value.
                    const auto cond = evaluate_const_value(v->condition, module_path, program, diag);
                    const auto *ci = cond ? std::get_if<int64_t>(&*cond) : nullptr;
                    if (!ci) return std::nullopt;
                    return evaluate_const_value(*ci != 0 ? v->then_expr : v->else_expr, module_path, program, diag);
                }

                if constexpr (std::is_same_v<V, std::unique_ptr<ast::WhenExpr>>) {
                    const auto cond = evaluate_const_value(v->condition, module_path, program, diag);
                    const auto *ci = cond ? std::get_if<int64_t>(&*cond) : nullptr;
                    if (!ci) return std::nullopt;
                    return evaluate_const_value(*ci != 0 ? v->then_expr : v->else_expr, module_path, program, diag);
                }

                return std::nullopt;
            },
            expr);
    }
}
