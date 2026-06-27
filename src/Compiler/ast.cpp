#include "ast.hpp"

#include <format>

namespace ast {
    namespace {
        auto parse_named_type(Parser &parser) -> NamedType {
            const auto location = parser.current_location();
            const auto &name = parser.expect(TokenKind::Identifier, "type name").lexeme;

            if (parser.match(TokenKind::Dot)) {
                auto member = parse_named_type(parser);

                return NamedType{
                    .name = name,
                    .member = std::make_unique<NamedType>(std::move(member)),
                    .location = location,
                };
            }

            return NamedType{
                .name = name,
                .member = nullptr,
                .location = location,
            };
        }

        auto parse_builtin_type_kind(Parser &parser) -> std::optional<BuiltinTypeKind> {
            switch (parser.advance().kind) {
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

        auto parse_struct_type(Parser &parser) -> Type {
            const auto location = parser.current_location();

            parser.expect(TokenKind::KwStruct, "'struct'");
            parser.expect(TokenKind::LBrace, "'{'");

            std::vector<StructType::Field> fields;
            while (!parser.check(TokenKind::RBrace) && !parser.at_end()) {
                const auto field_location = parser.current_location();
                const auto field_name = parser.expect(TokenKind::Identifier, "identifier").lexeme;

                parser.expect(TokenKind::Colon, "':'");

                fields.push_back({
                    .name = field_name,
                    .type = parse_type(parser),
                    .location = field_location,
                });
            }

            parser.expect(TokenKind::RBrace, "'}'");

            return std::make_unique<StructType>(StructType{
                .fields = std::move(fields),
                .location = location,
            });
        }

        template <typename T>
        auto make_expr(T &&value) -> std::unique_ptr<T> {
            return std::make_unique<T>(std::forward<T>(value));
        }

        auto can_start_expr(const TokenKind kind) -> bool {
            switch (kind) {
            case TokenKind::Identifier:
            case TokenKind::IntLiteral:
            case TokenKind::FloatLiteral:
            case TokenKind::StringLiteral:
            case TokenKind::KwTrue:
            case TokenKind::KwFalse:
            case TokenKind::KwNil:
            case TokenKind::LParen:
            case TokenKind::Minus:
            case TokenKind::Bang:
            case TokenKind::Tilde:
            case TokenKind::Ampersand:
            case TokenKind::KwCast:
            case TokenKind::KwSizeOf:
            case TokenKind::KwLen:
            case TokenKind::KwDefault:
                return true;
            default:
                return false;
            }
        }

        auto parse_literal_integer_expr(Parser &parser) -> Expr {
            const auto ToInt = [](const char ch) -> uint64_t {
                if (std::isdigit(ch)) {
                    return ch - '0';
                }

                return 10 + (std::tolower(ch) - 'a');
            };

            const auto location = parser.current_location();
            auto &token = parser.advance();

            uint64_t value = 0;

            if (const auto lexeme = token.lexeme; lexeme.starts_with("0x") || lexeme.starts_with("0X")) {
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
                for (const char c : lexeme) {
                    if (c == '_') {
                        continue;
                    }

                    value = value * 10 + (c - '0');
                }
            }

            return LiteralIntegerExpr{
                .value = value,
                .location = location,
            };
        }

        auto parse_cast_expr(Parser &parser) -> Expr {
            const auto location = parser.current_location();

            parser.expect(TokenKind::KwCast, "'cast'");
            parser.expect(TokenKind::LParen, "'('");
            auto expr = parse_expr(parser);
            parser.expect(TokenKind::Comma, "','");
            auto as_type = parse_type(parser);
            parser.expect(TokenKind::RParen, "')'");

            return std::make_unique<CastExpr>(CastExpr{
                .value = std::move(expr),
                .as_type = std::move(as_type),
                .location = location,
            });
        }

        auto parse_primary(Parser &parser) -> Expr {
            const auto location = parser.current_location();

            if (parser.check(TokenKind::IntLiteral)) {
                return parse_literal_integer_expr(parser);
            }

            if (parser.check(TokenKind::FloatLiteral)) {
                auto &token = parser.advance();

                return LiteralFloatExpr{
                    .value = std::stod(token.lexeme),
                    .location = location,
                };
            }

            if (parser.check(TokenKind::StringLiteral)) {
                auto &token = parser.advance();

                auto value = token.lexeme;
                if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
                    value = value.substr(1, value.size() - 2);
                }

                return LiteralStringExpr{
                    .value = std::move(value),
                    .location = location,
                };
            }

            if (parser.check(TokenKind::KwTrue)) {
                parser.advance();

                return LiteralBoolExpr{
                    .value = true,
                    .location = location,
                };
            }

            if (parser.check(TokenKind::KwFalse)) {
                parser.advance();

                return LiteralBoolExpr{
                    .value = false,
                    .location = location,
                };
            }

            if (parser.check(TokenKind::Identifier)) {
                auto &token = parser.advance();

                return IdentExpr{
                    .name = token.lexeme,
                    .location = location,
                };
            }

            if (parser.check(TokenKind::KwNil)) {
                parser.advance();

                return LiteralNilExpr{
                    .location = location,
                };
            }

            if (parser.check(TokenKind::KwSizeOf)) {
                parser.advance();

                return std::make_unique<SizeOfExpr>(SizeOfExpr{
                    .operand = parse_expr(parser),
                    .location = location,
                });
            }

            if (parser.check(TokenKind::KwCast)) {
                return parse_cast_expr(parser);
            }

            if (parser.match(TokenKind::LParen)) {
                auto inner = parse_expr(parser);

                parser.expect(TokenKind::RParen, "')'");

                return inner;
            }

            parser.report_error(location, std::format("expected expression, got '{}'", parser.current_lexeme()));
            parser.advance();

            return LiteralIntegerExpr{
                .value = 0,
                .location = location,
            };
        }

        auto parse_postfix(Parser &parser) -> Expr {
            auto expr = parse_primary(parser);

            const auto location = parser.current_location();

            while (true) {
                if (parser.check(TokenKind::LParen)) {
                    parser.advance();

                    std::vector<Expr> args;
                    while (!parser.check(TokenKind::RParen) && !parser.at_end()) {
                        args.push_back(parse_expr(parser));
                        if (!parser.check(TokenKind::RParen)) {
                            parser.expect(TokenKind::Comma, "','");
                        }
                    }

                    parser.expect(TokenKind::RParen, "')'");

                    expr = make_expr(CallExpr{
                        .callee = std::move(expr),
                        .args = std::move(args),
                        .location = location,
                    });
                } else if (parser.check(TokenKind::Dot)) {
                    parser.advance();

                    expr = make_expr(MemberExpr{
                        .object = std::move(expr),
                        .member = parser.expect_identifier(),
                        .location = location,
                    });
                } else if (parser.check(TokenKind::PlusPlus)) {
                    parser.advance();

                    expr = make_expr(IncrDecrExpr{
                        .operand = std::move(expr),
                        .is_increment = true,
                        .is_prefix = false,
                        .location = location,
                    });
                } else if (parser.check(TokenKind::MinusMinus)) {
                    parser.advance();

                    expr = make_expr(IncrDecrExpr{
                        .operand = std::move(expr),
                        .is_increment = false,
                        .is_prefix = false,
                        .location = location,
                    });
                } else if (parser.check(TokenKind::Star) && !can_start_expr(parser.peek().kind)) {
                    parser.advance();

                    expr = make_expr(DerefExpr{
                        .operand = std::move(expr),
                        .location = location,
                    });
                } else {
                    break;
                }
            }

            return expr;
        }

        auto parse_unary(Parser &parser) -> Expr {
            auto match_unary_op = [](const TokenKind kind) -> std::optional<UnaryOp> {
                switch (kind) {
                case TokenKind::Minus:     return UnaryOp::Negate;
                case TokenKind::Bang:      return UnaryOp::LogicalNot;
                case TokenKind::Tilde:     return UnaryOp::BitwiseNot;
                case TokenKind::Ampersand: return UnaryOp::AddressOf;
                default:                   return std::nullopt;
                }
            };

            if (const auto op = match_unary_op(parser.current().kind)) {
                const auto location = parser.current_location();

                parser.advance();

                return make_expr(UnaryExpr{
                    .op = *op,
                    .operand = parse_unary(parser),
                    .location = location,
                });
            }

            return parse_postfix(parser);
        }

        auto parse_multiplicative(Parser &parser) -> Expr {
            auto lhs = parse_unary(parser);

            while (parser.check(TokenKind::Star) ||
                   parser.check(TokenKind::Slash) ||
                   parser.check(TokenKind::Percent)) {

                BinaryOp op;

                switch (parser.current().kind) {
                case TokenKind::Star:    op = BinaryOp::Mul; break;
                case TokenKind::Slash:   op = BinaryOp::Div; break;
                case TokenKind::Percent: op = BinaryOp::Mod; break;
                default:                 __builtin_unreachable();
                }

                const auto location = parser.current_location();

                parser.advance();

                lhs = make_expr(BinaryExpr{
                    .op = op,
                    .lhs = std::move(lhs),
                    .rhs = parse_unary(parser),
                    .location = location,
                });
            }

            return lhs;
        }

        auto parse_additive(Parser &parser) -> Expr {
            auto lhs = parse_multiplicative(parser);

            while (parser.check(TokenKind::Plus) || parser.check(TokenKind::Minus)) {
                const auto op = parser.current().kind == TokenKind::Plus ? BinaryOp::Add : BinaryOp::Sub;
                const auto location = parser.current_location();

                parser.advance();

                lhs = make_expr(BinaryExpr{
                    .op = op,
                    .lhs = std::move(lhs),
                    .rhs = parse_multiplicative(parser),
                    .location = location,
                });
            }

            return lhs;
        }

        auto parse_shift(Parser &parser) -> Expr {
            auto lhs = parse_additive(parser);

            while (parser.check(TokenKind::ShiftLeft) || parser.check(TokenKind::ShiftRight)) {
                const auto op = parser.current().kind == TokenKind::ShiftLeft ? BinaryOp::ShiftLeft : BinaryOp::ShiftRight;
                const auto location = parser.current_location();

                parser.advance();

                lhs = make_expr(BinaryExpr{
                    .op = op,
                    .lhs = std::move(lhs),
                    .rhs = parse_additive(parser),
                    .location = location,
                });
            }

            return lhs;
        }

        auto parse_comparison(Parser &parser) -> Expr {
            auto lhs = parse_shift(parser);

            while (parser.check(TokenKind::Less) ||
                   parser.check(TokenKind::Greater) ||
                   parser.check(TokenKind::LessEqual) ||
                   parser.check(TokenKind::GreaterEqual)) {

                BinaryOp op;

                switch (parser.current().kind) {
                case TokenKind::Less:         op = BinaryOp::Less; break;
                case TokenKind::Greater:      op = BinaryOp::Greater; break;
                case TokenKind::LessEqual:    op = BinaryOp::LessEqual; break;
                case TokenKind::GreaterEqual: op = BinaryOp::GreaterEqual; break;
                default:                      __builtin_unreachable();
                }

                const auto location = parser.current_location();

                parser.advance();

                lhs = make_expr(BinaryExpr{
                    .op = op,
                    .lhs = std::move(lhs),
                    .rhs = parse_shift(parser),
                    .location = location,
                });
            }

            return lhs;
        }

        auto parse_equality(Parser &parser) -> Expr {
            auto lhs = parse_comparison(parser);

            while (parser.check(TokenKind::EqualEqual) || parser.check(TokenKind::BangEqual)) {
                const auto op = parser.current().kind == TokenKind::EqualEqual ? BinaryOp::Equal : BinaryOp::NotEqual;
                const auto location = parser.current_location();

                parser.advance();

                lhs = make_expr(BinaryExpr{
                    .op = op,
                    .lhs = std::move(lhs),
                    .rhs = parse_comparison(parser),
                    .location = location,
                });
            }

            return lhs;
        }

        auto parse_bitwise_and(Parser &parser) -> Expr {
            auto lhs = parse_equality(parser);

            while (parser.check(TokenKind::Ampersand) && !parser.check(TokenKind::AmpAmp)) {
                parser.advance();

                const auto location = parser.current_location();

                lhs = make_expr(BinaryExpr{
                    .op = BinaryOp::BitwiseAnd,
                    .lhs = std::move(lhs),
                    .rhs = parse_equality(parser),
                    .location = location,
                });
            }

            return lhs;
        }

        auto parse_bitwise_xor(Parser &parser) -> Expr {
            auto lhs = parse_bitwise_and(parser);

            while (parser.match(TokenKind::Caret)) {
                const auto location = parser.current_location();

                lhs = make_expr(BinaryExpr{
                    .op = BinaryOp::BitwiseXor,
                    .lhs = std::move(lhs),
                    .rhs = parse_bitwise_and(parser),
                    .location = location,
                });
            }

            return lhs;
        }

        auto parse_bitwise_or(Parser &parser) -> Expr {
            auto lhs = parse_bitwise_xor(parser);

            while (parser.check(TokenKind::Pipe) && !parser.check(TokenKind::PipePipe)) {
                parser.advance();

                const auto location = parser.current_location();

                return make_expr(BinaryExpr{
                    .op = BinaryOp::BitwiseOr,
                    .lhs = std::move(lhs),
                    .rhs = parse_bitwise_xor(parser),
                    .location = location,
                });
            }

            return lhs;
        }

        auto parse_logical_and(Parser &parser) -> Expr {
            auto lhs = parse_bitwise_or(parser);

            while (parser.match(TokenKind::AmpAmp)) {
                const auto location = parser.current_location();

                return make_expr(BinaryExpr{
                    .op = BinaryOp::LogicalAnd,
                    .lhs = std::move(lhs),
                    .rhs = parse_bitwise_or(parser),
                    .location = location,
                });
            }

            return lhs;
        }

        auto parse_logical_or(Parser &parser) -> Expr {
            auto lhs = parse_logical_and(parser);

            while (parser.match(TokenKind::PipePipe)) {
                const auto location = parser.current_location();

                return make_expr(BinaryExpr{
                    .op = BinaryOp::LogicalOr,
                    .lhs = std::move(lhs),
                    .rhs = parse_logical_and(parser),
                    .location = location,
                });
            }

            return lhs;
        }

        auto parse_ternary_expr(Parser &parser) -> Expr {
            auto expr = parse_logical_or(parser);

            if (parser.match(TokenKind::Question)) {
                const auto location = parser.current_location();

                auto then_expr = parse_expr(parser);
                parser.expect(TokenKind::Colon, "':'");
                auto else_expr = parse_expr(parser);

                return make_expr(TernaryExpr{
                    .condition = std::move(expr),
                    .then_expr = std::move(then_expr),
                    .else_expr = std::move(else_expr),
                    .location = location,
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

            if (const auto op = MatchAssignOp(parser.current().kind)) {
                const auto location = parser.current_location();

                parser.advance();

                return make_expr(AssignExpr{
                    .op = *op,
                    .target = std::move(expr),
                    .value = parse_assign_expr(parser),
                    .location = location,
                });
            }

            return expr;
        }

        auto parse_import_expr(Parser &parser) -> Expr {
            const auto location = parser.current_location();

            parser.expect(TokenKind::KwImport, "'import'");
            parser.expect(TokenKind::LParen, "'('");

            const auto module_name = parser.expect(TokenKind::StringLiteral, "string literal").lexeme;

            parser.expect(TokenKind::RParen, "')'");

            return ImportExpr{
                .module_name = module_name,
                .location = location,
            };
        }

        auto parse_block_stmt(Parser &parser) -> Stmt {
            const auto location = parser.current_location();

            parser.expect(TokenKind::LBrace, "'{'");

            std::vector<Stmt> stmts;
            while (!parser.check(TokenKind::RBrace)) {
                stmts.push_back(parse_stmt(parser));
            }

            parser.expect(TokenKind::RBrace, "'}'");

            return std::make_unique<BlockStmt>(BlockStmt{
                .stmts = std::move(stmts),
                .location = location,
            });
        }

        auto parse_expr_stmt(Parser &parser) -> Stmt {
            const auto location = parser.current_location();

            return ExprStmt{
                .expr = parse_expr(parser),
                .location = location,
            };
        }

        auto parse_var_decl_stmt(Parser &parser) -> VarDeclStmt {
            const auto location = parser.current_location();

            const auto is_mut = parser.match(TokenKind::KwMut);
            if (!is_mut) {
                parser.expect(TokenKind::KwConst, "'const' or 'mut'");
            }

            const auto var_name = parser.expect_identifier();

            std::optional<Type> type = std::nullopt;
            if (parser.match(TokenKind::Colon)) {
                type = parse_type(parser);
            }

            std::optional<Expr> init_expr = std::nullopt;
            if (type.has_value()) {
                if (parser.match(TokenKind::Equal)) {
                    init_expr = parse_expr(parser, false);
                }
            } else {
                parser.expect(TokenKind::ColonEqual, "':' or ':='");
                init_expr = parse_expr(parser, !is_mut);
            }

            if (!is_mut && init_expr == std::nullopt) {
                parser.report_error(parser.current_location(), "'const' requires an initializer");
            }

            return VarDeclStmt{
                .is_mut = is_mut,
                .name = var_name,
                .type = std::move(type),
                .init = std::move(init_expr),
                .location = location,
            };
        }

        auto parse_if_stmt(Parser &parser) -> std::unique_ptr<IfStmt> {
            const auto location = parser.current_location();

            parser.expect(TokenKind::KwIf, "'if'");

            auto condition = parse_expr(parser);
            auto then_stmt = parse_stmt(parser);

            std::optional<Stmt> else_stmt = std::nullopt;
            if (parser.match(TokenKind::KwElse)) {
                else_stmt = parse_stmt(parser);
            }

            return std::make_unique<IfStmt>(IfStmt{
                .condition = std::move(condition),
                .then_stmt = std::move(then_stmt),
                .else_stmt = std::move(else_stmt),
                .location = location,
            });
        }

        auto parse_while_stmt(Parser &parser) -> std::unique_ptr<WhileStmt> {
            const auto location = parser.current_location();

            parser.expect(TokenKind::KwWhile, "'while'");

            auto condition = parse_expr(parser);
            auto body = parse_block_stmt(parser);

            return std::make_unique<WhileStmt>(WhileStmt{
                .condition = std::move(condition),
                .body = std::move(body),
                .location = location,
            });
        }

        auto parse_continue_stmt(Parser &parser) -> ContinueStmt {
            const auto location = parser.current_location();

            parser.expect(TokenKind::KwContinue, "'continue'");

            return ContinueStmt{
                .location = location,
            };
        }

        auto parse_return_stmt(Parser &parser) -> ReturnStmt {
            const auto location = parser.current_location();

            parser.expect(TokenKind::KwReturn, "'return'");

            std::vector<Expr> values;
            if (can_start_expr(parser.current().kind)) {
                values.push_back(parse_expr(parser));

                while (parser.match(TokenKind::Comma)) {
                    values.push_back(parse_expr(parser));
                }
            }

            return ReturnStmt{
                .return_values = std::move(values),
                .location = location,
            };
        }

        auto parse_function_params(Parser &parser) -> std::vector<FunctionDecl::Param> {
            parser.expect(TokenKind::LParen, "'('");

            std::vector<FunctionDecl::Param> params;

            while (!parser.check(TokenKind::RParen) && !parser.at_end()) {
                const auto param_location = parser.current_location();
                const auto param_is_mutable = parser.match(TokenKind::KwMut);
                const auto param_name = parser.expect(TokenKind::Identifier, "parameter name");

                parser.expect(TokenKind::Colon, "':'");

                params.push_back({
                    .is_mut = param_is_mutable,
                    .name = param_name.lexeme,
                    .type = parse_type(parser),
                    .location = param_location,
                });

                if (!parser.check(TokenKind::RParen)) {
                    parser.expect(TokenKind::Comma, "','");
                }
            }

            parser.expect(TokenKind::RParen, "')'");

            return params;
        }

        auto parse_function_return_types(Parser &parser) -> std::vector<Type> {
            std::vector<Type> return_types;

            if (parser.match(TokenKind::Arrow)) {
                return_types.push_back(parse_type(parser));

                while (parser.match(TokenKind::Comma)) {
                    return_types.push_back(parse_type(parser));
                }
            }

            return return_types;
        }

        auto parse_function_decl(Parser &parser, const bool is_pub) -> FunctionDecl {
            const auto location = parser.current_location();

            parser.expect(TokenKind::KwFn, "'fn'");

            auto fn_name = parser.expect(TokenKind::Identifier, "function name").lexeme;
            auto fn_params = parse_function_params(parser);
            auto fn_return_types = parse_function_return_types(parser);
            auto fn_body = parse_stmt(parser);

            return FunctionDecl{
                .is_pub = is_pub,
                .name = std::move(fn_name),
                .params = std::move(fn_params),
                .return_types = std::move(fn_return_types),
                .body = std::move(fn_body),
                .location = location,
            };
        }

        auto parse_ext_function_params(Parser &parser) -> std::vector<ExtFunctionDecl::Param> {
            parser.expect(TokenKind::LParen, "'('");

            std::vector<ExtFunctionDecl::Param> params;

            while (!parser.check(TokenKind::RParen) && !parser.at_end()) {
                const auto param_location = parser.current_location();
                const auto param_name = parser.expect(TokenKind::Identifier, "parameter name");

                parser.expect(TokenKind::Colon, "':'");

                params.push_back({
                    .name = param_name.lexeme,
                    .type = parse_type(parser),
                    .location = param_location,
                });

                if (!parser.check(TokenKind::RParen)) {
                    parser.expect(TokenKind::Comma, "','");
                }
            }

            parser.expect(TokenKind::RParen, "')'");

            return params;
        }

        auto parse_ext_function_return_type(Parser &parser) -> std::optional<Type> {
            if (parser.match(TokenKind::Arrow)) {
                return parse_type(parser);
            }

            return std::nullopt;
        }

        auto parse_ext_function_decl(Parser &parser, const bool is_pub) -> ExtFunctionDecl {
            const auto location = parser.current_location();

            parser.expect(TokenKind::KwFn, "'fn'");

            const auto fn_name = parser.expect(TokenKind::Identifier, "identifier").lexeme;
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
            const auto location = parser.current_location();

            const auto is_mut = parser.match(TokenKind::KwMut);
            if (!is_mut) {
                parser.expect(TokenKind::KwConst, "'const' or 'mut'");
            }

            const auto var_name = parser.expect(TokenKind::Identifier, "identifier").lexeme;

            std::optional<Type> type = std::nullopt;
            if (parser.match(TokenKind::Colon)) {
                type = parse_type(parser);
            }

            std::optional<Expr> init_expr = std::nullopt;
            if (type.has_value()) {
                if (parser.match(TokenKind::Equal)) {
                    init_expr = parse_expr(parser, false);
                }
            } else {
                parser.expect(TokenKind::ColonEqual, "':' or ':='");
                init_expr = parse_expr(parser, !is_mut);
            }

            if (!is_mut && init_expr == std::nullopt) {
                parser.report_error(parser.current_location(), "'const' requires an initializer");
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

        auto parse_macro_params(Parser &parser) -> std::vector<MacroDecl::Param> {
            std::vector<MacroDecl::Param> params;

            parser.expect(TokenKind::LParen, "'('");

            while (!parser.check(TokenKind::RParen) && !parser.at_end()) {
                const auto param_location = parser.current_location();
                const auto param_name = parser.expect(TokenKind::Identifier, "identifier").lexeme;

                parser.expect(TokenKind::Colon, "':'");

                params.push_back({
                    .name = param_name,
                    .type = parse_type(parser),
                    .location = param_location,
                });

                if (!parser.check(TokenKind::RParen)) {
                    parser.expect(TokenKind::Comma, "','");
                }
            }

            parser.expect(TokenKind::RParen, "')'");

            return params;
        }

        auto parse_macro_decl(Parser &parser, const bool is_pub) -> MacroDecl {
            const auto location = parser.current_location();

            parser.expect(TokenKind::KwMacro, "'macro'");

            const auto m_name = parser.expect(TokenKind::Identifier, "identifier").lexeme;
            auto m_params = parse_macro_params(parser);

            parser.expect(TokenKind::Arrow, "'->'");

            return MacroDecl{
                .is_pub = is_pub,
                .name = m_name,
                .params = std::move(m_params),
                .expr_template = parse_expr(parser),
                .location = location,
            };
        }

        auto parse_type_decl(Parser &parser, const bool is_pub) -> TypeDecl {
            const auto location = parser.current_location();

            parser.expect(TokenKind::KwType, "'type'");

            const auto type_name = parser.expect_identifier();

            parser.expect(TokenKind::Equal, "'='");

            return TypeDecl{
                .is_pub = is_pub,
                .name = type_name,
                .type = parse_type(parser),
                .location = location,
            };
        }
    }

    auto parse_type(Parser &parser) -> Type {
        const auto location = parser.current_location();

        if (parser.match(TokenKind::Star)) {
            return std::make_unique<PointerType>(PointerType{
                .pointee = parse_type(parser),
                .location = location,
            });
        }

        if (parser.check(TokenKind::KwStruct)) {
            return parse_struct_type(parser);
        }

        if (parser.check(TokenKind::Identifier)) {
            return parse_named_type(parser);
        }

        if (const auto kind = parse_builtin_type_kind(parser); kind.has_value()) {
            return BuiltinType{
                .kind = kind.value(),
                .location = location,
            };
        }

        parser.report_error(location, std::format("expected type, got '{}'", parser.current_lexeme()));

        return std::monostate();
    }

    auto parse_expr(Parser &parser, const bool allow_import) -> Expr {
        if (parser.check(TokenKind::KwImport)) {
            if (allow_import) {
                return parse_import_expr(parser);
            }

            parser.report_error(parser.current_location(), "'import()' can only be used to initialize a 'const' declaration with no explicit type");

            return parse_import_expr(parser);
        }

        return parse_assign_expr(parser);
    }

    auto parse_stmt(Parser &parser) -> Stmt {
        if (parser.check(TokenKind::LBrace)) {
            return parse_block_stmt(parser);
        }

        if (parser.check(TokenKind::KwMut) || parser.check(TokenKind::KwConst)) {
            return parse_var_decl_stmt(parser);
        }

        if (parser.check(TokenKind::KwIf)) {
            return parse_if_stmt(parser);
        }

        if (parser.check(TokenKind::KwWhile)) {
            return parse_while_stmt(parser);
        }

        if (parser.check(TokenKind::KwContinue)) {
            return parse_continue_stmt(parser);
        }

        if (parser.check(TokenKind::KwReturn)) {
            return parse_return_stmt(parser);
        }

        return parse_expr_stmt(parser);
    }

    auto parse_decl(Parser &parser, const bool top_level) -> std::optional<Decl> {
        const auto is_pub = !top_level || parser.match(TokenKind::KwPub);

        if (parser.check(TokenKind::Identifier) && parser.current_lexeme() == "ext") {
            parser.advance();

            return parse_ext_function_decl(parser, is_pub);
        }

        if (parser.check(TokenKind::KwFn)) {
            return parse_function_decl(parser, is_pub);
        }

        if (parser.check(TokenKind::KwType)) {
            return parse_type_decl(parser, is_pub);
        }

        if (parser.check(TokenKind::KwMut) || parser.check(TokenKind::KwConst)) {
            return parse_var_decl(parser, is_pub);
        }

        if (parser.check(TokenKind::KwMacro)) {
            return parse_macro_decl(parser, is_pub);
        }

        parser.report_error(
            parser.current_location(),
            std::format(
                "expected declaration, got '{}'",
                parser.current_lexeme()));

        parser.advance();

        return std::nullopt;
    }
}
