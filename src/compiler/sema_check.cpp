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

        // Like error(), but returns a caller-supplied fallback type instead of Invalid. Used
        // where the expected type is already known (that's how the mismatch was detected), so
        // resolving to it here prevents the same root-cause error from cascading into a second,
        // redundant diagnostic at the call site (e.g. a return-statement type mismatch).
        auto error_as(DiagnosticEngine &diag, const SourceLocation &loc, std::string msg,
                       const ResolvedType &fallback) -> ResolvedType {
            diag.report_error(DiagnosticStage::Sema, loc, std::move(msg));
            return fallback;
        }

        auto format_named_type(const ast::NamedType &named) -> std::string {
            std::string result = named.name;
            for (const ast::NamedType *m = named.member.get(); m; m = m->member.get()) {
                result += '.';
                result += m->name;
            }
            return result;
        }

        // NamedType holds its `member` chain via unique_ptr, so it's move-only; deep-copy it here
        // rather than moving out of an AST node, since expressions can be re-checked more than once.
        auto clone_named_type(const ast::NamedType &named) -> ast::NamedType {
            return ast::NamedType{
                .name = named.name,
                .member = named.member ? std::make_unique<ast::NamedType>(clone_named_type(*named.member)) : nullptr,
                .location = named.location,
            };
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

        auto is_coercible_literal(const ast::Expr &expr) -> bool {
            return std::visit(
                [&]<typename T0>(const T0 &v) -> bool {
                    using V = std::decay_t<T0>;
                    if constexpr (std::is_same_v<V, ast::LiteralIntegerExpr> ||
                                  std::is_same_v<V, ast::LiteralFloatExpr> ||
                                  std::is_same_v<V, ast::LiteralNilExpr>) {
                        return true;
                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::UnaryExpr>>) {
                        return v->op == ast::UnaryOp::Negate && is_coercible_literal(v->operand);
                    } else {
                        return false;
                    }
                },
                expr);
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
            if (from.kind == TypeKind::Array && (to.kind == TypeKind::Pointer || to.kind == TypeKind::Anyptr)) return true;
            if (from.kind == TypeKind::Slice && (to.kind == TypeKind::Pointer || to.kind == TypeKind::Anyptr)) return true;
            // Function pointers can be cast to/from anyptr (C callback interop)
            if (from.kind == TypeKind::Function && to.kind == TypeKind::Anyptr) return true;
            if (from.kind == TypeKind::Anyptr && to.kind == TypeKind::Function) return true;
            return from.is_scalar() && to.is_scalar();
        }

        // Returns the FunctionTypeInfo for a function-kind ResolvedType. Falls
        // back to a static empty signature for a stale/out-of-range index
        // rather than throwing - see Program::fn_signature_at().
        auto fn_sig(const ResolvedType &ty, const Program &program) -> const FunctionTypeInfo & {
            static const FunctionTypeInfo empty{};
            const auto *sig = program.fn_signature_at(ty.fn_index);
            return sig ? *sig : empty;
        }

        auto slice_element_type(const ResolvedType &slice, [[maybe_unused]] const std::string &module_path, Program &program) -> ResolvedType {
            const auto *info = program.slice_at(slice.slice_index);
            return info ? info->element_type : ResolvedType{.kind = TypeKind::Invalid};
        }

        auto array_element_type(const ResolvedType &array, [[maybe_unused]] const std::string &module_path, Program &program) -> ResolvedType {
            const auto *info = program.array_at(array.array_index);
            return info ? info->element_type : ResolvedType{.kind = TypeKind::Invalid};
        }

        auto assignable_in_module(const ResolvedType &from, const ResolvedType &to, const std::string &module_path, Program &program) -> bool {
            if (from.kind == TypeKind::Array && to.kind == TypeKind::Slice) {
                return array_element_type(from, module_path, program) == slice_element_type(to, module_path, program);
            }
            if (from.kind == TypeKind::Slice && to.kind == TypeKind::Array) {
                return slice_element_type(from, module_path, program) == array_element_type(to, module_path, program);
            }
            if (from.kind == TypeKind::Array && to.kind == TypeKind::Pointer) {
                const auto *pointee = program.pointee_at(to.pointee_index);
                return pointee && array_element_type(from, module_path, program) == *pointee;
            }
            return is_assignable(from, to);
        }

        auto slice_cast_elements_match(const ResolvedType &from, const ResolvedType &to, const std::string &module_path, Program &program) -> bool {
            if (to.kind != TypeKind::Slice) return true;
            if (from.kind == TypeKind::Array) return array_element_type(from, module_path, program) == slice_element_type(to, module_path, program);
            if (from.kind == TypeKind::Slice) return slice_element_type(from, module_path, program) == slice_element_type(to, module_path, program);
            return true;
        }

        auto check_call_args(const std::vector<ast::Expr> &args, const std::vector<ResolvedType> &params, bool is_variadic, LocalScope &locals, const std::string &module_path, Program &program, DiagnosticEngine &diag, const SourceLocation &loc, const std::string &callee_desc, int loop_depth, int defer_loop_base, bool fn_returns_error, bool native_variadic = false) -> bool;
        auto try_resolve_namespace_chain(const ast::Expr &expr, const std::string &module_path, LocalScope &locals, Program &program) -> std::optional<std::string>;

        // Tier-3 method-call resolution: 'receiver_type' is an actual dyn-handle
        // (TypeKind::Trait), which has no concrete MethodInfo/body to look up via
        // find_method — dispatch is resolved against the trait's own method list
        // instead. Returns std::nullopt (not an error) when receiver_type isn't a
        // trait handle, so callers fall through to the existing find_method path
        // unchanged. 'dispatch_key' is the address of the ast::CallExpr node itself
        // (stable across check_expr / check_group_call_returns / codegen's emit_call
        // and call_return_types, all of which take a 'const ast::CallExpr&' referring
        // to the same heap-allocated node) — NOT sema::get_expr_key's variant-slot
        // address, which check_group_call_returns has no way to reproduce since it
        // only receives the unwrapped CallExpr, not the outer Expr variant.
        auto try_trait_handle_dispatch(const ResolvedType &receiver_type, const std::string &method_name,
                                        const std::vector<ast::Expr> &args, const void *dispatch_key,
                                        LocalScope &locals, const std::string &module_path, Program &program,
                                        DiagnosticEngine &diag, const SourceLocation &loc, const int loop_depth,
                                        const int defer_loop_base, const bool fn_returns_error) -> std::optional<std::vector<ResolvedType>> {
            if (receiver_type.kind != TypeKind::Trait) return std::nullopt;

            const auto *trait_info = program.trait_at(receiver_type.trait_index);
            const TraitMethodInfo *trait_method = nullptr;
            int method_order_index = -1;
            if (trait_info) {
                for (size_t i = 0; i < trait_info->methods.size(); ++i) {
                    if (trait_info->methods[i].name == method_name) {
                        trait_method = &trait_info->methods[i];
                        method_order_index = static_cast<int>(i);
                        break;
                    }
                }
            }

            if (!trait_method) {
                const auto [trait_module, trait_name] = find_type_module_and_name(receiver_type, program);
                error(diag, loc, std::format("no method '{}' on trait '{}'", method_name, trait_name.empty() ? "?" : trait_name));
                return std::vector<ResolvedType>{};
            }

            check_call_args(args, trait_method->params, false, locals, module_path, program, diag, loc, method_name, loop_depth, defer_loop_base, fn_returns_error);

            program.modules.at(module_path).expr_trait_dispatch[dispatch_key] = TraitDispatchInfo{
                .trait_index = receiver_type.trait_index,
                .method_order_index = method_order_index,
            };

            return trait_method->return_types;
        }

        auto check_group_call_returns(const ast::CallExpr &call, LocalScope &locals, const std::string &module_path, Program &program, DiagnosticEngine &diag, const int loop_depth, const int defer_loop_base, const bool fn_returns_error) -> std::vector<ResolvedType> {
            std::string target_module = module_path;
            std::string name;
            bool check_pub = false;

            if (const auto *callee_ident = std::get_if<ast::IdentExpr>(&call.callee)) {
                if (auto local_it = locals.find(callee_ident->name); local_it != locals.end()) {
                    const auto &local_ty = local_it->second.type;
                    if (local_ty.kind == TypeKind::Function) {
                        const auto &sig = fn_sig(local_ty, program);
                        check_call_args(call.args, sig.param_types, sig.is_variadic, locals, module_path, program, diag, call.location, callee_ident->name, loop_depth, defer_loop_base, fn_returns_error);
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
                    auto receiver_type = check_expr((*member)->object, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_returns_error);
                    if (receiver_type.kind == TypeKind::Pointer) {
                        if (const auto *pointee = program.pointee_at(receiver_type.pointee_index)) {
                            receiver_type = *pointee;
                        } else {
                            receiver_type = ResolvedType{.kind = TypeKind::Invalid};
                        }
                    }
                    if (auto trait_returns = try_trait_handle_dispatch(receiver_type, (*member)->member, call.args, &call, locals, module_path, program, diag, call.location, loop_depth, defer_loop_base, fn_returns_error)) {
                        return *trait_returns;
                    }
                    const auto *method = find_method(receiver_type, (*member)->member, program);
                    if (!method) {
                        // Struct field with function type
                        if (receiver_type.kind == TypeKind::Struct) {
                            if (const auto *struct_info = program.struct_at(receiver_type.struct_index)) {
                                for (const auto &field : struct_info->fields) {
                                    if (field.name == (*member)->member && field.type.kind == TypeKind::Function) {
                                        const auto &sig = fn_sig(field.type, program);
                                        check_call_args(call.args, sig.param_types, sig.is_variadic, locals, module_path, program, diag, call.location, (*member)->member, loop_depth, defer_loop_base, fn_returns_error);
                                        return sig.return_types;
                                    }
                                }
                            }
                        }
                        error(diag, call.location, std::format("no method '{}' on type", (*member)->member));
                        return {};
                    }
                    check_call_args(call.args, method->param_types, false, locals, module_path, program, diag, call.location, (*member)->member, loop_depth, defer_loop_base, fn_returns_error, method->is_variadic);
                    return method->return_types;
                }
            } else {
                // General expression callee (e.g. deref of fn ptr, indexed fn ptr array)
                const auto callee_ty = check_expr(call.callee, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_returns_error);
                if (callee_ty.kind == TypeKind::Function) {
                    const auto &sig = fn_sig(callee_ty, program);
                    check_call_args(call.args, sig.param_types, sig.is_variadic, locals, module_path, program, diag, call.location, "<fn ptr>", loop_depth, defer_loop_base, fn_returns_error);
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
                        check_call_args(call.args, sym.params, false, locals, module_path, program, diag, call.location, name, loop_depth, defer_loop_base, fn_returns_error, sym.is_variadic);
                        return sym.return_types;
                    } else if constexpr (std::is_same_v<S, ExtFunctionSymbol>) {
                        if (check_pub && !sym.is_pub) {
                            error(diag, call.location, std::format("'{}' is not pub", name));
                            return {};
                        }
                        check_call_args(call.args, sym.params, sym.is_variadic, locals, module_path, program, diag, call.location, name, loop_depth, defer_loop_base, fn_returns_error);
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

        auto check_call_args(const std::vector<ast::Expr> &args, const std::vector<ResolvedType> &params, const bool is_variadic, LocalScope &locals, const std::string &module_path, Program &program, DiagnosticEngine &diag, const SourceLocation &loc, const std::string &callee_desc, const int loop_depth, const int defer_loop_base, const bool fn_returns_error, const bool native_variadic) -> bool {
            if (native_variadic) {
                // Last entry of 'params' is the dissolved '[]T' slot for the native '...T' parameter.
                const size_t fixed_count = params.size() - 1;
                if (args.size() < fixed_count) {
                    error(diag, loc, std::format("'{}' expects at least {} argument(s), got {}", callee_desc, fixed_count, args.size()));
                    return false;
                }

                bool ok = true;
                for (size_t i = 0; i < fixed_count; ++i) {
                    if (auto arg_ty = check_expr(args[i], locals, module_path, program, diag, params[i], loop_depth, defer_loop_base, fn_returns_error); !assignable_in_module(arg_ty, params[i], module_path, program)) {
                        error(diag, loc, std::format("'{}' argument {} type mismatch", callee_desc, i + 1));
                        ok = false;
                    }
                }

                const auto &slice_ty = params.back();
                const auto element_ty = slice_element_type(slice_ty, module_path, program);
                const size_t tail_count = args.size() - fixed_count;

                if (tail_count == 1) {
                    if (const auto *spread = std::get_if<std::unique_ptr<ast::SpreadExpr>>(&args[fixed_count])) {
                        const auto spread_ty = check_expr((*spread)->operand, locals, module_path, program, diag, slice_ty, loop_depth, defer_loop_base, fn_returns_error);
                        if (!assignable_in_module(spread_ty, slice_ty, module_path, program)) {
                            error(diag, loc, std::format("'{}' spread argument type mismatch: expected a slice matching the variadic element type", callee_desc));
                            ok = false;
                        }
                        return ok;
                    }
                }

                for (size_t i = fixed_count; i < args.size(); ++i) {
                    if (std::holds_alternative<std::unique_ptr<ast::SpreadExpr>>(args[i])) {
                        error(diag, loc, std::format("'{}': '...' spread argument must be the sole variadic argument", callee_desc));
                        ok = false;
                        continue;
                    }
                    if (auto arg_ty = check_expr(args[i], locals, module_path, program, diag, element_ty, loop_depth, defer_loop_base, fn_returns_error); !assignable_in_module(arg_ty, element_ty, module_path, program)) {
                        error(diag, loc, std::format("'{}' variadic argument {} type mismatch", callee_desc, i - fixed_count + 1));
                        ok = false;
                    }
                }
                return ok;
            }

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
                if (auto arg_ty = check_expr(args[i], locals, module_path, program, diag, params[i], loop_depth, defer_loop_base, fn_returns_error); !assignable_in_module(arg_ty, params[i], module_path, program)) {
                    error(diag, loc, std::format("'{}' argument {} type mismatch", callee_desc, i + 1));
                    ok = false;
                }
            }
            if (is_variadic) {
                for (size_t i = params.size(); i < args.size(); ++i) {
                    const auto arg_ty = check_expr(args[i], locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_returns_error);
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

        auto resolve_lvalue(const ast::Expr &expr, LocalScope &locals, const std::string &module_path, Program &program, DiagnosticEngine &diag, int loop_depth, int defer_loop_base, bool fn_returns_error) -> LvalueInfo;

        // `need_writable` controls whether a struct/union-valued `m.object` gets speculatively
        // probed for writability via resolve_lvalue(). That probe is only meaningful when the
        // caller is actually going to use the resulting .writable flag (assignment targets,
        // address-of, etc. - reached via resolve_lvalue()'s own MemberExpr case, which doesn't
        // pass this and keeps the default true). A plain read of `m` (check_expr's MemberExpr
        // case) never looks at .writable, so it passes false: resolve_lvalue's fallback for any
        // object shape it doesn't recognize as inherently addressable (e.g. a CallExpr, as in
        // `f().field`) reports "not an assignable expression" - correct when something is
        // actually being assigned to or addressed, but a spurious compile error for an ordinary
        // read of a field on a temporary struct/union value, which is always legal.
        auto resolve_member(const ast::MemberExpr &m, LocalScope &locals, const std::string &module_path, Program &program, DiagnosticEngine &diag, const int loop_depth, const int defer_loop_base, const bool fn_returns_error, const bool need_writable = true) -> LvalueInfo {
            if (const auto target_module = try_resolve_namespace_chain(m.object, module_path, locals, program)) {
                return check_member_cross_module(m, *target_module, program, diag);
            }

            // Handle fully-qualified enum field: e.g. EnumType.field or module.EnumType.field
            if (const auto type_ref = try_resolve_type_chain(m.object, module_path, locals, program)) {
                if (type_ref->kind == TypeKind::Enum) {
                    if (const auto *enum_info = program.enum_at(type_ref->enum_index)) {
                        for (const auto &field : enum_info->fields) {
                            if (field.name == m.member) {
                                return {*type_ref, false};
                            }
                        }
                    }
                    error(diag, m.location, std::format("no enum field named '{}'", m.member));
                    return {ResolvedType{.kind = TypeKind::Invalid}, false};
                }
            }

            const auto object_type = check_expr(m.object, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_returns_error);

            ResolvedType effective_type;
            bool writable;

            if (object_type.kind == TypeKind::Pointer) {
                const auto *pointee = program.pointee_at(object_type.pointee_index);
                if (!pointee) {
                    error(diag, m.location, "internal error: invalid pointer index");
                    return {ResolvedType{.kind = TypeKind::Invalid}, false};
                }
                effective_type = *pointee;
                writable = true;
            } else if (object_type.kind == TypeKind::Struct || object_type.kind == TypeKind::Union) {
                effective_type = object_type;
                if (need_writable) {
                    const auto object_lvalue = resolve_lvalue(m.object, locals, module_path, program, diag, loop_depth, defer_loop_base, fn_returns_error);
                    writable = object_lvalue.type == object_type && object_lvalue.writable;
                } else {
                    writable = false;
                }
            } else if (object_type.kind == TypeKind::Trait) {
                error(diag, m.location, "cannot access fields on a trait handle; handles have no visible layout");
                return {ResolvedType{.kind = TypeKind::Invalid}, false};
            } else if (object_type.kind == TypeKind::Invalid) {
                return {ResolvedType{.kind = TypeKind::Invalid}, false};
            } else {
                error(diag, m.location, "'.' requires a struct, union, or pointer-to-struct/union value");
                return {ResolvedType{.kind = TypeKind::Invalid}, false};
            }

            if (effective_type.kind == TypeKind::Struct) {
                if (const auto *info = program.struct_at(effective_type.struct_index)) {
                    for (auto &field : info->fields) {
                        if (field.name == m.member) {
                            return {field.type, writable};
                        }
                    }
                }
                error(diag, m.location, std::format("no field named '{}'", m.member));
                return {ResolvedType{.kind = TypeKind::Invalid}, false};
            }

            // TypeKind::Union
            {
                const auto *info = program.union_at(effective_type.union_index);
                if (!info) {
                    error(diag, m.location, "internal error: invalid union index");
                    return {ResolvedType{.kind = TypeKind::Invalid}, false};
                }
                if (info->is_tagged) {
                    error(diag, m.location, "cannot access tagged union variants directly; use 'match' to destructure");
                    return {ResolvedType{.kind = TypeKind::Invalid}, false};
                }
                for (auto &member : info->members) {
                    if (member.name == m.member) {
                        return {member.type, writable};
                    }
                }
            }

            error(diag, m.location, std::format("no member named '{}'", m.member));
            return {ResolvedType{.kind = TypeKind::Invalid}, false};
        }

        auto resolve_lvalue(const ast::Expr &expr, LocalScope &locals, const std::string &module_path, Program &program, DiagnosticEngine &diag, const int loop_depth, const int defer_loop_base, const bool fn_returns_error) -> LvalueInfo {
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
                        const ResolvedType ptr_ty = check_expr(v->operand, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_returns_error);
                        if (ptr_ty.kind == TypeKind::Trait) {
                            error(diag, v->location, "cannot dereference a trait handle");
                            return {ResolvedType{.kind = TypeKind::Invalid}, false};
                        }
                        if (ptr_ty.kind != TypeKind::Pointer) {
                            error(diag, v->location, "cannot dereference a non-pointer value");
                            return {ResolvedType{.kind = TypeKind::Invalid}, false};
                        }
                        const auto *pointee = program.pointee_at(ptr_ty.pointee_index);
                        if (!pointee) {
                            error(diag, v->location, "internal error: invalid pointer index");
                            return {ResolvedType{.kind = TypeKind::Invalid}, false};
                        }
                        return {*pointee, true};

                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::MemberExpr>>) {
                        return resolve_member(*v, locals, module_path, program, diag, loop_depth, defer_loop_base, fn_returns_error);

                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::IndexExpr>>) {
                        const auto operand = check_expr(v->operand, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_returns_error);
                        const auto index = check_expr(v->index, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_returns_error);
                        if (!index.is_integer()) {
                            error(diag, v->location, "index must be an integer expression");
                        }
                        if (operand.kind == TypeKind::Pointer) {
                            const auto *pointee = program.pointee_at(operand.pointee_index);
                            return {pointee ? *pointee : ResolvedType{.kind = TypeKind::Invalid}, true};
                        }
                        if (operand.kind == TypeKind::Array) {
                            auto owner = resolve_lvalue(v->operand, locals, module_path, program, diag, loop_depth, defer_loop_base, fn_returns_error);
                            return {array_element_type(operand, module_path, program), owner.writable};
                        }
                        if (operand.kind == TypeKind::Slice) {
                            return {slice_element_type(operand, module_path, program), true};
                        }
                        error(diag, v->location, "indexing requires a pointer, array, or slice operand");
                        return {ResolvedType{.kind = TypeKind::Invalid}, false};

                    } else {
                        // get_expr_location(), not a zero-valued SourceLocation{}: an empty
                        // location's filename doesn't match any open document, so the LSP's
                        // diagnostics publisher (see diagnostics.cpp) silently drops it - the
                        // CLI would still print it, but editors would report success on a
                        // build that actually fails.
                        error(diag, get_expr_location(expr), "not an assignable expression");
                        return {ResolvedType{.kind = TypeKind::Invalid}, false};
                    }
                },
                expr);
        }
    }

    auto check_expr(const ast::Expr &expr, LocalScope &locals, const std::string &module_path, Program &program, DiagnosticEngine &diag, const std::optional<ResolvedType> expected, const int loop_depth, const int defer_loop_base, const bool fn_returns_error) -> ResolvedType {
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
                    return intern_slice(program, ResolvedType{.kind = TypeKind::U8});

                } else if constexpr (std::is_same_v<V, ast::LiteralCharExpr>) {
                    return ResolvedType{.kind = TypeKind::U8};

                } else if constexpr (std::is_same_v<V, ast::LiteralBoolExpr>) {
                    return ResolvedType{.kind = TypeKind::Bool};

                } else if constexpr (std::is_same_v<V, ast::LiteralNilExpr>) {
                    if (expected && (expected->kind == TypeKind::Slice || expected->kind == TypeKind::Trait)) return *expected;
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
                                if (sym.is_variadic) {
                                    return error(diag, v.location, std::format("cannot take the address of variadic function '{}'; function pointers to variadic functions are not supported", v.name));
                                }
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
                            const ResolvedType operand = check_expr(v->operand, locals, module_path, program, diag, expected, loop_depth, defer_loop_base, fn_returns_error);
                            if (!operand.is_integer() && !operand.is_float()) {
                                return error(diag, v->location, "unary '-' requires a numeric operand");
                            }
                            return operand;
                        }
                    case ast::UnaryOp::LogicalNot:
                        check_expr(v->operand, locals, module_path, program, diag, ResolvedType{.kind = TypeKind::Bool}, loop_depth, defer_loop_base, fn_returns_error);
                        return ResolvedType{.kind = TypeKind::Bool};
                    case ast::UnaryOp::BitwiseNot:
                        {
                            const ResolvedType operand = check_expr(v->operand, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_returns_error);
                            if (!operand.is_integer()) return error(diag, v->location, "unary '~' requires an integer operand");
                            return operand;
                        }
                    case ast::UnaryOp::AddressOf:
                        {
                            const LvalueInfo lv = resolve_lvalue(v->operand, locals, module_path, program, diag, loop_depth, defer_loop_base, fn_returns_error);
                            return intern_pointer(program, lv.type);
                        }
                    case ast::UnaryOp::Deref:
                        {
                            const ResolvedType operand = check_expr(v->operand, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_returns_error);
                            if (operand.kind == TypeKind::Trait) return error(diag, v->location, "cannot dereference a trait handle");
                            if (operand.kind != TypeKind::Pointer) return error(diag, v->location, "cannot dereference a non-pointer value");
                            const auto *pointee = program.pointee_at(operand.pointee_index);
                            if (!pointee) return error(diag, v->location, "internal error: invalid pointer index");
                            return *pointee;
                        }
                    }
                    return ResolvedType{.kind = TypeKind::Invalid};

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::BinaryExpr>>) {
                    ResolvedType lhs, rhs;
                    if (is_coercible_literal(v->lhs) && !is_coercible_literal(v->rhs)) {
                        rhs = check_expr(v->rhs, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_returns_error);
                        lhs = check_expr(v->lhs, locals, module_path, program, diag, rhs, loop_depth, defer_loop_base, fn_returns_error);
                    } else {
                        lhs = check_expr(v->lhs, locals, module_path, program, diag, expected, loop_depth, defer_loop_base, fn_returns_error);
                        rhs = check_expr(v->rhs, locals, module_path, program, diag, lhs, loop_depth, defer_loop_base, fn_returns_error);
                    }

                    // Trait handles support no operator except '==' / '!=' against a literal
                    // 'nil' operand (checked on the raw AST, not the resolved type, since a
                    // nil literal coerced to the handle's trait type is otherwise structurally
                    // indistinguishable from an actual handle value of that same trait).
                    if (lhs.kind == TypeKind::Trait || rhs.kind == TypeKind::Trait) {
                        const bool lhs_is_nil = std::holds_alternative<ast::LiteralNilExpr>(v->lhs);
                        const bool rhs_is_nil = std::holds_alternative<ast::LiteralNilExpr>(v->rhs);
                        const bool is_eq = v->op == ast::BinaryOp::Equal || v->op == ast::BinaryOp::NotEqual;
                        if (is_eq && (lhs_is_nil || rhs_is_nil)) {
                            return ResolvedType{.kind = TypeKind::Bool};
                        }
                        return error(diag, v->location, "trait handles only support '==' / '!=' comparison against 'nil'; no other operators are supported");
                    }

                    return binary_op_result(v->op, lhs, rhs, diag, v->location);

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::TernaryExpr>>) {
                    check_expr(v->condition, locals, module_path, program, diag, ResolvedType{.kind = TypeKind::Bool}, loop_depth, defer_loop_base, fn_returns_error);
                    ResolvedType then_ty = check_expr(v->then_expr, locals, module_path, program, diag, expected, loop_depth, defer_loop_base, fn_returns_error);
                    const ResolvedType else_ty = check_expr(v->else_expr, locals, module_path, program, diag, then_ty, loop_depth, defer_loop_base, fn_returns_error);
                    if (then_ty != else_ty) {
                        return error(diag, v->location, "ternary branches have different types");
                    }
                    return then_ty;

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::AssignExpr>>) {
                    auto target = resolve_lvalue(v->target, locals, module_path, program, diag, loop_depth, defer_loop_base, fn_returns_error);
                    if (target.type.kind != TypeKind::Invalid && !target.writable) {
                        error(diag, v->location, "left-hand side of assignment is not mutable");
                    }

                    const auto value_ty = check_expr(v->value, locals, module_path, program, diag, target.type, loop_depth, defer_loop_base, fn_returns_error);
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
                                        check_call_args(v->args, sym.params, false, locals, module_path, program, diag, v->location, fn_name, loop_depth, defer_loop_base, fn_returns_error, sym.is_variadic);
                                        if (sym.return_types.size() > 1) {
                                            return error(diag, v->location, "multi-value capture is not yet supported here");
                                        }
                                        return sym.return_types.empty() ? ResolvedType{.kind = TypeKind::Void} : sym.return_types.front();
                                    } else if constexpr (std::is_same_v<S, ExtFunctionSymbol>) {
                                        if (!sym.is_pub) return error(diag, v->location, std::format("'{}' is not pub", fn_name));
                                        check_call_args(v->args, sym.params, sym.is_variadic, locals, module_path, program, diag, v->location, fn_name, loop_depth, defer_loop_base, fn_returns_error);
                                        return sym.return_type.value_or(ResolvedType{.kind = TypeKind::Void});
                                    } else if constexpr (std::is_same_v<S, MacroSymbol>) {
                                        if (!sym.is_pub) return error(diag, v->location, std::format("'{}' is not pub", fn_name));
                                        auto &resolved_macro = resolve_macro_symbol(*target_module, fn_name, program, diag, v->location);
                                        check_call_args(v->args, resolved_macro.params, false, locals, module_path, program, diag, v->location, fn_name, loop_depth, defer_loop_base, fn_returns_error);
                                        return resolved_macro.result_type;
                                    } else {
                                        return error(diag, v->location, std::format("'{}' is not callable", fn_name));
                                    }
                                },
                                sym_it->second);
                        }
                        // Method call on a value
                        auto receiver_type = check_expr((*member_callee)->object, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_returns_error);
                        if (receiver_type.kind == TypeKind::Pointer) {
                            if (const auto *pointee = program.pointee_at(receiver_type.pointee_index)) {
                                receiver_type = *pointee;
                            } else {
                                receiver_type = ResolvedType{.kind = TypeKind::Invalid};
                            }
                        }
                        if (auto trait_returns = try_trait_handle_dispatch(receiver_type, (*member_callee)->member, v->args, v.get(), locals, module_path, program, diag, v->location, loop_depth, defer_loop_base, fn_returns_error)) {
                            if (trait_returns->size() > 1) {
                                return error(diag, v->location, "multi-value capture is not yet supported here");
                            }
                            return trait_returns->empty() ? ResolvedType{.kind = TypeKind::Void} : trait_returns->front();
                        }
                        const auto *method = find_method(receiver_type, (*member_callee)->member, program);
                        if (!method) {
                            // Maybe it's a struct field with function type
                            if (receiver_type.kind == TypeKind::Struct && program.struct_at(receiver_type.struct_index)) {
                                for (const auto &field : program.struct_at(receiver_type.struct_index)->fields) {
                                    if (field.name == (*member_callee)->member) {
                                        if (field.type.kind == TypeKind::Function) {
                                            const auto &sig = fn_sig(field.type, program);
                                            check_call_args(v->args, sig.param_types, sig.is_variadic, locals, module_path, program, diag, v->location, (*member_callee)->member, loop_depth, defer_loop_base, fn_returns_error);
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
                        check_call_args(v->args, method->param_types, false, locals, module_path, program, diag, v->location, (*member_callee)->member, loop_depth, defer_loop_base, fn_returns_error, method->is_variadic);
                        if (method->return_types.size() > 1) {
                            return error(diag, v->location, "multi-value capture is not yet supported here");
                        }
                        return method->return_types.empty() ? ResolvedType{.kind = TypeKind::Void} : method->return_types.front();
                    }

                    // General expression callee: evaluate, then call through if it's a function type
                    if (!std::holds_alternative<ast::IdentExpr>(v->callee)) {
                        const auto callee_ty = check_expr(v->callee, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_returns_error);
                        if (callee_ty.kind == TypeKind::Function) {
                            const auto &sig = fn_sig(callee_ty, program);
                            check_call_args(v->args, sig.param_types, sig.is_variadic, locals, module_path, program, diag, v->location, "<fn ptr>", loop_depth, defer_loop_base, fn_returns_error);
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
                            check_call_args(v->args, sig.param_types, sig.is_variadic, locals, module_path, program, diag, v->location, callee_ident->name, loop_depth, defer_loop_base, fn_returns_error);
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
                                check_call_args(v->args, sym.params, false, locals, module_path, program, diag, v->location, callee_ident->name, loop_depth, defer_loop_base, fn_returns_error, sym.is_variadic);
                                if (sym.return_types.size() > 1) {
                                    return error(diag, v->location, "multi-value capture is not yet supported here");
                                }
                                return sym.return_types.empty() ? ResolvedType{.kind = TypeKind::Void} : sym.return_types.front();
                            } else if constexpr (std::is_same_v<S, ExtFunctionSymbol>) {
                                check_call_args(v->args, sym.params, sym.is_variadic, locals, module_path, program, diag, v->location, callee_ident->name, loop_depth, defer_loop_base, fn_returns_error);
                                return sym.return_type.value_or(ResolvedType{.kind = TypeKind::Void});
                            } else if constexpr (std::is_same_v<S, MacroSymbol>) {
                                auto &resolved_macro = resolve_macro_symbol(module_path, callee_ident->name, program, diag, v->location);
                                check_call_args(v->args, resolved_macro.params, false, locals, module_path, program, diag, v->location, callee_ident->name, loop_depth, defer_loop_base, fn_returns_error);
                                return resolved_macro.result_type;
                            } else {
                                return error(diag, v->location, std::format("'{}' is not callable", callee_ident->name));
                            }
                        },
                        sym_it->second);

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::IncrDecrExpr>>) {
                    const LvalueInfo lv = resolve_lvalue(v->operand, locals, module_path, program, diag, loop_depth, defer_loop_base, fn_returns_error);
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
                    // (runtime sizeof) if it isn't a type-name shape. Operand
                    // shapes the parser can't spell as an IdentExpr/MemberExpr
                    // (pointer/array/slice/fn-ptr types, builtin type keywords)
                    // arrive as a TypeExpr instead and fall through to the
                    // generic check_expr call below, which resolves them via
                    // the TypeExpr case further down in this dispatch.
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
                    check_expr(v->operand, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_returns_error);
                    return ResolvedType{.kind = TypeKind::USize};

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::TypeExpr>>) {
                    // A Type wrapped in an Expr slot (currently only produced by the parser for
                    // sizeof operands that can't be spelled as an ordinary expression - see
                    // starts_type_only in ast.cpp). Resolves like any other type reference; the
                    // result is cached into expr_types by the generic caching below, which is
                    // how codegen's sizeof_operand and type_resolver's sizeof_expr_operand read
                    // it back without needing to know about TypeExpr themselves.
                    return resolve_type(v->type, module_path, program, diag);

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::LenExpr>>) {
                    const auto operand = check_expr(v->operand, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_returns_error);
                    if (operand.kind != TypeKind::Array && operand.kind != TypeKind::Slice) {
                        return error(diag, v->location, "len() requires an array or slice operand");
                    }
                    return ResolvedType{.kind = TypeKind::USize};

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::CastExpr>>) {
                    // cast(expr, Type) - value first, target type second.
                    const ResolvedType from = check_expr(v->value, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_returns_error);
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
                        const auto len_ty = check_expr(*v->len_expr, locals, module_path, program, diag, ResolvedType{.kind = TypeKind::USize}, loop_depth, defer_loop_base, fn_returns_error);
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
                                        if (fn->is_variadic) {
                                            return error(diag, v->location, std::format("cannot take the address of variadic function '{}'; function pointers to variadic functions are not supported", v->member));
                                        }
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
                    return resolve_member(*v, locals, module_path, program, diag, loop_depth, defer_loop_base, fn_returns_error, /*need_writable=*/false).type;

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::IndexExpr>>) {
                    const auto operand = check_expr(v->operand, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_returns_error);
                    const auto index = check_expr(v->index, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_returns_error);
                    if (!index.is_integer()) {
                        error(diag, v->location, "index must be an integer expression");
                    }
                    if (operand.kind == TypeKind::Pointer) {
                        const auto *pointee = program.pointee_at(operand.pointee_index);
                        return pointee ? *pointee : ResolvedType{.kind = TypeKind::Invalid};
                    }
                    if (operand.kind == TypeKind::Array) {
                        return array_element_type(operand, module_path, program);
                    }
                    if (operand.kind == TypeKind::Slice) {
                        return slice_element_type(operand, module_path, program);
                    }
                    return error(diag, v->location, "indexing requires a pointer, array, or slice operand");

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::SliceExpr>>) {
                    const auto operand = check_expr(v->operand, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_returns_error);
                    const auto start = check_expr(v->start, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_returns_error);
                    const auto end = check_expr(v->end, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_returns_error);
                    if (!start.is_integer() || !end.is_integer()) {
                        error(diag, v->location, "slice bounds must be integer expressions");
                    }
                    if (operand.kind == TypeKind::Array) {
                        const auto element = array_element_type(operand, module_path, program);
                        return intern_slice(program, element);
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
                        const auto *enum_info = program.enum_at(expected->enum_index);
                        if (!enum_info) return error(diag, v.location, "internal error: invalid enum index");
                        for (const auto &field : enum_info->fields) {
                            if (field.name == v.name) {
                                return *expected;
                            }
                        }
                        return error_as(diag, v.location, std::format("no enum field named '{}'", v.name), *expected);
                    }
                    if (expected && expected->kind == TypeKind::Union) {
                        const auto *union_info = program.union_at(expected->union_index);
                        if (union_info && union_info->is_tagged) {
                            const auto it = std::ranges::find(union_info->variants, v.name, &TaggedUnionVariant::name);
                            if (it == union_info->variants.end()) {
                                return error_as(diag, v.location, std::format("no variant '{}' on tagged union", v.name), *expected);
                            }
                            if (it->payload_struct_index >= 0) {
                                return error_as(diag, v.location, std::format("variant '{}' has a payload; use '.{}{{...}}' syntax", v.name, v.name), *expected);
                            }
                            return *expected;
                        }
                    }
                    return error(diag, v.location, std::format("cannot resolve '.{}' without an expected enum or tagged union type", v.name));

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::MatchExpr>>) {
                    const auto operand_type = check_expr(v->operand, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_returns_error);

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
                                    arm_type.kind != TypeKind::Invalid ? std::optional<ResolvedType>{arm_type} : expected, loop_depth, defer_loop_base, fn_returns_error);
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
                            check_expr(*lp.expr, arm_locals, module_path, program, diag, operand_type, loop_depth, defer_loop_base, fn_returns_error);
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
                                arm_type.kind != TypeKind::Invalid ? std::optional<ResolvedType>{arm_type} : expected, loop_depth, defer_loop_base, fn_returns_error);
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
                        const auto *union_info_ptr = program.union_at(operand_type.union_index);
                        if (!union_info_ptr) {
                            return error(diag, v->location, "internal error: invalid union index");
                        }
                        const auto &union_info = *union_info_ptr;
                        if (!union_info.is_tagged) {
                            return error(diag, v->location, "match operand must be an enum or tagged union type");
                        }

                        // Check if any arm uses by-ref capture; operand must be an lvalue in that case
                        const bool any_ref_capture = std::ranges::any_of(v->arms, [](const auto &a) {
                            const auto *vp = std::get_if<ast::MatchExpr::VariantPattern>(&a.pattern);
                            return vp && vp->capture_by_ref;
                        });
                        if (any_ref_capture) {
                            const auto lv = resolve_lvalue(v->operand, locals, module_path, program, diag, loop_depth, defer_loop_base, fn_returns_error);
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
                                    arm_type.kind != TypeKind::Invalid ? std::optional<ResolvedType>{arm_type} : expected, loop_depth, defer_loop_base, fn_returns_error);
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
                                            const ResolvedType payload_ty = variant.payload_type;
                                            if (vp.capture_by_ref) {
                                                arm_locals[*vp.capture_name] = LocalBinding{
                                                    .type = intern_pointer(program, payload_ty),
                                                    .is_mut = false,
                                                };
                                            } else {
                                                arm_locals[*vp.capture_name] = LocalBinding{.type = payload_ty, .is_mut = false};
                                            }
                                        }
                                    }

                                    const auto val_type = check_expr(arm.value, arm_locals, module_path, program, diag,
                                                                     arm_type.kind != TypeKind::Invalid ? std::optional<ResolvedType>{arm_type} : expected, loop_depth, defer_loop_base, fn_returns_error);
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
                                const auto val_type = check_expr(arm.value, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_returns_error);
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

                    const auto *enum_info_ptr = program.enum_at(operand_type.enum_index);
                    if (!enum_info_ptr) {
                        return error(diag, v->location, "internal error: invalid enum index");
                    }
                    const auto &enum_info = *enum_info_ptr;

                    ResolvedType arm_type{.kind = TypeKind::Invalid};
                    bool first_arm = true;
                    std::vector<bool> covered(enum_info.fields.size(), false);

                    for (const auto &arm : v->arms) {
                        if (std::holds_alternative<ast::MatchExpr::DefaultPattern>(arm.pattern)) {
                            const auto val_type = check_expr(arm.value, locals, module_path, program, diag,
                                arm_type.kind != TypeKind::Invalid ? std::optional<ResolvedType>{arm_type} : expected, loop_depth, defer_loop_base, fn_returns_error);
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
                            arm_type.kind != TypeKind::Invalid ? std::optional<ResolvedType>{arm_type} : expected, loop_depth, defer_loop_base, fn_returns_error);
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
                    if (const auto *union_info = expected->kind == TypeKind::Union ? program.union_at(expected->union_index) : nullptr;
                        union_info && union_info->is_tagged) {
                        return error(diag, v.location, "tagged unions have no 'undefined' form; use an explicit variant initializer");
                    }
                    return *expected;
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::TryExpr>>) {
                    if (defer_loop_base >= 0) {
                        return error(diag, v->location, "'try' cannot propagate errors out of a 'defer' body");
                    }
                    if (!fn_returns_error) {
                        return error(diag, v->location, "enclosing function must return 'error' to use 'try'");
                    }
                    // The operand must be a CallExpr
                    const auto *call = std::get_if<std::unique_ptr<ast::CallExpr>>(&v->call);
                    if (!call) {
                        return error(diag, v->location, "'try' operand must be a direct function call");
                    }
                    const auto returns = check_group_call_returns(**call, locals, module_path, program, diag, loop_depth, defer_loop_base, fn_returns_error);
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
                    if (v->type_path) {
                        union_ty = resolve_type(ast::Type{clone_named_type(*v->type_path)}, module_path, program, diag);
                    } else if (expected && expected->kind == TypeKind::Union) {
                        union_ty = *expected;
                    } else {
                        return error(diag, v->location, "cannot infer tagged union type; provide an explicit type name (e.g. 'TypeName.variant{...}')");
                    }
                    if (union_ty.kind != TypeKind::Union) {
                        return error(diag, v->location, std::format("'{}' is not a union type", format_named_type(*v->type_path)));
                    }
                    const auto *union_info_ptr = program.union_at(union_ty.union_index);
                    if (!union_info_ptr) {
                        return error(diag, v->location, "internal error: invalid union index");
                    }
                    const auto &union_info = *union_info_ptr;
                    if (!union_info.is_tagged) {
                        return error(diag, v->location, "use '{member = val}' syntax for untagged unions");
                    }
                    const auto variant_it = std::ranges::find(union_info.variants, v->variant_name, &TaggedUnionVariant::name);
                    if (variant_it == union_info.variants.end()) {
                        return error_as(diag, v->location, std::format("no variant '{}' on tagged union", v->variant_name), union_ty);
                    }
                    const bool has_payload = variant_it->payload_struct_index >= 0;
                    if (!has_payload && v->payload.has_value()) {
                        return error_as(diag, v->location, std::format("variant '{}' has no payload; use '.{}' without braces", v->variant_name, v->variant_name), union_ty);
                    }
                    if (has_payload) {
                        if (!v->payload.has_value()) {
                            return error_as(diag, v->location, std::format("variant '{}' requires a payload initializer; use '.{}{{field = val}}'", v->variant_name, v->variant_name), union_ty);
                        }
                        const auto &bv = *v->payload;
                        const auto *struct_info_ptr = program.struct_at(variant_it->payload_struct_index);
                        if (!struct_info_ptr) {
                            return error(diag, v->location, "internal error: invalid payload struct index");
                        }
                        const auto &struct_info = *struct_info_ptr;
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
                            const auto val_ty = check_expr(sf.expr, locals, module_path, program, diag, it->type, loop_depth, defer_loop_base, fn_returns_error);
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
                                    const auto *union_info_ptr = program.union_at(expected->union_index);
                                    if (!union_info_ptr) {
                                        return error(diag, bv.location, "internal error: invalid union index");
                                    }
                                    const auto &union_info = *union_info_ptr;
                                    if (bv.fields.size() != 1) {
                                        return error(diag, bv.location, std::format("a union initializer must set exactly one member, got {}", bv.fields.size()));
                                    }
                                    const auto &sf = bv.fields[0];
                                    const auto it = std::ranges::find(union_info.members, sf.name, &sema::UnionMember::name);
                                    if (it == union_info.members.end()) {
                                        error(diag, sf.location, std::format("no member '{}' on union", sf.name));
                                        return *expected;
                                    }
                                    const auto val_ty = check_expr(sf.expr, locals, module_path, program, diag, it->type, loop_depth, defer_loop_base, fn_returns_error);
                                    if (!assignable_in_module(val_ty, it->type, module_path, program)) {
                                        error(diag, sf.location, std::format("type mismatch for union member '{}'", sf.name));
                                    }
                                    return *expected;
                                }
                                if (expected->kind != TypeKind::Struct) {
                                    return error(diag, bv.location, "struct initializer requires a struct type");
                                }
                                const auto *info_ptr = program.struct_at(expected->struct_index);
                                if (!info_ptr) {
                                    return error(diag, bv.location, "internal error: invalid struct index");
                                }
                                const auto &info = *info_ptr;
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
                                    const auto val_ty = check_expr(sf.expr, locals, module_path, program, diag, it->type, loop_depth, defer_loop_base, fn_returns_error);
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
                                    if (bv.has_fill) {
                                        return error(diag, bv.location, "fill '...' is not allowed in a positional struct initializer");
                                    }
                                    const auto *info_ptr = program.struct_at(expected->struct_index);
                                    if (!info_ptr) {
                                        return error(diag, bv.location, "internal error: invalid struct index");
                                    }
                                    const auto &info = *info_ptr;
                                    if (bv.values.size() > info.fields.size()) {
                                        return error(diag, bv.location, std::format("too many values in struct initializer: struct has {} field(s), got {}", info.fields.size(), bv.values.size()));
                                    }
                                    for (size_t i = 0; i < bv.values.size(); ++i) {
                                        const auto &field = info.fields[i];
                                        const auto val_ty = check_expr(bv.values[i], locals, module_path, program, diag, field.type, loop_depth, defer_loop_base, fn_returns_error);
                                        if (!assignable_in_module(val_ty, field.type, module_path, program)) {
                                            error(diag, bv.location, std::format("type mismatch for field '{}'", field.name));
                                        }
                                    }
                                    for (size_t i = bv.values.size(); i < info.fields.size(); ++i) {
                                        if (info.fields[i].init_expr == nullptr) {
                                            error(diag, bv.location, std::format("missing field '{}' in struct initializer", info.fields[i].name));
                                        }
                                    }
                                    return *expected;
                                }
                                if (expected->kind != TypeKind::Array) {
                                    return error(diag, bv.location, "array initializer requires an array type");
                                }
                                const auto *array_info_ptr = program.array_at(expected->array_index);
                                if (!array_info_ptr) {
                                    return error(diag, bv.location, "internal error: invalid array index");
                                }
                                const auto &array_info = *array_info_ptr;
                                if (bv.values.size() > array_info.count) {
                                    return error(diag, bv.location, std::format("too many elements in array initializer: array has {} element(s), got {}", array_info.count, bv.values.size()));
                                }
                                for (const auto &val : bv.values) {
                                    const auto val_ty = check_expr(val, locals, module_path, program, diag, array_info.element_type, loop_depth, defer_loop_base, fn_returns_error);
                                    if (!assignable_in_module(val_ty, array_info.element_type, module_path, program)) {
                                        error(diag, bv.location, "type mismatch in array initializer element");
                                    }
                                }
                                return *expected;
                            }
                        },
                        *v);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::RangeExpr>>) {
                    // Range expressions are only valid as for-in operands; type-check bounds
                    // so expr_types is populated for codegen, then report the contextual error.
                    const auto upper_type = check_expr(v->upper, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_returns_error);
                    if (v->lower) {
                        check_expr(*v->lower, locals, module_path, program, diag, upper_type, loop_depth, defer_loop_base, fn_returns_error);
                    }
                    return error(diag, v->location, "range expression is only valid as a 'for-in' operand");
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::SpreadExpr>>) {
                    // Legal spreads are unwrapped and checked by check_call_args's variadic-tail
                    // handling *before* recursing into check_expr on the operand directly — this
                    // node is only ever visited here when the spread was in an illegal position
                    // (not the sole trailing argument of a native-variadic call).
                    check_expr(v->operand, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_returns_error);
                    return error(diag, v->location,
                        "'...' spread argument is only valid as the sole trailing argument in a call to a "
                        "function with a native '...T' variadic parameter");
                }
            },
            expr);

        program.modules.at(module_path).expr_types[get_expr_key(expr)] = ty;

        // Implicit tagged-union coercion: applies generically wherever an expected type is
        // threaded through check_expr (call args, return statements, var-decl initializers,
        // struct/array/union field init) — not special-cased to variadics, even though that's
        // the primary motivating use case. See VariantCoercion's doc comment in sema.hpp for why
        // this is recorded in a side table rather than overwriting expr_types for this node.
        if (const auto *union_info_ptr = expected && ty.kind != TypeKind::Invalid && ty != *expected && expected->kind == TypeKind::Union
                                              ? program.union_at(expected->union_index)
                                              : nullptr;
            union_info_ptr && union_info_ptr->is_tagged) {
            const auto &union_info = *union_info_ptr;
            {
                const TaggedUnionVariant *match = nullptr;
                std::vector<std::string> match_names;
                for (const auto &variant : union_info.variants) {
                    if (variant.payload_struct_index < 0) continue;
                    // Either the value's type is exactly the declared payload type (covers
                    // scalar/slice/pointer payloads directly, and struct payloads reused
                    // verbatim without wrapping — see layout_union in type_resolver.cpp), or
                    // the payload struct has exactly one field and the value matches that
                    // field's type (covers single-field struct payloads passed as a bare value).
                    if (variant.payload_type == ty) {
                        match = &variant;
                        match_names.push_back(variant.name);
                        continue;
                    }
                    const auto *payload_struct = program.struct_at(variant.payload_struct_index);
                    if (payload_struct && payload_struct->fields.size() == 1 && payload_struct->fields[0].type == ty) {
                        match = &variant;
                        match_names.push_back(variant.name);
                    }
                }
                if (match_names.size() == 1) {
                    program.modules.at(module_path).expr_variant_coercions[get_expr_key(expr)] = VariantCoercion{
                        .union_type = *expected,
                        .tag_value = match->tag_value,
                        .payload_struct_index = match->payload_struct_index,
                    };
                    return *expected;
                }
                if (match_names.size() > 1) {
                    std::string joined;
                    for (size_t i = 0; i < match_names.size(); ++i) {
                        if (i > 0) joined += ", ";
                        joined += match_names[i];
                    }
                    const auto [union_module, union_name] = find_type_module_and_name(*expected, program);
                    return error(diag, get_expr_location(expr), std::format(
                        "ambiguous implicit coercion to tagged union '{}': variants {} all accept a payload of this type; "
                        "use an explicit variant constructor",
                        union_name.empty() ? "<union>" : union_name, joined));
                }
            }
        }

        // Implicit pointer-to-trait-handle coercion: applies through the same
        // expected-type channel as the tagged-union coercion above (call args, return
        // statements, var-decl initializers, struct/array/union field init). The source
        // must be a pointer to a type that implements the trait; see TraitCoercion's doc
        // comment in sema.hpp for why this is recorded in a side table rather than
        // overwriting expr_types for this node.
        if (expected && expected->kind == TypeKind::Trait && ty.kind != TypeKind::Invalid && ty != *expected) {
            if (ty.kind != TypeKind::Pointer) {
                const auto [trait_module, trait_name] = find_type_module_and_name(*expected, program);
                return error(diag, get_expr_location(expr), std::format(
                    "cannot coerce non-pointer value to trait handle '{}'; a pointer to a type implementing the trait is required",
                    trait_name.empty() ? "<trait>" : trait_name));
            }

            const auto *pointee = program.pointee_at(ty.pointee_index);
            const auto [pointee_module, pointee_name] = pointee ? find_type_module_and_name(*pointee, program) : std::pair<std::string, std::string>{};

            bool implemented = false;
            if (const auto it = program.trait_impls_by_type.find({pointee_module, pointee_name}); it != program.trait_impls_by_type.end()) {
                for (const auto &impl_info : it->second) {
                    if (impl_info.trait_index == expected->trait_index) {
                        implemented = true;
                        break;
                    }
                }
            }

            if (!implemented) {
                const auto [trait_module, trait_name] = find_type_module_and_name(*expected, program);
                return error(diag, get_expr_location(expr), std::format(
                    "type '{}' does not implement trait '{}'",
                    pointee_name.empty() ? "<type>" : pointee_name, trait_name.empty() ? "<trait>" : trait_name));
            }

            program.modules.at(module_path).expr_trait_coercions[get_expr_key(expr)] = TraitCoercion{
                .trait_index = expected->trait_index,
            };
            return *expected;
        }

        return ty;
    }

    auto check_stmt(const ast::Stmt &stmt, LocalScope &locals, const std::string &module_path, Program &program, DiagnosticEngine &diag, const std::vector<ResolvedType> &expected_returns, int loop_depth, int defer_loop_base) -> void {
        const bool fn_returns_error = !expected_returns.empty() && expected_returns.back().kind == TypeKind::Error;
        std::visit(
            [&]<typename T>(const T &v) {
                using V = std::decay_t<T>;

                if constexpr (std::is_same_v<V, std::unique_ptr<ast::BlockStmt>>) {
                    auto inner = locals;
                    for (auto &s : v->stmts)
                        check_stmt(s, inner, module_path, program, diag, expected_returns, loop_depth, defer_loop_base);

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::IfStmt>>) {
                    check_expr(v->condition, locals, module_path, program, diag, ResolvedType{.kind = TypeKind::Bool}, loop_depth, defer_loop_base, fn_returns_error);
                    check_stmt(v->then_stmt, locals, module_path, program, diag, expected_returns, loop_depth, defer_loop_base);
                    if (v->else_stmt) check_stmt(*v->else_stmt, locals, module_path, program, diag, expected_returns, loop_depth, defer_loop_base);

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::WhileStmt>>) {
                    check_expr(v->condition, locals, module_path, program, diag, ResolvedType{.kind = TypeKind::Bool}, loop_depth, defer_loop_base, fn_returns_error);
                    check_stmt(v->body, locals, module_path, program, diag, expected_returns, loop_depth + 1, defer_loop_base);

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::ForInStmt>>) {
                    if (const auto *rp = std::get_if<std::unique_ptr<ast::RangeExpr>>(&v->iterable)) {
                        const auto &range = **rp;
                        if (v->element_by_ref) {
                            diag.report_error(DiagnosticStage::Sema, v->location, "range 'for-in' does not support '&' element binding");
                            return;
                        }
                        const auto upper_type = check_expr(range.upper, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_returns_error);
                        if (!upper_type.is_integer()) {
                            diag.report_error(DiagnosticStage::Sema, v->location, "range upper bound must be an integer type");
                            return;
                        }
                        if (range.lower) {
                            const auto lower_type = check_expr(*range.lower, locals, module_path, program, diag, upper_type, loop_depth, defer_loop_base, fn_returns_error);
                            if (lower_type != upper_type) {
                                diag.report_error(DiagnosticStage::Sema, v->location, "range lower and upper bounds must have the same type");
                                return;
                            }
                        }
                        auto inner = locals;
                        if (v->index_name != "_") {
                            inner[v->index_name] = LocalBinding{.type = ResolvedType{.kind = TypeKind::USize}, .is_mut = false};
                        }
                        if (v->element_name != "_") {
                            inner[v->element_name] = LocalBinding{.type = upper_type, .is_mut = false};
                        }
                        check_stmt(v->body, inner, module_path, program, diag, expected_returns, loop_depth + 1, defer_loop_base);
                        return;
                    }
                    const auto iterable_type = check_expr(v->iterable, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_returns_error);
                    if (iterable_type.kind != TypeKind::Slice && iterable_type.kind != TypeKind::Array) {
                        diag.report_error(DiagnosticStage::Sema, v->location, "'for-in' requires a slice or array operand");
                        return;
                    }
                    const auto elem_type = iterable_type.kind == TypeKind::Array
                        ? array_element_type(iterable_type, module_path, program)
                        : slice_element_type(iterable_type, module_path, program);
                    auto inner = locals;
                    if (v->index_name != "_") {
                        inner[v->index_name] = LocalBinding{.type = ResolvedType{.kind = TypeKind::USize}, .is_mut = false};
                    }
                    if (v->element_name != "_") {
                        if (v->element_by_ref) {
                            auto ptr_type = intern_pointer(program, elem_type);
                            inner[v->element_name] = LocalBinding{.type = ptr_type, .is_mut = false};
                        } else {
                            inner[v->element_name] = LocalBinding{.type = elem_type, .is_mut = false};
                        }
                    }
                    check_stmt(v->body, inner, module_path, program, diag, expected_returns, loop_depth + 1, defer_loop_base);

                } else if constexpr (std::is_same_v<V, ast::ExprStmt>) {
                    const auto expr_ty = check_expr(v.expr, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_returns_error);
                    // Detect ignored errors from fallible calls
                    if (expr_ty.kind == TypeKind::Error &&
                        !std::holds_alternative<std::unique_ptr<ast::TryExpr>>(v.expr)) {
                        diag.report_error(DiagnosticStage::Sema, v.location,
                            "error from fallible function call must be captured or propagated with 'try'");
                    }

                } else if constexpr (std::is_same_v<V, ast::VarDeclStmt>) {
                    ResolvedType declared_ty{.kind = TypeKind::Void};
                    bool has_declared_ty = false;
                    if (const auto resolved = resolve_declared_type(v.type, v.init, module_path, program, diag, v.location)) {
                        declared_ty = *resolved;
                        has_declared_ty = true;
                    }
                    if (v.init) {
                        if (!v.is_mut && contains_undefined(*v.init)) {
                            diag.report_error(DiagnosticStage::Sema, v.location, "'undefined' is not allowed in a 'const' declaration");
                        }
                        auto init_ty = check_expr(*v.init, locals, module_path, program, diag,
                                                  has_declared_ty ? std::optional(declared_ty) : std::nullopt, loop_depth, defer_loop_base, fn_returns_error);
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
                        check_expr(v.init, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_returns_error);
                        return;
                    }

                    auto returns = check_group_call_returns(*call, locals, module_path, program, diag, loop_depth, defer_loop_base, fn_returns_error);
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
                    const auto operand_type = check_expr(v->operand, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_returns_error);

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
                            check_expr(*lp.expr, locals, module_path, program, diag, operand_type, loop_depth, defer_loop_base, fn_returns_error);
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
                        const auto *union_info_ptr = program.union_at(operand_type.union_index);
                        if (!union_info_ptr) {
                            diag.report_error(DiagnosticStage::Sema, v->location, "internal error: invalid union index");
                            return;
                        }
                        const auto &union_info = *union_info_ptr;
                        if (!union_info.is_tagged) {
                            diag.report_error(DiagnosticStage::Sema, v->location, "switch operand must be an enum, tagged union, integer, or bool type");
                            return;
                        }
                        const bool any_ref_capture = std::ranges::any_of(v->arms, [](const auto &a) {
                            const auto *vp = std::get_if<ast::MatchExpr::VariantPattern>(&a.pattern);
                            return vp && vp->capture_by_ref;
                        });
                        if (any_ref_capture) {
                            const auto lv = resolve_lvalue(v->operand, locals, module_path, program, diag, loop_depth, defer_loop_base, fn_returns_error);
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
                                            const ResolvedType payload_ty = variant.payload_type;
                                            if (vp.capture_by_ref) {
                                                arm_locals[*vp.capture_name] = LocalBinding{
                                                    .type = intern_pointer(program, payload_ty),
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
                    const auto *enum_info_ptr = program.enum_at(operand_type.enum_index);
                    if (!enum_info_ptr) {
                        diag.report_error(DiagnosticStage::Sema, v->location, "internal error: invalid enum index");
                        return;
                    }
                    const auto &enum_info = *enum_info_ptr;
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
                    if (v.return_values.size() == 1 && expected_returns.size() > 1) {
                        if (const auto *call = std::get_if<std::unique_ptr<ast::CallExpr>>(&v.return_values[0])) {
                            const auto returns = check_group_call_returns(**call, locals, module_path, program, diag, loop_depth, defer_loop_base, fn_returns_error);
                            if (returns.size() != expected_returns.size()) {
                                diag.report_error(DiagnosticStage::Sema, v.location,
                                                  std::format("expected {} return value(s), got {}", expected_returns.size(), returns.size()));
                                return;
                            }
                            for (size_t i = 0; i < returns.size(); ++i) {
                                if (!assignable_in_module(returns[i], expected_returns[i], module_path, program)) {
                                    diag.report_error(DiagnosticStage::Sema, v.location, std::format("return value {} type mismatch", i + 1));
                                }
                            }
                            return;
                        }
                    }
                    if (v.return_values.size() != expected_returns.size()) {
                        diag.report_error(DiagnosticStage::Sema, v.location,
                                          std::format("expected {} return value(s), got {}", expected_returns.size(), v.return_values.size()));
                        for (auto &val : v.return_values)
                            check_expr(val, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_returns_error);
                        return;
                    }
                    for (size_t i = 0; i < v.return_values.size(); ++i) {
                        auto ty = check_expr(v.return_values[i], locals, module_path, program, diag, expected_returns[i], loop_depth, defer_loop_base, fn_returns_error);
                        if (!assignable_in_module(ty, expected_returns[i], module_path, program)) {
                            diag.report_error(DiagnosticStage::Sema, v.location, std::format("return value {} type mismatch", i + 1));
                        }
                    }

                } else if constexpr (std::is_same_v<V, ast::ReturnErrStmt>) {
                    if (defer_loop_base >= 0) {
                        diag.report_error(DiagnosticStage::Sema, v.location, "'return_err' cannot escape a 'defer' body");
                        return;
                    }
                    if (expected_returns.empty() || expected_returns.back().kind != TypeKind::Error) {
                        diag.report_error(DiagnosticStage::Sema, v.location,
                                          "enclosing function must return 'error' to use 'return_err'");
                        check_expr(v.error_value, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_returns_error);
                        return;
                    }
                    auto ty = check_expr(v.error_value, locals, module_path, program, diag,
                                         ResolvedType{.kind = TypeKind::Error}, loop_depth, defer_loop_base, fn_returns_error);
                    if (!assignable_in_module(ty, ResolvedType{.kind = TypeKind::Error}, module_path, program)) {
                        diag.report_error(DiagnosticStage::Sema, v.location, "'return_err' operand must be of type 'error'");
                    } else if (is_constant_expr(v.error_value, module_path, program)) {
                        if (const auto val = evaluate_integer_constant(v.error_value, module_path, program); val && *val == 0) {
                            diag.report_error(DiagnosticStage::Sema, v.location,
                                              "returning E_OK via return_err is certainly a bug; use return_ok or return 0");
                        }
                    }

                } else if constexpr (std::is_same_v<V, ast::ReturnOkStmt>) {
                    if (defer_loop_base >= 0) {
                        diag.report_error(DiagnosticStage::Sema, v.location, "'return_ok' cannot escape a 'defer' body");
                        return;
                    }
                    if (expected_returns.empty() || expected_returns.back().kind != TypeKind::Error) {
                        diag.report_error(DiagnosticStage::Sema, v.location,
                                          "enclosing function must return 'error' to use 'return_ok'");
                        for (auto &val : v.return_values)
                            check_expr(val, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_returns_error);
                        return;
                    }
                    const size_t expected_count = expected_returns.size() - 1;
                    if (v.return_values.size() != expected_count) {
                        diag.report_error(DiagnosticStage::Sema, v.location,
                                          std::format("expected {} return value(s), got {}", expected_count, v.return_values.size()));
                        for (auto &val : v.return_values)
                            check_expr(val, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_returns_error);
                        return;
                    }
                    for (size_t i = 0; i < v.return_values.size(); ++i) {
                        auto ty = check_expr(v.return_values[i], locals, module_path, program, diag, expected_returns[i], loop_depth, defer_loop_base, fn_returns_error);
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
