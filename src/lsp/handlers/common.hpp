#pragma once

#include "../analysis.hpp"
#include "compiler/token.hpp"

#include <optional>
#include <string>
#include <vector>

namespace lsp::handlers {
    // What sits at a cursor position, resolved as far as we can take it:
    // a module-scope symbol, a local variable/parameter, or a member of a
    // struct/union/enum type reached by walking a dotted identifier chain.
    struct Resolution {
        enum class Kind : uint8_t {
            None,
            Symbol,       // module_path/symbol identify a sema::Symbol
            Local,
            Param,
            StructField,
            UnionMember,
            EnumField,
            Method,
        };

        Kind kind = Kind::None;
        std::string name;

        // Kind::Symbol
        std::string module_path;
        const sema::Symbol *symbol = nullptr;

        // Kind::Local / Kind::Param / Kind::StructField / Kind::UnionMember /
        // Kind::EnumField / Kind::Method: declaration-site location and
        // (best-effort; TypeKind::Invalid if not determinable) resolved type.
        SourceLocation location;
        sema::ResolvedType type;

        const sema::StructField *struct_field = nullptr;
        const sema::UnionMember *union_member = nullptr;
        const sema::MethodInfo *method = nullptr;
    };

    // Finds the token whose span contains 1-based (line, column); returns
    // its index into `tokens`, or nullopt if the position isn't on a token.
    auto token_at(const std::vector<Token> &tokens, size_t line, size_t column) -> std::optional<size_t>;

    // Returns the dotted-chain identifier prefix immediately preceding
    // tokens[index], left to right, NOT including tokens[index] itself.
    // E.g. for "io.load_file" with `index` on "load_file", returns ["io"].
    // Empty if tokens[index] isn't preceded by '.'.
    //
    // This sidesteps a quirk in the parser: ast::MemberExpr.location is set
    // once per postfix-chain (see parse_postfix) rather than per member
    // token, so it cannot be used to locate which identifier in a chain like
    // "a.b.c" the cursor is actually on - re-deriving the chain from the
    // token stream avoids relying on that field for this purpose.
    auto chain_prefix(const std::vector<Token> &tokens, size_t index) -> std::vector<std::string>;

    // Resolves whatever identifier sits at 1-based (line, column) in the
    // buffer for `path`, a file within `module_path`'s module directory.
    // Handles plain identifiers (locals, params, module symbols) and dotted
    // chains (cross-module member access, struct/union/enum member access),
    // by walking module symbol tables and sema's type tables directly -
    // this mirrors resolve_member's logic in sema_check.cpp without needing
    // to re-run or duplicate sema's own checking.
    auto resolve_at(analysis::ProgramResult &result, const std::string &module_path,
                     const std::string &path, size_t line, size_t column) -> Resolution;
}
