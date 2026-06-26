#pragma once

#include <cstdint>

namespace Sema {
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
        Byte,
        Error,
        Anyptr,
        Pointer,
    };

    struct ResolvedType {
        TypeKind Kind = TypeKind::Void;
        int PointeeIndex = -1;

        auto operator==(const ResolvedType &other) const -> bool {
            return Kind == other.Kind && PointeeIndex == other.PointeeIndex;
        }

        [[nodiscard]] auto IsInteger() const -> bool {
            switch (Kind) {
            case TypeKind::U8:
            case TypeKind::U16:
            case TypeKind::U32:
            case TypeKind::U64:
            case TypeKind::I8:
            case TypeKind::I16:
            case TypeKind::I32:
            case TypeKind::I64:
            case TypeKind::Byte:
            case TypeKind::USize:
            case TypeKind::Error:
                return true;
            default:
                return false;
            }
        }

        [[nodiscard]] auto IsSigned() const -> bool {
            switch (Kind) {
            case TypeKind::I8:
            case TypeKind::I16:
            case TypeKind::I32:
            case TypeKind::I64:
                return true;
            default:
                return false;
            }
        }

        [[nodiscard]] auto IsFloat() const -> bool {
            switch (Kind) {
            case TypeKind::F32:
            case TypeKind::F64:
                return true;
            default:
                return false;
            }
        }
    };
}
