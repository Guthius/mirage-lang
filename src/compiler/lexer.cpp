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
            {"return_err",TokenKind::KwReturnErr},
            {"return_ok", TokenKind::KwReturnOk },
            {"if",        TokenKind::KwIf       },
            {"else",      TokenKind::KwElse     },
            {"while",     TokenKind::KwWhile    },
            {"in",        TokenKind::KwIn       },
            {"import",    TokenKind::KwImport   },
            {"import_bin",TokenKind::KwImportBin},
            {"namespace", TokenKind::KwNamespace},
            {"asm",       TokenKind::KwAsm      },
            {"macro",     TokenKind::KwMacro    },
            {"try",       TokenKind::KwTry      },
            {"when",      TokenKind::KwWhen     },
            {"nil",       TokenKind::KwNil      },
            {"true",      TokenKind::KwTrue     },
            {"false",     TokenKind::KwFalse    },
            {"size_of",   TokenKind::KwSizeOf   },
            {"align_of",  TokenKind::KwAlignOf  },
            {"type_of",   TokenKind::KwTypeOf   },
            {"type_info_of", TokenKind::KwTypeInfoOf},
            {"offsetof",  TokenKind::KwOffsetOf },
            {"stackalloc",TokenKind::KwStackAlloc},
            {"cast",      TokenKind::KwCast     },
            {"enum",      TokenKind::KwEnum     },
            {"bitset",    TokenKind::KwBitset   },
            {"iota",      TokenKind::KwIota     },
            {"default",   TokenKind::KwDefault  },
            {"undefined", TokenKind::KwUndefined},
            {"len",       TokenKind::KwLen      },
            {"match",     TokenKind::KwMatch    },
            {"union",     TokenKind::KwUnion    },
            {"switch",    TokenKind::KwSwitch   },
            {"defer",     TokenKind::KwDefer    },

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
            {"any",       TokenKind::KwAny      },
        };

        // The <cctype> classifiers take an int whose value must be representable as
        // unsigned char (or EOF); passing a plain char is undefined behaviour for any byte
        // with the high bit set -- non-ASCII text, a stray UTF-8 continuation byte, or a
        // UTF-8 BOM at the start of a file. asm_lexer.cpp already casts everywhere; these
        // four are the main lexer's equivalents.
        auto is_digit(const char ch) -> bool { return std::isdigit(static_cast<unsigned char>(ch)) != 0; }
        auto is_hex_digit(const char ch) -> bool { return std::isxdigit(static_cast<unsigned char>(ch)) != 0; }
        auto is_alpha(const char ch) -> bool { return std::isalpha(static_cast<unsigned char>(ch)) != 0; }
        auto is_alpha_numeric(const char ch) -> bool { return std::isalnum(static_cast<unsigned char>(ch)) != 0; }

        // Go-style automatic semicolon insertion: a virtual ';' is inserted after the last
        // token on a line if that token could legally end a statement/expression. Kept in sync
        // with TokenKind::Semicolon's exclusion below (never a trigger) to avoid double-insertion.
        auto is_asi_trigger(const TokenKind kind) -> bool {
            switch (kind) {
            case TokenKind::Identifier:
            case TokenKind::IntLiteral:
            case TokenKind::FloatLiteral:
            case TokenKind::StringLiteral:
            case TokenKind::CharLiteral:
            case TokenKind::KwTrue:
            case TokenKind::KwFalse:
            case TokenKind::KwNil:
            // 'default' and 'undefined' are complete terminal expressions exactly like
            // true/false/nil above -- 'mut x: T = default' is a finished statement.
            case TokenKind::KwDefault:
            case TokenKind::KwUndefined:
            case TokenKind::RParen:
            case TokenKind::RBrace:
            case TokenKind::RBracket:
            case TokenKind::PlusPlus:
            case TokenKind::MinusMinus:
            case TokenKind::KwReturn:
            case TokenKind::KwReturnOk:
            case TokenKind::KwBreak:
            case TokenKind::KwContinue:
            // The raw-captured asm body ('asm { ... }' / 'x := asm -> rax { ... }') ends a
            // statement exactly like RBrace above. AsmBlock tokens are pushed outside
            // lex_token(), so their two push sites in tokenize() set the flag directly;
            // this case keeps the switch the single source of truth.
            case TokenKind::AsmBlock:
                return true;
            default:
                return false;
            }
        }

        struct LexerImpl {
            std::string_view source_;
            std::string_view filename_;
            DiagnosticEngine &diagnostics_;
            size_t pos_ = 0;
            size_t line_ = 1;
            size_t col_ = 1;
            std::optional<TokenKind> last_real_kind_ = std::nullopt;
            // Set right after a 'KwAsm' token whose immediate lex_asm_block() attempt failed
            // (i.e. the next real char is '-' '>' — the expression form's header, not the
            // statement form's immediate '{') — arms a bounded retry of lex_asm_block() at the
            // top of the main loop so the '{' that eventually follows the header ('-> reg
            // [: type]', ordinary tokens lexed normally by lex_token()) gets raw-captured
            // instead of being lexed as an ordinary LBrace. See tokenize()'s two call sites.
            bool awaiting_asm_expr_body_ = false;
            int asm_header_budget_ = 0; // remaining header tokens before giving up (safety valve)
            // Whether the last real token could legally end a statement/expression. Usually
            // just is_asi_trigger(last_real_kind_), but 'Star' is dual-purpose: it's the
            // binary-multiply operator (never a trigger — 'a *\n b' must keep continuing) AND
            // the second token of postfix deref 'expr.*' (always a trigger, since '.*' can
            // complete a statement, e.g. 'x := p.*'). Distinguishing them needs one token of
            // extra lookback (was the token before this Star a Dot?), which is still a purely
            // lexer-level, previous-token-based decision — just over 2 tokens instead of 1.
            bool last_token_is_asi_trigger_ = false;
            // End position (just past the last character) of the last real token. Virtual
            // semicolons are located here — at the end of the line they terminate — rather
            // than at the *next* token's start: aliasing the next token's offset made a
            // recovery iteration that consumed only the virtual ';' look like "no progress"
            // to LoopProgressGuard, which then force-advanced past a real token.
            SourceLocation last_token_end_{};
            // Line/column of the token currently being lexed, snapshotted by lex_token()
            // before it consumes anything. make_token() reports these verbatim rather than
            // back-computing the start from the end position, which is only correct while a
            // token stays on one line. lex_asm_block() has always snapshotted its own start
            // for exactly this reason; these two members generalize that to every token.
            size_t token_start_line_ = 1;
            size_t token_start_col_ = 1;

            LexerImpl(const std::string_view source, const std::string_view filename, DiagnosticEngine &diagnostics) : source_(source), filename_(filename), diagnostics_(diagnostics) {}

            auto tokenize() -> std::vector<Token> {
                std::vector<Token> tokens;

                while (true) {
                    // Bounded retry for the expression form's header ('asm -> reg [: type]'):
                    // once armed below, try raw-capturing at every loop iteration until either
                    // the real '{' is reached (success) or the budget runs out (malformed
                    // input — give up and let normal tokenization/parsing report its own error).
                    if (awaiting_asm_expr_body_) {
                        if (auto asm_token = lex_asm_block(); asm_token.has_value()) {
                            awaiting_asm_expr_body_ = false;
                            tokens.push_back(std::move(*asm_token));
                            last_real_kind_ = TokenKind::AsmBlock;
                            // An asm block can end a statement ('x := asm -> rax { ... }'),
                            // so it must trigger ASI: a following line starting with '-',
                            // '(' or '[' was silently glued into the expression.
                            last_token_is_asi_trigger_ = true;
                            last_token_end_ = make_location();
                            continue;
                        }
                        if (--asm_header_budget_ <= 0) {
                            awaiting_asm_expr_body_ = false;
                        }
                    }

                    auto maybe_token = lex_token();
                    if (!maybe_token.has_value()) {
                        // An unrecognized byte: already reported and consumed. Skip it and
                        // keep lexing, so one stray character does not remove the rest of
                        // the file from the token stream.
                        continue;
                    }
                    auto token = std::move(*maybe_token);

                    // Go-style ASI also fires before EOF, so the last statement in a file
                    // terminates even without a trailing newline. Semicolon is never itself
                    // a trigger, so this can't double-insert. lex_token() deliberately leaves
                    // last_token_is_asi_trigger_/last_real_kind_ untouched on its EOF path, so
                    // they still reflect the last real token here.
                    if (token.kind == TokenKind::Eof && last_token_is_asi_trigger_) {
                        tokens.push_back(Token{
                            .kind = TokenKind::Semicolon,
                            .lexeme = {},
                            .location = last_token_end_,
                        });
                        last_real_kind_ = TokenKind::Semicolon;
                        last_token_is_asi_trigger_ = false;
                    }

                    tokens.push_back(token);

                    if (token.kind == TokenKind::KwAsm) {
                        if (auto asm_token = lex_asm_block(); asm_token.has_value()) {
                            tokens.push_back(std::move(*asm_token));
                            last_real_kind_ = TokenKind::AsmBlock;
                            last_token_is_asi_trigger_ = true;
                            last_token_end_ = make_location();
                        } else if (peek() == '-' && peek_next() == '>') {
                            // 'asm -> reg [: type] { ... }' — the header is ordinary Mirage
                            // grammar (Arrow, an identifier, an optional ':' + type), lexed
                            // token-by-token below; only the raw body needs lex_asm_block()'s
                            // brace-balanced capture, so arm the retry above for once the
                            // header has been lexed through.
                            awaiting_asm_expr_body_ = true;
                            asm_header_budget_ = 16;
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

            [[nodiscard]] auto make_token(const TokenKind kind, const size_t start) const -> Token {
                auto location = SourceLocation{
                    .filename = filename_,
                    .line = token_start_line_,
                    .column = token_start_col_,
                    .offset = start,
                };
                location.length = pos_ - start;
                return Token{
                    .kind = kind,
                    .lexeme = std::string(source_.substr(start, pos_ - start)),
                    .location = location,
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

            // EOF reads as '\n' on purpose: the line-comment, string- and
            // char-literal scanners test `peek() == '\n'` to detect an
            // unterminated construct at end of line, and the sentinel makes
            // end of file take those same paths without a separate at_end()
            // check at every site. Do not change it to '\0'.
            [[nodiscard]] auto peek() const -> char {
                return at_end() ? '\n' : source_[pos_];
            }

            // Deliberately NOT the '\n' sentinel peek() uses: two-char
            // lookahead only asks "is this specific character next", and a
            // fabricated '\n' could satisfy such a test at EOF. '\0' matches
            // nothing the lexer ever looks for.
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

            // Returns whether a '\n' was consumed while skipping, so the caller can decide
            // whether an ASI-triggering token just crossed a statement boundary.
            auto skip_whitespace_and_comments() -> bool {
                bool crossed_newline = false;
                while (!at_end()) {
                    const char ch = peek();
                    if (ch == ' ' || ch == '\t' || ch == '\r') {
                        advance();
                    } else if (ch == '\n') {
                        crossed_newline = true;
                        advance();
                    } else if (ch == '/' && peek_next() == '/') {
                        while (!at_end() && peek() != '\n') {
                            advance();
                        }
                    } else if (ch == '/' && peek_next() == '*') {
                        advance();
                        advance();

                        while (!at_end() && !(peek() == '*' && peek_next() == '/')) {
                            if (peek() == '\n') {
                                crossed_newline = true;
                            }
                            advance();
                        }

                        if (at_end()) {
                            diagnostics_.report_error(DiagnosticStage::Lexer, make_location(), "unterminated block comment");
                        } else {
                            advance();
                            advance();
                        }
                    } else {
                        break;
                    }
                }
                return crossed_newline;
            }

            void skip_digits() {
                while (!at_end() && (is_digit(peek()) || peek() == '_')) {
                    advance();
                }
            }

            // Returns nullopt when the input byte was unrecognized: it has been reported and
            // consumed, and the caller should simply keep going. Every other path yields a
            // token, including the genuine end-of-input Eof.
            auto lex_token() -> std::optional<Token> {
                const bool crossed_newline = skip_whitespace_and_comments();

                if (crossed_newline && last_token_is_asi_trigger_) {
                    last_real_kind_ = TokenKind::Semicolon;
                    last_token_is_asi_trigger_ = false;
                    return Token{
                        .kind = TokenKind::Semicolon,
                        .lexeme = {},
                        .location = last_token_end_,
                    };
                }

                if (at_end()) {
                    // Deliberately do not touch last_real_kind_/last_token_is_asi_trigger_ here:
                    // tokenize() needs to see the last *real* token's trigger status to decide
                    // whether to insert a semicolon before this Eof.
                    return make_eof();
                }

                const auto start = pos_;
                token_start_line_ = line_;
                token_start_col_ = col_;
                const auto ch = advance();

                std::optional<Token> token = [&]() -> std::optional<Token> {
                    if (is_digit(ch)) {
                        return lex_number(start);
                    }

                    if (is_alpha(ch) || ch == '_') {
                        return lex_identifier_or_keyword(start);
                    }

                    if (ch == '.' && is_digit(peek())) {
                        return lex_fractional_number(start);
                    }

                    if (ch == '"') {
                        return lex_string(start);
                    }

                    if (ch == '\'') {
                        return lex_char(start);
                    }

                    return lex_symbol(start, ch);
                }();

                if (!token.has_value()) {
                    // Deliberately leave last_real_kind_/last_token_is_asi_trigger_ alone, as
                    // the Eof path above does: a skipped byte is not a token, and the ASI
                    // state must keep describing the last *real* token.
                    return std::nullopt;
                }

                // 'expr.*' (postfix deref) can end a statement even though bare Star (binary
                // multiply) cannot — see last_token_is_asi_trigger_'s doc comment.
                const bool is_dot_star = token->kind == TokenKind::Star && last_real_kind_ == TokenKind::Dot;
                last_token_is_asi_trigger_ = is_asi_trigger(token->kind) || is_dot_star;
                last_real_kind_ = token->kind;
                last_token_end_ = make_location();
                return token;
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

                    if (next == 'o' || next == 'O') {
                        return lex_octal_number(start);
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

                    const auto exponent_digits_start = pos_;
                    skip_digits();
                    if (pos_ == exponent_digits_start) {
                        // '1e' / '1e+' — previously lexed as-is and silently decoded as 1.0.
                        diagnostics_.report_error(DiagnosticStage::Lexer, make_location(), "missing digits in float exponent");
                    }
                }

                return make_token(is_float ? TokenKind::FloatLiteral : TokenKind::IntLiteral, start);
            }

            // Called with 'start' at the leading '.' of a number with no integer part
            // (e.g. '.35'); the caller has already confirmed a digit follows.
            auto lex_fractional_number(const size_t start) -> Token {
                skip_digits();

                if (!at_end() && (peek() == 'e' || peek() == 'E')) {
                    advance();

                    if (!at_end() && (peek() == '+' || peek() == '-')) {
                        advance();
                    }

                    const auto exponent_digits_start = pos_;
                    skip_digits();
                    if (pos_ == exponent_digits_start) {
                        diagnostics_.report_error(DiagnosticStage::Lexer, make_location(), "missing digits in float exponent");
                    }
                }

                return make_token(TokenKind::FloatLiteral, start);
            }

            auto lex_hex_number(const size_t start) -> Token {
                advance();

                bool saw_digit = false;
                while (!at_end() && (is_hex_digit(peek()) || peek() == '_')) {
                    saw_digit |= peek() != '_';
                    advance();
                }
                if (!saw_digit) {
                    // '0x' with nothing after it decoded silently as 0 ('0xG' became 0
                    // followed by identifier 'G'); the grammar requires at least one digit.
                    diagnostics_.report_error(DiagnosticStage::Lexer, make_location(), "missing digits after integer base prefix");
                }

                return make_token(TokenKind::IntLiteral, start);
            }

            auto lex_binary_number(const size_t start) -> Token {
                advance();

                bool saw_digit = false;
                while (!at_end() && (peek() == '0' || peek() == '1' || peek() == '_')) {
                    saw_digit |= peek() != '_';
                    advance();
                }
                if (!saw_digit) {
                    diagnostics_.report_error(DiagnosticStage::Lexer, make_location(), "missing digits after integer base prefix");
                }

                return make_token(TokenKind::IntLiteral, start);
            }

            auto lex_octal_number(const size_t start) -> Token {
                advance();

                bool saw_digit = false;
                while (!at_end() && ((peek() >= '0' && peek() <= '7') || peek() == '_')) {
                    saw_digit |= peek() != '_';
                    advance();
                }
                if (!saw_digit) {
                    diagnostics_.report_error(DiagnosticStage::Lexer, make_location(), "missing digits after integer base prefix");
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

            // A string literal may not span a raw newline, as in most C-family languages.
            // Stopping at the newline keeps the diagnostic on the line with the mistake
            // instead of swallowing the following lines until some later quote closes it,
            // and it guarantees a token never spans lines.
            auto lex_string(const size_t start) -> Token {
                while (!at_end() && peek() != '"' && peek() != '\n') {
                    if (peek() == '\\') {
                        advance();
                        // A trailing backslash does not splice the next line into the
                        // literal; leave the newline for the check below to report.
                        if (at_end() || peek() == '\n') {
                            break;
                        }
                    }

                    advance();
                }

                if (at_end() || peek() == '\n') {
                    diagnostics_.report_error(
                        DiagnosticStage::Lexer,
                        SourceLocation{
                            .filename = filename_,
                            .line = token_start_line_,
                            .column = token_start_col_,
                            .offset = start,
                            .length = pos_ - start,
                        },
                        "unterminated string literal");

                    return make_token(TokenKind::StringLiteral, start);
                }

                advance();

                return make_token(TokenKind::StringLiteral, start);
            }

            auto lex_char(const size_t start) -> Token {
                // As with lex_string, a raw newline terminates the attempt rather than
                // being absorbed into the literal, so a character literal never spans
                // lines and the diagnostic stays on the offending line.
                const auto literal_start_location = [&] {
                    return SourceLocation{
                        .filename = filename_,
                        .line = token_start_line_,
                        .column = token_start_col_,
                        .offset = start,
                        .length = pos_ - start,
                    };
                };

                if (at_end() || peek() == '\'') {
                    diagnostics_.report_error(DiagnosticStage::Lexer, make_location(), "empty character literal");
                    if (!at_end()) advance();
                    return make_token(TokenKind::CharLiteral, start);
                }
                if (peek() == '\n') {
                    diagnostics_.report_error(DiagnosticStage::Lexer, literal_start_location(), "unterminated character literal");
                    return make_token(TokenKind::CharLiteral, start);
                }
                if (peek() == '\\') {
                    advance();
                    if (at_end() || peek() == '\n') {
                        // fall through to unterminated check below
                    } else if (peek() == 'x') {
                        advance();
                        for (int i = 0; i < 2 && !at_end() && is_hex_digit(peek()); ++i) {
                            advance();
                        }
                    } else if (peek() >= '0' && peek() <= '7') {
                        advance();
                        for (int i = 0; i < 2 && !at_end() && peek() >= '0' && peek() <= '7'; ++i) {
                            advance();
                        }
                    } else {
                        advance();
                    }
                } else if (static_cast<unsigned char>(peek()) >= 0x80) {
                    // A UTF-8 lead byte: char literals hold a single byte, so a multi-byte
                    // code point can never fit. Consume the whole code point (and the
                    // closing quote, if it is right there) so 'é' gets this one diagnostic
                    // instead of a misleading "unterminated" plus trailing-garbage errors.
                    advance();
                    while (!at_end() && (static_cast<unsigned char>(peek()) & 0xC0) == 0x80) {
                        advance();
                    }
                    if (!at_end() && peek() == '\'') {
                        advance();
                    }
                    diagnostics_.report_error(DiagnosticStage::Lexer, literal_start_location(), "multi-byte character literals are not supported");
                    return make_token(TokenKind::CharLiteral, start);
                } else {
                    advance();
                }
                if (at_end() || peek() != '\'') {
                    diagnostics_.report_error(DiagnosticStage::Lexer, literal_start_location(), "unterminated character literal");
                    return make_token(TokenKind::CharLiteral, start);
                }
                advance();
                return make_token(TokenKind::CharLiteral, start);
            }

            // nullopt means the byte was unrecognized (reported and consumed) — see lex_token.
            auto lex_symbol(const size_t start, const char ch) -> std::optional<Token> {
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
                case '~': return match_double('=', TokenKind::TildeEqual, TokenKind::Tilde);
                case '?': return make_token(TokenKind::Question, start);
                case ';': return make_token(TokenKind::Semicolon, start);
                case '@': return make_token(TokenKind::At, start);
                case '#': return make_token(TokenKind::Hash, start);
                case '$': return make_token(TokenKind::Dollar, start);

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
                            .line = token_start_line_,
                            .column = token_start_col_,
                            .offset = start,
                            .length = 1,
                        },
                        std::string("unexpected character '") + ch + "'");

                    // The byte is already consumed. Yield no token rather than a synthesized
                    // Eof: tokenize()'s loop stops on any Eof, so returning one here truncated
                    // the entire rest of the file from tokenization -- it was never parsed,
                    // checked or compiled, with no indication beyond this one diagnostic.
                    return std::nullopt;
                }
            }

            auto lex_asm_block() -> std::optional<Token> {
                skip_whitespace_and_comments();
                if (at_end() || peek() != '{') {
                    return std::nullopt;
                }

                // Snapshot the opening brace's position now, before scanning through the body —
                // line_/col_ advance past it below, and the emitted token's location must point
                // at the opening '{' (per the asm-lexer's diagnostics, which report offsets
                // relative to this token's start), not at wherever the scan happens to end.
                const auto block_start = pos_;
                const auto brace_line = line_;
                const auto brace_col = col_;

                advance();

                // Comment/string-naive: a raw '{'/'}' inside an asm-body ';'/'#' comment could
                // desync this counter. Acceptable for v1 — asm bodies are plain
                // mnemonic/operand/comment lines with no string operands (out of scope for v1).
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

                if (depth > 0) {
                    diagnostics_.report_error(
                        DiagnosticStage::Lexer,
                        SourceLocation{
                            .filename = filename_,
                            .line = brace_line,
                            .column = brace_col,
                            .offset = block_start,
                            .length = pos_ - block_start,
                        },
                        "unterminated asm block");
                    return std::nullopt;
                }

                const auto body = source_.substr(block_start + 1, pos_ - block_start - 1);
                auto tok = Token{
                    .kind = TokenKind::AsmBlock,
                    .lexeme = std::string(body),
                    .location =
                        {
                                   .filename = filename_,
                                   .line = brace_line,
                                   .column = brace_col,
                                   .offset = block_start,
                                   .length = pos_ - block_start,
                                   },
                };

                advance(); // consume the matching '}'

                return tok;
            }
        };
    }

    auto tokenize(const std::string_view source, const std::string_view filename, DiagnosticEngine &diagnostics) -> std::vector<Token> {
        return LexerImpl(source, filename, diagnostics).tokenize();
    }

    auto keyword_spellings() -> const std::unordered_map<std::string_view, TokenKind> & {
        return keywords;
    }
}
