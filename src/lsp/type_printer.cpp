#include "type_printer.hpp"

#include <filesystem>
#include <type_traits>

namespace lsp {
    namespace {
        // "[i32]", "[16]", "[u8, 16]" — the concrete generic_args suffix for a monomorphized
        // Struct/Enum/Union/Bitset instantiation (see sema::GenericInstanceInfo), or "" for a
        // non-generic type. Without this, type_to_string's bare-name rendering for e.g. a
        // 'List[i32]'-typed local silently collapsed to just "List" — find_type_module_and_name
        // deliberately returns the UNSPECIALIZED decl's own name for a generic instantiation
        // (see its doc comment in sema.cpp), so the generic_args have to be re-appended here.
        auto generic_args_suffix(const sema::ResolvedType &type, const sema::Program &program,
                                  const std::string &current_module_path) -> std::string {
            const sema::GenericInstanceInfo *info = nullptr;
            switch (type.kind) {
            case sema::TypeKind::Struct:
                if (const auto *i = program.struct_at(type.struct_index); i && i->generic_instance) info = &*i->generic_instance;
                break;
            case sema::TypeKind::Enum:
                if (const auto *i = program.enum_at(type.enum_index); i && i->generic_instance) info = &*i->generic_instance;
                break;
            case sema::TypeKind::Union:
                if (const auto *i = program.union_at(type.union_index); i && i->generic_instance) info = &*i->generic_instance;
                break;
            case sema::TypeKind::Bitset:
                if (const auto *i = program.bitset_at(type.bitset_index); i && i->generic_instance) info = &*i->generic_instance;
                break;
            default:
                break;
            }
            if (!info) return "";

            std::string out = "[";
            for (size_t i = 0; i < info->args.size(); ++i) {
                if (i > 0) out += ", ";
                const auto &arg = info->args[i];
                if (arg.is_type) {
                    out += type_to_string(arg.type_arg, program, current_module_path);
                } else if (const auto *iv = std::get_if<int64_t>(&arg.value_arg)) {
                    // A bool const-generic argument is stored as int64_t (type_resolver.cpp
                    // binds it that way; ConstFoldValue has no bool alternative), so rendering
                    // it as a number showed 'Foo[1]' where the source says 'Foo[true]'.
                    // value_arg_scalar_type carries the parameter's declared type, which is the
                    // only thing that distinguishes the two here.
                    if (arg.value_arg_scalar_type.kind == sema::TypeKind::Bool) {
                        out += *iv != 0 ? "true" : "false";
                    } else {
                        out += std::to_string(*iv);
                    }
                } else if (const auto *sv = std::get_if<std::string>(&arg.value_arg)) {
                    // The other ConstFoldValue alternative. The finding says to add a 'bool'
                    // branch; there is no bool alternative to add, but std::string fell through
                    // to "?" for real, which is the same symptom for a different reason.
                    out += '"' + *sv + '"';
                } else {
                    out += "?";
                }
            }
            return out + "]";
        }
        auto join_types(const std::vector<sema::ResolvedType> &types, const sema::Program &program,
                         const std::string &current_module_path) -> std::string {
            std::string out;
            for (size_t i = 0; i < types.size(); ++i) {
                if (i > 0) out += ", ";
                out += type_to_string(types[i], program, current_module_path);
            }
            return out;
        }

        // find_type_module_and_name returns the type's *defining* module as a
        // canonical absolute directory path, not the local alias a hovering
        // file bound it to via `const alias := import(...)`. The directory's
        // basename (e.g. ".../examples/compiler/std" -> "std") is used as a
        // readable stand-in, since module directories are conventionally
        // named after what they're imported as.
        auto module_display_name(const std::string &module_path) -> std::string {
            return std::filesystem::path(module_path).filename().string();
        }

        // A tagged-union variant's payload struct is either a real named type (referenced or
        // reused as-is) or an anonymous slot synthesized for an inline `struct { ... }`
        // payload (see type_resolver.cpp's tagged-union layout pass) - the latter has no name
        // for type_to_string to print, so expand its fields inline instead.
        auto describe_variant_payload(const sema::ResolvedType &payload_type, const sema::Program &program,
                                       const std::string &current_module_path) -> std::string {
            if (payload_type.kind == sema::TypeKind::Struct) {
                const auto [mod_path, name] = sema::find_type_module_and_name(payload_type, program);
                if (name.empty()) {
                    if (const auto *info = program.struct_at(payload_type.struct_index)) {
                        std::string out = "{ ";
                        for (size_t i = 0; i < info->fields.size(); ++i) {
                            if (i > 0) out += ", ";
                            out += info->fields[i].name + ": " + type_to_string(info->fields[i].type, program, current_module_path);
                        }
                        return out + " }";
                    }
                }
            }
            return type_to_string(payload_type, program, current_module_path);
        }
    }

    auto describe_type_definition(const sema::ResolvedType &type, const sema::Program &program,
                                   const std::string &current_module_path) -> std::string {
        using sema::TypeKind;
        const auto name = type_to_string(type, program, current_module_path);

        if (type.kind == TypeKind::Struct) {
            const auto *info = program.struct_at(type.struct_index);
            if (!info) return "struct " + name + " {}";
            const std::string keyword = info->is_packed ? "packed struct " : "struct ";
            // An anonymous struct (a tagged-union variant's inline 'struct { ... }' payload -
            // see describe_variant_payload above, which handles the same case for a bare
            // (non-defining) reference) has no real name for type_to_string to have found -
            // "struct <anonymous type> { ... }" reads oddly, so omit the placeholder name here.
            const auto struct_name = name == "<anonymous type>" ? "" : name + " ";
            if (info->fields.empty()) return keyword + struct_name + "{}";
            std::string out = keyword + struct_name + "{\n";
            for (const auto &field : info->fields) {
                out += "    " + field.name + ": " + type_to_string(field.type, program, current_module_path) + ",\n";
            }
            return out + "}";
        }

        if (type.kind == TypeKind::Enum) {
            const auto *info = program.enum_at(type.enum_index);
            if (!info || info->fields.empty()) return "enum " + name + " {}";
            std::string out = "enum " + name + " {\n";
            for (const auto &field : info->fields) {
                out += "    " + field.name + " = " + std::to_string(field.value) + ",\n";
            }
            return out + "}";
        }

        if (type.kind == TypeKind::Union) {
            const auto *info = program.union_at(type.union_index);
            if (!info) return "union " + name + " {}";
            // A compiler-synthesized error(...) wrapper (see type_to_string's own Union case
            // above) is never a real user-declared tagged union - expanding it into its
            // synthesized Ok/Failed variant listing would misleadingly suggest those are
            // switchable/matchable tags a caller can see, when they're purely an
            // implementation detail (see sema's Typed Error System). Render it exactly like an
            // ordinary reference to it instead - 'error(Members)'.
            if (info->is_error_union) {
                return name;
            }
            if (info->is_tagged) {
                if (info->variants.empty()) return "union(enum) " + name + " {}";
                std::string out = "union(enum) " + name + " {\n";
                for (const auto &variant : info->variants) {
                    out += "    " + variant.name;
                    if (variant.payload_struct_index >= 0) {
                        out += ": " + describe_variant_payload(variant.payload_type, program, current_module_path);
                    }
                    out += ",\n";
                }
                return out + "}";
            }
            if (info->members.empty()) return "union " + name + " {}";
            std::string out = "union " + name + " {\n";
            for (const auto &member : info->members) {
                out += "    " + member.name + ": " + type_to_string(member.type, program, current_module_path) + ",\n";
            }
            return out + "}";
        }

        if (type.kind == TypeKind::Bitset) {
            const auto *info = program.bitset_at(type.bitset_index);
            if (!info) return "bitset(<unknown>)";
            return "bitset(" + type_to_string(info->member_enum_type, program, current_module_path) + ", " +
                   type_to_string(info->storage_type, program, current_module_path) + ")";
        }

        if (type.kind == TypeKind::Trait) {
            const auto *info = program.trait_at(type.trait_index);
            if (!info || info->methods.empty()) return "trait " + name + " {}";
            std::string out = "trait " + name + " {\n";
            for (const auto &method : info->methods) {
                out += "    fn " + method.name + "(" + (method.is_mut_self ? "mut self" : "self");
                // Zips the AST params (for names) against the resolved params (for types).
                // These are built from the same declaration and must be the same length; if
                // they ever desynchronize, hover would silently print a parameter with no type
                // rather than anything indicating the mismatch. Say so instead of guessing.
                if (method.decl) {
                    if (method.decl->params.size() != method.params.size()) {
                        out += ", <parameter names and types are out of sync>";
                    } else {
                        for (size_t i = 0; i < method.decl->params.size(); ++i) {
                            out += ", " + method.decl->params[i].name;
                            out += ": " + type_to_string(method.params[i], program, current_module_path);
                        }
                    }
                }
                out += ")";
                if (!method.return_types.empty()) {
                    out += " -> ";
                    out += method.return_types.size() == 1
                               ? type_to_string(method.return_types[0], program, current_module_path)
                               : "(" + join_types(method.return_types, program, current_module_path) + ")";
                }
                out += "\n";
            }
            return out + "}";
        }

        return name;
    }

    auto type_to_string(const sema::ResolvedType &type, const sema::Program &program,
                         const std::string &current_module_path) -> std::string {
        using sema::TypeKind;

        switch (type.kind) {
        case TypeKind::Invalid: return "<invalid>";
        case TypeKind::Void: return "void";
        case TypeKind::U8: return "u8";
        case TypeKind::U16: return "u16";
        case TypeKind::U32: return "u32";
        case TypeKind::U64: return "u64";
        case TypeKind::I8: return "i8";
        case TypeKind::I16: return "i16";
        case TypeKind::I32: return "i32";
        case TypeKind::I64: return "i64";
        case TypeKind::F32: return "f32";
        case TypeKind::F64: return "f64";
        case TypeKind::USize: return "usize";
        case TypeKind::Bool: return "bool";
        case TypeKind::Anyptr: return "anyptr";
        case TypeKind::Any: return "any";
        // A type-generic-parameter reference's own compile-time-constant kind ('T' inside a
        // generic decl, before any concrete instantiation binds it — see find_enclosing_function's
        // synthetic generic-param ParamInfo entries) — was previously missing entirely, falling
        // through to "<invalid>".
        case TypeKind::Type: return "type";

        case TypeKind::Pointer:
            if (type.pointee_index < 0 || static_cast<size_t>(type.pointee_index) >= program.pointer_pointees.size()) {
                return "*<unknown>";
            }
            return "*" + type_to_string(program.pointer_pointees[type.pointee_index], program, current_module_path);

        case TypeKind::Array: {
            if (type.array_index < 0 || static_cast<size_t>(type.array_index) >= program.arrays.size()) {
                return "[]<unknown>";
            }
            const auto &info = program.arrays[type.array_index];
            return "[" + std::to_string(info.count) + "]" + type_to_string(info.element_type, program, current_module_path);
        }

        case TypeKind::Slice: {
            if (type.slice_index < 0 || static_cast<size_t>(type.slice_index) >= program.slices.size()) {
                return "[]<unknown>";
            }
            return "[]" + type_to_string(program.slices[type.slice_index].element_type, program, current_module_path);
        }

        case TypeKind::Union: {
            // Compiler-synthesized error(...) unions have no TypeSymbol (they're never
            // user-nameable) — render from their own member-type list instead of falling
            // through to the <anonymous type> case below.
            if (const auto *info = program.union_at(type.union_index); info && info->is_error_union) {
                std::string out = info->is_optional ? "?error(" : "error(";
                for (size_t i = 0; i < info->error_member_types.size(); ++i) {
                    if (i > 0) out += " | ";
                    out += type_to_string(info->error_member_types[i], program, current_module_path);
                }
                return out + ")";
            }
            [[fallthrough]];
        }
        case TypeKind::Struct:
        case TypeKind::Enum:
        case TypeKind::Bitset:
        case TypeKind::Trait: {
            const auto [module_path, name] = sema::find_type_module_and_name(type, program);
            if (name.empty()) {
                return "<anonymous type>";
            }
            const auto suffix = generic_args_suffix(type, program, current_module_path);
            if (module_path != current_module_path) {
                return module_display_name(module_path) + "." + name + suffix;
            }
            return name + suffix;
        }

        case TypeKind::Function: {
            if (type.fn_index < 0 || static_cast<size_t>(type.fn_index) >= program.fn_signatures.size()) {
                return "fn(...) -> <unknown>";
            }
            const auto &info = program.fn_signatures[type.fn_index];
            std::string params;
            for (size_t i = 0; i < info.param_types.size(); ++i) {
                if (i > 0) params += ", ";
                if (i < info.param_names.size() && !info.param_names[i].empty()) {
                    params += info.param_names[i] + ": ";
                }
                params += type_to_string(info.param_types[i], program, current_module_path);
            }
            std::string result = "fn(" + params + (info.is_variadic ? ", ..." : "") + ")";
            if (!info.return_types.empty()) {
                result += " -> ";
                result += info.return_types.size() == 1
                               ? type_to_string(info.return_types[0], program, current_module_path)
                               : "(" + join_types(info.return_types, program, current_module_path) + ")";
            }
            return result;
        }

        case TypeKind::Namespace: return "<namespace>";
        default: return "<invalid>";
        }
    }

    auto ast_generic_arg_to_string(const ast::GenericArg &arg) -> std::string {
        if (const auto *t = std::get_if<ast::Type>(&arg.value)) {
            return ast_type_to_string(*t);
        }
        if (const auto *e = std::get_if<ast::Expr>(&arg.value)) {
            if (const auto *ident = std::get_if<ast::IdentExpr>(e)) {
                return ident->name;
            }
            if (const auto *lit = std::get_if<ast::LiteralIntegerExpr>(e)) {
                return std::to_string(lit->value);
            }
        }
        return "...";
    }

    auto ast_type_to_string(const ast::Type &type, const std::unordered_map<std::string, std::string> &subst) -> std::string {
        return std::visit(
            [&]<typename T>(const T &v) -> std::string {
                using V = std::decay_t<T>;
                if constexpr (std::is_same_v<V, std::monostate>) {
                    return "<?>";
                } else if constexpr (std::is_same_v<V, ast::BuiltinType>) {
                    switch (v.kind) {
                    case ast::BuiltinTypeKind::U8:     return "u8";
                    case ast::BuiltinTypeKind::U16:    return "u16";
                    case ast::BuiltinTypeKind::U32:    return "u32";
                    case ast::BuiltinTypeKind::U64:    return "u64";
                    case ast::BuiltinTypeKind::I8:     return "i8";
                    case ast::BuiltinTypeKind::I16:    return "i16";
                    case ast::BuiltinTypeKind::I32:    return "i32";
                    case ast::BuiltinTypeKind::I64:    return "i64";
                    case ast::BuiltinTypeKind::F32:    return "f32";
                    case ast::BuiltinTypeKind::F64:    return "f64";
                    case ast::BuiltinTypeKind::Usize:  return "usize";
                    case ast::BuiltinTypeKind::Bool:   return "bool";
                    case ast::BuiltinTypeKind::Byte:   return "byte";
                    case ast::BuiltinTypeKind::Anyptr: return "anyptr";
                    case ast::BuiltinTypeKind::Type:   return "type";
                    case ast::BuiltinTypeKind::Any:    return "any";
                    }
                    return "<?>";
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::PointerType>>) {
                    return "*" + ast_type_to_string(v->pointee, subst);
                } else if constexpr (std::is_same_v<V, ast::NamedType>) {
                    if (v.member == nullptr && v.generic_args.empty()) {
                        if (const auto it = subst.find(v.name); it != subst.end()) return it->second;
                    }
                    std::string out = v.name;
                    const ast::NamedType *leaf = &v;
                    while (leaf->member) {
                        out += "." + leaf->member->name;
                        leaf = leaf->member.get();
                    }
                    if (!leaf->generic_args.empty()) {
                        out += "[";
                        for (size_t i = 0; i < leaf->generic_args.size(); ++i) {
                            if (i > 0) out += ", ";
                            const auto &arg = *leaf->generic_args[i];
                            if (const auto *t = std::get_if<ast::Type>(&arg.value)) {
                                out += ast_type_to_string(*t, subst);
                            } else if (const auto *e = std::get_if<ast::Expr>(&arg.value)) {
                                if (const auto *ident = std::get_if<ast::IdentExpr>(e)) {
                                    if (const auto it = subst.find(ident->name); it != subst.end()) {
                                        out += it->second;
                                    } else {
                                        out += ident->name;
                                    }
                                } else if (const auto *lit = std::get_if<ast::LiteralIntegerExpr>(e)) {
                                    out += std::to_string(lit->value);
                                } else {
                                    out += "...";
                                }
                            }
                        }
                        out += "]";
                    }
                    return out;
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::ArrayType>>) {
                    std::string size_str = "?";
                    if (v->size) {
                        if (const auto *ident = std::get_if<ast::IdentExpr>(&*v->size)) {
                            if (const auto it = subst.find(ident->name); it != subst.end()) {
                                size_str = it->second;
                            } else {
                                size_str = ident->name;
                            }
                        } else if (const auto *lit = std::get_if<ast::LiteralIntegerExpr>(&*v->size)) {
                            size_str = std::to_string(lit->value);
                        } else {
                            size_str = "...";
                        }
                    }
                    return "[" + size_str + "]" + ast_type_to_string(v->base_type, subst);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::SliceType>>) {
                    return "[]" + ast_type_to_string(v->base_type, subst);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::StructType>>) {
                    return "struct {...}";
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::EnumType>>) {
                    return "enum {...}";
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::UnionType>>) {
                    return "union {...}";
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::FunctionType>>) {
                    return "fn(...)";
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::TraitType>>) {
                    return "trait {...}";
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::ErrorType>>) {
                    return "error(...)";
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::OptionalErrorType>>) {
                    return "?" + ast_type_to_string(v->inner, subst);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::BitsetType>>) {
                    return "bitset(...)";
                } else {
                    return "<?>";
                }
            },
            type);
    }

    auto ast_generic_params_to_string(const std::vector<ast::GenericParam> &params) -> std::string {
        if (params.empty()) return "";
        std::string out = "[";
        for (size_t i = 0; i < params.size(); ++i) {
            if (i > 0) out += ", ";
            out += params[i].name + ": " + ast_type_to_string(params[i].type);
        }
        return out + "]";
    }
}
