// Lexer robustness tests: token locations and recovery from malformed input.
//
// tests/lexer_asi_test.cpp covers automatic semicolon insertion; this file covers the
// rest of the lexer's contract with the stages above it -- that every token reports the
// line and column it *starts* on, and that malformed input produces a diagnostic without
// losing the rest of the file.

#include "compiler/diagnostic_engine.hpp"
#include "compiler/lexer.hpp"
#include "compiler/source_manager.hpp"
#include "compiler/token.hpp"

#include <cstdio>
#include <string_view>
#include <vector>

namespace {
    int failures = 0;

    void check(const bool condition, const char *name) {
        if (condition) {
            return;
        }
        ++failures;
        std::fprintf(stderr, "FAIL %s\n", name);
    }

    struct LexResult {
        std::vector<Token> tokens;
        size_t error_count;
    };

    auto lex(const std::string_view source) -> LexResult {
        SourceManager source_manager;
        DiagnosticEngine diagnostics(source_manager);
        auto tokens = lexer::tokenize(source, "<test>", diagnostics);
        return LexResult{.tokens = std::move(tokens), .error_count = diagnostics.error_count()};
    }

    auto find_first(const std::vector<Token> &tokens, const TokenKind kind) -> const Token * {
        for (const auto &token : tokens) {
            if (token.kind == kind) {
                return &token;
            }
        }
        return nullptr;
    }

    void expect_location(const char *name, const std::string_view source, const TokenKind kind,
                         const size_t line, const size_t column) {
        const auto result = lex(source);
        const auto *token = find_first(result.tokens, kind);
        if (token == nullptr) {
            ++failures;
            std::fprintf(stderr, "FAIL %s: no %s token produced\n", name, token_kind_name(kind));
            return;
        }
        if (token->location.line == line && token->location.column == column) {
            return;
        }
        ++failures;
        std::fprintf(stderr, "FAIL %s: expected %s at %zu:%zu, got %zu:%zu\n", name,
                     token_kind_name(kind), line, column, token->location.line, token->location.column);
    }

    // Every token must report where it starts. This used to be back-computed as
    // 'col_ - (pos_ - offset)', which is only correct while a token stays on one line.
    void test_token_start_locations() {
        expect_location("int_on_second_line", "x\n  42\n", TokenKind::IntLiteral, 2, 3);
        expect_location("string_start_column", "x = \"abc\"\n", TokenKind::StringLiteral, 1, 5);
        expect_location("char_start_column", "x = 'a'\n", TokenKind::CharLiteral, 1, 5);
        expect_location("ident_after_tabs", "\t\tname\n", TokenKind::Identifier, 1, 3);
        expect_location("multichar_operator", "a\n  <<= 1\n", TokenKind::ShiftLeftEqual, 2, 3);
    }

    // A string literal may not span a raw newline. Before this was enforced, a missing
    // closing quote produced a genuinely multi-line token whose reported column underflowed
    // (size_t is unsigned) to a value near SIZE_MAX, which then crashed the diagnostic
    // printer's caret rendering.
    void test_unterminated_string_stays_on_its_line() {
        const auto result = lex("const s = \"abc\ndef\"\n");
        check(result.error_count > 0, "unterminated string reports an error");

        const auto *string_token = find_first(result.tokens, TokenKind::StringLiteral);
        check(string_token != nullptr, "unterminated string still yields a token");
        if (string_token != nullptr) {
            check(string_token->location.line == 1, "unterminated string reports its own line");
            check(string_token->location.column == 11, "unterminated string reports its start column");
            // The real regression guard: an underflowed column lands near SIZE_MAX.
            check(string_token->location.column < 1000, "unterminated string column does not underflow");
            check(string_token->lexeme.find('\n') == std::string::npos,
                  "unterminated string does not swallow the newline");
        }

        // The rest of the file must still be lexed rather than absorbed into the literal.
        check(find_first(result.tokens, TokenKind::Identifier) != nullptr,
              "content after an unterminated string is still tokenized");
    }

    void test_unterminated_char_stays_on_its_line() {
        const auto result = lex("x = 'a\ny = 1\n");
        check(result.error_count > 0, "unterminated char reports an error");

        const auto *char_token = find_first(result.tokens, TokenKind::CharLiteral);
        check(char_token != nullptr, "unterminated char still yields a token");
        if (char_token != nullptr) {
            check(char_token->location.line == 1, "unterminated char reports its own line");
            check(char_token->location.column < 1000, "unterminated char column does not underflow");
            check(char_token->lexeme.find('\n') == std::string::npos,
                  "unterminated char does not swallow the newline");
        }
    }

    // A trailing backslash must not splice the following line into the literal.
    void test_backslash_before_newline_does_not_continue_literal() {
        const auto result = lex("x = \"abc\\\ndef\"\n");
        check(result.error_count > 0, "backslash-newline in a string reports an error");

        const auto *string_token = find_first(result.tokens, TokenKind::StringLiteral);
        if (string_token != nullptr) {
            check(string_token->location.line == 1, "backslash-newline string reports its own line");
            check(string_token->lexeme.find('\n') == std::string::npos,
                  "backslash-newline does not splice the next line into the literal");
        }
    }

    // An unrecognized byte used to be reported and then returned as a synthesized Eof
    // token, which tokenize()'s loop treats as end-of-input -- so one stray character
    // silently removed everything after it from the token stream.
    void test_stray_byte_does_not_truncate_the_file() {
        const auto result = lex("a = 1\n`\nb = 2\nc = 3\nd = 4\n");
        check(result.error_count == 1, "one stray byte reports exactly one error");

        // Everything after the stray byte must still be tokenized.
        int identifiers = 0;
        for (const auto &token : result.tokens) {
            if (token.kind == TokenKind::Identifier) {
                ++identifiers;
            }
        }
        check(identifiers == 4, "identifiers before and after a stray byte are all lexed");

        check(!result.tokens.empty() && result.tokens.back().kind == TokenKind::Eof,
              "token stream still ends with a real Eof");

        // The fabricated Eof also carried the stray byte as its lexeme.
        for (const auto &token : result.tokens) {
            if (token.kind == TokenKind::Eof) {
                check(token.lexeme.empty(), "Eof token carries no lexeme");
            }
        }
    }

    // A stray byte must not disturb ASI state: it is not a real token, so the trigger
    // status of the last real token has to survive it.
    void test_stray_byte_preserves_asi_state() {
        const auto result = lex("x`\ny\n");
        check(result.error_count == 1, "stray byte adjacent to an identifier reports once");

        int semicolons = 0;
        for (const auto &token : result.tokens) {
            if (token.kind == TokenKind::Semicolon) {
                ++semicolons;
            }
        }
        check(semicolons == 2, "ASI still fires across a stray byte");
    }

    // The <cctype> classifiers are undefined for a plain char with the high bit set, so
    // any high-bit byte reaching is_digit/is_alpha/... was UB. These exercise the three
    // realistic sources of such bytes.
    void test_high_bit_bytes_are_handled() {
        // A UTF-8 BOM before otherwise valid source.
        const auto bom = lex("\xEF\xBB\xBFpub fn f() {}\n");
        check(find_first(bom.tokens, TokenKind::Identifier) != nullptr,
              "source after a UTF-8 BOM is still lexed");

        // Non-ASCII inside a string literal is just payload bytes.
        const auto in_string = lex("s = \"caf\xC3\xA9\"\n");
        check(in_string.error_count == 0, "non-ASCII inside a string literal is not an error");
        check(find_first(in_string.tokens, TokenKind::StringLiteral) != nullptr,
              "string literal with non-ASCII content still lexes");

        // Non-ASCII inside a comment is skipped with the rest of the comment.
        const auto in_comment = lex("// caf\xC3\xA9\nx = 1\n");
        check(in_comment.error_count == 0, "non-ASCII inside a comment is not an error");
        check(find_first(in_comment.tokens, TokenKind::Identifier) != nullptr,
              "code after a comment containing non-ASCII still lexes");
    }

    // Escapes inside a well-formed literal must keep working unchanged.
    void test_valid_escapes_still_lex() {
        const auto result = lex("a = \"x\\\"y\"\nb = '\\n'\nc = '\\x41'\n");
        check(result.error_count == 0, "valid escapes produce no errors");
        check(find_first(result.tokens, TokenKind::StringLiteral) != nullptr,
              "escaped quote inside a string still lexes");
    }
}

auto main() -> int {
    test_token_start_locations();
    test_unterminated_string_stays_on_its_line();
    test_unterminated_char_stays_on_its_line();
    test_backslash_before_newline_does_not_continue_literal();
    test_stray_byte_does_not_truncate_the_file();
    test_stray_byte_preserves_asi_state();
    test_high_bit_bytes_are_handled();
    test_valid_escapes_still_lex();

    if (failures > 0) {
        std::fprintf(stderr, "%d lexer robustness test(s) failed\n", failures);
        return 1;
    }
    std::printf("all lexer robustness tests passed\n");
    return 0;
}
