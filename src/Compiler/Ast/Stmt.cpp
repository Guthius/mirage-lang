#include <Compiler/Ast.hpp>

namespace Ast {
    namespace {
        auto ParseBlockStmt(Parser &parser) -> Stmt {
            const auto location = parser.CurrentLocation();

            parser.Expect(TokenKind::LBrace, "'{'");

            std::vector<Stmt> stmts;
            while (!parser.Check(TokenKind::RBrace)) {
                stmts.push_back(ParseStmt(parser));
            }

            parser.Expect(TokenKind::RBrace, "'}'");

            return std::make_unique<BlockStmt>(BlockStmt{
                .Statements = std::move(stmts),
                .Location = location,
            });
        }

        auto ParseAsmStmt(Parser &parser) -> Stmt {
            const auto location = parser.CurrentLocation();

            parser.Expect(TokenKind::AsmBlock, "asm block");

            return std::make_unique<AsmStmt>(AsmStmt{
                .Location = location,
            });
        }

        auto ParseExprStmt(Parser &parser) -> Stmt {
            const auto location = parser.CurrentLocation();

            return std::make_unique<ExprStmt>(ExprStmt{
                .Expr = parse_expr(parser),
                .Location = location,
            });
        }
    }

    auto ParseStmt(Parser &parser) -> Stmt {
        if (parser.Check(TokenKind::LBrace)) {
            return ParseBlockStmt(parser);
        }

        if (parser.Match(TokenKind::KwAsm)) {
            return ParseAsmStmt(parser);
        }

        return ParseExprStmt(parser);
    }
}
