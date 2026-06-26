#pragma once

#include <cstdint>

namespace sema {
    enum class TypeKind : uint8_t {
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
    };

    struct ResolvedType {
        TypeKind kind = TypeKind::Void;
        int pointee_index = -1;

        auto operator==(const ResolvedType &other) const -> bool {
            return kind == other.kind && pointee_index == other.pointee_index;
        }

        [[nodiscard]] auto is_integer() const -> bool {
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

        [[nodiscard]] auto is_signed() const -> bool {
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

        [[nodiscard]] auto is_float() const -> bool {
            switch (kind) {
            case TypeKind::F32:
            case TypeKind::F64:
                return true;
            default:
                return false;
            }
        }
    };
}
