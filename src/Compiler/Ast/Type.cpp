#include <Compiler/Ast.hpp>

#include <format>

namespace Ast {
    namespace {
        auto ParseNamedType(Parser &parser) -> std::unique_ptr<NamedType> {
            const auto location = parser.CurrentLocation();
            const auto &name = parser.Expect(TokenKind::Identifier, "type name").Lexeme;

            if (parser.Match(TokenKind::Dot)) {
                return std::make_unique<NamedType>(NamedType{
                    .Name = name,
                    .Member = ParseNamedType(parser),
                    .Location = location,
                });
            }

            return std::make_unique<NamedType>(NamedType{
                .Name = name,
                .Member = nullptr,
                .Location = location,
            });
        }

        auto ParseBuiltinTypeKind(Parser &parser) -> std::optional<BuiltinTypeKind> {
            switch (parser.Advance().Kind) {
            case TokenKind::KwU8:     return BuiltinTypeKind::U8;
            case TokenKind::KwU16:    return BuiltinTypeKind::U16;
            case TokenKind::KwU32:    return BuiltinTypeKind::U32;
            case TokenKind::KwU64:    return BuiltinTypeKind::U64;
            case TokenKind::KwI8:     return BuiltinTypeKind::I8;
            case TokenKind::KwI16:    return BuiltinTypeKind::I16;
            case TokenKind::KwI32:    return BuiltinTypeKind::I32;
            case TokenKind::KwI64:    return BuiltinTypeKind::I64;
            case TokenKind::KwF32:    return BuiltinTypeKind::F32;
            case TokenKind::KwF64:    return BuiltinTypeKind::F64;
            case TokenKind::KwUSize:  return BuiltinTypeKind::Usize;
            case TokenKind::KwBool:   return BuiltinTypeKind::Bool;
            case TokenKind::KwByte:   return BuiltinTypeKind::Byte;
            case TokenKind::KwError:  return BuiltinTypeKind::Error;
            case TokenKind::KwAnyptr: return BuiltinTypeKind::Anyptr;
            case TokenKind::KwType:   return BuiltinTypeKind::Type;
            default:                  return std::nullopt;
            }
        }
    }

    auto ParseType(Parser &parser) -> Type {
        const auto location = parser.CurrentLocation();

        if (parser.Match(TokenKind::Star)) {
            return std::make_unique<PointerType>(PointerType{
                .Pointee = ParseType(parser),
                .Location = location,
            });
        }

        if (parser.Check(TokenKind::Identifier)) {
            return ParseNamedType(parser);
        }

        if (const auto kind = ParseBuiltinTypeKind(parser); kind.has_value()) {
            return std::make_unique<BuiltinType>(BuiltinType{
                .Kind = kind.value(),
                .Location = location,
            });
        }

        parser.ReportError(location, std::format("expected type, got '{}'", parser.CurrentLexeme()));

        return std::monostate();
    }
}
