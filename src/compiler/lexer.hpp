#pragma once

#include "diagnostic_engine.hpp"
#include "token.hpp"

#include <unordered_map>
#include <vector>

namespace lexer {
    auto tokenize(std::string_view source, std::string_view filename, DiagnosticEngine &diagnostics) -> std::vector<Token>;

    // The keyword spelling -> TokenKind table the lexer itself matches identifiers against.
    // Exposed so consumers that need the authoritative keyword list (e.g. the LSP's keyword
    // completion) reuse it verbatim instead of hand-maintaining a second copy that could
    // drift as keywords are added or renamed.
    auto keyword_spellings() -> const std::unordered_map<std::string_view, TokenKind> &;
}
