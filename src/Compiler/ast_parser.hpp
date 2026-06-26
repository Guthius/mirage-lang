#pragma once

#include "token.hpp"

namespace ast {
    class Parser {
      public:
        virtual ~Parser() = default;

        virtual auto current() const -> const Token & = 0;
        virtual auto current_lexeme() const -> std::string_view = 0;
        virtual auto current_location() const -> SourceLocation = 0;
        virtual auto at_end() const -> bool = 0;
        virtual auto peek() const -> const Token & = 0;
        virtual auto advance() -> const Token & = 0;
        virtual auto check(TokenKind kind) const -> bool = 0;
        virtual auto match(TokenKind kind) -> bool = 0;
        virtual auto expect(TokenKind kind, std::string_view message) -> const Token = 0;
        virtual auto expect_identifier() -> const std::string & = 0;
        virtual auto report_error(SourceLocation location, std::string message) -> void = 0;
    };
}
