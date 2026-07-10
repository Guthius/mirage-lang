#include "sema.hpp"

#include <algorithm>
#include <format>
#include <unordered_map>

namespace sema {
    auto intern_pointer(Program &program, const ResolvedType &pointee) -> ResolvedType {
        for (size_t i = 0; i < program.pointer_pointees.size(); ++i) {
            if (program.pointer_pointees[i] == pointee) {
                return ResolvedType{.kind = TypeKind::Pointer, .pointee_index = static_cast<int>(i)};
            }
        }
        program.pointer_pointees.push_back(pointee);
        return ResolvedType{.kind = TypeKind::Pointer, .pointee_index = static_cast<int>(program.pointer_pointees.size()) - 1};
    }

    auto intern_function_type(Program &program, FunctionTypeInfo sig) -> ResolvedType {
        for (size_t i = 0; i < program.fn_signatures.size(); ++i) {
            const auto &s = program.fn_signatures[i];
            if (s.is_variadic == sig.is_variadic &&
                s.param_types == sig.param_types &&
                s.return_types == sig.return_types) {
                return ResolvedType{.kind = TypeKind::Function, .fn_index = static_cast<int>(i)};
            }
        }
        program.fn_signatures.push_back(std::move(sig));
        return ResolvedType{.kind = TypeKind::Function, .fn_index = static_cast<int>(program.fn_signatures.size()) - 1};
    }

    auto intern_slice(Program &program, const ResolvedType &element) -> ResolvedType {
        for (size_t i = 0; i < program.slices.size(); ++i) {
            if (program.slices[i].element_type == element) {
                return ResolvedType{.kind = TypeKind::Slice, .slice_index = static_cast<int>(i)};
            }
        }
        program.slices.push_back(SliceInfo{.element_type = element});
        return ResolvedType{.kind = TypeKind::Slice, .slice_index = static_cast<int>(program.slices.size()) - 1};
    }

    namespace {
        auto intern_array(Program &program, const ResolvedType &element, const uint64_t count, const uint32_t size, const uint32_t align) -> ResolvedType {
            for (size_t i = 0; i < program.arrays.size(); ++i) {
                if (program.arrays[i].element_type == element && program.arrays[i].count == count) {
                    return ResolvedType{.kind = TypeKind::Array, .array_index = static_cast<int>(i)};
                }
            }
            program.arrays.push_back(ArrayInfo{.element_type = element, .count = count, .size = size, .align = align});
            return ResolvedType{.kind = TypeKind::Array, .array_index = static_cast<int>(program.arrays.size()) - 1};
        }

        auto primitive_size(const TypeKind kind) -> uint32_t {
            switch (kind) {
            case TypeKind::U8:
            case TypeKind::I8:
            case TypeKind::Bool:
                return 1;

            case TypeKind::U16:
            case TypeKind::I16:
                return 2;

            case TypeKind::U32:
            case TypeKind::I32:
            case TypeKind::F32:
                return 4;

            case TypeKind::U64:
            case TypeKind::I64:
            case TypeKind::F64:
            case TypeKind::USize:
            case TypeKind::Error:
            case TypeKind::Pointer:
            case TypeKind::Anyptr:
            case TypeKind::Function: // code pointer, 8 bytes
                return 8;

            case TypeKind::Slice:
                return 16;

            default:
                return 0;
            }
        }

        auto primitive_align(const TypeKind kind) -> uint32_t { return primitive_size(kind); }

        auto error(DiagnosticEngine &diag, const SourceLocation &loc, std::string msg) -> ResolvedType {
            diag.report_error(DiagnosticStage::Sema, loc, std::move(msg));
            return ResolvedType{
                .kind = TypeKind::Invalid,
            };
        }

        auto expr_location(const ast::Expr &expr) -> SourceLocation {
            return std::visit(
                []<typename T>(const T &v) -> SourceLocation {
                    using V = std::decay_t<T>;
                    if constexpr (requires { v.location; }) {
                        return v.location;
                    } else if constexpr (requires { v->location; }) {
                        return v->location;
                    } else {
                        return {};
                    }
                },
                expr);
        }

        struct ChainTarget {
            std::string module_path;
            std::string name;
            bool crossed_boundary;
        };

        auto walk_namespace_chain(const std::string &start_module, const ast::NamedType &named, Program &program, DiagnosticEngine &diag) -> std::optional<ChainTarget> {
            std::string current_module = start_module;
            const auto *current = &named;
            bool crossed = false;

            while (current->member != nullptr) {
                auto mod_it = program.modules.find(current_module);
                if (mod_it == program.modules.end()) {
                    diag.report_error(DiagnosticStage::Sema, current->location, std::format("internal error: module '{}' not found", current_module));
                    return std::nullopt;
                }

                auto sym_it = mod_it->second.symbols.find(current->name);
                if (sym_it == mod_it->second.symbols.end()) {
                    diag.report_error(DiagnosticStage::Sema, current->location, std::format("unknown identifier '{}'", current->name));
                    return std::nullopt;
                }

                const auto *imp = std::get_if<ImportSymbol>(&sym_it->second);
                if (!imp) {
                    diag.report_error(DiagnosticStage::Sema, current->location, std::format("'{}' is not a namespace", current->name));
                    return std::nullopt;
                }

                if (crossed && !imp->is_pub) {
                    diag.report_error(DiagnosticStage::Sema, current->location, std::format("'{}' is not pub", current->name));
                    return std::nullopt;
                }

                current_module = imp->module_path;
                current = current->member.get();
                crossed = true;
            }

            return ChainTarget{current_module, current->name, crossed};
        }

        struct Resolver {
            Program &program;
            DiagnosticEngine &diag;

            auto find_type_symbol(ProgramModule &mod, const std::string &name, const SourceLocation &loc) const -> TypeSymbol * {
                const auto it = mod.symbols.find(name);
                if (it == mod.symbols.end()) {
                    error(diag, loc, std::format("unknown type '{}'", name));
                    return nullptr;
                }

                auto *ts = std::get_if<TypeSymbol>(&it->second);
                if (!ts) {
                    error(diag, loc, std::format("'{}' is not a type", name));
                    return nullptr;
                }

                return ts;
            }

            [[nodiscard]] auto resolve_final_shallow(const std::string &module_path, const std::string &name, const bool check_pub, const SourceLocation &loc) const -> ResolvedType {
                const auto mod_it = program.modules.find(module_path);
                if (mod_it == program.modules.end()) {
                    return error(diag, loc, std::format("internal error: module '{}' not found", module_path));
                }

                auto &mod = mod_it->second;

                auto *ts = find_type_symbol(mod, name, loc);
                if (!ts) {
                    return ResolvedType{.kind = TypeKind::Invalid};
                }

                if (check_pub && !ts->is_pub) {
                    return error(diag, loc, std::format("'{}' is not pub", name));
                }

                if (ts->resolved) {
                    return *ts->resolved;
                }

                const auto key = std::make_pair(module_path, name);
                if (program.resolve_state.alias_resolving.contains(key)) {
                    return error(diag, loc, std::format("type alias cycle detected at '{}'", name));
                }

                program.resolve_state.alias_resolving.insert(key);
                Resolver inner{program, diag};
                auto resolved = inner.resolve_type_impl(ts->decl->type, module_path);
                program.resolve_state.alias_resolving.erase(key);

                ts->resolved = resolved;
                return resolved;
            }

            [[nodiscard]] auto resolve_final_full(const std::string &module_path, const std::string &name, const bool check_pub, const SourceLocation &loc) const -> ResolvedType {
                const auto mod_it = program.modules.find(module_path);
                if (mod_it == program.modules.end()) {
                    return error(diag, loc, std::format("internal error: module '{}' not found", module_path));
                }
                ProgramModule &mod = mod_it->second;

                const TypeSymbol *ts = find_type_symbol(mod, name, loc);
                if (!ts) return ResolvedType{.kind = TypeKind::Invalid};
                if (check_pub && !ts->is_pub) {
                    return error(diag, loc, std::format("'{}' is not pub", name));
                }

                if (ts->resolved && ts->resolved->kind == TypeKind::Struct) {
                    const int slot = ts->resolved->struct_index;
                    if (program.structs[slot].layout_done) return *ts->resolved;

                    const auto key = std::make_pair(module_path, name);
                    if (program.resolve_state.struct_resolving.contains(key)) {
                        return error(diag, loc, std::format("by-value struct cycle detected at '{}'", name));
                    }

                    program.resolve_state.struct_resolving.insert(key);
                    Resolver inner{program, diag};
                    inner.layout_struct(module_path, slot, std::get<std::unique_ptr<ast::StructType>>(ts->decl->type));
                    program.resolve_state.struct_resolving.erase(key);

                    return *ts->resolved;
                }

                if (ts->resolved && ts->resolved->kind == TypeKind::Enum) {
                    const int slot = ts->resolved->enum_index;
                    if (program.enums[slot].layout_done) return *ts->resolved;

                    Resolver inner{program, diag};
                    inner.layout_enum(module_path, slot, std::get<std::unique_ptr<ast::EnumType>>(ts->decl->type));
                    return *ts->resolved;
                }

                if (ts->resolved && ts->resolved->kind == TypeKind::Union) {
                    const int slot = ts->resolved->union_index;
                    if (program.unions[slot].layout_done) return *ts->resolved;

                    const auto key = std::make_pair(module_path, name);
                    if (program.resolve_state.union_resolving.contains(key)) {
                        return error(diag, loc, std::format("by-value union cycle detected at '{}'", name));
                    }

                    program.resolve_state.union_resolving.insert(key);
                    Resolver inner{program, diag};
                    inner.layout_union(module_path, slot, std::get<std::unique_ptr<ast::UnionType>>(ts->decl->type));
                    program.resolve_state.union_resolving.erase(key);

                    return *ts->resolved;
                }

                return resolve_final_shallow(module_path, name, check_pub, loc);
            }

            void layout_struct(const std::string &module_path, const int slot, const std::unique_ptr<ast::StructType> &decl) {
                StructInfo info;
                info.module_path = module_path;
                info.is_packed = decl->is_packed;

                uint32_t offset = 0;
                uint32_t max_align = 1;

                for (auto &field : decl->fields) {
                    auto field_type = resolve_field_type(module_path, field.type, field.location);
                    const uint32_t f_size = size_of(module_path, field_type);
                    uint32_t f_align = decl->is_packed ? 1 : align_of(module_path, field_type);

                    if (!decl->is_packed && f_align > 0) {
                        offset = (offset + f_align - 1) / f_align * f_align;
                    }

                    info.fields.push_back(StructField{
                        .name = field.name,
                        .type = field_type,
                        .offset = offset,
                        .init_expr = field.init ? &field.init.value() : nullptr,
                    });
                    offset += f_size;
                    max_align = std::max(max_align, f_align);
                }

                info.size = decl->is_packed ? offset : (offset + max_align - 1) / max_align * max_align;
                info.align = decl->is_packed ? 1 : max_align;
                info.layout_done = true;

                program.structs[slot] = std::move(info);
            }

            auto resolve_field_type(const std::string &module_path, const ast::Type &field_type, SourceLocation loc) -> ResolvedType {
                if (auto *named = std::get_if<ast::NamedType>(&field_type)) {
                    const auto target = walk_namespace_chain(module_path, *named, program, diag);
                    if (!target) {
                        return ResolvedType{.kind = TypeKind::Invalid};
                    }
                    return resolve_final_full(target->module_path, target->name, target->crossed_boundary, loc);
                }
                return resolve_type_impl(field_type, module_path);
            }

            [[nodiscard]] auto size_of(const std::string &module_path, const ResolvedType &t) const -> uint32_t {
                if (t.kind == TypeKind::Struct) return program.structs[t.struct_index].size;
                if (t.kind == TypeKind::Array) return program.arrays[t.array_index].size;
                if (t.kind == TypeKind::Slice) return 16;
                if (t.kind == TypeKind::Enum) return primitive_size(program.enums[t.enum_index].underlying_type.kind);
                if (t.kind == TypeKind::Union) return program.unions[t.union_index].size;
                return primitive_size(t.kind);
            }

            [[nodiscard]] auto align_of(const std::string &module_path, const ResolvedType &t) const -> uint32_t {
                if (t.kind == TypeKind::Struct) return program.structs[t.struct_index].align;
                if (t.kind == TypeKind::Array) return program.arrays[t.array_index].align;
                if (t.kind == TypeKind::Slice) return 8;
                if (t.kind == TypeKind::Enum) return primitive_align(program.enums[t.enum_index].underlying_type.kind);
                if (t.kind == TypeKind::Union) return program.unions[t.union_index].align;
                return primitive_align(t.kind);
            }

            auto sizeof_expr_operand(const std::string &module_path, const ast::SizeOfExpr &expr) -> uint64_t {
                if (const auto *ident = std::get_if<ast::IdentExpr>(&expr.operand)) {
                    auto &mod = program.modules.at(module_path);
                    if (const auto it = mod.symbols.find(ident->name); it != mod.symbols.end()) {
                        if (std::holds_alternative<TypeSymbol>(it->second)) {
                            return size_of(module_path, resolve_type_symbol(module_path, ident->name, program, diag, ident->location));
                        }
                    }
                }

                if (const auto *member = std::get_if<std::unique_ptr<ast::MemberExpr>>(&expr.operand)) {
                    if (const auto target = walk_namespace_chain(module_path, as_named_member(**member), program, diag)) {
                        const auto &mod = program.modules.at(target->module_path);
                        if (const auto it = mod.symbols.find(target->name); it != mod.symbols.end()) {
                            if (std::holds_alternative<TypeSymbol>(it->second)) {
                                return size_of(target->module_path, resolve_type_symbol(target->module_path, target->name, program, diag, (*member)->location));
                            }
                        }
                    }
                }

                LocalScope no_locals;
                const auto operand_type = check_expr(expr.operand, no_locals, module_path, program, diag, std::nullopt, 0);
                return size_of(module_path, operand_type);
            }

            static auto as_named_member(const ast::MemberExpr &member) -> ast::NamedType {
                std::vector<std::pair<std::string, SourceLocation>> parts;
                const auto collect = [&](this const auto &self, const ast::Expr &expr) -> bool {
                    if (const auto *ident = std::get_if<ast::IdentExpr>(&expr)) {
                        parts.emplace_back(ident->name, ident->location);
                        return true;
                    }
                    if (const auto *inner = std::get_if<std::unique_ptr<ast::MemberExpr>>(&expr)) {
                        if (!self((*inner)->object)) return false;
                        parts.emplace_back((*inner)->member, (*inner)->location);
                        return true;
                    }
                    return false;
                };

                if (!collect(member.object)) return ast::NamedType{.name = member.member, .location = member.location};
                parts.emplace_back(member.member, member.location);

                ast::NamedType result{
                    .name = parts.front().first,
                    .location = parts.front().second,
                };
                ast::NamedType *tail = &result;
                for (size_t i = 1; i < parts.size(); ++i) {
                    tail->member = std::make_unique<ast::NamedType>(ast::NamedType{
                        .name = parts[i].first,
                        .location = parts[i].second,
                    });
                    tail = tail->member.get();
                }

                return result;
            }

            static auto contains_iota(const ast::Expr &expr) -> bool {
                return std::visit(
                    []<typename T>(const T &v) -> bool {
                        using V = std::decay_t<T>;
                        if constexpr (std::is_same_v<V, ast::IotaExpr>) {
                            return true;
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::UnaryExpr>>) {
                            return contains_iota(v->operand);
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::BinaryExpr>>) {
                            return contains_iota(v->lhs) || contains_iota(v->rhs);
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::TernaryExpr>>) {
                            return contains_iota(v->condition) || contains_iota(v->then_expr) || contains_iota(v->else_expr);
                        } else {
                            return false;
                        }
                    },
                    expr);
            }

            auto eval_integer_const_expr(const ast::Expr &expr, const std::string &module_path, const std::unordered_map<std::string, const ast::Expr *> &macro_args, uint64_t iota_value = 0) -> std::optional<uint64_t> {
                return std::visit(
                    [&]<typename T>(const T &v) -> std::optional<uint64_t> {
                        using V = std::decay_t<T>;

                        if constexpr (std::is_same_v<V, ast::IotaExpr>) {
                            return iota_value;

                        } else if constexpr (std::is_same_v<V, ast::LiteralIntegerExpr>) {
                            return v.value;

                        } else if constexpr (std::is_same_v<V, ast::LiteralBoolExpr>) {
                            return v.value ? 1 : 0;

                        } else if constexpr (std::is_same_v<V, ast::IdentExpr>) {
                            if (const auto arg = macro_args.find(v.name); arg != macro_args.end()) {
                                return eval_integer_const_expr(*arg->second, module_path, macro_args);
                            }

                            auto &mod = program.modules.at(module_path);
                            const auto sym_it = mod.symbols.find(v.name);
                            if (sym_it == mod.symbols.end()) return std::nullopt;

                            const auto *global = std::get_if<GlobalSymbol>(&sym_it->second);
                            if (!global || global->is_mut || !global->decl->init) return std::nullopt;
                            resolve_global_symbol(module_path, v.name, program, diag, v.location);
                            return eval_integer_const_expr(*global->decl->init, module_path, macro_args);

                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::SizeOfExpr>>) {
                            return sizeof_expr_operand(module_path, *v);

                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::CastExpr>>) {
                            return eval_integer_const_expr(v->value, module_path, macro_args);

                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::UnaryExpr>>) {
                            const auto operand = eval_integer_const_expr(v->operand, module_path, macro_args);
                            if (!operand) return std::nullopt;
                            switch (v->op) {
                            case ast::UnaryOp::Negate:     return uint64_t{0} - *operand;
                            case ast::UnaryOp::BitwiseNot: return ~*operand;
                            case ast::UnaryOp::LogicalNot: return *operand == 0 ? 1 : 0;
                            default:                       return std::nullopt;
                            }

                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::BinaryExpr>>) {
                            const auto lhs = eval_integer_const_expr(v->lhs, module_path, macro_args);
                            const auto rhs = eval_integer_const_expr(v->rhs, module_path, macro_args);
                            if (!lhs || !rhs) return std::nullopt;
                            switch (v->op) {
                            case ast::BinaryOp::Add:          return *lhs + *rhs;
                            case ast::BinaryOp::Sub:          return *lhs - *rhs;
                            case ast::BinaryOp::Mul:          return *lhs * *rhs;
                            case ast::BinaryOp::Div:          return *rhs == 0 ? std::nullopt : std::optional<uint64_t>{*lhs / *rhs};
                            case ast::BinaryOp::Mod:          return *rhs == 0 ? std::nullopt : std::optional<uint64_t>{*lhs % *rhs};
                            case ast::BinaryOp::BitwiseAnd:   return *lhs & *rhs;
                            case ast::BinaryOp::BitwiseOr:    return *lhs | *rhs;
                            case ast::BinaryOp::BitwiseXor:   return *lhs ^ *rhs;
                            case ast::BinaryOp::ShiftLeft:    return *rhs >= 64 ? std::nullopt : std::optional<uint64_t>{*lhs << *rhs};
                            case ast::BinaryOp::ShiftRight:   return *rhs >= 64 ? std::nullopt : std::optional<uint64_t>{*lhs >> *rhs};
                            case ast::BinaryOp::Equal:        return *lhs == *rhs ? 1 : 0;
                            case ast::BinaryOp::NotEqual:     return *lhs != *rhs ? 1 : 0;
                            case ast::BinaryOp::Less:         return *lhs < *rhs ? 1 : 0;
                            case ast::BinaryOp::Greater:      return *lhs > *rhs ? 1 : 0;
                            case ast::BinaryOp::LessEqual:    return *lhs <= *rhs ? 1 : 0;
                            case ast::BinaryOp::GreaterEqual: return *lhs >= *rhs ? 1 : 0;
                            case ast::BinaryOp::LogicalAnd:   return (*lhs != 0 && *rhs != 0) ? 1 : 0;
                            case ast::BinaryOp::LogicalOr:    return (*lhs != 0 || *rhs != 0) ? 1 : 0;
                            }
                            return std::nullopt;

                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::TernaryExpr>>) {
                            const auto cond = eval_integer_const_expr(v->condition, module_path, macro_args);
                            if (!cond) return std::nullopt;
                            return eval_integer_const_expr(*cond != 0 ? v->then_expr : v->else_expr, module_path, macro_args);

                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::CallExpr>>) {
                            const auto *callee = std::get_if<ast::IdentExpr>(&v->callee);
                            if (!callee) return std::nullopt;
                            auto &mod = program.modules.at(module_path);
                            const auto sym_it = mod.symbols.find(callee->name);
                            if (sym_it == mod.symbols.end()) return std::nullopt;
                            auto *macro = std::get_if<MacroSymbol>(&sym_it->second);
                            if (!macro) return std::nullopt;
                            auto &resolved_macro = resolve_macro_symbol(module_path, callee->name, program, diag, callee->location);

                            std::unordered_map<std::string, const ast::Expr *> nested_args = macro_args;
                            for (size_t i = 0; i < resolved_macro.decl->params.size(); ++i) {
                                nested_args[resolved_macro.decl->params[i].name] = &v->args[i];
                            }
                            return eval_integer_const_expr(resolved_macro.decl->expr_template, module_path, nested_args);

                        } else {
                            return std::nullopt;
                        }
                    },
                    expr);
            }

            auto array_len_expr_value(const ast::Expr &expr, const std::string &module_path) -> uint64_t {
                LocalScope no_locals;
                const auto len_type = check_expr(expr, no_locals, module_path, program, diag, ResolvedType{.kind = TypeKind::USize}, 0);
                if (!len_type.is_integer()) {
                    diag.report_error(DiagnosticStage::Sema, expr_location(expr), "array length must be an integer constant expression");
                    return 0;
                }
                if (!is_constant_expr(expr, module_path, program)) {
                    diag.report_error(DiagnosticStage::Sema, expr_location(expr), "array length must be a compile-time constant expression");
                    return 0;
                }
                if (const auto value = eval_integer_const_expr(expr, module_path, {})) {
                    return *value;
                }
                diag.report_error(DiagnosticStage::Sema, expr_location(expr), "array length constant expression could not be evaluated");
                return 0;
            }

            void layout_enum(const std::string &module_path, const int slot, const std::unique_ptr<ast::EnumType> &decl) {
                auto &mod = program.modules.at(module_path);

                // Resolve underlying type (default: i32)
                ResolvedType underlying;
                if (decl->underlying_type) {
                    underlying = resolve_type_impl(*decl->underlying_type, module_path);
                } else {
                    underlying = ResolvedType{.kind = TypeKind::I32};
                }

                EnumInfo info;
                info.underlying_type = underlying;

                uint64_t iota_counter = 0;
                const ast::Expr *iota_template = nullptr; // nullptr = just use iota value directly

                for (const auto &field : decl->fields) {
                    int64_t field_value;
                    if (field.init) {
                        const auto result = eval_integer_const_expr(*field.init, module_path, {}, iota_counter);
                        if (!result) {
                            diag.report_error(DiagnosticStage::Sema, field.location, std::format("enum field '{}' is not a compile-time constant", field.name));
                            field_value = static_cast<int64_t>(iota_counter);
                        } else {
                            field_value = static_cast<int64_t>(*result);
                        }
                        if (contains_iota(*field.init)) {
                            iota_template = &*field.init;
                        } else {
                            iota_template = nullptr;
                        }
                    } else {
                        if (iota_template) {
                            const auto result = eval_integer_const_expr(*iota_template, module_path, {}, iota_counter);
                            field_value = result ? static_cast<int64_t>(*result) : static_cast<int64_t>(iota_counter);
                        } else {
                            field_value = static_cast<int64_t>(iota_counter);
                        }
                    }

                    for (const auto &existing : info.fields) {
                        if (existing.name == field.name) {
                            diag.report_error(DiagnosticStage::Sema, field.location, std::format("duplicate enum field name '{}'", field.name));
                        }
                    }

                    info.fields.push_back(EnumFieldInfo{.name = field.name, .value = field_value});
                    ++iota_counter;
                }

                info.layout_done = true;
                program.enums[slot] = std::move(info);
            }

            void layout_union(const std::string &module_path, const int slot, const std::unique_ptr<ast::UnionType> &decl) {
                UnionInfo info;
                info.module_path = module_path;
                info.is_tagged = decl->is_tagged;

                if (decl->is_tagged) {
                    // Tagged union: tag (u32) + optional payload
                    // Layout: [u32 tag | padding | max_payload bytes]
                    static constexpr uint32_t TAG_SIZE = 4;
                    static constexpr uint32_t TAG_ALIGN = 4;

                    uint32_t max_payload_size = 0;
                    uint32_t max_payload_align = 1;

                    for (int32_t i = 0; i < static_cast<int32_t>(decl->members.size()); ++i) {
                        const auto &member = decl->members[i];
                        TaggedUnionVariant variant;
                        variant.name = member.name;
                        variant.tag_value = i;
                        variant.payload_struct_index = -1;

                        if (!std::holds_alternative<std::monostate>(member.type)) {
                            // Variant has a payload type. Struct payloads (whether an inline
                            // `struct{...}` or a reference to a separately-named struct type) use
                            // their own fields directly for ergonomics; any other payload type is
                            // wrapped in a synthetic one-field struct named "v".
                            int struct_slot;

                            if (const auto *st = std::get_if<std::unique_ptr<ast::StructType>>(&member.type)) {
                                // Inline struct payload — allocate an anonymous struct slot for it.
                                struct_slot = static_cast<int>(program.structs.size());
                                program.structs.push_back(StructInfo{.module_path = module_path});
                                layout_struct(module_path, struct_slot, *st);
                                variant.payload_type = ResolvedType{.kind = TypeKind::Struct, .struct_index = struct_slot};
                            } else {
                                auto payload_type = resolve_field_type(module_path, member.type, member.location);
                                if (payload_type.kind == TypeKind::Struct) {
                                    // Named struct payload — reuse its own slot, no wrapping.
                                    struct_slot = payload_type.struct_index;
                                } else {
                                    // Non-struct payload: create a one-field anonymous struct
                                    struct_slot = static_cast<int>(program.structs.size());
                                    program.structs.push_back(StructInfo{.module_path = module_path});
                                    StructInfo payload_info;
                                    payload_info.module_path = module_path;
                                    const uint32_t p_size = size_of(module_path, payload_type);
                                    const uint32_t p_align = align_of(module_path, payload_type);
                                    payload_info.fields.push_back(StructField{
                                        .name = "v",
                                        .type = payload_type,
                                        .offset = 0,
                                        .init_expr = nullptr,
                                    });
                                    payload_info.size = p_size;
                                    payload_info.align = p_align;
                                    payload_info.layout_done = true;
                                    program.structs[struct_slot] = std::move(payload_info);
                                }
                                variant.payload_type = payload_type;
                            }

                            variant.payload_struct_index = struct_slot;
                            const auto &payload_struct = program.structs[struct_slot];
                            max_payload_size = std::max(max_payload_size, payload_struct.size);
                            max_payload_align = std::max(max_payload_align, payload_struct.align);
                        }

                        info.variants.push_back(std::move(variant));
                    }

                    // payload_offset = align_up(TAG_SIZE, max_payload_align)
                    const uint32_t effective_payload_align = std::max(max_payload_align, 1u);
                    info.payload_offset = (TAG_SIZE + effective_payload_align - 1) / effective_payload_align * effective_payload_align;

                    // Total align = max(tag_align, payload_align)
                    info.align = std::max(TAG_ALIGN, max_payload_align);
                    // Total size = align_up(payload_offset + max_payload_size, align)
                    const uint32_t raw_size = info.payload_offset + max_payload_size;
                    info.size = (raw_size + info.align - 1) / info.align * info.align;
                    if (info.size == 0) info.size = TAG_ALIGN; // minimum size for tag-only unions
                } else {
                    uint32_t max_size = 0;
                    uint32_t max_align = 1;

                    for (const auto &member : decl->members) {
                        auto member_type = resolve_field_type(module_path, member.type, member.location);
                        const uint32_t m_size = size_of(module_path, member_type);
                        const uint32_t m_align = align_of(module_path, member_type);

                        info.members.push_back(UnionMember{
                            .name = member.name,
                            .type = member_type,
                        });
                        max_size = std::max(max_size, m_size);
                        max_align = std::max(max_align, m_align);
                    }

                    // Union size = largest member size, rounded up to alignment
                    info.size = (max_size + max_align - 1) / max_align * max_align;
                    info.align = max_align;
                }

                info.layout_done = true;
                program.unions[slot] = std::move(info);
            }

            auto resolve_type_impl(const ast::Type &type, const std::string &module_path) -> ResolvedType {
                return std::visit(
                    [&]<typename T>(const T &v) -> ResolvedType {
                        using V = std::decay_t<T>;

                        if constexpr (std::is_same_v<V, std::monostate>) {
                            return ResolvedType{.kind = TypeKind::Invalid};

                        } else if constexpr (std::is_same_v<V, ast::BuiltinType>) {
                            return resolve_builtin(v.kind);

                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::PointerType>>) {
                            ResolvedType pointee;
                            if (auto *named = std::get_if<ast::NamedType>(&v->pointee)) {
                                auto target = walk_namespace_chain(module_path, *named, program, diag);
                                pointee = target
                                              ? resolve_final_shallow(target->module_path, target->name, target->crossed_boundary, v->location)
                                              : ResolvedType{.kind = TypeKind::Invalid};
                            } else {
                                pointee = resolve_type_impl(v->pointee, module_path);
                            }
                            return intern_pointer(program, pointee);

                        } else if constexpr (std::is_same_v<V, ast::NamedType>) {
                            auto target = walk_namespace_chain(module_path, v, program, diag);
                            if (!target) return ResolvedType{.kind = TypeKind::Invalid};
                            return resolve_final_shallow(target->module_path, target->name, target->crossed_boundary, v.location);

                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::StructType>>) {
                            int slot = static_cast<int>(program.structs.size());
                            program.structs.push_back(StructInfo{.module_path = module_path});
                            layout_struct(module_path, slot, v);
                            return ResolvedType{.kind = TypeKind::Struct, .struct_index = slot};

                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::ArrayType>>) {
                            if (!v->size.has_value()) {
                                return error(diag, v->location,
                                    "array type '[?]T' can only be used as the declared type of a 'const'/'let' declaration with an array literal initializer");
                            }
                            auto element = resolve_type_impl(v->base_type, module_path);
                            const auto count = array_len_expr_value(*v->size, module_path);
                            return intern_array(program, element, count, size_of(module_path, element) * count, align_of(module_path, element));

                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::SliceType>>) {
                            auto element = resolve_type_impl(v->base_type, module_path);
                            return intern_slice(program, element);

                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::EnumType>>) {
                            const int slot = static_cast<int>(program.enums.size());
                            program.enums.push_back(EnumInfo{});
                            layout_enum(module_path, slot, v);
                            return ResolvedType{.kind = TypeKind::Enum, .enum_index = slot};
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::UnionType>>) {
                            const int slot = static_cast<int>(program.unions.size());
                            program.unions.push_back(UnionInfo{.module_path = module_path});
                            layout_union(module_path, slot, v);
                            return ResolvedType{.kind = TypeKind::Union, .union_index = slot};
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::FunctionType>>) {
                            FunctionTypeInfo sig;
                            sig.is_variadic = v->is_variadic;
                            for (const auto &pt : v->param_types) {
                                sig.param_types.push_back(resolve_type_impl(pt, module_path));
                            }
                            for (const auto &rt : v->return_types) {
                                sig.return_types.push_back(resolve_type_impl(rt, module_path));
                            }
                            return intern_function_type(program, std::move(sig));
                        } else {
                            return ResolvedType{.kind = TypeKind::Invalid};
                        }
                    },
                    type);
            }

            static auto resolve_builtin(const ast::BuiltinTypeKind kind) -> ResolvedType {
                switch (kind) {
                case ast::BuiltinTypeKind::U8:     return ResolvedType{.kind = TypeKind::U8};
                case ast::BuiltinTypeKind::U16:    return ResolvedType{.kind = TypeKind::U16};
                case ast::BuiltinTypeKind::U32:    return ResolvedType{.kind = TypeKind::U32};
                case ast::BuiltinTypeKind::U64:    return ResolvedType{.kind = TypeKind::U64};
                case ast::BuiltinTypeKind::I8:     return ResolvedType{.kind = TypeKind::I8};
                case ast::BuiltinTypeKind::I16:    return ResolvedType{.kind = TypeKind::I16};
                case ast::BuiltinTypeKind::I32:    return ResolvedType{.kind = TypeKind::I32};
                case ast::BuiltinTypeKind::I64:    return ResolvedType{.kind = TypeKind::I64};
                case ast::BuiltinTypeKind::F32:    return ResolvedType{.kind = TypeKind::F32};
                case ast::BuiltinTypeKind::F64:    return ResolvedType{.kind = TypeKind::F64};
                case ast::BuiltinTypeKind::Usize:  return ResolvedType{.kind = TypeKind::USize};
                case ast::BuiltinTypeKind::Bool:   return ResolvedType{.kind = TypeKind::Bool};
                case ast::BuiltinTypeKind::Byte:   return ResolvedType{.kind = TypeKind::U8};
                case ast::BuiltinTypeKind::Error:  return ResolvedType{.kind = TypeKind::Error};
                case ast::BuiltinTypeKind::Anyptr: return ResolvedType{.kind = TypeKind::Anyptr};
                case ast::BuiltinTypeKind::Type:   return ResolvedType{.kind = TypeKind::Void};
                }

                return ResolvedType{.kind = TypeKind::Invalid};
            }
        };
    }

    auto is_assignable(const ResolvedType &from, const ResolvedType &to) -> bool {
        if (from == to) return true;
        if (from.kind == TypeKind::Anyptr && to.kind == TypeKind::Pointer) return true;
        if (from.kind == TypeKind::Pointer && to.kind == TypeKind::Anyptr) return true;
        // nil (Anyptr) is assignable to/from function pointer types
        if (from.kind == TypeKind::Anyptr && to.kind == TypeKind::Function) return true;
        if (from.kind == TypeKind::Function && to.kind == TypeKind::Anyptr) return true;
        if (from.kind == TypeKind::Array && to.kind == TypeKind::Slice) return true;
        if (from.kind == TypeKind::Slice && to.kind == TypeKind::Array) return true;
        if (from.kind == TypeKind::Anyptr && to.kind == TypeKind::Slice) return true;
        if (from.kind == TypeKind::Slice && to.kind == TypeKind::Pointer) return true;
        if (from.kind == TypeKind::Slice && to.kind == TypeKind::Anyptr) return true;
        return false;
    }

    auto resolve_type(const ast::Type &type, const std::string &module_path, Program &program, DiagnosticEngine &diag) -> ResolvedType {
        Resolver resolver{program, diag};
        return resolver.resolve_type_impl(type, module_path);
    }

    auto resolve_declared_type(const std::optional<ast::Type> &type, const std::optional<ast::Expr> &init,
                                const std::string &module_path, Program &program, DiagnosticEngine &diag,
                                const SourceLocation &decl_loc) -> std::optional<ResolvedType> {
        if (!type) return std::nullopt;

        const auto *array_type = std::get_if<std::unique_ptr<ast::ArrayType>>(&*type);
        if (!array_type || (*array_type)->size.has_value()) {
            return resolve_type(*type, module_path, program, diag); // unchanged behavior
        }

        // '[?]T': infer the element count from a literal array initializer.
        if (!init) {
            return error(diag, decl_loc, "cannot infer array length: '[?]' array declaration requires an initializer");
        }
        const auto *braced = std::get_if<std::unique_ptr<ast::BracedInitializerExpr>>(&*init);
        const auto *array_lit = braced ? std::get_if<ast::ArrayExpr>(braced->get()) : nullptr;
        if (!array_lit) {
            return error(diag, expr_location(*init), "cannot infer array length: initializer for a '[?]' array type must be an array literal");
        }
        if (array_lit->has_fill) {
            return error(diag, array_lit->location, "cannot infer array length: initializer must not use '...' to fill remaining elements");
        }

        Resolver resolver{program, diag};
        const auto element = resolver.resolve_type_impl((*array_type)->base_type, module_path);
        const auto count = static_cast<uint64_t>(array_lit->values.size());
        return intern_array(program, element, count,
                             resolver.size_of(module_path, element) * static_cast<uint32_t>(count),
                             resolver.align_of(module_path, element));
    }

    auto resolve_type_symbol(const std::string &module_path, const std::string &name, Program &program, DiagnosticEngine &diag, const SourceLocation &loc) -> ResolvedType {
        const Resolver resolver{program, diag};
        return resolver.resolve_final_full(module_path, name, false, loc);
    }
}
