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
        // An UNBOUND generic type parameter, as seen while eagerly type-checking a generic
        // declaration's body before any concrete instantiation exists. Deliberately permissive:
        // an operation on it succeeds and yields Opaque again rather than reporting, so the
        // eager pass only reports what is independent of the parameter (unknown names, arity,
        // concrete-type mismatches). It is NOT 'any' — 'any' is a concrete 16-byte fat pointer
        // with a NARROWER operation set than most real T, so binding T to it would invent
        // errors on correct code.
        //
        // Reuses 'trait_index': -1 means unconstrained, >= 0 means bounded by that trait
        // ('[T: Hashable]'), which flips the behavior from permissive to strict — only the
        // trait's flattened method set plus a small universal core is allowed.
        //
        // 'opaque_param_index' names the parameter for display purposes only; two Opaques
        // still compare equal regardless of it, which is what keeps the permissiveness above
        // from turning into a strict 'T vs U' distinction. One consequence worth knowing: a
        // pointer/slice/array whose element is Opaque is interned by that same equality, so
        // '*K' and '*V' in a two-parameter generic share one interned slot and print under
        // whichever name got there first (see intern_pointer). Narrow, and the alternative —
        // keying interning on the name — is exactly the strictness change this avoids.
        //
        // Never reaches codegen: an instantiation whose arguments contain an Opaque is never
        // registered (see instantiate_generic_function). Every codegen switch treats it as an
        // internal error.
        Opaque,
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

        // DISPLAY ONLY. Index into Program::opaque_param_names, or -1. Meaningful only when
        // 'kind' is Opaque, and set only for an Opaque that IS a named generic parameter (the
        // bindings opaque_args_for creates, and results propagated straight through an
        // operator by opaque_unary_result/opaque_binary_op_result). An Opaque merely DERIVED
        // from a parameter — the type of a member access on an Opaque receiver, of indexing
        // one, and so on — deliberately keeps -1, because "T" would be a lie there.
        //
        // Exists because a ResolvedType otherwise carries no trace of which parameter it came
        // from, so 'mut n := value' inside 'fn f[T: type](value: T)' could not be reported as
        // ': T' by anything downstream. Diagnostics (describe_type) and the LSP's type printer
        // fall back to "<generic>"/"<generic: Trait>" when it is -1.
        //
        // DELIBERATELY ABSENT FROM operator== AND std::hash BELOW — see the note on each, and
        // TypeKind::Opaque's own doc comment. Comparing it would make 'T' and 'U' distinct
        // types, which flips the eager generic checker from permissive to strict and invents
        // diagnostics on correct code.
        int opaque_param_index = -1;

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

        // A typed pointer or 'anyptr' — the two kinds that support pointer arithmetic and
        // nil comparison. codegen.cpp has its own is_pointer_like free function predating
        // this; both must agree.
        auto is_pointer_like() const -> bool {
            return kind == TypeKind::Pointer || kind == TypeKind::Anyptr;
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

        // 'opaque_param_index' is intentionally NOT compared: it is a display label, not part
        // of a type's identity. Two unbound generic parameters must stay interchangeable here
        // so the eager template check keeps reporting only what is independent of the
        // parameters — see the field's own comment above.
        // constexpr only so the static_assert below this struct can run: it is the guard that
        // makes an accidental "consistency fix" here fail the build instead of silently
        // changing the language.
        constexpr auto operator==(const ResolvedType &other) const -> bool {
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

        constexpr auto operator!=(const ResolvedType &other) const -> bool {
            return !(*this == other);
        }
    };

    // Two unbound generic parameters are the SAME type as far as type checking is concerned,
    // and differ only in how they are spelled back to the reader.
    //
    // This is load-bearing, not a curiosity. TypeKind::Opaque is deliberately permissive so
    // that eagerly checking a generic declaration's body reports only what is independent of
    // its parameters; making 'T' and 'U' distinct would turn 'mut x: T = u' — correct or not
    // depending entirely on the instantiation — into a declaration-time error, across the
    // whole language. It would also silently split the pointer/slice/array interning tables.
    //
    // Asserted here rather than in a test file because the mistake it guards against is a
    // plausible tidy-up of the two comments above ("operator== forgot a field"), and this
    // fails that edit at the point of making it.
    static_assert(ResolvedType{.kind = TypeKind::Opaque, .opaque_param_index = 0} ==
                      ResolvedType{.kind = TypeKind::Opaque, .opaque_param_index = 1},
                  "ResolvedType::opaque_param_index must stay out of operator== (and std::hash) - "
                  "see TypeKind::Opaque");

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

    // Whether a folded constant's BITS fit the declared scalar kind: unsigned kinds compare
    // the unsigned value against the width's max; signed kinds require the signed reading
    // to be in range. The const evaluators fold negative literals as wrapped
    // two's-complement bits, which this reading round-trips correctly. Returns true for
    // non-scalar kinds (nothing to check). Used to validate generic value arguments
    // ('Buf[300]' for '[N: u8]') and '$option'/'$env' integer values against their targets.
    [[nodiscard]] inline auto scalar_value_fits(const uint64_t bits, const TypeKind kind) -> bool {
        const auto sv = static_cast<int64_t>(bits);
        switch (kind) {
        case TypeKind::Bool: return bits <= 1;
        case TypeKind::U8:   return bits <= 0xFF;
        case TypeKind::U16:  return bits <= 0xFFFF;
        case TypeKind::U32:  return bits <= 0xFFFF'FFFF;
        case TypeKind::I8:   return sv >= -128 && sv <= 127;
        case TypeKind::I16:  return sv >= -32768 && sv <= 32767;
        case TypeKind::I32:  return sv >= -2147483648LL && sv <= 2147483647LL;
        default:             return true;
        }
    }
}

// Hashes exactly the fields operator== compares (kind + the 9 identity *_index fields), so
// two ResolvedTypes that compare equal always hash equal - required for
// std::unordered_map<ResolvedType, uint64_t> (sema::Program::type_ids).
//
// 'opaque_param_index' is excluded here for the same reason operator== excludes it, and the
// two exclusions must stay in step: adding it to only one of them breaks the equal-implies-
// equal-hash contract, and adding it to BOTH silently changes the language (see the field's
// own comment in ResolvedType).
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
