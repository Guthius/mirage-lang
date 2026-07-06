#include "lexer.hpp"

#include <cctype>
#include <optional>
#include <unordered_map>

namespace lexer {
    namespace {
        using KeywordMap = std::unordered_map<std::string_view, TokenKind>;

        const KeywordMap keywords = {
            {"const",     TokenKind::KwConst    },
            {"mut",       TokenKind::KwMut      },
            {"fn",        TokenKind::KwFn       },
            {"type",      TokenKind::KwType     },
            {"trait",     TokenKind::KwTrait    },
            {"struct",    TokenKind::KwStruct   },
            {"impl",      TokenKind::KwImpl     },
            {"for",       TokenKind::KwFor      },
            {"pub",       TokenKind::KwPub      },
            {"continue",  TokenKind::KwContinue },
            {"break",     TokenKind::KwBreak    },
            {"return",    TokenKind::KwReturn   },
            {"if",        TokenKind::KwIf       },
            {"else",      TokenKind::KwElse     },
            {"while",     TokenKind::KwWhile    },
            {"in",        TokenKind::KwIn       },
            {"import",    TokenKind::KwImport   },
            {"namespace", TokenKind::KwNamespace},
            {"asm",       TokenKind::KwAsm      },
            {"macro",     TokenKind::KwMacro    },
            {"try",       TokenKind::KwTry      },
            {"when",      TokenKind::KwWhen     },
            {"nil",       TokenKind::KwNil      },
            {"true",      TokenKind::KwTrue     },
            {"false",     TokenKind::KwFalse    },
            {"sizeof",    TokenKind::KwSizeOf   },
            {"offsetof",  TokenKind::KwOffsetOf },
            {"cast",      TokenKind::KwCast     },
            {"enum",      TokenKind::KwEnum     },
            {"iota",      TokenKind::KwIota     },
            {"default",   TokenKind::KwDefault  },
            {"undefined", TokenKind::KwUndefined},
            {"len",       TokenKind::KwLen      },
            {"match",     TokenKind::KwMatch    },
            {"union",     TokenKind::KwUnion    },

            // Primitive types
            {"u8",        TokenKind::KwU8       },
            {"u16",       TokenKind::KwU16      },
            {"u32",       TokenKind::KwU32      },
            {"u64",       TokenKind::KwU64      },
            {"i8",        TokenKind::KwI8       },
            {"i16",       TokenKind::KwI16      },
            {"i32",       TokenKind::KwI32      },
            {"i64",       TokenKind::KwI64      },
            {"f32",       TokenKind::KwF32      },
            {"f64",       TokenKind::KwF64      },
            {"usize",     TokenKind::KwUSize    },
            {"bool",      TokenKind::KwBool     },
            {"byte",      TokenKind::KwByte     },
            {"error",     TokenKind::KwError    },
            {"anyptr",    TokenKind::KwAnyptr   },
        };

        auto is_digit(const char ch) -> bool { return std::isdigit(ch) != 0; }
        auto is_hex_digit(const char ch) -> bool { return std::isxdigit(ch) != 0; }
        auto is_alpha(const char ch) -> bool { return std::isalpha(ch) != 0; }
        auto is_alpha_numeric(const char ch) -> bool { return std::isalnum(ch) != 0; }

        struct LexerImpl {
            std::string_view source_;
            std::string_view filename_;
            DiagnosticEngine &diagnostics_;
            size_t pos_ = 0;
            uint32_t line_ = 1;
            uint32_t col_ = 1;

            LexerImpl(const std::string_view source, const std::string_view filename, DiagnosticEngine &diagnostics) : source_(source), filename_(filename), diagnostics_(diagnostics) {}

            auto tokenize() -> std::vector<Token> {
                std::vector<Token> tokens;

                while (true) {
                    auto token = lex_token();

                    tokens.push_back(token);

                    if (token.kind == TokenKind::KwAsm) {
                        if (auto asm_token = lex_asm_block(); asm_token.has_value()) {
                            tokens.push_back(std::move(*asm_token));
                        }
                    }

                    if (token.kind == TokenKind::Eof) {
                        break;
                    }
                }

                return tokens;
            }

            [[nodiscard]] auto make_location() const -> SourceLocation {
                return SourceLocation{
                    .filename = filename_,
                    .line = line_,
                    .column = col_,
                    .offset = pos_,
                };
            }

            [[nodiscard]] auto make_location_from_offset(const size_t offset) const -> SourceLocation {
                return SourceLocation{
                    .filename = filename_,
                    .line = line_,
                    .column = col_ - (pos_ - offset),
                    .offset = offset,
                };
            }

            [[nodiscard]] auto make_token(const TokenKind kind, const size_t start) const -> Token {
                return Token{
                    .kind = kind,
                    .lexeme = std::string(source_.substr(start, pos_ - start)),
                    .location = make_location_from_offset(start),
                };
            }

            [[nodiscard]] auto make_eof() const -> Token {
                return Token{
                    .kind = TokenKind::Eof,
                    .lexeme = {},
                    .location = make_location(),
                };
            }

            [[nodiscard]] auto at_end() const -> bool {
                return pos_ >= source_.size();
            }

            [[nodiscard]] auto peek() const -> char {
                return at_end() ? '\n' : source_[pos_];
            }

            [[nodiscard]] auto peek_next() const -> char {
                if (pos_ + 1 >= source_.size()) {
                    return '\0';
                }
                return source_[pos_ + 1];
            }

            auto advance() -> char {
                const char ch = source_[pos_++];

                if (ch == '\n') {
                    ++line_;
                    col_ = 1;
                } else {
                    ++col_;
                }

                return ch;
            }

            auto match(const char expected) -> bool {
                if (at_end() || source_[pos_] != expected) {
                    return false;
                }

                advance();

                return true;
            }

            void skip_whitespace_and_comments() {
                while (!at_end()) {
                    const char ch = peek();
                    if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
                        advance();
                    } else if (ch == '/' && peek_next() == '/') {
                        while (!at_end() && peek() != '\n') {
                            advance();
                        }
                    } else {
                        break;
                    }
                }
            }

            void skip_digits() {
                while (!at_end() && (is_digit(peek()) || peek() == '_')) {
                    advance();
                }
            }

            auto lex_token() -> Token {
                skip_whitespace_and_comments();

                if (at_end()) {
                    return make_eof();
                }

                const auto start = pos_;
                const auto ch = advance();

                if (is_digit(ch)) {
                    return lex_number(start);
                }

                if (is_alpha(ch) || ch == '_') {
                    return lex_identifier_or_keyword(start);
                }

                if (ch == '"') {
                    return lex_string(start);
                }

                return lex_symbol(start, ch);
            }

            auto lex_number(const size_t start) -> Token {
                auto is_float = false;

                if (source_[start] == '0' && pos_ < source_.size()) {
                    const auto next = peek();
                    if (next == 'x' || next == 'X') {
                        return lex_hex_number(start);
                    }

                    if (next == 'b' || next == 'B') {
                        return lex_binary_number(start);
                    }
                }

                skip_digits();

                if (!at_end() && peek() == '.' && is_digit(peek_next())) {
                    is_float = true;

                    advance();

                    skip_digits();
                }

                if (!at_end() && (peek() == 'e' || peek() == 'E')) {
                    is_float = true;

                    advance();

                    if (!at_end() && (peek() == '+' || peek() == '-')) {
                        advance();
                    }

                    while (!at_end() && is_digit(peek())) {
                        advance();
                    }
                }

                return make_token(is_float ? TokenKind::FloatLiteral : TokenKind::IntLiteral, start);
            }

            auto lex_hex_number(const size_t start) -> Token {
                advance();

                while (!at_end() && (is_hex_digit(peek()) || peek() == '_')) {
                    advance();
                }

                return make_token(TokenKind::IntLiteral, start);
            }

            auto lex_binary_number(const size_t start) -> Token {
                advance();

                while (!at_end() && (peek() == '0' || peek() == '1' || peek() == '_')) {
                    advance();
                }

                return make_token(TokenKind::IntLiteral, start);
            }

            auto lex_identifier_or_keyword(const size_t start) -> Token {
                while (!at_end() && (is_alpha_numeric(peek()) || peek() == '_')) {
                    advance();
                }

                const auto text = source_.substr(start, pos_ - start);

                const auto it = keywords.find(text);
                if (it != keywords.end()) {
                    return make_token(it->second, start);
                }

                return make_token(TokenKind::Identifier, start);
            }

            auto lex_string(const size_t start) -> Token {
                while (!at_end() && peek() != '"') {
                    if (peek() == '\\') {
                        advance();
                    }

                    advance();
                }

                if (at_end()) {
                    diagnostics_.report_error(DiagnosticStage::Lexer, make_location(), "unterminated string literal");

                    return make_token(TokenKind::StringLiteral, start);
                }

                advance();

                return make_token(TokenKind::StringLiteral, start);
            }

            auto lex_symbol(const size_t start, const char ch) -> Token {
                const auto match_double = [&](const char expected, const TokenKind double_token,
                                              const TokenKind single_token) -> Token {
                    return make_token(match(expected) ? double_token : single_token, start);
                };

                switch (ch) {
                case '(': return make_token(TokenKind::LParen, start);
                case ')': return make_token(TokenKind::RParen, start);
                case '{': return make_token(TokenKind::LBrace, start);
                case '}': return make_token(TokenKind::RBrace, start);
                case '[': return make_token(TokenKind::LBracket, start);
                case ']': return make_token(TokenKind::RBracket, start);
                case ',': return make_token(TokenKind::Comma, start);
                case '~': return make_token(TokenKind::Tilde, start);
                case '?': return make_token(TokenKind::Question, start);
                case ';': return make_token(TokenKind::Semicolon, start);

                case '.':
                    if (match('.')) {
                        if (match('.')) return make_token(TokenKind::DotDotDot, start);
                        return make_token(TokenKind::DotDot, start);
                    }
                    return make_token(TokenKind::Dot, start);

                case '+':
                    if (match('+')) return make_token(TokenKind::PlusPlus, start);
                    if (match('=')) return make_token(TokenKind::PlusEqual, start);
                    return make_token(TokenKind::Plus, start);

                case '-':
                    if (match('-')) return make_token(TokenKind::MinusMinus, start);
                    if (match('=')) return make_token(TokenKind::MinusEqual, start);
                    if (match('>')) return make_token(TokenKind::Arrow, start);
                    return make_token(TokenKind::Minus, start);

                case '*': return match_double('=', TokenKind::StarEqual, TokenKind::Star);
                case '/': return match_double('=', TokenKind::SlashEqual, TokenKind::Slash);
                case '%': return make_token(TokenKind::Percent, start);

                case '&':
                    if (match('&')) return make_token(TokenKind::AmpAmp, start);
                    if (match('=')) return make_token(TokenKind::AmpEqual, start);
                    return make_token(TokenKind::Ampersand, start);

                case '|':
                    if (match('|')) return make_token(TokenKind::PipePipe, start);
                    if (match('=')) return make_token(TokenKind::PipeEqual, start);
                    return make_token(TokenKind::Pipe, start);

                case '^': return match_double('=', TokenKind::CaretEqual, TokenKind::Caret);
                case '=': return match_double('=', TokenKind::EqualEqual, TokenKind::Equal);
                case '!': return match_double('=', TokenKind::BangEqual, TokenKind::Bang);

                case '<': return match('<') ? match_double('=', TokenKind::ShiftLeftEqual, TokenKind::ShiftLeft)
                                            : match_double('=', TokenKind::LessEqual, TokenKind::Less);

                case '>': return match('>') ? match_double('=', TokenKind::ShiftRightEqual, TokenKind::ShiftRight)
                                            : match_double('=', TokenKind::GreaterEqual, TokenKind::Greater);

                case ':': return match_double('=', TokenKind::ColonEqual, TokenKind::Colon);

                default:
                    diagnostics_.report_error(
                        DiagnosticStage::Lexer,
                        SourceLocation{
                            .filename = filename_,
                            .line = line_,
                            .column = col_ - 1,
                            .offset = start,
                        },
                        std::string("unexpected character '") + ch + "'");

                    return make_token(TokenKind::Eof, start);
                }
            }

            auto lex_asm_block() -> std::optional<Token> {
                skip_whitespace_and_comments();
                if (at_end() || peek() != '{') {
                    return std::nullopt;
                }

                const auto block_start = pos_;

                advance();

                int depth = 1;
                while (!at_end() && depth > 0) {
                    const auto ch = peek();
                    if (ch == '{') {
                        ++depth;
                    } else if (ch == '}') {
                        --depth;
                    }

                    if (depth > 0) {
                        advance();
                    }
                }

                const auto body = source_.substr(block_start + 1, pos_ - block_start - 1);
                auto tok = Token{
                    .kind = TokenKind::AsmBlock,
                    .lexeme = std::string(body),
                    .location =
                        {
                                   .filename = filename_,
                                   .line = line_,
                                   .column = col_,
                                   .offset = block_start,
                                   },
                };

                if (!at_end()) {
                    advance();
                }

                return tok;
            }
        };
    }

    auto tokenize(const std::string_view source, const std::string_view filename, DiagnosticEngine &diagnostics) -> std::vector<Token> {
        return LexerImpl(source, filename, diagnostics).tokenize();
    }
}
