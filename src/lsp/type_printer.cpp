#include "type_printer.hpp"

#include <filesystem>

namespace lsp {
    namespace {
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
            if (info->fields.empty()) return keyword + name + " {}";
            std::string out = keyword + name + " {\n";
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
                std::string out = "error(";
                for (size_t i = 0; i < info->error_member_types.size(); ++i) {
                    if (i > 0) out += " | ";
                    out += type_to_string(info->error_member_types[i], program, current_module_path);
                }
                return out + ")";
            }
            [[fallthrough]];
        }
        case TypeKind::Struct:
        case TypeKind::Enum: {
            const auto [module_path, name] = sema::find_type_module_and_name(type, program);
            if (name.empty()) {
                return "<anonymous type>";
            }
            if (module_path != current_module_path) {
                return module_display_name(module_path) + "." + name;
            }
            return name;
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
}
