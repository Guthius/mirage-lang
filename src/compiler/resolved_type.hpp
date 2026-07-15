#pragma once

#include <cstdint>

namespace sema {
    enum class TypeKind : uint8_t {
        Invalid,
        Void,
        U8,
        U16,
        U32,
        U64,
        I8,
        I16,
        I32,
        I64,
        F32,
        F64,
        USize,
        Bool,
        Error,
        Anyptr,
        Pointer,
        Struct,
        Array,
        Slice,
        Namespace,
        Enum,
        Union,
        Function,
        Trait,
    };

    struct ResolvedType {
        TypeKind kind = TypeKind::Void;
        int pointee_index = -1;
        int struct_index = -1;  // global index into Program::structs
        int array_index = -1;
        int slice_index = -1;
        int enum_index = -1;
        int union_index = -1;   // global index into Program::unions
        int fn_index = -1;      // global index into Program::fn_signatures
        int trait_index = -1;   // global index into Program::traits

        auto is_integer() const -> bool {
            switch (kind) {
            case TypeKind::U8:
            case TypeKind::U16:
            case TypeKind::U32:
            case TypeKind::U64:
            case TypeKind::I8:
            case TypeKind::I16:
            case TypeKind::I32:
            case TypeKind::I64:
            case TypeKind::USize:
            case TypeKind::Error:
                return true;

            default:
                return false;
            }
        }

        auto is_signed() const -> bool {
            switch (kind) {
            case TypeKind::I8:
            case TypeKind::I16:
            case TypeKind::I32:
            case TypeKind::I64:
                return true;

            default:
                return false;
            }
        }

        auto is_float() const -> bool {
            return kind == TypeKind::F32 || kind == TypeKind::F64;
        }

        auto is_scalar() const -> bool {
            return is_integer() || is_float() ||
                   kind == TypeKind::Bool ||
                   kind == TypeKind::Anyptr ||
                   kind == TypeKind::Pointer;
        }

        auto operator==(const ResolvedType &other) const -> bool {
            return other.kind == kind &&
                   other.pointee_index == pointee_index &&
                   other.struct_index == struct_index &&
                   other.array_index == array_index &&
                   other.slice_index == slice_index &&
                   other.enum_index == enum_index &&
                   other.union_index == union_index &&
                   other.fn_index == fn_index &&
                   other.trait_index == trait_index;
        }

        auto operator!=(const ResolvedType &other) const -> bool {
            return !(*this == other);
        }
    };
}
