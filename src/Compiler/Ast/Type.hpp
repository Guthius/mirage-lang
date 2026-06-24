#pragma once

#include <Compiler/SourceLocation.hpp>

#include <cstdint>
#include <memory>
#include <variant>

namespace Ast {
    struct BuiltinType;
    struct PointerType;
    struct NamedType;

    using Type = std::variant<
        std::monostate,
        std::unique_ptr<BuiltinType>,
        std::unique_ptr<PointerType>,
        std::unique_ptr<NamedType>>;

    enum class BuiltinTypeKind : uint8_t {
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
        Usize,
        Bool,
        Byte,
        Error,
        Anyptr,
        Type,
    };

    struct BuiltinType {
        BuiltinTypeKind Kind;
        SourceLocation Location;
    };

    struct PointerType {
        Type Pointee;
        SourceLocation Location;
    };

    struct NamedType {
        std::string Name;
        std::unique_ptr<NamedType> Member;
        SourceLocation Location;
    };
}
