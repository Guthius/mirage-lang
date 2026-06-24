#include "../Ast.hpp"

#include <format>

namespace Ast {
    namespace {
        auto ParseNamedType(Parser &parser) -> std::unique_ptr<NamedType> {
            const auto location = parser.CurrentLocation();
            const auto &name = parser.Expect(TokenKind::identifier, "type name").Lexeme;

            if (parser.Match(TokenKind::dot)) {
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
            case TokenKind::kw_u8:     return BuiltinTypeKind::U8;
            case TokenKind::kw_u16:    return BuiltinTypeKind::U16;
            case TokenKind::kw_u32:    return BuiltinTypeKind::U32;
            case TokenKind::kw_u64:    return BuiltinTypeKind::U64;
            case TokenKind::kw_i8:     return BuiltinTypeKind::I8;
            case TokenKind::kw_i16:    return BuiltinTypeKind::I16;
            case TokenKind::kw_i32:    return BuiltinTypeKind::I32;
            case TokenKind::kw_i64:    return BuiltinTypeKind::I64;
            case TokenKind::kw_f32:    return BuiltinTypeKind::F32;
            case TokenKind::kw_f64:    return BuiltinTypeKind::F64;
            case TokenKind::kw_usize:  return BuiltinTypeKind::Usize;
            case TokenKind::kw_bool:   return BuiltinTypeKind::Bool;
            case TokenKind::kw_byte:   return BuiltinTypeKind::Byte;
            case TokenKind::kw_error:  return BuiltinTypeKind::Error;
            case TokenKind::kw_anyptr: return BuiltinTypeKind::Anyptr;
            case TokenKind::kw_type:   return BuiltinTypeKind::Type;
            default:                    return std::nullopt;
            }
        }
    }

    auto ParseType(Parser &parser) -> Type {
        const auto location = parser.CurrentLocation();

        if (parser.Match(TokenKind::star)) {
            return std::make_unique<PointerType>(PointerType{
                .Pointee = ParseType(parser),
                .Location = location,
            });
        }

        if (parser.Check(TokenKind::identifier)) {
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
