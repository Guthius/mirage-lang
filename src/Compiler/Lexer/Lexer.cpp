#include "Lexer.hpp"

#include <cctype>
#include <optional>
#include <unordered_map>

namespace Lexer {
    namespace {
        using KeywordMap = std::unordered_map<std::string_view, TokenKind>;

        const KeywordMap keywords = {
            {"const",     TokenKind::kw_const    },
            {"mut",       TokenKind::kw_mut      },
            {"fn",        TokenKind::kw_fn       },
            {"type",      TokenKind::kw_type     },
            {"trait",     TokenKind::kw_trait    },
            {"struct",    TokenKind::kw_struct   },
            {"impl",      TokenKind::kw_impl     },
            {"for",       TokenKind::kw_for      },
            {"pub",       TokenKind::kw_pub      },
            {"return",    TokenKind::kw_return   },
            {"if",        TokenKind::kw_if       },
            {"else",      TokenKind::kw_else     },
            {"while",     TokenKind::kw_while    },
            {"in",        TokenKind::kw_in       },
            {"import",    TokenKind::kw_import   },
            {"namespace", TokenKind::kw_namespace},
            {"asm",       TokenKind::kw_asm      },
            {"macro",     TokenKind::kw_macro    },
            {"try",       TokenKind::kw_try      },
            {"when",      TokenKind::kw_when     },
            {"nil",       TokenKind::kw_nil      },
            {"true",      TokenKind::kw_true     },
            {"false",     TokenKind::kw_false    },
            {"sizeof",    TokenKind::kw_sizeof   },
            {"offsetof",  TokenKind::kw_offsetof },
            {"cast",      TokenKind::kw_cast     },
            {"enum",      TokenKind::kw_enum     },
            {"packed",    TokenKind::kw_packed   },
            {"iota",      TokenKind::kw_iota     },
            {"default",   TokenKind::kw_default  },
            {"undefined", TokenKind::kw_undefined},
            {"len",       TokenKind::kw_len      },

            // Primitive types
            {"u8",        TokenKind::kw_u8       },
            {"u16",       TokenKind::kw_u16      },
            {"u32",       TokenKind::kw_u32      },
            {"u64",       TokenKind::kw_u64      },
            {"i8",        TokenKind::kw_i8       },
            {"i16",       TokenKind::kw_i16      },
            {"i32",       TokenKind::kw_i32      },
            {"i64",       TokenKind::kw_i64      },
            {"f32",       TokenKind::kw_f32      },
            {"f64",       TokenKind::kw_f64      },
            {"usize",     TokenKind::kw_usize    },
            {"bool",      TokenKind::kw_bool     },
            {"byte",      TokenKind::kw_byte     },
            {"error",     TokenKind::kw_error    },
            {"anyptr",    TokenKind::kw_anyptr   },
        };

        auto IsDigit(const char ch) -> bool { return std::isdigit(ch) != 0; }
        auto IsHexDigit(const char ch) -> bool { return std::isxdigit(ch) != 0; }
        auto IsAlpha(const char ch) -> bool { return std::isalpha(ch) != 0; }
        auto IsAlphaNumeric(const char ch) -> bool { return std::isalnum(ch) != 0; }

        struct LexerImpl {
            std::string_view source_;
            std::string_view filename_;
            DiagnosticEngine &diagnostics_;
            size_t pos_ = 0;
            uint32_t line_ = 1;
            uint32_t col_ = 1;

            LexerImpl(const std::string_view source, const std::string_view filename, DiagnosticEngine &diagnostics) : source_(source), filename_(filename), diagnostics_(diagnostics) {}

            auto Tokenize() -> std::vector<Token> {
                std::vector<Token> tokens;

                while (true) {
                    auto token = LexToken();

                    tokens.push_back(token);

                    if (token.Kind == TokenKind::kw_asm) {
                        if (auto asm_token = lex_asm_block(); asm_token.has_value()) {
                            tokens.push_back(std::move(*asm_token));
                        }
                    }

                    if (token.Kind == TokenKind::eof) {
                        break;
                    }
                }

                return tokens;
            }

            [[nodiscard]] auto MakeLocation() const -> SourceLocation {
                return SourceLocation{
                    .Filename = filename_,
                    .Line = line_,
                    .Column = col_,
                    .Offset = pos_,
                };
            }

            [[nodiscard]] auto MakeLocationFromOffset(const size_t offset) const -> SourceLocation {
                return SourceLocation{
                    .Filename = filename_,
                    .Line = line_,
                    .Column = col_ - (pos_ - offset),
                    .Offset = offset,
                };
            }

            [[nodiscard]] auto MakeToken(const TokenKind kind, const size_t start) const -> Token {
                return Token{
                    .Kind = kind,
                    .Lexeme = std::string(source_.substr(start, pos_ - start)),
                    .Location = MakeLocationFromOffset(start),
                };
            }

            [[nodiscard]] auto MakeEof() const -> Token {
                return Token{
                    .Kind = TokenKind::eof,
                    .Lexeme = {},
                    .Location = MakeLocation(),
                };
            }

            [[nodiscard]] auto AtEnd() const -> bool {
                return pos_ >= source_.size();
            }

            [[nodiscard]] auto Peek() const -> char {
                return AtEnd() ? '\n' : source_[pos_];
            }

            [[nodiscard]] auto PeekNext() const -> char {
                if (pos_ + 1 >= source_.size()) {
                    return '\0';
                }
                return source_[pos_ + 1];
            }

            auto Advance() -> char {
                const char ch = source_[pos_++];

                if (ch == '\n') {
                    ++line_;
                    col_ = 1;
                } else {
                    ++col_;
                }

                return ch;
            }

            auto Match(const char expected) -> bool {
                if (AtEnd() || source_[pos_] != expected) {
                    return false;
                }

                Advance();

                return true;
            }

            void SkipWhitespaceAndComments() {
                while (!AtEnd()) {
                    const char ch = Peek();
                    if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
                        Advance();
                    } else if (ch == '/' && PeekNext() == '/') {
                        while (!AtEnd() && Peek() != '\n') {
                            Advance();
                        }
                    } else {
                        break;
                    }
                }
            }

            void SkipDigits() {
                while (!AtEnd() && (IsDigit(Peek()) || Peek() == '_')) {
                    Advance();
                }
            }

            auto LexToken() -> Token {
                SkipWhitespaceAndComments();

                if (AtEnd()) {
                    return MakeEof();
                }

                const auto start = pos_;
                const auto ch = Advance();

                if (IsDigit(ch)) {
                    return LexNumber(start);
                }

                if (IsAlpha(ch) || ch == '_') {
                    return LexIdentifierOrKeyword(start);
                }

                if (ch == '"') {
                    return LexString(start);
                }

                return LexSymbol(start, ch);
            }

            auto LexNumber(const size_t start) -> Token {
                auto is_float = false;

                if (source_[start] == '0' && pos_ < source_.size()) {
                    const auto next = Peek();
                    if (next == 'x' || next == 'X') {
                        return LexHexNumber(start);
                    }

                    if (next == 'b' || next == 'B') {
                        return LexBinaryNumber(start);
                    }
                }

                SkipDigits();

                if (!AtEnd() && Peek() == '.' && IsDigit(PeekNext())) {
                    is_float = true;

                    Advance();

                    SkipDigits();
                }

                if (!AtEnd() && (Peek() == 'e' || Peek() == 'E')) {
                    is_float = true;

                    Advance();

                    if (!AtEnd() && (Peek() == '+' || Peek() == '-')) {
                        Advance();
                    }

                    while (!AtEnd() && IsDigit(Peek())) {
                        Advance();
                    }
                }

                return MakeToken(is_float ? TokenKind::float_literal : TokenKind::int_literal, start);
            }

            auto LexHexNumber(const size_t start) -> Token {
                Advance();

                while (!AtEnd() && (IsHexDigit(Peek()) || Peek() == '_')) {
                    Advance();
                }

                return MakeToken(TokenKind::int_literal, start);
            }

            auto LexBinaryNumber(const size_t start) -> Token {
                Advance();

                while (!AtEnd() && (Peek() == '0' || Peek() == '1' || Peek() == '_')) {
                    Advance();
                }

                return MakeToken(TokenKind::int_literal, start);
            }

            auto LexIdentifierOrKeyword(const size_t start) -> Token {
                while (!AtEnd() && (IsAlphaNumeric(Peek()) || Peek() == '_')) {
                    Advance();
                }

                const auto text = source_.substr(start, pos_ - start);

                const auto it = keywords.find(text);
                if (it != keywords.end()) {
                    return MakeToken(it->second, start);
                }

                return MakeToken(TokenKind::identifier, start);
            }

            auto LexString(const size_t start) -> Token {
                while (!AtEnd() && Peek() != '"') {
                    if (Peek() == '\\') {
                        Advance();
                    }

                    Advance();
                }

                if (AtEnd()) {
                    diagnostics_.ReportError(DiagnosticStage::Lexer, MakeLocation(), "unterminated string literal");

                    return MakeToken(TokenKind::string_literal, start);
                }

                Advance();

                return MakeToken(TokenKind::string_literal, start);
            }

            auto LexSymbol(const size_t start, const char ch) -> Token {
                const auto MatchDouble = [&](const char expected, const TokenKind double_token,
                                              const TokenKind single_token) -> Token {
                    return MakeToken(Match(expected) ? double_token : single_token, start);
                };

                switch (ch) {
                case '(':
                    return MakeToken(TokenKind::lparen, start);
                case ')':
                    return MakeToken(TokenKind::rparen, start);
                case '{':
                    return MakeToken(TokenKind::lbrace, start);
                case '}':
                    return MakeToken(TokenKind::rbrace, start);
                case '[':
                    return MakeToken(TokenKind::lbracket, start);
                case ']':
                    return MakeToken(TokenKind::rbracket, start);
                case ',':
                    return MakeToken(TokenKind::comma, start);
                case '~':
                    return MakeToken(TokenKind::tilde, start);
                case '?':
                    return MakeToken(TokenKind::question, start);
                case ';':
                    return MakeToken(TokenKind::semicolon, start);
                case '.':
                    return MatchDouble('.', TokenKind::dot_dot, TokenKind::dot);

                case '+':
                    if (Match('+'))
                        return MakeToken(TokenKind::plus_plus, start);
                    if (Match('='))
                        return MakeToken(TokenKind::plus_equal, start);
                    return MakeToken(TokenKind::plus, start);

                case '-':
                    if (Match('-'))
                        return MakeToken(TokenKind::minus_minus, start);
                    if (Match('='))
                        return MakeToken(TokenKind::minus_equal, start);
                    if (Match('>'))
                        return MakeToken(TokenKind::arrow, start);
                    return MakeToken(TokenKind::minus, start);

                case '*':
                    return MatchDouble('=', TokenKind::star_equal, TokenKind::star);
                case '/':
                    return MatchDouble('=', TokenKind::slash_equal, TokenKind::slash);
                case '%':
                    return MakeToken(TokenKind::percent, start);

                case '&':
                    if (Match('&'))
                        return MakeToken(TokenKind::amp_amp, start);
                    if (Match('='))
                        return MakeToken(TokenKind::amp_equal, start);
                    return MakeToken(TokenKind::ampersand, start);

                case '|':
                    if (Match('|'))
                        return MakeToken(TokenKind::pipe_pipe, start);
                    if (Match('='))
                        return MakeToken(TokenKind::pipe_equal, start);
                    return MakeToken(TokenKind::pipe, start);

                case '^':
                    return MatchDouble('=', TokenKind::caret_equal, TokenKind::caret);
                case '=':
                    return MatchDouble('=', TokenKind::equal_equal, TokenKind::equal);
                case '!':
                    return MatchDouble('=', TokenKind::bang_equal, TokenKind::bang);

                case '<':
                    return Match('<') ? MatchDouble('=', TokenKind::shift_left_equal, TokenKind::shift_left)
                                      : MatchDouble('=', TokenKind::less_equal, TokenKind::less);

                case '>':
                    return Match('>') ? MatchDouble('=', TokenKind::shift_right_equal, TokenKind::shift_right)
                                      : MatchDouble('=', TokenKind::greater_equal, TokenKind::greater);

                case ':':
                    return MatchDouble('=', TokenKind::colon_equal, TokenKind::colon);

                default:
                    diagnostics_.ReportError(
                        DiagnosticStage::Lexer,
                        SourceLocation{
                            .Filename = filename_,
                            .Line = line_,
                            .Column = col_ - 1,
                            .Offset = start,
                        },
                        std::string("unexpected character '") + ch + "'");

                    return MakeToken(TokenKind::eof, start);
                }
            }

            auto lex_asm_block() -> std::optional<Token> {
                SkipWhitespaceAndComments();
                if (AtEnd() || Peek() != '{') {
                    return std::nullopt;
                }

                const auto block_start = pos_;

                Advance();

                int depth = 1;
                while (!AtEnd() && depth > 0) {
                    const auto ch = Peek();
                    if (ch == '{') {
                        ++depth;
                    } else if (ch == '}') {
                        --depth;
                    }

                    if (depth > 0) {
                        Advance();
                    }
                }

                const auto body = source_.substr(block_start + 1, pos_ - block_start - 1);
                auto tok = Token{
                    .Kind = TokenKind::asm_block,
                    .Lexeme = std::string(body),
                    .Location =
                        {
                                   .Filename = filename_,
                                   .Line = line_,
                                   .Column = col_,
                                   .Offset = block_start,
                                   },
                };

                if (!AtEnd()) {
                    Advance();
                }

                return tok;
            }
        };
    }

    auto Tokenize(const std::string_view source, const std::string_view filename, DiagnosticEngine &diagnostics) -> std::vector<Token> {
        return LexerImpl(source, filename, diagnostics).Tokenize();
    }
}
