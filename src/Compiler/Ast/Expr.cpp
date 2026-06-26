#include <Compiler/Ast.hpp>

#include <format>

namespace Ast {
    namespace {
        template <typename T>
        auto make_expr(T &&value) -> std::unique_ptr<T> {
            return std::make_unique<T>(std::forward<T>(value));
        }

        auto parse_literal_integer_expr(Parser &parser) -> Expr {
            const auto ToInt = [](const char ch) -> uint64_t {
                if (std::isdigit(ch)) {
                    return ch - '0';
                }

                return 10 + (std::tolower(ch) - 'a');
            };

            const auto location = parser.CurrentLocation();
            auto &token = parser.Advance();

            uint64_t value = 0;

            if (const auto lexeme = token.Lexeme; lexeme.starts_with("0x") || lexeme.starts_with("0X")) {
                for (size_t i = 2; i < lexeme.size(); ++i) {
                    if (lexeme[i] == '_') {
                        continue;
                    }

                    value = value * 16 + ToInt(lexeme[i]);
                }
            } else if (lexeme.starts_with("0b") || lexeme.starts_with("0B")) {
                for (size_t i = 2; i < lexeme.size(); ++i) {
                    if (lexeme[i] == '_') {
                        continue;
                    }

                    value = value * 2 + (lexeme[i] - '0');
                }
            } else {
                for (char c : lexeme) {
                    if (c == '_') {
                        continue;
                    }

                    value = value * 10 + (c - '0');
                }
            }

            return LiteralIntegerExpr{
                .Value = value,
                .Location = location,
            };
        }

        auto parse_primary(Parser &parser) -> Expr {
            const auto location = parser.CurrentLocation();

            if (parser.Check(TokenKind::IntLiteral)) {
                return parse_literal_integer_expr(parser);
            }

            if (parser.Check(TokenKind::FloatLiteral)) {
                auto &token = parser.Advance();

                return LiteralFloatExpr{
                    .Value = std::stod(token.Lexeme),
                    .Location = location,
                };
            }

            if (parser.Check(TokenKind::StringLiteral)) {
                auto &token = parser.Advance();

                auto value = token.Lexeme;
                if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
                    value = value.substr(1, value.size() - 2);
                }

                return LiteralStringExpr{
                    .Value = std::move(value),
                    .Location = location,
                };
            }

            if (parser.Check(TokenKind::KwTrue)) {
                parser.Advance();

                return LiteralBoolExpr{
                    .Value = true,
                    .Location = location,
                };
            }

            if (parser.Check(TokenKind::KwFalse)) {
                parser.Advance();

                return LiteralBoolExpr{
                    .Value = false,
                    .Location = location,
                };
            }

            if (parser.Check(TokenKind::Identifier)) {
                auto &token = parser.Advance();

                return IdentExpr{
                    .Name = token.Lexeme,
                    .Location = location,
                };
            }

            if (parser.Check(TokenKind::KwNil)) {
                parser.Advance();

                return LiteralNilExpr{
                    .Location = location,
                };
            }

            if (parser.Match(TokenKind::LParen)) {
                auto inner = parse_expr(parser);

                parser.Expect(TokenKind::RParen, "')'");

                return inner;
            }

            parser.ReportError(location, std::format("expected expression, got '{}'", parser.CurrentLexeme()));
            parser.Advance();

            return LiteralIntegerExpr{
                .Value = 0,
                .Location = location,
            };
        }

        auto parse_postfix(Parser &parser) -> Expr {
            auto expr = parse_primary(parser);

            while (true) {
                if (parser.Check(TokenKind::LParen)) {
                    const auto location = parser.CurrentLocation();

                    parser.Advance();

                    std::vector<Expr> args;
                    while (!parser.Check(TokenKind::RParen) && !parser.AtEnd()) {
                        args.push_back(parse_expr(parser));
                        if (!parser.Check(TokenKind::RParen)) {
                            parser.Expect(TokenKind::Comma, "','");
                        }
                    }

                    parser.Expect(TokenKind::RParen, "')'");

                    expr = make_expr(CallExpr{
                        .Callee = std::move(expr),
                        .Args = std::move(args),
                        .Location = location,
                    });
                } else if (parser.Check(TokenKind::PlusPlus)) {
                    const auto location = parser.CurrentLocation();

                    parser.Advance();

                    expr = make_expr(IncrDecrExpr{
                        .Operand = std::move(expr),
                        .IsIncrement = true,
                        .IsPrefix = false,
                        .Location = location,
                    });
                } else if (parser.Check(TokenKind::MinusMinus)) {
                    const auto location = parser.CurrentLocation();

                    parser.Advance();

                    expr = make_expr(IncrDecrExpr{
                        .Operand = std::move(expr),
                        .IsIncrement = false,
                        .IsPrefix = false,
                        .Location = location,
                    });
                } else {
                    break;
                }
            }

            return expr;
        }

        auto parse_unary(Parser &parser) -> Expr {
            auto MatchUnaryOp = [](const TokenKind kind) -> std::optional<UnaryOp> {
                switch (kind) {
                case TokenKind::Minus:     return UnaryOp::Negate;
                case TokenKind::Bang:      return UnaryOp::LogicalNot;
                case TokenKind::Tilde:     return UnaryOp::BitwiseNot;
                case TokenKind::Ampersand: return UnaryOp::AddressOf;
                default:                   return std::nullopt;
                }
            };

            if (auto op = MatchUnaryOp(parser.Current().Kind)) {
                const auto location = parser.CurrentLocation();

                parser.Advance();

                return make_expr(UnaryExpr{
                    .Op = *op,
                    .Operand = parse_unary(parser),
                    .Location = location,
                });
            }

            return parse_postfix(parser);
        }

        auto parse_multiplicative(Parser &parser) -> Expr {
            auto lhs = parse_unary(parser);

            while (parser.Check(TokenKind::Star) ||
                   parser.Check(TokenKind::Slash) ||
                   parser.Check(TokenKind::Percent)) {

                BinaryOp op;

                switch (parser.Current().Kind) {
                case TokenKind::Star:    op = BinaryOp::Mul; break;
                case TokenKind::Slash:   op = BinaryOp::Div; break;
                case TokenKind::Percent: op = BinaryOp::Mod; break;
                default:                 __builtin_unreachable();
                }

                const auto location = parser.CurrentLocation();

                parser.Advance();

                lhs = make_expr(BinaryExpr{
                    .Op = op,
                    .Lhs = std::move(lhs),
                    .Rhs = parse_unary(parser),
                    .Location = location,
                });
            }

            return lhs;
        }

        auto parse_additive(Parser &parser) -> Expr {
            auto lhs = parse_multiplicative(parser);

            while (parser.Check(TokenKind::Plus) || parser.Check(TokenKind::Minus)) {
                const auto op = parser.Current().Kind == TokenKind::Plus ? BinaryOp::Add : BinaryOp::Sub;
                const auto location = parser.CurrentLocation();

                parser.Advance();

                lhs = make_expr(BinaryExpr{
                    .Op = op,
                    .Lhs = std::move(lhs),
                    .Rhs = parse_multiplicative(parser),
                    .Location = location,
                });
            }

            return lhs;
        }

        auto parse_shift(Parser &parser) -> Expr {
            auto lhs = parse_additive(parser);

            while (parser.Check(TokenKind::ShiftLeft) || parser.Check(TokenKind::ShiftRight)) {
                const auto op = parser.Current().Kind == TokenKind::ShiftLeft ? BinaryOp::ShiftLeft : BinaryOp::ShiftRight;
                const auto location = parser.CurrentLocation();

                parser.Advance();

                lhs = make_expr(BinaryExpr{
                    .Op = op,
                    .Lhs = std::move(lhs),
                    .Rhs = parse_additive(parser),
                    .Location = location,
                });
            }

            return lhs;
        }

        auto parse_comparison(Parser &parser) -> Expr {
            auto lhs = parse_shift(parser);

            while (parser.Check(TokenKind::Less) ||
                   parser.Check(TokenKind::Greater) ||
                   parser.Check(TokenKind::LessEqual) ||
                   parser.Check(TokenKind::GreaterEqual)) {

                BinaryOp op;

                switch (parser.Current().Kind) {
                case TokenKind::Less:         op = BinaryOp::Less; break;
                case TokenKind::Greater:      op = BinaryOp::Greater; break;
                case TokenKind::LessEqual:    op = BinaryOp::LessEqual; break;
                case TokenKind::GreaterEqual: op = BinaryOp::GreaterEqual; break;
                default:                      __builtin_unreachable();
                }

                const auto location = parser.CurrentLocation();

                parser.Advance();

                lhs = make_expr(BinaryExpr{
                    .Op = op,
                    .Lhs = std::move(lhs),
                    .Rhs = parse_shift(parser),
                    .Location = location,
                });
            }

            return lhs;
        }

        auto parse_equality(Parser &parser) -> Expr {
            auto lhs = parse_comparison(parser);

            while (parser.Check(TokenKind::EqualEqual) || parser.Check(TokenKind::BangEqual)) {
                const auto op = parser.Current().Kind == TokenKind::EqualEqual ? BinaryOp::Equal : BinaryOp::NotEqual;
                const auto location = parser.CurrentLocation();

                parser.Advance();

                lhs = make_expr(BinaryExpr{
                    .Op = op,
                    .Lhs = std::move(lhs),
                    .Rhs = parse_comparison(parser),
                    .Location = location,
                });
            }

            return lhs;
        }

        auto parse_bitwise_and(Parser &parser) -> Expr {
            auto lhs = parse_equality(parser);

            while (parser.Check(TokenKind::Ampersand) && !parser.Check(TokenKind::AmpAmp)) {
                parser.Advance();

                const auto location = parser.CurrentLocation();

                lhs = make_expr(BinaryExpr{
                    .Op = BinaryOp::BitwiseAnd,
                    .Lhs = std::move(lhs),
                    .Rhs = parse_equality(parser),
                    .Location = location,
                });
            }

            return lhs;
        }

        auto parse_bitwise_xor(Parser &parser) -> Expr {
            auto lhs = parse_bitwise_and(parser);

            while (parser.Match(TokenKind::Caret)) {
                const auto location = parser.CurrentLocation();

                lhs = make_expr(BinaryExpr{
                    .Op = BinaryOp::BitwiseXor,
                    .Lhs = std::move(lhs),
                    .Rhs = parse_bitwise_and(parser),
                    .Location = location,
                });
            }

            return lhs;
        }

        auto parse_bitwise_or(Parser &parser) -> Expr {
            auto lhs = parse_bitwise_xor(parser);

            while (parser.Check(TokenKind::Pipe) && !parser.Check(TokenKind::PipePipe)) {
                parser.Advance();

                const auto location = parser.CurrentLocation();

                return make_expr(BinaryExpr{
                    .Op = BinaryOp::BitwiseOr,
                    .Lhs = std::move(lhs),
                    .Rhs = parse_bitwise_xor(parser),
                    .Location = location,
                });
            }

            return lhs;
        }

        auto parse_logical_and(Parser &parser) -> Expr {
            auto lhs = parse_bitwise_or(parser);

            while (parser.Match(TokenKind::AmpAmp)) {
                const auto location = parser.CurrentLocation();

                return make_expr(BinaryExpr{
                    .Op = BinaryOp::LogicalAnd,
                    .Lhs = std::move(lhs),
                    .Rhs = parse_bitwise_or(parser),
                    .Location = location,
                });
            }

            return lhs;
        }

        auto parse_logical_or(Parser &parser) -> Expr {
            auto lhs = parse_logical_and(parser);

            while (parser.Match(TokenKind::PipePipe)) {
                const auto location = parser.CurrentLocation();

                return make_expr(BinaryExpr{
                    .Op = BinaryOp::LogicalOr,
                    .Lhs = std::move(lhs),
                    .Rhs = parse_logical_and(parser),
                    .Location = location,
                });
            }

            return lhs;
        }

        auto parse_ternary_expr(Parser &parser) -> Expr {
            auto expr = parse_logical_or(parser);

            if (parser.Match(TokenKind::Question)) {
                const auto location = parser.CurrentLocation();

                auto then_expr = parse_expr(parser);
                parser.Expect(TokenKind::Colon, "':'");
                auto else_expr = parse_expr(parser);

                return make_expr(TernaryExpr{
                    .Condition = std::move(expr),
                    .ThenExpr = std::move(then_expr),
                    .ElseExpr = std::move(else_expr),
                    .Location = location,
                });
            }

            return expr;
        }

        auto parse_assign_expr(Parser &parser) -> Expr {
            auto expr = parse_ternary_expr(parser);

            auto MatchAssignOp = [](const TokenKind kind) -> std::optional<AssignOp> {
                switch (kind) {
                case TokenKind::Equal:           return AssignOp::Assign;
                case TokenKind::PlusEqual:       return AssignOp::AddAssign;
                case TokenKind::MinusEqual:      return AssignOp::SubAssign;
                case TokenKind::StarEqual:       return AssignOp::MulAssign;
                case TokenKind::SlashEqual:      return AssignOp::DivAssign;
                case TokenKind::AmpEqual:        return AssignOp::AndAssign;
                case TokenKind::PipeEqual:       return AssignOp::OrAssign;
                case TokenKind::CaretEqual:      return AssignOp::XorAssign;
                case TokenKind::ShiftLeftEqual:  return AssignOp::ShlAssign;
                case TokenKind::ShiftRightEqual: return AssignOp::ShrAssign;
                default:                         return std::nullopt;
                }
            };

            if (const auto op = MatchAssignOp(parser.Current().Kind)) {
                const auto location = parser.CurrentLocation();

                parser.Advance();

                return make_expr(AssignExpr{
                    .Op = *op,
                    .Target = std::move(expr),
                    .Value = parse_assign_expr(parser),
                    .Location = location,
                });
            }

            return expr;
        }

        auto parse_import_expr(Parser &parser) -> Expr {
            const auto location = parser.CurrentLocation();

            parser.Expect(TokenKind::KwImport, "'import'");
            parser.Expect(TokenKind::LParen, "'('");

            auto module_name = parser.Expect(TokenKind::StringLiteral, "string literal").Lexeme;

            parser.Expect(TokenKind::RParen, "')'");

            return ImportExpr{
                .ModuleName = module_name,
                .Location = location,
            };
        }
    }

    auto parse_expr(Parser &parser, const bool allow_import) -> Expr {
        if (parser.Check(TokenKind::KwImport)) {
            if (allow_import) {
                return parse_import_expr(parser);
            }

            parser.ReportError(parser.CurrentLocation(), "'import()' can only be used to initialize a 'const' declaration with no explicit type");

            return parse_import_expr(parser);
        }

        return parse_assign_expr(parser);
    }
}
