#include <Compiler/Ast.hpp>

#include <format>

namespace Ast {
    namespace {
        auto ParseFunctionParams(Parser &parser) -> std::vector<FunctionDecl::Param> {
            parser.Expect(TokenKind::LParen, "'('");

            std::vector<FunctionDecl::Param> params;

            while (!parser.Check(TokenKind::RParen) && !parser.AtEnd()) {
                const auto param_location = parser.CurrentLocation();
                const auto param_is_mutable = parser.Match(TokenKind::KwMut);
                const auto param_name = parser.Expect(TokenKind::Identifier, "parameter name");

                parser.Expect(TokenKind::Colon, "':'");

                params.push_back({
                    .IsMutable = param_is_mutable,
                    .Name = param_name.Lexeme,
                    .Type = ParseType(parser),
                    .Location = param_location,
                });

                if (!parser.Check(TokenKind::RParen)) {
                    parser.Expect(TokenKind::Comma, "','");
                }
            }

            parser.Expect(TokenKind::RParen, "')'");

            return params;
        }

        auto parse_function_return_types(Parser &parser) -> std::vector<Type> {
            std::vector<Type> return_types;

            if (parser.Match(TokenKind::Arrow)) {
                return_types.push_back(ParseType(parser));

                while (parser.Match(TokenKind::Comma)) {
                    return_types.push_back(ParseType(parser));
                }
            }

            return return_types;
        }

        auto ParseFunctionDecl(Parser &parser, bool is_public) -> std::unique_ptr<FunctionDecl> {
            auto location = parser.CurrentLocation();

            parser.Expect(TokenKind::KwFn, "'fn'");

            auto fn_name = parser.Expect(TokenKind::Identifier, "function name").Lexeme;
            auto fn_params = ParseFunctionParams(parser);
            auto fn_return_types = parse_function_return_types(parser);
            auto fn_body = ParseStmt(parser);

            return std::make_unique<FunctionDecl>(FunctionDecl{
                .IsPublic = is_public,
                .Name = std::move(fn_name),
                .Params = std::move(fn_params),
                .ReturnTypes = std::move(fn_return_types),
                .Body = std::move(fn_body),
                .Location = location,
            });
        }
    }

    auto ParseDecl(Parser &parser, const bool top_level) -> std::optional<Decl> {
        const auto is_public = !top_level || parser.Match(TokenKind::KwPub);

        if (parser.Check(TokenKind::KwFn)) {
            return ParseFunctionDecl(parser, is_public);
        }

        parser.ReportError(
            parser.CurrentLocation(),
            std::format(
                "expected declaration, got '{}'",
                parser.CurrentLexeme()));

        parser.Advance();

        return std::nullopt;
    }
}
