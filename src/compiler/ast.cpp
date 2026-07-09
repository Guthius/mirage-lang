#include "ast.hpp"

#include <format>
#include <sstream>

namespace ast {
    namespace {
        auto parse_named_type(Parser &parser) -> NamedType {
            const auto location = parser.current_location();
            const auto name = parser.expect_identifier();

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

            auto is_packed = false;

            parser.expect(TokenKind::KwStruct, "'struct'");
            if (parser.match(TokenKind::LParen)) {
                is_packed = parser.match_identifier("packed");

                parser.expect(TokenKind::RParen, "')'");
            }

            parser.expect(TokenKind::LBrace, "'{'");

            std::vector<StructType::Field> fields;
            while (!parser.check(TokenKind::RBrace) && !parser.at_end()) {
                const auto field_location = parser.current_location();
                const auto field_name = parser.expect_identifier();

                parser.expect(TokenKind::Colon, "':'");

                auto field_type = parse_type(parser);

                std::optional<Expr> init;
                if (parser.match(TokenKind::Equal)) {
                    init = parse_expr(parser);
                }

                fields.push_back({
                    .name = field_name,
                    .type = std::move(field_type),
                    .init = std::move(init),
                    .location = field_location,
                });
            }

            parser.expect(TokenKind::RBrace, "'}'");

            return std::make_unique<StructType>(StructType{
                .is_packed = is_packed,
                .fields = std::move(fields),
                .location = location,
            });
        }

        auto parse_enum_field(Parser &parser) -> EnumType::Field {
            const auto location = parser.current_location();

            auto name = parser.expect_identifier();

            std::optional<Expr> init;
            if (parser.match(TokenKind::Equal)) {
                init = parse_expr(parser);
            }

            return EnumType::Field{
                .name = std::move(name),
                .init = std::move(init),
                .location = location,
            };
        }

        auto parse_union_type(Parser &parser) -> Type {
            const auto location = parser.current_location();

            parser.expect(TokenKind::KwUnion, "'union'");

            bool is_tagged = false;
            if (parser.match(TokenKind::LParen)) {
                // tagged union: union(enum) { ... }
                parser.expect(TokenKind::KwEnum, "'enum'");
                parser.expect(TokenKind::RParen, "')'");
                is_tagged = true;
            }

            parser.expect(TokenKind::LBrace, "'{'");

            std::vector<UnionType::Member> members;
            while (!parser.check(TokenKind::RBrace) && !parser.at_end()) {
                const auto member_location = parser.current_location();
                const auto member_name = parser.expect_identifier();

                if (is_tagged) {
                    // Tagged variant: optional `: type` payload; no default initializers
                    if (parser.match(TokenKind::Colon)) {
                        auto member_type = parse_type(parser);
                        members.push_back({
                            .name = member_name,
                            .type = std::move(member_type),
                            .location = member_location,
                        });
                    } else {
                        // Payload-free variant (monostate type)
                        members.push_back({
                            .name = member_name,
                            .type = std::monostate{},
                            .location = member_location,
                        });
                    }
                } else {
                    parser.expect(TokenKind::Colon, "':'");

                    auto member_type = parse_type(parser);

                    if (parser.match(TokenKind::Equal)) {
                        parser.report_error(member_location, "union member default initializers are not allowed");
                        parse_expr(parser); // consume and discard
                    }

                    members.push_back({
                        .name = member_name,
                        .type = std::move(member_type),
                        .location = member_location,
                    });
                }
            }

            parser.expect(TokenKind::RBrace, "'}'");

            return std::make_unique<UnionType>(UnionType{
                .is_tagged = is_tagged,
                .members = std::move(members),
                .location = location,
            });
        }

        auto parse_enum_type(Parser &parser) -> Type {
            const auto location = parser.current_location();

            parser.expect(TokenKind::KwEnum, "'enum'");

            std::optional<Type> underlying_type;
            if (parser.match(TokenKind::LParen)) {
                underlying_type = parse_type(parser);

                parser.expect(TokenKind::RParen, "')'");
            }

            parser.expect(TokenKind::LBrace, "'{'");

            std::vector<EnumType::Field> fields;
            while (!parser.check(TokenKind::RBrace) && !parser.at_end()) {
                fields.push_back(parse_enum_field(parser));
            }

            parser.expect(TokenKind::RBrace, "'}'");

            return std::make_unique<EnumType>(EnumType{
                .underlying_type = std::move(underlying_type),
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
            case TokenKind::KwUndefined:
            case TokenKind::KwMatch:
            case TokenKind::KwTry:
            case TokenKind::KwIota:
            case TokenKind::Dot:
            case TokenKind::LBrace:
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

            std::optional<Expr> len_expr = std::nullopt;
            if (parser.match(TokenKind::Comma)) {
                len_expr = parse_expr(parser);
            }

            parser.expect(TokenKind::RParen, "')'");

            return std::make_unique<CastExpr>(CastExpr{
                .value = std::move(expr),
                .as_type = std::move(as_type),
                .len_expr = std::move(len_expr),
                .location = location,
            });
        }

        // Decodes a single escape sequence. `str` must point just past the
        // backslash on entry (str[0] is the escape designator) and is left
        // positioned just past the whole escape sequence on return.
        auto decode_escape_sequence(Parser &parser, const SourceLocation location, std::string_view &str,
                                     const std::string_view kind) -> uint8_t {
            const auto HexDigitValue = [](const char ch) -> uint8_t {
                if (ch >= '0' && ch <= '9') {
                    return static_cast<uint8_t>(ch - '0');
                }

                return static_cast<uint8_t>(10 + (std::tolower(ch) - 'a'));
            };

            const char c = str[0];

            switch (c) {
            case '\\': str.remove_prefix(1); return '\\';
            case '"':  str.remove_prefix(1); return '"';
            case '\'': str.remove_prefix(1); return '\'';
            case 'n':  str.remove_prefix(1); return '\n';
            case 't':  str.remove_prefix(1); return '\t';
            case 'r':  str.remove_prefix(1); return '\r';
            case 'x': {
                str.remove_prefix(1);

                if (str.size() < 2 || !std::isxdigit(static_cast<unsigned char>(str[0])) ||
                    !std::isxdigit(static_cast<unsigned char>(str[1]))) {
                    parser.report_error(location,
                                         std::format("hex escape sequence requires exactly 2 hex digits in {} literal", kind));
                    return 0;
                }

                const uint8_t value = static_cast<uint8_t>(HexDigitValue(str[0]) * 16 + HexDigitValue(str[1]));
                str.remove_prefix(2);
                return value;
            }
            default:
                if (c >= '0' && c <= '7') {
                    unsigned value = 0;
                    int count = 0;

                    while (!str.empty() && count < 3 && str[0] >= '0' && str[0] <= '7') {
                        value = value * 8 + static_cast<unsigned>(str[0] - '0');
                        str.remove_prefix(1);
                        ++count;
                    }

                    if (value > 0xFF) {
                        parser.report_error(location, std::format("octal escape sequence out of range in {} literal", kind));
                        value &= 0xFF;
                    }

                    return static_cast<uint8_t>(value);
                }

                parser.report_error(location, std::format("unknown escape sequence '\\{}' in {} literal", c, kind));
                str.remove_prefix(1);
                return 0;
            }
        }

        auto parse_string_literal(Parser &parser) -> LiteralStringExpr {
            const auto location = parser.current_location();

            auto &token = parser.expect(TokenKind::StringLiteral, "string literal");

            if (token.lexeme.size() < 2) {
                parser.report_error(location, "malformed string literal");

                return LiteralStringExpr{
                    .value = {},
                    .location = location,
                };
            }

            std::ostringstream oss;

            std::string_view str = token.lexeme;

            str.remove_prefix(1);
            str.remove_suffix(1);

            while (!str.empty()) {
                if (str[0] == '\\') {
                    str.remove_prefix(1);
                    if (str.empty()) {
                        break;
                    }

                    oss << static_cast<char>(decode_escape_sequence(parser, location, str, "string"));
                } else {
                    oss << str[0];
                    str.remove_prefix(1);
                }
            }

            return LiteralStringExpr{
                .value = oss.str(),
                .location = location,
            };
        }

        auto parse_char_literal(Parser &parser) -> LiteralCharExpr {
            const auto location = parser.current_location();
            auto &token = parser.expect(TokenKind::CharLiteral, "character literal");

            std::string_view str = token.lexeme;
            if (str.size() < 3) {
                parser.report_error(location, "malformed character literal");
                return LiteralCharExpr{.value = 0, .location = location};
            }
            str.remove_prefix(1);
            str.remove_suffix(1);

            uint8_t val = 0;
            if (str[0] == '\\' && str.size() > 1) {
                str.remove_prefix(1);
                val = decode_escape_sequence(parser, location, str, "character");
            } else if (!str.empty()) {
                val = static_cast<uint8_t>(str[0]);
            }

            return LiteralCharExpr{.value = val, .location = location};
        }

        auto parse_braced_initializer(Parser &parser) -> std::unique_ptr<BracedInitializerExpr> {
            const auto location = parser.current_location();

            parser.expect(TokenKind::LBrace, "'{'");
            if (parser.match(TokenKind::RBrace)) {
                return std::make_unique<BracedInitializerExpr>(EmptyExpr{
                    .location = location,
                });
            }

            if (parser.check(TokenKind::Dot) && parser.peek().kind == TokenKind::Identifier) {
                std::vector<StructExpr::Field> fields;
                while (!parser.check(TokenKind::RBrace) && !parser.at_end()) {
                    parser.expect(TokenKind::Dot, "'.'");
                    const auto value_name = parser.expect_identifier();

                    parser.expect(TokenKind::Equal, "'='");

                    const auto value_location = parser.current_location();

                    fields.push_back(StructExpr::Field{
                        .name = value_name,
                        .expr = parse_expr(parser),
                        .location = value_location,
                    });

                    if (parser.check(TokenKind::RBrace)) {
                        break;
                    }

                    parser.expect(TokenKind::Comma, "','");
                }

                parser.expect(TokenKind::RBrace, "'}'");

                return std::make_unique<BracedInitializerExpr>(StructExpr{
                    .fields = std::move(fields),
                    .location = location,
                });
            }

            std::vector<Expr> values;
            bool has_fill = false;
            while (!parser.check(TokenKind::RBrace) && !parser.at_end()) {
                values.push_back(parse_expr(parser));
                if (parser.match(TokenKind::DotDotDot)) {
                    has_fill = true;
                    break;
                }
                if (!parser.match(TokenKind::Comma)) {
                    break;
                }
            }

            parser.expect(TokenKind::RBrace, "'}'");

            return std::make_unique<BracedInitializerExpr>(ArrayExpr{
                .values = std::move(values),
                .has_fill = has_fill,
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
                return parse_string_literal(parser);
            }

            if (parser.check(TokenKind::CharLiteral)) {
                return parse_char_literal(parser);
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
                parser.expect(TokenKind::LParen, "'('");
                auto operand = parse_expr(parser);
                parser.expect(TokenKind::RParen, "')'");

                return std::make_unique<SizeOfExpr>(SizeOfExpr{
                    .operand = std::move(operand),
                    .location = location,
                });
            }

            if (parser.check(TokenKind::KwLen)) {
                parser.advance();
                parser.expect(TokenKind::LParen, "'('");
                auto operand = parse_expr(parser);
                parser.expect(TokenKind::RParen, "')'");

                return std::make_unique<LenExpr>(LenExpr{
                    .operand = std::move(operand),
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

            if (parser.check(TokenKind::KwIota)) {
                parser.advance();

                return IotaExpr{
                    .location = location,
                };
            }

            if (parser.check(TokenKind::KwDefault)) {
                parser.advance();
                return DefaultExpr{.location = location};
            }

            if (parser.check(TokenKind::KwUndefined)) {
                parser.advance();
                return UndefinedExpr{.location = location};
            }

            if (parser.check(TokenKind::LBrace)) {
                return parse_braced_initializer(parser);
            }

            if (parser.check(TokenKind::Dot)) {
                parser.advance();
                const auto name = parser.expect_identifier();

                // If followed by '{.' this is a contextual tagged variant: .variant{.field = val}
                // The leading '.' inside the braces disambiguates payload from block statements.
                if (parser.check(TokenKind::LBrace) && parser.peek().kind == TokenKind::Dot) {
                    const auto brace_loc = parser.current_location();
                    parser.advance(); // consume '{'
                    std::vector<StructExpr::Field> fields;
                    while (!parser.check(TokenKind::RBrace) && !parser.at_end()) {
                        parser.expect(TokenKind::Dot, "'.'");
                        const auto field_name = parser.expect_identifier();
                        parser.expect(TokenKind::Equal, "'='");
                        const auto field_loc = parser.current_location();
                        fields.push_back(StructExpr::Field{
                            .name = field_name,
                            .expr = parse_expr(parser),
                            .location = field_loc,
                        });
                        if (parser.check(TokenKind::RBrace)) break;
                        parser.expect(TokenKind::Comma, "','");
                    }
                    parser.expect(TokenKind::RBrace, "'}'");
                    return std::make_unique<TaggedVariantExpr>(TaggedVariantExpr{
                        .type_name = "",
                        .variant_name = name,
                        .payload = StructExpr{.fields = std::move(fields), .location = brace_loc},
                        .location = location,
                    });
                }

                return DotIdentExpr{
                    .name = name,
                    .location = location,
                };
            }

            if (parser.check(TokenKind::KwMatch)) {
                parser.advance();

                auto operand = parse_expr(parser);
                parser.expect(TokenKind::LBrace, "'{'");

                std::vector<MatchExpr::Arm> arms;
                while (!parser.check(TokenKind::RBrace) && !parser.at_end()) {
                    const auto arm_location = parser.current_location();

                    // Parse arm pattern
                    auto pattern = [&]() -> MatchExpr::ArmPattern {
                        if (parser.check(TokenKind::Dot)) {
                            // VariantPattern: .name or .name(capture) or .name(&capture)
                            parser.advance();
                            auto vname = parser.expect_identifier();
                            std::optional<std::string> capture_name;
                            bool capture_by_ref = false;
                            if (parser.match(TokenKind::LParen)) {
                                if (parser.match(TokenKind::Ampersand)) {
                                    capture_by_ref = true;
                                }
                                capture_name = parser.expect_identifier();
                                parser.expect(TokenKind::RParen, "')'");
                            }
                            return MatchExpr::VariantPattern{
                                .name = std::move(vname),
                                .capture_name = std::move(capture_name),
                                .capture_by_ref = capture_by_ref,
                            };
                        }
                        if (parser.check(TokenKind::Identifier) && parser.current_lexeme() == "_") {
                            // DefaultPattern: _
                            parser.advance();
                            return MatchExpr::DefaultPattern{};
                        }
                        // LiteralPattern: constant expression
                        auto lp_expr = std::make_unique<Expr>(parse_expr(parser));
                        return MatchExpr::LiteralPattern{std::move(lp_expr)};
                    }();

                    parser.expect(TokenKind::Colon, "':'");
                    auto arm_value = parse_expr(parser);

                    arms.push_back(MatchExpr::Arm{
                        .pattern = std::move(pattern),
                        .value = std::move(arm_value),
                        .location = arm_location,
                    });

                    if (!parser.check(TokenKind::RBrace)) {
                        parser.expect(TokenKind::Comma, "','");
                    }
                }

                parser.expect(TokenKind::RBrace, "'}'");

                return make_expr(MatchExpr{
                    .operand = std::move(operand),
                    .arms = std::move(arms),
                    .location = location,
                });
            }

            parser.report_error(location, std::format("expected expression, got '{}'", parser.current_lexeme()));
            parser.advance();

            return LiteralIntegerExpr{
                .value = 0,
                .location = location,
            };
        }

        auto parse_index_or_slice_expr(Expr operand, Parser &parser) -> Expr {
            const auto location = parser.current_location();

            parser.expect(TokenKind::LBracket, "'['");

            auto index = parse_expr(parser);
            if (parser.match(TokenKind::DotDot)) {
                auto end = parse_expr(parser);
                parser.expect(TokenKind::RBracket, "']'");

                return make_expr(SliceExpr{
                    .operand = std::move(operand),
                    .start = std::move(index),
                    .end = std::move(end),
                    .location = location,
                });
            }

            parser.expect(TokenKind::RBracket, "']'");

            return make_expr(IndexExpr{
                .operand = std::move(operand),
                .index = std::move(index),
                .location = location,
            });
        }

        auto parse_postfix(Parser &parser) -> Expr {
            auto expr = parse_primary(parser);

            const auto location = parser.current_location();

            while (true) {
                if (parser.check(TokenKind::LParen)) {
                    parser.advance();

                    std::vector<Expr> args;
                    while (!parser.check(TokenKind::RParen) && !parser.at_end()) {
                        // '...' here is call-argument spread (position 5, see parse_function_params'
                        // comment for the full list of '...' positions) — forwards an existing slice
                        // as a variadic argument, written as a postfix suffix: 'expr...'. Legality
                        // (sole/last/variadic-callee) is sema's job.
                        auto arg = parse_expr(parser);
                        if (parser.check(TokenKind::DotDotDot)) {
                            const auto spread_loc = parser.current_location();
                            parser.advance();
                            args.push_back(std::make_unique<SpreadExpr>(SpreadExpr{
                                .operand = std::move(arg),
                                .location = spread_loc,
                            }));
                        } else {
                            args.push_back(std::move(arg));
                        }
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
                    const auto member_name = parser.expect_identifier();

                    // If followed by '{.' and base is an IdentExpr, parse as qualified tagged variant.
                    // The leading '.' inside braces disambiguates payload from block statements.
                    if (parser.check(TokenKind::LBrace) && parser.peek().kind == TokenKind::Dot) {
                        if (const auto *base_ident = std::get_if<IdentExpr>(&expr)) {
                            const auto brace_loc = parser.current_location();
                            parser.advance(); // consume '{'
                            std::vector<StructExpr::Field> fields;
                            while (!parser.check(TokenKind::RBrace) && !parser.at_end()) {
                                parser.expect(TokenKind::Dot, "'.'");
                                const auto field_name = parser.expect_identifier();
                                parser.expect(TokenKind::Equal, "'='");
                                const auto field_loc = parser.current_location();
                                fields.push_back(StructExpr::Field{
                                    .name = field_name,
                                    .expr = parse_expr(parser),
                                    .location = field_loc,
                                });
                                if (parser.check(TokenKind::RBrace)) break;
                                parser.expect(TokenKind::Comma, "','");
                            }
                            parser.expect(TokenKind::RBrace, "'}'");
                            expr = std::make_unique<TaggedVariantExpr>(TaggedVariantExpr{
                                .type_name = base_ident->name,
                                .variant_name = member_name,
                                .payload = StructExpr{.fields = std::move(fields), .location = brace_loc},
                                .location = location,
                            });
                            continue;
                        }
                    }

                    expr = make_expr(MemberExpr{
                        .object = std::move(expr),
                        .member = member_name,
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

                } else if (parser.check(TokenKind::LBracket)) {
                    expr = parse_index_or_slice_expr(std::move(expr), parser);

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
                case TokenKind::Star:      return UnaryOp::Deref;
                default:                   return std::nullopt;
                }
            };

            if (parser.check(TokenKind::KwTry)) {
                const auto location = parser.current_location();
                parser.advance();
                // `try` binds tighter than binary ops: `try f(x) + g()` → `(try f(x)) + g()`.
                // For chained access after try, write `(try f(x)).field`.
                return make_expr(TryExpr{
                    .call = parse_postfix(parser),
                    .location = location,
                });
            }

            if (parser.check(TokenKind::PlusPlus) || parser.check(TokenKind::MinusMinus)) {
                const bool is_increment = parser.check(TokenKind::PlusPlus);
                const auto location = parser.current_location();
                parser.advance();

                return make_expr(IncrDecrExpr{
                    .operand = parse_unary(parser),
                    .is_increment = is_increment,
                    .is_prefix = true,
                    .location = location,
                });
            }

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

                lhs = make_expr(BinaryExpr{
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

                lhs = make_expr(BinaryExpr{
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

                lhs = make_expr(BinaryExpr{
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

            const auto module_name = parse_string_literal(parser);

            parser.expect(TokenKind::RParen, "')'");

            return ImportExpr{
                .module_name = module_name.value,
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

        auto parse_var_decl_group_stmt(Parser &parser, const bool is_mut, SourceLocation location, std::string first_name) -> Stmt {
            std::vector<std::string> names;

            names.push_back(std::move(first_name));
            while (parser.match(TokenKind::Comma) && !parser.at_end()) {
                if (parser.check(TokenKind::ColonEqual)) {
                    names.emplace_back();
                    break;
                }
                if (parser.check(TokenKind::Comma)) {
                    names.emplace_back();
                    continue;
                }
                names.push_back(parser.expect_identifier());
            }

            parser.expect(TokenKind::ColonEqual, "':='");

            return VarDeclGroupStmt{
                .is_mut = is_mut,
                .names = std::move(names),
                .init = parse_expr(parser),
                .location = location,
            };
        }

        auto parse_var_decl_stmt(Parser &parser) -> Stmt {
            const auto location = parser.current_location();

            const auto is_mut = parser.match(TokenKind::KwMut);
            if (!is_mut) {
                parser.expect(TokenKind::KwConst, "'const' or 'mut'");
            }

            const auto var_name = parser.expect_identifier();
            if (parser.check(TokenKind::Comma)) {
                return parse_var_decl_group_stmt(parser, is_mut, location, var_name);
            }

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

        auto parse_for_in_stmt(Parser &parser) -> std::unique_ptr<ForInStmt> {
            const auto location = parser.current_location();

            parser.expect(TokenKind::KwFor, "'for'");

            std::string index_name = "_";
            std::string element_name;
            bool element_by_ref = false;

            if (parser.check(TokenKind::Ampersand)) {
                // for &val in ...
                parser.advance();
                element_by_ref = true;
                element_name = std::string{parser.expect(TokenKind::Identifier, "element variable name").lexeme};
            } else {
                std::string first{parser.expect(TokenKind::Identifier, "variable name").lexeme};
                if (parser.match(TokenKind::Comma)) {
                    // for idx, [&]val in ...
                    index_name = std::move(first);
                    if (parser.check(TokenKind::Ampersand)) {
                        parser.advance();
                        element_by_ref = true;
                    }
                    element_name = std::string{parser.expect(TokenKind::Identifier, "element variable name").lexeme};
                } else {
                    // for val in ...
                    element_name = std::move(first);
                }
            }

            parser.expect(TokenKind::KwIn, "'in'");

            auto iterable = [&]() -> ast::Expr {
                const auto loc = parser.current_location();
                if (parser.match(TokenKind::DotDot)) {
                    auto upper = parse_expr(parser);
                    return make_expr(ast::RangeExpr{.lower = std::nullopt, .upper = std::move(upper), .location = loc});
                }
                auto expr = parse_expr(parser);
                if (parser.match(TokenKind::DotDot)) {
                    auto upper = parse_expr(parser);
                    return make_expr(ast::RangeExpr{.lower = std::move(expr), .upper = std::move(upper), .location = loc});
                }
                return expr;
            }();
            auto body = parse_block_stmt(parser);

            return std::make_unique<ForInStmt>(ForInStmt{
                .index_name = std::move(index_name),
                .element_name = std::move(element_name),
                .element_by_ref = element_by_ref,
                .iterable = std::move(iterable),
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

        auto parse_break_stmt(Parser &parser) -> BreakStmt {
            const auto location = parser.current_location();

            parser.expect(TokenKind::KwBreak, "'break'");

            return BreakStmt{
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

        // The token '...' appears in four unrelated grammar positions, disambiguated purely by
        // parse context (never by a shared representation):
        //   1. Native variadic parameter: 'name: ...T' (here) — a type follows the dots; dissolves
        //      to '[]T' in sema. Only legal as the final parameter of a 'fn'.
        //   2. 'ext fn' C-varargs: a bare trailing '...' with no type (parse_ext_function_params).
        //   3. 'fn(...)' function-pointer-type C-varargs: a bare trailing '...' with no type
        //      (parse_function_type).
        //   4. Array-fill initializer: trailing '...' after the last element of '{ ... }' repeats it
        //      (braced-initializer parsing).
        //   5. Call-site spread: 'expr...' forwards an existing slice into a variadic parameter
        //      (parse_postfix's call-argument loop).
        auto parse_function_params(Parser &parser) -> std::vector<FunctionDecl::Param> {
            parser.expect(TokenKind::LParen, "'('");

            std::vector<FunctionDecl::Param> params;
            bool seen_variadic = false;

            while (!parser.check(TokenKind::RParen) && !parser.at_end()) {
                if (parser.check(TokenKind::DotDotDot)) {
                    parser.report_error(parser.current_location(),
                        "variadic parameters ('...') are only allowed on 'ext fn' declarations, not 'fn'; "
                        "to declare a native variadic parameter, use 'name: ...T' with an element type");
                    parser.advance();
                    break;
                }

                const auto param_location = parser.current_location();
                const auto param_is_mutable = parser.match(TokenKind::KwMut);
                const auto param_name = parser.expect_identifier();

                parser.expect(TokenKind::Colon, "':'");

                if (seen_variadic) {
                    parser.report_error(param_location, "'...' variadic parameter must be the last parameter in the parameter list");
                }

                bool param_is_variadic = false;
                if (parser.check(TokenKind::DotDotDot)) {
                    parser.advance();
                    param_is_variadic = true;
                    seen_variadic = true;
                }

                params.push_back({
                    .is_mut = param_is_mutable,
                    .name = param_name,
                    .type = parse_type(parser),
                    .is_variadic = param_is_variadic,
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
                if (parser.match(TokenKind::LParen)) {
                    // Multi-return: -> (T1, T2, ...)
                    while (!parser.check(TokenKind::RParen) && !parser.at_end()) {
                        return_types.push_back(parse_type(parser));
                        if (!parser.check(TokenKind::RParen)) {
                            parser.expect(TokenKind::Comma, "','");
                        }
                    }
                    parser.expect(TokenKind::RParen, "')'");
                } else {
                    // Single return: -> T
                    return_types.push_back(parse_type(parser));
                }
            }

            return return_types;
        }

        // fn(ParamType, ...) -> RetType  or  fn(ParamType, ...) -> (R1, R2)
        auto parse_function_type(Parser &parser) -> Type {
            const auto location = parser.current_location();

            parser.expect(TokenKind::KwFn, "'fn'");
            parser.expect(TokenKind::LParen, "'('");

            std::vector<Type> param_types;
            bool is_variadic = false;

            while (!parser.check(TokenKind::RParen) && !parser.at_end()) {
                if (parser.check(TokenKind::DotDotDot)) {
                    parser.advance();
                    is_variadic = true;
                    break;
                }
                param_types.push_back(parse_type(parser));
                if (!parser.check(TokenKind::RParen)) {
                    parser.expect(TokenKind::Comma, "','");
                }
            }
            parser.expect(TokenKind::RParen, "')'");

            std::vector<Type> return_types;
            if (parser.match(TokenKind::Arrow)) {
                if (parser.match(TokenKind::LParen)) {
                    // Multi-return: -> (T1, T2, ...)
                    while (!parser.check(TokenKind::RParen) && !parser.at_end()) {
                        return_types.push_back(parse_type(parser));
                        if (!parser.check(TokenKind::RParen)) {
                            parser.expect(TokenKind::Comma, "','");
                        }
                    }
                    parser.expect(TokenKind::RParen, "')'");
                } else {
                    // Single return: -> T
                    return_types.push_back(parse_type(parser));
                }
            }

            return std::make_unique<FunctionType>(FunctionType{
                .param_types = std::move(param_types),
                .return_types = std::move(return_types),
                .is_variadic = is_variadic,
                .location = location,
            });
        }

        auto parse_function_decl(Parser &parser, const bool is_pub) -> FunctionDecl {
            const auto location = parser.current_location();

            parser.expect(TokenKind::KwFn, "'fn'");

            auto fn_name = parser.expect_identifier();
            auto fn_params = parse_function_params(parser);
            auto fn_return_types = parse_function_return_types(parser);
            auto fn_body = parse_stmt(parser);

            return FunctionDecl{
                .is_pub = is_pub,
                .name = fn_name,
                .params = std::move(fn_params),
                .return_types = std::move(fn_return_types),
                .body = std::move(fn_body),
                .location = location,
            };
        }

        auto parse_ext_function_params(Parser &parser, bool &out_is_variadic) -> std::vector<ExtFunctionDecl::Param> {
            parser.expect(TokenKind::LParen, "'('");

            std::vector<ExtFunctionDecl::Param> params;
            out_is_variadic = false;

            while (!parser.check(TokenKind::RParen) && !parser.at_end()) {
                if (parser.check(TokenKind::DotDotDot)) {
                    if (params.empty()) {
                        parser.report_error(parser.current_location(),
                            "'...' requires at least one named parameter before it in 'ext fn'");
                    }
                    if (out_is_variadic) {
                        parser.report_error(parser.current_location(), "duplicate '...' in parameter list");
                    }
                    parser.advance();
                    out_is_variadic = true;
                    break;
                }

                const auto param_location = parser.current_location();
                const auto param_name = parser.expect_identifier();

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

        auto parse_ext_function_return_type(Parser &parser) -> std::optional<Type> {
            if (parser.match(TokenKind::Arrow)) {
                return parse_type(parser);
            }

            return std::nullopt;
        }

        auto parse_ext_function_decl(Parser &parser, const bool is_pub) -> ExtFunctionDecl {
            const auto location = parser.current_location();

            parser.expect(TokenKind::KwFn, "'fn'");

            const auto fn_name = parser.expect_identifier();
            bool is_variadic = false;
            auto fn_params = parse_ext_function_params(parser, is_variadic);
            auto fn_return_type = parse_ext_function_return_type(parser);

            return ExtFunctionDecl{
                .is_pub = is_pub,
                .is_variadic = is_variadic,
                .name = fn_name,
                .params = std::move(fn_params),
                .return_type = std::move(fn_return_type),
                .location = location,
            };
        }

        auto parse_var_decl(Parser &parser, const bool is_pub) -> Decl {
            const auto location = parser.current_location();

            const auto is_mut = parser.match(TokenKind::KwMut);
            if (!is_mut) {
                parser.expect(TokenKind::KwConst, "'const' or 'mut'");
            }

            const auto name = parser.expect_identifier();

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
                .name = name,
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
                const auto param_name = parser.expect_identifier();

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

            const auto name = parser.expect_identifier();
            auto params = parse_macro_params(parser);

            parser.expect(TokenKind::Arrow, "'->'");

            return MacroDecl{
                .is_pub = is_pub,
                .name = name,
                .params = std::move(params),
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

        auto parse_array_or_slice_type(Parser &parser) -> Type {
            const auto location = parser.current_location();

            parser.expect(TokenKind::LBracket, "'['");

            if (parser.match(TokenKind::RBracket)) {
                return std::make_unique<SliceType>(SliceType{
                    .base_type = parse_type(parser),
                    .location = location,
                });
            }

            auto size = parse_expr(parser);

            parser.expect(TokenKind::RBracket, "']'");
            return std::make_unique<ArrayType>(ArrayType{
                .base_type = parse_type(parser),
                .size = std::move(size),
                .location = location,
            });
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

        if (parser.check(TokenKind::LBracket)) {
            return parse_array_or_slice_type(parser);
        }

        if (parser.check(TokenKind::KwStruct)) {
            return parse_struct_type(parser);
        }

        if (parser.check(TokenKind::KwEnum)) {
            return parse_enum_type(parser);
        }

        if (parser.check(TokenKind::KwUnion)) {
            return parse_union_type(parser);
        }

        if (parser.check(TokenKind::KwFn)) {
            return parse_function_type(parser);
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

    auto parse_switch_stmt(Parser &parser) -> Stmt {
        const auto location = parser.current_location();
        parser.expect(TokenKind::KwSwitch, "'switch'");

        auto operand = parse_expr(parser);
        parser.expect(TokenKind::LBrace, "'{'");

        std::vector<SwitchStmt::Arm> arms;
        while (!parser.check(TokenKind::RBrace) && !parser.at_end()) {
            const auto arm_location = parser.current_location();

            auto arm_pattern = [&]() -> MatchExpr::ArmPattern {
                if (parser.check(TokenKind::Dot)) {
                    parser.advance();
                    auto vname = parser.expect_identifier();
                    std::optional<std::string> capture_name;
                    bool capture_by_ref = false;
                    if (parser.match(TokenKind::LParen)) {
                        if (parser.match(TokenKind::Ampersand)) {
                            capture_by_ref = true;
                        }
                        capture_name = parser.expect_identifier();
                        parser.expect(TokenKind::RParen, "')'");
                    }
                    return MatchExpr::VariantPattern{
                        .name = std::move(vname),
                        .capture_name = std::move(capture_name),
                        .capture_by_ref = capture_by_ref,
                    };
                }
                if (parser.check(TokenKind::Identifier) && parser.current_lexeme() == "_") {
                    parser.advance();
                    return MatchExpr::DefaultPattern{};
                }
                auto lp_expr = std::make_unique<Expr>(parse_expr(parser));
                return MatchExpr::LiteralPattern{std::move(lp_expr)};
            }();

            parser.expect(TokenKind::Colon, "':'");
            auto body = parse_stmt(parser);

            arms.push_back(SwitchStmt::Arm{
                .pattern = std::move(arm_pattern),
                .body = std::move(body),
                .location = arm_location,
            });

            if (!parser.check(TokenKind::RBrace)) {
                parser.expect(TokenKind::Comma, "','");
            }
        }

        parser.expect(TokenKind::RBrace, "'}'");

        return std::make_unique<SwitchStmt>(SwitchStmt{
            .operand = std::move(operand),
            .arms = std::move(arms),
            .location = location,
        });
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

        if (parser.check(TokenKind::KwFor)) {
            return parse_for_in_stmt(parser);
        }

        if (parser.check(TokenKind::KwSwitch)) {
            return parse_switch_stmt(parser);
        }

        if (parser.check(TokenKind::KwContinue)) {
            return parse_continue_stmt(parser);
        }

        if (parser.check(TokenKind::KwBreak)) {
            return parse_break_stmt(parser);
        }

        if (parser.check(TokenKind::KwReturn)) {
            return parse_return_stmt(parser);
        }

        if (parser.check(TokenKind::KwDefer)) {
            const auto location = parser.current_location();
            parser.advance();
            return std::make_unique<DeferStmt>(DeferStmt{
                .body = parse_stmt(parser),
                .location = location,
            });
        }

        return parse_expr_stmt(parser);
    }

    auto parse_impl_method(Parser &parser) -> ImplDecl::Function {
        const auto location = parser.current_location();
        const bool is_pub = parser.match(TokenKind::KwPub);

        parser.expect(TokenKind::KwFn, "'fn'");
        auto name = parser.expect_identifier();

        parser.expect(TokenKind::LParen, "'('");

        // Parse self parameter: `self` or `mut self`
        bool is_mut_self = false;
        if (parser.check(TokenKind::KwMut)) {
            parser.advance();
            is_mut_self = true;
        }

        // Consume 'self' identifier
        const auto self_name = parser.expect_identifier();
        if (self_name != "self") {
            parser.report_error(location, "first parameter of impl function must be 'self' or 'mut self'");
        }

        // Parse remaining params (with types)
        std::vector<ImplDecl::Function::Param> params;
        bool seen_variadic = false;
        while (!parser.check(TokenKind::RParen) && !parser.at_end()) {
            parser.expect(TokenKind::Comma, "','");
            const auto param_location = parser.current_location();
            const bool param_is_mut = parser.match(TokenKind::KwMut);
            auto param_name = parser.expect_identifier();
            parser.expect(TokenKind::Colon, "':'");

            if (seen_variadic) {
                parser.report_error(param_location, "'...' variadic parameter must be the last parameter in the parameter list");
            }

            bool param_is_variadic = false;
            if (parser.check(TokenKind::DotDotDot)) {
                parser.advance();
                param_is_variadic = true;
                seen_variadic = true;
            }

            params.push_back(ImplDecl::Function::Param{
                .name = std::move(param_name),
                .type = parse_type(parser),
                .is_mut = param_is_mut,
                .is_variadic = param_is_variadic,
                .location = param_location,
            });
        }

        parser.expect(TokenKind::RParen, "')'");
        auto return_types = parse_function_return_types(parser);
        auto body = parse_stmt(parser);

        return ImplDecl::Function{
            .is_pub = is_pub,
            .is_mut_self = is_mut_self,
            .name = std::move(name),
            .params = std::move(params),
            .return_types = std::move(return_types),
            .body = std::move(body),
            .location = location,
        };
    }

    auto parse_impl_decl(Parser &parser) -> ImplDecl {
        const auto location = parser.current_location();
        parser.expect(TokenKind::KwImpl, "'impl'");
        auto target = parse_named_type(parser);
        parser.expect(TokenKind::LBrace, "'{'");

        std::vector<ImplDecl::Function> functions;
        while (!parser.check(TokenKind::RBrace) && !parser.at_end()) {
            functions.push_back(parse_impl_method(parser));
        }

        parser.expect(TokenKind::RBrace, "'}'");

        return ImplDecl{
            .target = std::move(target),
            .functions = std::move(functions),
            .location = location,
        };
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

        if (parser.check(TokenKind::KwImpl)) {
            if (is_pub) {
                parser.report_error(parser.current_location(), "'impl' blocks cannot be 'pub'");
            }
            return parse_impl_decl(parser);
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
