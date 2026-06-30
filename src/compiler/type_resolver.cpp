#include "sema.hpp"

#include <algorithm>
#include <format>

namespace sema {
    auto intern_pointer(ProgramModule &module, const ResolvedType &pointee) -> ResolvedType {
        for (size_t i = 0; i < module.pointer_pointees.size(); ++i) {
            if (module.pointer_pointees[i] == pointee) {
                return ResolvedType{.kind = TypeKind::Pointer, .pointee_index = static_cast<int>(i)};
            }
        }
        module.pointer_pointees.push_back(pointee);
        return ResolvedType{.kind = TypeKind::Pointer, .pointee_index = static_cast<int>(module.pointer_pointees.size()) - 1};
    }

    namespace {
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
                return 8;

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
                    if (mod.structs[slot].layout_done) return *ts->resolved;

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

                return resolve_final_shallow(module_path, name, check_pub, loc);
            }

            void layout_struct(const std::string &module_path, const int slot, const std::unique_ptr<ast::StructType> &decl) {
                StructInfo info;
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

                    info.fields.push_back(StructField{.name = field.name, .type = field_type, .offset = offset});
                    offset += f_size;
                    max_align = std::max(max_align, f_align);
                }

                info.size = decl->is_packed ? offset : (offset + max_align - 1) / max_align * max_align;
                info.align = decl->is_packed ? 1 : max_align;
                info.layout_done = true;

                program.modules.at(module_path).structs[slot] = std::move(info);
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
                if (t.kind == TypeKind::Struct) return program.modules.at(module_path).structs[t.struct_index].size;
                return primitive_size(t.kind);
            }

            [[nodiscard]] auto align_of(const std::string &module_path, const ResolvedType &t) const -> uint32_t {
                if (t.kind == TypeKind::Struct) return program.modules.at(module_path).structs[t.struct_index].align;
                return primitive_align(t.kind);
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
                            return intern_pointer(program.modules.at(module_path), pointee);

                        } else if constexpr (std::is_same_v<V, ast::NamedType>) {
                            auto target = walk_namespace_chain(module_path, v, program, diag);
                            if (!target) return ResolvedType{.kind = TypeKind::Invalid};
                            return resolve_final_shallow(target->module_path, target->name, target->crossed_boundary, v.location);

                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::StructType>>) {
                            auto &mod = program.modules.at(module_path);
                            int slot = static_cast<int>(mod.structs.size());
                            mod.structs.push_back(StructInfo{});
                            layout_struct(module_path, slot, v);
                            return ResolvedType{.kind = TypeKind::Struct, .struct_index = slot};
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
        return false;
    }

    auto resolve_type(const ast::Type &type, const std::string &module_path, Program &program, DiagnosticEngine &diag) -> ResolvedType {
        Resolver resolver{program, diag};
        return resolver.resolve_type_impl(type, module_path);
    }

    auto resolve_type_symbol(const std::string &module_path, const std::string &name, Program &program, DiagnosticEngine &diag, const SourceLocation &loc) -> ResolvedType {
        const Resolver resolver{program, diag};
        return resolver.resolve_final_full(module_path, name, false, loc);
    }
}
