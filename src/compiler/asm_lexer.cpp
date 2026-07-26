#include "asm_lexer.hpp"

#include "asm_registers.hpp"
#include "diagnostic_engine.hpp"

#include <cctype>
#include <charconv>

namespace asm_lexer {
    namespace {
        auto is_ident_start(const char ch) -> bool {
            return std::isalpha(static_cast<unsigned char>(ch)) != 0 || ch == '_';
        }

        auto is_ident_continue(const char ch) -> bool {
            return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_';
        }

        auto to_lower(std::string s) -> std::string {
            for (auto &ch : s) {
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            }
            return s;
        }

        struct Lexer {
            std::string_view text_;
            std::string_view filename_;
            DiagnosticEngine &diagnostics_;
            size_t pos_ = 0;
            size_t line_;
            size_t col_;

            Lexer(const std::string_view text, const SourceLocation &block_location, DiagnosticEngine &diagnostics)
                : text_(text), filename_(block_location.filename), diagnostics_(diagnostics),
                  line_(block_location.line), col_(block_location.column + 1) {
            }

            [[nodiscard]] auto at_end() const -> bool { return pos_ >= text_.size(); }
            [[nodiscard]] auto peek() const -> char { return at_end() ? '\0' : text_[pos_]; }
            [[nodiscard]] auto peek_next() const -> char { return pos_ + 1 >= text_.size() ? '\0' : text_[pos_ + 1]; }

            [[nodiscard]] auto current_location() const -> SourceLocation {
                return SourceLocation{.filename = filename_, .line = line_, .column = col_, .offset = pos_};
            }

            auto advance() -> char {
                const char ch = text_[pos_++];
                if (ch == '\n') {
                    ++line_;
                    col_ = 1;
                } else {
                    ++col_;
                }
                return ch;
            }

            // Skips spaces/tabs/'\r', and ';'/'#' comments to end of line — but never consumes
            // the newline itself, so the caller can detect and collapse instruction separators.
            auto skip_horizontal_ws_and_comment() -> void {
                while (!at_end()) {
                    const char ch = peek();
                    if (ch == ' ' || ch == '\t' || ch == '\r') {
                        advance();
                    } else if (ch == ';' || ch == '#') {
                        while (!at_end() && peek() != '\n') {
                            advance();
                        }
                    } else {
                        break;
                    }
                }
            }

            auto lex_word(std::vector<AsmToken> &tokens) -> void {
                const auto start = pos_;
                const auto loc = current_location();
                while (!at_end() && is_ident_continue(peek())) {
                    advance();
                }
                const auto raw = std::string(text_.substr(start, pos_ - start));
                const auto lowered = to_lower(raw);

                if (const auto *reg = asm_registers::lookup_register(lowered)) {
                    tokens.push_back(AsmToken{
                        .kind = AsmTokenKind::Register,
                        .text = lowered,
                        .width_bits = reg->width_bits,
                        .location = loc,
                    });
                    return;
                }

                // Not a register — could be a not-yet-supported register-like name (xmm0, cs,
                // cr0, ...) or an ordinary word (mnemonic-by-position or a Mirage variable
                // reference). Either way, the lexer itself only needs to hand back the verbatim
                // spelling; asm_parser decides which of those it is based on operand position
                // and re-checks the unsupported-register table itself.
                tokens.push_back(AsmToken{
                    .kind = AsmTokenKind::Identifier,
                    .text = raw,
                    .location = loc,
                });
            }

            auto lex_number(std::vector<AsmToken> &tokens) -> void {
                const auto start = pos_;
                const auto loc = current_location();

                bool negative = false;
                if (peek() == '-') {
                    negative = true;
                    advance();
                }

                bool is_hex = false;
                if (peek() == '0' && (peek_next() == 'x' || peek_next() == 'X')) {
                    is_hex = true;
                    advance();
                    advance();
                }

                const auto digits_start = pos_;
                if (is_hex) {
                    while (!at_end() && std::isxdigit(static_cast<unsigned char>(peek()))) {
                        advance();
                    }
                } else {
                    while (!at_end() && std::isdigit(static_cast<unsigned char>(peek()))) {
                        advance();
                    }
                }
                const auto digits_end = pos_;

                // A '.' (or exponent marker) immediately following the digits we just scanned
                // means this was actually a float literal — not supported in v1. Consume the
                // rest of the float-shaped token so lexing can resume cleanly afterward, report
                // a diagnostic, and emit no token for it at all.
                if (!is_hex && peek() == '.' && std::isdigit(static_cast<unsigned char>(peek_next()))) {
                    advance(); // '.'
                    while (!at_end() && std::isdigit(static_cast<unsigned char>(peek()))) {
                        advance();
                    }
                    diagnostics_.report_error(DiagnosticStage::Lexer, loc,
                        "float immediates are not supported in inline asm (v1)");
                    return;
                }

                int64_t value = 0;
                const auto [ptr, ec] = std::from_chars(text_.data() + digits_start, text_.data() + digits_end, value, is_hex ? 16 : 10);
                (void)ptr;
                if (ec != std::errc{} || digits_start == digits_end) {
                    diagnostics_.report_error(DiagnosticStage::Lexer, loc, "malformed integer immediate in asm block");
                    return;
                }
                if (negative) {
                    value = -value;
                }

                tokens.push_back(AsmToken{
                    .kind = AsmTokenKind::Immediate,
                    .text = std::string(text_.substr(start, pos_ - start)),
                    .int_value = value,
                    .location = loc,
                });
            }

            auto lex_address_of(std::vector<AsmToken> &tokens) -> void {
                const auto loc = current_location();
                advance(); // '&'
                if (at_end() || !is_ident_start(peek())) {
                    diagnostics_.report_error(DiagnosticStage::Lexer, loc,
                        "expected identifier after '&' in asm operand");
                    return;
                }
                const auto start = pos_;
                while (!at_end() && is_ident_continue(peek())) {
                    advance();
                }
                tokens.push_back(AsmToken{
                    .kind = AsmTokenKind::AddressOf,
                    .text = std::string(text_.substr(start, pos_ - start)),
                    .location = loc,
                });
            }

            auto tokenize() -> std::vector<AsmToken> {
                std::vector<AsmToken> tokens;

                for (;;) {
                    skip_horizontal_ws_and_comment();

                    if (at_end()) {
                        tokens.push_back(AsmToken{.kind = AsmTokenKind::Eof, .location = current_location()});
                        break;
                    }

                    if (peek() == '\n') {
                        const auto loc = current_location();
                        while (!at_end()) {
                            skip_horizontal_ws_and_comment();
                            if (!at_end() && peek() == '\n') {
                                advance();
                            } else {
                                break;
                            }
                        }
                        tokens.push_back(AsmToken{.kind = AsmTokenKind::Newline, .location = loc});
                        continue;
                    }

                    const char ch = peek();
                    const auto loc = current_location();

                    if (is_ident_start(ch)) {
                        lex_word(tokens);
                    } else if (std::isdigit(static_cast<unsigned char>(ch)) ||
                               (ch == '-' && std::isdigit(static_cast<unsigned char>(peek_next())))) {
                        lex_number(tokens);
                    } else if (ch == '&') {
                        lex_address_of(tokens);
                    } else if (ch == ',') {
                        advance();
                        tokens.push_back(AsmToken{.kind = AsmTokenKind::Comma, .location = loc});
                    } else if (ch == ':') {
                        advance();
                        tokens.push_back(AsmToken{.kind = AsmTokenKind::Colon, .location = loc});
                    } else if (ch == '[') {
                        advance();
                        tokens.push_back(AsmToken{.kind = AsmTokenKind::LBracket, .location = loc});
                    } else if (ch == ']') {
                        advance();
                        tokens.push_back(AsmToken{.kind = AsmTokenKind::RBracket, .location = loc});
                    } else {
                        diagnostics_.report_error(DiagnosticStage::Lexer, loc,
                            std::string("unexpected character '") + ch + "' in asm block");
                        advance();
                    }
                }

                return tokens;
            }
        };
    }

    auto tokenize(const std::string_view raw_text, const SourceLocation &block_location, DiagnosticEngine &diagnostics) -> std::vector<AsmToken> {
        Lexer lexer(raw_text, block_location, diagnostics);
        return lexer.tokenize();
    }
}
