#include "../Ast.hpp"

namespace Ast {
    namespace {
        auto ParseBlockStmt(Parser &parser) -> Stmt {
            const auto location = parser.CurrentLocation();

            parser.Expect(TokenKind::lbrace, "'{'");

            while (!parser.Check(TokenKind::rbrace)) {
                parser.Advance();
            }

            parser.Expect(TokenKind::rbrace, "'}'");

            return std::make_unique<BlockStmt>(BlockStmt{
                .Location = location,
            });
        }

        auto ParseExprStmt(Parser &parser) -> Stmt {
            const auto location = parser.CurrentLocation();

            parser.Advance();

            return std::make_unique<ExprStmt>(ExprStmt{
                .Location = location,
            });
        }
    }

    auto ParseStmt(Parser &parser) -> Stmt {
        if (parser.Check(TokenKind::lbrace)) {
            return ParseBlockStmt(parser);
        }

        return ParseExprStmt(parser);
    }
}
