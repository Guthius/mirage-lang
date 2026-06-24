#include "../Ast.hpp"

#include <format>

namespace Ast {
    namespace {
        auto ParseFunctionParams(Parser &parser) -> std::vector<FunctionDecl::Param> {
            parser.Expect(TokenKind::lparen, "'('");

            std::vector<FunctionDecl::Param> params;

            while (!parser.Check(TokenKind::rparen) && !parser.AtEnd()) {
                const auto param_location = parser.CurrentLocation();
                const auto param_is_mutable = parser.Match(TokenKind::kw_mut);
                const auto param_name = parser.Expect(TokenKind::identifier, "parameter name");

                parser.Expect(TokenKind::colon, "':'");

                params.push_back({
                    .IsMutable = param_is_mutable,
                    .Name = param_name.Lexeme,
                    .Type = ParseType(parser),
                    .Location = param_location,
                });

                if (!parser.Check(TokenKind::rparen)) {
                    parser.Expect(TokenKind::comma, "','");
                }
            }

            parser.Expect(TokenKind::rparen, "')'");

            return params;
        }

        auto parse_function_return_types(Parser &parser) -> std::vector<Type> {
            std::vector<Type> return_types;

            if (parser.Match(TokenKind::arrow)) {
                return_types.push_back(ParseType(parser));

                while (parser.Match(TokenKind::comma)) {
                    return_types.push_back(ParseType(parser));
                }
            }

            return return_types;
        }

        auto ParseFunctionDecl(Parser &parser, bool is_public) -> std::unique_ptr<FunctionDecl> {
            auto location = parser.CurrentLocation();

            parser.Expect(TokenKind::kw_fn, "'fn'");

            auto fn_name = parser.Expect(TokenKind::identifier, "function name").Lexeme;
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
        const auto is_public = !top_level || parser.Match(TokenKind::kw_pub);

        if (parser.Check(TokenKind::kw_fn)) {
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
