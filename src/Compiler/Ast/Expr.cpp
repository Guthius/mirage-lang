#include <Compiler/Ast.hpp>

#include <format>

namespace Ast {
    namespace {
        template <typename T>
        auto MakeExpr(T &&value) -> std::unique_ptr<T> {
            return std::make_unique<T>(std::forward<T>(value));
        }

        auto ParseIntLiteralExpr(Parser &p) -> Expr {
            const auto ToInt = [](const char ch) -> uint64_t {
                if (std::isdigit(ch)) {
                    return ch - '0';
                }

                return 10 + (std::tolower(ch) - 'a');
            };

            const auto location = p.CurrentLocation();
            auto &token = p.Advance();

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

            return MakeExpr(IntLiteralExpr{
                .Value = value,
                .Location = location,
            });
        }

        auto ParsePrimary(Parser &parser) -> Expr {
            const auto location = parser.CurrentLocation();

            if (parser.Check(TokenKind::IntLiteral)) {
                return ParseIntLiteralExpr(parser);
            }

            if (parser.Check(TokenKind::FloatLiteral)) {
                auto &token = parser.Advance();

                return MakeExpr(FloatLiteralExpr{
                    .Value = std::stod(token.Lexeme),
                    .Location = location,
                });
            }

            if (parser.Check(TokenKind::StringLiteral)) {
                auto &token = parser.Advance();

                auto value = token.Lexeme;
                if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
                    value = value.substr(1, value.size() - 2);
                }

                return MakeExpr(StringLiteralExpr{
                    .Value = std::move(value),
                    .Location = location,
                });
            }

            if (parser.Check(TokenKind::KwTrue)) {
                parser.Advance();

                return MakeExpr(BoolLiteralExpr{
                    .Value = true,
                    .Location = location,
                });
            }

            if (parser.Check(TokenKind::KwFalse)) {
                parser.Advance();

                return MakeExpr(BoolLiteralExpr{
                    .Value = false,
                    .Location = location,
                });
            }

            if (parser.Check(TokenKind::Identifier)) {
                auto &token = parser.Advance();

                return MakeExpr(IdentExpr{
                    .Name = token.Lexeme,
                    .Location = location,
                });
            }

            if (parser.Check(TokenKind::KwNil)) {
                parser.Advance();

                return MakeExpr(NilLiteralExpr{
                    .Location = location,
                });
            }

            if (parser.Match(TokenKind::LParen)) {
                auto inner = ParseExpr(parser);

                parser.Expect(TokenKind::RParen, "')'");

                return inner;
            }

            parser.ReportError(location, std::format("expected expression, got '{}'", parser.CurrentLexeme()));
            parser.Advance();

            return MakeExpr(IntLiteralExpr{
                .Value = 0,
                .Location = location,
            });
        }

        auto ParsePostfix(Parser &parser) -> Expr {
            auto expr = ParsePrimary(parser);

            while (true) {
                if (parser.Check(TokenKind::LParen)) {
                    const auto location = parser.CurrentLocation();

                    parser.Advance();

                    std::vector<Expr> args;
                    while (!parser.Check(TokenKind::RParen) && !parser.AtEnd()) {
                        args.push_back(ParseExpr(parser));
                        if (!parser.Check(TokenKind::RParen)) {
                            parser.Expect(TokenKind::Comma, "','");
                        }
                    }

                    parser.Expect(TokenKind::RParen, "')'");

                    expr = MakeExpr(CallExpr{
                        .Callee = std::move(expr),
                        .Args = std::move(args),
                        .Location = location,
                    });
                } else if (parser.Check(TokenKind::PlusPlus)) {
                    const auto location = parser.CurrentLocation();

                    parser.Advance();

                    expr = MakeExpr(IncrDecrExpr{
                        .Operand = std::move(expr),
                        .IsIncrement = true,
                        .IsPrefix = false,
                        .Location = location,
                    });
                } else if (parser.Check(TokenKind::MinusMinus)) {
                    const auto location = parser.CurrentLocation();

                    parser.Advance();

                    expr = MakeExpr(IncrDecrExpr{
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

        auto ParseUnary(Parser &parser) -> Expr {
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

                return MakeExpr(UnaryExpr{
                    .Op = *op,
                    .Operand = ParseUnary(parser),
                    .Location = location,
                });
            }

            return ParsePostfix(parser);
        }

        auto ParseMultiplicative(Parser &parser) -> Expr {
            auto lhs = ParseUnary(parser);

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

                lhs = MakeExpr(BinaryExpr{
                    .Op = op,
                    .Lhs = std::move(lhs),
                    .Rhs = ParseUnary(parser),
                    .Location = location,
                });
            }

            return lhs;
        }

        auto ParseAdditive(Parser &parser) -> Expr {
            auto lhs = ParseMultiplicative(parser);

            while (parser.Check(TokenKind::Plus) || parser.Check(TokenKind::Minus)) {
                const auto op = parser.Current().Kind == TokenKind::Plus ? BinaryOp::Add : BinaryOp::Sub;
                const auto location = parser.CurrentLocation();

                parser.Advance();

                lhs = MakeExpr(BinaryExpr{
                    .Op = op,
                    .Lhs = std::move(lhs),
                    .Rhs = ParseMultiplicative(parser),
                    .Location = location,
                });
            }

            return lhs;
        }

        auto ParseShift(Parser &parser) -> Expr {
            auto lhs = ParseAdditive(parser);

            while (parser.Check(TokenKind::ShiftLeft) || parser.Check(TokenKind::ShiftRight)) {
                const auto op = parser.Current().Kind == TokenKind::ShiftLeft ? BinaryOp::ShiftLeft : BinaryOp::ShiftRight;
                const auto location = parser.CurrentLocation();

                parser.Advance();

                lhs = MakeExpr(BinaryExpr{
                    .Op = op,
                    .Lhs = std::move(lhs),
                    .Rhs = ParseAdditive(parser),
                    .Location = location,
                });
            }

            return lhs;
        }

        auto ParseComparison(Parser &parser) -> Expr {
            auto lhs = ParseShift(parser);

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

                lhs = MakeExpr(BinaryExpr{
                    .Op = op,
                    .Lhs = std::move(lhs),
                    .Rhs = ParseShift(parser),
                    .Location = location,
                });
            }

            return lhs;
        }

        auto ParseEquality(Parser &parser) -> Expr {
            auto lhs = ParseComparison(parser);

            while (parser.Check(TokenKind::EqualEqual) || parser.Check(TokenKind::BangEqual)) {
                const auto op = parser.Current().Kind == TokenKind::EqualEqual ? BinaryOp::Equal : BinaryOp::NotEqual;
                const auto location = parser.CurrentLocation();

                parser.Advance();

                lhs = MakeExpr(BinaryExpr{
                    .Op = op,
                    .Lhs = std::move(lhs),
                    .Rhs = ParseComparison(parser),
                    .Location = location,
                });
            }

            return lhs;
        }

        auto ParseBitwiseAnd(Parser &parser) -> Expr {
            auto lhs = ParseEquality(parser);

            while (parser.Check(TokenKind::Ampersand) && !parser.Check(TokenKind::AmpAmp)) {
                parser.Advance();

                const auto location = parser.CurrentLocation();

                lhs = MakeExpr(BinaryExpr{
                    .Op = BinaryOp::BitwiseAnd,
                    .Lhs = std::move(lhs),
                    .Rhs = ParseEquality(parser),
                    .Location = location,
                });
            }

            return lhs;
        }

        auto ParseBitwiseXor(Parser &parser) -> Expr {
            auto lhs = ParseBitwiseAnd(parser);

            while (parser.Match(TokenKind::Caret)) {
                const auto location = parser.CurrentLocation();

                lhs = MakeExpr(BinaryExpr{
                    .Op = BinaryOp::BitwiseXor,
                    .Lhs = std::move(lhs),
                    .Rhs = ParseBitwiseAnd(parser),
                    .Location = location,
                });
            }

            return lhs;
        }

        auto ParseBitwiseOr(Parser &parser) -> Expr {
            auto lhs = ParseBitwiseXor(parser);

            while (parser.Check(TokenKind::Pipe) && !parser.Check(TokenKind::PipePipe)) {
                parser.Advance();

                const auto location = parser.CurrentLocation();

                return MakeExpr(BinaryExpr{
                    .Op = BinaryOp::BitwiseOr,
                    .Lhs = std::move(lhs),
                    .Rhs = ParseBitwiseXor(parser),
                    .Location = location,
                });
            }

            return lhs;
        }

        auto ParseLogicalAnd(Parser &parser) -> Expr {
            auto lhs = ParseBitwiseOr(parser);

            while (parser.Match(TokenKind::AmpAmp)) {
                const auto location = parser.CurrentLocation();

                return MakeExpr(BinaryExpr{
                    .Op = BinaryOp::LogicalAnd,
                    .Lhs = std::move(lhs),
                    .Rhs = ParseBitwiseOr(parser),
                    .Location = location,
                });
            }

            return lhs;
        }

        auto ParseLogicalOr(Parser &parser) -> Expr {
            auto lhs = ParseLogicalAnd(parser);

            while (parser.Match(TokenKind::PipePipe)) {
                const auto location = parser.CurrentLocation();

                return MakeExpr(BinaryExpr{
                    .Op = BinaryOp::LogicalOr,
                    .Lhs = std::move(lhs),
                    .Rhs = ParseLogicalAnd(parser),
                    .Location = location,
                });
            }

            return lhs;
        }

        auto ParseTernaryExpr(Parser &parser) -> Expr {
            auto expr = ParseLogicalOr(parser);

            if (parser.Match(TokenKind::Question)) {
                const auto location = parser.CurrentLocation();

                auto then_expr = ParseExpr(parser);
                parser.Expect(TokenKind::Colon, "':'");
                auto else_expr = ParseExpr(parser);

                return MakeExpr(TernaryExpr{
                    .Condition = std::move(expr),
                    .ThenExpr = std::move(then_expr),
                    .ElseExpr = std::move(else_expr),
                    .Location = location,
                });
            }

            return expr;
        }

        auto ParseAssignExpr(Parser &parser) -> Expr {
            auto expr = ParseTernaryExpr(parser);

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

                return MakeExpr(AssignExpr{
                    .Op = *op,
                    .Target = std::move(expr),
                    .Value = ParseAssignExpr(parser),
                    .Location = location,
                });
            }

            return expr;
        }
    }

    auto ParseExpr(Parser &parser) -> Expr {
        return ParseAssignExpr(parser);
    }
}
