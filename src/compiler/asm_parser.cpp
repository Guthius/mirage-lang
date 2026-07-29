#include "asm_parser.hpp"

#include "asm_registers.hpp"
#include "ast.hpp"
#include "diagnostic_engine.hpp"

#include <cctype>
#include <format>
#include <optional>

namespace asm_parser {
    namespace {
        using asm_lexer::AsmToken;
        using asm_lexer::AsmTokenKind;

        auto to_lower(std::string s) -> std::string {
            for (auto &ch : s) {
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            }
            return s;
        }

        struct Cursor {
            std::span<const AsmToken> tokens;
            size_t pos = 0;
            DiagnosticEngine &diagnostics;

            [[nodiscard]] auto current() const -> const AsmToken & { return tokens[pos]; }
            [[nodiscard]] auto at_end() const -> bool { return current().kind == AsmTokenKind::Eof; }
            [[nodiscard]] auto check(const AsmTokenKind kind) const -> bool { return current().kind == kind; }

            auto advance() -> const AsmToken & {
                const auto &tok = tokens[pos];
                if (!at_end()) {
                    ++pos;
                }
                return tok;
            }

            auto error(const SourceLocation &loc, std::string message) const -> void {
                diagnostics.report_error(DiagnosticStage::Parser, loc, std::move(message));
            }
        };

        // Skips a run of Newline tokens (blank/empty instruction lines).
        auto skip_newlines(Cursor &c) -> void {
            while (c.check(AsmTokenKind::Newline)) {
                c.advance();
            }
        }

        // Consumes a '[' ... ']' span (best-effort brace-depth tracking) so a single unsupported
        // memory operand doesn't cascade into spurious follow-on parse errors.
        auto skip_bracketed_span(Cursor &c) -> void {
            c.advance(); // '['
            int depth = 1;
            while (!c.at_end() && depth > 0) {
                if (c.check(AsmTokenKind::LBracket)) {
                    ++depth;
                } else if (c.check(AsmTokenKind::RBracket)) {
                    --depth;
                    if (depth == 0) {
                        c.advance();
                        return;
                    }
                }
                c.advance();
            }
        }

        auto parse_operand(Cursor &c) -> std::optional<ast::AsmOperand> {
            const auto tok = c.current();
            switch (tok.kind) {
            case AsmTokenKind::Register:
                c.advance();
                return ast::AsmRegisterOperand{.name = tok.text, .width_bits = tok.width_bits, .location = tok.location};

            case AsmTokenKind::Immediate:
                c.advance();
                return ast::AsmImmediateOperand{.value = tok.int_value, .location = tok.location};

            case AsmTokenKind::AddressOf:
                c.advance();
                return ast::AsmVariableOperand{.name = tok.text, .is_address = true, .location = tok.location};

            case AsmTokenKind::Identifier:
                if (asm_registers::is_unsupported_register(to_lower(tok.text))) {
                    c.error(tok.location, std::format("register '{}' is not supported in inline asm (v1)", tok.text));
                    c.advance();
                    return std::nullopt;
                }
                c.advance();
                return ast::AsmVariableOperand{.name = tok.text, .is_address = false, .location = tok.location};

            case AsmTokenKind::LBracket:
                c.error(tok.location,
                    "memory operands with displacement/scale syntax (e.g. '[rax+8]') are not "
                    "supported in inline asm (v1)");
                skip_bracketed_span(c);
                return std::nullopt;

            default:
                c.error(tok.location, "expected an operand (register, immediate, identifier, or '&identifier')");
                // Consume the offending token, as every other branch here does -- otherwise
                // parse_instruction's operand loop breaks on it immediately and fires a
                // second, redundant "expected ',' or end of instruction" at the same column.
                //
                // Comma and Newline are the exception: they are the caller's own structural
                // tokens. Eating a stray ',' would leave the loop unable to see the separator
                // and turn one diagnostic into two; eating a Newline would merge this
                // instruction with the next line.
                if (!c.check(AsmTokenKind::Comma) && !c.check(AsmTokenKind::Newline)) {
                    c.advance();
                }
                return std::nullopt;
            }
        }

        auto parse_instruction(Cursor &c) -> std::optional<ast::AsmInstruction> {
            const auto mnem_tok = c.current();
            if (mnem_tok.kind != AsmTokenKind::Identifier) {
                c.error(mnem_tok.location, "expected an instruction mnemonic");
                while (!c.at_end() && !c.check(AsmTokenKind::Newline)) {
                    c.advance();
                }
                return std::nullopt;
            }
            c.advance();

            ast::AsmInstruction instr;
            instr.mnemonic = to_lower(mnem_tok.text);
            instr.location = mnem_tok.location;

            if (c.check(AsmTokenKind::Newline) || c.at_end()) {
                return instr; // zero-operand instruction (syscall, ret, nop, ...)
            }

            for (;;) {
                if (auto operand = parse_operand(c)) {
                    instr.operands.push_back(std::move(*operand));
                }
                if (c.check(AsmTokenKind::Comma)) {
                    c.advance();
                    continue;
                }
                break;
            }

            if (!c.check(AsmTokenKind::Newline) && !c.at_end()) {
                c.error(c.current().location, "expected ',' or end of instruction");
                while (!c.at_end() && !c.check(AsmTokenKind::Newline)) {
                    c.advance();
                }
            }

            return instr;
        }
    }

    auto parse(const std::span<const AsmToken> tokens, DiagnosticEngine &diagnostics) -> ast::AsmStmt {
        Cursor c{.tokens = tokens, .diagnostics = diagnostics};

        ast::AsmStmt stmt;
        stmt.location = tokens.front().location;

        skip_newlines(c);
        while (!c.at_end()) {
            if (auto instr = parse_instruction(c)) {
                stmt.instructions.push_back(std::move(*instr));
            }
            skip_newlines(c);
        }

        return stmt;
    }
}
