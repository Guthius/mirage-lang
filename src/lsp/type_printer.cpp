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
        case TypeKind::Error: return "error";
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

        case TypeKind::Struct:
        case TypeKind::Enum:
        case TypeKind::Union: {
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
            std::string result = "fn(" + join_types(info.param_types, program, current_module_path) + (info.is_variadic ? ", ..." : "") + ")";
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
