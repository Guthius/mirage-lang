#pragma once

#include "source_location.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

class DiagnosticEngine;

// Standalone lexer for the raw text captured by an 'asm { ... }' block's 'AsmBlock' token.
// Completely independent of the main Mirage lexer (lexer.hpp/token.hpp) — it has its own token
// kind enum and knows nothing about Mirage's grammar beyond "a bare word that isn't a known
// register is a Mirage variable reference."
namespace asm_lexer {
    enum class AsmTokenKind : uint8_t {
        // A bare word that isn't a recognized register name. Could be an instruction mnemonic
        // (if it's the first token on an instruction line) or a Mirage variable reference
        // (otherwise) — asm_parser disambiguates by position, not the lexer, since the lexer has
        // no closed mnemonic table (unknown mnemonics must pass through to sema unmolested).
        Identifier,
        Register,
        Immediate,
        AddressOf, // '&' followed immediately (no whitespace) by an identifier
        Comma,
        Colon,   // reserved for future label/operand-size syntax; unused by v1's grammar
        Newline, // instruction separator; a run of consecutive newlines collapses to one
        LBracket, // '[' / ']' — recognized only so asm_parser can name unsupported memory
        RBracket, // operands ('[rax+8]') precisely instead of a generic lex error
        Eof,
    };

    struct AsmToken {
        AsmTokenKind kind;
        std::string text;        // verbatim spelling for Identifier/AddressOf (case-sensitive
                                  // Mirage variable names); normalized-lowercase for Register
        int64_t int_value = 0;   // populated for Immediate
        uint32_t width_bits = 0; // populated for Register
        SourceLocation location;
    };

    // Tokenizes 'raw_text' (an AsmBlock token's raw lexeme). 'block_location' must be the
    // location of the block's opening '{' (see lexer.cpp's lex_asm_block) — it seeds the
    // returned tokens' line/column/offset so diagnostics inside the asm body point at the
    // right place in the original Mirage source file. Errors are reported through
    // 'diagnostics' tagged DiagnosticStage::Lexer; malformed tokens are skipped rather than
    // aborting the whole block, so the caller always gets a full (possibly degraded) token
    // stream ending in Eof.
    auto tokenize(std::string_view raw_text, const SourceLocation &block_location, DiagnosticEngine &diagnostics) -> std::vector<AsmToken>;
}
