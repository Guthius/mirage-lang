#pragma once

#include "token.hpp"

class DiagnosticEngine;

namespace ast {
    class Parser {
      public:
        virtual ~Parser() = default;

        virtual auto diagnostics() -> DiagnosticEngine & = 0;
        virtual auto current() const -> const Token & = 0;
        virtual auto current_lexeme() const -> std::string_view = 0;
        virtual auto current_location() const -> SourceLocation = 0;
        virtual auto at_end() const -> bool = 0;
        // Token 'offset' positions ahead of the cursor; offset 0 is current(). Clamps to the
        // final (Eof) token rather than going out of range, so a lookahead near the end of
        // input needs no separate bounds check at the call site.
        //
        // peek()/peek_next() are the offset-1 and offset-2 cases, kept because they read
        // better at the many call sites that only need one or two. Reach for peek_at when a
        // decision genuinely needs more: the tagged-variant-vs-match-arm disambiguation in
        // parse_postfix turns on the token at offset 3.
        virtual auto peek_at(size_t offset) const -> const Token & = 0;
        virtual auto peek() const -> const Token & = 0;
        virtual auto peek_next() const -> const Token & = 0;
        virtual auto advance() -> const Token & = 0;
        virtual auto check(TokenKind kind) const -> bool = 0;
        virtual auto check_at(size_t offset, TokenKind kind) const -> bool = 0;
        virtual auto check_next(TokenKind kind) const -> bool = 0;
        virtual auto match(TokenKind kind) -> bool = 0;
        virtual auto match_identifier(std::string_view lexeme) -> bool = 0;
        virtual auto expect(TokenKind kind, std::string_view message) -> const Token = 0;
        virtual auto expect_identifier() -> const std::string & = 0;
        virtual auto report_error(SourceLocation location, std::string message) -> void = 0;
        virtual auto has_reached_max_errors() const -> bool = 0;

        // Set while parsing the UNBRACED body of a switch arm, where a top-level ',' is the
        // arm separator: comma-greedy statements (a multi-value 'return'/'return_ok') must
        // stop at it instead of consuming it as a value separator ('0: return 1,' used to
        // swallow the comma and cascade parse errors into the next arm). Cleared for the
        // duration of any braced block — '0: { return a, b },' keeps the multi-return
        // reading. Managed via ScopedCommaTerminatesStmt (ast.cpp), never written directly.
        bool comma_terminates_stmt = false;
    };
}
