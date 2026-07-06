#include "sema.hpp"

#include <algorithm>
#include <format>
#include <unordered_set>

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
            // Function pointers do not support arithmetic; only equality comparison is allowed
            const bool is_cmp = op == ast::BinaryOp::Equal || op == ast::BinaryOp::NotEqual ||
                                 op == ast::BinaryOp::Less || op == ast::BinaryOp::Greater ||
                                 op == ast::BinaryOp::LessEqual || op == ast::BinaryOp::GreaterEqual;
            if (!is_cmp && (lhs.kind == TypeKind::Function || rhs.kind == TypeKind::Function)) {
                return error(diag, loc, "arithmetic is not allowed on function pointer types");
            }

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

        auto contains_undefined(const ast::Expr &expr) -> bool;

        auto contains_undefined_in_braced(const ast::BracedInitializerExpr &bi) -> bool {
            return std::visit([]<typename BV>(const BV &bv) -> bool {
                using BVT = std::decay_t<BV>;
                if constexpr (std::is_same_v<BVT, ast::StructExpr>) {
                    return std::ranges::any_of(bv.fields, [](const auto &sf) { return contains_undefined(sf.expr); });
                } else if constexpr (std::is_same_v<BVT, ast::ArrayExpr>) {
                    return std::ranges::any_of(bv.values, [](const auto &val) { return contains_undefined(val); });
                } else {
                    return false;
                }
            }, bi);
        }

        auto contains_undefined(const ast::Expr &expr) -> bool {
            return std::visit([]<typename V>(const V &v) -> bool {
                using VT = std::decay_t<V>;
                if constexpr (std::is_same_v<VT, ast::UndefinedExpr>) {
                    return true;
                } else if constexpr (std::is_same_v<VT, std::unique_ptr<ast::BracedInitializerExpr>>) {
                    return contains_undefined_in_braced(*v);
                } else {
                    return false;
                }
            }, expr);
        }

        auto is_cast_legal(const ResolvedType &from, const ResolvedType &to) -> bool {
            if (to.kind == TypeKind::Slice) return from.kind == TypeKind::Pointer || from.kind == TypeKind::Anyptr || from.kind == TypeKind::Array || from.kind == TypeKind::Slice;
            // Function pointers can be cast to/from anyptr (C callback interop)
            if (from.kind == TypeKind::Function && to.kind == TypeKind::Anyptr) return true;
            if (from.kind == TypeKind::Anyptr && to.kind == TypeKind::Function) return true;
            return from.is_scalar() && to.is_scalar();
        }

        // Returns the FunctionTypeInfo for a function-kind ResolvedType.
        auto fn_sig(const ResolvedType &ty, const Program &program) -> const FunctionTypeInfo & {
            return program.fn_signatures.at(ty.fn_index);
        }

        auto slice_element_type(const ResolvedType &slice, const std::string &module_path, Program &program) -> ResolvedType {
            return program.modules.at(module_path).slices.at(slice.slice_index).element_type;
        }

        auto array_element_type(const ResolvedType &array, const std::string &module_path, Program &program) -> ResolvedType {
            return program.modules.at(module_path).arrays.at(array.array_index).element_type;
        }

        auto assignable_in_module(const ResolvedType &from, const ResolvedType &to, const std::string &module_path, Program &program) -> bool {
            if (from.kind == TypeKind::Array && to.kind == TypeKind::Slice) {
                return array_element_type(from, module_path, program) == slice_element_type(to, module_path, program);
            }
            return is_assignable(from, to);
        }

        auto slice_cast_elements_match(const ResolvedType &from, const ResolvedType &to, const std::string &module_path, Program &program) -> bool {
            if (to.kind != TypeKind::Slice) return true;
            if (from.kind == TypeKind::Array) return array_element_type(from, module_path, program) == slice_element_type(to, module_path, program);
            if (from.kind == TypeKind::Slice) return slice_element_type(from, module_path, program) == slice_element_type(to, module_path, program);
            return true;
        }

        auto check_call_args(const std::vector<ast::Expr> &args, const std::vector<ResolvedType> &params, bool is_variadic, LocalScope &locals, const std::string &module_path, Program &program, DiagnosticEngine &diag, const SourceLocation &loc, const std::string &callee_desc, int loop_depth, int defer_loop_base = -1) -> bool;
        auto try_resolve_namespace_chain(const ast::Expr &expr, const std::string &module_path, LocalScope &locals, Program &program) -> std::optional<std::string>;

        auto check_group_call_returns(const ast::CallExpr &call, LocalScope &locals, const std::string &module_path, Program &program, DiagnosticEngine &diag, const int loop_depth, const int defer_loop_base = -1) -> std::vector<ResolvedType> {
            std::string target_module = module_path;
            std::string name;
            bool check_pub = false;

            if (const auto *callee_ident = std::get_if<ast::IdentExpr>(&call.callee)) {
                if (auto local_it = locals.find(callee_ident->name); local_it != locals.end()) {
                    const auto &local_ty = local_it->second.type;
                    if (local_ty.kind == TypeKind::Function) {
                        const auto &sig = fn_sig(local_ty, program);
                        check_call_args(call.args, sig.param_types, sig.is_variadic, locals, module_path, program, diag, call.location, callee_ident->name, loop_depth, defer_loop_base);
                        return sig.return_types;
                    }
                    error(diag, call.location, std::format("'{}' is not callable", callee_ident->name));
                    return {};
                }
                name = callee_ident->name;
            } else if (const auto *member = std::get_if<std::unique_ptr<ast::MemberExpr>>(&call.callee)) {
                if (auto ns = try_resolve_namespace_chain((*member)->object, module_path, locals, program)) {
                    target_module = *ns;
                    name = (*member)->member;
                    check_pub = true;
                } else {
                    // Method call on a value
                    auto receiver_type = check_expr((*member)->object, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base);
                    if (receiver_type.kind == TypeKind::Pointer) {
                        receiver_type = program.modules.at(module_path).pointer_pointees[receiver_type.pointee_index];
                    }
                    const auto *method = find_method(receiver_type, (*member)->member, program);
                    if (!method) {
                        // Struct field with function type
                        if (receiver_type.kind == TypeKind::Struct) {
                            for (const auto &field : program.structs[receiver_type.struct_index].fields) {
                                if (field.name == (*member)->member && field.type.kind == TypeKind::Function) {
                                    const auto &sig = fn_sig(field.type, program);
                                    check_call_args(call.args, sig.param_types, sig.is_variadic, locals, module_path, program, diag, call.location, (*member)->member, loop_depth, defer_loop_base);
                                    return sig.return_types;
                                }
                            }
                        }
                        error(diag, call.location, std::format("no method '{}' on type", (*member)->member));
                        return {};
                    }
                    check_call_args(call.args, method->param_types, false, locals, module_path, program, diag, call.location, (*member)->member, loop_depth, defer_loop_base);
                    return method->return_types;
                }
            } else {
                // General expression callee (e.g. deref of fn ptr, indexed fn ptr array)
                const auto callee_ty = check_expr(call.callee, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base);
                if (callee_ty.kind == TypeKind::Function) {
                    const auto &sig = fn_sig(callee_ty, program);
                    check_call_args(call.args, sig.param_types, sig.is_variadic, locals, module_path, program, diag, call.location, "<fn ptr>", loop_depth, defer_loop_base);
                    return sig.return_types;
                }
                error(diag, call.location, "unsupported call target");
                return {};
            }

            const auto mod_it = program.modules.find(target_module);
            if (mod_it == program.modules.end()) {
                error(diag, call.location, std::format("internal error: module '{}' not found", target_module));
                return {};
            }

            const auto sym_it = mod_it->second.symbols.find(name);
            if (sym_it == mod_it->second.symbols.end()) {
                error(diag, call.location, std::format("unknown function '{}'", name));
                return {};
            }

            return std::visit(
                [&]<typename T>(const T &sym) -> std::vector<ResolvedType> {
                    using S = std::decay_t<T>;
                    if constexpr (std::is_same_v<S, FunctionSymbol>) {
                        if (check_pub && !sym.is_pub) {
                            error(diag, call.location, std::format("'{}' is not pub", name));
                            return {};
                        }
                        check_call_args(call.args, sym.params, false, locals, module_path, program, diag, call.location, name, loop_depth, defer_loop_base);
                        return sym.return_types;
                    } else if constexpr (std::is_same_v<S, ExtFunctionSymbol>) {
                        if (check_pub && !sym.is_pub) {
                            error(diag, call.location, std::format("'{}' is not pub", name));
                            return {};
                        }
                        check_call_args(call.args, sym.params, sym.is_variadic, locals, module_path, program, diag, call.location, name, loop_depth, defer_loop_base);
                        std::vector<ResolvedType> returns;
                        if (sym.return_type) returns.push_back(*sym.return_type);
                        return returns;
                    } else {
                        error(diag, call.location, std::format("'{}' is not callable", name));
                        return {};
                    }
                },
                sym_it->second);
        }

        static auto is_valid_variadic_arg(const ResolvedType &ty) -> bool {
            switch (ty.kind) {
            case TypeKind::I32: case TypeKind::U32:
            case TypeKind::I64: case TypeKind::U64:
            case TypeKind::USize: case TypeKind::Error:
            case TypeKind::F64:
            case TypeKind::Pointer: case TypeKind::Anyptr:
                return true;
            default:
                return false;
            }
        }

        auto check_call_args(const std::vector<ast::Expr> &args, const std::vector<ResolvedType> &params, const bool is_variadic, LocalScope &locals, const std::string &module_path, Program &program, DiagnosticEngine &diag, const SourceLocation &loc, const std::string &callee_desc, const int loop_depth, const int defer_loop_base) -> bool {
            if (is_variadic) {
                if (args.size() < params.size()) {
                    error(diag, loc, std::format("'{}' expects at least {} argument(s), got {}", callee_desc, params.size(), args.size()));
                    return false;
                }
            } else {
                if (args.size() != params.size()) {
                    error(diag, loc, std::format("'{}' expects {} argument(s), got {}", callee_desc, params.size(), args.size()));
                    return false;
                }
            }

            bool ok = true;
            for (size_t i = 0; i < params.size(); ++i) {
                if (auto arg_ty = check_expr(args[i], locals, module_path, program, diag, params[i], loop_depth, defer_loop_base); !assignable_in_module(arg_ty, params[i], module_path, program)) {
                    error(diag, loc, std::format("'{}' argument {} type mismatch", callee_desc, i + 1));
                    ok = false;
                }
            }
            if (is_variadic) {
                for (size_t i = params.size(); i < args.size(); ++i) {
                    const auto arg_ty = check_expr(args[i], locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base);
                    if (!is_valid_variadic_arg(arg_ty)) {
                        error(diag, loc, std::format(
                            "'{}' variadic argument {} has a type that violates C default argument promotions: "
                            "variadic arguments must be at least 32 bits wide and floats must be f64; use cast()",
                            callee_desc, i + 1));
                        ok = false;
                    }
                }
            }
            return ok;
        }

        auto try_resolve_namespace_chain(const ast::Expr &expr, const std::string &module_path, LocalScope &locals, Program &program) -> std::optional<std::string> {
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

        // Resolves an expression as a type reference (not a value).
        // Returns the ResolvedType if expr names a type (locally or via module chain), nullopt otherwise.
        auto try_resolve_type_chain(const ast::Expr &expr, const std::string &module_path, LocalScope &locals, Program &program) -> std::optional<ResolvedType> {
            if (const auto *ident = std::get_if<ast::IdentExpr>(&expr)) {
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
                if (const auto *ts = std::get_if<TypeSymbol>(&sym_it->second)) {
                    return ts->resolved;
                }
                return std::nullopt;
            }
            if (const auto *mem = std::get_if<std::unique_ptr<ast::MemberExpr>>(&expr)) {
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
                if (const auto *ts = std::get_if<TypeSymbol>(&sym_it->second)) {
                    return ts->resolved;
                }
                return std::nullopt;
            }
            return std::nullopt;
        }

        auto check_member_cross_module(const ast::MemberExpr &m, const std::string &target_module_path, Program &program, DiagnosticEngine &diag) -> LvalueInfo {
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

        auto resolve_lvalue(const ast::Expr &expr, LocalScope &locals, const std::string &module_path, Program &program, DiagnosticEngine &diag, int loop_depth, int defer_loop_base = -1) -> LvalueInfo;

        auto resolve_member(const ast::MemberExpr &m, LocalScope &locals, const std::string &module_path, Program &program, DiagnosticEngine &diag, const int loop_depth, const int defer_loop_base = -1) -> LvalueInfo {
            if (const auto target_module = try_resolve_namespace_chain(m.object, module_path, locals, program)) {
                return check_member_cross_module(m, *target_module, program, diag);
            }

            // Handle fully-qualified enum field: e.g. EnumType.field or module.EnumType.field
            if (const auto type_ref = try_resolve_type_chain(m.object, module_path, locals, program)) {
                if (type_ref->kind == TypeKind::Enum) {
                    const auto &enum_info = program.enums[type_ref->enum_index];
                    for (const auto &field : enum_info.fields) {
                        if (field.name == m.member) {
                            return {*type_ref, false};
                        }
                    }
                    error(diag, m.location, std::format("no enum field named '{}'", m.member));
                    return {ResolvedType{.kind = TypeKind::Invalid}, false};
                }
            }

            const auto object_type = check_expr(m.object, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base);

            ResolvedType effective_type;
            bool writable;

            if (object_type.kind == TypeKind::Pointer) {
                const auto &mod = program.modules.at(module_path);
                effective_type = mod.pointer_pointees[object_type.pointee_index];
                writable = true;
            } else if (object_type.kind == TypeKind::Struct || object_type.kind == TypeKind::Union) {
                effective_type = object_type;
                const auto object_lvalue = resolve_lvalue(m.object, locals, module_path, program, diag, loop_depth, defer_loop_base);
                writable = object_lvalue.type == object_type && object_lvalue.writable;
            } else if (object_type.kind == TypeKind::Invalid) {
                return {ResolvedType{.kind = TypeKind::Invalid}, false};
            } else {
                error(diag, m.location, "'.' requires a struct, union, or pointer-to-struct/union value");
                return {ResolvedType{.kind = TypeKind::Invalid}, false};
            }

            if (effective_type.kind == TypeKind::Struct) {
                for (const auto &info = program.structs[effective_type.struct_index]; auto &field : info.fields) {
                    if (field.name == m.member) {
                        return {field.type, writable};
                    }
                }
                error(diag, m.location, std::format("no field named '{}'", m.member));
                return {ResolvedType{.kind = TypeKind::Invalid}, false};
            }

            // TypeKind::Union
            {
                const auto &info = program.unions[effective_type.union_index];
                if (info.is_tagged) {
                    error(diag, m.location, "cannot access tagged union variants directly; use 'match' to destructure");
                    return {ResolvedType{.kind = TypeKind::Invalid}, false};
                }
                for (auto &member : info.members) {
                    if (member.name == m.member) {
                        return {member.type, writable};
                    }
                }
            }

            error(diag, m.location, std::format("no member named '{}'", m.member));
            return {ResolvedType{.kind = TypeKind::Invalid}, false};
        }

        auto resolve_lvalue(const ast::Expr &expr, LocalScope &locals, const std::string &module_path, Program &program, DiagnosticEngine &diag, const int loop_depth, const int defer_loop_base) -> LvalueInfo {
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
                        const ResolvedType ptr_ty = check_expr(v->operand, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base);
                        if (ptr_ty.kind != TypeKind::Pointer) {
                            error(diag, v->location, "cannot dereference a non-pointer value");
                            return {ResolvedType{.kind = TypeKind::Invalid}, false};
                        }
                        return {program.modules.at(module_path).pointer_pointees[ptr_ty.pointee_index], true};

                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::MemberExpr>>) {
                        return resolve_member(*v, locals, module_path, program, diag, loop_depth, defer_loop_base);

                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::IndexExpr>>) {
                        const auto operand = check_expr(v->operand, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base);
                        const auto index = check_expr(v->index, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base);
                        if (!index.is_integer()) {
                            error(diag, v->location, "index must be an integer expression");
                        }
                        if (operand.kind == TypeKind::Pointer) {
                            return {program.modules.at(module_path).pointer_pointees.at(operand.pointee_index), true};
                        }
                        if (operand.kind == TypeKind::Array) {
                            auto owner = resolve_lvalue(v->operand, locals, module_path, program, diag, loop_depth, defer_loop_base);
                            return {array_element_type(operand, module_path, program), owner.writable};
                        }
                        if (operand.kind == TypeKind::Slice) {
                            return {slice_element_type(operand, module_path, program), true};
                        }
                        error(diag, v->location, "indexing requires a pointer, array, or slice operand");
                        return {ResolvedType{.kind = TypeKind::Invalid}, false};

                    } else {
                        error(diag, SourceLocation{}, "not an assignable expression");
                        return {ResolvedType{.kind = TypeKind::Invalid}, false};
                    }
                },
                expr);
        }
    }

    auto check_expr(const ast::Expr &expr, LocalScope &locals, const std::string &module_path, Program &program, DiagnosticEngine &diag, const std::optional<ResolvedType> expected, const int loop_depth, const int defer_loop_base) -> ResolvedType {
        const auto ty = std::visit(
            [&]<typename T0>(const T0 &v) -> ResolvedType {
                using V = std::decay_t<T0>;

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

                    return std::visit(
                        [&]<typename T1>(const T1 &sym) -> ResolvedType {
                            using S = std::decay_t<T1>;
                            if constexpr (std::is_same_v<S, GlobalSymbol>) {
                                return resolve_global_symbol(module_path, v.name, program, diag, v.location);
                            } else if constexpr (std::is_same_v<S, ImportSymbol>) {
                                return ResolvedType{.kind = TypeKind::Namespace};
                            } else if constexpr (std::is_same_v<S, FunctionSymbol>) {
                                // Allow taking address when expected type is a matching function type
                                if (expected && expected->kind == TypeKind::Function) {
                                    const auto &exp_sig = fn_sig(*expected, program);
                                    if (sym.params == exp_sig.param_types &&
                                        sym.return_types == exp_sig.return_types &&
                                        !exp_sig.is_variadic) {
                                        return *expected;
                                    }
                                    return error(diag, v.location, std::format("'{}' has a different signature from the expected function type", v.name));
                                }
                                return error(diag, v.location, std::format("'{}' is a function; did you mean to call it?", v.name));
                            } else if constexpr (std::is_same_v<S, ExtFunctionSymbol>) {
                                // Allow taking address when expected type is a matching function type
                                if (expected && expected->kind == TypeKind::Function) {
                                    const auto &exp_sig = fn_sig(*expected, program);
                                    std::vector<ResolvedType> ext_returns;
                                    if (sym.return_type) ext_returns.push_back(*sym.return_type);
                                    if (sym.params == exp_sig.param_types &&
                                        ext_returns == exp_sig.return_types &&
                                        sym.is_variadic == exp_sig.is_variadic) {
                                        return *expected;
                                    }
                                    return error(diag, v.location, std::format("'{}' has a different signature from the expected function type", v.name));
                                }
                                return error(diag, v.location, std::format("'{}' is an external function; did you mean to call it?", v.name));
                            } else if constexpr (std::is_same_v<S, MacroSymbol>) {
                                if (expected && expected->kind == TypeKind::Function) {
                                    return error(diag, v.location, std::format("cannot take the address of macro '{}'", v.name));
                                }
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
                            const ResolvedType operand = check_expr(v->operand, locals, module_path, program, diag, expected, loop_depth, defer_loop_base);
                            if (!operand.is_integer() && !operand.is_float()) {
                                return error(diag, v->location, "unary '-' requires a numeric operand");
                            }
                            return operand;
                        }
                    case ast::UnaryOp::LogicalNot:
                        check_expr(v->operand, locals, module_path, program, diag, ResolvedType{.kind = TypeKind::Bool}, loop_depth, defer_loop_base);
                        return ResolvedType{.kind = TypeKind::Bool};
                    case ast::UnaryOp::BitwiseNot:
                        {
                            const ResolvedType operand = check_expr(v->operand, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base);
                            if (!operand.is_integer()) return error(diag, v->location, "unary '~' requires an integer operand");
                            return operand;
                        }
                    case ast::UnaryOp::AddressOf:
                        {
                            const LvalueInfo lv = resolve_lvalue(v->operand, locals, module_path, program, diag, loop_depth, defer_loop_base);
                            return intern_pointer(program.modules.at(module_path), lv.type);
                        }
                    case ast::UnaryOp::Deref:
                        {
                            const ResolvedType operand = check_expr(v->operand, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base);
                            if (operand.kind != TypeKind::Pointer) return error(diag, v->location, "cannot dereference a non-pointer value");
                            return program.modules.at(module_path).pointer_pointees[operand.pointee_index];
                        }
                    }
                    return ResolvedType{.kind = TypeKind::Invalid};

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::BinaryExpr>>) {
                    ResolvedType lhs = check_expr(v->lhs, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base);
                    const ResolvedType rhs = check_expr(v->rhs, locals, module_path, program, diag, lhs, loop_depth, defer_loop_base);
                    return binary_op_result(v->op, lhs, rhs, diag, v->location);

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::TernaryExpr>>) {
                    check_expr(v->condition, locals, module_path, program, diag, ResolvedType{.kind = TypeKind::Bool}, loop_depth, defer_loop_base);
                    ResolvedType then_ty = check_expr(v->then_expr, locals, module_path, program, diag, expected, loop_depth, defer_loop_base);
                    const ResolvedType else_ty = check_expr(v->else_expr, locals, module_path, program, diag, then_ty, loop_depth, defer_loop_base);
                    if (then_ty != else_ty) {
                        return error(diag, v->location, "ternary branches have different types");
                    }
                    return then_ty;

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::AssignExpr>>) {
                    auto target = resolve_lvalue(v->target, locals, module_path, program, diag, loop_depth, defer_loop_base);
                    if (target.type.kind != TypeKind::Invalid && !target.writable) {
                        error(diag, v->location, "left-hand side of assignment is not mutable");
                    }

                    const auto value_ty = check_expr(v->value, locals, module_path, program, diag, target.type, loop_depth, defer_loop_base);
                    if (v->op == ast::AssignOp::Assign) {
                        if (!assignable_in_module(value_ty, target.type, module_path, program)) {
                            error(diag, v->location, "type mismatch in assignment");
                        }
                        return target.type;
                    }

                    if (!target.type.is_scalar()) {
                        error(diag, v->location, "compound assignment requires a scalar left-hand side");
                        return target.type;
                    }

                    auto equivalent_op = ast::BinaryOp::Add;

                    switch (v->op) {
                    case ast::AssignOp::AddAssign: equivalent_op = ast::BinaryOp::Add; break;
                    case ast::AssignOp::SubAssign: equivalent_op = ast::BinaryOp::Sub; break;
                    case ast::AssignOp::MulAssign: equivalent_op = ast::BinaryOp::Mul; break;
                    case ast::AssignOp::DivAssign: equivalent_op = ast::BinaryOp::Div; break;
                    case ast::AssignOp::AndAssign: equivalent_op = ast::BinaryOp::BitwiseAnd; break;
                    case ast::AssignOp::OrAssign:  equivalent_op = ast::BinaryOp::BitwiseOr; break;
                    case ast::AssignOp::XorAssign: equivalent_op = ast::BinaryOp::BitwiseXor; break;
                    case ast::AssignOp::ShlAssign: equivalent_op = ast::BinaryOp::ShiftLeft; break;
                    case ast::AssignOp::ShrAssign: equivalent_op = ast::BinaryOp::ShiftRight; break;
                    case ast::AssignOp::Assign:    break;
                    }

                    if (auto op_result_ty = binary_op_result(equivalent_op, target.type, value_ty, diag, v->location); op_result_ty.kind != TypeKind::Invalid && !assignable_in_module(op_result_ty, target.type, module_path, program)) {
                        error(diag, v->location, "compound assignment result type does not match the left-hand side's type");
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

                            return std::visit(
                                [&]<typename T1>(const T1 &sym) -> ResolvedType {
                                    using S = std::decay_t<T1>;
                                    if constexpr (std::is_same_v<S, FunctionSymbol>) {
                                        if (!sym.is_pub) return error(diag, v->location, std::format("'{}' is not pub", fn_name));
                                        check_call_args(v->args, sym.params, false, locals, module_path, program, diag, v->location, fn_name, loop_depth, defer_loop_base);
                                        if (sym.return_types.size() > 1) {
                                            return error(diag, v->location, "multi-value capture is not yet supported here");
                                        }
                                        return sym.return_types.empty() ? ResolvedType{.kind = TypeKind::Void} : sym.return_types.front();
                                    } else if constexpr (std::is_same_v<S, ExtFunctionSymbol>) {
                                        if (!sym.is_pub) return error(diag, v->location, std::format("'{}' is not pub", fn_name));
                                        check_call_args(v->args, sym.params, sym.is_variadic, locals, module_path, program, diag, v->location, fn_name, loop_depth, defer_loop_base);
                                        return sym.return_type.value_or(ResolvedType{.kind = TypeKind::Void});
                                    } else if constexpr (std::is_same_v<S, MacroSymbol>) {
                                        if (!sym.is_pub) return error(diag, v->location, std::format("'{}' is not pub", fn_name));
                                        auto &resolved_macro = resolve_macro_symbol(*target_module, fn_name, program, diag, v->location);
                                        check_call_args(v->args, resolved_macro.params, false, locals, module_path, program, diag, v->location, fn_name, loop_depth, defer_loop_base);
                                        return resolved_macro.result_type;
                                    } else {
                                        return error(diag, v->location, std::format("'{}' is not callable", fn_name));
                                    }
                                },
                                sym_it->second);
                        }
                        // Method call on a value
                        auto receiver_type = check_expr((*member_callee)->object, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base);
                        if (receiver_type.kind == TypeKind::Pointer) {
                            receiver_type = program.modules.at(module_path).pointer_pointees[receiver_type.pointee_index];
                        }
                        const auto *method = find_method(receiver_type, (*member_callee)->member, program);
                        if (!method) {
                            // Maybe it's a struct field with function type
                            if (receiver_type.kind == TypeKind::Struct) {
                                for (const auto &field : program.structs[receiver_type.struct_index].fields) {
                                    if (field.name == (*member_callee)->member) {
                                        if (field.type.kind == TypeKind::Function) {
                                            const auto &sig = fn_sig(field.type, program);
                                            check_call_args(v->args, sig.param_types, sig.is_variadic, locals, module_path, program, diag, v->location, (*member_callee)->member, loop_depth, defer_loop_base);
                                            if (sig.return_types.size() > 1) {
                                                return error(diag, v->location, "multi-value capture is not yet supported here");
                                            }
                                            return sig.return_types.empty() ? ResolvedType{.kind = TypeKind::Void} : sig.return_types.front();
                                        }
                                        break;
                                    }
                                }
                            }
                            return error(diag, v->location, std::format("no method '{}' on type", (*member_callee)->member));
                        }
                        check_call_args(v->args, method->param_types, false, locals, module_path, program, diag, v->location, (*member_callee)->member, loop_depth, defer_loop_base);
                        return method->return_types.empty() ? ResolvedType{.kind = TypeKind::Void} : method->return_types.front();
                    }

                    // General expression callee: evaluate, then call through if it's a function type
                    if (!std::holds_alternative<ast::IdentExpr>(v->callee)) {
                        const auto callee_ty = check_expr(v->callee, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base);
                        if (callee_ty.kind == TypeKind::Function) {
                            const auto &sig = fn_sig(callee_ty, program);
                            check_call_args(v->args, sig.param_types, sig.is_variadic, locals, module_path, program, diag, v->location, "<fn ptr>", loop_depth, defer_loop_base);
                            if (sig.return_types.size() > 1) {
                                return error(diag, v->location, "multi-value capture is not yet supported here");
                            }
                            return sig.return_types.empty() ? ResolvedType{.kind = TypeKind::Void} : sig.return_types.front();
                        }
                        return error(diag, v->location, "unsupported call target");
                    }

                    auto *callee_ident = std::get_if<ast::IdentExpr>(&v->callee);

                    if (auto local_it = locals.find(callee_ident->name); local_it != locals.end()) {
                        const auto &local_ty = local_it->second.type;
                        if (local_ty.kind == TypeKind::Function) {
                            const auto &sig = fn_sig(local_ty, program);
                            check_call_args(v->args, sig.param_types, sig.is_variadic, locals, module_path, program, diag, v->location, callee_ident->name, loop_depth, defer_loop_base);
                            if (sig.return_types.size() > 1) {
                                return error(diag, v->location, "multi-value capture is not yet supported here");
                            }
                            return sig.return_types.empty() ? ResolvedType{.kind = TypeKind::Void} : sig.return_types.front();
                        }
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

                    return std::visit(
                        [&]<typename T1>(const T1 &sym) -> ResolvedType {
                            using S = std::decay_t<T1>;
                            if constexpr (std::is_same_v<S, FunctionSymbol>) {
                                check_call_args(v->args, sym.params, false, locals, module_path, program, diag, v->location, callee_ident->name, loop_depth, defer_loop_base);
                                if (sym.return_types.size() > 1) {
                                    return error(diag, v->location, "multi-value capture is not yet supported here");
                                }
                                return sym.return_types.empty() ? ResolvedType{.kind = TypeKind::Void} : sym.return_types.front();
                            } else if constexpr (std::is_same_v<S, ExtFunctionSymbol>) {
                                check_call_args(v->args, sym.params, sym.is_variadic, locals, module_path, program, diag, v->location, callee_ident->name, loop_depth, defer_loop_base);
                                return sym.return_type.value_or(ResolvedType{.kind = TypeKind::Void});
                            } else if constexpr (std::is_same_v<S, MacroSymbol>) {
                                auto &resolved_macro = resolve_macro_symbol(module_path, callee_ident->name, program, diag, v->location);
                                check_call_args(v->args, resolved_macro.params, false, locals, module_path, program, diag, v->location, callee_ident->name, loop_depth, defer_loop_base);
                                return resolved_macro.result_type;
                            } else {
                                return error(diag, v->location, std::format("'{}' is not callable", callee_ident->name));
                            }
                        },
                        sym_it->second);

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::IncrDecrExpr>>) {
                    const LvalueInfo lv = resolve_lvalue(v->operand, locals, module_path, program, diag, loop_depth, defer_loop_base);
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
                    check_expr(v->operand, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base);
                    return ResolvedType{.kind = TypeKind::USize};

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::LenExpr>>) {
                    const auto operand = check_expr(v->operand, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base);
                    if (operand.kind != TypeKind::Array && operand.kind != TypeKind::Slice) {
                        return error(diag, v->location, "len() requires an array or slice operand");
                    }
                    return ResolvedType{.kind = TypeKind::USize};

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::CastExpr>>) {
                    // cast(expr, Type) - value first, target type second.
                    const ResolvedType from = check_expr(v->value, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base);
                    const ResolvedType to = resolve_type(v->as_type, module_path, program, diag);
                    if (from.kind != TypeKind::Invalid && to.kind != TypeKind::Invalid && !is_cast_legal(from, to)) {
                        return error(diag, v->location, "illegal cast between these types");
                    }
                    if (from.kind != TypeKind::Invalid && to.kind == TypeKind::Slice && !slice_cast_elements_match(from, to, module_path, program)) {
                        return error(diag, v->location, "slice cast element type mismatch");
                    }
                    if (v->len_expr) {
                        if (to.kind != TypeKind::Slice) {
                            error(diag, v->location, "cast length is only valid when casting to a slice type");
                        }
                        const auto len_ty = check_expr(*v->len_expr, locals, module_path, program, diag, ResolvedType{.kind = TypeKind::USize}, loop_depth, defer_loop_base);
                        if (!len_ty.is_integer()) {
                            error(diag, v->location, "cast length must be an integer expression");
                        }
                    } else if (to.kind == TypeKind::Slice && from.kind != TypeKind::Array && from.kind != TypeKind::Slice) {
                        error(diag, v->location, "cast to slice from a pointer requires a length expression");
                    }
                    return to;

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::MemberExpr>>) {
                    // Cross-module function pointer taking: mod.fn_name when expected type is a function type
                    if (expected && expected->kind == TypeKind::Function) {
                        if (const auto target_mod = try_resolve_namespace_chain(v->object, module_path, locals, program)) {
                            const auto mod_it = program.modules.find(*target_mod);
                            if (mod_it != program.modules.end()) {
                                const auto sym_it = mod_it->second.symbols.find(v->member);
                                if (sym_it != mod_it->second.symbols.end()) {
                                    const auto &exp_sig = fn_sig(*expected, program);
                                    if (const auto *fn = std::get_if<FunctionSymbol>(&sym_it->second)) {
                                        if (!fn->is_pub) return error(diag, v->location, std::format("'{}' is not pub", v->member));
                                        if (fn->params == exp_sig.param_types &&
                                            fn->return_types == exp_sig.return_types &&
                                            !exp_sig.is_variadic) {
                                            return *expected;
                                        }
                                        return error(diag, v->location, std::format("'{}' has a different signature from the expected function type", v->member));
                                    }
                                    if (const auto *ef = std::get_if<ExtFunctionSymbol>(&sym_it->second)) {
                                        if (!ef->is_pub) return error(diag, v->location, std::format("'{}' is not pub", v->member));
                                        std::vector<ResolvedType> ext_returns;
                                        if (ef->return_type) ext_returns.push_back(*ef->return_type);
                                        if (ef->params == exp_sig.param_types &&
                                            ext_returns == exp_sig.return_types &&
                                            ef->is_variadic == exp_sig.is_variadic) {
                                            return *expected;
                                        }
                                        return error(diag, v->location, std::format("'{}' has a different signature from the expected function type", v->member));
                                    }
                                }
                            }
                        }
                    }
                    return resolve_member(*v, locals, module_path, program, diag, loop_depth, defer_loop_base).type;

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::IndexExpr>>) {
                    const auto operand = check_expr(v->operand, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base);
                    const auto index = check_expr(v->index, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base);
                    if (!index.is_integer()) {
                        error(diag, v->location, "index must be an integer expression");
                    }
                    if (operand.kind == TypeKind::Pointer) {
                        return program.modules.at(module_path).pointer_pointees.at(operand.pointee_index);
                    }
                    if (operand.kind == TypeKind::Array) {
                        return array_element_type(operand, module_path, program);
                    }
                    if (operand.kind == TypeKind::Slice) {
                        return slice_element_type(operand, module_path, program);
                    }
                    return error(diag, v->location, "indexing requires a pointer, array, or slice operand");

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::SliceExpr>>) {
                    const auto operand = check_expr(v->operand, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base);
                    const auto start = check_expr(v->start, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base);
                    const auto end = check_expr(v->end, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base);
                    if (!start.is_integer() || !end.is_integer()) {
                        error(diag, v->location, "slice bounds must be integer expressions");
                    }
                    if (operand.kind == TypeKind::Array) {
                        const auto element = array_element_type(operand, module_path, program);
                        return intern_slice(program.modules.at(module_path), element);
                    }
                    if (operand.kind == TypeKind::Slice) {
                        return operand;
                    }
                    return error(diag, v->location, "slicing requires an array or slice operand");

                } else if constexpr (std::is_same_v<V, ast::IotaExpr>) {
                    return error(diag, v.location, "'iota' is only valid inside enum field initializers");

                } else if constexpr (std::is_same_v<V, ast::DotIdentExpr>) {
                    // Dot-prefixed enum field literal: .field_name
                    // Requires an expected enum or tagged union type
                    if (expected && expected->kind == TypeKind::Enum) {
                        const auto &enum_info = program.enums[expected->enum_index];
                        for (const auto &field : enum_info.fields) {
                            if (field.name == v.name) {
                                return *expected;
                            }
                        }
                        return error(diag, v.location, std::format("no enum field named '{}'", v.name));
                    }
                    if (expected && expected->kind == TypeKind::Union) {
                        const auto &union_info = program.unions.at(expected->union_index);
                        if (union_info.is_tagged) {
                            const auto it = std::ranges::find(union_info.variants, v.name, &TaggedUnionVariant::name);
                            if (it == union_info.variants.end()) {
                                return error(diag, v.location, std::format("no variant '{}' on tagged union", v.name));
                            }
                            if (it->payload_struct_index >= 0) {
                                return error(diag, v.location, std::format("variant '{}' has a payload; use '.{}{{...}}' syntax", v.name, v.name));
                            }
                            return *expected;
                        }
                    }
                    return error(diag, v.location, std::format("cannot resolve '.{}' without an expected enum or tagged union type", v.name));

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::MatchExpr>>) {
                    const auto operand_type = check_expr(v->operand, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base);

                    // ---- Pre-pass: validate '_' arm placement (shared for all operand types) ----
                    std::optional<size_t> default_arm_idx;
                    for (size_t i = 0; i < v->arms.size(); ++i) {
                        if (std::holds_alternative<ast::MatchExpr::DefaultPattern>(v->arms[i].pattern)) {
                            if (default_arm_idx.has_value()) {
                                error(diag, v->arms[i].location, "duplicate default arm '_'");
                            } else if (i + 1 != v->arms.size()) {
                                error(diag, v->arms[i].location, "default arm '_' must be the last arm");
                            }
                            default_arm_idx = i;
                        }
                    }

                    // ---- Reject invalid operand types ----
                    if (operand_type.is_float()) {
                        return error(diag, v->location, "cannot match on floating-point types; use if/else chains");
                    }
                    if (operand_type.kind == TypeKind::Pointer || operand_type.kind == TypeKind::Anyptr) {
                        return error(diag, v->location, "cannot match on pointer types");
                    }

                    // ---- Scalar match (integer or bool operand) ----
                    if (operand_type.is_integer() || operand_type.kind == TypeKind::Bool) {
                        std::unordered_map<int64_t, size_t> seen_values; // evaluated value -> arm index
                        ResolvedType arm_type{.kind = TypeKind::Invalid};
                        bool first_arm = true;
                        bool true_covered = false, false_covered = false;

                        for (size_t arm_i = 0; arm_i < v->arms.size(); ++arm_i) {
                            const auto &arm = v->arms[arm_i];
                            const auto &arm_loc = arm.location;

                            if (std::holds_alternative<ast::MatchExpr::VariantPattern>(arm.pattern)) {
                                error(diag, arm_loc, "'.name' patterns require an enum or tagged union operand");
                                continue;
                            }

                            auto arm_locals = locals;

                            if (std::holds_alternative<ast::MatchExpr::DefaultPattern>(arm.pattern)) {
                                // Default arm value
                                const auto val_type = check_expr(arm.value, arm_locals, module_path, program, diag,
                                    first_arm ? std::nullopt : std::optional<ResolvedType>{arm_type}, loop_depth, defer_loop_base);
                                if (first_arm) { arm_type = val_type; first_arm = false; }
                                else if (arm_type.kind != TypeKind::Invalid && val_type != arm_type) {
                                    error(diag, arm_loc, "all match arms must have the same type");
                                }
                                continue;
                            }

                            const auto &lp = std::get<ast::MatchExpr::LiteralPattern>(arm.pattern);
                            // Pattern must be compile-time constant
                            if (!is_constant_expr(*lp.expr, module_path, program)) {
                                error(diag, arm_loc, "match arm pattern must be a compile-time constant");
                            }
                            // Type-check the pattern against the operand type
                            check_expr(*lp.expr, arm_locals, module_path, program, diag, operand_type, loop_depth, defer_loop_base);
                            // Evaluate for duplicate detection
                            const auto val = evaluate_integer_constant(*lp.expr, module_path, program);
                            if (val) {
                                if (seen_values.count(*val)) {
                                    error(diag, arm_loc, std::format("duplicate match arm: value already covered by arm {}", seen_values.at(*val) + 1));
                                } else {
                                    seen_values[*val] = arm_i;
                                    if (operand_type.kind == TypeKind::Bool) {
                                        if (*val == 0) false_covered = true;
                                        else if (*val == 1) true_covered = true;
                                    }
                                }
                            }
                            // Check arm result value
                            const auto val_type = check_expr(arm.value, arm_locals, module_path, program, diag,
                                first_arm ? std::nullopt : std::optional<ResolvedType>{arm_type}, loop_depth, defer_loop_base);
                            if (first_arm) { arm_type = val_type; first_arm = false; }
                            else if (arm_type.kind != TypeKind::Invalid && val_type != arm_type) {
                                error(diag, arm_loc, "all match arms must have the same type");
                            }
                        }

                        // Exhaustiveness
                        const bool has_default = default_arm_idx.has_value();
                        if (operand_type.kind == TypeKind::Bool) {
                            if (!has_default && !(true_covered && false_covered)) {
                                error(diag, v->location, "bool match must cover both 'true' and 'false', or have a default '_' arm");
                            }
                            if (has_default && true_covered && false_covered) {
                                error(diag, v->arms[*default_arm_idx].location, "unreachable default arm: bool match already covers both 'true' and 'false'");
                            }
                        } else {
                            if (!has_default) {
                                error(diag, v->location, "non-bool scalar match requires a default '_' arm");
                            }
                        }

                        return arm_type.kind == TypeKind::Invalid ? ResolvedType{.kind = TypeKind::Void} : arm_type;
                    }

                    // ---- Tagged union match ----
                    if (operand_type.kind == TypeKind::Union) {
                        const auto &union_info = program.unions[operand_type.union_index];
                        if (!union_info.is_tagged) {
                            return error(diag, v->location, "match operand must be an enum or tagged union type");
                        }

                        // Check if any arm uses by-ref capture; operand must be an lvalue in that case
                        const bool any_ref_capture = std::ranges::any_of(v->arms, [](const auto &a) {
                            const auto *vp = std::get_if<ast::MatchExpr::VariantPattern>(&a.pattern);
                            return vp && vp->capture_by_ref;
                        });
                        if (any_ref_capture) {
                            const auto lv = resolve_lvalue(v->operand, locals, module_path, program, diag, loop_depth, defer_loop_base);
                            if (lv.type.kind == TypeKind::Invalid) {
                                error(diag, v->location, "by-ref capture requires an lvalue match operand");
                            }
                        }

                        ResolvedType arm_type{.kind = TypeKind::Invalid};
                        bool first_arm = true;
                        std::vector<bool> covered(union_info.variants.size(), false);

                        for (const auto &arm : v->arms) {
                            if (std::holds_alternative<ast::MatchExpr::DefaultPattern>(arm.pattern)) {
                                const auto val_type = check_expr(arm.value, locals, module_path, program, diag,
                                    first_arm ? std::nullopt : std::optional<ResolvedType>{arm_type}, loop_depth, defer_loop_base);
                                if (first_arm) { arm_type = val_type; first_arm = false; }
                                else if (arm_type.kind != TypeKind::Invalid && val_type != arm_type) {
                                    error(diag, arm.location, "all match arms must have the same type");
                                }
                                continue;
                            }
                            if (!std::holds_alternative<ast::MatchExpr::VariantPattern>(arm.pattern)) {
                                error(diag, arm.location, "literal patterns require a scalar (integer/bool) operand");
                                continue;
                            }
                            const auto &vp = std::get<ast::MatchExpr::VariantPattern>(arm.pattern);

                            bool found = false;
                            for (size_t i = 0; i < union_info.variants.size(); ++i) {
                                if (union_info.variants[i].name == vp.name) {
                                    if (covered[i]) {
                                        error(diag, arm.location, std::format("duplicate match arm for variant '{}'", vp.name));
                                    }
                                    covered[i] = true;
                                    found = true;

                                    const auto &variant = union_info.variants[i];
                                    auto arm_locals = locals;

                                    if (vp.capture_name) {
                                        if (variant.payload_struct_index < 0) {
                                            error(diag, arm.location, std::format("variant '{}' has no payload; cannot capture", vp.name));
                                        } else {
                                            const ResolvedType payload_ty{.kind = TypeKind::Struct, .struct_index = variant.payload_struct_index};
                                            if (vp.capture_by_ref) {
                                                arm_locals[*vp.capture_name] = LocalBinding{
                                                    .type = intern_pointer(program.modules.at(module_path), payload_ty),
                                                    .is_mut = false,
                                                };
                                            } else {
                                                arm_locals[*vp.capture_name] = LocalBinding{.type = payload_ty, .is_mut = false};
                                            }
                                        }
                                    }

                                    const auto val_type = check_expr(arm.value, arm_locals, module_path, program, diag,
                                                                     first_arm ? std::nullopt : std::optional<ResolvedType>{arm_type}, loop_depth, defer_loop_base);
                                    if (first_arm) {
                                        arm_type = val_type;
                                        first_arm = false;
                                    } else if (arm_type.kind != TypeKind::Invalid && val_type != arm_type) {
                                        error(diag, arm.location, "all match arms must have the same type");
                                    }
                                    break;
                                }
                            }
                            if (!found) {
                                error(diag, arm.location, std::format("no variant '{}' on tagged union", vp.name));
                                const auto val_type = check_expr(arm.value, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base);
                                if (first_arm) { arm_type = val_type; first_arm = false; }
                            }
                        }

                        const bool has_default = default_arm_idx.has_value();
                        // Exhaustiveness: all variants must be covered, OR a default arm is present
                        for (size_t i = 0; i < union_info.variants.size(); ++i) {
                            if (!covered[i] && !has_default) {
                                error(diag, v->location, std::format("match is not exhaustive: missing arm for '{}'", union_info.variants[i].name));
                            }
                        }
                        // Unreachable default: _ after all variants are covered
                        if (has_default && std::ranges::all_of(covered, [](bool b) { return b; })) {
                            error(diag, v->arms[*default_arm_idx].location, "unreachable default arm: all variants are already covered");
                        }

                        return arm_type.kind == TypeKind::Invalid ? ResolvedType{.kind = TypeKind::Void} : arm_type;
                    }

                    // ---- Enum match ----
                    if (operand_type.kind != TypeKind::Enum) {
                        return error(diag, v->location, "match operand must be an enum, tagged union, integer, or bool type");
                    }

                    const auto &enum_info = program.enums[operand_type.enum_index];

                    ResolvedType arm_type{.kind = TypeKind::Invalid};
                    bool first_arm = true;
                    std::vector<bool> covered(enum_info.fields.size(), false);

                    for (const auto &arm : v->arms) {
                        if (std::holds_alternative<ast::MatchExpr::DefaultPattern>(arm.pattern)) {
                            const auto val_type = check_expr(arm.value, locals, module_path, program, diag,
                                first_arm ? std::nullopt : std::optional<ResolvedType>{arm_type}, loop_depth, defer_loop_base);
                            if (first_arm) { arm_type = val_type; first_arm = false; }
                            else if (arm_type.kind != TypeKind::Invalid && val_type != arm_type) {
                                error(diag, arm.location, "all match arms must have the same type");
                            }
                            continue;
                        }
                        if (!std::holds_alternative<ast::MatchExpr::VariantPattern>(arm.pattern)) {
                            error(diag, arm.location, "literal patterns require a scalar (integer/bool) operand");
                            continue;
                        }
                        const auto &vp = std::get<ast::MatchExpr::VariantPattern>(arm.pattern);

                        if (vp.capture_name) {
                            error(diag, arm.location, "payload capture is only valid for tagged union match arms");
                        }
                        bool found = false;
                        for (size_t i = 0; i < enum_info.fields.size(); ++i) {
                            if (enum_info.fields[i].name == vp.name) {
                                if (covered[i]) {
                                    error(diag, arm.location, std::format("duplicate match arm for enum field '{}'", vp.name));
                                }
                                covered[i] = true;
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
                            error(diag, arm.location, std::format("no enum field named '{}'", vp.name));
                        }

                        const auto val_type = check_expr(arm.value, locals, module_path, program, diag,
                            first_arm ? std::nullopt : std::optional<ResolvedType>{arm_type}, loop_depth, defer_loop_base);
                        if (first_arm) { arm_type = val_type; first_arm = false; }
                        else if (arm_type.kind != TypeKind::Invalid && val_type != arm_type) {
                            error(diag, arm.location, "all match arms must have the same type");
                        }
                    }

                    const bool has_default = default_arm_idx.has_value();
                    for (size_t i = 0; i < enum_info.fields.size(); ++i) {
                        if (!covered[i] && !has_default) {
                            error(diag, v->location, std::format("match is not exhaustive: missing arm for '{}'", enum_info.fields[i].name));
                        }
                    }
                    if (has_default && std::ranges::all_of(covered, [](bool b) { return b; })) {
                        error(diag, v->arms[*default_arm_idx].location, "unreachable default arm: all enum fields are already covered");
                    }

                    return arm_type.kind == TypeKind::Invalid ? ResolvedType{.kind = TypeKind::Void} : arm_type;

                } else if constexpr (std::is_same_v<V, ast::DefaultExpr>) {
                    if (!expected) {
                        return error(diag, v.location, "'default' requires a known target type");
                    }
                    if (expected->kind == TypeKind::Union) {
                        return error(diag, v.location, "unions have no default value; use an explicit single-member initializer or 'undefined'");
                    }
                    return *expected;
                } else if constexpr (std::is_same_v<V, ast::UndefinedExpr>) {
                    if (!expected) {
                        return error(diag, v.location, "'undefined' requires a known target type");
                    }
                    if (expected->kind == TypeKind::Union && program.unions.at(expected->union_index).is_tagged) {
                        return error(diag, v.location, "tagged unions have no 'undefined' form; use an explicit variant initializer");
                    }
                    return *expected;
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::TryExpr>>) {
                    if (defer_loop_base >= 0) {
                        return error(diag, v->location, "'try' cannot propagate errors out of a 'defer' body");
                    }
                    // The operand must be a CallExpr
                    const auto *call = std::get_if<std::unique_ptr<ast::CallExpr>>(&v->call);
                    if (!call) {
                        return error(diag, v->location, "'try' operand must be a direct function call");
                    }
                    const auto returns = check_group_call_returns(**call, locals, module_path, program, diag, loop_depth, defer_loop_base);
                    if (returns.empty()) {
                        return ResolvedType{.kind = TypeKind::Void};
                    }
                    // Last return type must be error
                    if (returns.back().kind != TypeKind::Error) {
                        return error(diag, v->location, "'try' can only be used on a function that returns 'error' as its last return value");
                    }
                    // Returns: all return types except the error slot
                    if (returns.size() == 1) {
                        // f() -> error: expression has no value (Void)
                        return ResolvedType{.kind = TypeKind::Void};
                    }
                    if (returns.size() == 2) {
                        // f() -> T, error: expression type is T
                        return returns[0];
                    }
                    // f() -> T1, T2, ..., error: multi-value; cannot be used in expression position
                    return error(diag, v->location, "multi-value 'try' cannot be used in expression position; use group declaration");
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::TaggedVariantExpr>>) {
                    // Resolve the tagged union type
                    ResolvedType union_ty;
                    if (!v->type_name.empty()) {
                        union_ty = resolve_type_symbol(module_path, v->type_name, program, diag, v->location);
                    } else if (expected && expected->kind == TypeKind::Union) {
                        union_ty = *expected;
                    } else {
                        return error(diag, v->location, "cannot infer tagged union type; provide an explicit type name (e.g. 'TypeName.variant{...}')");
                    }
                    if (union_ty.kind != TypeKind::Union) {
                        return error(diag, v->location, std::format("'{}' is not a union type", v->type_name));
                    }
                    const auto &union_info = program.unions.at(union_ty.union_index);
                    if (!union_info.is_tagged) {
                        return error(diag, v->location, "use '{member = val}' syntax for untagged unions");
                    }
                    const auto variant_it = std::ranges::find(union_info.variants, v->variant_name, &TaggedUnionVariant::name);
                    if (variant_it == union_info.variants.end()) {
                        return error(diag, v->location, std::format("no variant '{}' on tagged union", v->variant_name));
                    }
                    const bool has_payload = variant_it->payload_struct_index >= 0;
                    if (!has_payload && v->payload.has_value()) {
                        return error(diag, v->location, std::format("variant '{}' has no payload; use '.{}' without braces", v->variant_name, v->variant_name));
                    }
                    if (has_payload) {
                        if (!v->payload.has_value()) {
                            return error(diag, v->location, std::format("variant '{}' requires a payload initializer; use '.{}{{field = val}}'", v->variant_name, v->variant_name));
                        }
                        const auto &bv = *v->payload;
                        const auto &struct_info = program.structs.at(variant_it->payload_struct_index);
                        std::unordered_set<std::string> seen;
                        for (const auto &sf : bv.fields) {
                            if (!seen.insert(sf.name).second) {
                                error(diag, sf.location, std::format("duplicate field '{}' in variant initializer", sf.name));
                            }
                            const auto it = std::ranges::find(struct_info.fields, sf.name, &StructField::name);
                            if (it == struct_info.fields.end()) {
                                error(diag, sf.location, std::format("no field '{}' in variant '{}'", sf.name, v->variant_name));
                                continue;
                            }
                            const auto val_ty = check_expr(sf.expr, locals, module_path, program, diag, it->type, loop_depth, defer_loop_base);
                            if (!assignable_in_module(val_ty, it->type, module_path, program)) {
                                error(diag, sf.location, std::format("type mismatch for field '{}'", sf.name));
                            }
                        }
                        for (const auto &f : struct_info.fields) {
                            if (f.init_expr != nullptr) continue;
                            if (!std::ranges::any_of(bv.fields, [&](const auto &sf) { return sf.name == f.name; })) {
                                error(diag, v->location, std::format("missing field '{}' in variant '{}'", f.name, v->variant_name));
                            }
                        }
                    }
                    return union_ty;
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::BracedInitializerExpr>>) {
                    return std::visit(
                        [&]<typename BV>(const BV &bv) -> ResolvedType {
                            using BVT = std::decay_t<BV>;

                            if constexpr (std::is_same_v<BVT, ast::EmptyExpr>) {
                                if (!expected || (expected->kind != TypeKind::Struct && expected->kind != TypeKind::Array)) {
                                    if (expected && expected->kind == TypeKind::Union) {
                                        return error(diag, bv.location, "a union initializer must set exactly one member");
                                    }
                                    return error(diag, bv.location, "braced initializer '{}' requires a struct or array type");
                                }
                                return *expected;

                            } else if constexpr (std::is_same_v<BVT, ast::StructExpr>) {
                                if (!expected) {
                                    return error(diag, bv.location, "struct initializer requires an expected type");
                                }
                                if (expected->kind == TypeKind::Array) {
                                    return error(diag, bv.location, "struct initializer used where array type is expected");
                                }
                                if (expected->kind == TypeKind::Union) {
                                    const auto &union_info = program.unions.at(expected->union_index);
                                    if (bv.fields.size() != 1) {
                                        return error(diag, bv.location, std::format("a union initializer must set exactly one member, got {}", bv.fields.size()));
                                    }
                                    const auto &sf = bv.fields[0];
                                    const auto it = std::ranges::find(union_info.members, sf.name, &sema::UnionMember::name);
                                    if (it == union_info.members.end()) {
                                        error(diag, sf.location, std::format("no member '{}' on union", sf.name));
                                        return *expected;
                                    }
                                    const auto val_ty = check_expr(sf.expr, locals, module_path, program, diag, it->type, loop_depth, defer_loop_base);
                                    if (!assignable_in_module(val_ty, it->type, module_path, program)) {
                                        error(diag, sf.location, std::format("type mismatch for union member '{}'", sf.name));
                                    }
                                    return *expected;
                                }
                                if (expected->kind != TypeKind::Struct) {
                                    return error(diag, bv.location, "struct initializer requires a struct type");
                                }
                                const auto &info = program.structs.at(expected->struct_index);
                                // Check for unknown and duplicate field names
                                std::unordered_set<std::string> seen;
                                for (const auto &sf : bv.fields) {
                                    if (!seen.insert(sf.name).second) {
                                        error(diag, sf.location, std::format("duplicate field '{}' in struct initializer", sf.name));
                                    }
                                    const auto it = std::ranges::find(info.fields, sf.name, &sema::StructField::name);
                                    if (it == info.fields.end()) {
                                        error(diag, sf.location, std::format("no field '{}' on struct", sf.name));
                                        continue;
                                    }
                                    const auto val_ty = check_expr(sf.expr, locals, module_path, program, diag, it->type, loop_depth, defer_loop_base);
                                    if (!assignable_in_module(val_ty, it->type, module_path, program)) {
                                        error(diag, sf.location, std::format("type mismatch for field '{}'", sf.name));
                                    }
                                }
                                // Check that all fields without a default are provided
                                for (const auto &f : info.fields) {
                                    if (f.init_expr != nullptr) continue;
                                    const bool provided = std::ranges::any_of(bv.fields, [&](const auto &sf) { return sf.name == f.name; });
                                    if (!provided) {
                                        error(diag, bv.location, std::format("missing field '{}' in struct initializer", f.name));
                                    }
                                }
                                return *expected;

                            } else { // ast::ArrayExpr
                                if (!expected) {
                                    return error(diag, bv.location, "array initializer requires an expected type");
                                }
                                if (expected->kind == TypeKind::Struct) {
                                    return error(diag, bv.location, "array initializer used where struct type is expected");
                                }
                                if (expected->kind != TypeKind::Array) {
                                    return error(diag, bv.location, "array initializer requires an array type");
                                }
                                const auto &array_info = program.modules.at(module_path).arrays.at(expected->array_index);
                                if (bv.values.size() > array_info.count) {
                                    return error(diag, bv.location, std::format("too many elements in array initializer: array has {} element(s), got {}", array_info.count, bv.values.size()));
                                }
                                for (const auto &val : bv.values) {
                                    const auto val_ty = check_expr(val, locals, module_path, program, diag, array_info.element_type, loop_depth, defer_loop_base);
                                    if (!assignable_in_module(val_ty, array_info.element_type, module_path, program)) {
                                        error(diag, bv.location, "type mismatch in array initializer element");
                                    }
                                }
                                return *expected;
                            }
                        },
                        *v);
                }
            },
            expr);

        program.modules.at(module_path).expr_types[get_expr_key(expr)] = ty;
        return ty;
    }

    auto check_stmt(const ast::Stmt &stmt, LocalScope &locals, const std::string &module_path, Program &program, DiagnosticEngine &diag, const std::vector<ResolvedType> &expected_returns, int loop_depth, int defer_loop_base) -> void {
        std::visit(
            [&]<typename T>(const T &v) {
                using V = std::decay_t<T>;

                if constexpr (std::is_same_v<V, std::unique_ptr<ast::BlockStmt>>) {
                    auto inner = locals;
                    for (auto &s : v->stmts)
                        check_stmt(s, inner, module_path, program, diag, expected_returns, loop_depth, defer_loop_base);

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::IfStmt>>) {
                    check_expr(v->condition, locals, module_path, program, diag, ResolvedType{.kind = TypeKind::Bool}, loop_depth, defer_loop_base);
                    check_stmt(v->then_stmt, locals, module_path, program, diag, expected_returns, loop_depth, defer_loop_base);
                    if (v->else_stmt) check_stmt(*v->else_stmt, locals, module_path, program, diag, expected_returns, loop_depth, defer_loop_base);

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::WhileStmt>>) {
                    check_expr(v->condition, locals, module_path, program, diag, ResolvedType{.kind = TypeKind::Bool}, loop_depth, defer_loop_base);
                    check_stmt(v->body, locals, module_path, program, diag, expected_returns, loop_depth + 1, defer_loop_base);

                } else if constexpr (std::is_same_v<V, ast::ExprStmt>) {
                    const auto expr_ty = check_expr(v.expr, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base);
                    // Detect ignored errors from fallible calls
                    if (expr_ty.kind == TypeKind::Error &&
                        !std::holds_alternative<std::unique_ptr<ast::TryExpr>>(v.expr)) {
                        diag.report_error(DiagnosticStage::Sema, v.location,
                            "error from fallible function call must be captured or propagated with 'try'");
                    }
                    // If this is a TryExpr, check the enclosing function is fallible
                    if (std::holds_alternative<std::unique_ptr<ast::TryExpr>>(v.expr)) {
                        if (expected_returns.empty() || expected_returns.back().kind != TypeKind::Error) {
                            diag.report_error(DiagnosticStage::Sema, v.location,
                                "enclosing function must return 'error' to use 'try'");
                        }
                    }

                } else if constexpr (std::is_same_v<V, ast::VarDeclStmt>) {
                    ResolvedType declared_ty{.kind = TypeKind::Void};
                    bool has_declared_ty = false;
                    if (v.type) {
                        declared_ty = resolve_type(*v.type, module_path, program, diag);
                        has_declared_ty = true;
                    }
                    if (v.init) {
                        if (!v.is_mut && contains_undefined(*v.init)) {
                            diag.report_error(DiagnosticStage::Sema, v.location, "'undefined' is not allowed in a 'const' declaration");
                        }
                        auto init_ty = check_expr(*v.init, locals, module_path, program, diag,
                                                  has_declared_ty ? std::optional(declared_ty) : std::nullopt, loop_depth, defer_loop_base);
                        if (has_declared_ty && !assignable_in_module(init_ty, declared_ty, module_path, program)) {
                            diag.report_error(DiagnosticStage::Sema, v.location, "type mismatch in variable declaration");
                        }
                        locals[v.name] = LocalBinding{.type = has_declared_ty ? declared_ty : init_ty, .is_mut = v.is_mut};
                    } else {
                        if (!v.is_mut) diag.report_error(DiagnosticStage::Sema, v.location, "'const' requires an initializer");
                        if (!has_declared_ty) diag.report_error(DiagnosticStage::Sema, v.location, "cannot infer type with no initializer and no type annotation");
                        locals[v.name] = LocalBinding{.type = declared_ty, .is_mut = v.is_mut};
                    }

                } else if constexpr (std::is_same_v<V, ast::VarDeclGroupStmt>) {
                    const ast::CallExpr *call = nullptr;
                    bool is_try = false;

                    if (const auto *c = std::get_if<std::unique_ptr<ast::CallExpr>>(&v.init)) {
                        call = c->get();
                    } else if (const auto *t = std::get_if<std::unique_ptr<ast::TryExpr>>(&v.init)) {
                        is_try = true;
                        const auto *inner_call = std::get_if<std::unique_ptr<ast::CallExpr>>(&(*t)->call);
                        if (!inner_call) {
                            diag.report_error(DiagnosticStage::Sema, v.location, "'try' operand must be a direct function call");
                            return;
                        }
                        call = inner_call->get();
                    }

                    if (!call) {
                        diag.report_error(DiagnosticStage::Sema, v.location, "group declaration initializer must be a function call or 'try' expression");
                        check_expr(v.init, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base);
                        return;
                    }

                    auto returns = check_group_call_returns(*call, locals, module_path, program, diag, loop_depth, defer_loop_base);
                    if (returns.empty()) {
                        return;
                    }

                    if (is_try) {
                        // Check enclosing function is fallible
                        if (expected_returns.empty() || expected_returns.back().kind != TypeKind::Error) {
                            diag.report_error(DiagnosticStage::Sema, v.location,
                                "enclosing function must return 'error' to use 'try'");
                        }
                        // Strip the error slot from returns
                        if (returns.back().kind == TypeKind::Error) {
                            returns.pop_back();
                        } else {
                            diag.report_error(DiagnosticStage::Sema, v.location,
                                "'try' can only be used on a function that returns 'error' as its last return value");
                            return;
                        }
                        // Check that we are not inside a defer body
                        if (defer_loop_base >= 0) {
                            diag.report_error(DiagnosticStage::Sema, v.location,
                                "'try' cannot propagate errors out of a 'defer' body");
                        }
                    }

                    if (returns.size() != v.names.size()) {
                        diag.report_error(DiagnosticStage::Sema, v.location, std::format("group declaration expects {} return value(s), got {}", v.names.size(), returns.size()));
                    }

                    for (size_t i = 0; i < v.names.size() && i < returns.size(); ++i) {
                        if (!v.names[i].empty() && v.names[i] != "_") {
                            locals[v.names[i]] = LocalBinding{.type = returns[i], .is_mut = v.is_mut};
                        }
                    }

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::SwitchStmt>>) {
                    const auto operand_type = check_expr(v->operand, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base);

                    // Validate '_' placement
                    std::optional<size_t> default_arm_idx;
                    for (size_t i = 0; i < v->arms.size(); ++i) {
                        if (std::holds_alternative<ast::MatchExpr::DefaultPattern>(v->arms[i].pattern)) {
                            if (default_arm_idx.has_value()) {
                                diag.report_error(DiagnosticStage::Sema, v->arms[i].location, "duplicate default arm '_'");
                            } else if (i + 1 != v->arms.size()) {
                                diag.report_error(DiagnosticStage::Sema, v->arms[i].location, "default arm '_' must be the last arm");
                            }
                            default_arm_idx = i;
                        }
                    }

                    if (operand_type.is_float()) {
                        diag.report_error(DiagnosticStage::Sema, v->location, "cannot switch on floating-point types; use if/else chains");
                        return;
                    }
                    if (operand_type.kind == TypeKind::Pointer || operand_type.kind == TypeKind::Anyptr) {
                        diag.report_error(DiagnosticStage::Sema, v->location, "cannot switch on pointer types");
                        return;
                    }

                    // Scalar switch (integer or bool)
                    if (operand_type.is_integer() || operand_type.kind == TypeKind::Bool) {
                        std::unordered_map<int64_t, size_t> seen_values;
                        bool true_covered = false, false_covered = false;

                        for (size_t arm_i = 0; arm_i < v->arms.size(); ++arm_i) {
                            const auto &arm = v->arms[arm_i];

                            if (std::holds_alternative<ast::MatchExpr::VariantPattern>(arm.pattern)) {
                                diag.report_error(DiagnosticStage::Sema, arm.location, "'.name' patterns require an enum or tagged union operand");
                                continue;
                            }
                            if (std::holds_alternative<ast::MatchExpr::DefaultPattern>(arm.pattern)) {
                                auto arm_locals = locals;
                                check_stmt(arm.body, arm_locals, module_path, program, diag, expected_returns, loop_depth, defer_loop_base);
                                continue;
                            }
                            const auto &lp = std::get<ast::MatchExpr::LiteralPattern>(arm.pattern);
                            if (!is_constant_expr(*lp.expr, module_path, program)) {
                                diag.report_error(DiagnosticStage::Sema, arm.location, "switch arm pattern must be a compile-time constant");
                            }
                            check_expr(*lp.expr, locals, module_path, program, diag, operand_type, loop_depth, defer_loop_base);
                            const auto val = evaluate_integer_constant(*lp.expr, module_path, program);
                            if (val) {
                                if (seen_values.count(*val)) {
                                    diag.report_error(DiagnosticStage::Sema, arm.location, std::format("duplicate switch arm: value already covered by arm {}", seen_values.at(*val) + 1));
                                } else {
                                    seen_values[*val] = arm_i;
                                    if (operand_type.kind == TypeKind::Bool) {
                                        if (*val == 0) false_covered = true;
                                        else if (*val == 1) true_covered = true;
                                    }
                                }
                            }
                            auto arm_locals = locals;
                            check_stmt(arm.body, arm_locals, module_path, program, diag, expected_returns, loop_depth, defer_loop_base);
                        }

                        // Check for unreachable '_' on bool (no exhaustiveness required for switch)
                        if (default_arm_idx && operand_type.kind == TypeKind::Bool && true_covered && false_covered) {
                            diag.report_error(DiagnosticStage::Sema, v->arms[*default_arm_idx].location,
                                "unreachable default arm: bool switch already covers both 'true' and 'false'");
                        }
                        return;
                    }

                    // Tagged union switch
                    if (operand_type.kind == TypeKind::Union) {
                        const auto &union_info = program.unions[operand_type.union_index];
                        if (!union_info.is_tagged) {
                            diag.report_error(DiagnosticStage::Sema, v->location, "switch operand must be an enum, tagged union, integer, or bool type");
                            return;
                        }
                        const bool any_ref_capture = std::ranges::any_of(v->arms, [](const auto &a) {
                            const auto *vp = std::get_if<ast::MatchExpr::VariantPattern>(&a.pattern);
                            return vp && vp->capture_by_ref;
                        });
                        if (any_ref_capture) {
                            const auto lv = resolve_lvalue(v->operand, locals, module_path, program, diag, loop_depth, defer_loop_base);
                            if (lv.type.kind == TypeKind::Invalid) {
                                diag.report_error(DiagnosticStage::Sema, v->location, "by-ref capture requires an lvalue switch operand");
                            }
                        }

                        std::vector<bool> covered(union_info.variants.size(), false);
                        for (const auto &arm : v->arms) {
                            if (std::holds_alternative<ast::MatchExpr::DefaultPattern>(arm.pattern)) {
                                auto arm_locals = locals;
                                check_stmt(arm.body, arm_locals, module_path, program, diag, expected_returns, loop_depth, defer_loop_base);
                                continue;
                            }
                            if (!std::holds_alternative<ast::MatchExpr::VariantPattern>(arm.pattern)) {
                                diag.report_error(DiagnosticStage::Sema, arm.location, "literal patterns require a scalar (integer/bool) operand");
                                continue;
                            }
                            const auto &vp = std::get<ast::MatchExpr::VariantPattern>(arm.pattern);
                            bool found = false;
                            for (size_t i = 0; i < union_info.variants.size(); ++i) {
                                if (union_info.variants[i].name == vp.name) {
                                    if (covered[i]) {
                                        diag.report_error(DiagnosticStage::Sema, arm.location, std::format("duplicate switch arm for variant '{}'", vp.name));
                                    }
                                    covered[i] = true;
                                    found = true;
                                    const auto &variant = union_info.variants[i];
                                    auto arm_locals = locals;
                                    if (vp.capture_name) {
                                        if (variant.payload_struct_index < 0) {
                                            diag.report_error(DiagnosticStage::Sema, arm.location, std::format("variant '{}' has no payload; cannot capture", vp.name));
                                        } else {
                                            const ResolvedType payload_ty{.kind = TypeKind::Struct, .struct_index = variant.payload_struct_index};
                                            if (vp.capture_by_ref) {
                                                arm_locals[*vp.capture_name] = LocalBinding{
                                                    .type = intern_pointer(program.modules.at(module_path), payload_ty),
                                                    .is_mut = false,
                                                };
                                            } else {
                                                arm_locals[*vp.capture_name] = LocalBinding{.type = payload_ty, .is_mut = false};
                                            }
                                        }
                                    }
                                    check_stmt(arm.body, arm_locals, module_path, program, diag, expected_returns, loop_depth, defer_loop_base);
                                    break;
                                }
                            }
                            if (!found) {
                                diag.report_error(DiagnosticStage::Sema, arm.location, std::format("no variant '{}' on tagged union", vp.name));
                            }
                        }
                        if (default_arm_idx && std::ranges::all_of(covered, [](bool b) { return b; })) {
                            diag.report_error(DiagnosticStage::Sema, v->arms[*default_arm_idx].location, "unreachable default arm: all variants are already covered");
                        }
                        return;
                    }

                    // Enum switch
                    if (operand_type.kind != TypeKind::Enum) {
                        diag.report_error(DiagnosticStage::Sema, v->location, "switch operand must be an enum, tagged union, integer, or bool type");
                        return;
                    }
                    const auto &enum_info = program.enums[operand_type.enum_index];
                    std::vector<bool> covered(enum_info.fields.size(), false);
                    for (const auto &arm : v->arms) {
                        if (std::holds_alternative<ast::MatchExpr::DefaultPattern>(arm.pattern)) {
                            auto arm_locals = locals;
                            check_stmt(arm.body, arm_locals, module_path, program, diag, expected_returns, loop_depth, defer_loop_base);
                            continue;
                        }
                        if (!std::holds_alternative<ast::MatchExpr::VariantPattern>(arm.pattern)) {
                            diag.report_error(DiagnosticStage::Sema, arm.location, "literal patterns require a scalar (integer/bool) operand");
                            continue;
                        }
                        const auto &vp = std::get<ast::MatchExpr::VariantPattern>(arm.pattern);
                        if (vp.capture_name) {
                            diag.report_error(DiagnosticStage::Sema, arm.location, "payload capture is only valid for tagged union arms");
                        }
                        bool found = false;
                        for (size_t i = 0; i < enum_info.fields.size(); ++i) {
                            if (enum_info.fields[i].name == vp.name) {
                                if (covered[i]) {
                                    diag.report_error(DiagnosticStage::Sema, arm.location, std::format("duplicate switch arm for enum field '{}'", vp.name));
                                }
                                covered[i] = true;
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
                            diag.report_error(DiagnosticStage::Sema, arm.location, std::format("no enum field named '{}'", vp.name));
                        }
                        auto arm_locals = locals;
                        check_stmt(arm.body, arm_locals, module_path, program, diag, expected_returns, loop_depth, defer_loop_base);
                    }
                    if (default_arm_idx && std::ranges::all_of(covered, [](bool b) { return b; })) {
                        diag.report_error(DiagnosticStage::Sema, v->arms[*default_arm_idx].location, "unreachable default arm: all enum fields are already covered");
                    }

                } else if constexpr (std::is_same_v<V, ast::ContinueStmt>) {
                    if (loop_depth == 0) {
                        diag.report_error(DiagnosticStage::Sema, v.location, "'continue' outside of a loop");
                    } else if (defer_loop_base >= 0 && loop_depth <= defer_loop_base) {
                        diag.report_error(DiagnosticStage::Sema, v.location, "'continue' cannot escape a 'defer' body");
                    }

                } else if constexpr (std::is_same_v<V, ast::BreakStmt>) {
                    if (loop_depth == 0) {
                        diag.report_error(DiagnosticStage::Sema, v.location, "'break' outside of a loop");
                    } else if (defer_loop_base >= 0 && loop_depth <= defer_loop_base) {
                        diag.report_error(DiagnosticStage::Sema, v.location, "'break' cannot escape a 'defer' body");
                    }

                } else if constexpr (std::is_same_v<V, ast::ReturnStmt>) {
                    if (defer_loop_base >= 0) {
                        diag.report_error(DiagnosticStage::Sema, v.location, "'return' cannot escape a 'defer' body");
                        return;
                    }
                    if (v.return_values.size() != expected_returns.size()) {
                        diag.report_error(DiagnosticStage::Sema, v.location,
                                          std::format("expected {} return value(s), got {}", expected_returns.size(), v.return_values.size()));
                        for (auto &val : v.return_values)
                            check_expr(val, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base);
                        return;
                    }
                    for (size_t i = 0; i < v.return_values.size(); ++i) {
                        auto ty = check_expr(v.return_values[i], locals, module_path, program, diag, expected_returns[i], loop_depth, defer_loop_base);
                        if (!assignable_in_module(ty, expected_returns[i], module_path, program)) {
                            diag.report_error(DiagnosticStage::Sema, v.location, std::format("return value {} type mismatch", i + 1));
                        }
                    }

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::DeferStmt>>) {
                    // Register defer: validate the defer body with defer_loop_base = current loop_depth.
                    // Inside the defer body, return/try are forbidden; break/continue only allowed for loops
                    // fully inside the defer body.
                    check_stmt(v->body, locals, module_path, program, diag, expected_returns, loop_depth, loop_depth);
                }
            },
            stmt);
    }
}
