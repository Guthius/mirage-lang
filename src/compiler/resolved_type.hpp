#pragma once

#include <cstdint>
#include <functional>

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
        Bitset,
        Type,
        Any,
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
        int bitset_index = -1;  // global index into Program::bitsets

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
                   other.trait_index == trait_index &&
                   other.bitset_index == bitset_index;
        }

        auto operator!=(const ResolvedType &other) const -> bool {
            return !(*this == other);
        }
    };

    // Bit width of a scalar TypeKind — used by sema's inline-asm width-mismatch check (comparing
    // a Mirage variable operand's type against the asm register it's paired with). Returns 0 for
    // any non-scalar kind. Deliberately not reused by codegen's own (already-correct, unrelated)
    // int_bits() helper — see asm sema-check code for why the two are kept independent.
    [[nodiscard]] inline auto scalar_bit_width(const TypeKind kind) -> unsigned {
        switch (kind) {
        case TypeKind::U8:
        case TypeKind::I8:
        case TypeKind::Bool:
            return 8;
        case TypeKind::U16:
        case TypeKind::I16:
            return 16;
        case TypeKind::U32:
        case TypeKind::I32:
        case TypeKind::F32:
            return 32;
        case TypeKind::U64:
        case TypeKind::I64:
        case TypeKind::USize:
        case TypeKind::Pointer:
        case TypeKind::Anyptr:
        case TypeKind::F64:
            return 64;
        default:
            return 0;
        }
    }
}

// Hashes every field operator== compares (kind + all 9 *_index fields), so two
// ResolvedTypes that compare equal always hash equal - required for
// std::unordered_map<ResolvedType, uint64_t> (sema::Program::type_ids).
template <>
struct std::hash<sema::ResolvedType> {
    auto operator()(const sema::ResolvedType &t) const noexcept -> size_t {
        size_t h = std::hash<int>{}(static_cast<int>(t.kind));
        auto combine = [&h](const int v) {
            h ^= std::hash<int>{}(v) + 0x9e3779b9U + (h << 6) + (h >> 2);
        };
        combine(t.pointee_index);
        combine(t.struct_index);
        combine(t.array_index);
        combine(t.slice_index);
        combine(t.enum_index);
        combine(t.union_index);
        combine(t.fn_index);
        combine(t.trait_index);
        combine(t.bitset_index);
        return h;
    }
};
