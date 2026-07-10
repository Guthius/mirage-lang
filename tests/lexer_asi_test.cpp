#include "compiler/diagnostic_engine.hpp"
#include "compiler/lexer.hpp"
#include "compiler/source_manager.hpp"
#include "compiler/token.hpp"

#include <cstdio>
#include <initializer_list>
#include <string_view>

namespace {
    int failures = 0;

    auto kind_name(const TokenKind kind) -> const char * {
        return token_kind_name(kind);
    }

    void expect_kinds(const char *name, const std::string_view source, const std::initializer_list<TokenKind> expected) {
        SourceManager source_manager;
        DiagnosticEngine diagnostics(source_manager);

        const auto tokens = lexer::tokenize(source, "<test>", diagnostics);

        bool ok = tokens.size() == expected.size();
        if (ok) {
            size_t i = 0;
            for (const auto expected_kind : expected) {
                if (tokens[i].kind != expected_kind) {
                    ok = false;
                    break;
                }
                ++i;
            }
        }

        if (ok) {
            return;
        }

        ++failures;
        std::fprintf(stderr, "FAIL %s\n  expected:", name);
        for (const auto expected_kind : expected) {
            std::fprintf(stderr, " %s", kind_name(expected_kind));
        }
        std::fprintf(stderr, "\n  actual:  ");
        for (const auto &token : tokens) {
            std::fprintf(stderr, " %s", kind_name(token.kind));
        }
        std::fprintf(stderr, "\n");
    }
}

auto main() -> int {
    using TK = TokenKind;

    // Original bug class: an identifier ends a line, the next line starts with a prefix
    // operator that's also binary-infix. Without ASI these silently merge into one expression.
    expect_kinds("ident_then_prefix_minus", "x\n-1\n",
        {TK::Identifier, TK::Semicolon, TK::Minus, TK::IntLiteral, TK::Semicolon, TK::Eof});

    expect_kinds("ident_then_prefix_ampersand", "x\n&y\n",
        {TK::Identifier, TK::Semicolon, TK::Ampersand, TK::Identifier, TK::Semicolon, TK::Eof});

    // Trigger set coverage: one case per token kind that should end a statement.
    expect_kinds("int_literal_line_end", "x = 1\ny = 2\n",
        {TK::Identifier, TK::Equal, TK::IntLiteral, TK::Semicolon,
         TK::Identifier, TK::Equal, TK::IntLiteral, TK::Semicolon, TK::Eof});

    expect_kinds("float_literal_line_end", "x = 1.5\ny = 2\n",
        {TK::Identifier, TK::Equal, TK::FloatLiteral, TK::Semicolon,
         TK::Identifier, TK::Equal, TK::IntLiteral, TK::Semicolon, TK::Eof});

    expect_kinds("string_literal_line_end", "x = \"a\"\ny = 2\n",
        {TK::Identifier, TK::Equal, TK::StringLiteral, TK::Semicolon,
         TK::Identifier, TK::Equal, TK::IntLiteral, TK::Semicolon, TK::Eof});

    expect_kinds("char_literal_line_end", "x = 'a'\ny = 2\n",
        {TK::Identifier, TK::Equal, TK::CharLiteral, TK::Semicolon,
         TK::Identifier, TK::Equal, TK::IntLiteral, TK::Semicolon, TK::Eof});

    expect_kinds("kw_true_line_end", "x = true\ny = 2\n",
        {TK::Identifier, TK::Equal, TK::KwTrue, TK::Semicolon,
         TK::Identifier, TK::Equal, TK::IntLiteral, TK::Semicolon, TK::Eof});

    expect_kinds("kw_false_line_end", "x = false\ny = 2\n",
        {TK::Identifier, TK::Equal, TK::KwFalse, TK::Semicolon,
         TK::Identifier, TK::Equal, TK::IntLiteral, TK::Semicolon, TK::Eof});

    expect_kinds("kw_nil_line_end", "x = nil\ny = 2\n",
        {TK::Identifier, TK::Equal, TK::KwNil, TK::Semicolon,
         TK::Identifier, TK::Equal, TK::IntLiteral, TK::Semicolon, TK::Eof});

    expect_kinds("rparen_line_end", "f()\ng()\n",
        {TK::Identifier, TK::LParen, TK::RParen, TK::Semicolon,
         TK::Identifier, TK::LParen, TK::RParen, TK::Semicolon, TK::Eof});

    expect_kinds("rbrace_line_end", "{ x }\ny\n",
        {TK::LBrace, TK::Identifier, TK::RBrace, TK::Semicolon, TK::Identifier, TK::Semicolon, TK::Eof});

    expect_kinds("rbracket_line_end", "a[0]\nb\n",
        {TK::Identifier, TK::LBracket, TK::IntLiteral, TK::RBracket, TK::Semicolon,
         TK::Identifier, TK::Semicolon, TK::Eof});

    expect_kinds("postfix_incr_line_end", "x++\ny--\n",
        {TK::Identifier, TK::PlusPlus, TK::Semicolon, TK::Identifier, TK::MinusMinus, TK::Semicolon, TK::Eof});

    expect_kinds("return_terminates", "return x\ny\n",
        {TK::KwReturn, TK::Identifier, TK::Semicolon, TK::Identifier, TK::Semicolon, TK::Eof});

    expect_kinds("break_continue_terminate", "break\ncontinue\n",
        {TK::KwBreak, TK::Semicolon, TK::KwContinue, TK::Semicolon, TK::Eof});

    // Intentional continuation: a trailing binary operator must NOT trigger insertion.
    expect_kinds("no_split_trailing_binary_op", "x +\ny\n",
        {TK::Identifier, TK::Plus, TK::Identifier, TK::Semicolon, TK::Eof});

    expect_kinds("no_split_trailing_comma", "f(\n  x,\n  y,\n)\n",
        {TK::Identifier, TK::LParen, TK::Identifier, TK::Comma, TK::Identifier, TK::Comma, TK::RParen,
         TK::Semicolon, TK::Eof});

    // Explicit semicolons keep working, and don't get double-inserted.
    expect_kinds("explicit_semicolon_unchanged", "x;\ny\n",
        {TK::Identifier, TK::Semicolon, TK::Identifier, TK::Semicolon, TK::Eof});

    // EOF extension: last statement terminates even with no trailing newline.
    expect_kinds("eof_extension_no_trailing_newline", "x = 1",
        {TK::Identifier, TK::Equal, TK::IntLiteral, TK::Semicolon, TK::Eof});

    // ...but not when the file ends on a dangling operator (not a trigger).
    expect_kinds("eof_extension_suppressed_after_operator", "x +",
        {TK::Identifier, TK::Plus, TK::Eof});

    // '.*' must tokenize as plain Dot Star, not a combined/misparsed token.
    expect_kinds("dot_star_tokenizes_as_two_tokens", "p.*\n",
        {TK::Identifier, TK::Dot, TK::Star, TK::Semicolon, TK::Eof});

    // 'Star' is dual-purpose: bare Star (binary multiply) must NOT trigger ASI, since
    // 'a *\n b' is an intentional continuation...
    expect_kinds("no_split_trailing_multiply", "a *\nb\n",
        {TK::Identifier, TK::Star, TK::Identifier, TK::Semicolon, TK::Eof});

    // ...but Star immediately preceded by Dot (postfix deref 'expr.*') completes a statement
    // and MUST trigger ASI, or 'p.*\n(x)' would silently merge into a call 'p.*(x)'.
    expect_kinds("dot_star_triggers_asi", "p.*\n(x)\n",
        {TK::Identifier, TK::Dot, TK::Star, TK::Semicolon,
         TK::LParen, TK::Identifier, TK::RParen, TK::Semicolon, TK::Eof});

    if (failures > 0) {
        std::fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }

    std::puts("all lexer ASI tests passed");
    return 0;
}
