#pragma once

#include "../analysis.hpp"
#include "compiler/ast.hpp"
#include "compiler/diagnostic_engine.hpp"
#include "compiler/token.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace lsp::handlers {
    // What sits at a cursor position, resolved as far as we can take it:
    // a module-scope symbol, a local variable/parameter, or a member of a
    // struct/union/enum type reached by walking a dotted identifier chain.
    struct Resolution {
        enum class Kind : uint8_t {
            None,
            Symbol, // module_path/symbol identify a sema::Symbol
            Local,
            Param,
            StructField,
            UnionMember,
            EnumField,
            Variant, // tagged-union variant, e.g. `.Invalid` or `TypeName.Invalid`
            Method,
            Builtin, // sizeof/len - synthetic, no declaration site; only .name/.type are set
        };

        Kind kind = Kind::None;
        std::string name;

        // Kind::Symbol
        std::string module_path;
        const sema::Symbol *symbol = nullptr;

        // Kind::Local / Kind::Param / Kind::StructField / Kind::UnionMember /
        // Kind::EnumField / Kind::Variant / Kind::Method: declaration-site location and
        // (best-effort; TypeKind::Invalid if not determinable) resolved type. Kind::Builtin
        // only sets .type (always usize); .location is unused (no declaration site).
        SourceLocation location;
        sema::ResolvedType type;

        const sema::StructField *struct_field = nullptr;
        const sema::UnionMember *union_member = nullptr;
        const sema::MethodInfo *method = nullptr;
    };

    // A stepping-stone while walking a dotted chain: either "we're inside
    // a module namespace" (after resolving an import) or "we're on a
    // value of this resolved type" (after resolving a local/global/field).
    struct Container {
        enum class Kind : uint8_t { None,
                                    Module,
                                    Type };
        Kind kind = Kind::None;
        std::string module_path;
        sema::ResolvedType type;
    };

    struct ParamInfo {
        std::string name;
        sema::ResolvedType type;
        SourceLocation location;
    };

    struct EnclosingFunction {
        std::vector<ParamInfo> params;
        const ast::Stmt *body = nullptr;
    };

    struct LocalInfo {
        SourceLocation location;
        sema::ResolvedType type;
    };

    // Bundles the mutable sema state a local-variable-type lookup needs:
    // resolving the declared type annotation (when present) via
    // sema::resolve_declared_type() needs a mutable Program& and a
    // DiagnosticEngine& (harmless to call again here: types are interned,
    // so this cannot produce new errors or diverge from what sema itself
    // already resolved during body checking).
    struct LocalLookupContext {
        const sema::ProgramModule &sema_module;
        sema::Program &sema_program;
        const std::string &module_path;
        DiagnosticEngine &diag;
    };

    // Maps a resolved sema::Symbol to the Container it becomes when a dotted chain steps
    // through it: an import symbol becomes a Module container, a value symbol (global,
    // resolved type alias) becomes a Type container, anything else (function/macro/
    // unresolved type) is a dead end (Kind::None).
    auto symbol_to_container(const sema::Symbol &symbol) -> Container;

    // Resolves `member` against `type_in`: struct/union field, enum field/tagged-union
    // variant, or method - transparently dereferencing one level of pointer first (so
    // `p.field`/`p.method()` resolve whether `p` is `T` or `*T`). Kind::None if `member`
    // doesn't match anything.
    auto resolve_member(const sema::ResolvedType &type_in, const std::string &member,
                        const sema::Program &program) -> Resolution;

    // One step of a dotted-chain walk: resolves `member` within `container`, returning both
    // the Resolution describing it and the Container to keep chaining from (Kind::None if
    // `member` is a dead end - e.g. a method or a function/macro symbol has no members).
    auto step(const Container &container, const std::string &member, const sema::Program &program)
        -> std::pair<Resolution, Container>;

    // Real [start,end] line containment: finds the FunctionDecl/ExtFunctionDecl/MacroDecl/
    // ImplDecl::Function (from a plain 'impl TYPE' or 'impl TRAIT for TYPE' block) whose
    // span contains `line`, returning its parameter list and (for FunctionDecl/
    // ImplDecl::Function only) its body statement.
    auto find_enclosing_function(const ast::Module &module, const sema::ProgramModule &sema_module,
                                 const std::vector<Token> &tokens, size_t line) -> EnclosingFunction;

    // Mirrors sema_check.cpp's own VarDeclStmt handling: when a declared type annotation is
    // present, it - not the initializer's own natural type - is the variable's actual type.
    auto resolve_var_decl_type(const ast::VarDeclStmt &node, const LocalLookupContext &ctx) -> sema::ResolvedType;

    // Finds the most recent declaration of `name` at or before `before_line` within `body`
    // (a function/method's block statement) - the same "last one wins" shadowing semantics
    // hover/definition use. Returns nullopt if `name` isn't declared anywhere in `body`.
    auto find_local(const ast::Stmt &body, const LocalLookupContext &ctx, const std::string &name,
                    size_t before_line) -> std::optional<LocalInfo>;

    // Collects every distinct local name declared at or before `before_line` within `body`,
    // keeping only the most recent declaration per name (same shadowing semantics as
    // find_local, generalized from "search for one name" to "list every name in scope") -
    // used by completion to offer every local variable currently in scope at the cursor.
    auto collect_locals_in_scope(const ast::Stmt &body, const LocalLookupContext &ctx, size_t before_line)
        -> std::unordered_map<std::string, LocalInfo>;

    // Finds the token whose span contains 1-based (line, column); returns
    // its index into `tokens`, or nullopt if the position isn't on a token.
    auto token_at(const std::vector<Token> &tokens, size_t line, size_t column) -> std::optional<size_t>;

    // Returns the dotted-chain identifier prefix immediately preceding
    // tokens[index], left to right, NOT including tokens[index] itself.
    // E.g. for "io.load_file" with `index` on "load_file", returns ["io"].
    // Empty if tokens[index] isn't preceded by '.', or if the chain's
    // receiver isn't a plain identifier.identifier sequence (e.g. it
    // involves a call or index, as in "f().field").
    //
    // resolve_at() prefers resolving '.name' positions directly off the
    // AST's own (now per-node-precise) MemberExpr/DotIdentExpr/
    // TaggedVariantExpr locations; this token-based reconstruction remains
    // the fallback for module-qualified chains ("greet.hello"), where the
    // chain's base isn't itself a typed value sema records in expr_types.
    auto chain_prefix(const std::vector<Token> &tokens, size_t index) -> std::vector<std::string>;

    // Maps each opening '('/'{'/'[' token's index to its matching closing token's index,
    // found via a single forward pass with one stack (kind-checked: a ')' only closes a '(',
    // etc., so a stray mismatched closer is left unmatched rather than corrupting the rest of
    // the pass). Unmatched brackets - common while typing mid-edit - simply have no entry;
    // callers (containment checks, folding ranges) should treat a missing match as "unknown
    // extent", not an error, since this is a best-effort UI utility, not a parser.
    auto build_bracket_index(const std::vector<Token> &tokens) -> std::unordered_map<size_t, size_t>;

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
