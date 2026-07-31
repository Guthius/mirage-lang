#include "sema.hpp"

#include "asm_registers.hpp"

#include <algorithm>
#include <format>
#include <ranges>
#include <unordered_set>

namespace sema {
    // Forward-declared: defined further below (their own signature-inference logic is
    // shared verbatim with check_expr's ordinary CallExpr case), needed earlier in this file
    // by check_group_call_returns's own explicit/inferred generic-instantiation handling.
    auto resolve_explicit_generic_args(const std::vector<ast::GenericParam> &params,
                                        const std::vector<ast::GenericArg> &args, const std::string &module_path,
                                        Program &program, DiagnosticEngine &diag, const SourceLocation &loc,
                                        const std::string &decl_name) -> std::optional<std::vector<GenericArgValue>>;
    auto infer_generic_function_args(const ast::FunctionDecl &decl, const std::vector<ast::Expr> &call_args,
                                      const std::optional<ResolvedType> &expected, LocalScope &locals,
                                      const std::string &module_path, Program &program, DiagnosticEngine &diag,
                                      const SourceLocation &loc, const int loop_depth, const int defer_loop_base,
                                      const ResolvedType *fn_error_type) -> std::optional<std::vector<GenericArgValue>>;

    // Which error-typed local a condition narrows, and to what state in each branch.
    // Declared up here, at the same scope as its definition further below, so check_expr's
    // TernaryExpr/WhenExpr cases can apply the same narrowing check_stmt's IfStmt/WhileStmt do.
    struct ConditionNarrowing {
        std::string var_name;
        ErrorState then_state;
        ErrorState else_state;
        // Set only for the two SIMPLE (non-compound) shapes 'err' / '!err'. Gates both the
        // early-return-narrowing rule and the redundant-check warning — per spec, neither
        // applies to compound conditions like 'err && x'.
        bool is_exact_err = false;
        bool is_exact_not_err = false;
    };

    // Possibly several: 'err1 && err2' narrows both, which a single-value return cannot
    // express. Empty means the condition's shape proves nothing about any error local.
    auto compute_condition_narrowing(const ast::Expr &condition, LocalScope &locals, const Program &program) -> std::vector<ConditionNarrowing>;

    // Whether the condition mentions an error local at all in a position the narrowing rules
    // cannot use. Drives the extra hint on "unknown state" -- see its use below.
    auto condition_mentions_unnarrowable_error(const ast::Expr &condition, LocalScope &locals, const Program &program) -> std::optional<std::string>;

    // Applies a condition's narrowings to one branch's scope, and records why any error local
    // the condition merely MENTIONS was left Unknown. Used by all four branching forms
    // (if/while/ternary/when) so they agree on both.
    auto apply_condition_narrowing(LocalScope &scope, const std::vector<ConditionNarrowing> &narrowings,
                                   bool then_branch, const ast::Expr &condition, const Program &program) -> void;

    // Every name that currently holds the capture pointer. Starts as just the capture itself
    // and grows as the arm binds it to further locals ('const p := v').
    //
    // This is the provenance the check was missing. DEFERRED proposed putting it on
    // LocalBinding, but this walk never consults LocalScope at all — it is a syntactic pass
    // over one arm body, and the only aliases it can possibly need are the ones introduced
    // inside that body. A name set is the same information at the granularity actually used.
    using CaptureAliases = std::set<std::string>;

    // Warns when a by-ref payload capture ('.Variant(&v):') is let out of its own arm.
    //
    // Codegen emits the match/switch operand as a VALUE into one scratch slot per function
    // ("match.union"/"switch.union" in codegen.cpp), so '&v' points into a compiler temporary,
    // not at the user's object -- and the next match through that slot overwrites it. Any use
    // after the arm ends therefore reads something the program never stored there.
    //
    // Diagnosing this properly needs full escape analysis, and sema has no lifetime machinery.
    // This is the conservative syntactic approximation: the pointer reaching a return, an
    // assignment, a call argument, an aggregate initializer or a variant payload.
    //
    // It DOES follow the pointer through arm-local bindings ('const p := v', then letting 'p'
    // escape) — see CaptureAliases — which is the under-report it used to have. A name later
    // rebound to something else drops out of that set, so shadowing does not produce a false
    // positive.
    //
    // It remains a WARNING because it still OVER-reports, and that half cannot be fixed
    // without per-callee summaries: passing the pointer to something that only reads it
    // during the arm — a comparison, a print — is perfectly safe and is still flagged. Until
    // sema can answer "does this function let its argument escape", every call argument has
    // to be treated as an escape.
    auto check_payload_capture_escape(const ast::Stmt &body, CaptureAliases &aliases, DiagnosticEngine &diag) -> void;
    auto check_payload_capture_escape_expr(const ast::Expr &value, const CaptureAliases &aliases, DiagnosticEngine &diag) -> void;

    namespace {
        struct LvalueInfo {
            ResolvedType type;
            bool writable = false;
        };

        auto error(DiagnosticEngine &diag, const SourceLocation &loc, std::string msg) -> ResolvedType {
            diag.report_error(DiagnosticStage::Sema, loc, std::move(msg));
            return ResolvedType{.kind = TypeKind::Invalid};
        }

        // Like error(), but returns a caller-supplied fallback type instead of Invalid. Used
        // where the expected type is already known (that's how the mismatch was detected), so
        // resolving to it here prevents the same root-cause error from cascading into a second,
        // redundant diagnostic at the call site (e.g. a return-statement type mismatch).
        auto error_as(DiagnosticEngine &diag, const SourceLocation &loc, std::string msg,
                       const ResolvedType &fallback) -> ResolvedType {
            diag.report_error(DiagnosticStage::Sema, loc, std::move(msg));
            return fallback;
        }

        // Struct/array/union/slice types lower to LLVM aggregate values (StructType/ArrayType),
        // which ICmp cannot operate on directly (unlike Trait handles, which are unwrapped to
        // their data pointer before comparison). Comparing them must be rejected in sema rather
        // than left to crash codegen's ICmp emission.
        auto is_aggregate_no_cmp(const ResolvedType &t) -> bool {
            return t.kind == TypeKind::Struct || t.kind == TypeKind::Array ||
                   t.kind == TypeKind::Union || t.kind == TypeKind::Slice;
        }

        // Locates 'pub type Type_Info = union(enum) {...}' by name, wherever it's declared in
        // the program (see runtime/type_info) - 'type_info_of' is name-driven rather than
        // path-driven so it isn't tied to any particular checkout layout. Forces the symbol's
        // resolution (rather than only checking whether it's already resolved) since a program
        // may reach 'type_info_of' before anything else has referenced 'Type_Info' itself.
        // Snapshots module paths before resolving (rather than holding a live iterator into
        // Program::modules across a call that can insert new modules on demand - the same
        // hazard documented on ensure_module_declared) to avoid unordered_map rehashing
        // invalidating the iteration mid-loop.
        auto find_type_info_union(Program &program, DiagnosticEngine &diag) -> const UnionInfo * {
            std::vector<std::string> module_paths;
            module_paths.reserve(program.modules.size());
            for (const auto &path : program.modules | std::views::keys) {
                module_paths.push_back(path);
            }
            for (const auto &path : module_paths) {
                const auto mod_it = program.modules.find(path);
                if (mod_it == program.modules.end()) continue;
                const auto sym_it = mod_it->second.symbols.find("Type_Info");
                if (sym_it == mod_it->second.symbols.end() || !std::holds_alternative<TypeSymbol>(sym_it->second)) {
                    continue;
                }
                const auto resolved = resolve_type_symbol(path, "Type_Info", program, diag, {});
                if (resolved.kind == TypeKind::Union) {
                    return program.union_at(resolved.union_index);
                }
            }
            return nullptr;
        }

        auto format_named_type(const ast::NamedType &named) -> std::string {
            std::string result = named.name;
            for (const ast::NamedType *m = named.member.get(); m; m = m->member.get()) {
                result += '.';
                result += m->name;
            }
            return result;
        }

        // NamedType holds its `member` chain via unique_ptr, so it's move-only; deep-copy it here
        // rather than moving out of an AST node, since expressions can be re-checked more than once.
        auto clone_named_type(const ast::NamedType &named) -> ast::NamedType {
            return ast::NamedType{
                .name = named.name,
                .member = named.member ? std::make_unique<ast::NamedType>(clone_named_type(*named.member)) : nullptr,
                .location = named.location,
            };
        }

        // True iff 'ty' is a compiler-synthesized 'error(...)' union (as opposed to some
        // ordinary user-declared tagged union). Used everywhere a function's last return
        // type must be recognized as marking it fallible, and to recognize error values in
        // boolean context (&&/|| below; if/while/!/ternary coercion is handled in codegen's
        // coerce_to_bool, since sema places no restriction on condition types generally).
        auto is_error_union_type(const ResolvedType &ty, const Program &program) -> bool {
            return ty.kind == TypeKind::Union && program.union_at(ty.union_index) != nullptr &&
                   program.union_at(ty.union_index)->is_error_union;
        }

        // True iff 'ty' is a '?error(...)' union — an error the CALLER may leave unhandled.
        // When a call's trailing slot has this type and the caller doesn't bind it, sema
        // records the omission in ExprSideTables::call_dropped_optional_error and codegen
        // synthesizes the check the caller didn't write (a panic on failure). Deliberately
        // narrower than is_error_union_type: everything about handling an error that is
        // already in hand ('try', 'return_err', match/switch, boolean coercion, the subset
        // rule) treats optional and non-optional errors identically.
        auto is_optional_error_union(const ResolvedType &ty, const Program &program) -> bool {
            const auto *info = ty.kind == TypeKind::Union ? program.union_at(ty.union_index) : nullptr;
            return info != nullptr && info->is_error_union && info->is_optional;
        }

        // Returns the LocalBinding for 'name' iff it names a currently-in-scope error(...)-typed
        // local (i.e. one with typestate tracking at all).
        auto find_error_local(const std::string &name, LocalScope &locals, const Program &program) -> LocalBinding * {
            const auto it = locals.find(name);
            if (it == locals.end() || !is_error_union_type(it->second.type, program)) return nullptr;
            return &it->second;
        }

        // Bitset operators are a curated, restricted set — not general scalar arithmetic
        // (see ResolvedType::is_scalar(), which deliberately excludes Bitset so the
        // ordinary arithmetic/comparison paths below never see one). Only two operands of
        // the SAME bitset type are legal; mixing a bitset with a raw integer or a
        // different bitset type must go through an explicit cast first.
        auto bitset_binary_op_result(const ast::BinaryOp op, const ResolvedType &lhs, const ResolvedType &rhs, DiagnosticEngine &diag, const SourceLocation loc) -> ResolvedType {
            if (lhs.kind != TypeKind::Bitset || rhs.kind != TypeKind::Bitset || lhs.bitset_index != rhs.bitset_index) {
                return error(diag, loc, "bitset operators require two operands of the same bitset type "
                                         "(cast to the storage type first to mix with a raw integer)");
            }
            switch (op) {
            case ast::BinaryOp::Add:        // union
            case ast::BinaryOp::Sub:        // difference
            case ast::BinaryOp::BitwiseAnd: // intersection
            case ast::BinaryOp::BitwiseXor: // symmetric difference — infix '~' desugars here, and raw '^' too
            case ast::BinaryOp::BitwiseOr:  // union synonym for '+'
                return lhs;
            case ast::BinaryOp::Equal:
            case ast::BinaryOp::NotEqual:
                return ResolvedType{.kind = TypeKind::Bool};
            default:
                return error(diag, loc, "operator not supported on bitset types; use +/-/&/~/^/| for set operations, "
                                         "==/!= for equality, or 'in' for membership testing");
            }
        }

        // Names the bound in a diagnostic about a trait-bounded generic parameter, or "" when
        // the parameter is unconstrained.
        auto opaque_bound_name(const ResolvedType &ty, const Program &program) -> std::string {
            if (ty.kind != TypeKind::Opaque || ty.trait_index < 0) return {};
            const auto *info = program.trait_at(ty.trait_index);
            return info ? info->name : std::string{};
        }

        // A unary operator applied to an unbound generic parameter. Same split as the binary
        // case: unconstrained is permissive and silent, trait-bounded is rejected here.
        // 'what' completes "cannot <what> a value of ...", e.g. "negate", "dereference".
        auto opaque_unary_result(std::string_view what, const ResolvedType &operand, DiagnosticEngine &diag,
                                  const SourceLocation loc, const Program &program) -> ResolvedType {
            if (const auto bound = opaque_bound_name(operand, program); !bound.empty()) {
                return error(diag, loc, std::format(
                    "cannot {} a value of a generic parameter bound by '{}'; the bound grants only its own "
                    "methods plus assignment, '==' and '!='", what, bound));
            }
            // '-n' is still n's parameter, so the display name rides along (see
            // ResolvedType::opaque_param_index).
            return ResolvedType{.kind = TypeKind::Opaque, .opaque_param_index = operand.opaque_param_index};
        }

        // Result of a binary operator with at least one unbound-generic-parameter operand.
        //
        // UNCONSTRAINED parameter: permissive. Whether the operator is valid depends entirely
        // on the instantiation, so reporting here would be reporting against code that may be
        // perfectly correct. Yields Opaque (or Bool for a comparison) and stays silent; the
        // real check happens when a concrete instantiation is checked.
        //
        // TRAIT-BOUNDED parameter: strict, because now there IS a contract to check against.
        // The bound grants exactly its method set plus a universal core — assignment, passing,
        // returning, address-of, and equality. Arithmetic and ordering are not in that core,
        // so they are an error at the declaration rather than a surprise at the first use.
        auto opaque_binary_op_result(const ast::BinaryOp op, const ResolvedType &lhs, const ResolvedType &rhs,
                                      DiagnosticEngine &diag, const SourceLocation loc, const Program &program) -> ResolvedType {
            const auto bound = !opaque_bound_name(lhs, program).empty() ? opaque_bound_name(lhs, program)
                                                                        : opaque_bound_name(rhs, program);

            switch (op) {
            case ast::BinaryOp::Equal:
            case ast::BinaryOp::NotEqual:
                return ResolvedType{.kind = TypeKind::Bool};

            case ast::BinaryOp::Less:
            case ast::BinaryOp::Greater:
            case ast::BinaryOp::LessEqual:
            case ast::BinaryOp::GreaterEqual:
                if (!bound.empty()) {
                    return error(diag, loc, std::format(
                        "cannot order values of a generic parameter bound by '{}'; the bound grants only its "
                        "own methods plus assignment, '==' and '!='", bound));
                }
                return ResolvedType{.kind = TypeKind::Bool};

            case ast::BinaryOp::LogicalAnd:
            case ast::BinaryOp::LogicalOr:
                return ResolvedType{.kind = TypeKind::Bool};

            default:
                if (!bound.empty()) {
                    return error(diag, loc, std::format(
                        "cannot apply arithmetic to a generic parameter bound by '{}'; the bound grants only its "
                        "own methods plus assignment, '==' and '!='", bound));
                }
                // Deliberately propagates Opaque rather than picking the concrete operand's
                // type: 'n * 2' where 'n' is generic is only as known as 'n' is. The display
                // name propagates with it, taken from whichever operand has one — for the
                // mixed 'n * 2' shape that is the generic side, and for 'n % base_t' (both
                // Opaque, both 'T') either answers the same.
                return ResolvedType{.kind = TypeKind::Opaque,
                                     .opaque_param_index = lhs.opaque_param_index >= 0 ? lhs.opaque_param_index
                                                                                        : rhs.opaque_param_index};
            }
        }

        auto binary_op_result(const ast::BinaryOp op, const ResolvedType &lhs, const ResolvedType &rhs, DiagnosticEngine &diag, SourceLocation loc, const Program &program) -> ResolvedType {
            // An operand that already failed carries no information — a second message here
            // just buries the first. 'mut idx := a & b' reporting a mismatch makes 'idx'
            // Invalid, and every later 'idx + 1' would otherwise report again.
            if (lhs.kind == TypeKind::Invalid || rhs.kind == TypeKind::Invalid) {
                return ResolvedType{.kind = TypeKind::Invalid};
            }
            // Before the Bitset dispatch below, not after: 'Opaque op Bitset' would otherwise
            // be swallowed by bitset_binary_op_result and reported against a type that isn't
            // known yet.
            if (lhs.kind == TypeKind::Opaque || rhs.kind == TypeKind::Opaque) {
                return opaque_binary_op_result(op, lhs, rhs, diag, loc, program);
            }
            if (lhs.kind == TypeKind::Bitset || rhs.kind == TypeKind::Bitset) {
                return bitset_binary_op_result(op, lhs, rhs, diag, loc);
            }
            // Function pointers do not support arithmetic; only equality comparison is allowed
            const bool is_cmp = op == ast::BinaryOp::Equal || op == ast::BinaryOp::NotEqual ||
                                 op == ast::BinaryOp::Less || op == ast::BinaryOp::Greater ||
                                 op == ast::BinaryOp::LessEqual || op == ast::BinaryOp::GreaterEqual;
            if (!is_cmp && (lhs.kind == TypeKind::Function || rhs.kind == TypeKind::Function)) {
                return error(diag, loc, "arithmetic is not allowed on function pointer types");
            }
            // 'type' values are opaque compile-time identifiers: no arithmetic, no ordering,
            // only identity comparison.
            if (lhs.kind == TypeKind::Type || rhs.kind == TypeKind::Type) {
                if (op != ast::BinaryOp::Equal && op != ast::BinaryOp::NotEqual) {
                    return error(diag, loc, "'type' values only support '==' and '!='");
                }
            }

            switch (op) {
            case ast::BinaryOp::Add:
            case ast::BinaryOp::Sub:
                // Typed pointers get arithmetic too, not just 'anyptr'. Everything else in the
                // compiler already assumed this and only binary '+'/'-' disagreed: IncrDecrExpr
                // permits TypeKind::Pointer, compound '+='/'-=' lowers through
                // emit_pointer_offset, and codegen's own binary path is gated on
                // is_pointer_like, which covers Pointer and Anyptr alike and scales the offset
                // by the pointee size. So 'p++' compiled while 'p = p + 1' did not, for the
                // same variable.
                //
                // Resolved in the permissive direction because the restrictive one would break
                // working code: examples/mem/mem.mir walks buffers with 'd++.* = s++.*'.
                // spec.md's "anyptr supports arithmetic" line is updated to match.
                if (lhs.is_pointer_like() && rhs.is_integer()) return lhs;
                if (rhs.is_pointer_like() && lhs.is_integer()) return rhs;
                [[fallthrough]];

            case ast::BinaryOp::Mul:
            case ast::BinaryOp::Div:
            case ast::BinaryOp::Mod:
                if (lhs != rhs) {
                    return error(diag, loc, "operand type mismatch in binary expression");
                }
                // Numeric operands only. Same-type aggregates, pointers, bools and enums
                // used to slip through the mismatch check above straight into codegen's
                // CreateAdd/CreateMul on values LLVM cannot add — the comparison group
                // below was hardened for exactly this (is_aggregate_no_cmp), but the
                // arithmetic group never was.
                if (!lhs.is_integer() && !lhs.is_float()) {
                    return error(diag, loc, "arithmetic operators require integer or float operands");
                }
                return lhs;

            case ast::BinaryOp::BitwiseAnd:
            case ast::BinaryOp::BitwiseOr:
            case ast::BinaryOp::BitwiseXor:
            case ast::BinaryOp::ShiftLeft:
            case ast::BinaryOp::ShiftRight:
                if (lhs != rhs) {
                    return error(diag, loc, "operand type mismatch in binary expression");
                }
                if (!lhs.is_integer()) {
                    return error(diag, loc, "bitwise and shift operators require integer operands");
                }
                return lhs;

            case ast::BinaryOp::Equal:
            case ast::BinaryOp::NotEqual:
            case ast::BinaryOp::Less:
            case ast::BinaryOp::Greater:
            case ast::BinaryOp::LessEqual:
            case ast::BinaryOp::GreaterEqual:
                if (is_aggregate_no_cmp(lhs) || is_aggregate_no_cmp(rhs)) {
                    return error(diag, loc, "struct, array, union, and slice types do not support comparison operators");
                }
                if (!is_assignable(lhs, rhs, program) && !is_assignable(rhs, lhs, program)) {
                    return error(diag, loc, "operand type mismatch in comparison");
                }
                return ResolvedType{.kind = TypeKind::Bool};

            case ast::BinaryOp::LogicalAnd:
            case ast::BinaryOp::LogicalOr: {
                // Error values coerce to bool here exactly like they do in if/while/!/ternary
                // condition position (Failed = true, Ok = false) — see coerce_to_bool in
                // codegen.cpp.
                const auto is_bool_like = [&](const ResolvedType &t) {
                    return t.kind == TypeKind::Bool || is_error_union_type(t, program);
                };
                if (!is_bool_like(lhs) || !is_bool_like(rhs)) {
                    return error(diag, loc, "&&/|| require bool (or error) operands");
                }
                return ResolvedType{.kind = TypeKind::Bool};
            }

            case ast::BinaryOp::In:
                // check_expr's BinaryExpr case intercepts 'In' before ever calling
                // binary_op_result (it needs RHS-then-LHS expected-type propagation, which
                // this function's symmetric operand model can't express) — unreachable.
                return error(diag, loc, "internal error: 'in' should be handled before binary_op_result");
            }

            return ResolvedType{.kind = TypeKind::Invalid};
        }

        auto is_coercible_literal(const ast::Expr &expr) -> bool {
            return std::visit(
                [&]<typename T0>(const T0 &v) -> bool {
                    using V = std::decay_t<T0>;
                    if constexpr (std::is_same_v<V, ast::LiteralIntegerExpr> ||
                                  std::is_same_v<V, ast::LiteralFloatExpr> ||
                                  std::is_same_v<V, ast::LiteralNilExpr>) {
                        return true;
                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::UnaryExpr>>) {
                        return v->op == ast::UnaryOp::Negate && is_coercible_literal(v->operand);
                    } else {
                        return false;
                    }
                },
                expr);
        }

        // Bit width of an integer TypeKind; 0 for anything else.
        auto integer_bit_width(const TypeKind kind) -> unsigned {
            switch (kind) {
            case TypeKind::U8:
            case TypeKind::I8:    return 8;
            case TypeKind::U16:
            case TypeKind::I16:   return 16;
            case TypeKind::U32:
            case TypeKind::I32:   return 32;
            case TypeKind::U64:
            case TypeKind::I64:
            case TypeKind::USize: return 64;
            default:              return 0;
            }
        }

        // Whether an integer literal of the given magnitude, optionally negated, is
        // representable in 'target'. 'magnitude' is the literal's own unsigned value as the
        // lexer produced it -- '-5' is UnaryExpr(Negate) wrapping the literal 5, so the sign
        // is not part of it and has to be passed in.
        auto integer_literal_fits(const uint64_t magnitude, const bool negative, const ResolvedType &target) -> bool {
            const auto bits = integer_bit_width(target.kind);
            if (bits == 0) return true;

            if (negative) {
                // A negative value is never representable in an unsigned type. Use an explicit
                // cast to reinterpret the bits deliberately.
                if (!target.is_signed()) return false;
                // The negative range is one wider than the positive one: i8 spans -128..127.
                return magnitude <= (uint64_t{1} << (bits - 1));
            }
            if (target.is_signed()) {
                return magnitude <= (uint64_t{1} << (bits - 1)) - 1;
            }
            if (bits >= 64) return true;
            return magnitude <= (uint64_t{1} << bits) - 1;
        }

        auto describe_integer_range(const ResolvedType &target) -> std::string {
            const auto bits = integer_bit_width(target.kind);
            if (bits == 0) return {};
            if (target.is_signed()) {
                const auto limit = uint64_t{1} << (bits - 1);
                return std::format("-{}..{}", limit, limit - 1);
            }
            if (bits >= 64) return std::format("0..{}", UINT64_MAX);
            return std::format("0..{}", (uint64_t{1} << bits) - 1);
        }

        auto contains_undefined(const ast::Expr &expr) -> bool;


        auto contains_undefined_in_braced(const ast::BracedInitializerExpr &bi) -> bool {
            return std::visit([]<typename BV>(const BV &bv) -> bool {
                using BVT = std::decay_t<BV>;
                if constexpr (std::is_same_v<BVT, ast::StructExpr>) {
                    return std::ranges::any_of(bv.fields, [](const auto &sf) { return contains_undefined(sf.expr); });
                } else if constexpr (std::is_same_v<BVT, ast::ArrayExpr>) {
                    return std::ranges::any_of(bv.values, [](const auto &val) { return contains_undefined(val); });
                } else {
                    return false;
                }
            }, bi);
        }

        auto contains_undefined(const ast::Expr &expr) -> bool {
            return std::visit([]<typename V>(const V &v) -> bool {
                using VT = std::decay_t<V>;
                if constexpr (std::is_same_v<VT, ast::UndefinedExpr>) {
                    return true;
                } else if constexpr (std::is_same_v<VT, std::unique_ptr<ast::BracedInitializerExpr>>) {
                    return contains_undefined_in_braced(*v);
                } else {
                    return false;
                }
            }, expr);
        }

        auto is_cast_legal(const ResolvedType &from, const ResolvedType &to) -> bool {
            // Casting to or from an unbound generic parameter — 'cast(self.capacity - 1, u64)'
            // inside a generic impl method. Legality depends on the instantiation, and is
            // re-checked there; an explicit cast is also the programmer stating intent, which
            // is exactly the case not to second-guess while the type is still unknown.
            if (from.kind == TypeKind::Opaque || to.kind == TypeKind::Opaque) return true;
            // 'any' can only be cast to a pointer type or 'anyptr' — extracts the fat value's
            // data word. No type-id check is performed; that's always the programmer's
            // responsibility, same posture as every other anyptr cast.
            if (from.kind == TypeKind::Any) return to.kind == TypeKind::Pointer || to.kind == TypeKind::Anyptr;
            if (to.kind == TypeKind::Slice) return from.kind == TypeKind::Pointer || from.kind == TypeKind::Anyptr || from.kind == TypeKind::Array || from.kind == TypeKind::Slice;
            if (from.kind == TypeKind::Array && (to.kind == TypeKind::Pointer || to.kind == TypeKind::Anyptr)) return true;
            if (from.kind == TypeKind::Slice && (to.kind == TypeKind::Pointer || to.kind == TypeKind::Anyptr)) return true;
            // Function pointers can be cast to/from anyptr (C callback interop)
            if (from.kind == TypeKind::Function && to.kind == TypeKind::Anyptr) return true;
            if (from.kind == TypeKind::Anyptr && to.kind == TypeKind::Function) return true;
            // Enums are represented as their underlying integer type - allow casting to/from it
            if (from.kind == TypeKind::Enum && to.is_integer()) return true;
            if (from.is_integer() && to.kind == TypeKind::Enum) return true;
            // Bitsets are likewise their storage integer type - explicit cast to/from any
            // integer is always legal (no range check: the programmer is asserting the
            // integer is a valid bitset value). Casting between two DIFFERENT bitset types
            // is illegal even when their storage types match - distinct bitsets require an
            // explicit integer intermediary (cast(cast(a, u16), TypeB)); a same-bitset cast
            // is a legal no-op identity cast.
            if (from.kind == TypeKind::Bitset && to.kind == TypeKind::Bitset) return from.bitset_index == to.bitset_index;
            if (from.kind == TypeKind::Bitset && to.is_integer()) return true;
            if (from.is_integer() && to.kind == TypeKind::Bitset) return true;
            return from.is_scalar() && to.is_scalar();
        }

        // Returns the FunctionTypeInfo for a function-kind ResolvedType. Falls
        // back to a static empty signature for a stale/out-of-range index
        // rather than throwing - see Program::fn_signature_at().
        auto fn_sig(const ResolvedType &ty, const Program &program) -> const FunctionTypeInfo & {
            static const FunctionTypeInfo empty{};
            const auto *sig = program.fn_signature_at(ty.fn_index);
            return sig ? *sig : empty;
        }

        auto slice_element_type(const ResolvedType &slice, [[maybe_unused]] const std::string &module_path, Program &program) -> ResolvedType {
            const auto *info = program.slice_at(slice.slice_index);
            return info ? info->element_type : ResolvedType{.kind = TypeKind::Invalid};
        }

        auto array_element_type(const ResolvedType &array, [[maybe_unused]] const std::string &module_path, Program &program) -> ResolvedType {
            const auto *info = program.array_at(array.array_index);
            return info ? info->element_type : ResolvedType{.kind = TypeKind::Invalid};
        }

    } // anonymous namespace — reopened below; assignable_in_module needs external linkage
      // (declared in sema.hpp) so sema.cpp's default-parameter-value checking can use it too.

    auto assignable_in_module(const ResolvedType &from, const ResolvedType &to, const std::string &module_path, Program &program) -> bool {
        // array<->slice and slice->pointer are element-checked inside is_assignable itself
        // (the fallthrough below); only the coercions unavailable to plain is_assignable
        // callers are added here.
        if (from.kind == TypeKind::Array && to.kind == TypeKind::Pointer) {
            const auto *pointee = program.pointee_at(to.pointee_index);
            return pointee && array_element_type(from, module_path, program) == *pointee;
        }
        // Bitset -> storage type is implicitly coercible ONLY in expected-type position
        // (every call site of assignable_in_module — call args, '=', compound-assign
        // result check, struct/array field literals, var-decl-with-annotation, return
        // statements, default parameter values). The reverse (storage type -> bitset) is
        // deliberately NOT allowed here, matching Part 4's asymmetric coercion rule; use
        // an explicit cast instead.
        if (from.kind == TypeKind::Bitset) {
            const auto *info = program.bitset_at(from.bitset_index);
            if (info && info->storage_type == to) return true;
        }
        // '?error(E)' and 'error(E)' are distinct types (see UnionInfo::is_optional) but
        // carry identical values — the '?' only ever describes an obligation at the CALL
        // SITE, so it is inert once the value is in hand. Assignment in either direction is
        // therefore allowed whenever the member sets are identical, which is what lets
        // 'fn g() -> (anyptr, error(E)) { return alloc(n) }' forward a '?error(E)' slot.
        // Differing member sets stay incompatible — widening still needs 'try'/'return_err'.
        if (from.kind == TypeKind::Union && to.kind == TypeKind::Union) {
            const auto *from_info = program.union_at(from.union_index);
            const auto *to_info = program.union_at(to.union_index);
            if (from_info && to_info && from_info->is_error_union && to_info->is_error_union &&
                from_info->is_optional != to_info->is_optional &&
                from_info->error_member_types == to_info->error_member_types) {
                return true;
            }
        }
        return is_assignable(from, to, program);
    }

    namespace {
        auto slice_cast_elements_match(const ResolvedType &from, const ResolvedType &to, const std::string &module_path, Program &program) -> bool {
            if (to.kind != TypeKind::Slice) return true;
            if (from.kind == TypeKind::Array) return array_element_type(from, module_path, program) == slice_element_type(to, module_path, program);
            if (from.kind == TypeKind::Slice) return slice_element_type(from, module_path, program) == slice_element_type(to, module_path, program);
            return true;
        }

        auto check_call_args(const std::vector<ast::Expr> &args, const std::vector<ResolvedType> &params, bool is_variadic, LocalScope &locals, const std::string &module_path, Program &program, DiagnosticEngine &diag, const SourceLocation &loc, const std::string &callee_desc, int loop_depth, int defer_loop_base, const ResolvedType *fn_error_type, bool native_variadic = false, std::optional<size_t> required_params = std::nullopt) -> bool;
        auto try_resolve_namespace_chain(const ast::Expr &expr, const std::string &module_path, LocalScope &locals, Program &program) -> std::optional<std::string>;

        // Tier-3 method-call resolution: 'receiver_type' is an actual dyn-handle
        // (TypeKind::Trait), which has no concrete MethodInfo/body to look up via
        // find_method — dispatch is resolved against the trait's own method list
        // instead. Returns std::nullopt (not an error) when receiver_type isn't a
        // trait handle, so callers fall through to the existing find_method path
        // unchanged. 'dispatch_key' is the address of the ast::CallExpr node itself
        // (stable across check_expr / check_group_call_returns / codegen's emit_call
        // and call_return_types, all of which take a 'const ast::CallExpr&' referring
        // to the same heap-allocated node) — NOT sema::get_expr_key's variant-slot
        // address, which check_group_call_returns has no way to reproduce since it
        // only receives the unwrapped CallExpr, not the outer Expr variant.
        // A trait-impl method is never allowed to declare its own default parameter
        // values (see resolve_trait_impl_signatures_for_program), so its own
        // 'required_params' always equals its full param count. A call resolved
        // via this MethodInfo (i.e. NOT through try_trait_handle_dispatch's dyn
        // Trait handle path) must instead source the defaulted-arg count from the
        // trait's own TraitMethodInfo when this method backs a trait impl.
        // True when 'ty' is a monomorphized instantiation of a generic struct/enum/union/
        // bitset declaration (its underlying *Info's 'generic_instance' is set).
        auto receiver_is_generic_instance(const ResolvedType &ty, const Program &program) -> bool {
            switch (ty.kind) {
            case TypeKind::Struct:
                if (const auto *info = program.struct_at(ty.struct_index)) return info->generic_instance.has_value();
                return false;
            case TypeKind::Enum:
                if (const auto *info = program.enum_at(ty.enum_index)) return info->generic_instance.has_value();
                return false;
            case TypeKind::Union:
                if (const auto *info = program.union_at(ty.union_index)) return info->generic_instance.has_value();
                return false;
            case TypeKind::Bitset:
                if (const auto *info = program.bitset_at(ty.bitset_index)) return info->generic_instance.has_value();
                return false;
            default:
                return false;
            }
        }

        auto method_required_params(const MethodInfo &method, const Program &program) -> size_t {
            if (method.trait_name) {
                if (const auto *trait_info = program.trait_at(method.trait_index);
                    trait_info && method.trait_method_index >= 0 &&
                    static_cast<size_t>(method.trait_method_index) < trait_info->methods.size()) {
                    return trait_info->methods[method.trait_method_index].required_params;
                }
            }
            return method.required_params;
        }

        // "hash', 'eq'" — for the "the bound declares ..." half of a missing-method message.
        auto join_method_names(const std::vector<TraitMethodInfo> &methods) -> std::string {
            std::string out;
            for (const auto &m : methods) {
                if (!out.empty()) out += "', '";
                out += m.name;
            }
            return out;
        }

        auto try_trait_handle_dispatch(const ResolvedType &receiver_type, const std::string &method_name,
                                        const std::vector<ast::Expr> &args, const void *dispatch_key,
                                        LocalScope &locals, const std::string &module_path, Program &program,
                                        DiagnosticEngine &diag, const SourceLocation &loc, const int loop_depth,
                                        const int defer_loop_base, const ResolvedType *fn_error_type) -> std::optional<std::vector<ResolvedType>> {
            // A method call on an unbound generic parameter. UNCONSTRAINED: permissive — 'T'
            // may well have this method, so check the arguments as ordinary expressions and
            // yield Opaque. BOUNDED: fall through to the trait-method lookup below, which is
            // exactly the right check — the bound's flattened method set is the complete list
            // of what 'T' offers.
            if (receiver_type.kind == TypeKind::Opaque) {
                if (receiver_type.trait_index < 0) {
                    for (const auto &arg : args) {
                        check_expr(arg, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_error_type);
                    }
                    return std::vector<ResolvedType>{ResolvedType{.kind = TypeKind::Opaque}};
                }
            } else if (receiver_type.kind != TypeKind::Trait) {
                return std::nullopt;
            }

            const auto *trait_info = program.trait_at(receiver_type.trait_index);
            const TraitMethodInfo *trait_method = nullptr;
            int method_order_index = -1;
            if (trait_info) {
                for (size_t i = 0; i < trait_info->methods.size(); ++i) {
                    if (trait_info->methods[i].name == method_name) {
                        trait_method = &trait_info->methods[i];
                        method_order_index = static_cast<int>(i);
                        break;
                    }
                }
            }

            if (!trait_method) {
                if (receiver_type.kind == TypeKind::Opaque) {
                    // A BOUNDED parameter, and the bound does not declare this method. Unlike
                    // a trait handle — which can still fall through to an inherent 'impl'
                    // block — the bound is the complete statement of what 'T' offers, so this
                    // is definitively wrong and is reported at the declaration.
                    diag.report_error(DiagnosticStage::Sema, loc, std::format(
                        "no method '{}' on a generic parameter bound by '{}'; the bound declares {}",
                        method_name, trait_info ? trait_info->name : "<trait>",
                        trait_info && !trait_info->methods.empty()
                            ? std::format("'{}'", join_method_names(trait_info->methods))
                            : "no methods"));
                    return std::vector<ResolvedType>{ResolvedType{.kind = TypeKind::Invalid}};
                }
                // Not one of the trait's own (dynamically-dispatched) methods — it may still be
                // an inherent method from a bare 'impl Control { ... }' block (e.g. a default
                // method implemented in terms of the trait's dynamic methods). Let the caller
                // fall through to find_method() for that lookup; only find_method's own failure
                // is a real "no such method" error.
                return std::nullopt;
            }

            check_call_args(args, trait_method->params, false, locals, module_path, program, diag, loc, method_name, loop_depth, defer_loop_base, fn_error_type, false, trait_method->required_params);

            // A bounded generic parameter is monomorphized: at each instantiation the concrete
            // impl is called directly, so there is no vtable slot to record. Only a real trait
            // HANDLE dispatches dynamically.
            if (receiver_type.kind == TypeKind::Trait) {
                expr_tables_for_write(program, module_path).expr_trait_dispatch[dispatch_key] = TraitDispatchInfo{
                    .trait_index = receiver_type.trait_index,
                    .method_order_index = method_order_index,
                };
            }

            return trait_method->return_types;
        }

        // THE call-resolution decision tree, shared by every position a call can appear in.
        //
        // Resolves the callee and checks the arguments, returning the callee's FULL return
        // list. In order: explicit generic-function instantiation ('make_fixed[16]()', and its
        // cross-module 'mod.fn[i32]()' shape), a cross-module namespace chain, a method call on
        // a value (trait-handle dynamic dispatch, then generic-method instantiation, then a
        // struct field holding a function pointer), a general expression callee, a local
        // holding a function pointer, and finally same-module symbol dispatch over
        // Function/ExtFunction/Macro.
        //
        // Callers differ in ARITY, not in resolution:
        //   - check_expr's CallExpr case wants one value, and narrows via single_value_result
        //     (which is also where a trailing '?error(...)' slot is dropped).
        //   - check_group_call_returns wants the whole list ('const a, b := f()', 'try f()').
        //
        // Two parameters exist purely because the callers legitimately differ:
        //
        //   'expected' is forwarded to generic argument inference. A group call has no single
        //   expected type and passes nullopt.
        //
        //   'instance_key' is the expr_generic_fn_instance key. The two callers genuinely use
        //   DIFFERENT keys — get_expr_key's variant-slot address for a value call, the
        //   ast::CallExpr's own address for a group call (which never sees the outer Expr
        //   variant) — and codegen reads each back where it was written. Passing it in is what
        //   lets one resolver serve both without codegen changing.
        auto resolve_call_returns(const ast::CallExpr &call, const std::optional<ResolvedType> &expected,
                                   const void *instance_key, LocalScope &locals, const std::string &module_path,
                                   Program &program, DiagnosticEngine &diag, const int loop_depth,
                                   const int defer_loop_base, const ResolvedType *fn_error_type) -> std::vector<ResolvedType> {
            const auto fail = [&](std::string msg) -> std::vector<ResolvedType> {
                error(diag, call.location, std::move(msg));
                return {};
            };
            const auto record_instance = [&](const size_t idx) -> std::vector<ResolvedType> {
                const auto &instance = *program.generic_fn_instances[idx];
                check_call_args(call.args, instance.param_types, false, locals, module_path, program, diag,
                                 call.location, instance.decl ? instance.decl->name : std::string{},
                                 loop_depth, defer_loop_base, fn_error_type, instance.is_variadic, instance.required_params);
                expr_tables_for_write(program, module_path).expr_generic_fn_instance[instance_key] = idx;
                return instance.return_types;
            };

            // Explicit generic-function instantiation call ('make_fixed[16]()',
            // 'make_list[i32]()', or cross-module 'list.make_list[i32]()') — the callee is an
            // IndexOrInstantiateExpr whose operand names a generic function, either directly
            // (IdentExpr, same module) or through a namespace chain (MemberExpr, 'mod.fn').
            // Only intercepted when 'operand' actually IS a generic function; anything else
            // (ordinary indexing whose result happens to be called, e.g. 'fn_table[i]()')
            // falls through to the general-expression-callee path below unchanged.
            if (const auto *index_callee = std::get_if<std::unique_ptr<ast::IndexOrInstantiateExpr>>(&call.callee)) {
                const ast::FunctionDecl *generic_decl = nullptr;
                std::string decl_module = module_path;
                std::string fn_name;
                if (const auto *op_ident = std::get_if<ast::IdentExpr>(&(*index_callee)->operand);
                    op_ident && !locals.contains(op_ident->name)) {
                    if (const auto mod_it = program.modules.find(module_path); mod_it != program.modules.end()) {
                        if (const auto sym_it = mod_it->second.symbols.find(op_ident->name); sym_it != mod_it->second.symbols.end()) {
                            if (const auto *fs = std::get_if<FunctionSymbol>(&sym_it->second); fs && fs->decl && !fs->decl->generic_params.empty()) {
                                generic_decl = fs->decl;
                                fn_name = op_ident->name;
                            }
                        }
                    }
                } else if (const auto *op_member = std::get_if<std::unique_ptr<ast::MemberExpr>>(&(*index_callee)->operand)) {
                    if (const auto target_mod = try_resolve_namespace_chain((*op_member)->object, module_path, locals, program)) {
                        if (const auto mod_it = program.modules.find(*target_mod); mod_it != program.modules.end()) {
                            if (const auto sym_it = mod_it->second.symbols.find((*op_member)->member); sym_it != mod_it->second.symbols.end()) {
                                if (const auto *fs = std::get_if<FunctionSymbol>(&sym_it->second); fs && fs->decl && !fs->decl->generic_params.empty()) {
                                    if (!fs->is_pub) {
                                        return fail(std::format("'{}' is not pub", (*op_member)->member));
                                    }
                                    generic_decl = fs->decl;
                                    fn_name = (*op_member)->member;
                                    decl_module = *target_mod;
                                }
                            }
                        }
                    }
                }
                if (generic_decl) {
                    std::optional<std::vector<GenericArgValue>> resolved_args;
                    if (!(*index_callee)->args.empty()) {
                        resolved_args = resolve_explicit_generic_args(generic_decl->generic_params, (*index_callee)->args, module_path, program, diag, call.location, fn_name);
                    } else {
                        resolved_args = infer_generic_function_args(*generic_decl, call.args, expected, locals, module_path, program, diag, call.location, loop_depth, defer_loop_base, fn_error_type);
                    }
                    if (!resolved_args) return {};
                    return record_instance(instantiate_generic_function(program, diag, decl_module, fn_name, std::move(*resolved_args), call.location));
                }
            }

            if (const auto *member_callee = std::get_if<std::unique_ptr<ast::MemberExpr>>(&call.callee)) {
                // Cross-module 'mod.fn(...)' — resolved against the named module's symbols.
                if (const auto target_module = try_resolve_namespace_chain((*member_callee)->object, module_path, locals, program)) {
                    const auto mod_it = program.modules.find(*target_module);
                    if (mod_it == program.modules.end()) return {};

                    const std::string &fn_name = (*member_callee)->member;
                    const auto sym_it = mod_it->second.symbols.find(fn_name);
                    if (sym_it == mod_it->second.symbols.end()) {
                        return fail(std::format("unknown function '{}'", fn_name));
                    }

                    return std::visit(
                        [&]<typename T1>(const T1 &sym) -> std::vector<ResolvedType> {
                            using S = std::decay_t<T1>;
                            if constexpr (std::is_same_v<S, FunctionSymbol>) {
                                if (!sym.is_pub) return fail(std::format("'{}' is not pub", fn_name));
                                // A bare cross-module call to a generic function with NO
                                // brackets at all ('list.make_list()', not
                                // 'list.make_list[i32]()') — inference only, mirroring the
                                // same-module bare-call case below. Without this the template
                                // would reach ensure_function_signature_resolved, which refuses
                                // to resolve a generic signature, leaving 'params' empty and
                                // producing a bogus arity error.
                                if (sym.decl && !sym.decl->generic_params.empty()) {
                                    auto resolved_args = infer_generic_function_args(*sym.decl, call.args, expected, locals, module_path, program, diag, call.location, loop_depth, defer_loop_base, fn_error_type);
                                    if (!resolved_args) return {};
                                    return record_instance(instantiate_generic_function(program, diag, *target_module, fn_name, std::move(*resolved_args), call.location));
                                }
                                auto &resolved_fn = ensure_function_signature_resolved(*target_module, fn_name, program, diag);
                                check_call_args(call.args, resolved_fn.params, false, locals, module_path, program, diag, call.location, fn_name, loop_depth, defer_loop_base, fn_error_type, resolved_fn.is_variadic, resolved_fn.required_params);
                                return resolved_fn.return_types;
                            } else if constexpr (std::is_same_v<S, ExtFunctionSymbol>) {
                                if (!sym.is_pub) return fail(std::format("'{}' is not pub", fn_name));
                                check_call_args(call.args, sym.params, sym.is_variadic, locals, module_path, program, diag, call.location, fn_name, loop_depth, defer_loop_base, fn_error_type);
                                std::vector<ResolvedType> returns;
                                if (sym.return_type) returns.push_back(*sym.return_type);
                                return returns;
                            } else if constexpr (std::is_same_v<S, MacroSymbol>) {
                                if (!sym.is_pub) return fail(std::format("'{}' is not pub", fn_name));
                                auto &resolved_macro = resolve_macro_symbol(*target_module, fn_name, program, diag, call.location);
                                check_call_args(call.args, resolved_macro.params, false, locals, module_path, program, diag, call.location, fn_name, loop_depth, defer_loop_base, fn_error_type);
                                return {resolved_macro.result_type};
                            } else {
                                return fail(std::format("'{}' is not callable", fn_name));
                            }
                        },
                        sym_it->second);
                }

                // Method call on a value.
                auto receiver_type = check_expr((*member_callee)->object, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_error_type);
                if (receiver_type.kind == TypeKind::Pointer) {
                    if (const auto *pointee = program.pointee_at(receiver_type.pointee_index)) {
                        receiver_type = *pointee;
                    } else {
                        receiver_type = ResolvedType{.kind = TypeKind::Invalid};
                    }
                }
                if (auto trait_returns = try_trait_handle_dispatch(receiver_type, (*member_callee)->member, call.args, &call, locals, module_path, program, diag, call.location, loop_depth, defer_loop_base, fn_error_type)) {
                    return *trait_returns;
                }
                const auto *method = find_method(receiver_type, (*member_callee)->member, program);
                if (!method) {
                    // A struct field holding a function pointer, called like a method.
                    if (receiver_type.kind == TypeKind::Struct) {
                        if (const auto *struct_info = program.struct_at(receiver_type.struct_index)) {
                            for (const auto &field : struct_info->fields) {
                                if (field.name != (*member_callee)->member) continue;
                                if (field.type.kind == TypeKind::Function) {
                                    const auto &sig = fn_sig(field.type, program);
                                    check_call_args(call.args, sig.param_types, sig.is_variadic, locals, module_path, program, diag, call.location, (*member_callee)->member, loop_depth, defer_loop_base, fn_error_type);
                                    return sig.return_types;
                                }
                                break;
                            }
                        }
                    }
                    return fail(std::format("no method '{}' on type", (*member_callee)->member));
                }
                // A method call on a generic type's concrete instantiation ('self.reserve(...)'
                // where 'self: List[i32]') — 'method' above is the unspecialized TEMPLATE
                // (find_method's bare "List"/"reserve" keying never changes), so its own
                // param_types/return_types are never populated for a generic type (see
                // resolve_impl_signatures_for_module's skip) — instantiate_generic_method
                // resolves the real, per-instantiation signature on demand instead. It is also
                // what body-checks the instance, so skipping it would silently skip the callee.
                if (receiver_is_generic_instance(receiver_type, program)) {
                    const auto idx = instantiate_generic_method(program, diag, receiver_type, (*member_callee)->member, call.location);
                    if (!idx) return fail(std::format("no method '{}' on type", (*member_callee)->member));
                    return record_instance(*idx);
                }
                check_call_args(call.args, method->param_types, false, locals, module_path, program, diag, call.location, (*member_callee)->member, loop_depth, defer_loop_base, fn_error_type, method->is_variadic, method_required_params(*method, program));
                return method->return_types;
            }

            // General expression callee (a deref of a function pointer, an indexed function
            // pointer array, ...): evaluate it, then call through if it is a function type.
            if (!std::holds_alternative<ast::IdentExpr>(call.callee)) {
                const auto callee_ty = check_expr(call.callee, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_error_type);
                if (callee_ty.kind == TypeKind::Function) {
                    const auto &sig = fn_sig(callee_ty, program);
                    check_call_args(call.args, sig.param_types, sig.is_variadic, locals, module_path, program, diag, call.location, "<fn ptr>", loop_depth, defer_loop_base, fn_error_type);
                    return sig.return_types;
                }
                return fail("unsupported call target");
            }

            const auto *callee_ident = std::get_if<ast::IdentExpr>(&call.callee);

            // A local holding a function pointer shadows any module symbol of the same name.
            if (const auto local_it = locals.find(callee_ident->name); local_it != locals.end()) {
                const auto &local_ty = local_it->second.type;
                if (local_ty.kind == TypeKind::Function) {
                    const auto &sig = fn_sig(local_ty, program);
                    check_call_args(call.args, sig.param_types, sig.is_variadic, locals, module_path, program, diag, call.location, callee_ident->name, loop_depth, defer_loop_base, fn_error_type);
                    return sig.return_types;
                }
                return fail(std::format("'{}' is not callable", callee_ident->name));
            }

            const auto mod_it = program.modules.find(module_path);
            if (mod_it == program.modules.end()) {
                return fail(std::format("internal error: module '{}' not found", module_path));
            }

            const auto sym_it = mod_it->second.symbols.find(callee_ident->name);
            if (sym_it == mod_it->second.symbols.end()) {
                return fail(std::format("unknown function '{}'", callee_ident->name));
            }

            return std::visit(
                [&]<typename T1>(const T1 &sym) -> std::vector<ResolvedType> {
                    using S = std::decay_t<T1>;
                    if constexpr (std::is_same_v<S, FunctionSymbol>) {
                        // A bare call to a generic function with NO brackets at all
                        // ('make_list()', not 'make_list[]()') — inference only, per spec.md
                        // §22 "Explicit vs. Inferred Instantiation".
                        if (sym.decl && !sym.decl->generic_params.empty()) {
                            auto resolved_args = infer_generic_function_args(*sym.decl, call.args, expected, locals, module_path, program, diag, call.location, loop_depth, defer_loop_base, fn_error_type);
                            if (!resolved_args) return {};
                            return record_instance(instantiate_generic_function(program, diag, module_path, callee_ident->name, std::move(*resolved_args), call.location));
                        }
                        auto &resolved_fn = ensure_function_signature_resolved(module_path, callee_ident->name, program, diag);
                        check_call_args(call.args, resolved_fn.params, false, locals, module_path, program, diag, call.location, callee_ident->name, loop_depth, defer_loop_base, fn_error_type, resolved_fn.is_variadic, resolved_fn.required_params);
                        return resolved_fn.return_types;
                    } else if constexpr (std::is_same_v<S, ExtFunctionSymbol>) {
                        check_call_args(call.args, sym.params, sym.is_variadic, locals, module_path, program, diag, call.location, callee_ident->name, loop_depth, defer_loop_base, fn_error_type);
                        std::vector<ResolvedType> returns;
                        if (sym.return_type) returns.push_back(*sym.return_type);
                        return returns;
                    } else if constexpr (std::is_same_v<S, MacroSymbol>) {
                        auto &resolved_macro = resolve_macro_symbol(module_path, callee_ident->name, program, diag, call.location);
                        check_call_args(call.args, resolved_macro.params, false, locals, module_path, program, diag, call.location, callee_ident->name, loop_depth, defer_loop_base, fn_error_type);
                        return {resolved_macro.result_type};
                    } else {
                        return fail(std::format("'{}' is not callable", callee_ident->name));
                    }
                },
                sym_it->second);
        }

        // Multi-return positions: 'const a, b := f()', 'try f()', a return-value group. Takes
        // the resolver's full return list as-is; there is no single expected type to infer
        // from, and the key is the CallExpr's own address (see resolve_call_returns).
        auto check_group_call_returns(const ast::CallExpr &call, LocalScope &locals, const std::string &module_path, Program &program, DiagnosticEngine &diag, const int loop_depth, const int defer_loop_base, const ResolvedType *fn_error_type) -> std::vector<ResolvedType> {
            return resolve_call_returns(call, std::nullopt, &call, locals, module_path, program, diag, loop_depth, defer_loop_base, fn_error_type);
        }

        static auto is_valid_variadic_arg(const ResolvedType &ty) -> bool {
            switch (ty.kind) {
            case TypeKind::I32: case TypeKind::U32:
            case TypeKind::I64: case TypeKind::U64:
            case TypeKind::USize:
            case TypeKind::F64:
            case TypeKind::Pointer: case TypeKind::Anyptr:
            // Eager template pass: an unbound generic value ('printf("%d", N)' with
            // '[N: usize]') has no width yet; the instantiation re-checks with the
            // concrete type.
            case TypeKind::Opaque:
                return true;
            default:
                return false;
            }
        }

        auto check_call_args(const std::vector<ast::Expr> &args, const std::vector<ResolvedType> &params, const bool is_variadic, LocalScope &locals, const std::string &module_path, Program &program, DiagnosticEngine &diag, const SourceLocation &loc, const std::string &callee_desc, const int loop_depth, const int defer_loop_base, const ResolvedType *fn_error_type, const bool native_variadic, const std::optional<size_t> required_params) -> bool {
            if (native_variadic) {
                // Last entry of 'params' is the dissolved '[]T' slot for the native '...T' parameter.
                const size_t fixed_count = params.size() - 1;
                if (args.size() < fixed_count) {
                    error(diag, loc, std::format("'{}' expects at least {} argument(s), got {}", callee_desc, fixed_count, args.size()));
                    return false;
                }

                bool ok = true;
                for (size_t i = 0; i < fixed_count; ++i) {
                    if (auto arg_ty = check_expr(args[i], locals, module_path, program, diag, params[i], loop_depth, defer_loop_base, fn_error_type); !assignable_in_module(arg_ty, params[i], module_path, program)) {
                        error(diag, get_expr_location(args[i]), std::format("'{}' argument {} type mismatch", callee_desc, i + 1));
                        ok = false;
                    }
                }

                const auto &slice_ty = params.back();
                const auto element_ty = slice_element_type(slice_ty, module_path, program);
                const size_t tail_count = args.size() - fixed_count;

                if (tail_count == 1) {
                    if (const auto *spread = std::get_if<std::unique_ptr<ast::SpreadExpr>>(&args[fixed_count])) {
                        const auto spread_ty = check_expr((*spread)->operand, locals, module_path, program, diag, slice_ty, loop_depth, defer_loop_base, fn_error_type);
                        if (!assignable_in_module(spread_ty, slice_ty, module_path, program)) {
                            error(diag, get_expr_location(args[fixed_count]), std::format("'{}' spread argument type mismatch: expected a slice matching the variadic element type", callee_desc));
                            ok = false;
                        }
                        return ok;
                    }
                }

                for (size_t i = fixed_count; i < args.size(); ++i) {
                    if (std::holds_alternative<std::unique_ptr<ast::SpreadExpr>>(args[i])) {
                        error(diag, loc, std::format("'{}': '...' spread argument must be the sole variadic argument", callee_desc));
                        ok = false;
                        continue;
                    }
                    if (auto arg_ty = check_expr(args[i], locals, module_path, program, diag, element_ty, loop_depth, defer_loop_base, fn_error_type); !assignable_in_module(arg_ty, element_ty, module_path, program)) {
                        error(diag, get_expr_location(args[i]), std::format("'{}' variadic argument {} type mismatch", callee_desc, i - fixed_count + 1));
                        ok = false;
                    }
                }
                return ok;
            }

            const size_t min_args = required_params.value_or(params.size());

            if (is_variadic) {
                if (args.size() < params.size()) {
                    error(diag, loc, std::format("'{}' expects at least {} argument(s), got {}", callee_desc, params.size(), args.size()));
                    return false;
                }
            } else {
                if (args.size() < min_args || args.size() > params.size()) {
                    error(diag, loc, min_args == params.size()
                        ? std::format("'{}' expects {} argument(s), got {}", callee_desc, params.size(), args.size())
                        : std::format("'{}' expects {} to {} argument(s), got {}", callee_desc, min_args, params.size(), args.size()));
                    return false;
                }
            }

            // Omitted trailing args (args.size() < params.size(), only possible when
            // min_args < params.size()) already had their default expressions checked
            // once, at signature-resolution time — no re-check needed here; codegen
            // reads the default expression directly off the callee's decl.
            //
            // Pre-existing bug fixed here (unrelated to inline asm): this loop used to run
            // 'i < args.size()' and index 'params[i]' unconditionally, which crashed (out-of-
            // bounds vector access) for any variadic call site with more arguments than named
            // parameters (e.g. 'printf("%d %d", a, b)', where 'params' has only the 'fmt'
            // entry) — the trailing variadic arguments are handled by the dedicated loop just
            // below, which type-checks 'args[params.size()..]' against no fixed parameter type.
            bool ok = true;
            for (size_t i = 0; i < std::min(args.size(), params.size()); ++i) {
                if (auto arg_ty = check_expr(args[i], locals, module_path, program, diag, params[i], loop_depth, defer_loop_base, fn_error_type); !assignable_in_module(arg_ty, params[i], module_path, program)) {
                    error(diag, get_expr_location(args[i]), std::format("'{}' argument {} type mismatch", callee_desc, i + 1));
                    ok = false;
                }
            }
            if (is_variadic) {
                for (size_t i = params.size(); i < args.size(); ++i) {
                    const auto arg_ty = check_expr(args[i], locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_error_type);
                    if (!is_valid_variadic_arg(arg_ty)) {
                        error(diag, get_expr_location(args[i]), std::format(
                            "'{}' variadic argument {} has a type that violates C default argument promotions: "
                            "variadic arguments must be at least 32 bits wide and floats must be f64; use cast()",
                            callee_desc, i + 1));
                        ok = false;
                    }
                }
            }
            return ok;
        }

        // Thin wrapper over the shared walker (sema.cpp): this file's call sites have a
        // LocalScope, whose bindings shadow module-level import aliases.
        auto try_resolve_namespace_chain(const ast::Expr &expr, const std::string &module_path, LocalScope &locals, Program &program) -> std::optional<std::string> {
            return resolve_expr_namespace_chain(
                expr, module_path, program,
                [&locals](const std::string &name) { return locals.contains(name); },
                /*follow_member_chains=*/true);
        }

        // Resolves an expression as a type reference (not a value).
        // Returns the ResolvedType if expr names a type (locally or via module chain), nullopt otherwise.
        auto try_resolve_type_chain(const ast::Expr &expr, const std::string &module_path, LocalScope &locals, Program &program) -> std::optional<ResolvedType> {
            if (const auto *ident = std::get_if<ast::IdentExpr>(&expr)) {
                if (locals.contains(ident->name)) {
                    return std::nullopt;
                }
                const auto mod_it = program.modules.find(module_path);
                if (mod_it == program.modules.end()) {
                    return std::nullopt;
                }
                const auto sym_it = mod_it->second.symbols.find(ident->name);
                if (sym_it == mod_it->second.symbols.end()) {
                    return std::nullopt;
                }
                if (const auto *ts = std::get_if<TypeSymbol>(&sym_it->second)) {
                    return ts->resolved;
                }
                return std::nullopt;
            }
            if (const auto *mem = std::get_if<std::unique_ptr<ast::MemberExpr>>(&expr)) {
                const auto inner_module = try_resolve_namespace_chain((*mem)->object, module_path, locals, program);
                if (!inner_module) {
                    return std::nullopt;
                }
                const auto mod_it = program.modules.find(*inner_module);
                if (mod_it == program.modules.end()) {
                    return std::nullopt;
                }
                const auto sym_it = mod_it->second.symbols.find((*mem)->member);
                if (sym_it == mod_it->second.symbols.end()) {
                    return std::nullopt;
                }
                if (const auto *ts = std::get_if<TypeSymbol>(&sym_it->second)) {
                    return ts->resolved;
                }
                return std::nullopt;
            }
            return std::nullopt;
        }

        auto check_member_cross_module(const ast::MemberExpr &m, const std::string &target_module_path, Program &program, DiagnosticEngine &diag) -> LvalueInfo {
            const auto mod_it = program.modules.find(target_module_path);
            if (mod_it == program.modules.end()) {
                return {ResolvedType{.kind = TypeKind::Invalid}, false};
            }

            const auto sym_it = mod_it->second.symbols.find(m.member);
            if (sym_it == mod_it->second.symbols.end()) {
                error(diag, m.location, std::format("no member named '{}'", m.member));
                return {ResolvedType{.kind = TypeKind::Invalid}, false};
            }

            return std::visit(
                [&]<typename T>(const T &sym) -> LvalueInfo {
                    using S = std::decay_t<T>;
                    if constexpr (std::is_same_v<S, GlobalSymbol>) {
                        if (!sym.is_pub) {
                            error(diag, m.location, std::format("'{}' is not pub", m.member));
                            return {ResolvedType{.kind = TypeKind::Invalid}, false};
                        }
                        const auto ty = resolve_global_symbol(target_module_path, m.member, program, diag, m.location);
                        return {ty, sym.is_mut};
                    } else {
                        error(diag, m.location, std::format("'{}' is not a value", m.member));
                        return {ResolvedType{.kind = TypeKind::Invalid}, false};
                    }
                },
                sym_it->second);
        }

        auto resolve_lvalue(const ast::Expr &expr, LocalScope &locals, const std::string &module_path, Program &program, DiagnosticEngine &diag, int loop_depth, int defer_loop_base, const ResolvedType *fn_error_type) -> LvalueInfo;

        // `need_writable` controls whether a struct/union-valued `m.object` gets speculatively
        // probed for writability via resolve_lvalue(). That probe is only meaningful when the
        // caller is actually going to use the resulting .writable flag (assignment targets,
        // address-of, etc. - reached via resolve_lvalue()'s own MemberExpr case, which doesn't
        // pass this and keeps the default true). A plain read of `m` (check_expr's MemberExpr
        // case) never looks at .writable, so it passes false: resolve_lvalue's fallback for any
        // object shape it doesn't recognize as inherently addressable (e.g. a CallExpr, as in
        // `f().field`) reports "not an assignable expression" - correct when something is
        // actually being assigned to or addressed, but a spurious compile error for an ordinary
        // read of a field on a temporary struct/union value, which is always legal.
        // A diagnostic-free, single-pass writability probe over an already-type-checked
        // object expression, reading the types check_expr just recorded. resolve_member
        // used to probe via resolve_lvalue(m.object), whose MemberExpr case recurses back
        // into resolve_member — re-type-checking every prefix of 'a.b.c.d = x' at every
        // level (exponential in chain depth) and stacking a spurious "not an assignable
        // expression" on top of the real diagnostic for 'f().field = x'.
        // nullopt = not an lvalue shape at all.
        auto probe_lvalue_writability(const ast::Expr &e, LocalScope &locals, const std::string &module_path, const Program &program) -> std::optional<bool> {
            const auto recorded_type = [&](const ast::Expr &node) -> const ResolvedType * {
                return find_expr_record(program, module_path, &ExprSideTables::expr_types, get_expr_key(node));
            };
            return std::visit(
                [&]<typename T>(const T &v) -> std::optional<bool> {
                    using V = std::decay_t<T>;
                    if constexpr (std::is_same_v<V, ast::IdentExpr>) {
                        if (const auto it = locals.find(v.name); it != locals.end()) {
                            return it->second.is_mut;
                        }
                        if (const auto mod_it = program.modules.find(module_path); mod_it != program.modules.end()) {
                            if (const auto sym_it = mod_it->second.symbols.find(v.name); sym_it != mod_it->second.symbols.end()) {
                                if (const auto *g = std::get_if<GlobalSymbol>(&sym_it->second)) {
                                    return g->is_mut;
                                }
                            }
                        }
                        return std::nullopt;
                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::UnaryExpr>>) {
                        // Writing through a pointer is always allowed; the handle being
                        // const says nothing about the pointee.
                        return v->op == ast::UnaryOp::Deref ? std::optional(true) : std::nullopt;
                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::MemberExpr>>) {
                        const auto *obj_ty = recorded_type(v->object);
                        if (!obj_ty) return std::nullopt;
                        if (obj_ty->kind == TypeKind::Pointer || obj_ty->kind == TypeKind::Opaque) return true;
                        if (obj_ty->kind == TypeKind::Struct || obj_ty->kind == TypeKind::Union) {
                            return probe_lvalue_writability(v->object, locals, module_path, program);
                        }
                        return std::nullopt;
                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::IndexOrInstantiateExpr>>) {
                        const auto *op_ty = recorded_type(v->operand);
                        if (!op_ty) return std::nullopt;
                        // An array is stored inline, so its element inherits the array's
                        // writability; slice/pointer elements live behind the handle.
                        if (op_ty->kind == TypeKind::Array) {
                            return probe_lvalue_writability(v->operand, locals, module_path, program);
                        }
                        return true;
                    } else {
                        return std::nullopt;
                    }
                },
                e);
        }

        auto resolve_member(const ast::MemberExpr &m, LocalScope &locals, const std::string &module_path, Program &program, DiagnosticEngine &diag, const int loop_depth, const int defer_loop_base, const ResolvedType *fn_error_type, const bool need_writable = true) -> LvalueInfo {
            if (const auto target_module = try_resolve_namespace_chain(m.object, module_path, locals, program)) {
                return check_member_cross_module(m, *target_module, program, diag);
            }

            // Handle fully-qualified enum field: e.g. EnumType.field or module.EnumType.field
            if (const auto type_ref = try_resolve_type_chain(m.object, module_path, locals, program)) {
                if (type_ref->kind == TypeKind::Enum) {
                    if (const auto *enum_info = program.enum_at(type_ref->enum_index)) {
                        for (const auto &field : enum_info->fields) {
                            if (field.name == m.member) {
                                return {*type_ref, false};
                            }
                        }
                    }
                    error(diag, m.location, std::format("no enum field named '{}'", m.member));
                    return {ResolvedType{.kind = TypeKind::Invalid}, false};
                }
                // Fully-qualified bitset member: e.g. BitsetType.field
                if (type_ref->kind == TypeKind::Bitset) {
                    if (const auto *bitset_info = program.bitset_at(type_ref->bitset_index)) {
                        if (const auto *enum_info = program.enum_at(bitset_info->member_enum_type.enum_index)) {
                            for (const auto &field : enum_info->fields) {
                                if (field.name == m.member) {
                                    return {*type_ref, false};
                                }
                            }
                        }
                    }
                    error(diag, m.location, std::format("no bitset member named '{}'", m.member));
                    return {ResolvedType{.kind = TypeKind::Invalid}, false};
                }
            }

            const auto object_type = check_expr(m.object, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_error_type);

            ResolvedType effective_type;
            bool writable;

            if (object_type.kind == TypeKind::Pointer) {
                const auto *pointee = program.pointee_at(object_type.pointee_index);
                if (!pointee) {
                    error(diag, m.location, "internal error: invalid pointer index");
                    return {ResolvedType{.kind = TypeKind::Invalid}, false};
                }
                effective_type = *pointee;
                writable = true;
            } else if (object_type.kind == TypeKind::Struct || object_type.kind == TypeKind::Union) {
                effective_type = object_type;
                // Single linear probe over the types check_expr just recorded — never
                // resolve_lvalue, whose MemberExpr case would recurse back into this
                // function and re-check every prefix of the chain. A non-lvalue object
                // ('f().field') probes nullopt -> not writable; the assignment path
                // reports that once, instead of stacking two diagnostics.
                writable = need_writable &&
                           probe_lvalue_writability(m.object, locals, module_path, program).value_or(false);
            } else if (object_type.kind == TypeKind::Trait) {
                error(diag, m.location, "cannot access fields on a trait handle; handles have no visible layout");
                return {ResolvedType{.kind = TypeKind::Invalid}, false};
            } else if (object_type.kind == TypeKind::Any) {
                // 'any' has no fields — it isn't a struct. Use type_of(a) for the type id and
                // cast(a, anyptr)/cast(a, *T) for the data pointer (see check_cast).
                error(diag, m.location, std::format("'any' has no field '{}'", m.member));
                return {ResolvedType{.kind = TypeKind::Invalid}, false};
            } else if (object_type.kind == TypeKind::Opaque) {
                effective_type = object_type;
                writable = true;
            } else if (object_type.kind == TypeKind::Invalid) {
                return {ResolvedType{.kind = TypeKind::Invalid}, false};
            } else {
                error(diag, m.location, "'.' requires a struct, union, or pointer-to-struct/union value");
                return {ResolvedType{.kind = TypeKind::Invalid}, false};
            }

            // Checked on 'effective_type', not 'object_type', so that '*T' — the shape 'self'
            // always has inside a generic impl method — is covered as well as a bare 'T'.
            // Whether an unconstrained 'T' has this field depends entirely on the
            // instantiation, so stay silent and yield Opaque. A trait bound, however, IS a
            // complete statement of what 'T' offers — and it offers methods, never fields.
            if (effective_type.kind == TypeKind::Opaque) {
                if (const auto bound = opaque_bound_name(effective_type, program); !bound.empty()) {
                    error(diag, m.location, std::format(
                        "no field '{}' on a generic parameter bound by '{}'; a trait bound grants methods, "
                        "not fields", m.member, bound));
                    return {ResolvedType{.kind = TypeKind::Invalid}, false};
                }
                return {ResolvedType{.kind = TypeKind::Opaque}, writable};
            }

            if (effective_type.kind == TypeKind::Struct) {
                if (const auto *info = program.struct_at(effective_type.struct_index)) {
                    for (auto &field : info->fields) {
                        if (field.name == m.member) {
                            return {field.type, writable};
                        }
                    }
                }
                error(diag, m.location, std::format("no field named '{}'", m.member));
                return {ResolvedType{.kind = TypeKind::Invalid}, false};
            }

            // TypeKind::Union
            {
                const auto *info = program.union_at(effective_type.union_index);
                if (!info) {
                    error(diag, m.location, "internal error: invalid union index");
                    return {ResolvedType{.kind = TypeKind::Invalid}, false};
                }
                if (info->is_tagged) {
                    error(diag, m.location, "cannot access tagged union variants directly; use 'match' to destructure");
                    return {ResolvedType{.kind = TypeKind::Invalid}, false};
                }
                for (auto &member : info->members) {
                    if (member.name == m.member) {
                        return {member.type, writable};
                    }
                }
            }

            error(diag, m.location, std::format("no member named '{}'", m.member));
            return {ResolvedType{.kind = TypeKind::Invalid}, false};
        }

        // What indexing 'expr' yields, once it is known to BE an index rather than a generic
        // instantiation: the element type, plus the operand type the caller may still need.
        struct IndexedElement {
            ResolvedType element;
            ResolvedType operand;
        };

        // Type-checks 'operand[subscript]' and returns the element type. nullopt means the
        // operand is not indexable at all, already reported.
        //
        // Shared by value position (check_expr) and lvalue position (resolve_lvalue), which
        // carried two copies of this — the finding notes any fix had to be applied to both.
        // What stays at the call sites is what genuinely differs: the GATE in front (an lvalue
        // rejects a generic instantiation as "not an assignable expression"; a value position
        // first tries to read one as a function-pointer value), and writability, which only an
        // lvalue cares about.
        auto check_indexed_element(const ast::IndexOrInstantiateExpr &v, LocalScope &locals, const std::string &module_path,
                                    Program &program, DiagnosticEngine &diag, const int loop_depth, const int defer_loop_base,
                                    const ResolvedType *fn_error_type) -> std::optional<IndexedElement> {
            const auto operand = check_expr(v.operand, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_error_type);
            const auto index = check_expr(std::get<ast::Expr>(v.args[0].value), locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_error_type);
            // Invalid means the index expression already reported; a second message here just
            // buries it.
            if (!index.is_integer() && index.kind != TypeKind::Opaque && index.kind != TypeKind::Invalid) {
                error(diag, v.location, "index must be an integer expression");
            }

            if (operand.kind == TypeKind::Opaque) {
                // Element type is unknowable until 'T' is. Writable, per the caller: 'buf[i] = x'
                // through a generic array must not be pre-emptively rejected.
                return IndexedElement{opaque_unary_result("index", operand, diag, v.location, program), operand};
            }
            if (operand.kind == TypeKind::Pointer) {
                const auto *pointee = program.pointee_at(operand.pointee_index);
                return IndexedElement{pointee ? *pointee : ResolvedType{.kind = TypeKind::Invalid}, operand};
            }
            if (operand.kind == TypeKind::Array) {
                return IndexedElement{array_element_type(operand, module_path, program), operand};
            }
            if (operand.kind == TypeKind::Slice) {
                return IndexedElement{slice_element_type(operand, module_path, program), operand};
            }
            error(diag, v.location, "indexing requires a pointer, array, or slice operand");
            return std::nullopt;
        }

        auto resolve_lvalue(const ast::Expr &expr, LocalScope &locals, const std::string &module_path, Program &program, DiagnosticEngine &diag, const int loop_depth, const int defer_loop_base, const ResolvedType *fn_error_type) -> LvalueInfo {
            return std::visit(
                [&]<typename T>(const T &v) -> LvalueInfo {
                    using V = std::decay_t<T>;

                    if constexpr (std::is_same_v<V, ast::IdentExpr>) {
                        if (auto it = locals.find(v.name); it != locals.end()) {
                            return {it->second.type, it->second.is_mut};
                        }
                        const auto mod_it = program.modules.find(module_path);
                        if (mod_it != program.modules.end()) {
                            if (auto sym_it = mod_it->second.symbols.find(v.name); sym_it != mod_it->second.symbols.end()) {
                                if (auto *g = std::get_if<GlobalSymbol>(&sym_it->second)) {
                                    const ResolvedType ty = resolve_global_symbol(module_path, v.name, program, diag, v.location);
                                    return {ty, g->is_mut};
                                }
                                error(diag, v.location, std::format("cannot assign to '{}': not a variable", v.name));
                                return {ResolvedType{.kind = TypeKind::Invalid}, false};
                            }
                        }
                        error(diag, v.location, std::format("unknown identifier '{}'", v.name));
                        return {ResolvedType{.kind = TypeKind::Invalid}, false};

                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::UnaryExpr>>) {
                        if (v->op != ast::UnaryOp::Deref) {
                            error(diag, v->location, "not an assignable expression");
                            return {ResolvedType{.kind = TypeKind::Invalid}, false};
                        }
                        const ResolvedType ptr_ty = check_expr(v->operand, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_error_type);
                        if (ptr_ty.kind == TypeKind::Opaque) {
                            // Writable: 'p.* = x' through a generic pointer is only knowable
                            // per-instantiation, so the eager pass must not reject it.
                            return {opaque_unary_result("dereference", ptr_ty, diag, v->location, program), true};
                        }
                        if (ptr_ty.kind == TypeKind::Trait) {
                            error(diag, v->location, "cannot dereference a trait handle");
                            return {ResolvedType{.kind = TypeKind::Invalid}, false};
                        }
                        if (ptr_ty.kind != TypeKind::Pointer) {
                            error(diag, v->location, "cannot dereference a non-pointer value");
                            return {ResolvedType{.kind = TypeKind::Invalid}, false};
                        }
                        const auto *pointee = program.pointee_at(ptr_ty.pointee_index);
                        if (!pointee) {
                            error(diag, v->location, "internal error: invalid pointer index");
                            return {ResolvedType{.kind = TypeKind::Invalid}, false};
                        }
                        return {*pointee, true};

                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::MemberExpr>>) {
                        return resolve_member(*v, locals, module_path, program, diag, loop_depth, defer_loop_base, fn_error_type);

                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::IndexOrInstantiateExpr>>) {
                        // A generic instantiation ('List[i32] = x') is never an assignable
                        // lvalue — only the ordinary single-Expr-arg index shape is. (Full
                        // declaration-based classification lives in check_expr's
                        // IndexOrInstantiateExpr handling; here it's enough to reject anything
                        // that isn't shaped like an ordinary index.)
                        if (v->args.size() != 1 || !std::holds_alternative<ast::Expr>(v->args[0].value)) {
                            error(diag, v->location, "not an assignable expression");
                            return {ResolvedType{.kind = TypeKind::Invalid}, false};
                        }
                        const auto indexed = check_indexed_element(*v, locals, module_path, program, diag, loop_depth, defer_loop_base, fn_error_type);
                        if (!indexed) return {ResolvedType{.kind = TypeKind::Invalid}, false};
                        // Indexing THROUGH a pointer or slice is always writable — the handle
                        // being const says nothing about the pointee. An array is stored inline,
                        // so writing an element needs the array itself to be writable.
                        if (indexed->operand.kind == TypeKind::Array) {
                            auto owner = resolve_lvalue(v->operand, locals, module_path, program, diag, loop_depth, defer_loop_base, fn_error_type);
                            return {indexed->element, owner.writable};
                        }
                        return {indexed->element, true};

                    } else {
                        // get_expr_location(), not a zero-valued SourceLocation{}: an empty
                        // location's filename doesn't match any open document, so the LSP's
                        // diagnostics publisher (see diagnostics.cpp) silently drops it - the
                        // CLI would still print it, but editors would report success on a
                        // build that actually fails.
                        error(diag, get_expr_location(expr), "not an assignable expression");
                        return {ResolvedType{.kind = TypeKind::Invalid}, false};
                    }
                },
                expr);
        }
    }

    // Forward-declared: defined alongside check_asm_stmt further down (both build on the shared
    // check_asm_instructions driver), but check_expr's AsmExpr case (below) needs to call it.
    auto check_asm_expr(const ast::AsmExpr &expr, LocalScope &locals, const std::string &module_path,
                         Program &program, DiagnosticEngine &diag,
                         const std::optional<ResolvedType> &expected) -> ResolvedType;

    // Stable, collision-free-enough symbol suffix for a concrete instantiation (e.g.
    // "__i32", "__16", "__i32_16" for a mixed [T, N] list) — sanitizes describe_type's
    // human-readable output (which may contain characters illegal in a symbol name, e.g.
    // '*') down to alnum/underscore.
    // Builds the substitution env binding each of 'params' (in order) to its concrete
    // argument — shared by instantiate_generic_function/instantiate_generic_method (for
    // resolve_type_with_generic_env) and check_generic_function_instance_body (for pushing
    // onto Program::active_generic_env_stack while checking the body).
    auto build_generic_binding_env(const std::vector<ast::GenericParam> &params, const std::vector<GenericArgValue> &args) -> GenericBindingEnv {
        GenericBindingEnv env;
        env.reserve(args.size());
        for (size_t i = 0; i < params.size() && i < args.size(); ++i) {
            const auto &param = params[i];
            const auto &arg = args[i];
            env.push_back(GenericBinding{
                .param_name = param.name, .is_type = arg.is_type, .type_value = arg.type_arg,
                .const_value = arg.value_arg, .const_value_type = arg.value_arg_scalar_type,
            });
        }
        return env;
    }

    auto mangle_generic_args(const std::vector<GenericArgValue> &args, const Program &program) -> std::string {
        std::string out;
        for (const auto &arg : args) {
            out += "__";
            if (arg.is_type) {
                // A symbol name must follow type IDENTITY, and ResolvedType::operator==
                // deliberately ignores opaque_param_index — so two args that compare equal
                // must not mangle differently just because one is spelled 'T' and the other
                // 'U'. describe_type prefers that spelling (it is written for humans), so
                // Opaque is answered here instead of going through it. Only reachable for a
                // suppressed template instance, which is never emitted; kept exact anyway,
                // since a name that disagrees with equality is a trap either way.
                std::string name;
                if (arg.type_arg.kind == TypeKind::Opaque) {
                    // The bound is part of identity (it lives in trait_index, which operator==
                    // DOES compare); the parameter's spelling is not. Rendered here rather than
                    // by describe_type so the two can never drift apart silently.
                    const auto *bound = program.trait_at(arg.type_arg.trait_index);
                    name = bound ? std::format("<generic: {}>", bound->name) : "<generic>";
                } else {
                    name = describe_type(arg.type_arg, program);
                }
                for (char &c : name) {
                    if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
                }
                out += name;
            } else if (const auto *iv = std::get_if<int64_t>(&arg.value_arg)) {
                out += std::to_string(*iv);
            }
        }
        return out;
    }

    // Temporarily shadows each TYPE generic-param of 'decl_params'/'args' as a TypeSymbol in
    // 'module's own symbol table, so ordinary type-resolution reached from inside
    // check_expr/check_stmt (size_of(T), *T casts, local var decl type annotations, ...)
    // finds the bound concrete type via the exact same lookup path any other named type
    // uses — the one piece of generic-body support that can't reuse Resolver::generic_env
    // directly, since check_expr's own internals call the plain (non-generic-env-aware)
    // resolve_type free function throughout. Returns the shadowed entries so the caller can
    // restore them afterward via restore_shadowed_symbols.
    auto shadow_generic_type_params(const std::vector<ast::GenericParam> &decl_params,
                                     const std::vector<GenericArgValue> &args,
                                     ProgramModule &module) -> std::vector<std::pair<std::string, std::optional<Symbol>>> {
        std::vector<std::pair<std::string, std::optional<Symbol>>> shadowed;
        for (size_t i = 0; i < decl_params.size() && i < args.size(); ++i) {
            if (!args[i].is_type) continue;
            const auto &name = decl_params[i].name;
            std::optional<Symbol> prior;
            if (const auto it = module.symbols.find(name); it != module.symbols.end()) prior = it->second;
            shadowed.emplace_back(name, prior);
            module.symbols[name] = TypeSymbol{.decl = nullptr, .resolved = args[i].type_arg, .is_pub = false,
                                               .location = {}, .is_generic_param_shadow = true};
        }
        return shadowed;
    }

    void restore_shadowed_symbols(ProgramModule &module, const std::vector<std::pair<std::string, std::optional<Symbol>>> &shadowed) {
        for (const auto &[name, prior] : shadowed) {
            if (prior) module.symbols[name] = *prior;
            else module.symbols.erase(name);
        }
    }

    // RAII pairing of shadow_generic_type_params + active_generic_env_stack, the two halves of
    // "make this instantiation's generic parameters visible to ordinary resolution". Both are
    // needed together and both must be undone on every exit path (see ScopedResolvePush's doc
    // comment on why a missed pop is worse than a leak — it dangles). Used for signature
    // resolution and body checking alike; the env must outlive the guard.
    // The module is re-looked-up by path on scope exit rather than held as a reference:
    // checking a default expression or a body can declare a module on demand, and inserting
    // into Program::modules (an unordered_map) may rehash and invalidate any outstanding
    // ProgramModule&.
    // 'exprs', when non-null, additionally routes every per-node record produced inside the
    // scope into that instantiation's own ExprSideTables instead of the module's — see
    // ExprSideTables and expr_tables_for_write (sema.hpp). It is a THIRD thing that must be
    // undone on every exit path, so it lives here rather than at the call sites.
    class ScopedGenericScope {
      public:
        ScopedGenericScope(Program &program, std::string module_path, const std::vector<ast::GenericParam> &decl_params,
                            const std::vector<GenericArgValue> &args, const GenericBindingEnv &env,
                            ExprSideTables *exprs = nullptr)
            : program_(program), module_path_(std::move(module_path)), pushed_exprs_(exprs != nullptr),
              shadowed_(shadow_generic_type_params(decl_params, args, program.modules.at(module_path_))) {
            program_.active_generic_env_stack.push_back(&env);
            if (exprs) program_.active_expr_tables.push_back(exprs);
        }
        ~ScopedGenericScope() {
            if (pushed_exprs_) program_.active_expr_tables.pop_back();
            program_.active_generic_env_stack.pop_back();
            restore_shadowed_symbols(program_.modules.at(module_path_), shadowed_);
        }

        ScopedGenericScope(const ScopedGenericScope &) = delete;
        auto operator=(const ScopedGenericScope &) -> ScopedGenericScope & = delete;

      private:
        Program &program_;
        std::string module_path_;
        bool pushed_exprs_;
        std::vector<std::pair<std::string, std::optional<Symbol>>> shadowed_;
    };

    // RAII for Program::template_check_depth, the last bare set/reset counter in this
    // file — everything else scope-shaped here is ScopedGenericScope-style. A counter
    // (not a flag) because checking one template can reach another.
    class ScopedTemplateCheck {
      public:
        explicit ScopedTemplateCheck(Program &program) : program_(program) {
            ++program_.template_check_depth;
        }
        ~ScopedTemplateCheck() {
            --program_.template_check_depth;
        }

        ScopedTemplateCheck(const ScopedTemplateCheck &) = delete;
        auto operator=(const ScopedTemplateCheck &) -> ScopedTemplateCheck & = delete;

      private:
        Program &program_;
    };

    // One immutable LocalScope binding per VALUE generic-param — mirrors
    // Resolver::generic_env_locals (type_resolver.cpp), duplicated here since check_expr's
    // LocalScope isn't reachable from that file's Resolver.
    void add_generic_value_param_locals(const std::vector<ast::GenericParam> &decl_params,
                                         const std::vector<GenericArgValue> &args, LocalScope &locals) {
        for (size_t i = 0; i < decl_params.size() && i < args.size(); ++i) {
            if (args[i].is_type) continue;
            locals[decl_params[i].name] = LocalBinding{.type = args[i].value_arg_scalar_type, .is_mut = false};
        }
    }

    // Checks a generic function/method instance's body exactly once (idempotent —
    // body_checked guards re-entry, including a recursive generic call reaching the same
    // instance while its own body is still being checked). Drives both the per-instantiation
    // check and, via check_generic_templates_for_program, the eager check of the DECLARATION
    // itself with its parameters bound to Opaque; the two differ only in the arguments.
    void check_generic_function_instance_body(size_t idx, Program &program, DiagnosticEngine &diag) {
        auto &instance = *program.generic_fn_instances[idx];
        if (instance.body_checked) return;
        instance.body_checked = true;
        if (!instance.decl && !instance.impl_decl) return;

        const auto &generic_params = instance.decl ? instance.decl->generic_params
                                                     : *instance.generic_params_for_method;

        // Held for the duration of body-checking so any nested resolve_type/
        // resolve_declared_type call reached from inside check_stmt/check_expr (a local var
        // decl's type annotation, size_of's operand, a cast target, ...) can find this
        // instance's type and value bindings — see Program::active_generic_env_stack's doc
        // comment and ScopedGenericScope's. Complements add_generic_value_param_locals below
        // (ordinary check_expr IdentExpr lookup only); the scope is what makes bare-type-
        // position resolution (not just value-expression lookup) inside the body work.
        const auto env = build_generic_binding_env(generic_params, instance.args);
        const ScopedGenericScope generic_scope(program, instance.module_path, generic_params, instance.args, env,
                                                &program.exprs_for_fn_instance(idx));

        LocalScope locals;
        for (auto &[gname, gsym] : program.modules.at(instance.module_path).symbols) {
            if (auto *g = std::get_if<GlobalSymbol>(&gsym)) {
                locals[gname] = LocalBinding{.type = g->type, .is_mut = g->is_mut};
            }
        }
        add_generic_value_param_locals(generic_params, instance.args, locals);

        if (instance.decl) {
            for (size_t i = 0; i < instance.decl->params.size(); ++i) {
                locals[instance.decl->params[i].name] = LocalBinding{.type = instance.param_types[i], .is_mut = instance.decl->params[i].is_mut};
            }
            check_stmt(instance.decl->body, locals, instance.module_path, program, diag, instance.return_types, 0);
        } else {
            const auto self_ptr = intern_pointer(program, *instance.self_type);
            locals["self"] = LocalBinding{.type = self_ptr, .is_mut = instance.impl_decl->is_mut_self};
            for (size_t i = 0; i < instance.impl_decl->params.size(); ++i) {
                locals[instance.impl_decl->params[i].name] = LocalBinding{.type = instance.param_types[i], .is_mut = instance.impl_decl->params[i].is_mut};
            }
            check_stmt(instance.impl_decl->body, locals, instance.module_path, program, diag, instance.return_types, 0);
        }
    }

    // Type-checks the field default initializers of every monomorphized generic struct.
    //
    // check_struct_field_defaults_for_module (sema.cpp) can't do this: it walks module.symbols
    // and requires TypeSymbol::resolved, which a generic TEMPLATE never has — resolving
    // 'slots: *Slot[K, V] = nil' means nothing until K and V are bound. So a generic struct's
    // defaults went entirely unchecked, which left no expr_types entry for them. Codegen's
    // runtime braced-initializer path emits an omitted field's default via emit_expr, whose
    // expr_type() falls back to Invalid when the entry is missing — lowering 'count: usize = 0'
    // to a zero-width 'i0 0' and failing LLVM module verification. Type errors in those
    // defaults also went unreported.
    //
    // Runs as a whole-program pass after body checking, since that is when most instantiations
    // come into existence. Checking one instance's defaults can itself instantiate another
    // generic type, so this loops to a fixed point, with 'field_defaults_checked' both
    // preventing rework and guaranteeing termination.
    //
    // Note that all instantiations of one generic struct share the same init AST nodes, so they
    // share expr_types entries and the last instantiation checked wins. That is pre-existing
    // behaviour for generic function bodies too (they are checked per instance against the same
    // shared map); it only becomes observable for a default whose TYPE varies per instantiation
    // ('x: K = 0'), not for the common 'count: usize = 0' / 'p: *K = nil' shapes.
    void check_generic_struct_field_defaults_for_program(Program &program, DiagnosticEngine &diag) {
        bool progressed = true;
        while (progressed) {
            progressed = false;
            // Index-based, re-fetching every time: check_expr below can instantiate another
            // generic type, and Program::structs is a plain vector whose reallocation would
            // invalidate any StructInfo reference held across the call (the same hazard
            // check_struct_field_defaults_for_module documents).
            for (size_t si = 0; si < program.structs.size(); ++si) {
                if (!program.structs[si].generic_instance) continue;
                if (program.structs[si].field_defaults_checked) continue;
                program.structs[si].field_defaults_checked = true;
                progressed = true;

                // Copied by value — the slot may move under us for the reason above.
                const auto instance_info = *program.structs[si].generic_instance;

                const auto mod_it = program.modules.find(instance_info.decl_module);
                if (mod_it == program.modules.end()) continue;
                const auto sym_it = mod_it->second.symbols.find(instance_info.decl_name);
                if (sym_it == mod_it->second.symbols.end()) continue;
                const auto *ts = std::get_if<TypeSymbol>(&sym_it->second);
                if (!ts || !ts->decl) continue;
                const auto *struct_decl = std::get_if<std::unique_ptr<ast::StructType>>(&ts->decl->type);
                if (!struct_decl) continue;

                const auto env = build_generic_binding_env(ts->decl->generic_params, instance_info.args);
                // Field-default init exprs are shared across every instantiation of this
                // struct exactly like a generic function's body is, so they need their own
                // records too — keyed by the global slot, since a monomorphized struct has no
                // GenericFunctionInstance to hang them off.
                const ScopedGenericScope generic_scope(program, instance_info.decl_module,
                                                        ts->decl->generic_params, instance_info.args, env,
                                                        &program.exprs_for_type_instance(static_cast<int>(si)));
                LocalScope scope;
                add_generic_value_param_locals(ts->decl->generic_params, instance_info.args, scope);

                for (size_t i = 0; i < (*struct_decl)->fields.size(); ++i) {
                    const auto &field = (*struct_decl)->fields[i];
                    if (!field.init) continue;

                    const auto *struct_info = program.struct_at(static_cast<int>(si));
                    if (!struct_info || i >= struct_info->fields.size()) break;
                    const auto field_type = struct_info->fields[i].type;

                    const auto init_ty = check_expr(*field.init, scope, instance_info.decl_module, program, diag, field_type, 0);
                    if (!is_assignable(init_ty, field_type, program)) {
                        diag.report_error(DiagnosticStage::Sema, field.location,
                            std::format("default value type mismatch for field '{}'", field.name));
                    }
                }
            }
        }
    }

    // Walks a braced initializer's element expressions without judging its SHAPE — used when
    // the expected type is an unbound generic parameter, where there is no concrete aggregate
    // to match field names or element counts against, but the elements themselves are still
    // ordinary expressions worth checking.
    //
    // Each element is checked with Opaque as its expected type, NOT with no expected type: a
    // field initialized from an enum dot-literal ('.state = .Occupied') needs *some* expected
    // type or it reports "cannot resolve '.Occupied'", and the only honest one available here
    // is "unknown".
    void check_braced_initializer_elements(const ast::BracedInitializerExpr &init, LocalScope &locals,
                                            const std::string &module_path, Program &program, DiagnosticEngine &diag,
                                            const int loop_depth, const int defer_loop_base,
                                            const ResolvedType *fn_error_type) {
        std::visit([&]<typename BV>(const BV &bv) {
            using BVT = std::decay_t<BV>;
            const ResolvedType opaque{.kind = TypeKind::Opaque};
            if constexpr (std::is_same_v<BVT, ast::StructExpr>) {
                for (const auto &f : bv.fields) {
                    if (std::holds_alternative<ast::UndefinedExpr>(f.expr)) continue;
                    check_expr(f.expr, locals, module_path, program, diag, opaque, loop_depth, defer_loop_base, fn_error_type);
                }
            } else if constexpr (std::is_same_v<BVT, ast::ArrayExpr>) {
                for (const auto &e : bv.values) {
                    if (std::holds_alternative<ast::UndefinedExpr>(e)) continue;
                    check_expr(e, locals, module_path, program, diag, opaque, loop_depth, defer_loop_base, fn_error_type);
                }
            }
            // EmptyExpr ('{}') and BitsetExpr ('{.A, .B}') carry no sub-expressions.
        }, init);
    }

    // Enforces '[T: Hashable]' at the point of instantiation: the argument supplied for a
    // bounded parameter must implement the bound. This is the half of bounds that protects
    // the CALLER — the declaration-side half (only the bound's methods are usable inside the
    // body) is enforced by the Opaque operation rules.
    // Returns false if any bound was unsatisfied, so the caller can skip checking the body
    // against an argument already known to be wrong — otherwise every use of the bound's
    // methods inside the body reports a second, more confusing "no method" error on top of
    // the one real message.
    auto check_generic_bounds(const std::vector<ast::GenericParam> &params, const std::vector<GenericArgValue> &args,
                               const std::string &module_path, Program &program, DiagnosticEngine &diag,
                               const SourceLocation &use_loc) -> bool {
        bool ok = true;
        for (size_t i = 0; i < params.size() && i < args.size(); ++i) {
            if (!args[i].is_type) continue;
            const int bound = generic_param_bound_trait_index(params[i], module_path, program, diag);
            if (bound < 0) continue;

            // Still a template: this is the eager pass instantiating with the parameter
            // unbound, not a real argument. Nothing to check against.
            if (contains_opaque(args[i].type_arg, program)) continue;

            const auto [arg_module, arg_name] = find_type_module_and_name(args[i].type_arg, program);
            if (arg_module.empty()) {
                // A builtin or synthesized type (i32, *u8, a slice): those have no module and
                // can carry no impl, so they satisfy no bound.
                const auto *trait_info = program.trait_at(bound);
                diag.report_error(DiagnosticStage::Sema, use_loc, std::format(
                    "'{}' does not implement trait '{}', required by generic parameter '{}'",
                    describe_type(args[i].type_arg, program), trait_info ? trait_info->name : "<trait>", params[i].name));
                ok = false;
                continue;
            }
            if (find_trait_provider(program, arg_module, arg_name, bound) < 0) {
                const auto *trait_info = program.trait_at(bound);
                diag.report_error(DiagnosticStage::Sema, use_loc, std::format(
                    "'{}' does not implement trait '{}', required by generic parameter '{}'",
                    arg_name, trait_info ? trait_info->name : "<trait>", params[i].name));
                ok = false;
            }
        }
        return ok;
    }

    // The argument list that stands in for "no instantiation yet": each type parameter binds to
    // an unconstrained Opaque (or, once bounds exist, one carrying the bound's trait index),
    // and each value parameter to an Opaque-typed constant. See TypeKind::Opaque.
    auto opaque_args_for(const std::vector<ast::GenericParam> &params, Program &program,
                          const std::string &module_path, DiagnosticEngine &diag,
                          const bool report_bad_bounds = false) -> std::vector<GenericArgValue> {
        std::vector<GenericArgValue> args;
        args.reserve(params.size());
        for (const auto &param : params) {
            // The parameter's own spelling, carried along for display only (see
            // ResolvedType::opaque_param_index). This is the ONE place a name enters the type
            // system; everywhere else either propagates it through an operator or leaves it
            // at -1.
            const int name_index = program.intern_opaque_param_name(param.name);
            if (is_generic_type_param(param.type)) {
                args.push_back(GenericArgValue{
                    .is_type = true,
                    .type_arg = ResolvedType{.kind = TypeKind::Opaque,
                                              .trait_index = generic_param_bound_trait_index(param, module_path, program, diag, report_bad_bounds),
                                              .opaque_param_index = name_index},
                });
            } else {
                // The folded value is unused (a suppressed instantiation is never mangled or
                // cached); the Opaque scalar TYPE is what makes every use of the parameter
                // opaque. See args_contain_opaque.
                args.push_back(GenericArgValue{
                    .is_type = false,
                    .value_arg = int64_t{0},
                    .value_arg_scalar_type = ResolvedType{.kind = TypeKind::Opaque, .opaque_param_index = name_index},
                });
            }
        }
        return args;
    }

    auto instantiate_generic_function(Program &program, DiagnosticEngine &diag, const std::string &module_path,
                                       const std::string &decl_name, std::vector<GenericArgValue> args,
                                       const SourceLocation &use_loc) -> size_t {
        // A bare-imported generic function is an ALIAS sharing the origin's FunctionDecl, so
        // instantiate it under the ORIGIN's module rather than the importer's. Every caller
        // below passes the module the NAME was found in, which for an alias is the importer —
        // and instantiating there would check the instance body with the importer's symbol
        // table as its resolution scope (the defining module's private helpers invisible,
        // same-named symbols in the importer silently capturing) and key the instance's
        // llvm::Function off a module that never declared it. Redirecting here covers every
        // instantiation path at once, and mirrors the origin-redirect every other symbol kind
        // already does in sema.cpp's resolve_signatures_for_module/resolve_values_for_module.
        // It also dedupes: two importers of the same generic now share one instantiation.
        if (const auto mod_it = program.modules.find(module_path); mod_it != program.modules.end()) {
            const auto &origins = mod_it->second.bare_import_origins;
            if (const auto o = origins.find(decl_name); o != origins.end()) {
                return instantiate_generic_function(program, diag, o->second.module_path, o->second.symbol_name,
                                                     std::move(args), use_loc);
            }
        }

        // See args_contain_opaque: a 'g[T]' reached from inside a generic declaration is not a
        // real monomorphization. Its signature is still resolved below so the call's arity and
        // arguments are checked, but it is kept out of every registry.
        const bool suppressed = args_contain_opaque(args, program);

        const GenericInstanceKey key{.module_path = module_path, .decl_name = decl_name, .args = args};

        if (!suppressed) {
            if (const auto it = program.generic_fn_instance_lookup.find(key); it != program.generic_fn_instance_lookup.end()) {
                const size_t idx = it->second;
                // Mark needed on the CACHE HIT too, not only on first creation. A generic
                // first reached by the eager pass is created with the mark suppressed; if
                // the first real call site then found it cached and returned here without
                // marking it, nothing would ever emit it and the link would fail.
                if (program.template_check_depth == 0) program.generic_fn_instances_needed.insert(idx);
                return idx;
            }
        }

        auto push_invalid = [&]() -> size_t {
            program.generic_fn_instances.push_back(std::make_unique<GenericFunctionInstance>());
            const size_t idx = program.generic_fn_instances.size() - 1;
            program.generic_fn_instance_lookup.emplace(key, idx);
            return idx;
        };

        const auto mod_it = program.modules.find(module_path);
        if (mod_it == program.modules.end()) {
            diag.report_error(DiagnosticStage::Sema, use_loc, std::format("internal error: module '{}' not found", module_path));
            return push_invalid();
        }
        const auto sym_it = mod_it->second.symbols.find(decl_name);
        if (sym_it == mod_it->second.symbols.end()) {
            diag.report_error(DiagnosticStage::Sema, use_loc, std::format("unknown function '{}'", decl_name));
            return push_invalid();
        }
        const auto *fs = std::get_if<FunctionSymbol>(&sym_it->second);
        if (!fs || !fs->decl) {
            diag.report_error(DiagnosticStage::Sema, use_loc, std::format("'{}' is not a function", decl_name));
            return push_invalid();
        }
        const ast::FunctionDecl &decl = *fs->decl;

        if (args.size() != decl.generic_params.size()) {
            diag.report_error(DiagnosticStage::Sema, use_loc, std::format(
                "'{}' expects {} generic argument(s), got {}", decl_name, decl.generic_params.size(), args.size()));
            return push_invalid();
        }

        // Not fatal: the SIGNATURE is still resolved below, so the call site gets sensible
        // param/return types and is checked normally. Only the body is skipped (see the use of
        // 'bounds_ok' further down) — checking it against an argument already known not to
        // satisfy the bound produces a cascade that buries the one message that matters.
        const bool bounds_ok = check_generic_bounds(decl.generic_params, args, module_path, program, diag, use_loc);

        const auto env = build_generic_binding_env(decl.generic_params, args);

        auto instance = std::make_unique<GenericFunctionInstance>();
        instance->decl = &decl;
        instance->module_path = module_path;
        instance->args = args;
        instance->mangled_name = decl_name + mangle_generic_args(args, program);

        // spec.md §22: "Default parameter values may reference the enclosing declaration's own
        // generic parameters" — type parameters as well as value ones. That needs the same
        // generic scope body-checking gets, because a default expression is an ordinary
        // expression: it reaches check_expr, whose internals call the plain resolve_type and
        // is_constant_expr, neither of which takes an explicit env. Held across the whole
        // signature region so 'hash := fnv1a[K]', 'n := size_of(K)' and
        // 'fill: [N]u8 = default' all resolve 'K'/'N'.
        //
        // The signature region needs this instance's own ExprSideTables just as much as the
        // body does: a default like 'hash := fnv1a[K]' records an expr_generic_fn_instance
        // entry on a node SHARED by every instantiation, and the index it resolves to differs
        // per instantiation. Owned locally and moved into the Program map once the instance's
        // index exists — the index cannot be predicted here, since resolving this signature
        // may itself instantiate other generics and push them first.
        auto instance_exprs = std::make_unique<ExprSideTables>();
        const ScopedGenericScope generic_scope(program, module_path, decl.generic_params, args, env, instance_exprs.get());
        LocalScope default_scope;
        add_generic_value_param_locals(decl.generic_params, args, default_scope);

        for (auto &p : decl.params) {
            ResolvedType pt;
            if (p.type) {
                pt = resolve_type_with_generic_env(*p.type, module_path, program, diag, env, &decl.generic_params);
            } else {
                // ':=' inferred-type param — its type IS whatever its default expression checks
                // to, so this is the one place a param type comes from check_expr rather than
                // resolve_type. 'default_scope' carries the value generic-params as locals; the
                // type ones are visible via generic_scope above.
                pt = check_expr(*p.default_value, default_scope, module_path, program, diag, std::nullopt, 0);
            }
            if (p.is_variadic) {
                instance->is_variadic = true;
                instance->variadic_element_type = pt;
                instance->param_types.push_back(intern_slice(program, pt));
            } else {
                instance->param_types.push_back(pt);
            }
        }
        for (auto &rt : decl.return_types) {
            instance->return_types.push_back(resolve_type_with_generic_env(rt, module_path, program, diag, env, &decl.generic_params));
        }

        check_param_defaults(decl.params, instance->param_types, instance->required_params,
                              instance->param_default_is_const, module_path, program, diag, &default_scope);

        program.generic_fn_instances.push_back(std::move(instance));
        const size_t idx = program.generic_fn_instances.size() - 1;
        // 'generic_scope' is still live and holds a raw pointer to the pointee; moving the
        // unique_ptr does not move what it points at.
        program.generic_fn_instance_exprs[idx] = std::move(instance_exprs);

        if (suppressed) {
            // Not cached, not marked for emission, body not checked here — the callee's own
            // eager pass checks its body once, at its declaration. The index is still returned
            // so the caller can check this call's arguments against the resolved signature.
            return idx;
        }

        program.generic_fn_instance_lookup.emplace(key, idx);
        if (program.template_check_depth == 0) program.generic_fn_instances_needed.insert(idx);

        if (bounds_ok) check_generic_function_instance_body(idx, program, diag);

        return idx;
    }

    auto instantiate_generic_method(Program &program, DiagnosticEngine &diag, const ResolvedType &receiver_instantiation,
                                     const std::string &method_name, const SourceLocation &use_loc) -> std::optional<size_t> {
        const auto [type_module, type_name] = find_type_module_and_name(receiver_instantiation, program);
        if (type_module.empty()) return std::nullopt;

        const auto *template_method = find_method(receiver_instantiation, method_name, program);
        if (!template_method) return std::nullopt;

        // The receiver's own concrete args (read off its ResolvedType's *Info.generic_instance,
        // already resolved by find_type_module_and_name's fast path above via the type
        // itself) — re-derive them directly here since GenericInstanceInfo isn't returned by
        // that lookup.
        std::vector<GenericArgValue> args;
        switch (receiver_instantiation.kind) {
        case TypeKind::Struct:
            if (const auto *info = program.struct_at(receiver_instantiation.struct_index); info && info->generic_instance) args = info->generic_instance->args;
            break;
        case TypeKind::Enum:
            if (const auto *info = program.enum_at(receiver_instantiation.enum_index); info && info->generic_instance) args = info->generic_instance->args;
            break;
        case TypeKind::Union:
            if (const auto *info = program.union_at(receiver_instantiation.union_index); info && info->generic_instance) args = info->generic_instance->args;
            break;
        case TypeKind::Bitset:
            if (const auto *info = program.bitset_at(receiver_instantiation.bitset_index); info && info->generic_instance) args = info->generic_instance->args;
            break;
        default:
            break;
        }

        // Same rule as instantiate_generic_function — see args_contain_opaque.
        const bool suppressed = args_contain_opaque(args, program);

        const GenericInstanceKey key{.module_path = type_module, .decl_name = type_name + "::" + method_name, .args = args};
        if (!suppressed) {
            if (const auto it = program.generic_fn_instance_lookup.find(key); it != program.generic_fn_instance_lookup.end()) {
                const size_t idx = it->second;
                // Mark on the cache hit too — see the identical note in
                // instantiate_generic_function.
                if (program.template_check_depth == 0) program.generic_fn_instances_needed.insert(idx);
                return idx;
            }
        }

        static const std::vector<ast::GenericParam> empty_generic_params;
        const auto &impl_generic_params = template_method->impl_generic_params ? *template_method->impl_generic_params
                                                                                : empty_generic_params;
        if (args.size() != impl_generic_params.size()) {
            diag.report_error(DiagnosticStage::Sema, use_loc, std::format(
                "internal error: method '{}' impl generic arity ({}) does not match receiver's instantiation arity ({})",
                method_name, impl_generic_params.size(), args.size()));
            return std::nullopt;
        }

        const auto env = build_generic_binding_env(impl_generic_params, args);

        auto instance = std::make_unique<GenericFunctionInstance>();
        instance->impl_decl = template_method->decl;
        instance->module_path = type_module;
        instance->args = args;
        instance->self_type = receiver_instantiation;
        instance->generic_params_for_method = &impl_generic_params;
        instance->mangled_name = type_name + mangle_generic_args(args, program) + "::" + method_name;

        // Same generic scope as instantiate_generic_function above — see its comment for why a
        // default expression needs it, and why the tables are owned locally until 'idx' exists.
        auto instance_exprs = std::make_unique<ExprSideTables>();
        const ScopedGenericScope generic_scope(program, type_module, impl_generic_params, args, env, instance_exprs.get());
        LocalScope default_scope;
        add_generic_value_param_locals(impl_generic_params, args, default_scope);

        for (auto &p : template_method->decl->params) {
            ResolvedType pt;
            if (p.type) {
                pt = resolve_type_with_generic_env(*p.type, type_module, program, diag, env, &impl_generic_params);
            } else {
                pt = check_expr(*p.default_value, default_scope, type_module, program, diag, std::nullopt, 0);
            }
            if (p.is_variadic) {
                instance->is_variadic = true;
                instance->variadic_element_type = pt;
                instance->param_types.push_back(intern_slice(program, pt));
            } else {
                instance->param_types.push_back(pt);
            }
        }
        for (auto &rt : template_method->decl->return_types) {
            instance->return_types.push_back(resolve_type_with_generic_env(rt, type_module, program, diag, env, &impl_generic_params));
        }

        check_param_defaults(template_method->decl->params, instance->param_types, instance->required_params,
                              instance->param_default_is_const, type_module, program, diag, &default_scope);

        program.generic_fn_instances.push_back(std::move(instance));
        const size_t idx = program.generic_fn_instances.size() - 1;
        program.generic_fn_instance_exprs[idx] = std::move(instance_exprs);

        if (suppressed) return idx; // see instantiate_generic_function's identical branch

        program.generic_fn_instance_lookup.emplace(key, idx);
        if (program.template_check_depth == 0) program.generic_fn_instances_needed.insert(idx);

        check_generic_function_instance_body(idx, program, diag);

        return idx;
    }

    // The impl-method half of eager checking: every method of a generic type's 'impl' block,
    // checked once with the type's own parameters unbound.
    //
    // The receiver is a REAL instantiation of the type with unbound arguments — a template
    // slot (see is_opaque_template_instance) — not a bare Opaque. That distinction is what
    // makes this pass worth having: a generic container's fields are usually only partly
    // parameter-dependent ('capacity: usize', 'hash: Hash_Fn[K]' returning a concrete 'u64'),
    // so with a real receiver 'self.hash(key) & mask' is checkable as 'u64 & usize' at the
    // DECLARATION. With a bare Opaque receiver every field read was opaque and nothing
    // reached through 'self' could be verified at all.
    //
    // A real receiver also means the ordinary instantiate_generic_method path applies: it
    // reads the arguments back off the receiver, resolves each signature, and — being
    // suppressed for unbound arguments — skips caching and emission, leaving the body for
    // this pass to drive.
    // Checks one just-instantiated method of a generic type against the trait it implements,
    // if it implements one.
    //
    // resolve_trait_impl_signatures_for_program does this for every non-generic target, but
    // cannot for a generic one: comparing 'fn draw(self) -> T' against the trait's
    // 'fn draw(self) -> i32' needs T bound to something. The template instance built by the
    // eager pass is exactly that binding, so the comparison happens here instead — same rule,
    // later moment. The structural halves (missing method, extra method, defaults) stay in
    // that pass, since they need no types at all.
    void check_generic_trait_impl_conformance(const std::string &module_path, const std::string &type_name,
                                               const std::string &method_name, const size_t instance_idx,
                                               Program &program, DiagnosticEngine &diag) {
        const auto trait_impls_it = program.trait_impls_by_type.find({module_path, type_name});
        if (trait_impls_it == program.trait_impls_by_type.end()) return;

        for (const auto &impl_info : trait_impls_it->second) {
            const auto method_it = impl_info.methods.find(method_name);
            if (method_it == impl_info.methods.end()) continue;

            const auto *trait_info = program.trait_at(impl_info.trait_index);
            if (!trait_info) return;
            const auto trait_method = std::ranges::find(trait_info->methods, method_name, &TraitMethodInfo::name);
            if (trait_method == trait_info->methods.end()) return; // "extra method" — already reported

            // The instance carries the substituted signature; the MethodInfo itself is still
            // unresolved for a generic target and would compare as empty.
            const auto &instance = *program.generic_fn_instances[instance_idx];
            MethodInfo substituted = method_it->second;
            substituted.param_types = instance.param_types;
            substituted.return_types = instance.return_types;
            substituted.is_variadic = instance.is_variadic;

            report_trait_conformance_mismatch(substituted, *trait_method, impl_info.trait_name, program, diag);
            return;
        }
    }

    void check_generic_type_method_bodies(const std::string &module_path, const std::string &type_name,
                                           Program &program, DiagnosticEngine &diag) {
        const auto mod_it = program.modules.find(module_path);
        if (mod_it == program.modules.end()) return;
        const auto sym_it = mod_it->second.symbols.find(type_name);
        if (sym_it == mod_it->second.symbols.end()) return;
        const auto *ts = std::get_if<TypeSymbol>(&sym_it->second);
        if (!ts || !ts->decl) return;

        // Both method tables for this type. A bare 'impl TYPE' block hangs off the module;
        // 'impl TRAIT for TYPE' blocks off the Program-level registry. Trait-impl methods used
        // to be skipped entirely here, which meant a generic trait impl had no eagerly-checked
        // instance at all — and, since resolve_trait_impl_signatures_for_program cannot resolve
        // a generic target's signatures either, no checking of ANY kind.
        const auto bare_methods_it = mod_it->second.methods.find(type_name);
        const auto *bare_methods = bare_methods_it != mod_it->second.methods.end() ? &bare_methods_it->second : nullptr;
        const auto trait_impls_it = program.trait_impls_by_type.find({module_path, type_name});
        const auto *trait_impls = trait_impls_it != program.trait_impls_by_type.end() ? &trait_impls_it->second : nullptr;
        if (!bare_methods && !trait_impls) return;

        // Names collected first, and sorted: checking one body can declare a module on demand
        // and rehash Program::modules, invalidating the iterators above, and both tables are
        // unordered so diagnostic order would otherwise vary between runs.
        std::vector<std::string> method_names;
        if (bare_methods) {
            for (const auto &mname : *bare_methods | std::views::keys) method_names.push_back(mname);
        }
        if (trait_impls) {
            for (const auto &impl_info : *trait_impls) {
                for (const auto &mname : impl_info.methods | std::views::keys) method_names.push_back(mname);
            }
        }
        std::ranges::sort(method_names);
        method_names.erase(std::ranges::unique(method_names).begin(), method_names.end());

        // The impl block's own parameter list, not the type declaration's: the two must match
        // in arity, but the impl chooses the names its method bodies actually use.
        const std::vector<ast::GenericParam> *impl_params = &ts->decl->generic_params;
        if (bare_methods) {
            for (const auto &[mname, info] : *bare_methods) {
                if (info.impl_generic_params) { impl_params = info.impl_generic_params; break; }
            }
        }

        auto args = opaque_args_for(*impl_params, program, module_path, diag);
        const auto receiver = instantiate_generic_type(program, diag, module_path, type_name, args, ts->decl->location);
        if (receiver.kind == TypeKind::Invalid || receiver.kind == TypeKind::Opaque) return;

        for (const auto &method_name : method_names) {
            const auto idx = instantiate_generic_method(program, diag, receiver, method_name, ts->decl->location);
            if (!idx) continue;

            // Recorded before the body is checked, so it is the instance this pass drives
            // that gets registered — see Program::template_method_instance_for_decl for why
            // this cannot be recovered by scanning afterwards.
            if (const auto *impl_decl = program.generic_fn_instances[*idx]->impl_decl) {
                program.template_method_instance_for_decl[impl_decl] = *idx;
            }
            check_generic_function_instance_body(*idx, program, diag);

            // Trait conformance for a generic target, which resolve_trait_impl_signatures_for_program
            // had to defer: it needs resolved param/return types, and those only exist once the
            // type's parameters are bound — which the instance above has just done.
            check_generic_trait_impl_conformance(module_path, type_name, method_name, *idx, program, diag);
        }
    }

    // Type-checks every generic function's body ONCE at its own declaration, with its
    // parameters bound to Opaque, so a generic that is never instantiated — or not yet
    // instantiated at the argument set that would expose a mistake — still gets checked.
    //
    // What this catches is everything INDEPENDENT of the parameters: unknown identifiers and
    // calls, wrong arity, fields on concrete types, mismatches between concrete types, missing
    // returns, 'break' outside a loop. What it deliberately does not catch is anything that
    // depends on an unconstrained parameter — 'a + b' for arbitrary 'T' is not checkable, and
    // reporting it would flag correct code. A trait BOUND turns that around: it states what T
    // offers, so the parameter becomes strict (see opaque_binary_op_result).
    //
    // Reuses instantiate_generic_function rather than re-deriving the signature: called with
    // Opaque arguments it resolves param/return types (so the body sees correctly-typed
    // locals) and then, being suppressed, skips caching, emission-marking and body-checking —
    // leaving the body for this pass to drive explicitly.
    void check_generic_templates_for_program(Program &program, DiagnosticEngine &diag) {
        if (!program.options.eager_generic_check) return;

        // Collected up front, and sorted: instantiating can declare a module on demand and
        // rehash Program::modules mid-iteration, and 'modules'/'symbols' are unordered, which
        // would otherwise make diagnostic ORDER vary between runs.
        std::vector<std::pair<std::string, std::string>> targets;
        // Generic TYPE declarations have no body to check, but their parameter bounds still
        // need validating — and this is the only pass that visits each declaration exactly
        // once, which is what keeps the diagnostic from repeating per instantiation.
        std::vector<std::pair<std::string, std::string>> type_targets;
        for (const auto &[path, module] : program.modules) {
            for (const auto &[name, sym] : module.symbols) {
                // A bare-import alias shares the origin's decl; the origin's own entry checks
                // it once, under the right module. Mirrors check_bodies_for_module.
                if (module.bare_import_origins.contains(name)) continue;
                if (const auto *ts = std::get_if<TypeSymbol>(&sym); ts && ts->decl && !ts->decl->generic_params.empty()) {
                    type_targets.emplace_back(path, name);
                    continue;
                }
                const auto *fn = std::get_if<FunctionSymbol>(&sym);
                if (!fn || !fn->decl || fn->decl->generic_params.empty()) continue;
                targets.emplace_back(path, name);
            }
        }
        std::ranges::sort(targets);
        std::ranges::sort(type_targets);

        for (const auto &[path, name] : type_targets) {
            const auto mod_it = program.modules.find(path);
            if (mod_it == program.modules.end()) continue;
            const auto sym_it = mod_it->second.symbols.find(name);
            if (sym_it == mod_it->second.symbols.end()) continue;
            const auto *ts = std::get_if<TypeSymbol>(&sym_it->second);
            if (!ts || !ts->decl) continue;
            for (const auto &param : ts->decl->generic_params) {
                generic_param_bound_trait_index(param, path, program, diag, /*report=*/true);
            }
            check_generic_type_method_bodies(path, name, program, diag);
        }

        const ScopedTemplateCheck template_check(program);
        for (const auto &[path, name] : targets) {
            // Re-looked-up rather than captured: the loop body can insert into
            // Program::modules and invalidate any held reference.
            const auto mod_it = program.modules.find(path);
            if (mod_it == program.modules.end()) continue;
            const auto sym_it = mod_it->second.symbols.find(name);
            if (sym_it == mod_it->second.symbols.end()) continue;
            const auto *fn = std::get_if<FunctionSymbol>(&sym_it->second);
            if (!fn || !fn->decl) continue;
            const auto &decl = *fn->decl;

            auto args = opaque_args_for(decl.generic_params, program, path, diag, /*report_bad_bounds=*/true);
            const size_t idx = instantiate_generic_function(program, diag, path, name, std::move(args), decl.location);
            // Recorded before the body is checked, so it is THIS instance — the one whose body
            // this pass is about to fill in — that gets registered, not whichever suppressed
            // instance a nested 'g[T](...)' elsewhere may have created for the same decl. See
            // Program::template_fn_instance_for_decl.
            program.template_fn_instance_for_decl[&decl] = idx;
            check_generic_function_instance_body(idx, program, diag);
        }
    }

    // Resolves an explicit generic_args list ('make_list[i32]', 'Fixed[u8, 16]') against
    // 'params' in declared order — arity + per-arg kind checking, matching the exact
    // diagnostic style used for $option/array-size compile-time-constant checks elsewhere.
    auto resolve_explicit_generic_args(const std::vector<ast::GenericParam> &params,
                                        const std::vector<ast::GenericArg> &args, const std::string &module_path,
                                        Program &program, DiagnosticEngine &diag, const SourceLocation &loc,
                                        const std::string &decl_name) -> std::optional<std::vector<GenericArgValue>>;

    // Recognizes 'Name[args]' / 'mod.Name[args]' where 'Name' is a generic TYPE declaration,
    // and returns its monomorphized ResolvedType. Returns nullopt when the operand does not
    // name a generic type declaration, so callers can fall through to their ordinary handling.
    //
    // size_of/align_of/type_of accept a type name in operand position, but their fast paths
    // only recognized IdentExpr and MemberExpr. 'size_of(List[i32])' parses as an
    // IndexOrInstantiateExpr, missed both, and fell through to plain check_expr -- which
    // rejected it with "generic-argument instantiation is not yet supported here", a stale
    // pre-generics gate.
    auto try_resolve_generic_type_instantiation(const ast::IndexOrInstantiateExpr &expr, const std::string &module_path,
                                                 Program &program, DiagnosticEngine &diag) -> std::optional<ResolvedType> {
        // Find the declaration the operand names, in this module or through a namespace chain.
        std::string decl_module = module_path;
        const std::string *decl_name = nullptr;

        if (const auto *ident = std::get_if<ast::IdentExpr>(&expr.operand)) {
            decl_name = &ident->name;
        } else if (const auto *member = std::get_if<std::unique_ptr<ast::MemberExpr>>(&expr.operand)) {
            LocalScope no_locals;
            if (const auto target = try_resolve_namespace_chain((*member)->object, module_path, no_locals, program)) {
                decl_module = *target;
                decl_name = &(*member)->member;
            }
        }
        if (!decl_name) return std::nullopt;

        const auto mod_it = program.modules.find(decl_module);
        if (mod_it == program.modules.end()) return std::nullopt;
        const auto sym_it = mod_it->second.symbols.find(*decl_name);
        if (sym_it == mod_it->second.symbols.end()) return std::nullopt;
        const auto *ts = std::get_if<TypeSymbol>(&sym_it->second);
        if (!ts || !ts->decl || ts->decl->generic_params.empty()) return std::nullopt;

        auto args = resolve_explicit_generic_args(ts->decl->generic_params, expr.args, module_path, program, diag,
                                                  expr.location, *decl_name);
        if (!args) return ResolvedType{.kind = TypeKind::Invalid};

        return instantiate_generic_type(program, diag, decl_module, *decl_name, std::move(*args), expr.location);
    }

    // The generic-FUNCTION sibling of try_resolve_generic_type_instantiation above: classifies
    // 'fnv1a[i32]' / 'fnv1a[K]' appearing in VALUE position (not as a call callee) as naming a
    // concrete monomorphized instantiation, and yields that instantiation's function-pointer
    // type. Returns nullopt when the operand doesn't name a generic function declaration, so
    // callers fall through to ordinary indexing.
    //
    // Generic arguments must be written explicitly here. infer_generic_function_args only
    // unifies against call arguments and a single return type, so there is nothing to infer
    // from in value position — 'fnv1a' alone is rejected by check_expr's IdentExpr arm with a
    // "supply its generic arguments" hint instead.
    //
    // 'expr_key' must be get_expr_key() of the ENCLOSING ast::Expr, matching what codegen
    // looks up in ProgramModule::expr_generic_fn_instance / expr_types.
    auto try_resolve_generic_fn_instantiation_value(const ast::IndexOrInstantiateExpr &expr, const void *expr_key,
                                                     LocalScope &locals, const std::string &module_path,
                                                     Program &program, DiagnosticEngine &diag) -> std::optional<ResolvedType> {
        std::string decl_module = module_path;
        const std::string *decl_name = nullptr;
        bool cross_module = false;

        if (const auto *ident = std::get_if<ast::IdentExpr>(&expr.operand)) {
            // A local of the same name shadows the declaration, in which case this really is
            // an index into that local — same precedence the call-callee path applies.
            if (locals.contains(ident->name)) return std::nullopt;
            decl_name = &ident->name;
        } else if (const auto *member = std::get_if<std::unique_ptr<ast::MemberExpr>>(&expr.operand)) {
            if (const auto target = try_resolve_namespace_chain((*member)->object, module_path, locals, program)) {
                decl_module = *target;
                decl_name = &(*member)->member;
                cross_module = true;
            }
        }
        if (!decl_name) return std::nullopt;

        const auto mod_it = program.modules.find(decl_module);
        if (mod_it == program.modules.end()) return std::nullopt;
        const auto sym_it = mod_it->second.symbols.find(*decl_name);
        if (sym_it == mod_it->second.symbols.end()) return std::nullopt;
        const auto *fs = std::get_if<FunctionSymbol>(&sym_it->second);
        if (!fs || !is_generic_function(*fs)) return std::nullopt;

        if (cross_module && !fs->is_pub) {
            return error(diag, expr.location, std::format("'{}' is not pub", *decl_name));
        }

        auto args = resolve_explicit_generic_args(fs->decl->generic_params, expr.args, module_path, program, diag,
                                                  expr.location, *decl_name);
        if (!args) return ResolvedType{.kind = TypeKind::Invalid};

        const size_t idx = instantiate_generic_function(program, diag, decl_module, *decl_name, std::move(*args), expr.location);
        const auto &instance = *program.generic_fn_instances[idx];
        if (instance.is_variadic) {
            return error(diag, expr.location, std::format(
                "cannot take the address of variadic function '{}'; function pointers to variadic functions are not supported", *decl_name));
        }
        if (instance.decl && find_attribute(instance.decl->attributes, "always_inline")) {
            diag.warn(DiagnosticStage::Sema, expr.location, std::format(
                "taking the address of '@always_inline' function '{}'. Calls through a "
                "function pointer cannot be inlined. The function will still exist as a "
                "symbol but the '@always_inline' attribute has no effect on indirect calls.",
                *decl_name));
        }
        // Recorded under the ENCLOSING expr's key so codegen can map this node straight to
        // 'instance.mangled_name''s llvm::Function — the same side table generic CALLS use.
        expr_tables_for_write(program, module_path).expr_generic_fn_instance[expr_key] = idx;
        return generic_instance_function_pointer_type(instance, program);
    }

    // Shared by generic function and (indirectly, via the caller pre-checking arity)
    // explicit-instantiation call sites; type-position instantiation ('List[i32]' as a
    // NamedType) uses type_resolver.cpp's own, independent copy of this same logic
    // (Resolver::resolve_generic_named_type) since it additionally needs generic_env-aware
    // nested resolution that only Resolver provides.
    auto resolve_explicit_generic_args(const std::vector<ast::GenericParam> &params,
                                        const std::vector<ast::GenericArg> &args, const std::string &module_path,
                                        Program &program, DiagnosticEngine &diag, const SourceLocation &loc,
                                        const std::string &decl_name) -> std::optional<std::vector<GenericArgValue>> {
        if (args.size() != params.size()) {
            diag.report_error(DiagnosticStage::Sema, loc, std::format(
                "'{}' expects {} generic argument(s), got {}", decl_name, params.size(), args.size()));
            return std::nullopt;
        }
        std::vector<GenericArgValue> result;
        result.reserve(params.size());
        bool ok = true;
        for (size_t i = 0; i < params.size(); ++i) {
            const auto &param = params[i];
            const auto &arg = args[i];
            if (is_generic_type_param(param.type)) {
                const auto *type_arg = std::get_if<ast::Type>(&arg.value);
                // A bare 'T' generic arg always parses as an Expr, never a Type - see
                // reinterpret_expr_as_type_name's doc comment (sema.hpp). Covers e.g.
                // 'fn wrap[T: type]() { make_list[T]() }', forwarding an enclosing generic's
                // own type param into another generic call.
                std::optional<ast::Type> reinterpreted;
                if (!type_arg) {
                    if (const auto *expr_arg = std::get_if<ast::Expr>(&arg.value)) {
                        reinterpreted = reinterpret_expr_as_type_name(*expr_arg);
                        if (reinterpreted) type_arg = &*reinterpreted;
                    }
                }
                if (!type_arg) {
                    diag.report_error(DiagnosticStage::Sema, arg.location, std::format(
                        "generic argument {} for '{}' must be a type (parameter '{}: type')", i + 1, decl_name, param.name));
                    ok = false;
                    continue;
                }
                result.push_back(GenericArgValue{.is_type = true, .type_arg = resolve_type(*type_arg, module_path, program, diag)});
            } else {
                const auto *expr_arg = std::get_if<ast::Expr>(&arg.value);
                if (!expr_arg) {
                    diag.report_error(DiagnosticStage::Sema, arg.location, std::format(
                        "generic argument {} for '{}' must be a compile-time constant expression", i + 1, decl_name));
                    ok = false;
                    continue;
                }
                const auto scalar_ty = resolve_type(param.type, module_path, program, diag);
                const auto value = evaluate_integer_constant(*expr_arg, module_path, program, diag);
                if (!value) {
                    diag.report_error(DiagnosticStage::Sema, arg.location, std::format(
                        "generic argument {} for '{}' must be a compile-time constant expression of type '{}'",
                        i + 1, decl_name, describe_type(scalar_ty, program)));
                    ok = false;
                    continue;
                }
                result.push_back(GenericArgValue{.is_type = false, .value_arg = *value, .value_arg_scalar_type = scalar_ty});
            }
        }
        if (!ok) return std::nullopt;
        return result;
    }

    // Infers a generic function's args when called with no explicit generic_args at all
    // ('make_list()', not 'make_list[]()' or 'make_list[i32]()'). Two passes, per spec.md
    // §22 "Explicit vs. Inferred Instantiation": (1) unify each type param that appears
    // literally as a parameter's own bare ': T' against that call argument's checked type;
    // (2) for anything still unbound, try the call's own expected-type context — supports a
    // bare ': T' return type (binds T directly to 'expected'), and a '-> [N]ElemType' return
    // type whose size is a bare generic value-param reference (binds N from 'expected's
    // array count). This is a deliberately narrow v1 subset of "expected-type context",
    // covering exactly the shapes spec.md's own worked examples use — not a general
    // structural unifier.
    auto infer_generic_function_args(const ast::FunctionDecl &decl, const std::vector<ast::Expr> &call_args,
                                      const std::optional<ResolvedType> &expected, LocalScope &locals,
                                      const std::string &module_path, Program &program, DiagnosticEngine &diag,
                                      const SourceLocation &loc, const int loop_depth, const int defer_loop_base,
                                      const ResolvedType *fn_error_type) -> std::optional<std::vector<GenericArgValue>> {
        std::vector<std::optional<GenericArgValue>> bound(decl.generic_params.size());

        for (size_t i = 0; i < decl.params.size() && i < call_args.size(); ++i) {
            const auto &p = decl.params[i];
            if (!p.type) continue;
            const auto *named = std::get_if<ast::NamedType>(&*p.type);
            if (!named || named->member != nullptr || !named->generic_args.empty()) continue;
            for (size_t gi = 0; gi < decl.generic_params.size(); ++gi) {
                if (bound[gi] || decl.generic_params[gi].name != named->name || !is_generic_type_param(decl.generic_params[gi].type)) continue;
                const auto arg_ty = check_expr(call_args[i], locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_error_type);
                bound[gi] = GenericArgValue{.is_type = true, .type_arg = arg_ty};
            }
        }

        if (expected && decl.return_types.size() == 1) {
            const auto &rt = decl.return_types[0];
            if (const auto *named = std::get_if<ast::NamedType>(&rt); named && named->member == nullptr && named->generic_args.empty()) {
                for (size_t gi = 0; gi < decl.generic_params.size(); ++gi) {
                    if (!bound[gi] && decl.generic_params[gi].name == named->name && is_generic_type_param(decl.generic_params[gi].type)) {
                        bound[gi] = GenericArgValue{.is_type = true, .type_arg = *expected};
                    }
                }
            } else if (const auto *arr = std::get_if<std::unique_ptr<ast::ArrayType>>(&rt);
                       arr && *arr && (*arr)->size && expected->kind == TypeKind::Array) {
                if (const auto *size_ident = std::get_if<ast::IdentExpr>(&*(*arr)->size)) {
                    for (size_t gi = 0; gi < decl.generic_params.size(); ++gi) {
                        if (bound[gi] || decl.generic_params[gi].name != size_ident->name || is_generic_type_param(decl.generic_params[gi].type)) continue;
                        if (const auto *array_info = program.array_at(expected->array_index)) {
                            bound[gi] = GenericArgValue{
                                .is_type = false,
                                .value_arg = static_cast<int64_t>(array_info->count),
                                .value_arg_scalar_type = resolve_type(decl.generic_params[gi].type, module_path, program, diag),
                            };
                        }
                    }
                }
            } else if (const auto *wrapped = std::get_if<ast::NamedType>(&rt); wrapped && !wrapped->generic_args.empty()) {
                // A return type that's itself a generic instantiation forwarding the
                // enclosing decl's own params ('-> List[T]', 'fn make_list[T: type]()') -
                // unify each of its generic_args positionally against 'expected's own
                // GenericInstanceInfo::args (no verification that 'wrapped' and 'expected'
                // name the exact same decl - a real mismatch still surfaces as the ordinary
                // "type mismatch" diagnostic once the instantiated return type is checked
                // against 'expected' downstream, same as any other inference miss would).
                std::vector<GenericArgValue> expected_args;
                switch (expected->kind) {
                case TypeKind::Struct:
                    if (const auto *info = program.struct_at(expected->struct_index); info && info->generic_instance) expected_args = info->generic_instance->args;
                    break;
                case TypeKind::Enum:
                    if (const auto *info = program.enum_at(expected->enum_index); info && info->generic_instance) expected_args = info->generic_instance->args;
                    break;
                case TypeKind::Union:
                    if (const auto *info = program.union_at(expected->union_index); info && info->generic_instance) expected_args = info->generic_instance->args;
                    break;
                case TypeKind::Bitset:
                    if (const auto *info = program.bitset_at(expected->bitset_index); info && info->generic_instance) expected_args = info->generic_instance->args;
                    break;
                default:
                    break;
                }
                if (expected_args.size() == wrapped->generic_args.size()) {
                    for (size_t ai = 0; ai < wrapped->generic_args.size(); ++ai) {
                        std::string ref_name;
                        if (const auto *type_ref = std::get_if<ast::Type>(&wrapped->generic_args[ai]->value)) {
                            if (const auto *inner_named = std::get_if<ast::NamedType>(type_ref); inner_named && inner_named->member == nullptr && inner_named->generic_args.empty()) {
                                ref_name = inner_named->name;
                            }
                        } else if (const auto *expr_ref = std::get_if<ast::Expr>(&wrapped->generic_args[ai]->value)) {
                            if (const auto *ident_ref = std::get_if<ast::IdentExpr>(expr_ref)) {
                                ref_name = ident_ref->name;
                            }
                        }
                        if (ref_name.empty()) continue;
                        for (size_t gi = 0; gi < decl.generic_params.size(); ++gi) {
                            if (!bound[gi] && decl.generic_params[gi].name == ref_name) {
                                bound[gi] = expected_args[ai];
                            }
                        }
                    }
                }
            }
        }

        std::vector<GenericArgValue> result;
        result.reserve(bound.size());
        for (size_t gi = 0; gi < bound.size(); ++gi) {
            if (!bound[gi]) {
                diag.report_error(DiagnosticStage::Sema, loc, std::format(
                    "could not infer generic parameter '{}' for '{}' — provide it explicitly ('{}[...]()') or use it "
                    "in a context with a known expected type",
                    decl.generic_params[gi].name, decl.name, decl.name));
                return std::nullopt;
            }
            result.push_back(*bound[gi]);
        }
        return result;
    }

    // The shared front half of 'match' and 'switch': resolve the operand, transparently
    // unwrap an error union, validate '_' arm placement, and reject operand kinds neither
    // construct can dispatch on.
    //
    // Returns the EFFECTIVE type the arms dispatch on — for an error union that is the INNER
    // representation (the member type directly for a single error member, or the synthesized
    // inner dispatch union for 2+), never the outer Ok/Failed wrapper, which has no
    // user-visible fields. nullopt means the construct was rejected and the caller should
    // stop.
    //
    // Templated on the arm type only because MatchExpr::Arm and SwitchStmt::Arm differ in
    // what they carry as a body; this half touches only '.pattern' and '.location'.
    //
    // 'switch' previously carried its own copy of all of this, and had drifted: its
    // unknown-error-state diagnostic was a single flat sentence where 'match' names which of
    // four causes actually applies. Sharing gives 'switch' the better message.
    template <typename ArmT>
    auto check_match_operand(const ast::Expr &operand_expr, const std::vector<ArmT> &arms, const std::string_view noun,
                              const SourceLocation &location, LocalScope &locals, const std::string &module_path,
                              Program &program, DiagnosticEngine &diag, const int loop_depth, const int defer_loop_base,
                              const ResolvedType *fn_error_type, std::optional<size_t> &out_default_arm_idx)
        -> std::optional<ResolvedType> {
        auto operand_type = check_expr(operand_expr, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_error_type);

        // Transparent error-value matching: 'match err {...}' requires 'err' (a plain
        // identifier) to be known Failed here, then dispatches the arms on the inner
        // representation.
        if (is_error_union_type(operand_type, program)) {
            const auto *ident = std::get_if<ast::IdentExpr>(&operand_expr);
            const auto *binding = ident ? find_error_local(ident->name, locals, program) : nullptr;
            if (!binding || binding->err_state != ErrorState::Failed) {
                // Four quite different situations reach this point. The generic "check it
                // first" advice is right only for the first — for the others it describes a
                // check the user has already written, which is why this used to be such a
                // confusing diagnostic. Name the actual cause instead where it is known.
                const auto generic_advice = std::format(
                    "check it first: 'if err {{ {} err {{ ... }} }}', "
                    "or use an early return: 'if !err {{ return_ok ... }}'", noun);
                const auto reason = binding ? binding->err_unknown_reason : ErrorUnknownReason::Unchecked;

                std::string detail;
                switch (reason) {
                case ErrorUnknownReason::Unchecked:
                    detail = generic_advice;
                    break;
                case ErrorUnknownReason::UncheckedConditionShape:
                    detail = std::format(
                        "the enclosing condition mentions it but does not narrow it HERE -- "
                        "'err', '!err' and an operand of a '&&' chain narrow the then-branch, "
                        "while an operand of a '||' chain narrows only the ELSE-branch; {}",
                        generic_advice);
                    break;
                case ErrorUnknownReason::AddressTaken:
                    detail = "its address was taken, so its state is no longer known here; "
                             "re-check it after the call";
                    break;
                case ErrorUnknownReason::BranchesDisagreed:
                    detail = "the preceding branches left it in different states, so nothing "
                             "is known about it here; re-check it";
                    break;
                }
                error(diag, location, std::format(
                    "cannot {} on an error value of unknown state; {}", noun, detail));
                return std::nullopt;
            }
            const auto &wrapper = *program.union_at(operand_type.union_index);
            const auto &failed_variant = wrapper.variants[1];
            const auto effective_type = wrapper.error_member_types.size() == 1
                ? wrapper.error_member_types[0]
                : failed_variant.payload_type;
            expr_tables_for_write(program, module_path).expr_error_match_unwrap[get_expr_key(operand_expr)] = ErrorMatchUnwrap{
                .wrapper_type = operand_type,
                .effective_type = effective_type,
            };
            operand_type = effective_type;
        }

        // '_' must appear at most once, and last.
        for (size_t i = 0; i < arms.size(); ++i) {
            if (!std::holds_alternative<ast::MatchExpr::DefaultPattern>(arms[i].pattern)) continue;
            if (out_default_arm_idx.has_value()) {
                // Keep the FIRST '_' as the recorded default: later diagnostics
                // ("unreachable default arm") must point at it, not at the duplicate
                // just reported here.
                error(diag, arms[i].location, "duplicate default arm '_'");
                continue;
            }
            if (i + 1 != arms.size()) {
                error(diag, arms[i].location, "default arm '_' must be the last arm");
            }
            out_default_arm_idx = i;
        }

        if (operand_type.is_float()) {
            error(diag, location, std::format("cannot {} on floating-point types; use if/else chains", noun));
            return std::nullopt;
        }
        if (operand_type.kind == TypeKind::Pointer || operand_type.kind == TypeKind::Anyptr) {
            error(diag, location, std::format("cannot {} on pointer types", noun));
            return std::nullopt;
        }

        return operand_type;
    }
    auto check_match_expr(const ast::Expr &expr, const std::unique_ptr<ast::MatchExpr> &v, LocalScope &locals, const std::string &module_path, Program &program, DiagnosticEngine &diag, const std::optional<ResolvedType> expected, const int loop_depth, const int defer_loop_base, const ResolvedType *fn_error_type) -> ResolvedType {
        std::optional<size_t> default_arm_idx;
        const auto effective_operand = check_match_operand(
            v->operand, v->arms, "match", v->location, locals, module_path, program, diag,
            loop_depth, defer_loop_base, fn_error_type, default_arm_idx);
        if (!effective_operand) return ResolvedType{.kind = TypeKind::Invalid};
        const auto operand_type = *effective_operand;

        // Eager template pass: the operand's type depends on unbound generic parameters
        // ('match self.tag'), so pattern validation and exhaustiveness wait for the
        // instantiation — the deliberate Opaque posture indexing/for-in/try already take.
        // Only the arm values are checked, with any payload capture bound as Opaque so a
        // valid template body doesn't report unknown identifiers.
        if (operand_type.kind == TypeKind::Opaque) {
            for (const auto &arm : v->arms) {
                auto arm_locals = locals;
                if (const auto *vp = std::get_if<ast::MatchExpr::VariantPattern>(&arm.pattern); vp && vp->capture_name) {
                    arm_locals[*vp->capture_name] = LocalBinding{.type = ResolvedType{.kind = TypeKind::Opaque}, .is_mut = false};
                }
                check_expr(arm.value, arm_locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_error_type);
            }
            return ResolvedType{.kind = TypeKind::Opaque};
        }

        // ---- Scalar match (integer or bool operand) ----
        if (operand_type.is_integer() || operand_type.kind == TypeKind::Bool) {
            std::unordered_map<int64_t, size_t> seen_values; // evaluated value -> arm index
            ResolvedType arm_type{.kind = TypeKind::Invalid};
            bool first_arm = true;
            bool true_covered = false, false_covered = false;

            for (size_t arm_i = 0; arm_i < v->arms.size(); ++arm_i) {
                const auto &arm = v->arms[arm_i];
                const auto &arm_loc = arm.location;

                if (std::holds_alternative<ast::MatchExpr::VariantPattern>(arm.pattern)) {
                    error(diag, arm_loc, "'.name' patterns require an enum or tagged union operand");
                    continue;
                }

                auto arm_locals = locals;

                if (std::holds_alternative<ast::MatchExpr::DefaultPattern>(arm.pattern)) {
                    // Default arm value
                    const auto val_type = check_expr(arm.value, arm_locals, module_path, program, diag,
                        arm_type.kind != TypeKind::Invalid ? std::optional<ResolvedType>{arm_type} : expected, loop_depth, defer_loop_base, fn_error_type);
                    if (first_arm) { arm_type = val_type; first_arm = false; }
                    else if (arm_type.kind != TypeKind::Invalid && val_type != arm_type) {
                        error(diag, arm_loc, "all match arms must have the same type");
                    }
                    continue;
                }

                const auto &lp = std::get<ast::MatchExpr::LiteralPattern>(arm.pattern);
                // Pattern must be compile-time constant
                const auto pattern_is_constant = is_constant_expr(*lp.expr, module_path, program);
                if (!pattern_is_constant) {
                    error(diag, arm_loc, "match arm pattern must be a compile-time constant");
                }
                // Type-check the pattern against the operand type. A plain literal
                // adapts to 'operand_type' via the expected-type coercion (and is
                // range-checked by it); a non-literal constant keeps its own declared
                // type and was never validated at all, so a u64 constant of 300 used as
                // a pattern in a match on u8 reached codegen, where the LLVM case value
                // silently truncates. The mismatched-type pattern's VALUE must fit the
                // operand type; the types themselves may differ ('size_of(i64)' — a
                // usize — is an accepted pattern for an i64 operand, see
                // example_match_const_expr_pattern).
                const auto pattern_ty = check_expr(*lp.expr, arm_locals, module_path, program, diag, operand_type, loop_depth, defer_loop_base, fn_error_type);
                // Evaluate for duplicate detection
                const auto val = evaluate_integer_constant(*lp.expr, module_path, program, diag);
                if (!val && pattern_is_constant) {
                    // Constant in shape but not evaluatable: a division that overflows
                    // (INT64_MIN / -1), a shift of 64 or more, or a constant that
                    // isn't an integer at all (a string). Reported here because
                    // codegen needs a concrete value for the LLVM switch case and has
                    // nothing to fall back on. Floating-point operands are rejected
                    // earlier, so every pattern reaching here should otherwise fold.
                    //
                    // "an expression this position cannot evaluate" used to be a
                    // fourth cause: arm patterns went through a folder that knew
                    // fewer AST shapes than the one array lengths used, so
                    // 'size_of(i64)' failed here while working there. Both are one
                    // folder now (TYPE-11), so that cause is gone.
                    error(diag, arm_loc,
                        "match arm pattern could not be folded to an integer constant "
                        "(an overflowing division, a shift of 64 or more, or a "
                        "constant that is not an integer)");
                }
                if (val && operand_type.is_integer() && pattern_ty.is_integer() && pattern_ty != operand_type) {
                    const bool negative = *val < 0;
                    const auto magnitude = negative ? ~static_cast<uint64_t>(*val) + 1 : static_cast<uint64_t>(*val);
                    if (!integer_literal_fits(magnitude, negative, operand_type)) {
                        error(diag, arm_loc, std::format("match arm pattern of type '{}' has value {}, which does not fit the operand type '{}' (range {})",
                            describe_type(pattern_ty, program), *val, describe_type(operand_type, program), describe_integer_range(operand_type)));
                    }
                }
                if (val) {
                    if (seen_values.count(*val)) {
                        error(diag, arm_loc, std::format("duplicate match arm: value already covered by arm {}", seen_values.at(*val) + 1));
                    } else {
                        seen_values[*val] = arm_i;
                        if (operand_type.kind == TypeKind::Bool) {
                            if (*val == 0) false_covered = true;
                            else if (*val == 1) true_covered = true;
                        }
                    }
                }
                // Check arm result value
                const auto val_type = check_expr(arm.value, arm_locals, module_path, program, diag,
                    arm_type.kind != TypeKind::Invalid ? std::optional<ResolvedType>{arm_type} : expected, loop_depth, defer_loop_base, fn_error_type);
                if (first_arm) { arm_type = val_type; first_arm = false; }
                else if (arm_type.kind != TypeKind::Invalid && val_type != arm_type) {
                    error(diag, arm_loc, "all match arms must have the same type");
                }
            }

            // Exhaustiveness
            const bool has_default = default_arm_idx.has_value();
            if (operand_type.kind == TypeKind::Bool) {
                if (!has_default && !(true_covered && false_covered)) {
                    error(diag, v->location, "bool match must cover both 'true' and 'false', or have a default '_' arm");
                }
                if (has_default && true_covered && false_covered) {
                    error(diag, v->arms[*default_arm_idx].location, "unreachable default arm: bool match already covers both 'true' and 'false'");
                }
            } else {
                if (!has_default) {
                    error(diag, v->location, "non-bool scalar match requires a default '_' arm");
                }
            }

            return arm_type.kind == TypeKind::Invalid ? ResolvedType{.kind = TypeKind::Void} : arm_type;
        }

        // ---- Tagged union match ----
        if (operand_type.kind == TypeKind::Union) {
            const auto *union_info_ptr = program.union_at(operand_type.union_index);
            if (!union_info_ptr) {
                return error(diag, v->location, "internal error: invalid union index");
            }
            const auto &union_info = *union_info_ptr;
            if (!union_info.is_tagged) {
                return error(diag, v->location, "match operand must be an enum or tagged union type");
            }

            // Check if any arm uses by-ref capture; operand must be an lvalue in that case
            const bool any_ref_capture = std::ranges::any_of(v->arms, [](const auto &a) {
                const auto *vp = std::get_if<ast::MatchExpr::VariantPattern>(&a.pattern);
                return vp && vp->capture_by_ref;
            });
            if (any_ref_capture) {
                const auto lv = resolve_lvalue(v->operand, locals, module_path, program, diag, loop_depth, defer_loop_base, fn_error_type);
                if (lv.type.kind == TypeKind::Invalid) {
                    error(diag, v->location, "by-ref capture requires an lvalue match operand");
                }
            }

            ResolvedType arm_type{.kind = TypeKind::Invalid};
            bool first_arm = true;
            std::vector<bool> covered(union_info.variants.size(), false);

            for (const auto &arm : v->arms) {
                if (std::holds_alternative<ast::MatchExpr::DefaultPattern>(arm.pattern)) {
                    // Arm-local copy, as in the scalar path: an arm body's error-state
                    // narrowing must not leak into sibling arms.
                    auto arm_locals = locals;
                    const auto val_type = check_expr(arm.value, arm_locals, module_path, program, diag,
                        arm_type.kind != TypeKind::Invalid ? std::optional<ResolvedType>{arm_type} : expected, loop_depth, defer_loop_base, fn_error_type);
                    if (first_arm) { arm_type = val_type; first_arm = false; }
                    else if (arm_type.kind != TypeKind::Invalid && val_type != arm_type) {
                        error(diag, arm.location, "all match arms must have the same type");
                    }
                    continue;
                }
                if (!std::holds_alternative<ast::MatchExpr::VariantPattern>(arm.pattern)) {
                    error(diag, arm.location, "literal patterns require a scalar (integer/bool) operand");
                    continue;
                }
                const auto &vp = std::get<ast::MatchExpr::VariantPattern>(arm.pattern);

                bool found = false;
                for (size_t i = 0; i < union_info.variants.size(); ++i) {
                    if (union_info.variants[i].name == vp.name) {
                        if (covered[i]) {
                            error(diag, arm.location, std::format("duplicate match arm for variant '{}'", vp.name));
                        }
                        covered[i] = true;
                        found = true;

                        const auto &variant = union_info.variants[i];
                        auto arm_locals = locals;

                        if (vp.capture_name) {
                            if (variant.payload_struct_index < 0) {
                                error(diag, arm.location, std::format("variant '{}' has no payload; cannot capture", vp.name));
                            } else {
                                const ResolvedType payload_ty = variant.payload_type;
                                if (vp.capture_by_ref) {
                                    arm_locals[*vp.capture_name] = LocalBinding{
                                        .type = intern_pointer(program, payload_ty),
                                        .is_mut = false,
                                    };
                                    CaptureAliases aliases{*vp.capture_name};
                                    check_payload_capture_escape_expr(arm.value, aliases, diag);
                                } else {
                                    arm_locals[*vp.capture_name] = LocalBinding{.type = payload_ty, .is_mut = false};
                                }
                            }
                        }

                        const auto val_type = check_expr(arm.value, arm_locals, module_path, program, diag,
                                                         arm_type.kind != TypeKind::Invalid ? std::optional<ResolvedType>{arm_type} : expected, loop_depth, defer_loop_base, fn_error_type);
                        if (first_arm) {
                            arm_type = val_type;
                            first_arm = false;
                        } else if (arm_type.kind != TypeKind::Invalid && val_type != arm_type) {
                            error(diag, arm.location, "all match arms must have the same type");
                        }
                        break;
                    }
                }
                if (!found) {
                    error(diag, arm.location, std::format("no variant '{}' on {}", vp.name, union_info.is_error_union ? "error" : "tagged union"));
                    auto arm_locals = locals;
                    const auto val_type = check_expr(arm.value, arm_locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_error_type);
                    if (first_arm) { arm_type = val_type; first_arm = false; }
                }
            }

            const bool has_default = default_arm_idx.has_value();
            // Exhaustiveness: all variants must be covered, OR a default arm is present
            for (size_t i = 0; i < union_info.variants.size(); ++i) {
                if (!covered[i] && !has_default) {
                    error(diag, v->location, std::format("match is not exhaustive: missing arm for '{}'", union_info.variants[i].name));
                }
            }
            // Unreachable default: _ after all variants are covered
            if (has_default && std::ranges::all_of(covered, [](bool b) { return b; })) {
                error(diag, v->arms[*default_arm_idx].location, "unreachable default arm: all variants are already covered");
            }

            return arm_type.kind == TypeKind::Invalid ? ResolvedType{.kind = TypeKind::Void} : arm_type;
        }

        // ---- Enum match ----
        if (operand_type.kind != TypeKind::Enum) {
            return error(diag, v->location, "match operand must be an enum, tagged union, integer, or bool type");
        }

        const auto *enum_info_ptr = program.enum_at(operand_type.enum_index);
        if (!enum_info_ptr) {
            return error(diag, v->location, "internal error: invalid enum index");
        }
        const auto &enum_info = *enum_info_ptr;

        ResolvedType arm_type{.kind = TypeKind::Invalid};
        bool first_arm = true;
        std::vector<bool> covered(enum_info.fields.size(), false);

        for (const auto &arm : v->arms) {
            if (std::holds_alternative<ast::MatchExpr::DefaultPattern>(arm.pattern)) {
                auto arm_locals = locals;
                const auto val_type = check_expr(arm.value, arm_locals, module_path, program, diag,
                    arm_type.kind != TypeKind::Invalid ? std::optional<ResolvedType>{arm_type} : expected, loop_depth, defer_loop_base, fn_error_type);
                if (first_arm) { arm_type = val_type; first_arm = false; }
                else if (arm_type.kind != TypeKind::Invalid && val_type != arm_type) {
                    error(diag, arm.location, "all match arms must have the same type");
                }
                continue;
            }
            if (!std::holds_alternative<ast::MatchExpr::VariantPattern>(arm.pattern)) {
                error(diag, arm.location, "literal patterns require a scalar (integer/bool) operand");
                continue;
            }
            const auto &vp = std::get<ast::MatchExpr::VariantPattern>(arm.pattern);

            if (vp.capture_name) {
                error(diag, arm.location, "payload capture is only valid for tagged union match arms");
            }
            bool found = false;
            for (size_t i = 0; i < enum_info.fields.size(); ++i) {
                if (enum_info.fields[i].name == vp.name) {
                    if (covered[i]) {
                        error(diag, arm.location, std::format("duplicate match arm for enum field '{}'", vp.name));
                    }
                    covered[i] = true;
                    found = true;
                    break;
                }
            }
            if (!found) {
                error(diag, arm.location, std::format("no enum field named '{}'", vp.name));
            }

            auto arm_locals = locals;
            const auto val_type = check_expr(arm.value, arm_locals, module_path, program, diag,
                arm_type.kind != TypeKind::Invalid ? std::optional<ResolvedType>{arm_type} : expected, loop_depth, defer_loop_base, fn_error_type);
            if (first_arm) { arm_type = val_type; first_arm = false; }
            else if (arm_type.kind != TypeKind::Invalid && val_type != arm_type) {
                error(diag, arm.location, "all match arms must have the same type");
            }
        }

        const bool has_default = default_arm_idx.has_value();
        for (size_t i = 0; i < enum_info.fields.size(); ++i) {
            if (!covered[i] && !has_default) {
                error(diag, v->location, std::format("match is not exhaustive: missing arm for '{}'", enum_info.fields[i].name));
            }
        }
        if (has_default && std::ranges::all_of(covered, [](bool b) { return b; })) {
            error(diag, v->arms[*default_arm_idx].location, "unreachable default arm: all enum fields are already covered");
        }

        return arm_type.kind == TypeKind::Invalid ? ResolvedType{.kind = TypeKind::Void} : arm_type;

    }

    void check_switch_stmt(const std::unique_ptr<ast::SwitchStmt> &v, LocalScope &locals, const std::string &module_path, Program &program, DiagnosticEngine &diag, const std::vector<ResolvedType> &expected_returns, const int loop_depth, const int defer_loop_base, const ResolvedType *fn_error_type) {
        // Exhaustiveness note: unlike 'match', 'switch' does NOT require an enum or
        // tagged-union operand to cover every variant, and a switch with no '_' arm
        // and a missing variant compiles silently. That is deliberate and follows
        // from what the two constructs are: 'match' is an EXPRESSION and must
        // produce a value on every path, so a gap has no answer; 'switch' is a
        // STATEMENT, and falling through all arms is simply doing nothing. The same
        // reasoning is already stated in-code for the bool case below.
        //
        // Called out here because the asymmetry with match's headline exhaustiveness
        // checking is surprising enough to look like an oversight.
        std::optional<size_t> default_arm_idx;
        const auto effective_operand = check_match_operand(
            v->operand, v->arms, "switch", v->location, locals, module_path, program, diag,
            loop_depth, defer_loop_base, fn_error_type, default_arm_idx);
        if (!effective_operand) return;
        const auto operand_type = *effective_operand;

        // Eager template pass — mirrors check_match_expr's Opaque posture: check only the
        // arm bodies, with captures bound Opaque; patterns wait for the instantiation.
        if (operand_type.kind == TypeKind::Opaque) {
            for (const auto &arm : v->arms) {
                auto arm_locals = locals;
                if (const auto *vp = std::get_if<ast::MatchExpr::VariantPattern>(&arm.pattern); vp && vp->capture_name) {
                    arm_locals[*vp->capture_name] = LocalBinding{.type = ResolvedType{.kind = TypeKind::Opaque}, .is_mut = false};
                }
                check_stmt(arm.body, arm_locals, module_path, program, diag, expected_returns, loop_depth, defer_loop_base);
            }
            return;
        }

        // Scalar switch (integer or bool)
        if (operand_type.is_integer() || operand_type.kind == TypeKind::Bool) {
            std::unordered_map<int64_t, size_t> seen_values;
            bool true_covered = false, false_covered = false;

            for (size_t arm_i = 0; arm_i < v->arms.size(); ++arm_i) {
                const auto &arm = v->arms[arm_i];

                if (std::holds_alternative<ast::MatchExpr::VariantPattern>(arm.pattern)) {
                    diag.report_error(DiagnosticStage::Sema, arm.location, "'.name' patterns require an enum or tagged union operand");
                    continue;
                }
                if (std::holds_alternative<ast::MatchExpr::DefaultPattern>(arm.pattern)) {
                    auto arm_locals = locals;
                    check_stmt(arm.body, arm_locals, module_path, program, diag, expected_returns, loop_depth, defer_loop_base);
                    continue;
                }
                const auto &lp = std::get<ast::MatchExpr::LiteralPattern>(arm.pattern);
                const auto pattern_is_constant = is_constant_expr(*lp.expr, module_path, program);
                if (!pattern_is_constant) {
                    diag.report_error(DiagnosticStage::Sema, arm.location, "switch arm pattern must be a compile-time constant");
                }
                // Same value-fit validation as the match path above.
                const auto pattern_ty = check_expr(*lp.expr, locals, module_path, program, diag, operand_type, loop_depth, defer_loop_base, fn_error_type);
                const auto val = evaluate_integer_constant(*lp.expr, module_path, program, diag);
                if (val && operand_type.is_integer() && pattern_ty.is_integer() && pattern_ty != operand_type) {
                    const bool negative = *val < 0;
                    const auto magnitude = negative ? ~static_cast<uint64_t>(*val) + 1 : static_cast<uint64_t>(*val);
                    if (!integer_literal_fits(magnitude, negative, operand_type)) {
                        diag.report_error(DiagnosticStage::Sema, arm.location, std::format("switch arm pattern of type '{}' has value {}, which does not fit the operand type '{}' (range {})",
                            describe_type(pattern_ty, program), *val, describe_type(operand_type, program), describe_integer_range(operand_type)));
                    }
                }
                if (!val && pattern_is_constant) {
                    // See the matching comment in the match-expression path above.
                    diag.report_error(DiagnosticStage::Sema, arm.location,
                        "switch arm pattern could not be folded to an integer constant "
                        "(an overflowing division, a shift of 64 or more, or a "
                        "constant that is not an integer)");
                }
                if (val) {
                    if (seen_values.count(*val)) {
                        diag.report_error(DiagnosticStage::Sema, arm.location, std::format("duplicate switch arm: value already covered by arm {}", seen_values.at(*val) + 1));
                    } else {
                        seen_values[*val] = arm_i;
                        if (operand_type.kind == TypeKind::Bool) {
                            if (*val == 0) false_covered = true;
                            else if (*val == 1) true_covered = true;
                        }
                    }
                }
                auto arm_locals = locals;
                check_stmt(arm.body, arm_locals, module_path, program, diag, expected_returns, loop_depth, defer_loop_base);
            }

            // Check for unreachable '_' on bool (no exhaustiveness required for switch)
            if (default_arm_idx && operand_type.kind == TypeKind::Bool && true_covered && false_covered) {
                diag.report_error(DiagnosticStage::Sema, v->arms[*default_arm_idx].location,
                    "unreachable default arm: bool switch already covers both 'true' and 'false'");
            }
            return;
        }

        // Tagged union switch
        if (operand_type.kind == TypeKind::Union) {
            const auto *union_info_ptr = program.union_at(operand_type.union_index);
            if (!union_info_ptr) {
                diag.report_error(DiagnosticStage::Sema, v->location, "internal error: invalid union index");
                return;
            }
            const auto &union_info = *union_info_ptr;
            if (!union_info.is_tagged) {
                diag.report_error(DiagnosticStage::Sema, v->location, "switch operand must be an enum, tagged union, integer, or bool type");
                return;
            }
            const bool any_ref_capture = std::ranges::any_of(v->arms, [](const auto &a) {
                const auto *vp = std::get_if<ast::MatchExpr::VariantPattern>(&a.pattern);
                return vp && vp->capture_by_ref;
            });
            if (any_ref_capture) {
                const auto lv = resolve_lvalue(v->operand, locals, module_path, program, diag, loop_depth, defer_loop_base, fn_error_type);
                if (lv.type.kind == TypeKind::Invalid) {
                    diag.report_error(DiagnosticStage::Sema, v->location, "by-ref capture requires an lvalue switch operand");
                }
            }

            std::vector<bool> covered(union_info.variants.size(), false);
            for (const auto &arm : v->arms) {
                if (std::holds_alternative<ast::MatchExpr::DefaultPattern>(arm.pattern)) {
                    auto arm_locals = locals;
                    check_stmt(arm.body, arm_locals, module_path, program, diag, expected_returns, loop_depth, defer_loop_base);
                    continue;
                }
                if (!std::holds_alternative<ast::MatchExpr::VariantPattern>(arm.pattern)) {
                    diag.report_error(DiagnosticStage::Sema, arm.location, "literal patterns require a scalar (integer/bool) operand");
                    continue;
                }
                const auto &vp = std::get<ast::MatchExpr::VariantPattern>(arm.pattern);
                bool found = false;
                for (size_t i = 0; i < union_info.variants.size(); ++i) {
                    if (union_info.variants[i].name == vp.name) {
                        if (covered[i]) {
                            diag.report_error(DiagnosticStage::Sema, arm.location, std::format("duplicate switch arm for variant '{}'", vp.name));
                        }
                        covered[i] = true;
                        found = true;
                        const auto &variant = union_info.variants[i];
                        auto arm_locals = locals;
                        if (vp.capture_name) {
                            if (variant.payload_struct_index < 0) {
                                diag.report_error(DiagnosticStage::Sema, arm.location, std::format("variant '{}' has no payload; cannot capture", vp.name));
                            } else {
                                const ResolvedType payload_ty = variant.payload_type;
                                if (vp.capture_by_ref) {
                                    arm_locals[*vp.capture_name] = LocalBinding{
                                        .type = intern_pointer(program, payload_ty),
                                        .is_mut = false,
                                    };
                                    CaptureAliases aliases{*vp.capture_name};
                                    check_payload_capture_escape(arm.body, aliases, diag);
                                } else {
                                    arm_locals[*vp.capture_name] = LocalBinding{.type = payload_ty, .is_mut = false};
                                }
                            }
                        }
                        check_stmt(arm.body, arm_locals, module_path, program, diag, expected_returns, loop_depth, defer_loop_base);
                        break;
                    }
                }
                if (!found) {
                    diag.report_error(DiagnosticStage::Sema, arm.location,
                        std::format("no variant '{}' on {}", vp.name, union_info.is_error_union ? "error" : "tagged union"));
                }
            }
            if (default_arm_idx && std::ranges::all_of(covered, [](bool b) { return b; })) {
                diag.report_error(DiagnosticStage::Sema, v->arms[*default_arm_idx].location, "unreachable default arm: all variants are already covered");
            }
            return;
        }

        // Enum switch
        if (operand_type.kind != TypeKind::Enum) {
            diag.report_error(DiagnosticStage::Sema, v->location, "switch operand must be an enum, tagged union, integer, or bool type");
            return;
        }
        const auto *enum_info_ptr = program.enum_at(operand_type.enum_index);
        if (!enum_info_ptr) {
            diag.report_error(DiagnosticStage::Sema, v->location, "internal error: invalid enum index");
            return;
        }
        const auto &enum_info = *enum_info_ptr;
        std::vector<bool> covered(enum_info.fields.size(), false);
        for (const auto &arm : v->arms) {
            if (std::holds_alternative<ast::MatchExpr::DefaultPattern>(arm.pattern)) {
                auto arm_locals = locals;
                check_stmt(arm.body, arm_locals, module_path, program, diag, expected_returns, loop_depth, defer_loop_base);
                continue;
            }
            if (!std::holds_alternative<ast::MatchExpr::VariantPattern>(arm.pattern)) {
                diag.report_error(DiagnosticStage::Sema, arm.location, "literal patterns require a scalar (integer/bool) operand");
                continue;
            }
            const auto &vp = std::get<ast::MatchExpr::VariantPattern>(arm.pattern);
            if (vp.capture_name) {
                diag.report_error(DiagnosticStage::Sema, arm.location, "payload capture is only valid for tagged union arms");
            }
            bool found = false;
            for (size_t i = 0; i < enum_info.fields.size(); ++i) {
                if (enum_info.fields[i].name == vp.name) {
                    if (covered[i]) {
                        diag.report_error(DiagnosticStage::Sema, arm.location, std::format("duplicate switch arm for enum field '{}'", vp.name));
                    }
                    covered[i] = true;
                    found = true;
                    break;
                }
            }
            if (!found) {
                diag.report_error(DiagnosticStage::Sema, arm.location, std::format("no enum field named '{}'", vp.name));
            }
            auto arm_locals = locals;
            check_stmt(arm.body, arm_locals, module_path, program, diag, expected_returns, loop_depth, defer_loop_base);
        }
        if (default_arm_idx && std::ranges::all_of(covered, [](bool b) { return b; })) {
            diag.report_error(DiagnosticStage::Sema, v->arms[*default_arm_idx].location, "unreachable default arm: all enum fields are already covered");
        }

    }

    // ---- check_expr / check_stmt per-node helpers (CHECK-9) --------------------
    //
    // Bodies moved verbatim out of the two std::visit dispatchers, which had grown to
    // 1683 and 684 lines — an order of magnitude over this project's own ~150-line
    // guideline, and long enough that finding the arm you wanted meant scrolling past
    // a dozen others.
    //
    // Signatures are explicit rather than bundled into a context struct on purpose: it
    // keeps every body byte-identical to what it was inline, so this is a move, not a
    // rewrite. The parameter lists are long because the dispatchers' always were.

    auto check_braced_initializer_expr(const ast::Expr &expr, const std::unique_ptr<ast::BracedInitializerExpr> & v, LocalScope &locals, const std::string &module_path, Program &program, DiagnosticEngine &diag, const std::optional<ResolvedType> expected, const int loop_depth, const int defer_loop_base, const ResolvedType *fn_error_type) -> ResolvedType {
        // 'return { .a = p, .n = 1 }' where the return type is a generic
        // instantiation ('Tb[T, T]') has no concrete struct to match field names
        // and types against — that aggregate does not exist until 'T' does. Still
        // walk every element expression so mistakes INSIDE the initializer (an
        // unknown name, a bad call) are reported; just don't judge the shape.
        if (expected && expected->kind == TypeKind::Opaque) {
            check_braced_initializer_elements(*v, locals, module_path, program, diag,
                                               loop_depth, defer_loop_base, fn_error_type);
            return ResolvedType{.kind = TypeKind::Opaque};
        }
        return std::visit(
            [&]<typename BV>(const BV &bv) -> ResolvedType {
                using BVT = std::decay_t<BV>;

                if constexpr (std::is_same_v<BVT, ast::EmptyExpr>) {
                    if (!expected || (expected->kind != TypeKind::Struct && expected->kind != TypeKind::Array)) {
                        if (expected && expected->kind == TypeKind::Union) {
                            return error(diag, bv.location, "a union initializer must set exactly one member");
                        }
                        return error(diag, bv.location, "braced initializer '{}' requires a struct or array type");
                    }
                    return *expected;

                } else if constexpr (std::is_same_v<BVT, ast::StructExpr>) {
                    if (!expected) {
                        return error(diag, bv.location, "struct initializer requires an expected type");
                    }
                    if (expected->kind == TypeKind::Array) {
                        return error(diag, bv.location, "struct initializer used where array type is expected");
                    }
                    if (expected->kind == TypeKind::Union) {
                        const auto *union_info_ptr = program.union_at(expected->union_index);
                        if (!union_info_ptr) {
                            return error(diag, bv.location, "internal error: invalid union index");
                        }
                        const auto &union_info = *union_info_ptr;
                        if (bv.fields.size() != 1) {
                            return error(diag, bv.location, std::format("a union initializer must set exactly one member, got {}", bv.fields.size()));
                        }
                        const auto &sf = bv.fields[0];
                        const auto it = std::ranges::find(union_info.members, sf.name, &sema::UnionMember::name);
                        if (it == union_info.members.end()) {
                            error(diag, sf.location, std::format("no member '{}' on union", sf.name));
                            return *expected;
                        }
                        const auto val_ty = check_expr(sf.expr, locals, module_path, program, diag, it->type, loop_depth, defer_loop_base, fn_error_type);
                        if (!assignable_in_module(val_ty, it->type, module_path, program)) {
                            error(diag, sf.location, std::format("type mismatch for union member '{}'", sf.name));
                        }
                        return *expected;
                    }
                    if (expected->kind != TypeKind::Struct) {
                        return error(diag, bv.location, "struct initializer requires a struct type");
                    }
                    const auto *info_ptr = program.struct_at(expected->struct_index);
                    if (!info_ptr) {
                        return error(diag, bv.location, "internal error: invalid struct index");
                    }
                    const auto &info = *info_ptr;
                    // Check for unknown and duplicate field names
                    std::unordered_set<std::string> seen;
                    for (const auto &sf : bv.fields) {
                        if (!seen.insert(sf.name).second) {
                            error(diag, sf.location, std::format("duplicate field '{}' in struct initializer", sf.name));
                        }
                        const auto it = std::ranges::find(info.fields, sf.name, &sema::StructField::name);
                        if (it == info.fields.end()) {
                            error(diag, sf.location, std::format("no field '{}' on struct", sf.name));
                            continue;
                        }
                        const auto val_ty = check_expr(sf.expr, locals, module_path, program, diag, it->type, loop_depth, defer_loop_base, fn_error_type);
                        if (!assignable_in_module(val_ty, it->type, module_path, program)) {
                            error(diag, sf.location, std::format("type mismatch for field '{}'", sf.name));
                        }
                    }
                    // Check that all fields without a default are provided
                    for (const auto &f : info.fields) {
                        if (f.init_expr != nullptr) continue;
                        const bool provided = std::ranges::any_of(bv.fields, [&](const auto &sf) { return sf.name == f.name; });
                        if (!provided) {
                            error(diag, bv.location, std::format("missing field '{}' in struct initializer", f.name));
                        }
                    }
                    return *expected;

                } else if constexpr (std::is_same_v<BVT, ast::BitsetExpr>) {
                    if (!expected || expected->kind != TypeKind::Bitset) {
                        return error(diag, bv.location, "bitset initializer requires an expected bitset type");
                    }
                    const auto *bitset_info = program.bitset_at(expected->bitset_index);
                    if (!bitset_info) {
                        return error(diag, bv.location, "internal error: invalid bitset index");
                    }
                    const auto *enum_info = program.enum_at(bitset_info->member_enum_type.enum_index);
                    if (!enum_info) {
                        return error(diag, bv.location, "internal error: invalid bitset member enum index");
                    }
                    // Both diagnostics point at the offending flag rather than at
                    // the whole '{.A, .B}' literal, which is why Member carries a
                    // location.
                    std::unordered_set<std::string> seen;
                    for (const auto &member : bv.members) {
                        if (!seen.insert(member.name).second) {
                            error(diag, member.location, std::format("duplicate member '{}' in bitset initializer", member.name));
                            continue;
                        }
                        const auto it = std::ranges::find(enum_info->fields, member.name, &sema::EnumFieldInfo::name);
                        if (it == enum_info->fields.end()) {
                            error(diag, member.location, std::format("no bitset member named '{}'", member.name));
                        }
                    }
                    return *expected;

                } else { // ast::ArrayExpr
                    if (!expected) {
                        return error(diag, bv.location, "array initializer requires an expected type");
                    }
                    if (expected->kind == TypeKind::Struct) {
                        if (bv.has_fill) {
                            return error(diag, bv.location, "fill '...' is not allowed in a positional struct initializer");
                        }
                        const auto *info_ptr = program.struct_at(expected->struct_index);
                        if (!info_ptr) {
                            return error(diag, bv.location, "internal error: invalid struct index");
                        }
                        const auto &info = *info_ptr;
                        if (bv.values.size() > info.fields.size()) {
                            return error(diag, bv.location, std::format("too many values in struct initializer: struct has {} field(s), got {}", info.fields.size(), bv.values.size()));
                        }
                        for (size_t i = 0; i < bv.values.size(); ++i) {
                            const auto &field = info.fields[i];
                            const auto val_ty = check_expr(bv.values[i], locals, module_path, program, diag, field.type, loop_depth, defer_loop_base, fn_error_type);
                            if (!assignable_in_module(val_ty, field.type, module_path, program)) {
                                error(diag, bv.location, std::format("type mismatch for field '{}'", field.name));
                            }
                        }
                        for (size_t i = bv.values.size(); i < info.fields.size(); ++i) {
                            if (info.fields[i].init_expr == nullptr) {
                                error(diag, bv.location, std::format("missing field '{}' in struct initializer", info.fields[i].name));
                            }
                        }
                        return *expected;
                    }
                    if (expected->kind != TypeKind::Array) {
                        return error(diag, bv.location, "array initializer requires an array type");
                    }
                    const auto *array_info_ptr = program.array_at(expected->array_index);
                    if (!array_info_ptr) {
                        return error(diag, bv.location, "internal error: invalid array index");
                    }
                    const auto &array_info = *array_info_ptr;
                    if (bv.values.size() > array_info.count) {
                        return error(diag, bv.location, std::format("too many elements in array initializer: array has {} element(s), got {}", array_info.count, bv.values.size()));
                    }
                    for (const auto &val : bv.values) {
                        const auto val_ty = check_expr(val, locals, module_path, program, diag, array_info.element_type, loop_depth, defer_loop_base, fn_error_type);
                        if (!assignable_in_module(val_ty, array_info.element_type, module_path, program)) {
                            error(diag, bv.location, "type mismatch in array initializer element");
                        }
                    }
                    return *expected;
                }
            },
            *v);
    }

    auto check_ident_expr(const ast::Expr &expr, const ast::IdentExpr & v, LocalScope &locals, const std::string &module_path, Program &program, DiagnosticEngine &diag, const std::optional<ResolvedType> expected, const int loop_depth, const int defer_loop_base, const ResolvedType *fn_error_type) -> ResolvedType {
        if (auto it = locals.find(v.name); it != locals.end()) return it->second.type;

        const auto mod_it = program.modules.find(module_path);
        if (mod_it == program.modules.end()) {
            return error(diag, v.location, std::format("internal error: module '{}' not found", module_path));
        }

        auto sym_it = mod_it->second.symbols.find(v.name);
        if (sym_it == mod_it->second.symbols.end()) {
            return error(diag, v.location, std::format("unknown identifier '{}'", v.name));
        }

        return std::visit(
            [&]<typename T1>(const T1 &sym) -> ResolvedType {
                using S = std::decay_t<T1>;
                if constexpr (std::is_same_v<S, GlobalSymbol>) {
                    return resolve_global_symbol(module_path, v.name, program, diag, v.location);
                } else if constexpr (std::is_same_v<S, ImportSymbol>) {
                    return ResolvedType{.kind = TypeKind::Namespace};
                } else if constexpr (std::is_same_v<S, FunctionSymbol>) {
                    // A generic TEMPLATE has no signature to take the address of —
                    // only a concrete instantiation does. Reject here rather than
                    // letting it reach ensure_function_signature_resolved, which
                    // would leave 'params' empty and produce a confusing
                    // signature-mismatch error against the empty list.
                    if (is_generic_function(sym)) {
                        return error(diag, v.location, std::format(
                            "'{}' is a generic function; supply its generic arguments to name a "
                            "specific instantiation (e.g. '{}[i32]')", v.name, v.name));
                    }
                    auto &resolved_fn = ensure_function_signature_resolved(module_path, v.name, program, diag);
                    if (resolved_fn.is_variadic) {
                        return error(diag, v.location, std::format("cannot take the address of variadic function '{}'; function pointers to variadic functions are not supported", v.name));
                    }
                    if (resolved_fn.decl && find_attribute(resolved_fn.decl->attributes, "always_inline")) {
                        diag.warn(DiagnosticStage::Sema, v.location, std::format(
                            "taking the address of '@always_inline' function '{}'. Calls through a "
                            "function pointer cannot be inlined. The function will still exist as a "
                            "symbol but the '@always_inline' attribute has no effect on indirect calls.",
                            v.name));
                    }
                    // With a function-typed expectation, honour it exactly (so a
                    // signature mismatch names the expectation rather than yielding a
                    // second, vaguer error downstream).
                    if (expected && expected->kind == TypeKind::Function) {
                        const auto &exp_sig = fn_sig(*expected, program);
                        if (function_params_compatible(resolved_fn.params, exp_sig.param_types) &&
                            resolved_fn.return_types == exp_sig.return_types &&
                            !exp_sig.is_variadic) {
                            return *expected;
                        }
                        return error(diag, v.location, std::format("'{}' has a different signature from the expected function type", v.name));
                    }
                    // No expectation (or a non-function one): a bare function name in
                    // value position decays to its own function-pointer type, so
                    // 'const g := f' and a ':='-inferred default parameter value work
                    // without the type having to be written out. A non-function
                    // expectation still fails, just via the ordinary assignability
                    // check at the use site instead of a special case here.
                    return function_pointer_type_of(resolved_fn, program);
                } else if constexpr (std::is_same_v<S, ExtFunctionSymbol>) {
                    if (sym.is_variadic && !(expected && expected->kind == TypeKind::Function)) {
                        return error(diag, v.location, std::format("cannot take the address of variadic function '{}'; function pointers to variadic functions are not supported", v.name));
                    }
                    // Allow taking address when expected type is a matching function type
                    if (expected && expected->kind == TypeKind::Function) {
                        const auto &exp_sig = fn_sig(*expected, program);
                        std::vector<ResolvedType> ext_returns;
                        if (sym.return_type) ext_returns.push_back(*sym.return_type);
                        if (function_params_compatible(sym.params, exp_sig.param_types) &&
                            ext_returns == exp_sig.return_types &&
                            sym.is_variadic == exp_sig.is_variadic) {
                            return *expected;
                        }
                        return error(diag, v.location, std::format("'{}' has a different signature from the expected function type", v.name));
                    }
                    // Same decay as an ordinary function above.
                    return ext_function_pointer_type_of(sym, program);
                } else if constexpr (std::is_same_v<S, MacroSymbol>) {
                    if (expected && expected->kind == TypeKind::Function) {
                        return error(diag, v.location, std::format("cannot take the address of macro '{}'", v.name));
                    }
                    return error(diag, v.location, std::format("'{}' is a macro; did you mean to call it?", v.name));
                } else {
                    return error(diag, v.location, std::format("'{}' is a type, not a value", v.name));
                }
            },
            sym_it->second);

    }

    auto check_unary_expr(const ast::Expr &expr, const std::unique_ptr<ast::UnaryExpr> & v, LocalScope &locals, const std::string &module_path, Program &program, DiagnosticEngine &diag, const std::optional<ResolvedType> expected, const int loop_depth, const int defer_loop_base, const ResolvedType *fn_error_type) -> ResolvedType {
        switch (v->op) {
        case ast::UnaryOp::Negate:
            {
                // A negated integer literal is range-checked here, as a whole,
                // rather than letting the literal case check its magnitude alone:
                // '-128' is representable in i8 while '128' is not, and '-1' must be
                // rejected for an unsigned target even though '1' fits.
                if (const auto *lit = std::get_if<ast::LiteralIntegerExpr>(&v->operand);
                    lit && expected && expected->is_integer()) {
                    if (!integer_literal_fits(lit->value, /*negative=*/true, *expected)) {
                        diag.report_error(DiagnosticStage::Sema, v->location, std::format(
                            "integer literal -{} is out of range for '{}' ({}); use 'cast(...)' if truncation is intended",
                            lit->value, describe_type(*expected, program), describe_integer_range(*expected)));
                    }
                    // Returning without recursing skips the expr_types entry
                    // check_expr's wrapper would have recorded for the operand, and
                    // codegen's UnaryExpr case reads exactly that to type the
                    // CreateNeg. Record it here instead of re-checking the literal,
                    // which would range-check its magnitude as a positive value and
                    // reject '-128' for i8.
                    expr_tables_for_write(program, module_path).expr_types[get_expr_key(v->operand)] = *expected;
                    return *expected;
                }
                const ResolvedType operand = check_expr(v->operand, locals, module_path, program, diag, expected, loop_depth, defer_loop_base, fn_error_type);
                if (operand.kind == TypeKind::Opaque) {
                    return opaque_unary_result("negate", operand, diag, v->location, program);
                }
                if (!operand.is_integer() && !operand.is_float()) {
                    return error(diag, v->location, "unary '-' requires a numeric operand");
                }
                return operand;
            }
        case ast::UnaryOp::LogicalNot:
            check_expr(v->operand, locals, module_path, program, diag, ResolvedType{.kind = TypeKind::Bool}, loop_depth, defer_loop_base, fn_error_type);
            return ResolvedType{.kind = TypeKind::Bool};
        case ast::UnaryOp::BitwiseNot:
            {
                const ResolvedType operand = check_expr(v->operand, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_error_type);
                if (operand.kind == TypeKind::Opaque) {
                    return opaque_unary_result("apply '~' to", operand, diag, v->location, program);
                }
                if (!operand.is_integer() && operand.kind != TypeKind::Bitset) {
                    return error(diag, v->location, "unary '~' requires an integer or bitset operand");
                }
                return operand;
            }
        case ast::UnaryOp::AddressOf:
            {
                const LvalueInfo lv = resolve_lvalue(v->operand, locals, module_path, program, diag, loop_depth, defer_loop_base, fn_error_type);
                // Taking the address of an error-tracked local invalidates its
                // typestate to Unknown — spec only requires this when the pointer
                // then feeds a call argument, but unconditional invalidation on any
                // address-of is a safe, simpler over-approximation.
                if (const auto *ident = std::get_if<ast::IdentExpr>(&v->operand)) {
                    if (const auto it = locals.find(ident->name); it != locals.end() && it->second.err_state != ErrorState::NotApplicable) {
                        it->second.err_state = ErrorState::Unknown;
                        it->second.err_unknown_reason = ErrorUnknownReason::AddressTaken;
                    }
                }
                return intern_pointer(program, lv.type);
            }
        case ast::UnaryOp::Deref:
            {
                const ResolvedType operand = check_expr(v->operand, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_error_type);
                if (operand.kind == TypeKind::Opaque) {
                    return opaque_unary_result("dereference", operand, diag, v->location, program);
                }
                if (operand.kind == TypeKind::Trait) return error(diag, v->location, "cannot dereference a trait handle");
                if (operand.kind != TypeKind::Pointer) return error(diag, v->location, "cannot dereference a non-pointer value");
                const auto *pointee = program.pointee_at(operand.pointee_index);
                if (!pointee) return error(diag, v->location, "internal error: invalid pointer index");
                return *pointee;
            }
        }
        return ResolvedType{.kind = TypeKind::Invalid};

    }

    auto check_assign_expr(const ast::Expr &expr, const std::unique_ptr<ast::AssignExpr> & v, LocalScope &locals, const std::string &module_path, Program &program, DiagnosticEngine &diag, const std::optional<ResolvedType> expected, const int loop_depth, const int defer_loop_base, const ResolvedType *fn_error_type) -> ResolvedType {
        auto target = resolve_lvalue(v->target, locals, module_path, program, diag, loop_depth, defer_loop_base, fn_error_type);
        if (target.type.kind != TypeKind::Invalid && !target.writable) {
            error(diag, v->location, "left-hand side of assignment is not mutable");
        }
        // The RHS is checked BEFORE the target's error typestate is invalidated: runtime
        // order evaluates the RHS first, so 'err = match err { ... }' inside an
        // 'if err { }' branch (the natural "map the error" idiom) must see the still-
        // narrowed state. Invalidating first spuriously rejected it with "cannot match on
        // an error value of unknown state".
        const auto value_ty = check_expr(v->value, locals, module_path, program, diag, target.type, loop_depth, defer_loop_base, fn_error_type);

        // Reassigning an error-tracked local invalidates its typestate to Unknown,
        // regardless of assignment operator. The reason resets too, so stale
        // AddressTaken-style advice text never outlives the binding it described.
        if (const auto *ident = std::get_if<ast::IdentExpr>(&v->target)) {
            if (const auto it = locals.find(ident->name); it != locals.end() && it->second.err_state != ErrorState::NotApplicable) {
                it->second.err_state = ErrorState::Unknown;
                it->second.err_unknown_reason = ErrorUnknownReason::Unchecked;
            }
        }
        if (v->op == ast::AssignOp::Assign) {
            if (!assignable_in_module(value_ty, target.type, module_path, program)) {
                error(diag, v->location, "type mismatch in assignment");
            }
            return target.type;
        }

        if (v->op == ast::AssignOp::ToggleAssign && target.type.kind != TypeKind::Bitset) {
            error(diag, v->location, "'~=' is only valid on bitset types; use '^=' for integer XOR-assign");
            return target.type;
        }
        if (target.type.kind == TypeKind::Bitset) {
            if (v->op != ast::AssignOp::AddAssign && v->op != ast::AssignOp::SubAssign && v->op != ast::AssignOp::ToggleAssign) {
                error(diag, v->location, "bitset types only support '+=', '-=', and '~=' compound assignment");
                return target.type;
            }
            // value_ty was already checked above with expected = target.type, so a
            // bare '.Member' RHS already resolved to this bitset type; this also
            // naturally rejects a raw-integer RHS (e.g. 'modes += 5').
            if (value_ty.kind != TypeKind::Bitset || value_ty.bitset_index != target.type.bitset_index) {
                error(diag, v->location, "right-hand side of bitset compound assignment must be a member of "
                                          "the bitset's enum or a value of the same bitset type");
            }
            return target.type;
        }

        // 'total += v' where 'total: T': whether 'T' is scalar is exactly what
        // isn't known yet. Permissive for an unconstrained parameter; a bounded one
        // is rejected by binary_op_result below, which owns that message.
        if (!target.type.is_scalar() && target.type.kind != TypeKind::Opaque) {
            error(diag, v->location, "compound assignment requires a scalar left-hand side");
            return target.type;
        }

        auto equivalent_op = ast::BinaryOp::Add;

        switch (v->op) {
        case ast::AssignOp::AddAssign: equivalent_op = ast::BinaryOp::Add; break;
        case ast::AssignOp::SubAssign: equivalent_op = ast::BinaryOp::Sub; break;
        case ast::AssignOp::MulAssign: equivalent_op = ast::BinaryOp::Mul; break;
        case ast::AssignOp::DivAssign: equivalent_op = ast::BinaryOp::Div; break;
        case ast::AssignOp::AndAssign: equivalent_op = ast::BinaryOp::BitwiseAnd; break;
        case ast::AssignOp::OrAssign:  equivalent_op = ast::BinaryOp::BitwiseOr; break;
        case ast::AssignOp::XorAssign: equivalent_op = ast::BinaryOp::BitwiseXor; break;
        case ast::AssignOp::ShlAssign: equivalent_op = ast::BinaryOp::ShiftLeft; break;
        case ast::AssignOp::ShrAssign: equivalent_op = ast::BinaryOp::ShiftRight; break;
        case ast::AssignOp::ToggleAssign: equivalent_op = ast::BinaryOp::BitwiseXor; break; // unreachable: bitset targets return above, non-bitset targets error above
        case ast::AssignOp::Assign:    break;
        }

        if (auto op_result_ty = binary_op_result(equivalent_op, target.type, value_ty, diag, v->location, program); op_result_ty.kind != TypeKind::Invalid && !assignable_in_module(op_result_ty, target.type, module_path, program)) {
            error(diag, v->location, "compound assignment result type does not match the left-hand side's type");
        }

        return target.type;

    }

    auto check_type_of_expr(const ast::Expr &expr, const std::unique_ptr<ast::TypeOfExpr> & v, LocalScope &locals, const std::string &module_path, Program &program, DiagnosticEngine &diag, const std::optional<ResolvedType> expected, const int loop_depth, const int defer_loop_base, const ResolvedType *fn_error_type) -> ResolvedType {
        // Same ident/qualified-type-name special lookup as SizeOfExpr above, so
        // 'type_of(TypeName)'/'type_of(module.TypeName)' resolve the NAMED type
        // directly rather than mis-evaluating it as a value expression. Every
        // resolved operand type gets registered into Program::type_ids right here
        // (not lazily at codegen time) — codegen only ever reads that map.
        ResolvedType operand_type;
        bool resolved_as_type_name = false;
        if (auto *ident = std::get_if<ast::IdentExpr>(&v->operand)) {
            const auto mod_it = program.modules.find(module_path);
            if (mod_it != program.modules.end()) {
                if (auto sym_it = mod_it->second.symbols.find(ident->name); sym_it != mod_it->second.symbols.end() && std::holds_alternative<TypeSymbol>(sym_it->second)) {
                    operand_type = resolve_type_symbol(module_path, ident->name, program, diag, v->location);
                    resolved_as_type_name = true;
                }
            }
        } else if (auto *mem = std::get_if<std::unique_ptr<ast::MemberExpr>>(&v->operand)) {
            if (auto target_module = try_resolve_namespace_chain((*mem)->object, module_path, locals, program)) {
                auto mod_it = program.modules.find(*target_module);
                if (mod_it != program.modules.end()) {
                    if (auto sym_it = mod_it->second.symbols.find((*mem)->member); sym_it != mod_it->second.symbols.end() && std::holds_alternative<TypeSymbol>(sym_it->second)) {
                        operand_type = resolve_type_symbol(*target_module, (*mem)->member, program, diag, v->location);
                        resolved_as_type_name = true;
                    }
                }
            }
        }
        if (!resolved_as_type_name) {
            if (const auto *inst = std::get_if<std::unique_ptr<ast::IndexOrInstantiateExpr>>(&v->operand)) {
                if (const auto instantiated = try_resolve_generic_type_instantiation(**inst, module_path, program, diag)) {
                    operand_type = *instantiated;
                    resolved_as_type_name = true;
                    // As in size_of/align_of above: codegen's type_of_operand falls
                    // back to expr_types for operand shapes its own fast paths do
                    // not recognize, and cannot instantiate one itself.
                    expr_tables_for_write(program, module_path).expr_types[get_expr_key(v->operand)] = operand_type;
                }
            }
        }
        if (!resolved_as_type_name) {
            operand_type = check_expr(v->operand, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_error_type);
        }
        if (operand_type.kind == TypeKind::Namespace) {
            // Same gap as size_of/align_of: a bare imported-module identifier is not
            // a value and has no type, but used to intern a meaningless type id.
            return error(diag, get_expr_location(v->operand),
                "'type_of' requires a type or a value; this names an imported module");
        }
        // 'contains_opaque': inside a generic DECLARATION 'type_of(x)' names a type
        // that does not exist yet, and registering it would make codegen emit a
        // '__type_info_<id>' global for it. Each real instantiation registers its
        // own concrete type when it is checked.
        if (operand_type.kind != TypeKind::Invalid && !contains_opaque(operand_type, program)) {
            const auto id = intern_type_id(program, operand_type);
            // Register for possible reflection unconditionally, not just when this
            // TypeOfExpr is the direct syntactic argument of type_info_of — the
            // resulting 'type' value can just as easily be stored in a variable and
            // handed to type_info_of indirectly (or reach it via any other runtime
            // control flow), and the runtime binary-search table has no other way to
            // learn about this type. See declare_type_info_globals in codegen.cpp.
            program.types_needing_info.insert(id);
            program.types_needing_info_types[id] = operand_type;
        }
        expr_tables_for_write(program, module_path).expr_type_of_operand_type[get_expr_key(expr)] = operand_type;
        return ResolvedType{.kind = TypeKind::Type};

    }

    auto check_member_expr(const ast::Expr &expr, const std::unique_ptr<ast::MemberExpr> & v, LocalScope &locals, const std::string &module_path, Program &program, DiagnosticEngine &diag, const std::optional<ResolvedType> expected, const int loop_depth, const int defer_loop_base, const ResolvedType *fn_error_type) -> ResolvedType {
        // Cross-module function pointer taking: 'mod.fn_name' in value position.
        // Mirrors the same-module IdentExpr arms above, including the decay when
        // there is no function-typed expectation — try_resolve_namespace_chain
        // already guarantees the object is a module here, so a FunctionSymbol/
        // ExtFunctionSymbol member can only mean "name this function as a value";
        // anything else falls through to ordinary member resolution below.
        if (const auto target_mod = try_resolve_namespace_chain(v->object, module_path, locals, program)) {
            const auto mod_it = program.modules.find(*target_mod);
            if (mod_it != program.modules.end()) {
                const auto sym_it = mod_it->second.symbols.find(v->member);
                if (sym_it != mod_it->second.symbols.end()) {
                    const auto *exp_sig = expected && expected->kind == TypeKind::Function
                        ? &fn_sig(*expected, program) : nullptr;
                    if (const auto *generic_fs = std::get_if<FunctionSymbol>(&sym_it->second);
                        generic_fs && is_generic_function(*generic_fs)) {
                        return error(diag, v->location, std::format(
                            "'{}' is a generic function; supply its generic arguments to name a "
                            "specific instantiation (e.g. '{}[i32]')", v->member, v->member));
                    }
                    if (std::holds_alternative<FunctionSymbol>(sym_it->second)) {
                        auto &fn = ensure_function_signature_resolved(*target_mod, v->member, program, diag);
                        if (!fn.is_pub) return error(diag, v->location, std::format("'{}' is not pub", v->member));
                        if (fn.is_variadic) {
                            return error(diag, v->location, std::format("cannot take the address of variadic function '{}'; function pointers to variadic functions are not supported", v->member));
                        }
                        if (fn.decl && find_attribute(fn.decl->attributes, "always_inline")) {
                            diag.warn(DiagnosticStage::Sema, v->location, std::format(
                                "taking the address of '@always_inline' function '{}'. Calls through a "
                                "function pointer cannot be inlined. The function will still exist as a "
                                "symbol but the '@always_inline' attribute has no effect on indirect calls.",
                                v->member));
                        }
                        if (!exp_sig) return function_pointer_type_of(fn, program);
                        if (function_params_compatible(fn.params, exp_sig->param_types) &&
                            fn.return_types == exp_sig->return_types &&
                            !exp_sig->is_variadic) {
                            return *expected;
                        }
                        return error(diag, v->location, std::format("'{}' has a different signature from the expected function type", v->member));
                    }
                    if (const auto *ef = std::get_if<ExtFunctionSymbol>(&sym_it->second)) {
                        if (!ef->is_pub) return error(diag, v->location, std::format("'{}' is not pub", v->member));
                        if (!exp_sig) {
                            if (ef->is_variadic) {
                                return error(diag, v->location, std::format("cannot take the address of variadic function '{}'; function pointers to variadic functions are not supported", v->member));
                            }
                            return ext_function_pointer_type_of(*ef, program);
                        }
                        std::vector<ResolvedType> ext_returns;
                        if (ef->return_type) ext_returns.push_back(*ef->return_type);
                        if (function_params_compatible(ef->params, exp_sig->param_types) &&
                            ext_returns == exp_sig->return_types &&
                            ef->is_variadic == exp_sig->is_variadic) {
                            return *expected;
                        }
                        return error(diag, v->location, std::format("'{}' has a different signature from the expected function type", v->member));
                    }
                }
            }
        }
        return resolve_member(*v, locals, module_path, program, diag, loop_depth, defer_loop_base, fn_error_type, /*need_writable=*/false).type;

    }

    auto check_tagged_variant_expr(const ast::Expr &expr, const std::unique_ptr<ast::TaggedVariantExpr> & v, LocalScope &locals, const std::string &module_path, Program &program, DiagnosticEngine &diag, const std::optional<ResolvedType> expected, const int loop_depth, const int defer_loop_base, const ResolvedType *fn_error_type) -> ResolvedType {
        // Resolve the tagged union type
        ResolvedType union_ty;
        if (v->type_path) {
            union_ty = resolve_type(ast::Type{clone_named_type(*v->type_path)}, module_path, program, diag);
        } else if (expected && expected->kind == TypeKind::Union) {
            union_ty = *expected;
        } else {
            return error(diag, v->location, "cannot infer tagged union type; provide an explicit type name (e.g. 'TypeName.variant{...}')");
        }
        if (union_ty.kind != TypeKind::Union) {
            return error(diag, v->location, std::format("'{}' is not a union type", format_named_type(*v->type_path)));
        }
        const auto *union_info_ptr = program.union_at(union_ty.union_index);
        if (!union_info_ptr) {
            return error(diag, v->location, "internal error: invalid union index");
        }
        const auto &union_info = *union_info_ptr;
        if (!union_info.is_tagged) {
            return error(diag, v->location, "use '{member = val}' syntax for untagged unions");
        }
        const auto variant_it = std::ranges::find(union_info.variants, v->variant_name, &TaggedUnionVariant::name);
        if (variant_it == union_info.variants.end()) {
            return error_as(diag, v->location,
                std::format("no variant '{}' on {}", v->variant_name, union_info.is_error_union ? "error" : "tagged union"), union_ty);
        }
        if (variant_it->payload_struct_index < 0) {
            return error_as(diag, v->location, std::format("variant '{}' has no payload; use '.{}' without braces", v->variant_name, v->variant_name), union_ty);
        }
        {
            const auto &bv = v->payload;
            const auto *struct_info_ptr = program.struct_at(variant_it->payload_struct_index);
            if (!struct_info_ptr) {
                return error(diag, v->location, "internal error: invalid payload struct index");
            }
            const auto &struct_info = *struct_info_ptr;
            std::unordered_set<std::string> seen;
            for (const auto &sf : bv.fields) {
                if (!seen.insert(sf.name).second) {
                    error(diag, sf.location, std::format("duplicate field '{}' in variant initializer", sf.name));
                }
                const auto it = std::ranges::find(struct_info.fields, sf.name, &StructField::name);
                if (it == struct_info.fields.end()) {
                    error(diag, sf.location, std::format("no field '{}' in variant '{}'", sf.name, v->variant_name));
                    continue;
                }
                const auto val_ty = check_expr(sf.expr, locals, module_path, program, diag, it->type, loop_depth, defer_loop_base, fn_error_type);
                if (!assignable_in_module(val_ty, it->type, module_path, program)) {
                    error(diag, sf.location, std::format("type mismatch for field '{}'", sf.name));
                }
            }
            for (const auto &f : struct_info.fields) {
                if (f.init_expr != nullptr) continue;
                if (!std::ranges::any_of(bv.fields, [&](const auto &sf) { return sf.name == f.name; })) {
                    error(diag, v->location, std::format("missing field '{}' in variant '{}'", f.name, v->variant_name));
                }
            }
        }
        return union_ty;
    }

    auto check_dot_ident_expr(const ast::Expr &expr, const ast::DotIdentExpr & v, LocalScope &locals, const std::string &module_path, Program &program, DiagnosticEngine &diag, const std::optional<ResolvedType> expected, const int loop_depth, const int defer_loop_base, const ResolvedType *fn_error_type) -> ResolvedType {
        // Dot-prefixed enum field literal: .field_name
        // Requires an expected enum or tagged union type
        if (expected && expected->kind == TypeKind::Enum) {
            const auto *enum_info = program.enum_at(expected->enum_index);
            if (!enum_info) return error(diag, v.location, "internal error: invalid enum index");
            for (const auto &field : enum_info->fields) {
                if (field.name == v.name) {
                    return *expected;
                }
            }
            return error_as(diag, v.location, std::format("no enum field named '{}'", v.name), *expected);
        }
        if (expected && expected->kind == TypeKind::Bitset) {
            // A bare '.Member' resolved against a bitset-expected type resolves
            // directly to the BITSET type (not the underlying enum) — this is what
            // makes '.Close' usable as a compound-assign RHS or 'in' operand with no
            // separate coercion step. Members ARE the underlying enum's variants;
            // bitset has no field list of its own.
            const auto *bitset_info = program.bitset_at(expected->bitset_index);
            if (!bitset_info) return error(diag, v.location, "internal error: invalid bitset index");
            const auto *enum_info = program.enum_at(bitset_info->member_enum_type.enum_index);
            if (!enum_info) return error(diag, v.location, "internal error: invalid bitset member enum index");
            for (const auto &field : enum_info->fields) {
                if (field.name == v.name) {
                    return *expected;
                }
            }
            return error_as(diag, v.location, std::format("no bitset member named '{}'", v.name), *expected);
        }
        if (expected && expected->kind == TypeKind::Union) {
            const auto *union_info = program.union_at(expected->union_index);
            if (union_info && union_info->is_tagged) {
                const auto it = std::ranges::find(union_info->variants, v.name, &TaggedUnionVariant::name);
                if (it == union_info->variants.end()) {
                    return error_as(diag, v.location,
                        std::format("no variant '{}' on {}", v.name, union_info->is_error_union ? "error" : "tagged union"), *expected);
                }
                if (it->payload_struct_index >= 0) {
                    return error_as(diag, v.location, std::format("variant '{}' has a payload; use '.{}{{...}}' syntax", v.name, v.name), *expected);
                }
                return *expected;
            }
        }
        // '.Empty' compared against a field whose type is an unbound generic
        // parameter (or a member of a generic aggregate, which is equally opaque):
        // there is no enum to resolve the name against yet. Accept and let the
        // real instantiation resolve it.
        if (expected && expected->kind == TypeKind::Opaque) {
            return ResolvedType{.kind = TypeKind::Opaque};
        }
        return error(diag, v.location, std::format("cannot resolve '.{}' without an expected enum or tagged union type", v.name));

    }

    void check_for_in_stmt(const std::unique_ptr<ast::ForInStmt> &v, LocalScope &locals, const std::string &module_path, Program &program, DiagnosticEngine &diag, const std::vector<ResolvedType> &expected_returns, const int loop_depth, const int defer_loop_base, const ResolvedType *fn_error_type) {
        if (const auto *rp = std::get_if<std::unique_ptr<ast::RangeExpr>>(&v->iterable)) {
            const auto &range = **rp;
            if (v->element_by_ref) {
                diag.report_error(DiagnosticStage::Sema, v->location, "range 'for-in' does not support '&' element binding");
                return;
            }
            // Non-literal side first, same trick BinaryExpr/Ternary use: a literal upper
            // bound must adopt a typed lower bound's type, or 'for i in start..10' with
            // 'start: usize' resolves '10' to i32 and then reports a bound mismatch —
            // while 'for i in 0..len' compiled.
            ResolvedType upper_type;
            std::optional<ResolvedType> lower_type;
            if (range.lower && is_coercible_literal(range.upper) && !is_coercible_literal(*range.lower)) {
                lower_type = check_expr(*range.lower, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_error_type);
                upper_type = check_expr(range.upper, locals, module_path, program, diag, *lower_type, loop_depth, defer_loop_base, fn_error_type);
            } else {
                upper_type = check_expr(range.upper, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_error_type);
                if (range.lower) {
                    lower_type = check_expr(*range.lower, locals, module_path, program, diag, upper_type, loop_depth, defer_loop_base, fn_error_type);
                }
            }
            // 'for i in 0..N' inside 'fn f[N: usize]()': the bound is a generic
            // value parameter, so it has no integer type yet. Accept and check the
            // body — the loop is perfectly well-formed once 'N' is known.
            const bool upper_opaque = upper_type.kind == TypeKind::Opaque;
            if (!upper_type.is_integer() && !upper_opaque) {
                diag.report_error(DiagnosticStage::Sema, v->location, "range upper bound must be an integer type");
                return;
            }
            if (lower_type && *lower_type != upper_type && !upper_opaque && lower_type->kind != TypeKind::Opaque) {
                diag.report_error(DiagnosticStage::Sema, v->location, "range lower and upper bounds must have the same type");
                return;
            }
            auto inner = locals;
            if (v->index_name != "_") {
                inner[v->index_name] = LocalBinding{.type = ResolvedType{.kind = TypeKind::USize}, .is_mut = false};
            }
            if (v->element_name != "_") {
                inner[v->element_name] = LocalBinding{.type = upper_type, .is_mut = false};
            }
            check_stmt(v->body, inner, module_path, program, diag, expected_returns, loop_depth + 1, defer_loop_base);
            return;
        }
        const auto iterable_type = check_expr(v->iterable, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_error_type);
        // Iterating a generic aggregate ('for v in self.data' where the field is
        // '[N]T'): the element type is unknowable, but the loop is still checkable.
        if (iterable_type.kind == TypeKind::Opaque) {
            auto inner = locals;
            if (v->index_name != "_") {
                inner[v->index_name] = LocalBinding{.type = ResolvedType{.kind = TypeKind::USize}, .is_mut = false};
            }
            if (v->element_name != "_") {
                inner[v->element_name] = LocalBinding{.type = ResolvedType{.kind = TypeKind::Opaque}, .is_mut = false};
            }
            check_stmt(v->body, inner, module_path, program, diag, expected_returns, loop_depth + 1, defer_loop_base);
            return;
        }
        if (iterable_type.kind != TypeKind::Slice && iterable_type.kind != TypeKind::Array) {
            diag.report_error(DiagnosticStage::Sema, v->location, "'for-in' requires a slice or array operand");
            return;
        }
        const auto elem_type = iterable_type.kind == TypeKind::Array
            ? array_element_type(iterable_type, module_path, program)
            : slice_element_type(iterable_type, module_path, program);
        auto inner = locals;
        if (v->index_name != "_") {
            inner[v->index_name] = LocalBinding{.type = ResolvedType{.kind = TypeKind::USize}, .is_mut = false};
        }
        if (v->element_name != "_") {
            if (v->element_by_ref) {
                auto ptr_type = intern_pointer(program, elem_type);
                inner[v->element_name] = LocalBinding{.type = ptr_type, .is_mut = false};
            } else {
                inner[v->element_name] = LocalBinding{.type = elem_type, .is_mut = false};
            }
        }
        check_stmt(v->body, inner, module_path, program, diag, expected_returns, loop_depth + 1, defer_loop_base);

    }

    auto check_expr(const ast::Expr &expr, LocalScope &locals, const std::string &module_path, Program &program, DiagnosticEngine &diag, const std::optional<ResolvedType> expected_in, const int loop_depth, const int defer_loop_base, const ResolvedType *fn_error_type) -> ResolvedType {
        // An 'any' expectation must not steer how the expression types ITSELF. 'any' erases
        // whatever natural type a value already has, so it carries no information downward —
        // and several arms below forward the incoming expected type into their sub-expressions
        // (BinaryExpr checks its LHS with it, check_unary_expr its operand). Forwarding 'any'
        // there made both operands of 'takes(x + 1)' individually coerce, and the operator then
        // saw two 16-byte fat pointers instead of two i32s. So the visit runs with no
        // expectation and the any-coercion tail erases the natural result afterwards.
        //
        // 'undefined'/'default' are the exceptions: they have no natural type of their own and
        // exist precisely to adopt the expected one.
        const bool erase_any_expectation = expected_in && expected_in->kind == TypeKind::Any &&
            !std::holds_alternative<ast::UndefinedExpr>(expr) && !std::holds_alternative<ast::DefaultExpr>(expr);
        const std::optional<ResolvedType> expected = erase_any_expectation ? std::nullopt : expected_in;

        const auto ty = std::visit(
            [&]<typename T0>(const T0 &v) -> ResolvedType {
                using V = std::decay_t<T0>;

                if constexpr (std::is_same_v<V, ast::LiteralIntegerExpr>) {
                    if (expected && expected->is_integer()) {
                        // Reject a literal that cannot be represented in the type it is being
                        // coerced to, rather than silently truncating it at codegen. Only the
                        // literal itself is checked, never a computed expression -- 'x + 1'
                        // overflowing is a runtime concern, not this.
                        //
                        // 'cast(300, u8)' stays legal: CastExpr checks its value with no
                        // expected type, so the literal defaults to i32 and never reaches here.
                        // That makes the explicit cast the intended escape hatch for
                        // deliberate truncation.
                        if (!integer_literal_fits(v.value, /*negative=*/false, *expected)) {
                            // Report but still return the expected type: yielding Invalid would
                            // add a second, redundant "type mismatch" on the same line.
                            diag.report_error(DiagnosticStage::Sema, v.location, std::format(
                                "integer literal {} is out of range for '{}' ({}); use 'cast(...)' if truncation is intended",
                                v.value, describe_type(*expected, program), describe_integer_range(*expected)));
                        }
                        return *expected;
                    }
                    if (expected && expected->is_float()) return *expected;
                    return ResolvedType{.kind = TypeKind::I32};

                } else if constexpr (std::is_same_v<V, ast::LiteralFloatExpr>) {
                    if (expected && expected->is_float()) return *expected;
                    return ResolvedType{.kind = TypeKind::F64};

                } else if constexpr (std::is_same_v<V, ast::LiteralStringExpr>) {
                    return intern_slice(program, ResolvedType{.kind = TypeKind::U8});

                } else if constexpr (std::is_same_v<V, ast::LiteralCharExpr>) {
                    return ResolvedType{.kind = TypeKind::U8};

                } else if constexpr (std::is_same_v<V, ast::LiteralBoolExpr>) {
                    return ResolvedType{.kind = TypeKind::Bool};

                } else if constexpr (std::is_same_v<V, ast::LiteralNilExpr>) {
                    if (expected && (expected->kind == TypeKind::Slice || expected->kind == TypeKind::Trait)) return *expected;
                    return ResolvedType{.kind = TypeKind::Anyptr};

                } else if constexpr (std::is_same_v<V, ast::IdentExpr>) {
                    return check_ident_expr(expr, v, locals, module_path, program, diag, expected, loop_depth, defer_loop_base, fn_error_type);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::UnaryExpr>>) {
                    return check_unary_expr(expr, v, locals, module_path, program, diag, expected, loop_depth, defer_loop_base, fn_error_type);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::BinaryExpr>>) {
                    if (v->op == ast::BinaryOp::In) {
                        // RHS checked FIRST (no expected type), then its resolved type is used
                        // as the expected type when checking LHS — this is what lets
                        // '{.Close, .Flush} in modes' and '.Close in modes' resolve: the
                        // braced-literal/dot-ident LHS needs a Bitset expected type to
                        // disambiguate against, which only the RHS can supply.
                        const ResolvedType rhs_ty = check_expr(v->rhs, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_error_type);
                        if (rhs_ty.kind != TypeKind::Bitset) {
                            return error(diag, v->location, "'in' is only valid for bitset membership testing; right-hand side must be a bitset value");
                        }
                        const ResolvedType lhs_ty = check_expr(v->lhs, locals, module_path, program, diag, rhs_ty, loop_depth, defer_loop_base, fn_error_type);
                        if (lhs_ty.kind != TypeKind::Bitset || lhs_ty.bitset_index != rhs_ty.bitset_index) {
                            return error(diag, v->location, "left-hand side of 'in' must be a single bitset member or a value of the same bitset type");
                        }
                        return ResolvedType{.kind = TypeKind::Bool};
                    }

                    // Contextual operands — literals, '.Variant', '{.A, .B}' — resolve
                    // against the OTHER side's type, whichever side they're on. The swap
                    // used to fire only for int/float/nil literals, so 'color == .Red'
                    // worked while '.Red == color' failed with "cannot resolve '.Red'".
                    const auto is_contextual = [](const ast::Expr &e) {
                        return is_coercible_literal(e) ||
                               std::holds_alternative<ast::DotIdentExpr>(e) ||
                               std::holds_alternative<std::unique_ptr<ast::BracedInitializerExpr>>(e);
                    };
                    const bool operand_cmp = v->op == ast::BinaryOp::Equal || v->op == ast::BinaryOp::NotEqual ||
                                             v->op == ast::BinaryOp::Less || v->op == ast::BinaryOp::Greater ||
                                             v->op == ast::BinaryOp::LessEqual || v->op == ast::BinaryOp::GreaterEqual;

                    ResolvedType lhs, rhs;
                    if (is_contextual(v->lhs) && !is_contextual(v->rhs)) {
                        rhs = check_expr(v->rhs, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_error_type);
                        lhs = check_expr(v->lhs, locals, module_path, program, diag, rhs, loop_depth, defer_loop_base, fn_error_type);
                    } else {
                        // A comparison's outer expected type describes its RESULT (Bool),
                        // not its operands — forwarding it into the LHS biased contextual
                        // operand resolution toward the wrong type.
                        lhs = check_expr(v->lhs, locals, module_path, program, diag, operand_cmp ? std::nullopt : expected, loop_depth, defer_loop_base, fn_error_type);
                        rhs = check_expr(v->rhs, locals, module_path, program, diag, lhs, loop_depth, defer_loop_base, fn_error_type);
                    }

                    // Trait handles support no operator except '==' / '!=' against a literal
                    // 'nil' operand (checked on the raw AST, not the resolved type, since a
                    // nil literal coerced to the handle's trait type is otherwise structurally
                    // indistinguishable from an actual handle value of that same trait), or
                    // against another handle of the same trait type (compares object identity).
                    if (lhs.kind == TypeKind::Trait || rhs.kind == TypeKind::Trait) {
                        const bool lhs_is_nil = std::holds_alternative<ast::LiteralNilExpr>(v->lhs);
                        const bool rhs_is_nil = std::holds_alternative<ast::LiteralNilExpr>(v->rhs);
                        const bool is_eq = v->op == ast::BinaryOp::Equal || v->op == ast::BinaryOp::NotEqual;
                        const bool same_trait_handles = lhs.kind == TypeKind::Trait && rhs.kind == TypeKind::Trait && lhs.trait_index == rhs.trait_index;
                        if (is_eq && (lhs_is_nil || rhs_is_nil || same_trait_handles)) {
                            return ResolvedType{.kind = TypeKind::Bool};
                        }
                        return error(diag, v->location, "trait handles only support '==' / '!=' comparison against 'nil' or a handle of the same trait type; no other operators are supported");
                    }

                    return binary_op_result(v->op, lhs, rhs, diag, v->location, program);

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::TernaryExpr>>) {
                    check_expr(v->condition, locals, module_path, program, diag, ResolvedType{.kind = TypeKind::Bool}, loop_depth, defer_loop_base, fn_error_type);
                    // Typestate narrowing, mirroring IfStmt/WhileStmt in check_stmt: inside
                    // 'e ? ... : ...' the then-branch knows 'e' is Failed and the else-branch
                    // knows it is Ok, exactly as the statement form does. Without this the
                    // typed-error system was invisible in expression position -- 'if e { match e
                    // {...} }' compiled while the identical 'e ? match e {...} : 0' failed with
                    // "cannot match on an error value of unknown state".
                    //
                    // Each branch gets its own copy of locals, as IfStmt does, so a narrowing
                    // cannot leak into the sibling branch or outward. No merge afterwards: an
                    // expression's branches do not join back into the enclosing scope's state
                    // the way statement branches do.
                    const auto narrowings = compute_condition_narrowing(v->condition, locals, program);
                    auto then_locals = locals;
                    auto else_locals = locals;
                    apply_condition_narrowing(then_locals, narrowings, true, v->condition, program);
                    apply_condition_narrowing(else_locals, narrowings, false, v->condition, program);

                    // Check the non-literal branch first when exactly one branch is a coercible
                    // literal, so the literal unifies against the other branch's real type --
                    // the same ordering swap BinaryExpr does above. Without it, 'cond ? 5 : x'
                    // defaulted the literal to i32 (there is usually no outer expected type),
                    // then checked 'x' with expected=i32, which IdentExpr ignores, and reported
                    // a spurious mismatch -- while 'cond ? x : 5', the identical expression with
                    // its operands swapped, compiled fine.
                    ResolvedType then_ty, else_ty;
                    if (is_coercible_literal(v->then_expr) && !is_coercible_literal(v->else_expr)) {
                        else_ty = check_expr(v->else_expr, else_locals, module_path, program, diag, expected, loop_depth, defer_loop_base, fn_error_type);
                        then_ty = check_expr(v->then_expr, then_locals, module_path, program, diag, else_ty, loop_depth, defer_loop_base, fn_error_type);
                    } else {
                        then_ty = check_expr(v->then_expr, then_locals, module_path, program, diag, expected, loop_depth, defer_loop_base, fn_error_type);
                        else_ty = check_expr(v->else_expr, else_locals, module_path, program, diag, then_ty, loop_depth, defer_loop_base, fn_error_type);
                    }
                    // Same rule as the 'when' expression: an Opaque branch matches anything,
                    // and the concrete side (if any) is the more useful result to propagate.
                    if (then_ty.kind == TypeKind::Opaque) return else_ty;
                    if (else_ty.kind == TypeKind::Opaque) return then_ty;
                    if (then_ty != else_ty) {
                        return error(diag, v->location, "ternary branches have different types");
                    }
                    return then_ty;

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::OptionExpr>>) {
                    return resolve_option_expr(get_expr_key(expr), *v, expected, module_path, program, diag);

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::EnvExpr>>) {
                    return resolve_env_expr(get_expr_key(expr), *v, expected, module_path, program, diag);

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::WhenExpr>>) {
                    check_expr(v->condition, locals, module_path, program, diag, ResolvedType{.kind = TypeKind::Bool}, loop_depth, defer_loop_base, fn_error_type);
                    // Same literal-coercion ordering and same per-branch typestate narrowing as
                    // TernaryExpr above.
                    const auto narrowings = compute_condition_narrowing(v->condition, locals, program);
                    auto then_locals = locals;
                    auto else_locals = locals;
                    apply_condition_narrowing(then_locals, narrowings, true, v->condition, program);
                    apply_condition_narrowing(else_locals, narrowings, false, v->condition, program);

                    ResolvedType then_ty, else_ty;
                    if (is_coercible_literal(v->then_expr) && !is_coercible_literal(v->else_expr)) {
                        else_ty = check_expr(v->else_expr, else_locals, module_path, program, diag, expected, loop_depth, defer_loop_base, fn_error_type);
                        then_ty = check_expr(v->then_expr, then_locals, module_path, program, diag, else_ty, loop_depth, defer_loop_base, fn_error_type);
                    } else {
                        then_ty = check_expr(v->then_expr, then_locals, module_path, program, diag, expected, loop_depth, defer_loop_base, fn_error_type);
                        else_ty = check_expr(v->else_expr, else_locals, module_path, program, diag, then_ty, loop_depth, defer_loop_base, fn_error_type);
                    }
                    // An Opaque branch matches anything: inside a generic declaration
                    // 'when N > 4 { 0 } else { t }' has a concrete 'then' and a generic 'else',
                    // which is not a mismatch until 'N' and 't' are known. Yield the concrete
                    // side when there is one so the surrounding context keeps its information.
                    if (then_ty.kind == TypeKind::Opaque) return else_ty;
                    if (else_ty.kind == TypeKind::Opaque) return then_ty;
                    if (then_ty != else_ty) {
                        return error(diag, v->location, "'when' expression branches have different types");
                    }

                    // The condition/branches may freely contain '$option(...)' (or anything
                    // else) — '$option' is always a compile-time-constant expression (see
                    // OptionExpr's doc comment in ast.hpp), so no special-casing is needed
                    // here beyond the ordinary is_constant_expr/evaluate_const_value fold
                    // below. The restriction to compile-time-constant conditions specifically
                    // inside '#link' contexts is enforced at that call site instead
                    // (declare_link_decl's is_constant_expr check).
                    if (is_constant_expr(v->condition, module_path, program)) {
                        if (const auto folded = evaluate_const_value(v->condition, module_path, program, diag)) {
                            if (const auto *iv = std::get_if<int64_t>(&*folded)) {
                                expr_tables_for_write(program, module_path).expr_when_selected[get_expr_key(expr)] = (*iv != 0);
                            }
                        }
                    }

                    return then_ty;

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::AssignExpr>>) {
                    return check_assign_expr(expr, v, locals, module_path, program, diag, expected, loop_depth, defer_loop_base, fn_error_type);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::CallExpr>>) {
                    // The single choke point every callee shape below funnels its resolved
                    // return list through, so the "one value expected here" rule is stated
                    // once. Two things happen:
                    //
                    //  1. A trailing '?error(...)' slot is DROPPED (and recorded, so codegen
                    //     emits the failure check) whenever the call has 2+ returns. That is
                    //     what makes 'const p := alloc(n)', 'foo(alloc(n))' and
                    //     'return alloc(n)' legal for '-> (anyptr, ?E)'.
                    //  2. Anything still multi-valued keeps the existing diagnostic.
                    //
                    // The '>= 2' guard is load-bearing: a fn returning ONLY '?E' must still
                    // yield that error as this expression's value, so 'const e := f()' keeps
                    // capturing it. A lone '?E' is dropped only in statement position, where
                    // the value is discarded outright (see check_stmt's ExprStmt case).
                    auto single_value_result = [&](std::vector<ResolvedType> returns) -> ResolvedType {
                        if (returns.size() >= 2 && is_optional_error_union(returns.back(), program)) {
                            expr_tables_for_write(program, module_path).call_dropped_optional_error[v.get()] =
                                DroppedOptionalError{.error_type = returns.back(), .slot_index = returns.size() - 1};
                            returns.pop_back();
                        }
                        if (returns.size() > 1) {
                            return error(diag, v->location, "multi-value capture is not yet supported here");
                        }
                        return returns.empty() ? ResolvedType{.kind = TypeKind::Void} : returns.front();
                    };

                    return single_value_result(resolve_call_returns(
                        *v, expected, get_expr_key(expr), locals, module_path, program, diag,
                        loop_depth, defer_loop_base, fn_error_type));

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::IncrDecrExpr>>) {
                    const LvalueInfo lv = resolve_lvalue(v->operand, locals, module_path, program, diag, loop_depth, defer_loop_base, fn_error_type);
                    if (lv.type.kind != TypeKind::Invalid) {
                        // Opaque: 'self.count++' where the field's type comes from an unbound
                        // parameter (or a generic aggregate). Whether it is an integer is a
                        // property of the instantiation.
                        if (!lv.type.is_integer() && lv.type.kind != TypeKind::Pointer && lv.type.kind != TypeKind::Anyptr
                            && lv.type.kind != TypeKind::Opaque) error(diag, v->location, "++ / -- requires an integer operand");
                        if (!lv.writable) error(diag, v->location, "++ / -- requires a mutable operand");
                    }
                    return lv.type;

                } else if constexpr (std::is_same_v<V, ast::ImportExpr>) {
                    return ResolvedType{.kind = TypeKind::Namespace};

                } else if constexpr (std::is_same_v<V, ast::ImportBinExpr>) {
                    return resolve_import_bin_type(module_path, v.path, v.location, program, diag);

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::SizeOfExpr>>) {
                    // size_of on a (possibly qualified) TYPE name - checked
                    // first via try_resolve_namespace_chain so `size_of(a.b.T)`
                    // resolves through arbitrarily many namespace hops, same
                    // as any other qualified type reference. Falls back to
                    // evaluating the operand as an ordinary value expression
                    // (runtime size_of) if it isn't a type-name shape. Operand
                    // shapes the parser can't spell as an IdentExpr/MemberExpr
                    // (pointer/array/slice/fn-ptr types, builtin type keywords)
                    // arrive as a TypeExpr instead and fall through to the
                    // generic check_expr call below, which resolves them via
                    // the TypeExpr case further down in this dispatch.
                    if (auto *ident = std::get_if<ast::IdentExpr>(&v->operand)) {
                        const auto mod_it = program.modules.find(module_path);
                        if (mod_it != program.modules.end()) {
                            if (auto sym_it = mod_it->second.symbols.find(ident->name); sym_it != mod_it->second.symbols.end() && std::holds_alternative<TypeSymbol>(sym_it->second)) {
                                return ResolvedType{.kind = TypeKind::USize};
                            }
                        }
                    } else if (auto *mem = std::get_if<std::unique_ptr<ast::MemberExpr>>(&v->operand)) {
                        if (auto target_module = try_resolve_namespace_chain((*mem)->object, module_path, locals, program)) {
                            auto mod_it = program.modules.find(*target_module);
                            if (mod_it != program.modules.end()) {
                                if (auto sym_it = mod_it->second.symbols.find((*mem)->member); sym_it != mod_it->second.symbols.end() && std::holds_alternative<TypeSymbol>(sym_it->second)) {
                                    return ResolvedType{.kind = TypeKind::USize};
                                }
                            }
                        }
                    }
                    if (const auto *inst = std::get_if<std::unique_ptr<ast::IndexOrInstantiateExpr>>(&v->operand)) {
                        if (const auto instantiated = try_resolve_generic_type_instantiation(**inst, module_path, program, diag)) {
                            // Record the operand's resolved type so codegen can read it back.
                            // codegen holds a const Program and cannot instantiate, so its
                            // 'size_of_operand' falls back to expr_types for anything its own
                            // ident/member fast paths do not recognize.
                            expr_tables_for_write(program, module_path).expr_types[get_expr_key(v->operand)] = *instantiated;
                            return ResolvedType{.kind = TypeKind::USize};
                        }
                    }
                    {
                        const auto operand_type = check_expr(v->operand, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_error_type);
                        if (operand_type.kind == TypeKind::Namespace) {
                            return error(diag, get_expr_location(v->operand),
                                "'size_of' requires a type or a value; this names an imported module");
                        }
                    }
                    return ResolvedType{.kind = TypeKind::USize};

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::AlignOfExpr>>) {
                    // Mirrors SizeOfExpr's case exactly (see comments above) — same type-name
                    // special lookup, same fallback to a generic expr, same USize result type.
                    if (auto *ident = std::get_if<ast::IdentExpr>(&v->operand)) {
                        const auto mod_it = program.modules.find(module_path);
                        if (mod_it != program.modules.end()) {
                            if (auto sym_it = mod_it->second.symbols.find(ident->name); sym_it != mod_it->second.symbols.end() && std::holds_alternative<TypeSymbol>(sym_it->second)) {
                                return ResolvedType{.kind = TypeKind::USize};
                            }
                        }
                    } else if (auto *mem = std::get_if<std::unique_ptr<ast::MemberExpr>>(&v->operand)) {
                        if (auto target_module = try_resolve_namespace_chain((*mem)->object, module_path, locals, program)) {
                            auto mod_it = program.modules.find(*target_module);
                            if (mod_it != program.modules.end()) {
                                if (auto sym_it = mod_it->second.symbols.find((*mem)->member); sym_it != mod_it->second.symbols.end() && std::holds_alternative<TypeSymbol>(sym_it->second)) {
                                    return ResolvedType{.kind = TypeKind::USize};
                                }
                            }
                        }
                    }
                    if (const auto *inst = std::get_if<std::unique_ptr<ast::IndexOrInstantiateExpr>>(&v->operand)) {
                        if (const auto instantiated = try_resolve_generic_type_instantiation(**inst, module_path, program, diag)) {
                            // Record the operand's resolved type so codegen can read it back.
                            // codegen holds a const Program and cannot instantiate, so its
                            // 'align_of_operand' falls back to expr_types for anything its own
                            // ident/member fast paths do not recognize.
                            expr_tables_for_write(program, module_path).expr_types[get_expr_key(v->operand)] = *instantiated;
                            return ResolvedType{.kind = TypeKind::USize};
                        }
                    }
                    {
                        const auto operand_type = check_expr(v->operand, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_error_type);
                        if (operand_type.kind == TypeKind::Namespace) {
                            return error(diag, get_expr_location(v->operand),
                                "'align_of' requires a type or a value; this names an imported module");
                        }
                    }
                    return ResolvedType{.kind = TypeKind::USize};

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::TypeOfExpr>>) {
                    return check_type_of_expr(expr, v, locals, module_path, program, diag, expected, loop_depth, defer_loop_base, fn_error_type);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::TypeInfoOfExpr>>) {
                    const auto operand_ty = check_expr(v->operand, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_error_type);
                    if (operand_ty.kind != TypeKind::Type && operand_ty.kind != TypeKind::Any) {
                        return error(diag, v->location,
                            std::format("type_info_of() requires an argument of type 'type' or 'any'; got '{}'. Use 'type_of(expr)' to get the type ID first.",
                                describe_type(operand_ty, program)));
                    }
                    if (!find_type_info_union(program, diag)) {
                        return error(diag, v->location,
                            "'type_info_of' requires importing a module that defines 'pub type Type_Info = union(enum) {...}' (see runtime/type_info)");
                    }
                    // Fast path: the operand is syntactically 'type_of(T)' — fold T's id right
                    // now and register it for a dedicated '__type_info_<id>' global, rather than
                    // the generic runtime binary-search table. Every other shape (a runtime
                    // 'type' value, or an 'any') falls through to that generic runtime lookup at
                    // codegen time — see codegen.cpp's TypeInfoOfExpr case.
                    if (std::holds_alternative<std::unique_ptr<ast::TypeOfExpr>>(v->operand)) {
                        // The inner 'type_of' was just checked above, so its record is in
                        // whichever table is currently active — read through the accessor
                        // rather than the module's, which an instantiation never writes to.
                        const auto *recorded = find_expr_record(program, module_path,
                            &ExprSideTables::expr_type_of_operand_type, get_expr_key(v->operand));
                        if (!recorded) return ResolvedType{.kind = TypeKind::Anyptr};
                        const auto inner_ty = *recorded;
                        if (!contains_opaque(inner_ty, program)) { // see the TypeOfExpr case above
                            const auto id = intern_type_id(program, inner_ty);
                            expr_tables_for_write(program, module_path).expr_type_info_const_id[get_expr_key(expr)] = id;
                            program.types_needing_info.insert(id);
                            program.types_needing_info_types[id] = inner_ty;
                        }
                    }
                    return ResolvedType{.kind = TypeKind::Anyptr};

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::TypeExpr>>) {
                    // A Type wrapped in an Expr slot (currently only produced by the parser for
                    // size_of/align_of operands that can't be spelled as an ordinary expression - see
                    // starts_type_only in ast.cpp). Resolves like any other type reference; the
                    // result is cached into expr_types by the generic caching below, which is
                    // how codegen's sizeof_operand and type_resolver's sizeof_expr_operand read
                    // it back without needing to know about TypeExpr themselves.
                    return resolve_type(v->type, module_path, program, diag);

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::LenExpr>>) {
                    const auto operand = check_expr(v->operand, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_error_type);
                    // 'len(self.data)' in the eager template pass: the operand type is an
                    // unbound parameter's — accept, like indexing/for-in already do; the
                    // result is usize either way, and the instantiation re-checks.
                    if (operand.kind != TypeKind::Array && operand.kind != TypeKind::Slice && operand.kind != TypeKind::Opaque) {
                        return error(diag, v->location, "len() requires an array or slice operand");
                    }
                    return ResolvedType{.kind = TypeKind::USize};

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::StackAllocExpr>>) {
                    const auto size_ty = check_expr(v->size, locals, module_path, program, diag, ResolvedType{.kind = TypeKind::USize}, loop_depth, defer_loop_base, fn_error_type);
                    // 'stackalloc(N)' with a value generic param: Opaque scalar, sized at
                    // instantiation.
                    if (!size_ty.is_integer() && size_ty.kind != TypeKind::Opaque) {
                        return error(diag, v->location, "stackalloc() requires an integer size expression");
                    }
                    return ResolvedType{.kind = TypeKind::Anyptr};

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::AsmExpr>>) {
                    return check_asm_expr(*v, locals, module_path, program, diag, expected);

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::CastExpr>>) {
                    // cast(expr, Type) - value first, target type second.
                    const ResolvedType from = check_expr(v->value, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_error_type);
                    const ResolvedType to = resolve_type(v->as_type, module_path, program, diag);
                    if (from.kind == TypeKind::Any && to.kind != TypeKind::Pointer && to.kind != TypeKind::Anyptr) {
                        return error(diag, v->location, "'any' may only be cast to a pointer type or 'anyptr'.");
                    }
                    if (from.kind != TypeKind::Invalid && to.kind != TypeKind::Invalid && !is_cast_legal(from, to)) {
                        return error(diag, v->location, "illegal cast between these types");
                    }
                    if (from.kind != TypeKind::Invalid && to.kind == TypeKind::Slice && !slice_cast_elements_match(from, to, module_path, program)) {
                        return error(diag, v->location, "slice cast element type mismatch");
                    }
                    if (v->len_expr) {
                        if (to.kind != TypeKind::Slice) {
                            error(diag, v->location, "cast length is only valid when casting to a slice type");
                        }
                        const auto len_ty = check_expr(*v->len_expr, locals, module_path, program, diag, ResolvedType{.kind = TypeKind::USize}, loop_depth, defer_loop_base, fn_error_type);
                        if (!len_ty.is_integer()) {
                            error(diag, v->location, "cast length must be an integer expression");
                        }
                    } else if (to.kind == TypeKind::Slice && from.kind != TypeKind::Array && from.kind != TypeKind::Slice) {
                        error(diag, v->location, "cast to slice from a pointer requires a length expression");
                    }
                    return to;

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::MemberExpr>>) {
                    return check_member_expr(expr, v, locals, module_path, program, diag, expected, loop_depth, defer_loop_base, fn_error_type);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::IndexOrInstantiateExpr>>) {
                    // Value position: either an ordinary index ('a[i]') or a generic FUNCTION
                    // instantiation naming a monomorphized instance as a function-pointer value
                    // ('fnv1a[i32]', 'fnv1a[K]'). Generic TYPE instantiation is classified
                    // before this, at each position that can accept one -- a call callee
                    // ('make[i32]()'), a declared type ('List[i32]' parsed as a NamedType), and
                    // size_of/align_of/type_of's operand (see
                    // try_resolve_generic_type_instantiation).
                    //
                    // The function check runs FIRST, and before the arg-shape guard below,
                    // because both bracket shapes can name an instantiation: a builtin arg
                    // ('[i32]') is Type-tagged, while a bare generic-param arg ('[K]') parses as
                    // an Expr and would otherwise be misread as an index subscript.
                    if (auto fn_value = try_resolve_generic_fn_instantiation_value(*v, get_expr_key(expr), locals, module_path, program, diag)) {
                        return *fn_value;
                    }
                    // A remaining multi-arg or type-tagged shape is a TYPE instantiation written
                    // somewhere a value is expected, e.g. 'const x := List[i32]' -- which names
                    // a type, not a value. The ordinary-index shape (exactly one Expr-tagged
                    // arg) is the only one that belongs below.
                    if (v->args.size() != 1 || !std::holds_alternative<ast::Expr>(v->args[0].value)) {
                        return error(diag, v->location,
                            "a generic type instantiation names a type, not a value; it cannot be used here");
                    }
                    const auto indexed = check_indexed_element(*v, locals, module_path, program, diag, loop_depth, defer_loop_base, fn_error_type);
                    return indexed ? indexed->element : ResolvedType{.kind = TypeKind::Invalid};

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::SliceExpr>>) {
                    const auto operand = check_expr(v->operand, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_error_type);
                    // An omitted bound has nothing to check: 'start' defaults to 0 and 'end'
                    // to the operand's length, both supplied by codegen.
                    // A bound may be an unbound generic value ('self.data[1..n]' with
                    // '[n: usize]') — Opaque in the eager pass, integer at instantiation.
                    const auto bound_ok = [](const ResolvedType &t) { return t.is_integer() || t.kind == TypeKind::Opaque; };
                    bool bounds_ok = true;
                    if (v->start) {
                        bounds_ok &= bound_ok(check_expr(*v->start, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_error_type));
                    }
                    if (v->end) {
                        bounds_ok &= bound_ok(check_expr(*v->end, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_error_type));
                    }
                    if (!bounds_ok) {
                        error(diag, v->location, "slice bounds must be integer expressions");
                    }
                    if (operand.kind == TypeKind::Array) {
                        const auto element = array_element_type(operand, module_path, program);
                        return intern_slice(program, element);
                    }
                    if (operand.kind == TypeKind::Slice) {
                        return operand;
                    }
                    // Slicing an unbound generic aggregate: the element type is unknowable
                    // here; the instantiation re-checks (same posture as indexing/for-in).
                    if (operand.kind == TypeKind::Opaque) {
                        return ResolvedType{.kind = TypeKind::Opaque};
                    }
                    return error(diag, v->location, "slicing requires an array or slice operand");

                } else if constexpr (std::is_same_v<V, ast::IotaExpr>) {
                    return error(diag, v.location, "'iota' is only valid inside enum field initializers");

                } else if constexpr (std::is_same_v<V, ast::DotIdentExpr>) {
                    return check_dot_ident_expr(expr, v, locals, module_path, program, diag, expected, loop_depth, defer_loop_base, fn_error_type);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::MatchExpr>>) {
                    return check_match_expr(expr, v, locals, module_path, program, diag, expected, loop_depth, defer_loop_base, fn_error_type);
                } else if constexpr (std::is_same_v<V, ast::DefaultExpr>) {
                    if (!expected) {
                        return error(diag, v.location, "'default' requires a known target type");
                    }
                    if (expected->kind == TypeKind::Union) {
                        return error(diag, v.location, "unions have no default value; use an explicit single-member initializer or 'undefined'");
                    }
                    return *expected;
                } else if constexpr (std::is_same_v<V, ast::UndefinedExpr>) {
                    if (!expected) {
                        return error(diag, v.location, "'undefined' requires a known target type");
                    }
                    if (const auto *union_info = expected->kind == TypeKind::Union ? program.union_at(expected->union_index) : nullptr;
                        union_info && union_info->is_tagged) {
                        return error(diag, v.location, "tagged unions have no 'undefined' form; use an explicit variant initializer");
                    }
                    return *expected;
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::TryExpr>>) {
                    if (defer_loop_base >= 0) {
                        return error(diag, v->location, "'try' cannot propagate errors out of a 'defer' body");
                    }
                    if (!fn_error_type) {
                        return error(diag, v->location, "enclosing function must return 'error(...)' to use 'try'");
                    }
                    // The operand must be a CallExpr
                    const auto *call = std::get_if<std::unique_ptr<ast::CallExpr>>(&v->call);
                    if (!call) {
                        return error(diag, v->location, "'try' operand must be a direct function call");
                    }
                    const auto returns = check_group_call_returns(**call, locals, module_path, program, diag, loop_depth, defer_loop_base, fn_error_type);
                    if (returns.empty()) {
                        return ResolvedType{.kind = TypeKind::Void};
                    }
                    // 'try self.helper()' inside a generic impl method: the receiver is an
                    // unbound parameter, so the call's return types are Opaque and there is no
                    // error union to recognize yet. The real instantiation checks it.
                    if (returns.back().kind == TypeKind::Opaque) {
                        return ResolvedType{.kind = TypeKind::Opaque};
                    }
                    // Last return type must be a synthesized error(...) union
                    if (!is_error_union_type(returns.back(), program)) {
                        return error(diag, v->location, "'try' can only be used on a function that returns 'error(...)' as its last return value");
                    }
                    // ...and every error member the callee can produce must be a member of
                    // the caller's own declared error(...) union.
                    if (!error_union_is_subset(returns.back(), *fn_error_type, program)) {
                        return error(diag, v->location,
                            "callee's error type is not a subset of the enclosing function's error type; "
                            "widen the enclosing function's 'error(...)' return type or handle the error explicitly");
                    }
                    // Returns: all return types except the error slot
                    if (returns.size() == 1) {
                        // f() -> error: expression has no value (Void)
                        return ResolvedType{.kind = TypeKind::Void};
                    }
                    if (returns.size() == 2) {
                        // f() -> T, error: expression type is T
                        return returns[0];
                    }
                    // f() -> T1, T2, ..., error: multi-value; cannot be used in expression position
                    return error(diag, v->location, "multi-value 'try' cannot be used in expression position; use group declaration");
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::TaggedVariantExpr>>) {
                    return check_tagged_variant_expr(expr, v, locals, module_path, program, diag, expected, loop_depth, defer_loop_base, fn_error_type);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::BracedInitializerExpr>>) {
                    return check_braced_initializer_expr(expr, v, locals, module_path, program, diag, expected, loop_depth, defer_loop_base, fn_error_type);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::RangeExpr>>) {
                    // Range expressions are only valid as for-in operands; type-check bounds
                    // so expr_types is populated for codegen, then report the contextual error.
                    const auto upper_type = check_expr(v->upper, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_error_type);
                    if (v->lower) {
                        check_expr(*v->lower, locals, module_path, program, diag, upper_type, loop_depth, defer_loop_base, fn_error_type);
                    }
                    return error(diag, v->location, "range expression is only valid as a 'for-in' operand");
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::SpreadExpr>>) {
                    // Legal spreads are unwrapped and checked by check_call_args's variadic-tail
                    // handling *before* recursing into check_expr on the operand directly — this
                    // node is only ever visited here when the spread was in an illegal position
                    // (not the sole trailing argument of a native-variadic call).
                    check_expr(v->operand, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_error_type);
                    return error(diag, v->location,
                        "'...' spread argument is only valid as the sole trailing argument in a call to a "
                        "function with a native '...T' variadic parameter");
                }
            },
            expr);

        expr_tables_for_write(program, module_path).expr_types[get_expr_key(expr)] = ty;

        // Implicit tagged-union coercion: applies generically wherever an expected type is
        // threaded through check_expr (call args, return statements, var-decl initializers,
        // struct/array/union field init) — not special-cased to variadics, even though that's
        // the primary motivating use case. See VariantCoercion's doc comment in sema.hpp for why
        // this is recorded in a side table rather than overwriting expr_types for this node.
        if (const auto *union_info_ptr = expected && ty.kind != TypeKind::Invalid && ty != *expected && expected->kind == TypeKind::Union
                                              ? program.union_at(expected->union_index)
                                              : nullptr;
            union_info_ptr && union_info_ptr->is_tagged) {
            const auto &union_info = *union_info_ptr;
            {
                const TaggedUnionVariant *match = nullptr;
                std::vector<std::string> match_names;
                for (const auto &variant : union_info.variants) {
                    if (variant.payload_struct_index < 0) continue;
                    // Either the value's type is exactly the declared payload type (covers
                    // scalar/slice/pointer payloads directly, and struct payloads reused
                    // verbatim without wrapping — see layout_union in type_resolver.cpp), or
                    // the payload struct has exactly one field and the value matches that
                    // field's type (covers single-field struct payloads passed as a bare value).
                    if (variant.payload_type == ty) {
                        match = &variant;
                        match_names.push_back(variant.name);
                        continue;
                    }
                    const auto *payload_struct = program.struct_at(variant.payload_struct_index);
                    if (payload_struct && payload_struct->fields.size() == 1 && payload_struct->fields[0].type == ty) {
                        match = &variant;
                        match_names.push_back(variant.name);
                    }
                }
                if (match_names.size() == 1) {
                    expr_tables_for_write(program, module_path).expr_variant_coercions[get_expr_key(expr)] = VariantCoercion{
                        .union_type = *expected,
                        .tag_value = match->tag_value,
                        .payload_struct_index = match->payload_struct_index,
                    };
                    return *expected;
                }
                if (match_names.size() > 1) {
                    std::string joined;
                    for (size_t i = 0; i < match_names.size(); ++i) {
                        if (i > 0) joined += ", ";
                        joined += match_names[i];
                    }
                    const auto [union_module, union_name] = find_type_module_and_name(*expected, program);
                    return error(diag, get_expr_location(expr), std::format(
                        "ambiguous implicit coercion to tagged union '{}': variants {} all accept a payload of this type; "
                        "use an explicit variant constructor",
                        union_name.empty() ? "<union>" : union_name, joined));
                }
            }
        }

        // Implicit trait-handle coercion: applies through the same expected-type channel as
        // the tagged-union coercion above (call args, return statements, var-decl
        // initializers, struct/array/union field init). Two independent source shapes:
        //   - a POINTER to a type that implements the trait (directly, or indirectly via a
        //     composed trait it implements — see the two-tier search below), or
        //   - an existing TRAIT HANDLE whose own trait composes the expected trait (a
        //     handle-to-handle narrowing coercion).
        // See TraitCoercion/TraitHandleCoercion's doc comments in sema.hpp for why each is
        // recorded in its own side table rather than overwriting expr_types for this node.
        if (expected && expected->kind == TypeKind::Trait && ty.kind != TypeKind::Invalid && ty != *expected) {
            if (ty.kind == TypeKind::Trait) {
                // Handle-to-handle narrowing: zero runtime checks, no concrete-type knowledge
                // needed — the target's synthesized sub-vtable address is read straight out of
                // the source handle's own vtable's trailing slots at codegen time (see
                // component_vtables_ in codegen.cpp).
                if (const auto *from_info = program.trait_at(ty.trait_index)) {
                    const auto it = std::ranges::find_if(from_info->component_traits,
                        [&](const auto &c) { return c.trait_index == expected->trait_index; });
                    if (it != from_info->component_traits.end()) {
                        const int ordinal = static_cast<int>(std::distance(from_info->component_traits.begin(), it));
                        expr_tables_for_write(program, module_path).expr_trait_handle_coercions[get_expr_key(expr)] = TraitHandleCoercion{
                            .from_trait_index = ty.trait_index,
                            .to_trait_index = expected->trait_index,
                            .slot_index = static_cast<int>(from_info->methods.size()) + ordinal,
                        };
                        return *expected;
                    }
                }
                // Not a reachable composed trait: fall through to the caller's own
                // "expected X, got Y" mismatch error — NOT the pointer-required error below,
                // which would be misleading for a source that's already a trait handle.
            } else if (ty.kind != TypeKind::Pointer) {
                const auto [trait_module, trait_name] = find_type_module_and_name(*expected, program);
                return error(diag, get_expr_location(expr), std::format(
                    "cannot coerce non-pointer value to trait handle '{}'; a pointer to a type implementing the trait is required",
                    trait_name.empty() ? "<trait>" : trait_name));
            } else {
                const auto *pointee = program.pointee_at(ty.pointee_index);
                const auto [pointee_module, pointee_name] = pointee ? find_type_module_and_name(*pointee, program) : std::pair<std::string, std::string>{};

                std::vector<std::string> candidate_names;
                const int provider_trait_index = find_trait_provider(program, pointee_module, pointee_name,
                                                                       expected->trait_index, &candidate_names);
                if (candidate_names.size() > 1) {
                    const auto [trait_module, trait_name] = find_type_module_and_name(*expected, program);
                    return error(diag, get_expr_location(expr), std::format(
                        "ambiguous implicit coercion to trait '{}': type '{}' implements it via both '{}' and "
                        "'{}'; implement '{}' directly for '{}' to disambiguate",
                        trait_name.empty() ? "<trait>" : trait_name, pointee_name.empty() ? "<type>" : pointee_name,
                        candidate_names[0], candidate_names[1],
                        trait_name.empty() ? "<trait>" : trait_name, pointee_name.empty() ? "<type>" : pointee_name));
                }

                if (provider_trait_index < 0) {
                    const auto [trait_module, trait_name] = find_type_module_and_name(*expected, program);
                    return error(diag, get_expr_location(expr), std::format(
                        "type '{}' does not implement trait '{}'",
                        pointee_name.empty() ? "<type>" : pointee_name, trait_name.empty() ? "<trait>" : trait_name));
                }

                expr_tables_for_write(program, module_path).expr_trait_coercions[get_expr_key(expr)] = TraitCoercion{
                    .trait_index = expected->trait_index,
                    .provider_trait_index = provider_trait_index,
                };
                return *expected;
            }
        }

        // Implicit value-to-'any' coercion: applies through the same expected-type channel as
        // the coercions above. See AnyCoercion's doc comment in sema.hpp for why this is
        // recorded in a side table rather than overwriting expr_types for this node. Also
        // registers the source type's id (for 'type_of'/the runtime Type_Info table lookup) and
        // marks it as needing a 'Type_Info' global, so the generic runtime lookup path has real
        // entries to find.
        //
        // The source does NOT have to be addressable. 'any' erases to {id, data: anyptr}, so a
        // non-addressable source needs storage invented for it — which codegen does, in .rodata
        // for a compile-time constant and in a caller-frame temporary otherwise (see
        // emit_any_coercion). Requiring the user to write 'mut tmp := 42' first bought nothing:
        // it produced the identical stack slot with the identical lifetime, just noisier.
        //
        // Reads 'expected_in', not 'expected': this is the one tail that fires for an 'any'
        // expectation, and 'expected' was deliberately blanked for exactly that case above.
        if (expected_in && expected_in->kind == TypeKind::Any && ty.kind != TypeKind::Invalid && ty.kind != TypeKind::Any) {
            // A generic value coerced to 'any' inside a DECLARATION: the coercion itself is
            // fine and is re-checked per instantiation, but there is no concrete type to
            // register reflection data for. Accept and record nothing.
            if (contains_opaque(ty, program)) return *expected_in;

            // The one thing that genuinely cannot be erased: an expression that yields no value
            // at all, so there is nothing to point 'data' at. A void call used to be rejected
            // only as a side effect of the addressability rule.
            if (ty.kind == TypeKind::Void || ty.kind == TypeKind::Namespace) {
                return error(diag, get_expr_location(expr),
                    "cannot coerce a valueless expression to 'any'");
            }

            // Computed BEFORE the record below is inserted, and the order is load-bearing:
            // is_constant_expr_impl returns false for any node that already carries an
            // expr_any_coercions record (an any-coercion materializes a runtime {id, data}
            // value), so asking afterwards would always answer false.
            const bool source_is_constant = is_constant_expr(expr, module_path, program);

            const auto id = intern_type_id(program, ty);
            program.types_needing_info.insert(id);
            program.types_needing_info_types[id] = ty;
            expr_tables_for_write(program, module_path).expr_any_coercions[get_expr_key(expr)] =
                AnyCoercion{.source_type = ty, .source_is_constant = source_is_constant};
            return *expected_in;
        }

        return ty;
    }

    // Recursively looks for a bare, unqualified '.Variant' / '.Variant(payload)' reachable in
    // VALUE POSITION inside a 'return_err' operand — directly, or nested inside 'match' arm
    // bodies, or 'when'/ternary branches (recursively, so e.g. a match-in-a-when-branch also
    // works) — since all three already forward their incoming 'expected' type down into their
    // value sub-expressions (see their 'check_expr' cases). Returns nullptr if none is
    // reachable at all, meaning <expr> carries no return_err sugar for the caller to resolve.
    auto find_bare_return_err_variant_name(const ast::Expr &expr) -> const std::string * {
        if (const auto *di = std::get_if<ast::DotIdentExpr>(&expr)) {
            return &di->name;
        }
        if (const auto *tv = std::get_if<std::unique_ptr<ast::TaggedVariantExpr>>(&expr); tv && *tv && !(*tv)->type_path) {
            return &(*tv)->variant_name;
        }
        if (const auto *me = std::get_if<std::unique_ptr<ast::MatchExpr>>(&expr); me && *me) {
            for (const auto &arm : (*me)->arms) {
                if (const auto *name = find_bare_return_err_variant_name(arm.value)) {
                    return name;
                }
            }
        }
        if (const auto *te = std::get_if<std::unique_ptr<ast::TernaryExpr>>(&expr); te && *te) {
            if (const auto *name = find_bare_return_err_variant_name((*te)->then_expr)) return name;
            return find_bare_return_err_variant_name((*te)->else_expr);
        }
        if (const auto *we = std::get_if<std::unique_ptr<ast::WhenExpr>>(&expr); we && *we) {
            if (const auto *name = find_bare_return_err_variant_name((*we)->then_expr)) return name;
            return find_bare_return_err_variant_name((*we)->else_expr);
        }
        return nullptr;
    }

    // Resolves a '.Variant' / '.Variant(payload)' 'return_err' operand against the enclosing
    // function's error(...) type, returning the concrete error MEMBER type (e.g. MemoryError)
    // that <expr> denotes a variant of. The caller then passes this back into check_expr as
    // 'expected' — DotIdentExpr and TaggedVariantExpr resolve against it with no changes of
    // their own, exactly as they would for an ordinary bare enum/tagged-union return. Also looks
    // through 'match'/'when'/ternary wrapping via 'find_bare_return_err_variant_name' above, so
    // 'return_err match x { _: .Variant }' resolves the same way a bare 'return_err .Variant'
    // would. Returns nullopt (with no diagnostic) if <expr> doesn't reach a bare '.Variant' form
    // at all; the caller falls back to the general expression path in that case.
    auto resolve_return_err_member_type(const ast::Expr &error_value, const ResolvedType &fn_error_type,
                                         const std::string &module_path, Program &program, DiagnosticEngine &diag,
                                         const SourceLocation &loc) -> std::optional<ResolvedType> {
        const auto *wrapper = program.union_at(fn_error_type.union_index);
        if (!wrapper) return std::nullopt;
        const auto &members = wrapper->error_member_types;

        // Qualified form 'TypeName.variant{...}' already carries an explicit type_path from
        // parsing; just validate it names one of this error type's members and defer entirely
        // to the ordinary TaggedVariantExpr resolution path.
        if (const auto *tv = std::get_if<std::unique_ptr<ast::TaggedVariantExpr>>(&error_value); tv && (*tv)->type_path) {
            const auto qualified_ty = resolve_type(ast::Type{clone_named_type(*(*tv)->type_path)}, module_path, program, diag);
            if (!std::ranges::any_of(members, [&](const auto &m) { return m == qualified_ty; })) {
                diag.report_error(DiagnosticStage::Sema, loc,
                    std::format("'{}' is not a member of this function's error type", format_named_type(*(*tv)->type_path)));
                return std::nullopt;
            }
            return qualified_ty;
        }

        const auto *name_ptr = find_bare_return_err_variant_name(error_value);
        if (!name_ptr) {
            // No bare '.Variant' reachable anywhere in <expr> — the caller falls back to the
            // general expression path instead.
            return std::nullopt;
        }
        const std::string &variant_name = *name_ptr;

        std::vector<ResolvedType> matches;
        for (const auto &member : members) {
            if (member.kind == TypeKind::Enum) {
                if (const auto *einfo = program.enum_at(member.enum_index)) {
                    if (std::ranges::any_of(einfo->fields, [&](const auto &f) { return f.name == variant_name; })) {
                        matches.push_back(member);
                    }
                }
            } else if (member.kind == TypeKind::Union) {
                if (const auto *uinfo = program.union_at(member.union_index)) {
                    if (std::ranges::any_of(uinfo->variants, [&](const auto &variant) { return variant.name == variant_name; })) {
                        matches.push_back(member);
                    }
                }
            }
        }

        if (matches.empty()) {
            diag.report_error(DiagnosticStage::Sema, loc, std::format("'{}' is not a variant of this function's error type", variant_name));
            return std::nullopt;
        }
        if (matches.size() > 1) {
            std::string joined;
            for (size_t i = 0; i < matches.size(); ++i) {
                if (i > 0) joined += ", ";
                const auto [mod, name] = find_type_module_and_name(matches[i], program);
                joined += name.empty() ? "<type>" : name;
            }
            // NOTE: payload-free (enum) variants have no qualified-reference grammar today
            // ('TypeName.variant{...}' requires a tagged-union payload); an ambiguous
            // payload-free variant name can only be resolved by renaming one of the members.
            diag.report_error(DiagnosticStage::Sema, loc, std::format(
                "'{}' is ambiguous across error member types {}; tagged-union variants can be qualified "
                "as 'TypeName.{}{{...}}', payload-free enum variants must be renamed to disambiguate",
                variant_name, joined, variant_name));
            return std::nullopt;
        }
        return matches.front();
    }

    // What a recognized error-condition shape ('err', '!err', 'err && x', 'err || x',
    // '!err && x', '!err || x') narrows its subject variable to in the then/else branches.
    // 'is_exact_not_err' gates the early-return-narrowing rule below — per spec it applies
    // ONLY to a condition that is exactly '!err', not any compound form.

    // Matches 'err' or '!err' where 'err' is a bare identifier naming an error-typed local.
    // Returns the name and whether it was negated.
    auto match_error_operand(const ast::Expr &expr, LocalScope &locals, const Program &program)
        -> std::optional<std::pair<std::string, bool>> {
        if (const auto *ident = std::get_if<ast::IdentExpr>(&expr)) {
            if (find_error_local(ident->name, locals, program)) {
                return std::make_pair(ident->name, false);
            }
            return std::nullopt;
        }
        if (const auto *un = std::get_if<std::unique_ptr<ast::UnaryExpr>>(&expr)) {
            if ((*un)->op != ast::UnaryOp::LogicalNot) return std::nullopt;
            if (const auto *ident = std::get_if<ast::IdentExpr>(&(*un)->operand)) {
                if (find_error_local(ident->name, locals, program)) {
                    return std::make_pair(ident->name, true);
                }
            }
        }
        return std::nullopt;
    }

    // Appends every operand of a chain of 'op', flattened. Stops at anything that is not
    // that operator, so a nested '||' inside an '&&' chain (or vice versa) arrives as a
    // single opaque operand and is never descended into — which is correct, since what a
    // mixed condition proves depends on how it nests.
    void flatten_logical_chain(const ast::Expr &expr, const ast::BinaryOp op, std::vector<const ast::Expr *> &out) {
        if (const auto *bin = std::get_if<std::unique_ptr<ast::BinaryExpr>>(&expr)) {
            if ((*bin)->op == op) {
                flatten_logical_chain((*bin)->lhs, op, out);
                flatten_logical_chain((*bin)->rhs, op, out);
                return;
            }
        }
        out.push_back(&expr);
    }

    auto compute_condition_narrowing(const ast::Expr &condition, LocalScope &locals, const Program &program) -> std::vector<ConditionNarrowing> {
        // The two SIMPLE shapes. Only these set is_exact_err/is_exact_not_err, which gate the
        // early-return-narrowing rule and the redundant-check warning -- per spec neither
        // applies to a compound condition.
        if (const auto simple = match_error_operand(condition, locals, program)) {
            const auto &[name, negated] = *simple;
            if (negated) {
                return {ConditionNarrowing{name, ErrorState::Ok, ErrorState::Failed, false, true}};
            }
            return {ConditionNarrowing{name, ErrorState::Failed, ErrorState::Ok, true, false}};
        }

        if (const auto *bin = std::get_if<std::unique_ptr<ast::BinaryExpr>>(&condition)) {
            const auto &b = **bin;

            // '&&': every operand must hold for the then-branch, so EACH one that is a bare
            // 'err'/'!err' narrows that variable there. The else-branch learns nothing -- any
            // single operand being false is enough to take it.
            if (b.op == ast::BinaryOp::LogicalAnd) {
                std::vector<const ast::Expr *> operands;
                flatten_logical_chain(condition, ast::BinaryOp::LogicalAnd, operands);

                std::vector<ConditionNarrowing> result;
                for (const auto *operand : operands) {
                    const auto matched = match_error_operand(*operand, locals, program);
                    if (!matched) continue;
                    const auto &[name, negated] = *matched;
                    const auto state = negated ? ErrorState::Ok : ErrorState::Failed;

                    // A variable named twice in one chain: consistent repeats ('err && err')
                    // keep their state, contradictions ('err && !err' -- unreachable, but
                    // legal to write) degrade to Unknown rather than letting operand order
                    // decide.
                    const auto existing = std::ranges::find_if(result, [&](const auto &n) { return n.var_name == name; });
                    if (existing != result.end()) {
                        if (existing->then_state != state) existing->then_state = ErrorState::Unknown;
                        continue;
                    }
                    result.push_back(ConditionNarrowing{name, state, ErrorState::Unknown, false, false});
                }
                return result;
            }

            // '||': the mirror image of '&&'. Taking the THEN-branch means at least one
            // operand held, which says nothing about any particular one — so the then-branch
            // still proves nothing. But reaching the ELSE-branch means EVERY operand was
            // false, which is exactly as informative as '&&'\'s then-branch: each bare
            // 'err'/'!err' operand narrows there, to the negation of what it asserts.
            //
            //     if a || err  { ... } else { /* err is Ok here     */ }
            //     if a || !err { ... } else { /* err is Failed here */ }
            //
            // The then-state stays Unknown deliberately, and that is not a no-op: it DEGRADES
            // whatever was previously known, which is the conservative reading once a
            // variable's state has been branched on inconclusively.
            if (b.op == ast::BinaryOp::LogicalOr) {
                std::vector<const ast::Expr *> operands;
                flatten_logical_chain(condition, ast::BinaryOp::LogicalOr, operands);

                std::vector<ConditionNarrowing> result;
                for (const auto *operand : operands) {
                    const auto matched = match_error_operand(*operand, locals, program);
                    if (!matched) continue;
                    const auto &[name, negated] = *matched;
                    // Negated: reaching the else-branch means '!err' was false, i.e. err holds.
                    const auto else_state = negated ? ErrorState::Failed : ErrorState::Ok;

                    // Same reconciliation as the '&&' chain, on the other branch: a variable
                    // named twice consistently keeps its state, a contradiction ('err || !err'
                    // — its else-branch is unreachable, but legal to write) degrades to
                    // Unknown rather than letting operand order decide.
                    const auto existing = std::ranges::find_if(result, [&](const auto &n) { return n.var_name == name; });
                    if (existing != result.end()) {
                        if (existing->else_state != else_state) existing->else_state = ErrorState::Unknown;
                        continue;
                    }
                    // is_exact_* stay false: per spec the early-return rule and the
                    // redundant-check warning apply only to a bare 'err'/'!err' condition,
                    // never to a compound one.
                    result.push_back(ConditionNarrowing{name, ErrorState::Unknown, else_state, false, false});
                }
                return result;
            }
        }

        // Anything else proves nothing: the error variable behind a member access, a deref, a
        // call, a comparison, or nested under a '||'. condition_mentions_unnarrowable_error
        // below turns that into an actionable hint rather than a bare "unknown state".
        return {};
    }

    // Finds an error local mentioned anywhere in a condition, for the case where narrowing
    // produced nothing. Used only to explain WHY -- so it deliberately looks everywhere,
    // including the places the narrowing rules refuse to draw conclusions from.
    auto condition_mentions_unnarrowable_error(const ast::Expr &condition, LocalScope &locals, const Program &program) -> std::optional<std::string> {
        if (const auto matched = match_error_operand(condition, locals, program)) {
            return matched->first;
        }
        if (const auto *un = std::get_if<std::unique_ptr<ast::UnaryExpr>>(&condition)) {
            return condition_mentions_unnarrowable_error((*un)->operand, locals, program);
        }
        if (const auto *bin = std::get_if<std::unique_ptr<ast::BinaryExpr>>(&condition)) {
            if (const auto lhs = condition_mentions_unnarrowable_error((*bin)->lhs, locals, program)) return lhs;
            return condition_mentions_unnarrowable_error((*bin)->rhs, locals, program);
        }
        return std::nullopt;
    }

    // Whether evaluating 'expr' yields the capture pointer ITSELF, as opposed to reading
    // through it. 'v' does; 'v.*', 'v.field' and 'v[i]' do not -- those copy out of the
    // scratch slot while the arm is still live, which is exactly the safe usage.
    auto yields_capture_pointer(const ast::Expr &expr, const CaptureAliases &aliases) -> bool {
        if (const auto *ident = std::get_if<ast::IdentExpr>(&expr)) {
            return aliases.contains(ident->name);
        }
        // 'cast(v, anyptr)' launders the pointer but still yields it.
        if (const auto *cast = std::get_if<std::unique_ptr<ast::CastExpr>>(&expr)) {
            return yields_capture_pointer((*cast)->value, aliases);
        }
        // Either branch escaping is enough.
        if (const auto *ternary = std::get_if<std::unique_ptr<ast::TernaryExpr>>(&expr)) {
            return yields_capture_pointer((*ternary)->then_expr, aliases)
                || yields_capture_pointer((*ternary)->else_expr, aliases);
        }
        // '&v.field' derives from the capture and outlives the arm exactly as 'v' does.
        if (const auto *unary = std::get_if<std::unique_ptr<ast::UnaryExpr>>(&expr)) {
            if ((*unary)->op == ast::UnaryOp::AddressOf) {
                return yields_capture_pointer((*unary)->operand, aliases);
            }
        }
        if (const auto *member = std::get_if<std::unique_ptr<ast::MemberExpr>>(&expr)) {
            // Only under '&' — a bare 'v.field' is a COPY, which is the safe usage. Reached
            // here only via the AddressOf arm above.
            return yields_capture_pointer((*member)->object, aliases);
        }
        return false;
    }

    void report_capture_escape(const CaptureAliases &aliases, const SourceLocation &loc, const char *what, DiagnosticEngine &diag) {
        const auto &name = *aliases.begin();
        diag.warn(DiagnosticStage::Sema, loc, std::format(
            "by-ref payload capture '{}' escapes its arm via {}; the match operand is emitted as a "
            "value into one scratch slot per function, so '{}' points at a compiler temporary rather "
            "than the original object, and the next match through that slot overwrites it. Capture "
            "by value, or copy what you need out of '{}' before the arm ends",
            name, what, name, name));
    }

    auto check_payload_capture_escape_expr(const ast::Expr &value, const CaptureAliases &aliases, DiagnosticEngine &diag) -> void {
        std::visit([&]<typename T>(const T &node) {
            using U = std::decay_t<T>;
            if constexpr (std::is_same_v<U, std::unique_ptr<ast::CallExpr>>) {
                for (const auto &arg : node->args) {
                    if (yields_capture_pointer(arg, aliases)) {
                        report_capture_escape(aliases, node->location, "a call argument", diag);
                    }
                    check_payload_capture_escape_expr(arg, aliases, diag);
                }
                check_payload_capture_escape_expr(node->callee, aliases, diag);
            } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::AssignExpr>>) {
                if (yields_capture_pointer(node->value, aliases)) {
                    report_capture_escape(aliases, node->location, "an assignment", diag);
                }
                check_payload_capture_escape_expr(node->target, aliases, diag);
                check_payload_capture_escape_expr(node->value, aliases, diag);
            } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::BracedInitializerExpr>>) {
                std::visit([&]<typename V>(const V &alt) {
                    using W = std::decay_t<V>;
                    if constexpr (std::is_same_v<W, ast::StructExpr>) {
                        for (const auto &field : alt.fields) {
                            if (yields_capture_pointer(field.expr, aliases)) {
                                report_capture_escape(aliases, field.location, "a struct field", diag);
                            }
                            check_payload_capture_escape_expr(field.expr, aliases, diag);
                        }
                    } else if constexpr (std::is_same_v<W, ast::ArrayExpr>) {
                        for (const auto &element : alt.values) {
                            if (yields_capture_pointer(element, aliases)) {
                                report_capture_escape(aliases, alt.location, "an array element", diag);
                            }
                            check_payload_capture_escape_expr(element, aliases, diag);
                        }
                    }
                }, *node);
            } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::UnaryExpr>>) {
                check_payload_capture_escape_expr(node->operand, aliases, diag);
            } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::BinaryExpr>>) {
                check_payload_capture_escape_expr(node->lhs, aliases, diag);
                check_payload_capture_escape_expr(node->rhs, aliases, diag);
            } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::TernaryExpr>>) {
                check_payload_capture_escape_expr(node->condition, aliases, diag);
                check_payload_capture_escape_expr(node->then_expr, aliases, diag);
                check_payload_capture_escape_expr(node->else_expr, aliases, diag);
            } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::MemberExpr>>) {
                check_payload_capture_escape_expr(node->object, aliases, diag);
            } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::CastExpr>>) {
                check_payload_capture_escape_expr(node->value, aliases, diag);
            } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::TaggedVariantExpr>>) {
                // Constructing a variant whose payload holds the pointer carries it out
                // exactly as a struct literal field does — this arm was simply missing.
                for (const auto &field : node->payload.fields) {
                    if (yields_capture_pointer(field.expr, aliases)) {
                        report_capture_escape(aliases, field.location, "a variant payload", diag);
                    }
                    check_payload_capture_escape_expr(field.expr, aliases, diag);
                }
            } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::MatchExpr>>) {
                // A nested match shadowing the same capture name rebinds it; the outer
                // capture is no longer what the name refers to inside those arms.
                check_payload_capture_escape_expr(node->operand, aliases, diag);
                for (const auto &arm : node->arms) {
                    const auto *vp = std::get_if<ast::MatchExpr::VariantPattern>(&arm.pattern);
                    if (vp && vp->capture_name && aliases.contains(*vp->capture_name)) continue;
                    check_payload_capture_escape_expr(arm.value, aliases, diag);
                }
            }
            // Everything else either has no sub-expressions or cannot carry the pointer out.
        }, value);
    }

    auto check_payload_capture_escape(const ast::Stmt &body, CaptureAliases &aliases, DiagnosticEngine &diag) -> void {
        std::visit([&]<typename T>(const T &node) {
            using U = std::decay_t<T>;
            if constexpr (std::is_same_v<U, std::unique_ptr<ast::BlockStmt>>) {
                for (const auto &s : node->stmts) check_payload_capture_escape(s, aliases, diag);
            } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::IfStmt>>) {
                check_payload_capture_escape_expr(node->condition, aliases, diag);
                check_payload_capture_escape(node->then_stmt, aliases, diag);
                if (node->else_stmt) check_payload_capture_escape(*node->else_stmt, aliases, diag);
            } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::WhileStmt>>) {
                check_payload_capture_escape_expr(node->condition, aliases, diag);
                check_payload_capture_escape(node->body, aliases, diag);
            } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::ForInStmt>>) {
                check_payload_capture_escape_expr(node->iterable, aliases, diag);
                check_payload_capture_escape(node->body, aliases, diag);
            } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::SwitchStmt>>) {
                check_payload_capture_escape_expr(node->operand, aliases, diag);
                for (const auto &arm : node->arms) {
                    const auto *vp = std::get_if<ast::MatchExpr::VariantPattern>(&arm.pattern);
                    if (vp && vp->capture_name && aliases.contains(*vp->capture_name)) continue;
                    check_payload_capture_escape(arm.body, aliases, diag);
                }
            } else if constexpr (std::is_same_v<U, std::unique_ptr<ast::DeferStmt>>) {
                // A defer runs at scope exit, by which point the slot may already have been
                // reused -- so the pointer reaching one is an escape in its own right.
                check_payload_capture_escape(node->body, aliases, diag);
            } else if constexpr (std::is_same_v<U, ast::ExprStmt>) {
                check_payload_capture_escape_expr(node.expr, aliases, diag);
            } else if constexpr (std::is_same_v<U, ast::VarDeclStmt>) {
                if (node.init) {
                    check_payload_capture_escape_expr(*node.init, aliases, diag);
                    // THE under-report this check used to have: binding the capture to a local
                    // ('const p := v') is not itself an escape — 'p' is arm-local too — but
                    // whatever 'p' does next is. Follow the pointer instead of losing it here.
                    if (yields_capture_pointer(*node.init, aliases)) {
                        aliases.insert(node.name);
                    } else {
                        // A declaration of a tracked name that does NOT hold the capture
                        // shadows it; the name no longer refers to the pointer.
                        aliases.erase(node.name);
                    }
                }
            } else if constexpr (std::is_same_v<U, ast::VarDeclGroupStmt>) {
                check_payload_capture_escape_expr(node.init, aliases, diag);
            } else if constexpr (std::is_same_v<U, ast::ReturnStmt>) {
                for (const auto &e : node.return_values) {
                    if (yields_capture_pointer(e, aliases)) {
                        report_capture_escape(aliases, node.location, "a return", diag);
                    }
                    check_payload_capture_escape_expr(e, aliases, diag);
                }
            } else if constexpr (std::is_same_v<U, ast::ReturnOkStmt>) {
                for (const auto &e : node.return_values) {
                    if (yields_capture_pointer(e, aliases)) {
                        report_capture_escape(aliases, node.location, "a return", diag);
                    }
                    check_payload_capture_escape_expr(e, aliases, diag);
                }
            } else if constexpr (std::is_same_v<U, ast::ReturnErrStmt>) {
                if (yields_capture_pointer(node.error_value, aliases)) {
                    report_capture_escape(aliases, node.location, "a return", diag);
                }
                check_payload_capture_escape_expr(node.error_value, aliases, diag);
            }
        }, body);
    }

    auto apply_condition_narrowing(LocalScope &scope, const std::vector<ConditionNarrowing> &narrowings,
                                   const bool then_branch, const ast::Expr &condition, const Program &program) -> void {
        for (const auto &narrowing : narrowings) {
            if (auto *b = find_error_local(narrowing.var_name, scope, program)) {
                b->err_state = then_branch ? narrowing.then_state : narrowing.else_state;
                if (b->err_state == ErrorState::Unknown) {
                    b->err_unknown_reason = ErrorUnknownReason::UncheckedConditionShape;
                }
            }
        }

        // The condition named an error local somewhere the rules draw no conclusion from
        // (behind a '||', a member access, a deref, a call, a comparison). It stays Unknown
        // either way -- this only records that the condition's shape is the reason.
        if (const auto mentioned = condition_mentions_unnarrowable_error(condition, scope, program)) {
            if (auto *b = find_error_local(*mentioned, scope, program); b && b->err_state == ErrorState::Unknown) {
                b->err_unknown_reason = ErrorUnknownReason::UncheckedConditionShape;
            }
        }
    }

    // Merges two branch-end scopes into 'target' (the scope visible in code following the
    // branch): a variable's error typestate carries over only if both branches agree;
    // otherwise it degrades to Unknown. Non-error-tracked bindings (NotApplicable) are left
    // alone. Used for if/else (a/b = then/else-or-original) and while (a/b = pre-loop/
    // post-body, since the loop may run zero times).
    auto merge_error_states(LocalScope &target, const LocalScope &a, const LocalScope &b) -> void {
        for (auto &[name, binding] : target) {
            if (binding.err_state == ErrorState::NotApplicable) continue;
            const auto a_it = a.find(name);
            const auto b_it = b.find(name);
            const auto a_state = a_it != a.end() ? a_it->second.err_state : ErrorState::Unknown;
            const auto b_state = b_it != b.end() ? b_it->second.err_state : ErrorState::Unknown;
            if (a_state == b_state) {
                binding.err_state = a_state;
                if (a_it != a.end()) binding.err_unknown_reason = a_it->second.err_unknown_reason;
            } else {
                binding.err_state = ErrorState::Unknown;
                binding.err_unknown_reason = ErrorUnknownReason::BranchesDisagreed;
            }
        }
    }

    // True iff every path through 'stmt' definitely exits the enclosing function/loop
    // (return / return_ok / return_err / break / continue) — used by the early-return
    // narrowing rule ("if !err { <body> }" where <body> never falls through). Conservative:
    // an if/switch without a provably-exhaustive set of exiting arms counts as NOT
    // always-exiting, even if a human could prove otherwise.
    auto stmt_always_exits(const ast::Stmt &stmt) -> bool {
        return std::visit(
            [&]<typename T>(const T &v) -> bool {
                using V = std::decay_t<T>;
                if constexpr (std::is_same_v<V, std::unique_ptr<ast::BlockStmt>>) {
                    if (v->stmts.empty()) return false;
                    return stmt_always_exits(v->stmts.back());
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::IfStmt>>) {
                    return v->else_stmt.has_value() && stmt_always_exits(v->then_stmt) && stmt_always_exits(*v->else_stmt);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::SwitchStmt>>) {
                    bool has_default = false;
                    for (const auto &arm : v->arms) {
                        if (std::holds_alternative<ast::MatchExpr::DefaultPattern>(arm.pattern)) has_default = true;
                        if (!stmt_always_exits(arm.body)) return false;
                    }
                    return has_default;
                } else if constexpr (std::is_same_v<V, ast::ReturnStmt> || std::is_same_v<V, ast::ReturnErrStmt> ||
                                      std::is_same_v<V, ast::ReturnOkStmt> || std::is_same_v<V, ast::BreakStmt> ||
                                      std::is_same_v<V, ast::ContinueStmt>) {
                    return true;
                } else {
                    return false;
                }
            },
            stmt);
    }

    // 'when cond { ... } [else (when ... | { ... })]' as a statement. The condition must
    // be a compile-time constant (hard error otherwise); BOTH branches are always
    // type-checked (deliberate design choice — prevents platform-specific code from
    // silently rotting when working on a different target), each with its own isolated
    // copy of 'locals' (mirrors IfStmt's then/else isolation), but only the selected
    // branch's statements are ever emitted by codegen (see ProgramModule::when_stmt_selected).
    // Standalone (not folded into check_stmt's own visitor) so the 'else when' chain can
    // recurse directly on a dereferenced 'const ast::WhenStmt&' without needing to
    // reconstruct an ast::Stmt variant around a borrowed (non-owned) unique_ptr.
    void check_when_stmt(const ast::WhenStmt &when_stmt, LocalScope &locals, const std::string &module_path, Program &program, DiagnosticEngine &diag, const std::vector<ResolvedType> &expected_returns, const int loop_depth, const int defer_loop_base, const ResolvedType *fn_error_type) {
        check_expr(when_stmt.condition, locals, module_path, program, diag, ResolvedType{.kind = TypeKind::Bool}, loop_depth, defer_loop_base, fn_error_type);

        bool selected = false;
        if (!is_constant_expr(when_stmt.condition, module_path, program)) {
            diag.report_error(DiagnosticStage::Sema, when_stmt.location,
                "'when' condition must be a compile-time constant expression. "
                "Use 'if' for runtime conditions.");
        } else {
            // "Couldn't fold" must never be silently read as "folded to false": that is how
            // a 'when' with a macro-call condition once compiled the wrong branch with no
            // diagnostic. Any constant condition that fails to fold to an integer here is a
            // bug in the evaluator (or already carries its own type diagnostic).
            const auto folded = evaluate_const_value(when_stmt.condition, module_path, program, diag);
            if (const auto *iv = folded ? std::get_if<int64_t>(&*folded) : nullptr) {
                selected = (*iv != 0);
            } else if (!folded) {
                // A string-valued fold is not reported here: the Bool expected-type check
                // above has already diagnosed that condition.
                diag.report_error(DiagnosticStage::Sema, when_stmt.location,
                    "internal error: 'when' condition is a constant expression but could not be evaluated");
            }
        }

        expr_tables_for_write(program, module_path).when_stmt_selected[&when_stmt] = selected;

        auto then_locals = locals;
        for (auto &s : when_stmt.then_block.stmts) {
            check_stmt(s, then_locals, module_path, program, diag, expected_returns, loop_depth, defer_loop_base);
        }

        if (when_stmt.else_branch) {
            auto else_locals = locals;
            std::visit(
                [&]<typename EV>(const EV &else_v) {
                    using EVT = std::decay_t<EV>;
                    if constexpr (std::is_same_v<EVT, ast::BlockStmt>) {
                        for (auto &s : else_v.stmts) {
                            check_stmt(s, else_locals, module_path, program, diag, expected_returns, loop_depth, defer_loop_base);
                        }
                    } else { // std::unique_ptr<ast::WhenStmt>
                        check_when_stmt(*else_v, else_locals, module_path, program, diag, expected_returns, loop_depth, defer_loop_base, fn_error_type);
                    }
                },
                *when_stmt.else_branch);
        }
    }

    enum class AsmOperandDirection : uint8_t { Read, Write, ReadWrite };

    // Tier-1: explicit per-mnemonic operand-direction table (see spec.md's "Inline Assembly"
    // section). A mnemonic not listed here falls through to check_asm_stmt's Tier-2 fallback
    // (first operand read/write, remaining operands read) with a warning. Deliberately does NOT
    // include div/idiv/mul/imul — the spec lists those only under "implicit clobbers" below;
    // their own explicit operand (if any) still goes through the Tier-2 fallback path, only
    // the implicit rdx:rax reads/writes are unconditional.
    auto asm_tier1_directions(const std::string &mnemonic) -> const std::vector<AsmOperandDirection> * {
        static const std::unordered_map<std::string, std::vector<AsmOperandDirection>> table = {
            {"mov", {AsmOperandDirection::Write, AsmOperandDirection::Read}},
            // movzx deliberately allows (and expects) its two operands to have DIFFERENT
            // widths — zero-extending a narrower source into a wider destination is the
            // whole point of the instruction — so it's excluded from the generic
            // width-must-match check below and given its own "destination strictly wider
            // than source" validation instead.
            {"movzx", {AsmOperandDirection::Write, AsmOperandDirection::Read}},
            {"lea", {AsmOperandDirection::Write, AsmOperandDirection::Read}},
            {"add", {AsmOperandDirection::ReadWrite, AsmOperandDirection::Read}},
            {"sub", {AsmOperandDirection::ReadWrite, AsmOperandDirection::Read}},
            {"and", {AsmOperandDirection::ReadWrite, AsmOperandDirection::Read}},
            {"or", {AsmOperandDirection::ReadWrite, AsmOperandDirection::Read}},
            {"xor", {AsmOperandDirection::ReadWrite, AsmOperandDirection::Read}},
            {"not", {AsmOperandDirection::ReadWrite}},
            {"neg", {AsmOperandDirection::ReadWrite}},
            {"inc", {AsmOperandDirection::ReadWrite}},
            {"dec", {AsmOperandDirection::ReadWrite}},
            {"cmp", {AsmOperandDirection::Read, AsmOperandDirection::Read}},
            {"test", {AsmOperandDirection::Read, AsmOperandDirection::Read}},
            {"push", {AsmOperandDirection::Read}},
            {"pop", {AsmOperandDirection::Write}},
            {"nop", {}},
            {"ret", {}},
            {"jmp", {AsmOperandDirection::Read}},
            {"je", {AsmOperandDirection::Read}},
            {"jne", {AsmOperandDirection::Read}},
            {"jl", {AsmOperandDirection::Read}},
            {"jle", {AsmOperandDirection::Read}},
            {"jg", {AsmOperandDirection::Read}},
            {"jge", {AsmOperandDirection::Read}},
            {"ja", {AsmOperandDirection::Read}},
            {"jae", {AsmOperandDirection::Read}},
            {"jb", {AsmOperandDirection::Read}},
            {"jbe", {AsmOperandDirection::Read}},
            {"jz", {AsmOperandDirection::Read}},
            {"jnz", {AsmOperandDirection::Read}},
            {"syscall", {}},
            {"call", {AsmOperandDirection::Read}},
        };
        const auto it = table.find(mnemonic);
        return it == table.end() ? nullptr : &it->second;
    }

    // Registers implicitly clobbered (or read) by a mnemonic, independent of its explicit
    // operand list — see spec.md's "Implicit clobbers". Returns the 64-bit family names to add
    // to the clobber set unconditionally.
    auto asm_implicit_clobbers(const std::string &mnemonic, const size_t operand_count) -> std::vector<std::string> {
        if (mnemonic == "syscall") return {"rax", "rcx", "r11"};
        if (mnemonic == "call") return {"rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11"};
        if (mnemonic == "div" || mnemonic == "idiv") return {"rax", "rdx"};
        if ((mnemonic == "mul" || mnemonic == "imul") && operand_count == 1) return {"rax", "rdx"};
        return {};
    }

    // Shared per-instruction-list driver for both the 'asm { ... }' STATEMENT (check_asm_stmt,
    // keyed by AsmStmt) and the 'asm -> reg [: type] { ... }' EXPRESSION (check_asm_expr, keyed
    // by AsmExpr) forms — variable resolution, Tier-1/Tier-2 operand-direction + arity checking,
    // clobber-set construction, the 'movzx' exception, and the generic width-mismatch warning.
    // Does not touch any side table itself; callers store the returned AsmStmtInfo under
    // whichever key (AsmStmt*/AsmExpr*) is appropriate for their own node.
    auto check_asm_instructions(const std::vector<ast::AsmInstruction> &instructions, LocalScope &locals,
                                 const std::string &module_path, Program &program, DiagnosticEngine &diag) -> AsmStmtInfo {
        AsmStmtInfo info;

        for (const auto &instr : instructions) {
            AsmInstructionInfo instr_info;
            instr_info.operand_types.resize(instr.operands.size());

            // Variable resolution + scalar-only check.
            for (size_t i = 0; i < instr.operands.size(); ++i) {
                const auto *var = std::get_if<ast::AsmVariableOperand>(&instr.operands[i]);
                if (!var) {
                    continue;
                }

                const auto it = locals.find(var->name);
                if (it == locals.end()) {
                    diag.report_error(DiagnosticStage::Sema, var->location,
                        std::format("unknown identifier '{}' in asm block.", var->name));
                    continue;
                }

                if (!it->second.type.is_scalar()) {
                    diag.report_error(DiagnosticStage::Sema, var->location,
                        std::format("asm operand '{}' has type '{}', but only scalar and pointer "
                                    "types are supported in asm operands",
                                    var->name, describe_type(it->second.type, program)));
                    continue;
                }

                instr_info.operand_types[i] = it->second.type;
            }

            // Tier-1/Tier-2 operand direction + arity check.
            std::vector<AsmOperandDirection> directions;
            if (const auto *tier1 = asm_tier1_directions(instr.mnemonic)) {
                if (tier1->size() != instr.operands.size()) {
                    diag.report_error(DiagnosticStage::Sema, instr.location,
                        std::format("'{}' expects {} operand(s), got {}", instr.mnemonic, tier1->size(), instr.operands.size()));
                }
                directions = *tier1;
            } else {
                diag.warn(DiagnosticStage::Sema, instr.location,
                    std::format("unknown mnemonic '{}' — assuming first operand is read/write and "
                                "remaining operands are read. Verify clobbers manually.",
                                instr.mnemonic));
                directions.assign(instr.operands.size(), AsmOperandDirection::Read);
                if (!directions.empty()) {
                    directions[0] = AsmOperandDirection::ReadWrite;
                }
            }

            for (const auto &family : asm_implicit_clobbers(instr.mnemonic, instr.operands.size())) {
                info.clobbered_families.insert(family);
            }

            // Clobber-set construction: explicit register writes + '&var' memory writes.
            for (size_t i = 0; i < instr.operands.size() && i < directions.size(); ++i) {
                const auto dir = directions[i];
                if (const auto *reg = std::get_if<ast::AsmRegisterOperand>(&instr.operands[i])) {
                    if (dir == AsmOperandDirection::Write || dir == AsmOperandDirection::ReadWrite) {
                        if (const auto *reg_info = asm_registers::lookup_register(reg->name)) {
                            info.clobbered_families.insert(std::string(reg_info->family));
                        }
                    }
                } else if (const auto *var = std::get_if<ast::AsmVariableOperand>(&instr.operands[i]);
                           var && var->is_address) {
                    info.clobbers_memory = true;
                }
            }

            if (instr.mnemonic == "movzx") {
                // 'movzx dst, src' requires dst to be STRICTLY WIDER than src (that's the
                // entire point of a zero-extending move) — the opposite of every other
                // instruction's "widths must match" rule, so it gets its own check instead
                // of the generic pairwise comparison below.
                const auto operand_width = [&](const size_t idx) -> std::optional<uint32_t> {
                    if (idx >= instr.operands.size()) {
                        return std::nullopt;
                    }
                    if (const auto *reg = std::get_if<ast::AsmRegisterOperand>(&instr.operands[idx])) {
                        return reg->width_bits;
                    }
                    if (instr_info.operand_types[idx]) {
                        if (const auto w = scalar_bit_width(instr_info.operand_types[idx]->kind); w != 0) {
                            return w;
                        }
                    }
                    return std::nullopt;
                };

                const auto dst_width = operand_width(0);
                const auto src_width = operand_width(1);
                if (src_width && *src_width != 8 && *src_width != 16) {
                    // Real x86 has no 'movzx' encoding for a 32- or 64-bit source — widening a
                    // 32-bit value into a 64-bit register is what a plain 'mov' into the
                    // destination's 32-bit sub-register already does for free (writing a
                    // 32-bit register always zero-extends the upper 32 bits of its 64-bit
                    // parent), so there's nothing for 'movzx' to do in that case.
                    diag.report_error(DiagnosticStage::Sema, instr.location,
                        std::format("'movzx' source must be 8 or 16 bits (got {} bits) — there is "
                                    "no 'movzx' encoding for a 32-bit or 64-bit source; use 'mov' "
                                    "instead, which already zero-extends automatically when "
                                    "writing a 32-bit register",
                                    *src_width));
                } else if (dst_width && src_width && *dst_width <= *src_width) {
                    diag.report_error(DiagnosticStage::Sema, instr.location,
                        std::format("'movzx' destination width ({} bits) must be wider than source "
                                    "width ({} bits)",
                                    *dst_width, *src_width));
                }
            } else {
                // Width-mismatch check: every register/variable operand pair within this
                // instruction (normally exactly one of each, but this stays general).
                for (size_t i = 0; i < instr.operands.size(); ++i) {
                    const auto *var = std::get_if<ast::AsmVariableOperand>(&instr.operands[i]);
                    if (!var || !instr_info.operand_types[i]) {
                        continue;
                    }

                    const auto var_width = scalar_bit_width(instr_info.operand_types[i]->kind);
                    if (var_width == 0) {
                        continue;
                    }

                    for (size_t j = 0; j < instr.operands.size(); ++j) {
                        const auto *reg = std::get_if<ast::AsmRegisterOperand>(&instr.operands[j]);
                        if (!reg || reg->width_bits == var_width) {
                            continue;
                        }

                        const auto *reg_info = asm_registers::lookup_register(reg->name);
                        const auto *suggestion = reg_info
                            ? asm_registers::lookup_register_by_family_and_width(reg_info->family, var_width)
                            : nullptr;

                        diag.warn(DiagnosticStage::Sema, var->location,
                            std::format("asm operand '{}' has type '{}' ({} bits) but register '{}' is {} bits.{}",
                                        var->name, describe_type(*instr_info.operand_types[i], program), var_width,
                                        reg->name, reg->width_bits,
                                        suggestion ? std::format(" Consider using '{}' instead.", suggestion->name) : ""));
                    }
                }
            }

            info.instructions.push_back(std::move(instr_info));
        }

        return info;
    }

    // 'asm { ... }' inside a function body — delegates the entire per-instruction analysis to
    // check_asm_instructions above; only stores the result under this AsmStmt's key. (Module-scope
    // 'asm' is rejected entirely separately, in sema_declare.cpp's declare_one_decl — this
    // function only ever runs on a function-body 'asm', since check_stmt is only ever invoked
    // starting from a function/method body.)
    void check_asm_stmt(const ast::AsmStmt &stmt, LocalScope &locals, const std::string &module_path, Program &program, DiagnosticEngine &diag) {
        expr_tables_for_write(program, module_path).asm_stmt_info[&stmt] = check_asm_instructions(stmt.instructions, locals, module_path, program, diag);
    }

    // 'asm -> reg { ... }' / 'asm -> reg: type { ... }' — the expression form. Runs the same
    // instruction analysis as check_asm_stmt, then resolves the result type (explicit ': type'
    // wins; otherwise the surrounding 'expected' context; otherwise a hard error), checks the
    // result register's width against that type, and implicitly clobbers the register's family
    // even if no instruction in the block explicitly wrote it (the block's caller reads it as
    // the output, whether or not the body's instructions did).
    auto check_asm_expr(const ast::AsmExpr &expr, LocalScope &locals, const std::string &module_path,
                         Program &program, DiagnosticEngine &diag,
                         const std::optional<ResolvedType> &expected) -> ResolvedType {
        AsmStmtInfo info = check_asm_instructions(expr.instructions, locals, module_path, program, diag);

        ResolvedType result_ty{.kind = TypeKind::Invalid};
        if (expr.result_type) {
            result_ty = resolve_type(*expr.result_type, module_path, program, diag);
            if (result_ty.kind != TypeKind::Invalid && !result_ty.is_scalar()) {
                diag.report_error(DiagnosticStage::Sema, expr.location,
                    "asm result type must be a scalar or pointer type");
                result_ty = ResolvedType{.kind = TypeKind::Invalid};
            }
        } else if (expected) {
            // The context-inferred type gets the same rejection as the explicit ': type'
            // path above: 'const s: SomeStruct = asm -> rax { ... }' must not hand a
            // register-sized struct result to codegen.
            result_ty = *expected;
            if (result_ty.kind != TypeKind::Invalid && !result_ty.is_scalar()) {
                diag.report_error(DiagnosticStage::Sema, expr.location,
                    "asm result type must be a scalar or pointer type");
                result_ty = ResolvedType{.kind = TypeKind::Invalid};
            }
        } else {
            diag.report_error(DiagnosticStage::Sema, expr.location,
                std::format("cannot infer result type for 'asm -> {}'; add an explicit type "
                            "annotation: 'asm -> {}: i32 {{ ... }}' or annotate the variable "
                            "receiving the result.",
                            expr.result_register.name, expr.result_register.name));
        }

        if (result_ty.kind != TypeKind::Invalid) {
            if (const auto ty_width = scalar_bit_width(result_ty.kind);
                ty_width != 0 && ty_width != expr.result_register.width_bits) {
                const auto *reg_info = asm_registers::lookup_register(expr.result_register.name);
                const auto *suggestion = reg_info
                    ? asm_registers::lookup_register_by_family_and_width(reg_info->family, ty_width)
                    : nullptr;
                diag.warn(DiagnosticStage::Sema, expr.location,
                    std::format("asm result register '{}' is {} bits but result type '{}' is {} bits.{}",
                                expr.result_register.name, expr.result_register.width_bits,
                                describe_type(result_ty, program), ty_width,
                                suggestion ? std::format(" Consider using '{}' to avoid implicit truncation.", suggestion->name) : ""));
            }
        }

        // The result register is always implicitly read as the block's output, whether or not
        // any instruction explicitly wrote it — std::set::insert on an already-present family
        // is a no-op, so no "was it already written?" check is needed here.
        if (const auto *reg_info = asm_registers::lookup_register(expr.result_register.name)) {
            info.clobbered_families.insert(std::string(reg_info->family));
        }

        expr_tables_for_write(program, module_path).asm_expr_info[&expr] = std::move(info);
        return result_ty;
    }

    void check_var_decl_group_stmt(const ast::VarDeclGroupStmt &v, LocalScope &locals, const std::string &module_path, Program &program, DiagnosticEngine &diag, const std::vector<ResolvedType> &expected_returns, const int loop_depth, const int defer_loop_base, const ResolvedType *fn_error_type) {
        const ast::CallExpr *call = nullptr;
        bool is_try = false;

        if (const auto *c = std::get_if<std::unique_ptr<ast::CallExpr>>(&v.init)) {
            call = c->get();
        } else if (const auto *t = std::get_if<std::unique_ptr<ast::TryExpr>>(&v.init)) {
            is_try = true;
            const auto *inner_call = std::get_if<std::unique_ptr<ast::CallExpr>>(&(*t)->call);
            if (!inner_call) {
                diag.report_error(DiagnosticStage::Sema, v.location, "'try' operand must be a direct function call");
                return;
            }
            call = inner_call->get();
        }

        if (!call) {
            diag.report_error(DiagnosticStage::Sema, v.location, "group declaration initializer must be a function call or 'try' expression");
            check_expr(v.init, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_error_type);
            return;
        }

        auto returns = check_group_call_returns(*call, locals, module_path, program, diag, loop_depth, defer_loop_base, fn_error_type);
        if (returns.empty()) {
            return;
        }

        if (is_try) {
            // 'try a, b := self.helper()' inside a generic impl method: Opaque returns,
            // no error union to recognize yet — the same tolerance the expression form
            // has. Bind the names Opaque and let the instantiation check for real.
            if (returns.back().kind == TypeKind::Opaque) {
                for (const auto &name : v.names) {
                    if (!name.empty() && name != "_") {
                        locals[name] = LocalBinding{.type = ResolvedType{.kind = TypeKind::Opaque}, .is_mut = v.is_mut};
                    }
                }
                return;
            }
            // Check enclosing function is fallible
            if (!fn_error_type) {
                diag.report_error(DiagnosticStage::Sema, v.location,
                    "enclosing function must return 'error(...)' to use 'try'");
            }
            // Strip the error slot from returns
            if (is_error_union_type(returns.back(), program)) {
                if (fn_error_type && !error_union_is_subset(returns.back(), *fn_error_type, program)) {
                    diag.report_error(DiagnosticStage::Sema, v.location,
                        "callee's error type is not a subset of the enclosing function's error type; "
                        "widen the enclosing function's 'error(...)' return type or handle the error explicitly");
                }
                returns.pop_back();
            } else {
                diag.report_error(DiagnosticStage::Sema, v.location,
                    "'try' can only be used on a function that returns 'error(...)' as its last return value");
                return;
            }
            // Check that we are not inside a defer body
            if (defer_loop_base >= 0) {
                diag.report_error(DiagnosticStage::Sema, v.location,
                    "'try' cannot propagate errors out of a 'defer' body");
            }
        }

        // One name short of the arity, with a '?error(...)' in the trailing slot:
        // the caller is dropping the ignorable error, so record it for codegen
        // rather than reporting an arity mismatch. Naming it — including naming
        // it '_' — takes this branch's 'else' and stays a silent discard, which
        // is the documented opt-out. 'try' already consumed the slot above.
        if (!is_try && returns.size() == v.names.size() + 1 && is_optional_error_union(returns.back(), program)) {
            expr_tables_for_write(program, module_path).call_dropped_optional_error[call] =
                DroppedOptionalError{.error_type = returns.back(), .slot_index = returns.size() - 1};
            returns.pop_back();
        }

        if (returns.size() != v.names.size()) {
            diag.report_error(DiagnosticStage::Sema, v.location, std::format("group declaration expects {} return value(s), got {}", v.names.size(), returns.size()));
        }

        for (size_t i = 0; i < v.names.size() && i < returns.size(); ++i) {
            if (!v.names[i].empty() && v.names[i] != "_") {
                locals[v.names[i]] = LocalBinding{.type = returns[i], .is_mut = v.is_mut,
                    .err_state = is_error_union_type(returns[i], program) ? ErrorState::Unknown : ErrorState::NotApplicable};
            }
        }

    }

    void check_return_stmt(const ast::ReturnStmt &v, LocalScope &locals, const std::string &module_path, Program &program, DiagnosticEngine &diag, const std::vector<ResolvedType> &expected_returns, const int loop_depth, const int defer_loop_base, const ResolvedType *fn_error_type) {
        if (defer_loop_base >= 0) {
            diag.report_error(DiagnosticStage::Sema, v.location, "'return' cannot escape a 'defer' body");
            return;
        }
        if (v.return_values.size() == 1 && expected_returns.size() > 1) {
            if (const auto *call = std::get_if<std::unique_ptr<ast::CallExpr>>(&v.return_values[0])) {
                auto returns = check_group_call_returns(**call, locals, module_path, program, diag, loop_depth, defer_loop_base, fn_error_type);
                // One slot over the arity with a trailing '?error(...)': the forward drops
                // the ignorable error, recorded for codegen's failure check — the same rule
                // group declarations and single-value contexts already apply. This path
                // used to compare exact arity only, so 'return f()' was rejected where
                // 'const a, b := f()' was accepted for the same callee.
                if (returns.size() == expected_returns.size() + 1 && is_optional_error_union(returns.back(), program)) {
                    expr_tables_for_write(program, module_path).call_dropped_optional_error[call->get()] =
                        DroppedOptionalError{.error_type = returns.back(), .slot_index = returns.size() - 1};
                    returns.pop_back();
                }
                if (returns.size() != expected_returns.size()) {
                    diag.report_error(DiagnosticStage::Sema, v.location,
                                      std::format("expected {} return value(s), got {}", expected_returns.size(), returns.size()));
                    return;
                }
                for (size_t i = 0; i < returns.size(); ++i) {
                    if (!assignable_in_module(returns[i], expected_returns[i], module_path, program)) {
                        diag.report_error(DiagnosticStage::Sema, v.location, std::format("return value {} type mismatch", i + 1));
                    }
                }
                return;
            }
        }
        if (v.return_values.size() != expected_returns.size()) {
            diag.report_error(DiagnosticStage::Sema, v.location,
                              std::format("expected {} return value(s), got {}{}", expected_returns.size(), v.return_values.size(),
                                          v.possible_asi_gotcha
                                              ? " (note: 'return' on its own line ends the statement immediately; move the value onto the same line as 'return')"
                                              : ""));
            for (auto &val : v.return_values)
                check_expr(val, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_error_type);
            return;
        }
        for (size_t i = 0; i < v.return_values.size(); ++i) {
            // Last return slot of a function returning 'error(...)': a bare
            // '.Variant' / '.Variant(payload)' (or one reachable through
            // 'match'/'when'/ternary) needs the same unwrap 'return_err' does,
            // since 'expected_returns[i]' here is the outer Ok/Failed wrapper,
            // whose own variants are always just "Ok"/"Failed" — never the
            // wrapped enum/union's names. A value that's already fully typed as
            // the wrapper (e.g. forwarding another call's error(...) result)
            // falls through to the ordinary path below unchanged.
            if (fn_error_type && i == v.return_values.size() - 1) {
                if (const auto member_type = resolve_return_err_member_type(
                        v.return_values[i], *fn_error_type, module_path, program, diag, v.location)) {
                    auto ty = check_expr(v.return_values[i], locals, module_path, program, diag,
                                          *member_type, loop_depth, defer_loop_base, fn_error_type);
                    if (!assignable_in_module(ty, *member_type, module_path, program)) {
                        diag.report_error(DiagnosticStage::Sema, v.location, std::format("return value {} type mismatch", i + 1));
                    }
                    continue;
                }
                if (find_bare_return_err_variant_name(v.return_values[i])) {
                    // resolve_return_err_member_type already reported a diagnostic
                    // for this sugar form (unknown/ambiguous variant, etc).
                    check_expr(v.return_values[i], locals, module_path, program, diag,
                               std::nullopt, loop_depth, defer_loop_base, fn_error_type);
                    continue;
                }
            }
            auto ty = check_expr(v.return_values[i], locals, module_path, program, diag, expected_returns[i], loop_depth, defer_loop_base, fn_error_type);
            if (!assignable_in_module(ty, expected_returns[i], module_path, program)) {
                diag.report_error(DiagnosticStage::Sema, v.location, std::format("return value {} type mismatch", i + 1));
            }
        }

    }

    void check_return_err_stmt(const ast::ReturnErrStmt &v, LocalScope &locals, const std::string &module_path, Program &program, DiagnosticEngine &diag, const std::vector<ResolvedType> &expected_returns, const int loop_depth, const int defer_loop_base, const ResolvedType *fn_error_type) {
        if (defer_loop_base >= 0) {
            diag.report_error(DiagnosticStage::Sema, v.location, "'return_err' cannot escape a 'defer' body");
            return;
        }
        if (!fn_error_type) {
            diag.report_error(DiagnosticStage::Sema, v.location,
                              "enclosing function must return 'error(...)' to use 'return_err'");
            check_expr(v.error_value, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_error_type);
            return;
        }
        // '.Variant' / '.Variant(payload)' — bare, or reachable through 'match'/
        // 'when'/ternary wrapping — need the expected member type fed in to resolve
        // at all (see resolve_return_err_member_type / find_bare_return_err_variant_name);
        // anything else is an ordinary expression that already carries its own type,
        // so check it with no expected type and classify what came back.
        if (const auto member_type = resolve_return_err_member_type(v.error_value, *fn_error_type, module_path, program, diag, v.location)) {
            auto ty = check_expr(v.error_value, locals, module_path, program, diag, *member_type, loop_depth, defer_loop_base, fn_error_type);
            if (!assignable_in_module(ty, *member_type, module_path, program)) {
                diag.report_error(DiagnosticStage::Sema, v.location, "'return_err' operand does not match the resolved error member type");
            }
        } else if (find_bare_return_err_variant_name(v.error_value)) {
            // resolve_return_err_member_type already reported a diagnostic for this
            // sugar form (unknown/ambiguous variant, etc), whether bare at the top
            // level or nested inside e.g. a 'match' arm.
            check_expr(v.error_value, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_error_type);
        } else {
            auto ty = check_expr(v.error_value, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_error_type);
            if (ty.kind == TypeKind::Invalid) {
                // check_expr already reported a diagnostic.
            } else if (is_error_union_type(ty, program)) {
                // Already a full error(...) value — this function's own, or (like
                // 'try') a subset of it; propagated as-is by codegen, translating
                // tags if the two unions differ.
                if (!error_union_is_subset(ty, *fn_error_type, program)) {
                    diag.report_error(DiagnosticStage::Sema, v.location,
                        "'return_err' operand's error type is not a subset of the enclosing function's error type; "
                        "widen the enclosing function's 'error(...)' return type or handle the error explicitly");
                }
            } else {
                const auto *wrapper = program.union_at(fn_error_type->union_index);
                const bool is_member = wrapper && std::ranges::any_of(
                    wrapper->error_member_types, [&](const auto &m) { return m == ty; });
                if (!is_member) {
                    diag.report_error(DiagnosticStage::Sema, v.location,
                        "'return_err' operand must be a '.Variant', '.Variant(payload)', a value of one of the "
                        "enclosing function's error member types, or a compatible 'error(...)' value");
                }
            }
        }

    }

    auto check_stmt(const ast::Stmt &stmt, LocalScope &locals, const std::string &module_path, Program &program, DiagnosticEngine &diag, const std::vector<ResolvedType> &expected_returns, int loop_depth, int defer_loop_base) -> void {
        const ResolvedType *fn_error_type = (!expected_returns.empty() && is_error_union_type(expected_returns.back(), program))
            ? &expected_returns.back() : nullptr;
        std::visit(
            [&]<typename T>(const T &v) {
                using V = std::decay_t<T>;

                if constexpr (std::is_same_v<V, std::unique_ptr<ast::BlockStmt>>) {
                    auto inner = locals;
                    for (auto &s : v->stmts)
                        check_stmt(s, inner, module_path, program, diag, expected_returns, loop_depth, defer_loop_base);

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::IfStmt>>) {
                    check_expr(v->condition, locals, module_path, program, diag, ResolvedType{.kind = TypeKind::Bool}, loop_depth, defer_loop_base, fn_error_type);

                    const auto narrowings = compute_condition_narrowing(v->condition, locals, program);

                    for (const auto &narrowing : narrowings) {
                        if (const auto *binding = find_error_local(narrowing.var_name, locals, program)) {
                            if (narrowing.is_exact_err && binding->err_state == ErrorState::Failed) {
                                diag.warn(DiagnosticStage::Sema, v->location, std::format(
                                    "redundant error check: '{}' is already known to be Failed here", narrowing.var_name));
                            } else if (narrowing.is_exact_not_err && binding->err_state == ErrorState::Ok) {
                                diag.warn(DiagnosticStage::Sema, v->location, std::format(
                                    "redundant error check: '{}' is already known to be Ok here", narrowing.var_name));
                            }
                        }
                    }

                    // then/else each get their OWN copy of locals — isolation is explicit here
                    // rather than relying on then_stmt/else_stmt happening to be BlockStmts
                    // (a bare non-block then-stmt is grammar-legal and would otherwise leak
                    // mutations into the other branch / the enclosing scope).
                    auto then_locals = locals;
                    apply_condition_narrowing(then_locals, narrowings, true, v->condition, program);
                    check_stmt(v->then_stmt, then_locals, module_path, program, diag, expected_returns, loop_depth, defer_loop_base);

                    auto else_locals = locals;
                    apply_condition_narrowing(else_locals, narrowings, false, v->condition, program);
                    if (v->else_stmt) check_stmt(*v->else_stmt, else_locals, module_path, program, diag, expected_returns, loop_depth, defer_loop_base);

                    // Early-return narrowing: 'if !err { <body that always exits> }' with no
                    // else strengthens the post-if state to Failed (the only path that
                    // reaches here is the condition being false, i.e. err was Failed) — this
                    // overrides what the general join below would otherwise conclude.
                    // is_exact_not_err is only ever set by the simple '!err' shape, so this
                    // list has exactly one entry whenever the rule applies.
                    const auto exact_not_err = std::ranges::find_if(narrowings, [](const auto &n) { return n.is_exact_not_err; });
                    if (exact_not_err != narrowings.end() && !v->else_stmt && stmt_always_exits(v->then_stmt)) {
                        if (auto *b = find_error_local(exact_not_err->var_name, locals, program)) {
                            b->err_state = ErrorState::Failed;
                        }
                    } else {
                        merge_error_states(locals, then_locals, else_locals);
                    }

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::WhileStmt>>) {
                    check_expr(v->condition, locals, module_path, program, diag, ResolvedType{.kind = TypeKind::Bool}, loop_depth, defer_loop_base, fn_error_type);

                    const auto narrowings = compute_condition_narrowing(v->condition, locals, program);
                    auto body_locals = locals;
                    apply_condition_narrowing(body_locals, narrowings, true, v->condition, program);
                    check_stmt(v->body, body_locals, module_path, program, diag, expected_returns, loop_depth + 1, defer_loop_base);

                    // The loop may run zero times, so code after it only knows what pre-loop
                    // and post-body states agree on (single-pass — no fixed-point widening).
                    merge_error_states(locals, locals, body_locals);

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::ForInStmt>>) {
                    check_for_in_stmt(v, locals, module_path, program, diag, expected_returns, loop_depth, defer_loop_base, fn_error_type);
                } else if constexpr (std::is_same_v<V, ast::ExprStmt>) {
                    const auto expr_ty = check_expr(v.expr, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_error_type);
                    // Detect ignored errors from fallible calls. An ASSIGNMENT whose target
                    // is error-typed is exempt: its statement-level "result" is the value
                    // just stored into the (error-tracked) target, not an ignored error —
                    // rejecting it made the 'err = match err { ... }' remap idiom (and even
                    // 'err = other') impossible to write.
                    if (is_error_union_type(expr_ty, program) &&
                        !std::holds_alternative<std::unique_ptr<ast::TryExpr>>(v.expr) &&
                        !std::holds_alternative<std::unique_ptr<ast::AssignExpr>>(v.expr)) {
                        // A call to a '?error(...)' function is allowed to ignore it here —
                        // that IS the feature — but the check still has to happen, so record
                        // the omission for codegen. This is the only place a lone '?E' return
                        // (a call with no other return values) can be dropped; in every value
                        // context single_value_result keeps it, since there the error is the
                        // expression's value. Gated on the node being a call: a bare 'e'
                        // statement naming a '?error(...)' LOCAL has no call site to attach a
                        // check to, so it keeps the diagnostic below.
                        const auto *call = std::get_if<std::unique_ptr<ast::CallExpr>>(&v.expr);
                        if (call && is_optional_error_union(expr_ty, program)) {
                            expr_tables_for_write(program, module_path).call_dropped_optional_error[call->get()] =
                                DroppedOptionalError{.error_type = expr_ty, .slot_index = 0};
                        } else {
                            diag.report_error(DiagnosticStage::Sema, v.location,
                                "error from fallible function call must be captured or propagated with 'try'");
                        }
                    }

                } else if constexpr (std::is_same_v<V, ast::VarDeclStmt>) {
                    ResolvedType declared_ty{.kind = TypeKind::Void};
                    bool has_declared_ty = false;
                    if (const auto resolved = resolve_declared_type(v.type, v.init, module_path, program, diag, v.location)) {
                        declared_ty = *resolved;
                        has_declared_ty = true;
                    }
                    if (v.init) {
                        if (!v.is_mut && contains_undefined(*v.init)) {
                            diag.report_error(DiagnosticStage::Sema, v.location, "'undefined' is not allowed in a 'const' declaration");
                        }
                        auto init_ty = check_expr(*v.init, locals, module_path, program, diag,
                                                  has_declared_ty ? std::optional(declared_ty) : std::nullopt, loop_depth, defer_loop_base, fn_error_type);
                        if (has_declared_ty && !assignable_in_module(init_ty, declared_ty, module_path, program)) {
                            diag.report_error(DiagnosticStage::Sema, v.location, "type mismatch in variable declaration");
                        }
                        const auto &bound_ty = has_declared_ty ? declared_ty : init_ty;
                        locals[v.name] = LocalBinding{.type = bound_ty, .is_mut = v.is_mut,
                            .err_state = is_error_union_type(bound_ty, program) ? ErrorState::Unknown : ErrorState::NotApplicable};
                    } else {
                        if (!v.is_mut) diag.report_error(DiagnosticStage::Sema, v.location, "'const' requires an initializer");
                        if (!has_declared_ty) diag.report_error(DiagnosticStage::Sema, v.location, "cannot infer type with no initializer and no type annotation");
                        locals[v.name] = LocalBinding{.type = declared_ty, .is_mut = v.is_mut,
                            .err_state = is_error_union_type(declared_ty, program) ? ErrorState::Unknown : ErrorState::NotApplicable};
                    }

                } else if constexpr (std::is_same_v<V, ast::VarDeclGroupStmt>) {
                    check_var_decl_group_stmt(v, locals, module_path, program, diag, expected_returns, loop_depth, defer_loop_base, fn_error_type);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::SwitchStmt>>) {
                    check_switch_stmt(v, locals, module_path, program, diag, expected_returns, loop_depth, defer_loop_base, fn_error_type);
                } else if constexpr (std::is_same_v<V, ast::ContinueStmt>) {
                    if (loop_depth == 0) {
                        diag.report_error(DiagnosticStage::Sema, v.location, "'continue' outside of a loop");
                    } else if (defer_loop_base >= 0 && loop_depth <= defer_loop_base) {
                        diag.report_error(DiagnosticStage::Sema, v.location, "'continue' cannot escape a 'defer' body");
                    }

                } else if constexpr (std::is_same_v<V, ast::BreakStmt>) {
                    if (loop_depth == 0) {
                        diag.report_error(DiagnosticStage::Sema, v.location, "'break' outside of a loop");
                    } else if (defer_loop_base >= 0 && loop_depth <= defer_loop_base) {
                        diag.report_error(DiagnosticStage::Sema, v.location, "'break' cannot escape a 'defer' body");
                    }

                } else if constexpr (std::is_same_v<V, ast::ReturnStmt>) {
                    check_return_stmt(v, locals, module_path, program, diag, expected_returns, loop_depth, defer_loop_base, fn_error_type);
                } else if constexpr (std::is_same_v<V, ast::ReturnErrStmt>) {
                    check_return_err_stmt(v, locals, module_path, program, diag, expected_returns, loop_depth, defer_loop_base, fn_error_type);
                } else if constexpr (std::is_same_v<V, ast::ReturnOkStmt>) {
                    if (defer_loop_base >= 0) {
                        diag.report_error(DiagnosticStage::Sema, v.location, "'return_ok' cannot escape a 'defer' body");
                        return;
                    }
                    if (!fn_error_type) {
                        diag.report_error(DiagnosticStage::Sema, v.location,
                                          "enclosing function must return 'error(...)' to use 'return_ok'");
                        for (auto &val : v.return_values)
                            check_expr(val, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_error_type);
                        return;
                    }
                    const size_t expected_count = expected_returns.size() - 1;
                    if (v.return_values.size() != expected_count) {
                        diag.report_error(DiagnosticStage::Sema, v.location,
                                          std::format("expected {} return value(s), got {}{}", expected_count, v.return_values.size(),
                                                      v.possible_asi_gotcha
                                                          ? " (note: 'return_ok' on its own line ends the statement immediately; move the value onto the same line as 'return_ok')"
                                                          : ""));
                        for (auto &val : v.return_values)
                            check_expr(val, locals, module_path, program, diag, std::nullopt, loop_depth, defer_loop_base, fn_error_type);
                        return;
                    }
                    for (size_t i = 0; i < v.return_values.size(); ++i) {
                        auto ty = check_expr(v.return_values[i], locals, module_path, program, diag, expected_returns[i], loop_depth, defer_loop_base, fn_error_type);
                        if (!assignable_in_module(ty, expected_returns[i], module_path, program)) {
                            diag.report_error(DiagnosticStage::Sema, v.location, std::format("return value {} type mismatch", i + 1));
                        }
                    }

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::DeferStmt>>) {
                    // Register defer: validate the defer body with defer_loop_base = current loop_depth.
                    // Inside the defer body, return/try are forbidden; break/continue only allowed for loops
                    // fully inside the defer body.
                    check_stmt(v->body, locals, module_path, program, diag, expected_returns, loop_depth, loop_depth);

                } else if constexpr (std::is_same_v<V, ast::LinkDecl>) {
                    // The parser accepts '#link(...)' here purely so this diagnostic can name
                    // the exact construct; it is never legal inside a function body.
                    diag.report_error(DiagnosticStage::Sema, v.location,
                        "'#link' is a linker directive and may only appear at module scope "
                        "or inside a module-scope 'when' block.");

                } else if constexpr (std::is_same_v<V, ast::DiagnosticDecl>) {
                    // Same reasoning as '#link' just above: parses here only so this
                    // diagnostic can name the exact construct.
                    diag.report_error(DiagnosticStage::Sema, v.location,
                        std::format("'#{}' is a compile-time diagnostic directive and may only "
                                    "appear at module scope or inside a module-scope 'when' block.",
                                    v.kind == ast::DiagnosticDirectiveKind::Error ? "error" : "warn"));

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::WhenStmt>>) {
                    check_when_stmt(*v, locals, module_path, program, diag, expected_returns, loop_depth, defer_loop_base, fn_error_type);

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::AsmStmt>>) {
                    check_asm_stmt(*v, locals, module_path, program, diag);
                }
            },
            stmt);
    }
}
