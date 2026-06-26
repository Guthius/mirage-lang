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
            {"packed",    TokenKind::KwPacked   },
            {"iota",      TokenKind::KwIota     },
            {"default",   TokenKind::KwDefault  },
            {"undefined", TokenKind::KwUndefined},
            {"len",       TokenKind::KwLen      },

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

            [[nodiscard]] auto MakeLocation() const -> SourceLocation {
                return SourceLocation{
                    .filename = filename_,
                    .line = line_,
                    .column = col_,
                    .offset = pos_,
                };
            }

            [[nodiscard]] auto MakeLocationFromOffset(const size_t offset) const -> SourceLocation {
                return SourceLocation{
                    .filename = filename_,
                    .line = line_,
                    .column = col_ - (pos_ - offset),
                    .offset = offset,
                };
            }

            [[nodiscard]] auto MakeToken(const TokenKind kind, const size_t start) const -> Token {
                return Token{
                    .kind = kind,
                    .lexeme = std::string(source_.substr(start, pos_ - start)),
                    .location = MakeLocationFromOffset(start),
                };
            }

            [[nodiscard]] auto MakeEof() const -> Token {
                return Token{
                    .kind = TokenKind::Eof,
                    .lexeme = {},
                    .location = MakeLocation(),
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

                return MakeToken(is_float ? TokenKind::FloatLiteral : TokenKind::IntLiteral, start);
            }

            auto LexHexNumber(const size_t start) -> Token {
                Advance();

                while (!AtEnd() && (IsHexDigit(Peek()) || Peek() == '_')) {
                    Advance();
                }

                return MakeToken(TokenKind::IntLiteral, start);
            }

            auto LexBinaryNumber(const size_t start) -> Token {
                Advance();

                while (!AtEnd() && (Peek() == '0' || Peek() == '1' || Peek() == '_')) {
                    Advance();
                }

                return MakeToken(TokenKind::IntLiteral, start);
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

                return MakeToken(TokenKind::Identifier, start);
            }

            auto LexString(const size_t start) -> Token {
                while (!AtEnd() && Peek() != '"') {
                    if (Peek() == '\\') {
                        Advance();
                    }

                    Advance();
                }

                if (AtEnd()) {
                    diagnostics_.report_error(DiagnosticStage::Lexer, MakeLocation(), "unterminated string literal");

                    return MakeToken(TokenKind::StringLiteral, start);
                }

                Advance();

                return MakeToken(TokenKind::StringLiteral, start);
            }

            auto LexSymbol(const size_t start, const char ch) -> Token {
                const auto MatchDouble = [&](const char expected, const TokenKind double_token,
                                             const TokenKind single_token) -> Token {
                    return MakeToken(Match(expected) ? double_token : single_token, start);
                };

                switch (ch) {
                case '(':
                    return MakeToken(TokenKind::LParen, start);
                case ')':
                    return MakeToken(TokenKind::RParen, start);
                case '{':
                    return MakeToken(TokenKind::LBrace, start);
                case '}':
                    return MakeToken(TokenKind::RBrace, start);
                case '[':
                    return MakeToken(TokenKind::LBracket, start);
                case ']':
                    return MakeToken(TokenKind::RBracket, start);
                case ',':
                    return MakeToken(TokenKind::Comma, start);
                case '~':
                    return MakeToken(TokenKind::Tilde, start);
                case '?':
                    return MakeToken(TokenKind::Question, start);
                case ';':
                    return MakeToken(TokenKind::Semicolon, start);
                case '.':
                    return MatchDouble('.', TokenKind::DotDot, TokenKind::Dot);

                case '+':
                    if (Match('+'))
                        return MakeToken(TokenKind::PlusPlus, start);
                    if (Match('='))
                        return MakeToken(TokenKind::PlusEqual, start);
                    return MakeToken(TokenKind::Plus, start);

                case '-':
                    if (Match('-'))
                        return MakeToken(TokenKind::MinusMinus, start);
                    if (Match('='))
                        return MakeToken(TokenKind::MinusEqual, start);
                    if (Match('>'))
                        return MakeToken(TokenKind::Arrow, start);
                    return MakeToken(TokenKind::Minus, start);

                case '*':
                    return MatchDouble('=', TokenKind::StarEqual, TokenKind::Star);
                case '/':
                    return MatchDouble('=', TokenKind::SlashEqual, TokenKind::Slash);
                case '%':
                    return MakeToken(TokenKind::Percent, start);

                case '&':
                    if (Match('&'))
                        return MakeToken(TokenKind::AmpAmp, start);
                    if (Match('='))
                        return MakeToken(TokenKind::AmpEqual, start);
                    return MakeToken(TokenKind::Ampersand, start);

                case '|':
                    if (Match('|'))
                        return MakeToken(TokenKind::PipePipe, start);
                    if (Match('='))
                        return MakeToken(TokenKind::PipeEqual, start);
                    return MakeToken(TokenKind::Pipe, start);

                case '^':
                    return MatchDouble('=', TokenKind::CaretEqual, TokenKind::Caret);
                case '=':
                    return MatchDouble('=', TokenKind::EqualEqual, TokenKind::Equal);
                case '!':
                    return MatchDouble('=', TokenKind::BangEqual, TokenKind::Bang);

                case '<':
                    return Match('<') ? MatchDouble('=', TokenKind::ShiftLeftEqual, TokenKind::ShiftLeft)
                                      : MatchDouble('=', TokenKind::LessEqual, TokenKind::Less);

                case '>':
                    return Match('>') ? MatchDouble('=', TokenKind::ShiftRightEqual, TokenKind::ShiftRight)
                                      : MatchDouble('=', TokenKind::GreaterEqual, TokenKind::Greater);

                case ':':
                    return MatchDouble('=', TokenKind::ColonEqual, TokenKind::Colon);

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

                    return MakeToken(TokenKind::Eof, start);
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
