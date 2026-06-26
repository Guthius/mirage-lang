#include <Compiler/Ast.hpp>

#include <format>

namespace Ast {
    namespace {
        auto parse_function_params(Parser &parser) -> std::vector<FunctionDecl::param> {
            parser.Expect(TokenKind::LParen, "'('");

            std::vector<FunctionDecl::param> params;

            while (!parser.Check(TokenKind::RParen) && !parser.AtEnd()) {
                const auto param_location = parser.CurrentLocation();
                const auto param_is_mutable = parser.Match(TokenKind::KwMut);
                const auto param_name = parser.Expect(TokenKind::Identifier, "parameter name");

                parser.Expect(TokenKind::Colon, "':'");

                params.push_back({
                    .is_mut = param_is_mutable,
                    .name = param_name.Lexeme,
                    .type = parse_type(parser),
                    .location = param_location,
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
                return_types.push_back(parse_type(parser));

                while (parser.Match(TokenKind::Comma)) {
                    return_types.push_back(parse_type(parser));
                }
            }

            return return_types;
        }

        auto parse_function_decl(Parser &parser, const bool is_pub) -> FunctionDecl {
            const auto location = parser.CurrentLocation();

            parser.Expect(TokenKind::KwFn, "'fn'");

            auto fn_name = parser.Expect(TokenKind::Identifier, "function name").Lexeme;
            auto fn_params = parse_function_params(parser);
            auto fn_return_types = parse_function_return_types(parser);
            auto fn_body = ParseStmt(parser);

            return FunctionDecl{
                .is_pub = is_pub,
                .name = std::move(fn_name),
                .params = std::move(fn_params),
                .return_types = std::move(fn_return_types),
                .body = std::move(fn_body),
                .location = location,
            };
        }

        auto parse_ext_function_params(Parser &parser) -> std::vector<ExtFunctionDecl::param> {
            parser.Expect(TokenKind::LParen, "'('");

            std::vector<ExtFunctionDecl::param> params;

            while (!parser.Check(TokenKind::RParen) && !parser.AtEnd()) {
                const auto param_location = parser.CurrentLocation();
                const auto param_name = parser.Expect(TokenKind::Identifier, "parameter name");

                parser.Expect(TokenKind::Colon, "':'");

                params.push_back({
                    .name = param_name.Lexeme,
                    .type = parse_type(parser),
                    .location = param_location,
                });

                if (!parser.Check(TokenKind::RParen)) {
                    parser.Expect(TokenKind::Comma, "','");
                }
            }

            parser.Expect(TokenKind::RParen, "')'");

            return params;
        }

        auto parse_ext_function_return_type(Parser &parser) -> std::optional<Type> {
            if (parser.Match(TokenKind::Arrow)) {
                return parse_type(parser);
            }

            return std::nullopt;
        }

        auto parse_ext_function_decl(Parser &parser, const bool is_pub) -> ExtFunctionDecl {
            const auto location = parser.CurrentLocation();

            parser.Expect(TokenKind::KwFn, "'fn'");

            auto fn_name = parser.Expect(TokenKind::Identifier, "identifier").Lexeme;
            auto fn_params = parse_ext_function_params(parser);
            auto fn_return_type = parse_ext_function_return_type(parser);

            return ExtFunctionDecl{
                .is_pub = is_pub,
                .name = fn_name,
                .params = std::move(fn_params),
                .return_type = std::move(fn_return_type),
                .location = location,
            };
        }

        auto parse_var_decl(Parser &parser, const bool is_pub) -> VarDecl {
            const auto location = parser.CurrentLocation();

            const auto is_mut = parser.Match(TokenKind::KwMut);
            if (!is_mut) {
                parser.Expect(TokenKind::KwConst, "'const' or 'mut'");
            }

            const auto var_name = parser.Expect(TokenKind::Identifier, "identifier").Lexeme;

            std::optional<Type> type = std::nullopt;
            if (parser.Match(TokenKind::Colon)) {
                type = parse_type(parser);
            }

            std::optional<Expr> init_expr = std::nullopt;
            if (type.has_value()) {
                if (parser.Match(TokenKind::Equal)) {
                    init_expr = parse_expr(parser, false);
                }
            } else {
                parser.Expect(TokenKind::ColonEqual, "':' or ':='");
                init_expr = parse_expr(parser, !is_mut);
            }

            if (!is_mut && init_expr == std::nullopt) {
                parser.ReportError(parser.CurrentLocation(), "'const' requires an initializer");
            }

            return VarDecl{
                .is_pub = is_pub,
                .is_mut = is_mut,
                .name = var_name,
                .type = std::move(type),
                .init = std::move(init_expr),
                .location = location,
            };
        }
    }

    auto parse_decl(Parser &parser, const bool top_level) -> std::optional<Decl> {
        const auto is_pub = !top_level || parser.Match(TokenKind::KwPub);

        if (parser.Check(TokenKind::Identifier) && parser.CurrentLexeme() == "ext") {
            parser.Advance();

            return parse_ext_function_decl(parser, is_pub);
        }

        if (parser.Check(TokenKind::KwFn)) {
            return parse_function_decl(parser, is_pub);
        }

        if (parser.Check(TokenKind::KwMut) || parser.Check(TokenKind::KwConst)) {
            return parse_var_decl(parser, is_pub);
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
