#include "ast.hpp"

#include "asm_lexer.hpp"
#include "asm_parser.hpp"
#include "asm_registers.hpp"

#include <charconv>
#include <format>
#include <limits>
#include <sstream>

namespace ast {
    namespace {
        // Defined further below (after 'starts_type_only', which parse_generic_arg needs);
        // forward-declared here so parse_named_type can use them.
        auto parse_generic_args(Parser &parser) -> std::vector<GenericArg>;
        auto parse_generic_params(Parser &parser) -> std::vector<GenericParam>;

        // 'allow_generic_args' is false only when called from parse_impl_decl for an impl's
        // own target/trait/type NamedType operands — there, a following '[...]' is always
        // impl_decl's own 'generic_params' clause (parsed separately by the caller), never
        // this NamedType's 'generic_args'. See grammar.md note 17. Only the leaf segment of a
        // dotted chain (the one with no following '.') ever consumes generic_args; the flag
        // itself is threaded unchanged through the recursion for '.'-qualified chains.
        auto parse_named_type(Parser &parser, bool allow_generic_args = true) -> NamedType {
            const auto location = parser.current_location();
            const auto name = parser.expect_identifier();

            if (parser.match(TokenKind::Dot)) {
                auto member = parse_named_type(parser, allow_generic_args);

                return NamedType{
                    .name = name,
                    .member = std::make_unique<NamedType>(std::move(member)),
                    .generic_args = {},
                    .location = location,
                };
            }

            std::vector<std::unique_ptr<GenericArg>> generic_args;
            if (allow_generic_args && parser.check(TokenKind::LBracket)) {
                for (auto &arg : parse_generic_args(parser)) {
                    generic_args.push_back(std::make_unique<GenericArg>(std::move(arg)));
                }
            }

            return NamedType{
                .name = name,
                .member = nullptr,
                .generic_args = std::move(generic_args),
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
            case TokenKind::KwAnyptr: return BuiltinTypeKind::Anyptr;
            case TokenKind::KwType:   return BuiltinTypeKind::Type;
            case TokenKind::KwAny:    return BuiltinTypeKind::Any;
            default:                  return std::nullopt;
            }
        }

        // True for tokens that can only ever begin a Type in this grammar, never an Expr -
        // lets size_of's operand disambiguate a type from a value with a single token of
        // lookahead instead of backtracking. Prefix '*' always means pointer-type (dereference
        // is postfix '.*', see parse_postfix); '[' only starts array/slice type syntax (array
        // literals use '{...}'); struct/enum/union/fn only appear in type/decl position, never
        // as expression-starters; builtin type keywords never lex as Identifier.
        auto starts_type_only(const Parser &parser) -> bool {
            switch (parser.current().kind) {
            case TokenKind::Star:
            case TokenKind::LBracket:
            case TokenKind::KwStruct:
            case TokenKind::KwEnum:
            case TokenKind::KwUnion:
            case TokenKind::KwBitset:
            case TokenKind::KwFn:
            case TokenKind::KwTrait:
            case TokenKind::KwU8:
            case TokenKind::KwU16:
            case TokenKind::KwU32:
            case TokenKind::KwU64:
            case TokenKind::KwI8:
            case TokenKind::KwI16:
            case TokenKind::KwI32:
            case TokenKind::KwI64:
            case TokenKind::KwF32:
            case TokenKind::KwF64:
            case TokenKind::KwUSize:
            case TokenKind::KwBool:
            case TokenKind::KwByte:
            case TokenKind::KwError:
            case TokenKind::KwAnyptr:
            case TokenKind::KwType:
            case TokenKind::KwAny:
                return true;
            default:
                return false;
            }
        }

        // Several list-parsing loops below (struct/union/enum/trait/impl bodies, parameter
        // lists) parse one member/param per iteration via expect()/expect_identifier()/
        // parse_type(), none of which are guaranteed to consume a token on failure - a
        // single unexpected token (not the closing delimiter, not EOF) would otherwise spin
        // the loop forever, pushing a new garbage entry every iteration. This guard forces
        // at least one token of progress per iteration regardless of what failed inside it.
        class LoopProgressGuard {
          public:
            explicit LoopProgressGuard(Parser &parser) : parser_(parser), start_offset_(parser.current_location().offset) {
            }

            LoopProgressGuard(const LoopProgressGuard &) = delete;
            auto operator=(const LoopProgressGuard &) -> LoopProgressGuard & = delete;

            ~LoopProgressGuard() {
                if (!parser_.at_end() && parser_.current_location().offset == start_offset_) {
                    parser_.advance();
                }
            }

          private:
            Parser &parser_;
            size_t start_offset_;
        };

        // A single 'generic_arg' — 'type' for a 'T: type' parameter, 'expr' for a value
        // parameter. Dispatched with exactly the same one-token lookahead rule size_of's
        // operand uses (grammar note 12): a builtin type keyword, or a token that can only
        // ever begin a type, parses as 'type'; anything else (including a plain identifier
        // that may itself simply name a type, e.g. a type parameter passed through
        // unchanged) parses as 'expr'. See grammar.md note 18.
        auto parse_generic_arg(Parser &parser) -> GenericArg {
            const auto location = parser.current_location();

            if (starts_type_only(parser)) {
                return GenericArg{
                    .value = parse_type(parser),
                    .location = location,
                };
            }

            return GenericArg{
                .value = parse_expr(parser),
                .location = location,
            };
        }

        // 'generic_args ::= '[' generic_arg { ',' generic_arg } ']''
        // Parses 'generic_arg { ',' generic_arg }' up to but not including the closing ']',
        // appending to 'args'. Shared by parse_generic_args and parse_index_or_slice_expr,
        // which differ only in how the first argument is obtained -- the latter has to parse
        // it before it can tell an index/instantiation from a slice.
        //
        // That first-argument difference is why the two keep their own '[' / ']' framing
        // rather than sharing a whole-list helper: parse_index_or_slice_expr cannot know it is
        // reading a generic argument list until after it has read the first item and found no
        // '..'. Only the repeated part is genuinely common, and that is this.
        auto parse_generic_arg_list_tail(Parser &parser, std::vector<GenericArg> &args) -> void {
            while (!parser.check(TokenKind::RBracket) && !parser.at_end()) {
                const LoopProgressGuard progress_guard(parser);

                args.push_back(parse_generic_arg(parser));

                if (!parser.check(TokenKind::RBracket)) {
                    parser.expect(TokenKind::Comma, "','");
                }
            }
        }

        auto parse_generic_args(Parser &parser) -> std::vector<GenericArg> {
            parser.expect(TokenKind::LBracket, "'['");

            std::vector<GenericArg> args;
            parse_generic_arg_list_tail(parser, args);

            parser.expect(TokenKind::RBracket, "']'");

            return args;
        }

        // 'generic_param ::= IDENT ':' type' — sema (not the parser) validates the declared
        // type resolves to the builtin 'type' keyword or a builtin scalar type.
        auto parse_generic_param(Parser &parser) -> GenericParam {
            const auto location = parser.current_location();
            const auto name = parser.expect_identifier();

            parser.expect(TokenKind::Colon, "':'");

            return GenericParam{
                .name = name,
                .type = parse_type(parser),
                .location = location,
            };
        }

        // 'generic_params ::= '[' generic_param { ',' generic_param } ']''
        auto parse_generic_params(Parser &parser) -> std::vector<GenericParam> {
            parser.expect(TokenKind::LBracket, "'['");

            std::vector<GenericParam> params;
            while (!parser.check(TokenKind::RBracket) && !parser.at_end()) {
                const LoopProgressGuard progress_guard(parser);

                params.push_back(parse_generic_param(parser));

                if (!parser.check(TokenKind::RBracket)) {
                    parser.expect(TokenKind::Comma, "','");
                }
            }

            parser.expect(TokenKind::RBracket, "']'");

            return params;
        }

        auto parse_struct_type(Parser &parser) -> Type {
            const auto location = parser.current_location();

            auto is_packed = false;

            parser.expect(TokenKind::KwStruct, "'struct'");
            if (parser.match(TokenKind::LParen)) {
                is_packed = parser.match_identifier("packed");
                if (!is_packed) {
                    // 'struct()' and 'struct(anything-else)' were silently accepted as
                    // unpacked; the only defined qualifier is 'packed'.
                    parser.report_error(parser.current_location(), std::format(
                        "expected 'packed' in struct qualifier list, got '{}'", parser.current_lexeme()));
                    if (parser.check(TokenKind::Identifier)) parser.advance();
                }

                parser.expect(TokenKind::RParen, "')'");
            }

            parser.expect(TokenKind::LBrace, "'{'");

            std::vector<StructType::Field> fields;
            while (true) {
                skip_semicolons(parser);
                if (parser.check(TokenKind::RBrace) || parser.at_end() || parser.has_reached_max_errors()) {
                    break;
                }

                const LoopProgressGuard progress_guard(parser);

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
            while (true) {
                skip_semicolons(parser);
                if (parser.check(TokenKind::RBrace) || parser.at_end() || parser.has_reached_max_errors()) {
                    break;
                }

                const LoopProgressGuard progress_guard(parser);

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
            while (true) {
                skip_semicolons(parser);
                if (parser.check(TokenKind::RBrace) || parser.at_end() || parser.has_reached_max_errors()) {
                    break;
                }

                const LoopProgressGuard progress_guard(parser);

                fields.push_back(parse_enum_field(parser));
            }

            parser.expect(TokenKind::RBrace, "'}'");

            return std::make_unique<EnumType>(EnumType{
                .underlying_type = std::move(underlying_type),
                .fields = std::move(fields),
                .location = location,
            });
        }

        // 'error(A | B | C)' — one or more named error member types, separated by '|'.
        auto parse_error_type(Parser &parser) -> Type {
            const auto location = parser.current_location();

            parser.expect(TokenKind::KwError, "'error'");
            parser.expect(TokenKind::LParen, "'('");

            std::vector<NamedType> members;
            members.push_back(parse_named_type(parser));
            while (parser.match(TokenKind::Pipe)) {
                members.push_back(parse_named_type(parser));
            }

            parser.expect(TokenKind::RParen, "')'");

            return std::make_unique<ErrorType>(ErrorType{
                .members = std::move(members),
                .location = location,
            });
        }

        // 'bitset(NamedType [, builtin_type])' — the storage type argument is parsed
        // generically here (like enum's underlying-type argument); sema validates it's
        // one of u8/u16/u32/u64.
        auto parse_bitset_type(Parser &parser) -> Type {
            const auto location = parser.current_location();

            parser.expect(TokenKind::KwBitset, "'bitset'");
            parser.expect(TokenKind::LParen, "'('");

            auto member_type = parse_named_type(parser);

            std::optional<Type> storage_type;
            if (parser.match(TokenKind::Comma)) {
                storage_type = parse_type(parser);
            }

            parser.expect(TokenKind::RParen, "')'");

            return std::make_unique<BitsetType>(BitsetType{
                .member_type = std::move(member_type),
                .storage_type = std::move(storage_type),
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
            case TokenKind::CharLiteral:
            case TokenKind::Dollar:
            case TokenKind::PlusPlus:
            case TokenKind::MinusMinus:
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
            case TokenKind::KwAlignOf:
            case TokenKind::KwTypeOf:
            case TokenKind::KwTypeInfoOf:
            case TokenKind::KwStackAlloc:
            case TokenKind::KwLen:
            case TokenKind::KwImportBin:
            case TokenKind::KwDefault:
            case TokenKind::KwUndefined:
            case TokenKind::KwMatch:
            case TokenKind::KwTry:
            case TokenKind::KwIota:
            case TokenKind::Dot:
            case TokenKind::LBrace:
            case TokenKind::KwAsm:
                return true;
            default:
                return false;
            }
        }

        // True when the current token is a synthesized (ASI-inserted) semicolon immediately
        // followed by a token that could plausibly start an expression. This flags the classic
        // 'return'/'return_ok' ASI gotcha: a bare keyword on its own line, followed by a
        // continuation expression the user likely intended to return, on the next line.
        auto is_asi_gotcha_candidate(const Parser &parser) -> bool {
            return parser.current().kind == TokenKind::Semicolon &&
                   parser.current_lexeme().empty() &&
                   can_start_expr(parser.peek().kind);
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
            const auto &lexeme = token.lexeme;

            uint64_t base = 10;
            size_t digit_start = 0;
            if (lexeme.starts_with("0x") || lexeme.starts_with("0X")) {
                base = 16;
                digit_start = 2;
            } else if (lexeme.starts_with("0b") || lexeme.starts_with("0B")) {
                base = 2;
                digit_start = 2;
            } else if (lexeme.starts_with("0o") || lexeme.starts_with("0O")) {
                base = 8;
                digit_start = 2;
            }

            // Accumulation is overflow-checked: a literal past 2^64-1 used to wrap silently
            // BEFORE sema's fits-in-target check ever saw it, so
            // '0x1_0000_0000_0000_0000' validated as 0.
            uint64_t value = 0;
            bool overflow = false;
            for (size_t i = digit_start; i < lexeme.size(); ++i) {
                if (lexeme[i] == '_') {
                    continue;
                }
                const auto digit = ToInt(lexeme[i]);
                if (value > (std::numeric_limits<uint64_t>::max() - digit) / base) {
                    overflow = true;
                    break;
                }
                value = value * base + digit;
            }
            if (overflow) {
                parser.report_error(location, std::format("integer literal '{}' does not fit in 64 bits", lexeme));
                value = 0;
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
                // Every token this short already carries a diagnostic: the lexer only
                // produces a StringLiteral without its closing quote after reporting it
                // unterminated, and a non-StringLiteral here means expect() just
                // reported the mismatch. A second "malformed" report would be noise.
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
                // As in parse_string_literal: a CharLiteral this short is exactly the
                // shape the lexer reported as empty/unterminated, and expect() reported
                // any non-CharLiteral. Stay silent instead of stacking a second error.
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

        // Parses one match/switch arm pattern, up to but not including the ':' that follows it.
        //
        //   .name            variant, no capture
        //   .name(capture)   variant, capture by value
        //   .name(&capture)  variant, capture by reference
        //   _                default
        //   <expr>           literal — any compile-time constant expression
        //
        // 'match' and 'switch' share the pattern grammar exactly; they differ only in the arm
        // BODY (an expression vs a statement). Keeping one parser is what stops that from
        // quietly stopping being true — sema and codegen both switch over a single
        // MatchExpr::ArmPattern for either construct.
        auto parse_match_arm_pattern(Parser &parser) -> MatchExpr::ArmPattern {
            if (parser.check(TokenKind::Dot)) {
                parser.advance();
                const auto name_location = parser.current_location();
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
                    .name_location = name_location,
                };
            }
            if (parser.check(TokenKind::Identifier) && parser.current_lexeme() == "_") {
                parser.advance();
                return MatchExpr::DefaultPattern{};
            }
            return MatchExpr::LiteralPattern{std::make_unique<Expr>(parse_expr(parser))};
        }

        // With the cursor ON the '{' that follows a variant name, is this a tagged-variant
        // constructor's payload ('Shape.circle{.radius = 3.0}') rather than the body of a
        // 'match'/'switch' whose scrutinee happens to end in that name ('switch h.kind { ... }')?
        //
        // Both open '{' '.' IDENT. The token AFTER that identifier is what separates them:
        //
        //     {  .radius  =  3.0     '=' -> a payload field, so a constructor
        //     {  .is_type :  {}      ':' -> a match arm
        //     {  .is_scalar( v )     '(' -> a match arm with a capture
        //
        // Two tokens of lookahead is one short, which is why 'switch h.kind { ... }' used to be
        // parsed as a constructor and rejected with "expected '=', got ':'". Identifiers, derefs,
        // calls, indexes and parenthesised expressions escaped only because named_type_from_expr
        // rejects them as type paths — an accident, not a rule.
        //
        // Note the decision is made BEFORE consuming anything: the parser has no backtracking,
        // so committing to a constructor and discovering an arm is unrecoverable.
        auto looks_like_variant_payload(Parser &parser) -> bool {
            return parser.check(TokenKind::LBrace) && parser.check_at(1, TokenKind::Dot) &&
                   parser.check_at(2, TokenKind::Identifier) && parser.check_at(3, TokenKind::Equal);
        }

        // Parses a '.field = expr, ...' list and its closing '}', starting from just after the
        // opening '{'. The caller consumes the '{' itself, because the three constructs that
        // use this reach it differently: a braced initializer expects one, while both tagged-
        // variant forms only get here after a lookahead has already confirmed it.
        //
        // Shared by '{.x = 1}' (StructExpr), '.variant{.x = 1}' (contextual) and
        // 'Type.variant{.x = 1}' (qualified). All three build the same StructExpr::Field list
        // and are read by the same sema code, so a divergence between them would be a
        // divergence in the language, not just in the parser.
        auto parse_dot_field_list(Parser &parser) -> std::vector<StructExpr::Field> {
            std::vector<StructExpr::Field> fields;
            while (!parser.check(TokenKind::RBrace) && !parser.at_end()) {
                const LoopProgressGuard progress_guard(parser);
                parser.expect(TokenKind::Dot, "'.'");
                const auto field_name = parser.expect_identifier();
                parser.expect(TokenKind::Equal, "'='");
                // Taken after '=', so the location spans the VALUE rather than the field name.
                const auto field_location = parser.current_location();
                fields.push_back(StructExpr::Field{
                    .name = field_name,
                    .expr = parse_expr(parser),
                    .location = field_location,
                });

                skip_semicolons(parser);
                if (parser.check(TokenKind::RBrace)) {
                    break;
                }

                parser.expect(TokenKind::Comma, "','");
            }

            parser.expect(TokenKind::RBrace, "'}'");
            return fields;
        }

        auto parse_braced_initializer(Parser &parser) -> std::unique_ptr<BracedInitializerExpr> {
            const auto location = parser.current_location();

            parser.expect(TokenKind::LBrace, "'{'");
            if (parser.match(TokenKind::RBrace)) {
                return std::make_unique<BracedInitializerExpr>(EmptyExpr{
                    .location = location,
                });
            }

            if (parser.check(TokenKind::Dot) && parser.peek().kind == TokenKind::Identifier &&
                parser.peek_next().kind != TokenKind::LBrace &&
                parser.peek_next().kind != TokenKind::LParen) {
                // Disambiguate '{.field = expr, ...}' (StructExpr) from '{.A, .B}' (a bitset
                // literal, BitsetExpr) by whether '=' follows the first '.IDENT'. A later
                // field of the "wrong" shape naturally trips the chosen loop's own expect(),
                // producing a parse error for mixed '.IDENT' / '.IDENT = expr' forms.
                //
                // '{' and '(' after the identifier both mean this is not struct/bitset
                // territory at all but a collection whose first element is a tagged-union
                // variant -- '.variant{...}' braced payload or '.variant(expr)' positional
                // payload. Both fall through to ordinary element parsing below, where
                // parse_primary's own '.' handling parses the variant correctly.
                if (parser.peek_next().kind == TokenKind::Equal) {
                    return std::make_unique<BracedInitializerExpr>(StructExpr{
                        .fields = parse_dot_field_list(parser),
                        .location = location,
                    });
                }

                std::vector<BitsetExpr::Member> members;
                while (!parser.check(TokenKind::RBrace) && !parser.at_end()) {
                    const LoopProgressGuard progress_guard(parser);
                    parser.expect(TokenKind::Dot, "'.'");
                    // Taken before expect_identifier consumes it, so the location points at
                    // the flag name rather than at whatever follows it.
                    const auto member_location = parser.current_location();
                    members.push_back(BitsetExpr::Member{
                        .name = parser.expect_identifier(),
                        .location = member_location,
                    });

                    skip_semicolons(parser);
                    if (parser.check(TokenKind::RBrace)) {
                        break;
                    }

                    parser.expect(TokenKind::Comma, "','");
                }

                parser.expect(TokenKind::RBrace, "'}'");

                return std::make_unique<BracedInitializerExpr>(BitsetExpr{
                    .members = std::move(members),
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

            skip_semicolons(parser);
            parser.expect(TokenKind::RBrace, "'}'");

            return std::make_unique<BracedInitializerExpr>(ArrayExpr{
                .values = std::move(values),
                .has_fill = has_fill,
                .location = location,
            });
        }

        // '$option(key)' / '$option(key, default)'. 'option' is parsed as a plain
        // identifier after the '$' sigil (not a keyword) — mirrors 'ext fn's precedent of
        // dispatching on a bare identifier lexeme rather than reserving a new keyword.
        // The shared '(key)' / '(key, default)' argument tail of '$option' and '$env'.
        struct KeyDefaultArgs {
            std::string key;
            std::optional<Expr> default_value;
        };
        auto parse_key_default_args(Parser &parser) -> KeyDefaultArgs {
            parser.expect(TokenKind::LParen, "'('");
            auto key = parse_string_literal(parser);

            std::optional<Expr> default_value;
            if (parser.match(TokenKind::Comma)) {
                default_value = parse_expr(parser);
            }

            parser.expect(TokenKind::RParen, "')'");
            return KeyDefaultArgs{.key = std::move(key.value), .default_value = std::move(default_value)};
        }

        auto parse_option_expr(Parser &parser) -> Expr {
            const auto location = parser.current_location();
            parser.expect(TokenKind::Dollar, "'$'");

            if (!parser.check(TokenKind::Identifier) || parser.current_lexeme() != "option") {
                parser.report_error(location, std::format("expected 'option' or 'env' after '$', got '{}'", parser.current_lexeme()));

                return LiteralIntegerExpr{.value = 0, .location = location};
            }
            parser.advance(); // consume 'option'

            auto [key, default_value] = parse_key_default_args(parser);

            return std::make_unique<OptionExpr>(OptionExpr{
                .key = std::move(key),
                .default_value = std::move(default_value),
                .location = location,
            });
        }

        // '$env(key)' / '$env(key, default)' — reads 'key' as an environment variable
        // instead of a '--opt key=value' driver flag. Parsed identically to '$option'
        // above (see its comment); the caller (parse_primary) has already peeked past '$'
        // to confirm the identifier is 'env' before dispatching here.
        auto parse_env_expr(Parser &parser) -> Expr {
            const auto location = parser.current_location();
            parser.expect(TokenKind::Dollar, "'$'");
            parser.advance(); // consume 'env'

            auto [key, default_value] = parse_key_default_args(parser);

            return std::make_unique<EnvExpr>(EnvExpr{
                .key = std::move(key),
                .default_value = std::move(default_value),
                .location = location,
            });
        }

        // '#link(category, data)' — a linker directive. 'link' (like 'error'/'warn' below)
        // is a plain identifier after '#', not a keyword — a distinct sigil from the '$'
        // used by 'option'/'env' above. Callable from both module-scope declaration
        // parsing and (permissively) statement parsing — see parse_stmt's dispatch, which
        // peeks past '#' to recognize '#link' before deciding whether to consume it here
        // or fall through to an ordinary expr-stmt.
        auto parse_link_decl(Parser &parser) -> LinkDecl {
            const auto location = parser.current_location();
            parser.expect(TokenKind::Hash, "'#'");

            if (!parser.check(TokenKind::Identifier) || parser.current_lexeme() != "link") {
                parser.report_error(location, std::format("expected 'link' after '#', got '{}'", parser.current_lexeme()));

                return LinkDecl{
                    .category = LinkCategory::Lib,
                    .data = LiteralIntegerExpr{.value = 0, .location = location},
                    .location = location,
                };
            }
            parser.advance(); // consume 'link'

            parser.expect(TokenKind::LParen, "'('");

            auto category = LinkCategory::Lib;
            if (parser.check(TokenKind::Identifier)) {
                const auto category_name = parser.current_lexeme();
                if (category_name == "lib") category = LinkCategory::Lib;
                else if (category_name == "system") category = LinkCategory::System;
                else if (category_name == "flag") category = LinkCategory::Flag;
                else parser.report_error(parser.current_location(), std::format("expected 'lib', 'system', or 'flag', got '{}'", category_name));
                parser.advance();
            } else {
                parser.report_error(parser.current_location(), "expected link category ('lib', 'system', or 'flag')");
            }

            parser.expect(TokenKind::Comma, "','");
            auto data = parse_expr(parser);
            parser.expect(TokenKind::RParen, "')'");

            return LinkDecl{
                .category = category,
                .data = std::move(data),
                .location = location,
            };
        }

        // '#error(message)' / '#warn(message)'. 'warn' (like 'option'/'link' above) is a
        // plain identifier after '#', not a keyword — but 'error' already IS a keyword
        // (KwError, for the 'error(T)' type syntax), so lookahead for these two can't share
        // a single 'Identifier' check; see peek_diagnostic_directive_kind below.
        auto parse_diagnostic_decl(Parser &parser, const DiagnosticDirectiveKind kind) -> DiagnosticDecl {
            const auto location = parser.current_location();
            parser.expect(TokenKind::Hash, "'#'");
            parser.advance(); // consume 'error' or 'warn'

            parser.expect(TokenKind::LParen, "'('");
            auto message = parse_expr(parser);
            parser.expect(TokenKind::RParen, "')'");

            return DiagnosticDecl{
                .kind = kind,
                .message = std::move(message),
                .location = location,
            };
        }

        // Returns which '#error'/'#warn' directive (if any) starts at the CURRENT token
        // (the '#' itself, not yet consumed) — nullopt if the current token isn't '#' or the
        // following token doesn't name either directive. 'error' lexes as KwError, 'warn' as
        // a plain Identifier.
        auto peek_diagnostic_directive_kind(const Parser &parser) -> std::optional<DiagnosticDirectiveKind> {
            if (!parser.check(TokenKind::Hash)) return std::nullopt;
            if (parser.peek().kind == TokenKind::KwError) return DiagnosticDirectiveKind::Error;
            if (parser.peek().kind == TokenKind::Identifier && parser.peek().lexeme == "warn") return DiagnosticDirectiveKind::Warn;
            return std::nullopt;
        }

        // Whether a '#link(...)' directive starts at the CURRENT token (the '#' itself, not yet
        // consumed). Mirrors peek_diagnostic_directive_kind above; both module-scope and
        // statement-position dispatch use it, which previously spelled the lookahead out
        // verbatim at each site.
        auto peek_link_decl(const Parser &parser) -> bool {
            return parser.check(TokenKind::Hash) && parser.peek().kind == TokenKind::Identifier &&
                   parser.peek().lexeme == "link";
        }

        // Reports a parser-stage "unknown attribute" error if 'name' isn't one of the five
        // known declaration-attribute names (see Attribute's doc comment in ast.hpp) — a pure
        // lexical fact, checked identically for both the bare/single-with-args form and the
        // grouped '@(...)' form's members below.
        void check_known_attribute_name(Parser &parser, const std::string &name, const SourceLocation &location) {
            if (name == "no_return" || name == "naked" || name == "always_inline" ||
                name == "section" || name == "init") {
                return;
            }
            parser.report_error(location, std::format(
                "unknown attribute '@{}'. Known attributes: no_return, naked, always_inline, "
                "section, init.", name));
        }

        // '@name' / '@name(arg1, arg2, ...)' — a single (non-grouped) attribute. The '@' is
        // known to be present (checked by the caller, parse_attribute_clause) but not yet
        // consumed.
        auto parse_single_attribute(Parser &parser) -> Attribute {
            const auto location = parser.current_location();
            parser.expect(TokenKind::At, "'@'");
            const auto name = parser.expect_identifier();
            check_known_attribute_name(parser, name, location);

            std::vector<Expr> args;
            if (parser.match(TokenKind::LParen)) {
                if (!parser.check(TokenKind::RParen)) {
                    args.push_back(parse_expr(parser));
                    while (parser.match(TokenKind::Comma)) {
                        args.push_back(parse_expr(parser));
                    }
                }
                parser.expect(TokenKind::RParen, "')'");
            }

            return Attribute{.name = name, .args = std::move(args), .location = location};
        }

        // '@(name1, name2, ...)' — the grouped attribute-clause form: one or more bare
        // attribute names, no per-name argument list (an argument list here, e.g.
        // '@(section("..."))', is a parse error — only the single '@name(args)' form in
        // parse_single_attribute takes arguments).
        // Consumes a '(' ... ')' span (best-effort depth tracking) so a single malformed
        // construct doesn't cascade into spurious follow-on parse errors. Mirrors
        // asm_parser.cpp's skip_bracketed_span. No-op unless positioned on a '('.
        auto skip_parenthesized_span(Parser &parser) -> void {
            if (!parser.check(TokenKind::LParen)) {
                return;
            }

            parser.advance(); // '('
            int depth = 1;
            while (!parser.at_end() && depth > 0) {
                if (parser.check(TokenKind::LParen)) {
                    ++depth;
                } else if (parser.check(TokenKind::RParen)) {
                    --depth;
                    if (depth == 0) {
                        parser.advance();
                        return;
                    }
                }
                parser.advance();
            }
        }

        auto parse_grouped_attributes(Parser &parser) -> std::vector<Attribute> {
            parser.expect(TokenKind::At, "'@'");
            parser.expect(TokenKind::LParen, "'('");

            std::vector<Attribute> attrs;
            do {
                const auto location = parser.current_location();
                const auto name = parser.expect_identifier();
                check_known_attribute_name(parser, name, location);

                if (parser.check(TokenKind::LParen)) {
                    parser.report_error(parser.current_location(),
                        "a grouped attribute ('@(...)') cannot take arguments; use the single "
                        "'@name(args)' form instead");
                    // Consume the offending argument list. Leaving it made the recovery loop
                    // exit, expect(RParen) fail on the same '(', and control return to
                    // parse_decl still sitting on it -- one mistake produced seven diagnostics.
                    skip_parenthesized_span(parser);
                }

                attrs.push_back(Attribute{.name = name, .args = {}, .location = location});
            } while (parser.match(TokenKind::Comma));

            parser.expect(TokenKind::RParen, "')'");
            return attrs;
        }

        // Parses zero or one attribute clause preceding a declaration: a bare '@name', a
        // single '@name(args)', or a grouped '@(name1, name2, ...)'. A second bare '@' after
        // the first clause is simply not consumed here (there is no "stacking" of separate
        // clauses) — the caller's own dispatch (expecting 'fn' next) naturally reports
        // "expected 'fn', got '@'" for e.g. '@naked @no_return', with no special-casing
        // needed.
        auto parse_attribute_clause(Parser &parser) -> std::vector<Attribute> {
            if (!parser.check(TokenKind::At)) return {};

            if (parser.peek().kind == TokenKind::LParen) {
                return parse_grouped_attributes(parser);
            }

            std::vector<Attribute> attrs;
            attrs.push_back(parse_single_attribute(parser));
            return attrs;
        }

        // Shared operand disambiguation for 'size_of'/'align_of'/'type_of': a token that
        // can only ever begin a type parses as a TypeExpr operand, anything else as an
        // ordinary expression — the same one-token lookahead rule parse_generic_arg uses
        // (grammar.md note 12).
        auto parse_type_or_expr_operand(Parser &parser) -> Expr {
            if (starts_type_only(parser)) {
                const auto type_location = parser.current_location();
                return std::make_unique<TypeExpr>(TypeExpr{
                    .type = parse_type(parser),
                    .location = type_location,
                });
            }
            return parse_expr(parser);
        }

        auto parse_primary(Parser &parser) -> Expr {
            const auto location = parser.current_location();

            if (parser.check(TokenKind::IntLiteral)) {
                return parse_literal_integer_expr(parser);
            }

            if (parser.check(TokenKind::FloatLiteral)) {
                auto &token = parser.advance();

                // Digit-group underscores are part of the lexeme ('1_000.5'); strip them
                // before conversion, which would otherwise stop at the first '_'.
                auto digits = token.lexeme;
                std::erase(digits, '_');

                // from_chars rather than std::stod: stod throws on out-of-range input
                // ('1e999'), which previously terminated the compiler.
                double value = 0.0;
                const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
                if (ec == std::errc::result_out_of_range) {
                    parser.report_error(location, std::format("float literal '{}' is out of range for f64", token.lexeme));
                    value = 0.0;
                } else if (ec != std::errc{} || ptr != digits.data() + digits.size()) {
                    parser.report_error(location, std::format("malformed float literal '{}'", token.lexeme));
                    value = 0.0;
                }

                return LiteralFloatExpr{
                    .value = value,
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

                auto operand = parse_type_or_expr_operand(parser);

                parser.expect(TokenKind::RParen, "')'");

                return std::make_unique<SizeOfExpr>(SizeOfExpr{
                    .operand = std::move(operand),
                    .location = location,
                });
            }

            if (parser.check(TokenKind::KwAlignOf)) {
                parser.advance();
                parser.expect(TokenKind::LParen, "'('");

                auto operand = parse_type_or_expr_operand(parser);

                parser.expect(TokenKind::RParen, "')'");

                return std::make_unique<AlignOfExpr>(AlignOfExpr{
                    .operand = std::move(operand),
                    .location = location,
                });
            }

            if (parser.check(TokenKind::KwTypeOf)) {
                parser.advance();
                parser.expect(TokenKind::LParen, "'('");

                auto operand = parse_type_or_expr_operand(parser);

                parser.expect(TokenKind::RParen, "')'");

                return std::make_unique<TypeOfExpr>(TypeOfExpr{
                    .operand = std::move(operand),
                    .location = location,
                });
            }

            if (parser.check(TokenKind::KwTypeInfoOf)) {
                parser.advance();
                parser.expect(TokenKind::LParen, "'('");

                // Unlike size_of/align_of/type_of, the operand is always a plain expr —
                // 'type_of(i32)' itself parses as an ordinary primary expression here, no
                // starts_type_only gating needed.
                auto operand = parse_expr(parser);

                parser.expect(TokenKind::RParen, "')'");

                return std::make_unique<TypeInfoOfExpr>(TypeInfoOfExpr{
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

            if (parser.check(TokenKind::KwStackAlloc)) {
                parser.advance();
                parser.expect(TokenKind::LParen, "'('");
                auto size = parse_expr(parser);
                parser.expect(TokenKind::RParen, "')'");

                return std::make_unique<StackAllocExpr>(StackAllocExpr{
                    .size = std::move(size),
                    .location = location,
                });
            }

            // 'asm -> reg { ... }' / 'asm -> reg: type { ... }' — the expression form of
            // inline assembly (see parse_asm_stmt above for the statement form's raw-body
            // pipeline, reused verbatim here). 'parse_stmt' only reaches this branch when
            // 'asm' is NOT immediately followed by '{' (the statement form dispatches
            // directly there instead).
            if (parser.check(TokenKind::KwAsm)) {
                parser.advance(); // 'asm'
                parser.expect(TokenKind::Arrow, "'->'");

                const auto reg_location = parser.current_location();
                // Normalized like asm bodies are ('MOV RAX' works), and stored normalized,
                // as AsmRegisterOperand::name documents.
                const auto reg_name = asm_registers::to_lower(parser.expect_identifier());
                const auto *reg_info = asm_registers::lookup_register(reg_name);
                if (!reg_info) {
                    if (asm_registers::is_unsupported_register(reg_name)) {
                        parser.report_error(reg_location,
                            std::format("register '{}' is not supported in inline asm (v1)", reg_name));
                    } else {
                        parser.report_error(reg_location, "expected a register name after 'asm ->'");
                    }
                }

                auto result_register = AsmRegisterOperand{
                    .name = reg_name,
                    .width_bits = reg_info ? reg_info->width_bits : 0,
                    .location = reg_location,
                };

                std::optional<Type> result_type;
                if (parser.match(TokenKind::Colon)) {
                    result_type = parse_type(parser);
                }

                const auto block_tok = parser.expect(TokenKind::AsmBlock, "asm block body");
                if (block_tok.kind != TokenKind::AsmBlock) {
                    // expect() reported and did NOT advance; tokenizing the unrelated
                    // current token's lexeme as asm produced cascading bogus diagnostics
                    // ("expected an instruction mnemonic") at a misleading location.
                    return std::make_unique<AsmExpr>(AsmExpr{
                        .instructions = {},
                        .result_register = std::move(result_register),
                        .result_type = std::move(result_type),
                        .location = location,
                    });
                }
                auto asm_tokens = asm_lexer::tokenize(block_tok.lexeme, block_tok.location, parser.diagnostics());
                auto stmt = asm_parser::parse(asm_tokens, parser.diagnostics());

                return std::make_unique<AsmExpr>(AsmExpr{
                    .instructions = std::move(stmt.instructions),
                    .result_register = std::move(result_register),
                    .result_type = std::move(result_type),
                    .location = location,
                });
            }

            if (parser.check(TokenKind::Dollar)) {
                if (parser.peek().kind == TokenKind::Identifier && parser.peek().lexeme == "env") {
                    return parse_env_expr(parser);
                }
                return parse_option_expr(parser);
            }

            if (parser.check(TokenKind::KwImportBin)) {
                parser.advance();
                parser.expect(TokenKind::LParen, "'('");

                const auto path = parse_string_literal(parser);

                parser.expect(TokenKind::RParen, "')'");

                return ImportBinExpr{
                    .path = path.value,
                    .location = location,
                };
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
                const auto name_location = parser.current_location();
                const auto name = parser.expect_identifier();

                // Extend the '.' token's span to cover the identifier too, so diagnostics
                // and LSP lookups for '.name' underline/target the whole expression rather
                // than just the leading dot.
                auto span = location;
                span.length = name_location.offset + name_location.length - location.offset;

                // A contextual tagged variant with a braced payload: .variant{.field = val}.
                // Same predicate as the qualified form in parse_postfix, deliberately: they are
                // one disambiguation rule and must not drift apart.
                if (looks_like_variant_payload(parser)) {
                    const auto brace_loc = parser.current_location();
                    parser.advance(); // consume '{'
                    return std::make_unique<TaggedVariantExpr>(TaggedVariantExpr{
                        .type_path = std::nullopt,
                        .variant_name = name,
                        .payload = StructExpr{.fields = parse_dot_field_list(parser), .location = brace_loc},
                        .location = span,
                    });
                }

                // '.variant(expr)' — sugar for a single-value payload, equivalent to
                // '.variant{.v = expr}'. Only meaningful for a variant whose payload struct
                // has exactly one field named "v" (the convention used for every non-struct
                // payload — scalar, enum, union, slice, pointer, array — see
                // type_resolver.cpp's payload-wrapping in layout_union/synthesize_error_union);
                // sema reports the ordinary "no field 'v'" error otherwise. Unambiguous with a
                // real call expression: a bare '.name' can never resolve to a callable value
                // (DotIdentExpr always requires an expected enum/tagged-union type), so this
                // parse never collides with an actual function call.
                if (parser.check(TokenKind::LParen)) {
                    const auto paren_loc = parser.current_location();
                    parser.advance(); // consume '('
                    std::vector<StructExpr::Field> fields;
                    if (!parser.check(TokenKind::RParen)) {
                        const auto field_loc = parser.current_location();
                        fields.push_back(StructExpr::Field{
                            .name = "v",
                            .expr = parse_expr(parser),
                            .location = field_loc,
                        });
                        if (parser.check(TokenKind::Comma)) {
                            parser.report_error(parser.current_location(), "tagged-variant payload construction takes exactly one value");
                            while (parser.match(TokenKind::Comma)) {
                                parse_expr(parser); // consume and discard extras
                            }
                        }
                    }
                    parser.expect(TokenKind::RParen, "')'");
                    if (fields.empty()) {
                        parser.report_error(paren_loc, "tagged-variant payload construction requires exactly one value; use '.name' for a payload-free variant");
                    }
                    return std::make_unique<TaggedVariantExpr>(TaggedVariantExpr{
                        .type_path = std::nullopt,
                        .variant_name = name,
                        .payload = StructExpr{.fields = std::move(fields), .location = paren_loc},
                        .location = span,
                    });
                }

                return DotIdentExpr{
                    .name = name,
                    .location = span,
                };
            }

            if (parser.check(TokenKind::KwMatch)) {
                parser.advance();

                auto operand = parse_expr(parser);
                parser.expect(TokenKind::LBrace, "'{'");

                std::vector<MatchExpr::Arm> arms;
                while (!parser.check(TokenKind::RBrace) && !parser.at_end()) {
                    const LoopProgressGuard progress_guard(parser);
                    const auto arm_location = parser.current_location();

                    auto pattern = parse_match_arm_pattern(parser);

                    parser.expect(TokenKind::Colon, "':'");
                    auto arm_value = parse_expr(parser);

                    arms.push_back(MatchExpr::Arm{
                        .pattern = std::move(pattern),
                        .value = std::move(arm_value),
                        .location = arm_location,
                    });

                    // A block-bodied-looking arm value (or any value ending in an ASI trigger
                    // token) immediately followed by a newline before the closing '}' picks up
                    // a virtual semicolon here if the arm has no trailing comma; skip it before
                    // checking for the terminator, same as skip_semicolons' other call sites.
                    skip_semicolons(parser);
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

        // Ordinary indexing ('arr[i]'), slicing ('arr[i..j]'), and explicit generic-argument
        // instantiation ('List[i32]', 'Fixed[u8, 16]') all share this one postfix '['...']'
        // production and cannot be told apart by shape alone for a single-item, non-slice
        // bracket — see IndexOrInstantiateExpr's doc comment in ast.hpp and grammar.md note
        // 16. A comma-separated bracket is unambiguously generic_args (a slice/index bound is
        // always exactly one item, never comma-separated), so only the single-item, non-'..'
        // case is genuinely deferred to sema; a slice range ('..') is detected immediately
        // after the first item and always wins when present, since generic_args never
        // contains '..'.
        auto parse_index_or_slice_expr(Expr operand, Parser &parser) -> Expr {
            const auto location = parser.current_location();

            parser.expect(TokenKind::LBracket, "'['");

            // A leading '..' ('arr[..5]', 'arr[..]') has to be recognized before
            // parse_generic_arg runs: it would otherwise reach parse_primary and fail with
            // "expected expression, got '..'". No ambiguity to resolve first -- a generic
            // argument list can never begin with '..'.
            if (parser.match(TokenKind::DotDot)) {
                std::optional<Expr> end;
                if (!parser.check(TokenKind::RBracket)) {
                    end = parse_expr(parser);
                }
                parser.expect(TokenKind::RBracket, "']'");

                return make_expr(SliceExpr{
                    .operand = std::move(operand),
                    .start = std::nullopt,
                    .end = std::move(end),
                    .location = location,
                });
            }

            auto first_arg = parse_generic_arg(parser);

            if (auto *first_expr = std::get_if<Expr>(&first_arg.value); first_expr != nullptr && parser.match(TokenKind::DotDot)) {
                // 'arr[2..]' ends here; anything else is an upper bound.
                std::optional<Expr> end;
                if (!parser.check(TokenKind::RBracket)) {
                    end = parse_expr(parser);
                }
                parser.expect(TokenKind::RBracket, "']'");

                return make_expr(SliceExpr{
                    .operand = std::move(operand),
                    .start = std::move(*first_expr),
                    .end = std::move(end),
                    .location = location,
                });
            }

            std::vector<GenericArg> args;
            args.push_back(std::move(first_arg));
            if (!parser.check(TokenKind::RBracket)) {
                parser.expect(TokenKind::Comma, "','");
            }
            parse_generic_arg_list_tail(parser, args);

            parser.expect(TokenKind::RBracket, "']'");

            return make_expr(IndexOrInstantiateExpr{
                .operand = std::move(operand),
                .args = std::move(args),
                .location = location,
            });
        }

        // Converts a plain dotted-identifier chain (e.g. `Type` or `mod.Type`) into a NamedType
        // path for qualified tagged-variant construction. Returns nullopt if expr contains
        // anything other than identifiers/member access (e.g. a call or index result), so the
        // caller can fall back to ordinary member-access parsing.
        auto named_type_from_expr(const Expr &expr) -> std::optional<NamedType> {
            if (const auto *ident = std::get_if<IdentExpr>(&expr)) {
                return NamedType{.name = ident->name, .location = ident->location};
            }
            if (const auto *member = std::get_if<std::unique_ptr<MemberExpr>>(&expr)) {
                auto base = named_type_from_expr((*member)->object);
                if (!base) return std::nullopt;
                NamedType *tail = &*base;
                while (tail->member) tail = tail->member.get();
                tail->member = std::make_unique<NamedType>(NamedType{
                    .name = (*member)->member,
                    .location = (*member)->location,
                });
                return base;
            }
            return std::nullopt;
        }

        auto parse_postfix(Parser &parser) -> Expr {
            auto expr = parse_primary(parser);

            while (true) {
                // Captured fresh each iteration (not once before the loop) so each postfix
                // node in a chain (`a.b().c`) gets its own location - pointing at its own
                // operator token - rather than every node after the first sharing the
                // position of the chain's very first postfix operator.
                const auto location = parser.current_location();

                if (parser.check(TokenKind::LParen)) {
                    parser.advance();

                    std::vector<Expr> args;
                    while (!parser.check(TokenKind::RParen) && !parser.at_end()) {
                        const LoopProgressGuard progress_guard(parser);
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
                        skip_semicolons(parser);
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

                } else if (parser.check(TokenKind::Dot) && parser.check_next(TokenKind::Star)) {
                    const auto deref_location = parser.current_location();
                    parser.advance(); // '.'
                    parser.advance(); // '*'

                    expr = make_expr(UnaryExpr{
                        .op = UnaryOp::Deref,
                        .operand = std::move(expr),
                        .location = deref_location,
                    });

                } else if (parser.check(TokenKind::Dot)) {
                    parser.advance();
                    const auto member_name = parser.expect_identifier();

                    // A '{.field = ...' payload and a dotted-identifier base (e.g. `Type` or
                    // `mod.Type`) together mean a qualified tagged-variant constructor. Anything
                    // else — notably 'switch h.kind { .arm: ... }' — is an ordinary member
                    // access, and the '{' belongs to whatever encloses it.
                    if (looks_like_variant_payload(parser)) {
                        if (auto type_path = named_type_from_expr(expr)) {
                            const auto brace_loc = parser.current_location();
                            parser.advance(); // consume '{'
                            expr = std::make_unique<TaggedVariantExpr>(TaggedVariantExpr{
                                .type_path = std::move(type_path),
                                .variant_name = member_name,
                                .payload = StructExpr{.fields = parse_dot_field_list(parser), .location = brace_loc},
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

            while (parser.check(TokenKind::Ampersand)) {
                const auto location = parser.current_location();

                parser.advance();

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

            // 'Tilde' here is unambiguously infix (bitwise-xor precedence, and — between two
            // bitsets — symmetric difference): once 'lhs' has been fully parsed as an operand,
            // a '~' token can only be an operator, never a prefix unary. Prefix '~' is still
            // handled separately by parse_unary, which only ever runs at the START of an
            // operand parse (no lhs yet). Both desugar to the same BinaryOp::BitwiseXor node,
            // so 'a ~ b' and 'a ^ b' are equivalent — see UnaryOp::BitwiseNot for the prefix form.
            while (parser.check(TokenKind::Caret) || parser.check(TokenKind::Tilde)) {
                const auto location = parser.current_location();
                parser.advance();

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

            while (parser.check(TokenKind::Pipe)) {
                const auto location = parser.current_location();

                parser.advance();

                lhs = make_expr(BinaryExpr{
                    .op = BinaryOp::BitwiseOr,
                    .lhs = std::move(lhs),
                    .rhs = parse_bitwise_xor(parser),
                    .location = location,
                });
            }

            return lhs;
        }

        // 'expr in expr' — bitset membership testing. Binds looser than every
        // arithmetic/bitwise/comparison operator, tighter than '&&'/'||'. Non-chaining
        // ('if', not 'while'): the result of 'in' is Bool, not a bitset, so 'a in b in c'
        // has no sensible left-associative reading. Distinct from the 'for pattern in
        // iterable' statement form, which consumes 'in' directly via expect(KwIn) on a
        // restricted loop-pattern path and never reaches general expression parsing.
        auto parse_in_expr(Parser &parser) -> Expr {
            auto lhs = parse_bitwise_or(parser);

            const auto location = parser.current_location();
            if (parser.match(TokenKind::KwIn)) {
                lhs = make_expr(BinaryExpr{
                    .op = BinaryOp::In,
                    .lhs = std::move(lhs),
                    .rhs = parse_bitwise_or(parser),
                    .location = location,
                });
            }

            return lhs;
        }

        auto parse_logical_and(Parser &parser) -> Expr {
            auto lhs = parse_in_expr(parser);

            while (parser.check(TokenKind::AmpAmp)) {
                const auto location = parser.current_location();

                parser.advance();

                lhs = make_expr(BinaryExpr{
                    .op = BinaryOp::LogicalAnd,
                    .lhs = std::move(lhs),
                    .rhs = parse_in_expr(parser),
                    .location = location,
                });
            }

            return lhs;
        }

        auto parse_logical_or(Parser &parser) -> Expr {
            auto lhs = parse_logical_and(parser);

            while (parser.check(TokenKind::PipePipe)) {
                const auto location = parser.current_location();

                parser.advance();

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

            const auto location = parser.current_location();
            if (parser.match(TokenKind::Question)) {
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

        // 'then_val when cond else else_val' — binds looser than all binary operators
        // (including ternary '?:') but tighter than assignment; sits between
        // parse_assign_expr and parse_ternary_expr in the precedence chain. 'then_expr'
        // and 'condition' parse at the ternary tier; 'else_expr' is a full parse_expr
        // (right-recursive), mirroring TernaryExpr's own else-branch convention so
        // 'z = a when b else c' parses as 'z = (a when b else c)' and
        // 'a when b else c when d else e' parses as 'a when b else (c when d else e)'.
        // A bare, unparenthesized nested 'when...else' in condition position does not
        // parse (needs parens) — deliberate, avoids ambiguity with the outer 'when'.
        auto parse_when_expr(Parser &parser) -> Expr {
            auto expr = parse_ternary_expr(parser);

            const auto location = parser.current_location();
            if (parser.match(TokenKind::KwWhen)) {
                auto condition = parse_ternary_expr(parser);
                parser.expect(TokenKind::KwElse, "'else'");
                auto else_expr = parse_expr(parser);

                return make_expr(WhenExpr{
                    .condition = std::move(condition),
                    .then_expr = std::move(expr),
                    .else_expr = std::move(else_expr),
                    .location = location,
                });
            }

            return expr;
        }

        auto parse_assign_expr(Parser &parser) -> Expr {
            auto expr = parse_when_expr(parser);

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
                case TokenKind::TildeEqual:      return AssignOp::ToggleAssign;
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

        // 'import("path")' as a standalone module-scope declaration — reuses
        // parse_import_expr for the shared '"import" "(" STRING ")"' grammar rather
        // than duplicating it; only the wrapping (a BareImportDecl, not an ImportExpr
        // handed back to a const initializer) differs.
        auto parse_bare_import_decl(Parser &parser) -> BareImportDecl {
            const auto location = parser.current_location();
            auto expr = parse_import_expr(parser);

            return BareImportDecl{
                .path = std::get<ImportExpr>(expr).module_name,
                .location = location,
            };
        }

        // See Parser::comma_terminates_stmt (ast_parser.hpp) for what the flag means and why.
        class ScopedCommaTerminatesStmt {
          public:
            ScopedCommaTerminatesStmt(Parser &parser, const bool value)
                : parser_(parser), saved_(parser.comma_terminates_stmt) {
                parser_.comma_terminates_stmt = value;
            }
            ~ScopedCommaTerminatesStmt() { parser_.comma_terminates_stmt = saved_; }
            ScopedCommaTerminatesStmt(const ScopedCommaTerminatesStmt &) = delete;
            auto operator=(const ScopedCommaTerminatesStmt &) -> ScopedCommaTerminatesStmt & = delete;

          private:
            Parser &parser_;
            bool saved_;
        };

        auto parse_block_stmt(Parser &parser) -> Stmt {
            const auto location = parser.current_location();

            const ScopedCommaTerminatesStmt comma_scope(parser, false);
            parser.expect(TokenKind::LBrace, "'{'");

            std::vector<Stmt> stmts;
            while (true) {
                skip_semicolons(parser);
                if (parser.check(TokenKind::RBrace) || parser.at_end()) {
                    break;
                }

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

        // The common core of a variable declaration -- '(mut|const) name' followed by
        // ': type [= init]' or ':= init' -- shared by the statement form and the
        // module-scope form, which differ only in the group-decl branch (statement
        // only) and 'is_pub' (module scope only).
        struct VarDeclParts {
            bool is_mut = false;
            std::string name;
            std::optional<Type> type;
            std::optional<Expr> init;
            SourceLocation location;
            // True only when parsing stopped at a comma IMMEDIATELY after the name — the
            // group-declaration header shape. The caller must branch on this, not on
            // "is the current token a comma": a fully-parsed declaration whose
            // initializer happens to end right before a comma ('mut a := 7, b := f()',
            // invalid grammar) otherwise silently misparses as a group declaration,
            // discarding the first initializer.
            bool is_group_header = false;
        };

        // 'allow_group': the statement form supports 'a, b := f()' group declarations;
        // when a ',' follows the first name we return with only the header fields
        // filled so the caller can hand off to the group parser. The module-scope form
        // has no group syntax, so a ',' there falls through to the normal
        // "expected ':' or ':='" error.
        auto parse_var_decl_parts(Parser &parser, const bool allow_group) -> VarDeclParts {
            VarDeclParts parts;
            parts.location = parser.current_location();

            parts.is_mut = parser.match(TokenKind::KwMut);
            if (!parts.is_mut) {
                parser.expect(TokenKind::KwConst, "'const' or 'mut'");
            }

            parts.name = parser.expect_identifier();
            if (allow_group && parser.check(TokenKind::Comma)) {
                parts.is_group_header = true;
                return parts;
            }

            if (parser.match(TokenKind::Colon)) {
                parts.type = parse_type(parser);
            }

            if (parts.type.has_value()) {
                if (parser.match(TokenKind::Equal)) {
                    parts.init = parse_expr(parser, false);
                }
            } else {
                parser.expect(TokenKind::ColonEqual, "':' or ':='");
                parts.init = parse_expr(parser, !parts.is_mut);
            }

            if (!parts.is_mut && parts.init == std::nullopt) {
                parser.report_error(parser.current_location(), "'const' requires an initializer");
            }

            return parts;
        }

        auto parse_var_decl_group_stmt(Parser &parser, const bool is_mut, SourceLocation location, std::string first_name) -> Stmt {
            std::vector<std::string> names;

            names.push_back(std::move(first_name));
            while (parser.match(TokenKind::Comma) && !parser.at_end()) {
                // A missing name is an error, not a second spelling of the '_' discard.
                // Both of these gaps used to be accepted silently and treated exactly like
                // '_' by sema, which meant 'a,, b := f()' and 'a, := f()' -- far more often
                // a stray keystroke than an intentional discard -- compiled without comment.
                // '_' is the documented discard (grammar.md note 7) and stays required.
                if (parser.check(TokenKind::ColonEqual) || parser.check(TokenKind::Comma)) {
                    parser.report_error(parser.current_location(),
                        "expected a name or '_' in a declaration group; use '_' to discard a value");
                    names.emplace_back();
                    if (parser.check(TokenKind::ColonEqual)) {
                        break;
                    }
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
            auto parts = parse_var_decl_parts(parser, /*allow_group=*/true);
            if (parts.is_group_header) {
                return parse_var_decl_group_stmt(parser, parts.is_mut, parts.location, std::move(parts.name));
            }

            return VarDeclStmt{
                .is_mut = parts.is_mut,
                .name = std::move(parts.name),
                .type = std::move(parts.type),
                .init = std::move(parts.init),
                .location = parts.location,
            };
        }

        // 'when cond { ... } [else (when ... | { ... })]' — a compile-time conditional
        // statement. The then-branch (and each block in the else-chain) is forced to be a
        // literal block (mirrors WhileStmt forcing parse_block_stmt for its body), unlike
        // IfStmt's 'then_stmt' which accepts any statement. Legality/foldability of
        // 'condition' as a compile-time constant is enforced in sema, not here.
        // 'when cond BODY [else (when cond BODY ... | BODY)]' — the compile-time conditional,
        // in both the forms it takes: WhenStmt over a block of statements, and WhenDecl over a
        // list of module-scope declarations.
        //
        // The two differ only in what a BODY is and which node they build. Sharing the chain
        // itself is what keeps 'else when' meaning the same thing in both — an inconsistency
        // there would be a difference in the language, not just in the parser.
        template <typename Node, typename Body>
        auto parse_when_chain(Parser &parser, auto parse_body) -> std::unique_ptr<Node> {
            const auto location = parser.current_location();
            parser.expect(TokenKind::KwWhen, "'when'");

            auto condition = parse_expr(parser);
            auto then_body = parse_body(parser);

            // 'else when' recurses (chaining another condition); a bare 'else' takes a body.
            std::optional<std::variant<Body, std::unique_ptr<Node>>> else_branch = std::nullopt;
            if (parser.match(TokenKind::KwElse)) {
                if (parser.check(TokenKind::KwWhen)) {
                    else_branch = parse_when_chain<Node, Body>(parser, parse_body);
                } else {
                    else_branch = parse_body(parser);
                }
            }

            Node node;
            node.condition = std::move(condition);
            node.else_branch = std::move(else_branch);
            node.location = location;
            // The one place the two nodes are not interchangeable: they name the then-body
            // field for what it holds.
            if constexpr (std::is_same_v<Node, WhenStmt>) {
                node.then_block = std::move(then_body);
            } else {
                node.then_decls = std::move(then_body);
            }
            return std::make_unique<Node>(std::move(node));
        }

        auto parse_when_stmt(Parser &parser) -> std::unique_ptr<WhenStmt> {
            return parse_when_chain<WhenStmt, BlockStmt>(parser, [](Parser &p) {
                return std::move(*std::get<std::unique_ptr<BlockStmt>>(parse_block_stmt(p)));
            });
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

        // The shared value list of 'return' and 'return_ok': zero or more comma-separated
        // expressions. With no values at all, 'out_possible_asi_gotcha' records whether the
        // next token suggests the intended value was severed by a virtual ';'.
        auto parse_return_value_list(Parser &parser, bool &out_possible_asi_gotcha) -> std::vector<Expr> {
            std::vector<Expr> values;
            out_possible_asi_gotcha = false;
            if (can_start_expr(parser.current().kind)) {
                values.push_back(parse_expr(parser));

                while (!parser.comma_terminates_stmt && parser.match(TokenKind::Comma)) {
                    values.push_back(parse_expr(parser));
                }
            } else {
                out_possible_asi_gotcha = is_asi_gotcha_candidate(parser);
            }
            return values;
        }

        auto parse_return_stmt(Parser &parser) -> ReturnStmt {
            const auto location = parser.current_location();

            parser.expect(TokenKind::KwReturn, "'return'");

            bool possible_asi_gotcha = false;
            auto values = parse_return_value_list(parser, possible_asi_gotcha);

            return ReturnStmt{
                .return_values = std::move(values),
                .location = location,
                .possible_asi_gotcha = possible_asi_gotcha,
            };
        }

        auto parse_return_err_stmt(Parser &parser) -> ReturnErrStmt {
            const auto location = parser.current_location();

            parser.expect(TokenKind::KwReturnErr, "'return_err'");

            auto value = parse_expr(parser);

            return ReturnErrStmt{
                .error_value = std::move(value),
                .location = location,
            };
        }

        auto parse_return_ok_stmt(Parser &parser) -> ReturnOkStmt {
            const auto location = parser.current_location();

            parser.expect(TokenKind::KwReturnOk, "'return_ok'");

            bool possible_asi_gotcha = false;
            auto values = parse_return_value_list(parser, possible_asi_gotcha);

            return ReturnOkStmt{
                .return_values = std::move(values),
                .location = location,
                .possible_asi_gotcha = possible_asi_gotcha,
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
        // One parsed parameter, before the caller narrows it to whichever Param struct its own
        // declaration uses (FunctionDecl::Param, ImplDecl::Function::Param, TraitType::Param).
        struct ParsedParam {
            bool is_mut = false;
            std::string name;
            std::optional<Type> type;
            std::optional<Expr> default_value;
            bool is_variadic = false;
            SourceLocation location;
        };

        struct ParamPolicy {
            // Trait methods take no 'mut' prefix. False means 'mut' is not consumed at all, so
            // writing it produces the ordinary "expected identifier" rather than being accepted
            // and silently ignored.
            bool allow_mut = true;
            // Empty means variadics are allowed here. Otherwise this is the diagnostic to
            // report for a '...', after which it is consumed and parsing continues — the
            // parameter is still parsed, so one bad '...' does not cascade.
            std::string_view variadic_rejection;
        };

        // Parses ONE parameter: 'mut'? IDENT ( ':' '...'? type ( '=' default )? | ':=' default ).
        //
        // Shared by the three parameter lists in the grammar — free functions, impl methods and
        // trait methods. They differ only in the two ParamPolicy knobs above and in how the
        // list around them is framed (a leading comma before each parameter, for the two that
        // follow 'self'); everything else was three copies, and the trait copy had already
        // drifted on variadic position tracking.
        auto parse_one_param(Parser &parser, const ParamPolicy &policy, bool &seen_variadic) -> ParsedParam {
            ParsedParam param;
            param.location = parser.current_location();
            param.is_mut = policy.allow_mut && parser.match(TokenKind::KwMut);
            param.name = parser.expect_identifier();

            // Before branching on ':' vs ':=': a ':='-form parameter after a variadic one
            // ('fn f(xs: ...i32, y := 0)') used to sail through because the check lived
            // inside the ':' branch only.
            if (policy.variadic_rejection.empty() && seen_variadic) {
                parser.report_error(param.location, "'...' variadic parameter must be the last parameter in the parameter list");
            }

            if (parser.match(TokenKind::Colon)) {
                if (parser.check(TokenKind::DotDotDot)) {
                    if (policy.variadic_rejection.empty()) {
                        parser.advance();
                        param.is_variadic = true;
                        seen_variadic = true;
                    } else {
                        parser.report_error(param.location, std::string(policy.variadic_rejection));
                        parser.advance();
                    }
                }

                param.type = parse_type(parser);

                // A variadic parameter cannot also have a default: it already absorbs "the rest",
                // which is empty when nothing is passed.
                if (!param.is_variadic && parser.match(TokenKind::Equal)) {
                    param.default_value = parse_expr(parser);
                }
            } else {
                parser.expect(TokenKind::ColonEqual, "':' or ':='");
                param.default_value = parse_expr(parser);
            }

            return param;
        }

        auto parse_function_params(Parser &parser) -> std::vector<FunctionDecl::Param> {
            parser.expect(TokenKind::LParen, "'('");

            std::vector<FunctionDecl::Param> params;
            bool seen_variadic = false;

            while (!parser.check(TokenKind::RParen) && !parser.at_end()) {
                const LoopProgressGuard progress_guard(parser);

                if (parser.check(TokenKind::DotDotDot)) {
                    parser.report_error(parser.current_location(),
                        "variadic parameters ('...') are only allowed on 'ext fn' declarations, not 'fn'; "
                        "to declare a native variadic parameter, use 'name: ...T' with an element type");
                    parser.advance();
                    break;
                }

                auto param = parse_one_param(parser, ParamPolicy{}, seen_variadic);
                params.push_back({
                    .is_mut = param.is_mut,
                    .name = std::move(param.name),
                    .type = std::move(param.type),
                    .default_value = std::move(param.default_value),
                    .is_variadic = param.is_variadic,
                    .location = param.location,
                });

                skip_semicolons(parser);
                if (!parser.check(TokenKind::RParen)) {
                    parser.expect(TokenKind::Comma, "','");
                }
            }

            parser.expect(TokenKind::RParen, "')'");

            return params;
        }

        struct FunctionReturnTypes {
            std::vector<Type> types;
            std::vector<std::string> names; // parallel to types; "" = unnamed
        };

        // Parses one return slot's type, consuming an optional leading '?' ('?E' — an error
        // a caller may leave unhandled) and wrapping the result in an OptionalErrorType.
        // Whether a '?' is on the LAST slot can't be decided while parsing slot i, so each
        // marker's location is recorded here and checked by report_misplaced_optional_errors
        // once the whole list is known. Shared by parse_function_return_types (free fns,
        // impl methods, trait methods) and parse_function_type (fn-pointer types) so '?'
        // behaves identically in every return-type position.
        auto parse_return_slot_type(Parser &parser, std::vector<std::optional<SourceLocation>> &question_locations) -> Type {
            if (!parser.check(TokenKind::Question)) {
                question_locations.emplace_back();
                return parse_type(parser);
            }

            const auto location = parser.current_location();
            parser.advance();
            question_locations.push_back(location);
            return std::make_unique<OptionalErrorType>(OptionalErrorType{
                .inner = parse_type(parser),
                .location = location,
            });
        }

        void report_misplaced_optional_errors(Parser &parser, const std::vector<std::optional<SourceLocation>> &question_locations) {
            for (size_t i = 0; i + 1 < question_locations.size(); ++i) {
                if (question_locations[i]) {
                    parser.report_error(*question_locations[i],
                                        "'?' may only mark the LAST return type; it marks an error the caller may leave unhandled");
                }
            }
        }

        // -> T   or   -> (T1, T2, ...)  — each entry may optionally be prefixed with
        // 'name:' purely for documentation (e.g. LSP hover / self-documenting code);
        // never used for matching or type identity, exactly like parse_function_type's
        // 'param_name' convention. Safe with 1 token of lookahead for the same reason
        // param names are: parse_type always starts with an Identifier/keyword and
        // never itself contains a top-level ':' immediately following a leading
        // identifier.
        // One optional '->' return-type clause: '-> T' or '-> (T1, T2, ...)', with
        // 'parse_slot' parsing a single slot (a bare type for fn-pointer types, an
        // optionally named type for fn declarations). Shared by
        // parse_function_return_types and parse_function_type so the list grammar —
        // virtual-';' skipping, comma placement, recovery — cannot drift between them.
        template <typename ParseSlot>
        void parse_return_type_clause(Parser &parser, ParseSlot &&parse_slot) {
            if (!parser.match(TokenKind::Arrow)) {
                return;
            }
            if (parser.match(TokenKind::LParen)) {
                // Multi-return: -> (T1, T2, ...)
                while (!parser.check(TokenKind::RParen) && !parser.at_end()) {
                    const LoopProgressGuard progress_guard(parser);
                    parse_slot();
                    skip_semicolons(parser);
                    if (!parser.check(TokenKind::RParen)) {
                        parser.expect(TokenKind::Comma, "','");
                    }
                }
                parser.expect(TokenKind::RParen, "')'");
            } else {
                // Single return: -> T
                parse_slot();
            }
        }

        auto parse_function_return_types(Parser &parser) -> FunctionReturnTypes {
            FunctionReturnTypes result;
            std::vector<std::optional<SourceLocation>> question_locations;

            auto parse_one = [&] {
                std::string name;
                if (parser.check(TokenKind::Identifier) && parser.check_next(TokenKind::Colon)) {
                    name = parser.expect_identifier();
                    parser.expect(TokenKind::Colon, "':'");
                }
                result.names.push_back(std::move(name));
                result.types.push_back(parse_return_slot_type(parser, question_locations));
            };

            parse_return_type_clause(parser, parse_one);

            report_misplaced_optional_errors(parser, question_locations);
            return result;
        }

        auto parse_trait_method_decl(Parser &parser) -> TraitType::Method {
            const auto location = parser.current_location();

            if (parser.check(TokenKind::KwPub)) {
                parser.report_error(
                    location,
                    "'pub' is not allowed on trait method declarations; the trait's own visibility governs");
                parser.advance();
            }

            parser.expect(TokenKind::KwFn, "'fn'");
            auto name = parser.expect_identifier();

            parser.expect(TokenKind::LParen, "'('");

            bool is_mut_self = false;
            if (parser.check(TokenKind::KwMut)) {
                parser.advance();
                is_mut_self = true;
            }

            const auto self_location = parser.current_location();
            const auto self_name = parser.expect_identifier();
            if (self_name != "self") {
                parser.report_error(location, "first parameter of trait method must be 'self' or 'mut self'");
            }

            // Non-self params: 'IDENT : type' only — no 'mut' prefix, no variadics. Both
            // restrictions are the ParamPolicy below; the parameter grammar itself is shared.
            constexpr ParamPolicy trait_policy{
                .allow_mut = false,
                .variadic_rejection = "variadic parameters are not allowed in trait method declarations",
            };
            std::vector<TraitType::Param> params;
            bool seen_variadic = false; // unused: trait_policy rejects '...' outright
            while (!parser.check(TokenKind::RParen) && !parser.at_end()) {
                const LoopProgressGuard progress_guard(parser);

                parser.expect(TokenKind::Comma, "','");
                auto param = parse_one_param(parser, trait_policy, seen_variadic);
                params.push_back(TraitType::Param{
                    .name = std::move(param.name),
                    .type = std::move(param.type),
                    .default_value = std::move(param.default_value),
                    .location = param.location,
                });
            }

            parser.expect(TokenKind::RParen, "')'");
            auto return_types = parse_function_return_types(parser);

            if (parser.check(TokenKind::LBrace)) {
                parser.report_error(parser.current_location(), "trait method declarations cannot have a body");
                parse_stmt(parser); // consume and discard so parsing can continue
            }

            return TraitType::Method{
                .name = std::move(name),
                .is_mut_self = is_mut_self,
                .params = std::move(params),
                .return_types = std::move(return_types.types),
                .return_names = std::move(return_types.names),
                .location = location,
                .self_location = self_location,
            };
        }

        auto parse_trait_type(Parser &parser) -> Type {
            const auto location = parser.current_location();

            parser.expect(TokenKind::KwTrait, "'trait'");

            // Optional 'trait(A, B, ...)' composition list. 'allow_generic_args=false' matches
            // 'impl TRAIT for TYPE''s two named_type operands above — traits are never generic.
            std::vector<NamedType> composed_traits;
            if (parser.match(TokenKind::LParen)) {
                composed_traits.push_back(parse_named_type(parser, /*allow_generic_args=*/false));
                while (parser.match(TokenKind::Comma)) {
                    const LoopProgressGuard progress_guard(parser);
                    composed_traits.push_back(parse_named_type(parser, /*allow_generic_args=*/false));
                }
                parser.expect(TokenKind::RParen, "')'");
            }

            // A composition list with no brace body is legal on its own; a bare 'trait' (no
            // list, no brace) falls through to expect(LBrace) below and reports its usual error.
            if (!composed_traits.empty() && !parser.check(TokenKind::LBrace)) {
                return std::make_unique<TraitType>(TraitType{
                    .methods = {},
                    .composed_traits = std::move(composed_traits),
                    .location = location,
                });
            }

            parser.expect(TokenKind::LBrace, "'{'");

            std::vector<TraitType::Method> methods;
            while (true) {
                skip_semicolons(parser);
                if (parser.check(TokenKind::RBrace) || parser.at_end() || parser.has_reached_max_errors()) {
                    break;
                }

                const LoopProgressGuard progress_guard(parser);

                methods.push_back(parse_trait_method_decl(parser));
            }

            parser.expect(TokenKind::RBrace, "'}'");

            if (methods.empty()) {
                parser.report_error(location, "trait must declare at least one method");
            }

            return std::make_unique<TraitType>(TraitType{
                .methods = std::move(methods),
                .composed_traits = std::move(composed_traits),
                .location = location,
            });
        }

        // fn(ParamType, ...) -> RetType  or  fn(ParamType, ...) -> (R1, R2)
        auto parse_function_type(Parser &parser) -> Type {
            const auto location = parser.current_location();

            parser.expect(TokenKind::KwFn, "'fn'");
            parser.expect(TokenKind::LParen, "'('");

            std::vector<Type> param_types;
            std::vector<std::string> param_names; // parallel to param_types; "" = unnamed
            bool is_variadic = false;

            while (!parser.check(TokenKind::RParen) && !parser.at_end()) {
                const LoopProgressGuard progress_guard(parser);
                if (parser.check(TokenKind::DotDotDot)) {
                    parser.advance();
                    is_variadic = true;
                    break;
                }
                // Optional 'name:' prefix, purely for documentation (e.g. LSP hover);
                // never used for matching or type identity. Safe with 1 token of
                // lookahead because parse_named_type always starts with an Identifier
                // and never itself contains a top-level ':' immediately following the
                // leading identifier (qualified names use '.', not ':').
                std::string param_name;
                if (parser.check(TokenKind::Identifier) && parser.check_next(TokenKind::Colon)) {
                    param_name = parser.expect_identifier();
                    parser.expect(TokenKind::Colon, "':'");
                }
                param_names.push_back(std::move(param_name));
                param_types.push_back(parse_type(parser));
                skip_semicolons(parser);
                if (!parser.check(TokenKind::RParen)) {
                    parser.expect(TokenKind::Comma, "','");
                }
            }
            parser.expect(TokenKind::RParen, "')'");

            std::vector<Type> return_types;
            std::vector<std::optional<SourceLocation>> question_locations;
            parse_return_type_clause(parser, [&] {
                return_types.push_back(parse_return_slot_type(parser, question_locations));
            });
            report_misplaced_optional_errors(parser, question_locations);

            return std::make_unique<FunctionType>(FunctionType{
                .param_types = std::move(param_types),
                .param_names = std::move(param_names),
                .return_types = std::move(return_types),
                .is_variadic = is_variadic,
                .location = location,
            });
        }

        auto parse_function_decl(Parser &parser, const bool is_pub, std::vector<Attribute> attributes) -> FunctionDecl {
            const auto location = parser.current_location();

            parser.expect(TokenKind::KwFn, "'fn'");

            auto fn_name = parser.expect_identifier();

            std::vector<GenericParam> fn_generic_params;
            if (parser.check(TokenKind::LBracket)) {
                fn_generic_params = parse_generic_params(parser);
            }

            auto fn_params = parse_function_params(parser);
            auto fn_return_types = parse_function_return_types(parser);
            auto fn_body = parse_stmt(parser);

            return FunctionDecl{
                .is_pub = is_pub,
                .attributes = std::move(attributes),
                .name = fn_name,
                .generic_params = std::move(fn_generic_params),
                .params = std::move(fn_params),
                .return_types = std::move(fn_return_types.types),
                .return_names = std::move(fn_return_types.names),
                .body = std::move(fn_body),
                .location = location,
            };
        }

        auto parse_ext_function_params(Parser &parser, bool &out_is_variadic) -> std::vector<ExtFunctionDecl::Param> {
            parser.expect(TokenKind::LParen, "'('");

            std::vector<ExtFunctionDecl::Param> params;
            out_is_variadic = false;

            while (!parser.check(TokenKind::RParen) && !parser.at_end()) {
                const LoopProgressGuard progress_guard(parser);

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
                auto param_type = parse_type(parser);

                std::optional<Expr> default_value;
                if (parser.match(TokenKind::Equal)) {
                    default_value = parse_expr(parser);
                }

                params.push_back({
                    .name = param_name,
                    .type = std::move(param_type),
                    .default_value = std::move(default_value),
                    .location = param_location,
                });

                skip_semicolons(parser);
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
            auto parts = parse_var_decl_parts(parser, /*allow_group=*/false);

            return VarDecl{
                .is_pub = is_pub,
                .is_mut = parts.is_mut,
                .name = std::move(parts.name),
                .type = std::move(parts.type),
                .init = std::move(parts.init),
                .location = parts.location,
            };
        }

        auto parse_macro_params(Parser &parser) -> std::vector<MacroDecl::Param> {
            std::vector<MacroDecl::Param> params;

            parser.expect(TokenKind::LParen, "'('");

            while (!parser.check(TokenKind::RParen) && !parser.at_end()) {
                const LoopProgressGuard progress_guard(parser);

                const auto param_location = parser.current_location();
                const auto param_name = parser.expect_identifier();

                parser.expect(TokenKind::Colon, "':'");

                params.push_back({
                    .name = param_name,
                    .type = parse_type(parser),
                    .location = param_location,
                });

                skip_semicolons(parser);
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

            std::optional<Type> result_type;
            if (parser.match(TokenKind::Colon)) {
                result_type = parse_type(parser);
            }

            parser.expect(TokenKind::Arrow, "'->'");

            return MacroDecl{
                .is_pub = is_pub,
                .name = name,
                .params = std::move(params),
                .result_type = std::move(result_type),
                .expr_template = parse_expr(parser),
                .location = location,
            };
        }

        auto parse_type_decl(Parser &parser, const bool is_pub) -> TypeDecl {
            const auto location = parser.current_location();

            parser.expect(TokenKind::KwType, "'type'");

            const auto type_name = parser.expect_identifier();

            std::vector<GenericParam> type_generic_params;
            if (parser.check(TokenKind::LBracket)) {
                type_generic_params = parse_generic_params(parser);
            }

            parser.expect(TokenKind::Equal, "'='");

            return TypeDecl{
                .is_pub = is_pub,
                .name = type_name,
                .generic_params = std::move(type_generic_params),
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

            std::optional<Expr> size;
            if (!parser.match(TokenKind::Question)) {
                size = parse_expr(parser);
            }

            parser.expect(TokenKind::RBracket, "']'");
            return std::make_unique<ArrayType>(ArrayType{
                .base_type = parse_type(parser),
                .size = std::move(size),
                .location = location,
            });
        }
    }

    auto skip_semicolons(Parser &parser) -> void {
        while (parser.match(TokenKind::Semicolon)) {
        }
    }

    auto find_leaf_import(const Expr &expr) -> const ImportExpr * {
        if (const auto *import_expr = std::get_if<ImportExpr>(&expr)) {
            return import_expr;
        }
        if (const auto *member = std::get_if<std::unique_ptr<MemberExpr>>(&expr)) {
            return find_leaf_import((*member)->object);
        }
        return nullptr;
    }

    auto parse_type(Parser &parser) -> Type {
        const auto location = parser.current_location();

        // '?' reaching general type position means it was written somewhere a return slot
        // isn't (a parameter, field, variable annotation, alias, ...). Only return-slot
        // parsing consumes it (see parse_return_slot_type), so diagnose it specifically
        // instead of letting it fall through to the generic "expected type" error below,
        // then recover by parsing the type it prefixed.
        if (parser.check(TokenKind::Question)) {
            parser.report_error(location, "'?' is only allowed on a function's last return type (it marks an error the caller may leave unhandled)");
            parser.advance();
            return parse_type(parser);
        }

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

        if (parser.check(TokenKind::KwError)) {
            return parse_error_type(parser);
        }

        if (parser.check(TokenKind::KwBitset)) {
            return parse_bitset_type(parser);
        }

        if (parser.check(TokenKind::KwFn)) {
            return parse_function_type(parser);
        }

        if (parser.check(TokenKind::KwTrait)) {
            return parse_trait_type(parser);
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
            if (!allow_import) {
                parser.report_error(parser.current_location(), "'import()' can only be used to initialize a 'const' declaration with no explicit type");
                return parse_import_expr(parser);
            }

            // import(...) is not a general expression (see find_leaf_import's comment) -
            // it never enters parse_postfix - but a trailing chain of plain '.field'
            // accesses is allowed here so `import("...").field` can be written without
            // first binding the import to its own const.
            Expr expr = parse_import_expr(parser);
            while (parser.check(TokenKind::Dot)) {
                const auto location = parser.current_location();
                parser.advance();
                const auto member_name = parser.expect_identifier();
                expr = make_expr(MemberExpr{
                    .object = std::move(expr),
                    .member = member_name,
                    .location = location,
                });
            }
            return expr;
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
            const LoopProgressGuard progress_guard(parser);
            const auto arm_location = parser.current_location();

            auto arm_pattern = parse_match_arm_pattern(parser);

            parser.expect(TokenKind::Colon, "':'");
            // An unbraced arm body ends at the arm-separator comma; a braced one resets the
            // flag itself (parse_block_stmt), keeping 'return a, b' legal inside it.
            const ScopedCommaTerminatesStmt comma_scope(parser, !parser.check(TokenKind::LBrace));
            auto body = parse_stmt(parser);

            arms.push_back(SwitchStmt::Arm{
                .pattern = std::move(arm_pattern),
                .body = std::move(body),
                .location = arm_location,
            });

            // See the identical comment in the match-expr arm loop above: a block-bodied arm
            // with no trailing comma picks up a virtual semicolon before the closing '}'.
            skip_semicolons(parser);
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

    // 'asm { ... }' — the main lexer already captured the raw text between the braces as a
    // single 'AsmBlock' token (see lexer.cpp's lex_asm_block), so parsing here is just
    // handing that raw text off to the standalone asm_lexer/asm_parser pair, never
    // reparsing it with the main Mirage grammar.
    auto parse_asm_stmt(Parser &parser) -> std::unique_ptr<AsmStmt> {
        parser.expect(TokenKind::KwAsm, "'asm'");
        const auto block_tok = parser.expect(TokenKind::AsmBlock, "asm block body");
        if (block_tok.kind != TokenKind::AsmBlock) {
            // Mirrors the AsmExpr guard in parse_primary: expect() reported without
            // advancing, so the current token's lexeme is not an asm body.
            return std::make_unique<AsmStmt>();
        }

        auto asm_tokens = asm_lexer::tokenize(block_tok.lexeme, block_tok.location, parser.diagnostics());
        auto stmt = asm_parser::parse(asm_tokens, parser.diagnostics());

        return std::make_unique<AsmStmt>(std::move(stmt));
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

        if (parser.check(TokenKind::KwWhen)) {
            return parse_when_stmt(parser);
        }

        // bare 'import(...)' is a module-scope declaration, not a statement — intercepted
        // here (before the parse_expr_stmt fallthrough) so it gets this precise,
        // actionable diagnostic instead of parse_expr's generic "'import()' can only be
        // used to initialize a 'const'..." message (which fires for every OTHER illegal
        // position, e.g. a call argument). Recovers by still parsing the ImportExpr into
        // an ordinary ExprStmt (ImportExpr remains a legal Expr alternative) so parsing
        // doesn't desync. This also covers a function-scope 'when {}' block for free,
        // since WhenStmt's branches are parsed through this same per-statement loop.
        if (parser.check(TokenKind::KwImport)) {
            const auto location = parser.current_location();
            parser.report_error(location,
                "bare 'import(...)' is a module-scope declaration, not a statement.\n"
                "       To use symbols from another module inline, bind it:\n"
                "       'const mod := import(\"path\")'");
            return ExprStmt{.expr = parse_import_expr(parser), .location = location};
        }

        // '#link(...)' parses successfully here (unlike anywhere else a runtime
        // statement is illegal) purely so sema can reject it with a precise
        // "module scope only" diagnostic. '$option(...)' is NOT special-cased here —
        // it falls through to parse_expr_stmt below, which reaches parse_primary's
        // own '$' handling for the option-expression form.
        if (peek_link_decl(parser)) {
            return parse_link_decl(parser);
        }

        // '#error(...)'/'#warn(...)' parse here for the same reason '#link' does just
        // above — so sema can reject them inside a function body with a precise "module
        // scope only" diagnostic instead of a raw parse error.
        if (const auto directive_kind = peek_diagnostic_directive_kind(parser)) {
            return parse_diagnostic_decl(parser, *directive_kind);
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

        if (parser.check(TokenKind::KwReturnErr)) {
            return parse_return_err_stmt(parser);
        }

        if (parser.check(TokenKind::KwReturnOk)) {
            return parse_return_ok_stmt(parser);
        }

        if (parser.check(TokenKind::KwDefer)) {
            const auto location = parser.current_location();
            parser.advance();
            return std::make_unique<DeferStmt>(DeferStmt{
                .body = parse_stmt(parser),
                .location = location,
            });
        }

        // Only the statement form ('asm { ... }') dispatches here directly. The expression
        // form ('asm -> reg [: type] { ... }') falls through to parse_expr_stmt below, which
        // reaches parse_primary's own 'asm ->' handling — it's a normal expression, usable
        // anywhere one is legal, not just as a bare statement.
        if (parser.check(TokenKind::KwAsm) && !parser.check_next(TokenKind::Arrow)) {
            return parse_asm_stmt(parser);
        }

        return parse_expr_stmt(parser);
    }

    auto parse_impl_method(Parser &parser, const bool allow_pub) -> ImplDecl::Function {
        const auto location = parser.current_location();

        // Parsed permissively here (any of the five known attribute names) exactly like a
        // free function's attribute clause — sema rejects '@init' specifically on a method
        // (see sema_attributes.cpp) rather than the parser vetoing every attribute name
        // uniformly, since the other four have no stated restriction against methods. Parsed
        // before 'pub', matching parse_decl's attribute-before-pub ordering.
        auto attributes = parse_attribute_clause(parser);
        if (!attributes.empty()) {
            // See parse_decl's identical skip: an attribute clause's last token is always an
            // ASI trigger, so a virtual ';' separates it from 'pub'/'fn' on the next line.
            skip_semicolons(parser);
        }

        const bool is_pub = parser.match(TokenKind::KwPub);
        if (is_pub && !allow_pub) {
            parser.report_error(
                location,
                "'pub' is not allowed on methods inside 'impl TRAIT for TYPE'; the trait's own visibility governs");
        }

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
        const auto self_location = parser.current_location();
        const auto self_name = parser.expect_identifier();
        if (self_name != "self") {
            parser.report_error(location, "first parameter of impl function must be 'self' or 'mut self'");
        }

        // Parse remaining params (with types)
        std::vector<ImplDecl::Function::Param> params;
        bool seen_variadic = false;
        while (!parser.check(TokenKind::RParen) && !parser.at_end()) {
            const LoopProgressGuard progress_guard(parser);

            parser.expect(TokenKind::Comma, "','");
            auto param = parse_one_param(parser, ParamPolicy{}, seen_variadic);
            params.push_back(ImplDecl::Function::Param{
                .name = std::move(param.name),
                .type = std::move(param.type),
                .default_value = std::move(param.default_value),
                .is_mut = param.is_mut,
                .is_variadic = param.is_variadic,
                .location = param.location,
            });
        }

        parser.expect(TokenKind::RParen, "')'");
        auto return_types = parse_function_return_types(parser);
        auto body = parse_stmt(parser);

        return ImplDecl::Function{
            .is_pub = is_pub,
            .attributes = std::move(attributes),
            .is_mut_self = is_mut_self,
            .name = std::move(name),
            .params = std::move(params),
            .return_types = std::move(return_types.types),
            .return_names = std::move(return_types.names),
            .body = std::move(body),
            .location = location,
            .self_location = self_location,
        };
    }

    auto parse_impl_decl(Parser &parser) -> Decl {
        const auto location = parser.current_location();
        parser.expect(TokenKind::KwImpl, "'impl'");
        // generic_args suppressed: the bracket immediately following an impl target/trait/type
        // name is always this decl's own trailing 'generic_params' clause below, never
        // NamedType's own 'generic_args' — see grammar.md note 17.
        auto target = parse_named_type(parser, /*allow_generic_args=*/false);

        if (parser.match(TokenKind::KwFor)) {
            // 'impl TRAIT for TYPE { ... }' — 'target' parsed above was actually the trait name.
            auto trait_name = std::move(target);
            auto type_name = parse_named_type(parser, /*allow_generic_args=*/false);

            std::vector<GenericParam> impl_generic_params;
            if (parser.check(TokenKind::LBracket)) {
                impl_generic_params = parse_generic_params(parser);
            }

            parser.expect(TokenKind::LBrace, "'{'");

            std::vector<ImplDecl::Function> functions;
            while (true) {
                skip_semicolons(parser);
                if (parser.check(TokenKind::RBrace) || parser.at_end() || parser.has_reached_max_errors()) {
                    break;
                }

                const LoopProgressGuard progress_guard(parser);

                functions.push_back(parse_impl_method(parser, /*allow_pub=*/false));
            }

            parser.expect(TokenKind::RBrace, "'}'");

            return TraitImplDecl{
                .trait_name = std::move(trait_name),
                .type_name = std::move(type_name),
                .generic_params = std::move(impl_generic_params),
                .functions = std::move(functions),
                .location = location,
            };
        }

        std::vector<GenericParam> impl_generic_params;
        if (parser.check(TokenKind::LBracket)) {
            impl_generic_params = parse_generic_params(parser);
        }

        parser.expect(TokenKind::LBrace, "'{'");

        std::vector<ImplDecl::Function> functions;
        while (true) {
            skip_semicolons(parser);
            if (parser.check(TokenKind::RBrace) || parser.at_end() || parser.has_reached_max_errors()) {
                break;
            }

            const LoopProgressGuard progress_guard(parser);

            functions.push_back(parse_impl_method(parser, /*allow_pub=*/true));
        }

        parser.expect(TokenKind::RBrace, "'}'");

        return ImplDecl{
            .target = std::move(target),
            .generic_params = std::move(impl_generic_params),
            .functions = std::move(functions),
            .location = location,
        };
    }

    auto parse_when_decl_body(Parser &parser) -> std::vector<Decl> {
        parser.expect(TokenKind::LBrace, "'{'");

        std::vector<Decl> decls;
        while (true) {
            skip_semicolons(parser);
            if (parser.check(TokenKind::RBrace) || parser.at_end() || parser.has_reached_max_errors()) {
                break;
            }

            const LoopProgressGuard progress_guard(parser);

            if (auto d = parse_decl(parser)) {
                decls.push_back(std::move(*d));
            }
        }

        parser.expect(TokenKind::RBrace, "'}'");

        return decls;
    }

    // 'when cond { decl... } [else (when ... | { decl... })]' — a module-scope
    // compile-time conditional declaration block. Parsed permissively (any Decl kind, via
    // the ordinary parse_decl dispatcher) — the allow-list restriction to '#link'/'const'
    // with '$option'/'type'/'ext fn' is a SEMA error (see spec), not a parse error, so
    // parsing here must succeed for any decl kind and let sema reject the disallowed ones.
    auto parse_when_decl(Parser &parser) -> std::unique_ptr<WhenDecl> {
        return parse_when_chain<WhenDecl, std::vector<Decl>>(parser, parse_when_decl_body);
    }

    auto parse_decl(Parser &parser) -> std::optional<Decl> {
        // Attributes are currently legal only immediately before a 'fn' declaration (see
        // Attribute's doc comment in ast.hpp). Parsed unconditionally here, before 'is_pub'
        // (attribute-before-pub ordering: '@naked pub fn f()', never 'pub @naked fn f()'), and
        // rejected-after-parse for every other decl kind below — the same "parse permissively,
        // reject with a precise diagnostic" idiom this function already uses for 'is_pub' on
        // 'impl'/'when'/'#link'/'#error'/'#warn'/'asm'.
        //
        // Every attribute clause's last token (an Identifier for '@name'/grouped members, an
        // RParen for '@name(args)'/'@(...)') is an ASI trigger, so the idiomatic one-clause-
        // per-line style (see spec.md's examples) leaves a virtual ';' between the clause and
        // 'pub'/'fn' on the next line — skip it here before checking what follows.
        auto attributes = parse_attribute_clause(parser);
        if (!attributes.empty()) {
            skip_semicolons(parser);
        }

        const auto is_pub = parser.match(TokenKind::KwPub);

        if (!attributes.empty() && !parser.check(TokenKind::KwFn)) {
            parser.report_error(attributes.front().location, "attributes are only allowed on 'fn' declarations");
        }

        if (parser.check(TokenKind::Identifier) && parser.current_lexeme() == "ext") {
            parser.advance();

            return parse_ext_function_decl(parser, is_pub);
        }

        if (parser.check(TokenKind::KwFn)) {
            return parse_function_decl(parser, is_pub, std::move(attributes));
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

        if (parser.check(TokenKind::KwImport)) {
            if (is_pub) {
                parser.report_error(parser.current_location(),
                    "bare imports cannot be 'pub'. All imported symbols are private\n"
                    "       to this module and cannot be re-exported.");
            }
            return Decl{parse_bare_import_decl(parser)};
        }

        if (parser.check(TokenKind::KwImpl)) {
            if (is_pub) {
                parser.report_error(parser.current_location(), "'impl' blocks cannot be 'pub'");
            }
            return parse_impl_decl(parser);
        }

        if (parser.check(TokenKind::KwWhen)) {
            if (is_pub) {
                parser.report_error(parser.current_location(), "'when' blocks cannot be 'pub'");
            }
            return Decl{parse_when_decl(parser)};
        }

        if (peek_link_decl(parser)) {
            if (is_pub) {
                parser.report_error(parser.current_location(), "'#link' directives cannot be 'pub'");
            }
            return Decl{parse_link_decl(parser)};
        }

        if (const auto directive_kind = peek_diagnostic_directive_kind(parser)) {
            if (is_pub) {
                parser.report_error(parser.current_location(),
                    std::format("'#{}' directives cannot be 'pub'",
                        *directive_kind == DiagnosticDirectiveKind::Error ? "error" : "warn"));
            }
            return Decl{parse_diagnostic_decl(parser, *directive_kind)};
        }

        // 'asm' is never legal at module scope, but it parses successfully here too (exactly
        // like '#link'/'#error'/'#warn' above parse successfully as a Stmt) purely so sema can
        // reject it with a precise diagnostic instead of a raw parse error. A bare top-level
        // 'asm ->' (the expression form) is excluded from this gate — it isn't a legal
        // declaration shape at all, so it falls through to the generic "expected declaration"
        // error below instead of the confusing "expected asm block body, got '->'" this branch
        // would otherwise produce.
        if (parser.check(TokenKind::KwAsm) && !parser.check_next(TokenKind::Arrow)) {
            if (is_pub) {
                parser.report_error(parser.current_location(), "'asm' blocks cannot be 'pub'");
            }
            return Decl{parse_asm_stmt(parser)};
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
