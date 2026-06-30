#include "sema.hpp"

#include <format>

namespace sema {
    namespace {
        struct LvalueInfo {
            ResolvedType type;
            bool writable = false;
        };

        auto error(DiagnosticEngine &diag, const SourceLocation &loc, std::string msg) -> ResolvedType {
            diag.report_error(DiagnosticStage::Sema, loc, std::move(msg));
            return ResolvedType{.kind = TypeKind::Invalid};
        }

        auto binary_op_result(const ast::BinaryOp op, const ResolvedType &lhs, const ResolvedType &rhs, DiagnosticEngine &diag, SourceLocation loc) -> ResolvedType {
            switch (op) {
            case ast::BinaryOp::Add:
            case ast::BinaryOp::Sub:
                if (lhs.kind == TypeKind::Anyptr && rhs.is_integer()) return lhs;
                if (rhs.kind == TypeKind::Anyptr && lhs.is_integer()) return rhs;
                [[fallthrough]];

            case ast::BinaryOp::Mul:
            case ast::BinaryOp::Div:
            case ast::BinaryOp::Mod:
            case ast::BinaryOp::BitwiseAnd:
            case ast::BinaryOp::BitwiseOr:
            case ast::BinaryOp::BitwiseXor:
            case ast::BinaryOp::ShiftLeft:
            case ast::BinaryOp::ShiftRight:
                if (lhs != rhs) {
                    return error(diag, loc, "operand type mismatch in binary expression");
                }
                return lhs;

            case ast::BinaryOp::Equal:
            case ast::BinaryOp::NotEqual:
            case ast::BinaryOp::Less:
            case ast::BinaryOp::Greater:
            case ast::BinaryOp::LessEqual:
            case ast::BinaryOp::GreaterEqual:
                if (!is_assignable(lhs, rhs) && !is_assignable(rhs, lhs)) {
                    return error(diag, loc, "operand type mismatch in comparison");
                }
                return ResolvedType{.kind = TypeKind::Bool};

            case ast::BinaryOp::LogicalAnd:
            case ast::BinaryOp::LogicalOr:
                if (lhs.kind != TypeKind::Bool || rhs.kind != TypeKind::Bool) {
                    return error(diag, loc, "&&/|| require bool operands");
                }
                return ResolvedType{.kind = TypeKind::Bool};
            }

            return ResolvedType{.kind = TypeKind::Invalid};
        }

        auto is_cast_legal(const ResolvedType &from, const ResolvedType &to) -> bool {
            return from.is_scalar() && to.is_scalar();
        }

        auto check_call_args(const std::vector<ast::Expr> &args, const std::vector<ResolvedType> &params, LocalScope &locals, const std::string &module_path, ProgramResult &program, DiagnosticEngine &diag, const SourceLocation &loc, const std::string &callee_desc, const int loop_depth) -> bool {
            if (args.size() != params.size()) {
                error(diag, loc, std::format("'{}' expects {} argument(s), got {}", callee_desc, params.size(), args.size()));
                return false;
            }

            bool ok = true;
            for (size_t i = 0; i < args.size(); ++i) {
                if (auto arg_ty = check_expr(args[i], locals, module_path, program, diag, params[i], loop_depth); !is_assignable(arg_ty, params[i])) {
                    error(diag, loc, std::format("'{}' argument {} type mismatch", callee_desc, i + 1));
                    ok = false;
                }
            }
            return ok;
        }

        auto try_resolve_namespace_chain(const ast::Expr &expr, const std::string &module_path, LocalScope &locals, ProgramResult &program) -> std::optional<std::string> {
            if (auto *ident = std::get_if<ast::IdentExpr>(&expr)) {
                if (locals.contains(ident->name)) {
                    return std::nullopt;
                }
                const auto mod_it = program.modules.find(module_path);
                if (mod_it == program.modules.end()) {
                    return std::nullopt;
                }
                const auto sym_it = mod_it->second.symbols.find(ident->name);
                if (sym_it == mod_it->second.symbols.end()) {
                    return std::nullopt;
                }
                if (auto *imp = std::get_if<ImportSymbol>(&sym_it->second)) {
                    return imp->module_path;
                }
                return std::nullopt;
            }

            if (auto *mem = std::get_if<std::unique_ptr<ast::MemberExpr>>(&expr)) {
                const auto inner_module = try_resolve_namespace_chain((*mem)->object, module_path, locals, program);
                if (!inner_module) {
                    return std::nullopt;
                }
                const auto mod_it = program.modules.find(*inner_module);
                if (mod_it == program.modules.end()) {
                    return std::nullopt;
                }
                const auto sym_it = mod_it->second.symbols.find((*mem)->member);
                if (sym_it == mod_it->second.symbols.end()) {
                    return std::nullopt;
                }
                if (auto *imp = std::get_if<ImportSymbol>(&sym_it->second)) {
                    if (!imp->is_pub) {
                        return std::nullopt;
                    }
                    return imp->module_path;
                }
                return std::nullopt;
            }
            return std::nullopt;
        }

        auto check_member_cross_module(const ast::MemberExpr &m, const std::string &target_module_path, ProgramResult &program, DiagnosticEngine &diag) -> LvalueInfo {
            const auto mod_it = program.modules.find(target_module_path);
            if (mod_it == program.modules.end()) {
                return {ResolvedType{.kind = TypeKind::Invalid}, false};
            }

            const auto sym_it = mod_it->second.symbols.find(m.member);
            if (sym_it == mod_it->second.symbols.end()) {
                error(diag, m.location, std::format("no member named '{}'", m.member));
                return {ResolvedType{.kind = TypeKind::Invalid}, false};
            }

            return std::visit(
                [&]<typename T>(const T &sym) -> LvalueInfo {
                    using S = std::decay_t<T>;
                    if constexpr (std::is_same_v<S, GlobalSymbol>) {
                        if (!sym.is_pub) {
                            error(diag, m.location, std::format("'{}' is not pub", m.member));
                            return {ResolvedType{.kind = TypeKind::Invalid}, false};
                        }
                        const auto ty = resolve_global_symbol(target_module_path, m.member, program, diag, m.location);
                        return {ty, sym.is_mut};
                    } else {
                        error(diag, m.location, std::format("'{}' is not a value", m.member));
                        return {ResolvedType{.kind = TypeKind::Invalid}, false};
                    }
                },
                sym_it->second);
        }

        auto resolve_lvalue(const ast::Expr &expr, LocalScope &locals, const std::string &module_path, ProgramResult &program, DiagnosticEngine &diag, int loop_depth) -> LvalueInfo;

        auto resolve_member(const ast::MemberExpr &m, LocalScope &locals, const std::string &module_path, ProgramResult &program, DiagnosticEngine &diag, const int loop_depth) -> LvalueInfo {
            if (const auto target_module = try_resolve_namespace_chain(m.object, module_path, locals, program)) {
                return check_member_cross_module(m, *target_module, program, diag);
            }

            const auto object_type = check_expr(m.object, locals, module_path, program, diag, std::nullopt, loop_depth);

            ResolvedType effective_type;
            bool writable;

            if (object_type.kind == TypeKind::Pointer) {
                const auto &mod = program.modules.at(module_path);
                effective_type = mod.pointer_pointees[object_type.pointee_index];
                writable = true;
            } else if (object_type.kind == TypeKind::Struct) {
                effective_type = object_type;
                const auto object_lvalue = resolve_lvalue(m.object, locals, module_path, program, diag, loop_depth);
                writable = object_lvalue.type == object_type && object_lvalue.writable;
            } else if (object_type.kind == TypeKind::Invalid) {
                return {ResolvedType{.kind = TypeKind::Invalid}, false};
            } else {
                error(diag, m.location, "'.' requires a struct or pointer-to-struct value");
                return {ResolvedType{.kind = TypeKind::Invalid}, false};
            }

            const auto &mod = program.modules.at(module_path);
            for (const auto &info = mod.structs[effective_type.struct_index]; auto &field : info.fields) {
                if (field.name == m.member) {
                    return {field.type, writable};
                }
            }

            error(diag, m.location, std::format("no field named '{}'", m.member));
            return {ResolvedType{.kind = TypeKind::Invalid}, false};
        }

        auto resolve_lvalue(const ast::Expr &expr, LocalScope &locals, const std::string &module_path, ProgramResult &program, DiagnosticEngine &diag, const int loop_depth) -> LvalueInfo {
            return std::visit(
                [&]<typename T>(const T &v) -> LvalueInfo {
                    using V = std::decay_t<T>;

                    if constexpr (std::is_same_v<V, ast::IdentExpr>) {
                        if (auto it = locals.find(v.name); it != locals.end()) {
                            return {it->second.type, it->second.is_mut};
                        }
                        const auto mod_it = program.modules.find(module_path);
                        if (mod_it != program.modules.end()) {
                            if (auto sym_it = mod_it->second.symbols.find(v.name); sym_it != mod_it->second.symbols.end()) {
                                if (auto *g = std::get_if<GlobalSymbol>(&sym_it->second)) {
                                    const ResolvedType ty = resolve_global_symbol(module_path, v.name, program, diag, v.location);
                                    return {ty, g->is_mut};
                                }
                                error(diag, v.location, std::format("cannot assign to '{}': not a variable", v.name));
                                return {ResolvedType{.kind = TypeKind::Invalid}, false};
                            }
                        }
                        error(diag, v.location, std::format("unknown identifier '{}'", v.name));
                        return {ResolvedType{.kind = TypeKind::Invalid}, false};

                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::UnaryExpr>>) {
                        if (v->op != ast::UnaryOp::Deref) {
                            error(diag, v->location, "not an assignable expression");
                            return {ResolvedType{.kind = TypeKind::Invalid}, false};
                        }
                        const ResolvedType ptr_ty = check_expr(v->operand, locals, module_path, program, diag, std::nullopt, loop_depth);
                        if (ptr_ty.kind != TypeKind::Pointer) {
                            error(diag, v->location, "cannot dereference a non-pointer value");
                            return {ResolvedType{.kind = TypeKind::Invalid}, false};
                        }
                        return {program.modules.at(module_path).pointer_pointees[ptr_ty.pointee_index], true};

                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::MemberExpr>>) {
                        return resolve_member(*v, locals, module_path, program, diag, loop_depth);

                    } else {
                        error(diag, SourceLocation{}, "not an assignable expression");
                        return {ResolvedType{.kind = TypeKind::Invalid}, false};
                    }
                },
                expr);
        }
    }

    auto check_expr(const ast::Expr &expr, LocalScope &locals, const std::string &module_path, ProgramResult &program, DiagnosticEngine &diag, const std::optional<ResolvedType> expected, const int loop_depth) -> ResolvedType {
        const auto ty = std::visit(
            [&]<typename T>(const T &v) -> ResolvedType {
                using V = std::decay_t<T>;

                if constexpr (std::is_same_v<V, ast::LiteralIntegerExpr>) {
                    if (expected && expected->is_integer()) return *expected;
                    return ResolvedType{.kind = TypeKind::I32};

                } else if constexpr (std::is_same_v<V, ast::LiteralFloatExpr>) {
                    if (expected && expected->is_float()) return *expected;
                    return ResolvedType{.kind = TypeKind::F64};

                } else if constexpr (std::is_same_v<V, ast::LiteralStringExpr>) {
                    return intern_pointer(program.modules.at(module_path), ResolvedType{.kind = TypeKind::U8});

                } else if constexpr (std::is_same_v<V, ast::LiteralBoolExpr>) {
                    return ResolvedType{.kind = TypeKind::Bool};

                } else if constexpr (std::is_same_v<V, ast::LiteralNilExpr>) {
                    return ResolvedType{.kind = TypeKind::Anyptr};

                } else if constexpr (std::is_same_v<V, ast::IdentExpr>) {
                    if (auto it = locals.find(v.name); it != locals.end()) return it->second.type;

                    const auto mod_it = program.modules.find(module_path);
                    if (mod_it == program.modules.end()) {
                        return error(diag, v.location, std::format("internal error: module '{}' not found", module_path));
                    }

                    auto sym_it = mod_it->second.symbols.find(v.name);
                    if (sym_it == mod_it->second.symbols.end()) {
                        return error(diag, v.location, std::format("unknown identifier '{}'", v.name));
                    }

                    return std::visit([&](const auto &sym) -> ResolvedType {
                        using S = std::decay_t<decltype(sym)>;
                        if constexpr (std::is_same_v<S, GlobalSymbol>) {
                            return resolve_global_symbol(module_path, v.name, program, diag, v.location);
                        } else if constexpr (std::is_same_v<S, ImportSymbol>) {
                            return ResolvedType{.kind = TypeKind::Namespace};
                        } else if constexpr (std::is_same_v<S, FunctionSymbol>) {
                            return error(diag, v.location, std::format("'{}' is a function; did you mean to call it?", v.name));
                        } else if constexpr (std::is_same_v<S, ExtFunctionSymbol>) {
                            return error(diag, v.location, std::format("'{}' is an external function; did you mean to call it?", v.name));
                        } else if constexpr (std::is_same_v<S, MacroSymbol>) {
                            return error(diag, v.location, std::format("'{}' is a macro; did you mean to call it?", v.name));
                        } else {
                            return error(diag, v.location, std::format("'{}' is a type, not a value", v.name));
                        }
                    },
                                      sym_it->second);

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::UnaryExpr>>) {
                    switch (v->op) {
                    case ast::UnaryOp::Negate:
                        {
                            const ResolvedType operand = check_expr(v->operand, locals, module_path, program, diag, expected, loop_depth);
                            if (!operand.is_integer() && !operand.is_float()) {
                                return error(diag, v->location, "unary '-' requires a numeric operand");
                            }
                            return operand;
                        }
                    case ast::UnaryOp::LogicalNot:
                        check_expr(v->operand, locals, module_path, program, diag, ResolvedType{.kind = TypeKind::Bool}, loop_depth);
                        return ResolvedType{.kind = TypeKind::Bool};
                    case ast::UnaryOp::BitwiseNot:
                        {
                            const ResolvedType operand = check_expr(v->operand, locals, module_path, program, diag, std::nullopt, loop_depth);
                            if (!operand.is_integer()) return error(diag, v->location, "unary '~' requires an integer operand");
                            return operand;
                        }
                    case ast::UnaryOp::AddressOf:
                        {
                            const LvalueInfo lv = resolve_lvalue(v->operand, locals, module_path, program, diag, loop_depth);
                            return intern_pointer(program.modules.at(module_path), lv.type);
                        }
                    case ast::UnaryOp::Deref:
                        {
                            const ResolvedType operand = check_expr(v->operand, locals, module_path, program, diag, std::nullopt, loop_depth);
                            if (operand.kind != TypeKind::Pointer) return error(diag, v->location, "cannot dereference a non-pointer value");
                            return program.modules.at(module_path).pointer_pointees[operand.pointee_index];
                        }
                    }
                    return ResolvedType{.kind = TypeKind::Invalid};

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::BinaryExpr>>) {
                    ResolvedType lhs = check_expr(v->lhs, locals, module_path, program, diag, std::nullopt, loop_depth);
                    const ResolvedType rhs = check_expr(v->rhs, locals, module_path, program, diag, lhs, loop_depth);
                    return binary_op_result(v->op, lhs, rhs, diag, v->location);

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::TernaryExpr>>) {
                    check_expr(v->condition, locals, module_path, program, diag, ResolvedType{.kind = TypeKind::Bool}, loop_depth);
                    ResolvedType then_ty = check_expr(v->then_expr, locals, module_path, program, diag, expected, loop_depth);
                    const ResolvedType else_ty = check_expr(v->else_expr, locals, module_path, program, diag, then_ty, loop_depth);
                    if (then_ty != else_ty) {
                        return error(diag, v->location, "ternary branches have different types");
                    }
                    return then_ty;

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::AssignExpr>>) {
                    LvalueInfo target = resolve_lvalue(v->target, locals, module_path, program, diag, loop_depth);
                    if (target.type.kind != TypeKind::Invalid && !target.writable) {
                        error(diag, v->location, "left-hand side of assignment is not mutable");
                    }
                    const ResolvedType value_ty = check_expr(v->value, locals, module_path, program, diag, target.type, loop_depth);
                    if (target.type.kind != TypeKind::Invalid && !is_assignable(value_ty, target.type)) {
                        error(diag, v->location, "type mismatch in assignment");
                    }
                    return target.type;

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::CallExpr>>) {
                    if (auto *member_callee = std::get_if<std::unique_ptr<ast::MemberExpr>>(&v->callee)) {
                        if (auto target_module = try_resolve_namespace_chain((*member_callee)->object, module_path, locals, program)) {
                            auto mod_it = program.modules.find(*target_module);
                            if (mod_it == program.modules.end()) {
                                return ResolvedType{.kind = TypeKind::Invalid};
                            }

                            const std::string &fn_name = (*member_callee)->member;

                            auto sym_it = mod_it->second.symbols.find(fn_name);
                            if (sym_it == mod_it->second.symbols.end()) {
                                return error(diag, v->location, std::format("unknown function '{}'", fn_name));
                            }

                            return std::visit([&](const auto &sym) -> ResolvedType {
                                using S = std::decay_t<decltype(sym)>;
                                if constexpr (std::is_same_v<S, FunctionSymbol>) {
                                    if (!sym.is_pub) return error(diag, v->location, std::format("'{}' is not pub", fn_name));
                                    check_call_args(v->args, sym.params, locals, module_path, program, diag, v->location, fn_name, loop_depth);
                                    if (sym.return_types.size() > 1) return error(diag, v->location, "multi-value capture is not yet supported here");
                                    return sym.return_types.empty() ? ResolvedType{.kind = TypeKind::Void} : sym.return_types.front();
                                } else if constexpr (std::is_same_v<S, ExtFunctionSymbol>) {
                                    if (!sym.is_pub) return error(diag, v->location, std::format("'{}' is not pub", fn_name));
                                    check_call_args(v->args, sym.params, locals, module_path, program, diag, v->location, fn_name, loop_depth);
                                    return sym.return_type.value_or(ResolvedType{.kind = TypeKind::Void});
                                } else if constexpr (std::is_same_v<S, MacroSymbol>) {
                                    if (!sym.is_pub) return error(diag, v->location, std::format("'{}' is not pub", fn_name));
                                    auto &resolved_macro = resolve_macro_symbol(*target_module, fn_name, program, diag, v->location);
                                    check_call_args(v->args, resolved_macro.params, locals, module_path, program, diag, v->location, fn_name, loop_depth);
                                    return resolved_macro.result_type;
                                } else {
                                    return error(diag, v->location, std::format("'{}' is not callable", fn_name));
                                }
                            },
                                              sym_it->second);
                        }
                        return error(diag, v->location, "method calls on struct values are not yet supported");
                    }

                    auto *callee_ident = std::get_if<ast::IdentExpr>(&v->callee);
                    if (!callee_ident) {
                        return error(diag, v->location, "unsupported call target");
                    }
                    if (locals.contains(callee_ident->name)) {
                        return error(diag, v->location, std::format("'{}' is not callable", callee_ident->name));
                    }

                    const auto mod_it = program.modules.find(module_path);
                    if (mod_it == program.modules.end()) {
                        return error(diag, v->location, std::format("internal error: module '{}' not found", module_path));
                    }
                    auto sym_it = mod_it->second.symbols.find(callee_ident->name);
                    if (sym_it == mod_it->second.symbols.end()) {
                        return error(diag, v->location, std::format("unknown function '{}'", callee_ident->name));
                    }

                    return std::visit([&](const auto &sym) -> ResolvedType {
                        using S = std::decay_t<decltype(sym)>;
                        if constexpr (std::is_same_v<S, FunctionSymbol>) {
                            check_call_args(v->args, sym.params, locals, module_path, program, diag, v->location, callee_ident->name, loop_depth);
                            if (sym.return_types.size() > 1) return error(diag, v->location, "multi-value capture is not yet supported here");
                            return sym.return_types.empty() ? ResolvedType{.kind = TypeKind::Void} : sym.return_types.front();
                        } else if constexpr (std::is_same_v<S, ExtFunctionSymbol>) {
                            check_call_args(v->args, sym.params, locals, module_path, program, diag, v->location, callee_ident->name, loop_depth);
                            return sym.return_type.value_or(ResolvedType{.kind = TypeKind::Void});
                        } else if constexpr (std::is_same_v<S, MacroSymbol>) {
                            auto &resolved_macro = resolve_macro_symbol(module_path, callee_ident->name, program, diag, v->location);
                            check_call_args(v->args, resolved_macro.params, locals, module_path, program, diag, v->location, callee_ident->name, loop_depth);
                            return resolved_macro.result_type;
                        } else {
                            return error(diag, v->location, std::format("'{}' is not callable", callee_ident->name));
                        }
                    },
                                      sym_it->second);

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::IncrDecrExpr>>) {
                    const LvalueInfo lv = resolve_lvalue(v->operand, locals, module_path, program, diag, loop_depth);
                    if (lv.type.kind != TypeKind::Invalid) {
                        if (!lv.type.is_integer() && lv.type.kind != TypeKind::Pointer && lv.type.kind != TypeKind::Anyptr) error(diag, v->location, "++ / -- requires an integer operand");
                        if (!lv.writable) error(diag, v->location, "++ / -- requires a mutable operand");
                    }
                    return lv.type;

                } else if constexpr (std::is_same_v<V, ast::ImportExpr>) {
                    return ResolvedType{.kind = TypeKind::Namespace};

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::SizeOfExpr>>) {
                    // sizeof on a (possibly qualified) TYPE name - checked
                    // first via try_resolve_namespace_chain so `sizeof(a.b.T)`
                    // resolves through arbitrarily many namespace hops, same
                    // as any other qualified type reference. Falls back to
                    // evaluating the operand as an ordinary value expression
                    // (runtime sizeof) if it isn't a type-name shape. Cannot
                    // represent a BUILTIN type keyword as the operand (e.g.
                    // sizeof(u64)) - SizeOfExpr's operand is Expr-only and
                    // builtin-type keywords never lex as Identifier - a
                    // pre-existing AST-level gap, not something fixable here.
                    if (auto *ident = std::get_if<ast::IdentExpr>(&v->operand)) {
                        const auto mod_it = program.modules.find(module_path);
                        if (mod_it != program.modules.end()) {
                            if (auto sym_it = mod_it->second.symbols.find(ident->name); sym_it != mod_it->second.symbols.end() && std::holds_alternative<TypeSymbol>(sym_it->second)) {
                                return ResolvedType{.kind = TypeKind::USize};
                            }
                        }
                    } else if (auto *mem = std::get_if<std::unique_ptr<ast::MemberExpr>>(&v->operand)) {
                        if (auto target_module = try_resolve_namespace_chain((*mem)->object, module_path, locals, program)) {
                            auto mod_it = program.modules.find(*target_module);
                            if (mod_it != program.modules.end()) {
                                if (auto sym_it = mod_it->second.symbols.find((*mem)->member); sym_it != mod_it->second.symbols.end() && std::holds_alternative<TypeSymbol>(sym_it->second)) {
                                    return ResolvedType{.kind = TypeKind::USize};
                                }
                            }
                        }
                    }
                    check_expr(v->operand, locals, module_path, program, diag, std::nullopt, loop_depth);
                    return ResolvedType{.kind = TypeKind::USize};

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::CastExpr>>) {
                    // cast(expr, Type) - value first, target type second.
                    const ResolvedType from = check_expr(v->value, locals, module_path, program, diag, std::nullopt, loop_depth);
                    const ResolvedType to = resolve_type(v->as_type, module_path, program, diag);
                    if (from.kind != TypeKind::Invalid && to.kind != TypeKind::Invalid && !is_cast_legal(from, to)) {
                        return error(diag, v->location, "illegal cast between these types");
                    }
                    return to;

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::MemberExpr>>) {
                    return resolve_member(*v, locals, module_path, program, diag, loop_depth).type;
                }
            },
            expr);

        program.modules.at(module_path).expr_types[get_expr_key(expr)] = ty;
        return ty;
    }

    auto check_stmt(const ast::Stmt &stmt, LocalScope &locals, const std::string &module_path, ProgramResult &program, DiagnosticEngine &diag, const std::vector<ResolvedType> &expected_returns, int loop_depth) -> void {
        std::visit(
            [&]<typename T>(const T &v) {
                using V = std::decay_t<T>;

                if constexpr (std::is_same_v<V, std::unique_ptr<ast::BlockStmt>>) {
                    auto inner = locals;
                    for (auto &s : v->stmts)
                        check_stmt(s, inner, module_path, program, diag, expected_returns, loop_depth);

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::IfStmt>>) {
                    check_expr(v->condition, locals, module_path, program, diag, ResolvedType{.kind = TypeKind::Bool}, loop_depth);
                    check_stmt(v->then_stmt, locals, module_path, program, diag, expected_returns, loop_depth);
                    if (v->else_stmt) check_stmt(*v->else_stmt, locals, module_path, program, diag, expected_returns, loop_depth);

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::WhileStmt>>) {
                    check_expr(v->condition, locals, module_path, program, diag, ResolvedType{.kind = TypeKind::Bool}, loop_depth);
                    check_stmt(v->body, locals, module_path, program, diag, expected_returns, loop_depth + 1);

                } else if constexpr (std::is_same_v<V, ast::ExprStmt>) {
                    check_expr(v.expr, locals, module_path, program, diag, std::nullopt, loop_depth);

                } else if constexpr (std::is_same_v<V, ast::VarDeclStmt>) {
                    ResolvedType declared_ty{.kind = TypeKind::Void};
                    bool has_declared_ty = false;
                    if (v.type) {
                        declared_ty = resolve_type(*v.type, module_path, program, diag);
                        has_declared_ty = true;
                    }
                    if (v.init) {
                        auto init_ty = check_expr(*v.init, locals, module_path, program, diag,
                                                  has_declared_ty ? std::optional(declared_ty) : std::nullopt, loop_depth);
                        if (has_declared_ty && !is_assignable(init_ty, declared_ty)) {
                            diag.report_error(DiagnosticStage::Sema, v.location, "type mismatch in variable declaration");
                        }
                        locals[v.name] = LocalBinding{.type = has_declared_ty ? declared_ty : init_ty, .is_mut = v.is_mut};
                    } else {
                        if (!v.is_mut) diag.report_error(DiagnosticStage::Sema, v.location, "'const' requires an initializer");
                        if (!has_declared_ty) diag.report_error(DiagnosticStage::Sema, v.location, "cannot infer type with no initializer and no type annotation");
                        locals[v.name] = LocalBinding{.type = declared_ty, .is_mut = v.is_mut};
                    }

                } else if constexpr (std::is_same_v<V, ast::ContinueStmt>) {
                    if (loop_depth == 0) diag.report_error(DiagnosticStage::Sema, v.location, "'continue' outside of a loop");

                } else if constexpr (std::is_same_v<V, ast::ReturnStmt>) {
                    if (v.return_values.size() != expected_returns.size()) {
                        diag.report_error(DiagnosticStage::Sema, v.location,
                                          std::format("expected {} return value(s), got {}", expected_returns.size(), v.return_values.size()));
                        for (auto &val : v.return_values)
                            check_expr(val, locals, module_path, program, diag, std::nullopt, loop_depth);
                        return;
                    }
                    for (size_t i = 0; i < v.return_values.size(); ++i) {
                        auto ty = check_expr(v.return_values[i], locals, module_path, program, diag, expected_returns[i], loop_depth);
                        if (!is_assignable(ty, expected_returns[i])) {
                            diag.report_error(DiagnosticStage::Sema, v.location, std::format("return value {} type mismatch", i + 1));
                        }
                    }
                }
            },
            stmt);
    }
}
