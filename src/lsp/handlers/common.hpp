#pragma once

#include "../analysis.hpp"
#include "compiler/ast.hpp"
#include "compiler/diagnostic_engine.hpp"
#include "compiler/token.hpp"

#include <optional>
#include <span>
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
            Builtin, // size_of/align_of/len - synthetic, no declaration site; only .name/.type/
                     // .builtin_operand_type/.builtin_const_value are set
            AsmRegister, // a register operand inside an 'asm { ... }'/'asm -> reg { ... }' body -
                         // synthetic like Builtin; only .name/.asm_register_* are set
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

        // Kind::Param only: see ParamInfo::display_override's doc comment - carries the same
        // AST-rendered fallback through to hover/definition/references display code.
        std::optional<std::string> display_override;

        const sema::StructField *struct_field = nullptr;
        const sema::UnionMember *union_member = nullptr;
        const sema::MethodInfo *method = nullptr;
        // Kind::Method reached through a TRAIT HANDLE receiver ('shape.draw()' where
        // 'shape: Drawable'). The method is declared on the trait itself, so there is no
        // MethodInfo for it -- 'method' stays null and this is set instead.
        const sema::TraitMethodInfo *trait_method = nullptr;

        // Kind::Builtin only: the operand's own resolved type (e.g. 'size_of(Point)''s
        // 'Point', 'len(arr)''s array/slice type) and, when statically known (size_of/align_of
        // always; len only for a fixed-size array, never a slice), the folded value.
        sema::ResolvedType builtin_operand_type;
        std::optional<uint64_t> builtin_const_value;

        // Kind::AsmRegister only: straight from asm_registers.hpp's table.
        uint32_t asm_register_width_bits = 0;
        std::string asm_register_family;
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

        // Set only for a param inside a generic decl's own template body whose declared
        // type references the enclosing generic_params (e.g. 'value: T') - such a type has
        // no concrete ResolvedType at all outside an actual instantiation, so 'type' above
        // is unreliable (may resolve to TypeKind::Invalid). When set, display code should
        // prefer this AST-rendered string ("T", "*T", "[N]u8", ...) over type_to_string(type).
        std::optional<std::string> display_override;
    };

    struct EnclosingFunction {
        std::vector<ParamInfo> params;
        const ast::Stmt *body = nullptr;

        // Non-null only when the enclosing decl (or, for a method, its enclosing 'impl'
        // block) is itself generic - the fn's own generic_params, or the impl's own
        // generic_params for a method. Together with 'self_target' below, lets
        // resolve_var_decl_type's last-resort shadow-instantiation fallback (see its own doc
        // comment) build a synthetic instantiation for real sema type-checking. Only reached
        // for the few shapes the eager template pass does not cover; everything else answers
        // from 'fn_decl'/'method_decl' below.
        const std::vector<ast::GenericParam> *generic_params = nullptr;
        // Set only for a method belonging to a generic 'impl' block - the impl's own
        // 'target' NamedType (e.g. 'List' in 'impl List[T: type] {...}'), used to build a
        // shadow instantiation for 'self'.
        const ast::NamedType *self_target = nullptr;
        // The enclosing decl's own declared return types (empty vector for a void fn, never
        // null unless there's no enclosing decl at all) - shadow_instantiate_and_resolve
        // needs the last one to reconstruct 'fn_error_type' for a 'try' expression inside a
        // generic template body (check_expr errors out immediately without it).
        const std::vector<ast::Type> *return_types = nullptr;

        // The declaration itself - exactly one of the two is set for anything with a body.
        // One field covers 'impl TYPE' and 'impl TRAIT for TYPE' alike, since
        // TraitImplDecl::functions is a vector<ImplDecl::Function> too.
        //
        // Set unconditionally, generic or not: they are the key into
        // Program::template_fn_instance_for_decl / template_method_instance_for_decl, which
        // simply misses for a non-generic decl (whose types are in the module's tables where
        // they belong). See template_exprs_for() in common.cpp.
        const ast::FunctionDecl *fn_decl = nullptr;
        const ast::ImplDecl::Function *method_decl = nullptr;
    };

    struct LocalInfo {
        SourceLocation location;
        sema::ResolvedType type;

        // See VarDeclTypeResult's doc comment - carried through from resolve_var_decl_type
        // when a ':='-inferred local's own type could only be recovered via AST fallback.
        std::optional<std::string> display_override;
    };

    // resolve_var_decl_type's result: 'type' is the ordinary sema-resolved answer, found via
    // find_expr_type (so it covers generic bodies, whose records live in the enclosing
    // declaration's template instance rather than the module's tables).
    //
    // 'display_override' is the escape hatch for the one shape a ResolvedType renders less
    // faithfully than the source does: an explicit generic call ('mut list := make_list[T](a)'),
    // whose recorded type is 'List[Opaque]' and therefore prints the parameter's name only
    // when the callee has a single type parameter. When set, 'type' stays at its default and
    // this AST-rendered string ("List[T]") is what display code must use.
    struct VarDeclTypeResult {
        sema::ResolvedType type;
        std::optional<std::string> display_override;
    };

    // Bundles the mutable sema state a local-variable-type lookup needs:
    // resolving the declared type annotation (when present) via
    // sema::resolve_declared_type() needs a mutable Program& and a
    // DiagnosticEngine& (harmless to call again here: types are interned,
    // so this cannot produce new errors or diverge from what sema itself
    // already resolved during body checking).
    //
    // 'tokens'/'program_result' are optional (null for callers that don't have them handy) -
    // needed only to resolve a VarDeclGroupStmt local's real per-name type (a multi-return
    // group declaration's own per-name types aren't recorded anywhere by sema; the only way to
    // get them back is to re-resolve the initializer call's callee - see
    // resolve_group_decl_name_type). Without them, a VarDeclGroupStmt local's type degrades to
    // the void default, same as before this pair of fields existed.
    //
    // 'template_exprs' is the ExprSideTables of the eagerly-checked template instance of the
    // generic declaration the query lands in, or null everywhere else (ordinary code, or a
    // shape the eager pass doesn't reach). It exists because a generic body's types are
    // recorded ONLY in that instance's tables and never in the module's - see
    // Program::template_fn_instance_for_decl. Every expr_types-style lookup in the LSP must go
    // through find_expr_type()/find_expr_record_in() with this, or it silently misses for every
    // expression inside every generic declaration.
    struct LocalLookupContext {
        const sema::ProgramModule &sema_module;
        sema::Program &sema_program;
        const std::string &module_path;
        DiagnosticEngine &diag;
        const std::vector<Token> *tokens = nullptr;
        analysis::ProgramResult *program_result = nullptr;
        const sema::ExprSideTables *template_exprs = nullptr;
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
    // Resolves 'member' as an enum field, a tagged-union variant, or a bitset flag of
    // 'type'. Shared by resolve_member and by the reference walker, which needs the same
    // matching for bare '.Variant'/'.Flag' expressions.
    auto match_enum_or_variant(const sema::ResolvedType &type, const std::string &member,
                               const sema::Program &program) -> Resolution;

    auto resolve_member(const sema::ResolvedType &type_in, const std::string &member,
                        const sema::Program &program) -> Resolution;

    // One step of a dotted-chain walk: resolves `member` within `container`, returning both
    // the Resolution describing it and the Container to keep chaining from (Kind::None if
    // `member` is a dead end - e.g. a method or a function/macro symbol has no members).
    auto step(const Container &container, const std::string &member, const sema::Program &program)
        -> std::pair<Resolution, Container>;

    // Real [start,end] line containment: finds the FunctionDecl/ExtFunctionDecl/MacroDecl/
    // ImplDecl::Function (from a plain 'impl TYPE' or 'impl TRAIT for TYPE' block) or trait
    // method (from a 'type T = trait { ... }' declaration) whose span contains `line`,
    // returning its parameter list (a trait method's own 'self' is given the trait's own
    // handle type, there being no concrete Self type to report) and (for FunctionDecl/
    // ImplDecl::Function only - a trait method is signature-only, never has a body) its body
    // statement.
    //
    // For a GENERIC fn/impl (non-empty 'generic_params' on the FunctionDecl or the
    // enclosing ImplDecl), sema never resolves the template's own param types (a param
    // typed 'T' only means something once a concrete instantiation exists) - 'program'/
    // 'module_path'/'diag' let this fall back to resolving each param's type straight from
    // its AST annotation instead (works for a param whose type doesn't itself reference a
    // generic parameter, e.g. 'index: usize'; a param typed 'T'/'[N]u8' resolves to
    // TypeKind::Invalid via the ordinary "unknown identifier" path, same graceful
    // degradation a truly-unresolvable type already gets elsewhere). 'self' is omitted
    // entirely for a generic method (no concrete receiver type exists to report) rather
    // than showing a wrong placeholder.
    auto find_enclosing_function(const ast::Module &module, const sema::ProgramModule &sema_module,
                                 sema::Program &program, const std::string &module_path, DiagnosticEngine &diag,
                                 const std::vector<Token> &tokens, size_t line) -> EnclosingFunction;

    // The eagerly-checked template instance's records for whichever declaration 'enclosing'
    // names, or null - the value LocalLookupContext::template_exprs wants. Null for a
    // non-generic decl and for a decl the eager pass doesn't reach; both degrade to a plain
    // module lookup, which is the right answer in the first case and the pre-existing
    // behaviour in the second.
    auto template_exprs_for(const EnclosingFunction &enclosing, const sema::Program &program)
        -> const sema::ExprSideTables *;

    // The type sema recorded for 'expr' - THE expression-type lookup for LSP handlers, and the
    // only one that works inside a generic declaration's body.
    //
    // A plain 'sema_module.exprs.expr_types' lookup misses every node in every generic body:
    // the eager template pass records those in the declaration's own template instance instead
    // (see Program::template_fn_instance_for_decl). This consults ctx.template_exprs first and
    // falls back to the module, exactly mirroring sema's own find_expr_record asymmetry.
    // Returns null when neither has a record.
    auto find_expr_type(const LocalLookupContext &ctx, const ast::Expr &expr) -> const sema::ResolvedType *;

    // Mirrors sema_check.cpp's own VarDeclStmt handling: when a declared type annotation is
    // present, it - not the initializer's own natural type - is the variable's actual type.
    // See VarDeclTypeResult's doc comment for the AST-fallback case.
    auto resolve_var_decl_type(const ast::VarDeclStmt &node, const LocalLookupContext &ctx) -> VarDeclTypeResult;

    // Finds the most recent declaration of `name` at or before `before_line` within `body`
    // (a function/method's block statement) - the same "last one wins" shadowing semantics
    // hover/definition use. Returns nullopt if `name` isn't declared anywhere in `body`.
    auto find_local(const ast::Stmt &body, const LocalLookupContext &ctx, const std::string &name,
                    size_t before_line) -> std::optional<LocalInfo>;

    // The bare-identifier resolution every feature shares: params first, then locals
    // declared in scope before `use_line` (via find_local), then module-scope symbols
    // (ctx.sema_module / ctx.module_path). Carries display_override through on params and
    // locals - the completion/references copies this replaced had drifted and dropped it.
    auto resolve_base_name(const std::string &name, size_t use_line, const std::vector<ParamInfo> &params,
                           const ast::Stmt *body, const LocalLookupContext &ctx) -> std::optional<Resolution>;

    // The Container a resolved base starts a dotted chain from: a symbol goes through
    // symbol_to_container, anything else is a value of its resolved type.
    auto chain_start(const Resolution &base) -> Container;

    // Steps `container` through each of `names` in order via step(); Kind::None from the
    // first dead end onward.
    auto walk_chain(Container container, std::span<const std::string> names, const sema::Program &program)
        -> Container;

    // Collects every distinct local name declared at or before `before_line` within `body`,
    // keeping only the most recent declaration per name (same shadowing semantics as
    // find_local, generalized from "search for one name" to "list every name in scope") -
    // used by completion to offer every local variable currently in scope at the cursor.
    auto collect_locals_in_scope(const ast::Stmt &body, const LocalLookupContext &ctx, size_t before_line)
        -> std::unordered_map<std::string, LocalInfo>;

    // The half-open [start, end) column span a token occupies, 1-based.
    //
    // Exists because the TWO cursor conventions in this file both need it and must not be
    // "unified" by accident (LSPH-10):
    //
    //   token_at, below, asks "is the cursor ON this token" — 'column >= start && column < end'.
    //     Used by hover/definition/references, where the cursor sits somewhere within the
    //     identifier the user is asking about.
    //
    //   completion.cpp's cursor classifier asks "did the cursor just FOLLOW this token" —
    //     'column > start && column <= end'. That inversion is not a bug: it is what lets
    //     completion tell "cursor mid-token, filter by the typed prefix" from "cursor sits
    //     immediately after this token's last character, nothing typed yet", and the second
    //     case yields an anchor index one PAST the matched token — something token_at can
    //     never return.
    //
    // Both are correct for their question. Neither is correct for the other's.
    struct TokenSpan {
        size_t start;
        size_t end;
    };
    auto token_span(const Token &token) -> TokenSpan;

    // The '{' of the braced literal enclosing token 'index', if any — walking out over
    // balanced brackets. Used to resolve a leading-dot name against the literal's own type
    // ('.field' in a struct literal, '.Flag' in a bitset one) rather than against whatever
    // else happens to be in scope.
    auto enclosing_struct_literal_brace(const std::vector<Token> &tokens, size_t index) -> std::optional<SourceLocation>;

    // The Expr node whose own location is exactly 'target', searched depth-first from a
    // statement. Source locations are unique per position, so at most one node matches.
    // Returns nullptr if none does — which for a mid-edit buffer is the common case, since a
    // half-written expression usually fails to parse into the tree at all.
    auto find_expr_by_location(const ast::Stmt &stmt, const SourceLocation &target) -> const ast::Expr *;

    // Finds the token whose span CONTAINS 1-based (line, column) — see token_span for why
    // that is not the same test completion uses. Returns its index into `tokens`, or nullopt
    // if the position isn't on a token.
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

    // Same as resolve_at, but reuses an already-tokenized `tokens` - avoids re-tokenizing the
    // same file once per call site (e.g. inlay hints resolving many call-expression callees
    // in one pass over a file).
    auto resolve_at_tokens(analysis::ProgramResult &result, const std::string &module_path,
                           const std::vector<Token> &tokens, size_t line, size_t column) -> Resolution;

    // Resolves a CallExpr's own callee down to the token position of the actually-called name:
    // the identifier itself for a plain call ('foo(...)'), or the member name for a
    // method/module-qualified call ('x.foo(...)') - MemberExpr::location is the '.' token's own
    // position (see parse_postfix's per-iteration 'location' capture, ast.cpp), so the member
    // name is the very next token in the stream. Shared by inlay hints (resolving a call's
    // parameter names) and resolve_group_decl_name_type below (resolving a group declaration's
    // per-name return types) - both need "what does this call actually call" and neither can
    // get it from the callee ast::Expr's own location directly.
    auto callee_name_location(const ast::Expr &callee, const std::vector<Token> &tokens) -> std::optional<SourceLocation>;

    // Declared return types for a resolved callee, in the order sema would return them - i.e.
    // ast::FunctionDecl/ast::ImplDecl::Function's own multi-return list. Empty if `res` isn't a
    // function/method with a statically-known return-type list (an ext fn/macro never
    // multi-returns; a call through a function-pointer-typed variable isn't Kind::Symbol/Method
    // at all).
    auto callee_return_types(const Resolution &res) -> std::vector<sema::ResolvedType>;

    // Resolves the real per-name type of the `name_index`-th name in a 'const a, b := f()' (or
    // 'const a, b := try f()') group declaration, by re-resolving the initializer call's callee
    // (via callee_name_location + resolve_at_tokens) and reading off its return-types list -
    // mirrors sema_check.cpp's own VarDeclGroupStmt handling (check_group_call_returns), which
    // computes this exact list but only into a local variable, never anywhere sema records
    // per-name - so hover/completion have no other way to recover it. Strips the trailing
    // error(...) slot for the 'try' form, same as sema does. Needs `ctx.tokens`/
    // `ctx.program_result` populated (see LocalLookupContext's own doc comment) - returns the
    // void default if either is null, or if resolution fails for any reason (unresolved callee,
    // index out of range, malformed group decl already reported elsewhere as its own diagnostic).
    auto resolve_group_decl_name_type(const ast::VarDeclGroupStmt &node, size_t name_index,
                                       const LocalLookupContext &ctx) -> sema::ResolvedType;

    // Raw source text of a module-scope const/mut's initializer: everything from just after its
    // ':='/'=' token up to (not including) the declaration's own terminating ';'. `decl_location`
    // is the VarDecl's own location (its 'const'/'mut' keyword - see parse_var_decl in ast.cpp);
    // deliberately NOT the initializer ast::Expr's own location, since a compound top-level
    // initializer expression's location can be its operator's position rather than its first
    // token (this AST stores one defining-token point per node, never a true span). Empty if
    // `decl_location` doesn't land on a real token, or no ':='/'=' is found after it.
    auto raw_const_init_text(const std::vector<Token> &tokens, std::string_view source_text,
                              const SourceLocation &decl_location) -> std::string;
}
