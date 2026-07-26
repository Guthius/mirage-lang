#pragma once

#include "asm_lexer.hpp"

#include <span>

class DiagnosticEngine;

namespace ast {
    struct AsmStmt;
}

// Standalone parser turning the asm_lexer's token stream into an ast::AsmStmt. Independent of
// the main Mirage parser (ast_parser.hpp) — it walks its own token kind (asm_lexer::AsmToken),
// not token.hpp's Token.
namespace asm_parser {
    // Unknown mnemonics are never a parse error — they're stored verbatim in the resulting
    // AsmInstruction and left for sema's Tier-1/Tier-2 dispatch to handle. Errors are reported
    // through 'diagnostics' tagged DiagnosticStage::Parser; every token already carries an
    // absolute SourceLocation (seeded by asm_lexer::tokenize), so no location offsetting is
    // needed here.
    auto parse(std::span<const asm_lexer::AsmToken> tokens, DiagnosticEngine &diagnostics) -> ast::AsmStmt;
}
