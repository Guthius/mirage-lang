#include "sema.hpp"

#include <filesystem>
#include <algorithm>
#include <format>
#include <functional>
#include <map>
#include <ranges>
#include <set>

namespace sema {
    auto find_attribute(const std::vector<ast::Attribute> &attrs, const std::string_view name) -> const ast::Attribute * {
        for (const auto &attr : attrs) {
            if (attr.name == name) return &attr;
        }
        return nullptr;
    }

    namespace {
        auto is_void_or_error_return(const std::vector<ResolvedType> &return_types, const Program &program) -> bool {
            if (return_types.empty()) return true;
            if (return_types.size() != 1) return false;
            const auto &rt = return_types.front();
            if (rt.kind != TypeKind::Union) return false;
            const auto *info = program.union_at(rt.union_index);
            return info != nullptr && info->is_error_union;
        }

        // Mirrors sema::get_expr_location's by-value/boxed-alternative handling, but for Stmt.
        auto stmt_location(const ast::Stmt &stmt) -> SourceLocation {
            return std::visit([](const auto &v) -> SourceLocation {
                if constexpr (requires { v->location; }) {
                    return v->location;
                } else {
                    return v.location;
                }
            }, stmt);
        }

        // '@naked' requires the function body to contain only inline-asm statements — purely
        // structural, unrelated to check_asm_instructions/check_asm_stmt's semantic validation
        // (sema_check.cpp). Returns the first offending statement, or nullptr if the body is
        // all-asm (or isn't a block at all, which shouldn't happen for a real function body).
        auto find_first_non_asm_stmt(const ast::Stmt &body) -> const ast::Stmt * {
            const auto *block = std::get_if<std::unique_ptr<ast::BlockStmt>>(&body);
            if (!block) return nullptr;
            for (const auto &stmt : (*block)->stmts) {
                if (!std::holds_alternative<std::unique_ptr<ast::AsmStmt>>(stmt)) {
                    return &stmt;
                }
            }
            return nullptr;
        }

        void validate_no_return_attribute(const ast::Attribute &attr, const std::vector<ResolvedType> &return_types, const Program &program, DiagnosticEngine &diag) {
            if (!attr.args.empty()) {
                diag.report_error(DiagnosticStage::Sema, attr.location, "'@no_return' takes no arguments");
            }
            if (!is_void_or_error_return(return_types, program)) {
                diag.warn(DiagnosticStage::Sema, attr.location,
                    "a '@no_return' function with a value return type will never return its value");
            }
        }

        void validate_naked_attribute(const ast::Attribute &attr, const ast::Stmt &body, DiagnosticEngine &diag) {
            if (!attr.args.empty()) {
                diag.report_error(DiagnosticStage::Sema, attr.location, "'@naked' takes no arguments");
            }
            if (const auto *bad_stmt = find_first_non_asm_stmt(body)) {
                diag.warn(DiagnosticStage::Sema, stmt_location(*bad_stmt),
                    "'@naked' function contains a non-'asm' statement; naked functions should "
                    "contain only inline 'asm' blocks, since the compiler emits no prologue/"
                    "epilogue for them");
            }
        }

        void validate_always_inline_attribute(const ast::Attribute &attr, DiagnosticEngine &diag) {
            if (!attr.args.empty()) {
                diag.report_error(DiagnosticStage::Sema, attr.location, "'@always_inline' takes no arguments");
            }
        }

        // Type-checks and folds one attribute argument as a compile-time constant '[]u8',
        // returning the string or nullopt (having reported why). '@section' grew this shape
        // first; '@export', '@callconv' and '@import' all need exactly the same three checks,
        // so 'what' names the attribute in each diagnostic rather than each re-spelling them.
        auto fold_string_attribute_arg(const ast::Expr &arg, const char *what, const SourceLocation &loc,
                                        const std::string &module_path, Program &program, DiagnosticEngine &diag) -> std::optional<std::string> {
            LocalScope empty;
            const auto u8_slice = intern_slice(program, ResolvedType{.kind = TypeKind::U8});
            const auto arg_type = check_expr(arg, empty, module_path, program, diag, u8_slice, 0);
            if (!is_assignable(arg_type, u8_slice, program)) {
                diag.report_error(DiagnosticStage::Sema, loc, std::format("'{}' argument must be a compile-time constant '[]u8' expression", what));
                return std::nullopt;
            }
            if (!is_constant_expr(arg, module_path, program)) {
                diag.report_error(DiagnosticStage::Sema, loc, std::format("'{}' argument must be a compile-time constant expression", what));
                return std::nullopt;
            }
            const auto folded = evaluate_const_value(arg, module_path, program, diag);
            if (!folded) {
                return std::nullopt;
            }
            const auto *str = std::get_if<std::string>(&*folded);
            if (!str) {
                return std::nullopt;
            }
            if (str->empty()) {
                diag.report_error(DiagnosticStage::Sema, loc, std::format("'{}' argument must not be an empty string", what));
                return std::nullopt;
            }
            return *str;
        }

        void validate_section_attribute(const ast::Attribute &attr, const std::string &module_path, Program &program, DiagnosticEngine &diag) {
            if (attr.args.size() != 1) {
                diag.report_error(DiagnosticStage::Sema, attr.location, "'@section' requires exactly one string argument");
                return;
            }
            (void) fold_string_attribute_arg(attr.args[0], "@section", attr.location, module_path, program, diag);
        }

        // Whether 'name' can be a linker symbol at all. Deliberately permissive -- '$' and
        // '.' appear in real mangled names, and this is not the place to relitigate what a
        // platform accepts -- but a name with whitespace, a NUL, or a leading digit is
        // always a mistake, and catching it here beats emitting an object the assembler
        // rejects with no reference to the source.
        auto is_plausible_symbol_name(const std::string &name) -> bool {
            if (name.empty() || (name.front() >= '0' && name.front() <= '9')) {
                return false;
            }
            for (const unsigned char ch : name) {
                if (ch <= ' ' || ch == 0x7f || ch == ',' || ch == '(' || ch == ')') {
                    return false;
                }
            }
            return true;
        }

        void validate_no_discard_attribute(const ast::Attribute &attr, const std::vector<ResolvedType> &return_types, DiagnosticEngine &diag) {
            if (!attr.args.empty()) {
                diag.report_error(DiagnosticStage::Sema, attr.location, "'@no_discard' takes no arguments");
            }
            // Nothing to discard, so the attribute could only ever mislead a reader into
            // thinking a result exists.
            if (return_types.empty()) {
                diag.report_error(DiagnosticStage::Sema, attr.location,
                    "'@no_discard' on a function with no return value has no effect");
            }
        }

        // '@export' / '@export("name")'. 'decl_name' is the declaration's own unqualified
        // name, used when no argument is given -- for a method too, which means two methods
        // of the same name on different types both default to that bare name and collide.
        // That collision is reported by validate_export_names_for_program with both
        // locations, which is a better answer than silently mangling one of them.
        auto validate_export_attribute(const ast::Attribute &attr, const std::string &decl_name, const bool is_generic,
                                        const std::string &module_path, Program &program, DiagnosticEngine &diag) -> ExportName {
            if (attr.args.size() > 1) {
                diag.report_error(DiagnosticStage::Sema, attr.location, "'@export' takes at most one string argument");
                return std::nullopt;
            }
            // Each instantiation would need its own export name, and nothing supplies one.
            // Same rationale as '@init''s and 'ext fn''s generic bans.
            if (is_generic) {
                diag.report_error(DiagnosticStage::Sema, attr.location,
                    "'@export' is not allowed on a generic function; each instantiation would need a distinct export name");
                return std::nullopt;
            }

            std::string name = decl_name;
            if (attr.args.size() == 1) {
                const auto folded = fold_string_attribute_arg(attr.args[0], "@export", attr.location, module_path, program, diag);
                if (!folded) {
                    return std::nullopt;
                }
                name = *folded;
            }
            if (!is_plausible_symbol_name(name)) {
                diag.report_error(DiagnosticStage::Sema, attr.location,
                    std::format("'@export' name '{}' is not a valid symbol name", name));
                return std::nullopt;
            }
            return name;
        }

        // '@callconv("c")' and its alias '@cdecl'. Returns the convention the declaration
        // ends up with; CallConv::Mirage on any error, which is the status quo and so cannot
        // turn a diagnostic into a miscompile.
        auto validate_callconv_attributes(const std::vector<ast::Attribute> &attrs, const bool is_generic,
                                           const std::string &module_path, Program &program, DiagnosticEngine &diag) -> CallConv {
            const auto *callconv = find_attribute(attrs, "callconv");
            const auto *cdecl_attr = find_attribute(attrs, "cdecl");

            if (cdecl_attr && !cdecl_attr->args.empty()) {
                diag.report_error(DiagnosticStage::Sema, cdecl_attr->location, "'@cdecl' takes no arguments");
            }
            if (callconv && cdecl_attr) {
                diag.report_error(DiagnosticStage::Sema, callconv->location,
                    "'@callconv' and '@cdecl' cannot be combined: '@cdecl' is an alias for '@callconv(\"c\")'");
                return CallConv::Mirage;
            }
            if (!callconv && !cdecl_attr) {
                return CallConv::Mirage;
            }

            const auto *chosen = callconv ? callconv : cdecl_attr;
            if (is_generic) {
                diag.report_error(DiagnosticStage::Sema, chosen->location,
                    "'@callconv' is not allowed on a generic function");
                return CallConv::Mirage;
            }
            // A naked function has no compiler-generated prologue, so there is nothing to
            // implement a convention with -- the body IS the ABI.
            if (find_attribute(attrs, "naked")) {
                diag.report_error(DiagnosticStage::Sema, chosen->location,
                    "'@callconv' and '@naked' cannot be combined: a naked function has no compiler-generated prologue to implement a convention with");
                return CallConv::Mirage;
            }

            if (cdecl_attr) {
                return CallConv::C;
            }

            if (callconv->args.size() != 1) {
                diag.report_error(DiagnosticStage::Sema, callconv->location, "'@callconv' requires exactly one string argument");
                return CallConv::Mirage;
            }
            const auto folded = fold_string_attribute_arg(callconv->args[0], "@callconv", callconv->location, module_path, program, diag);
            if (!folded) {
                return CallConv::Mirage;
            }
            if (*folded == "c") {
                return CallConv::C;
            }
            if (*folded == "mirage") {
                return CallConv::Mirage;
            }
            // Names that are real conventions elsewhere get their own message rather than
            // the generic one -- the same courtesy asm_registers.hpp extends to SSE
            // registers, so "not supported" reads differently from "not a thing".
            if (*folded == "sysv" || *folded == "win64" || *folded == "fastcall" ||
                *folded == "stdcall" || *folded == "thiscall" || *folded == "vectorcall") {
                diag.report_error(DiagnosticStage::Sema, callconv->location, std::format(
                    "calling convention '{}' is not supported in v1; only \"c\" and \"mirage\" are", *folded));
                return CallConv::Mirage;
            }
            diag.report_error(DiagnosticStage::Sema, callconv->location, std::format(
                "unknown calling convention '{}'; expected \"c\" or \"mirage\"", *folded));
            return CallConv::Mirage;
        }

        // Whether any parameter or return type crosses the boundary as a by-value aggregate,
        // i.e. whether the Mirage and C conventions actually differ for this signature. Used
        // only to decide whether an '@export' without '@cdecl' deserves a warning: for a
        // scalar-only signature the two conventions coincide and there is nothing to say.
        auto signature_passes_aggregate_by_value(const std::vector<ResolvedType> &params,
                                                  const std::vector<ResolvedType> &returns) -> bool {
            const auto is_aggregate = [](const ResolvedType &t) {
                return t.kind == TypeKind::Struct || t.kind == TypeKind::Array || t.kind == TypeKind::Union;
            };
            return std::ranges::any_of(params, is_aggregate) || std::ranges::any_of(returns, is_aggregate);
        }

        // The attribute facts that outlive this pass and get copied onto the
        // FunctionSymbol/MethodInfo, so no later stage has to re-scan an attribute list.
        struct AttributeFacts {
            bool no_discard = false;
            ExportName export_name;
            CallConv call_conv = CallConv::Mirage;
        };

        // Covers the attribute checks that apply identically regardless of whether the
        // declaration is a free function, a bare-impl method, or a trait-impl method, plus
        // their mutual conflicts that aren't '@init'-specific.
        // '@init' is deliberately NOT handled here: it's rejected on methods upstream at
        // declare time (sema_declare.cpp), so a method's attrs never legitimately contain it;
        // its own structural check and '@init'-combination conflicts stay free-function-only,
        // inline at validate_attributes_for_module's call site below.
        auto validate_common_attributes(const std::vector<ast::Attribute> &attrs, const std::vector<ResolvedType> &params,
                                         const std::vector<ResolvedType> &return_types, const std::string &decl_name,
                                         const bool is_generic, const bool is_method, const ast::Stmt &body,
                                         const std::string &module_path,
                                         Program &program, DiagnosticEngine &diag) -> AttributeFacts {
            // Bound once and reused: 'naked' and 'always_inline' were each looked up twice
            // more for the combination check below, which re-scanned the attribute list to
            // rediscover what had just been found.
            const auto *no_return = find_attribute(attrs, "no_return");
            const auto *naked = find_attribute(attrs, "naked");
            const auto *always_inline = find_attribute(attrs, "always_inline");
            const auto *section = find_attribute(attrs, "section");
            const auto *no_discard = find_attribute(attrs, "no_discard");
            const auto *export_attr = find_attribute(attrs, "export");
            const auto *import_attr = find_attribute(attrs, "import");

            if (no_return) validate_no_return_attribute(*no_return, return_types, program, diag);
            if (naked) validate_naked_attribute(*naked, body, diag);
            if (always_inline) validate_always_inline_attribute(*always_inline, diag);
            if (section) validate_section_attribute(*section, module_path, program, diag);
            if (no_discard) validate_no_discard_attribute(*no_discard, return_types, diag);
            if (naked && always_inline) {
                diag.report_error(DiagnosticStage::Sema, naked->location,
                    "'@naked' and '@always_inline' cannot be combined: a naked function has no body to inline");
            }
            // '@import' names a wasm import for an 'ext fn'; a 'fn' has a body and IS the
            // definition, so there is nothing to import.
            if (import_attr) {
                diag.report_error(DiagnosticStage::Sema, import_attr->location,
                    "'@import' is only allowed on an 'ext fn' declaration");
            }

            AttributeFacts facts;
            facts.no_discard = no_discard != nullptr;
            facts.call_conv = validate_callconv_attributes(attrs, is_generic, module_path, program, diag);
            // A method's receiver would also have to cross the boundary under the C ABI, and
            // method call sites do not go through the C-ABI marshalling path. Rejected in v1
            // rather than accepted and silently ignored, which is the failure mode that
            // actually costs someone a day. '@export' on a method IS honoured -- it only
            // changes the symbol's name and linkage, not how anything is passed.
            if (is_method && facts.call_conv == CallConv::C) {
                const auto *conv_attr = find_attribute(attrs, "callconv");
                const auto *loc_attr = conv_attr ? conv_attr : find_attribute(attrs, "cdecl");
                diag.report_error(DiagnosticStage::Sema, loc_attr ? loc_attr->location : SourceLocation{},
                    "'@callconv' is not supported on impl methods in v1; declare a module-scope function instead");
                facts.call_conv = CallConv::Mirage;
            }
            // C has no multi-return, and the C-ABI lowering (ext_function_type) accepts at
            // most one return type -- silently lowering only the first would produce a
            // function C callers read wrongly.
            if (facts.call_conv == CallConv::C && return_types.size() > 1) {
                const auto *conv_attr = find_attribute(attrs, "callconv");
                const auto *loc_attr = conv_attr ? conv_attr : find_attribute(attrs, "cdecl");
                diag.report_error(DiagnosticStage::Sema, loc_attr ? loc_attr->location : SourceLocation{},
                    "'@callconv(\"c\")' is not allowed on a multi-return function; C has no multi-return convention");
                facts.call_conv = CallConv::Mirage;
            }
            if (export_attr) {
                facts.export_name = validate_export_attribute(*export_attr, decl_name, is_generic, module_path, program, diag);

                // '@export' and '@callconv' are deliberately orthogonal (an exported symbol
                // is also how two separately-compiled Mirage objects will find each other,
                // and that path must keep the Mirage ABI). But the one-word mistake --
                // '@export' alone on a signature passing a struct by value -- produces a
                // symbol C callers silently mis-marshal, so say so where it can happen.
                // Scalar-only signatures stay quiet: there the conventions coincide.
                if (facts.call_conv != CallConv::C && signature_passes_aggregate_by_value(params, return_types)) {
                    diag.warn(DiagnosticStage::Sema, export_attr->location, std::format(
                        "'@export' function '{}' passes or returns an aggregate by value but does not declare '@cdecl'; "
                        "C callers will not see the C ABI. Add '@cdecl' if this is meant to be called from C.", decl_name));
                }
            }
            return facts;
        }

        // '@test' signature restrictions. Checked in EVERY driver action, not just under
        // 'mirage test': a malformed '@test' declaration is always an error, so switching
        // actions never changes whether the declaration itself is valid. Only the BODY's
        // type-checking is mode-dependent (see Options::test_mode).
        void validate_test_structural(const ast::Attribute &attr, const ast::FunctionDecl &decl, const FunctionSymbol &fn,
                                       const Program &program, DiagnosticEngine &diag) {
            if (!attr.args.empty()) {
                diag.report_error(DiagnosticStage::Sema, attr.location, "'@test' takes no arguments");
            }
            // The harness calls every test uniformly through a 'fn() -> bool' wrapper, so
            // there is nothing to supply an argument from -- not even a defaulted one.
            if (!decl.params.empty()) {
                diag.report_error(DiagnosticStage::Sema, attr.location, "'@test' functions may not take parameters");
            }
            // Same rationale as 'ext fn'/'macro'/'trait' and '@init': there is no way to call
            // an uninstantiated template uniformly from the harness.
            if (!decl.generic_params.empty()) {
                diag.report_error(DiagnosticStage::Sema, attr.location, "'@test' is not allowed on a generic function");
                // A template's return_types are never resolved (there is one per
                // instantiation, and none exists), so the check below would always fire a
                // second, misleading "must return exactly 'error(...)'" on top of this one.
                return;
            }
            // Deliberately WIDER than '@init', which restricts to 'enum(i32)': a test reports
            // pass/fail through the Ok/Failed tag, and any error type (or union of them)
            // carries that. Documented as an explicit asymmetry in spec.md's Testing section.
            const bool returns_error_union = fn.return_types.size() == 1 &&
                fn.return_types.front().kind == TypeKind::Union &&
                program.union_at(fn.return_types.front().union_index) != nullptr &&
                program.union_at(fn.return_types.front().union_index)->is_error_union;
            if (!returns_error_union) {
                diag.report_error(DiagnosticStage::Sema, attr.location,
                    "'@test' functions must return exactly 'error(...)'");
            }
        }

        void validate_init_structural(const ast::Attribute &attr, const ast::FunctionDecl &decl, const FunctionSymbol &fn, const Program &program, DiagnosticEngine &diag) {
            if (!attr.args.empty()) {
                diag.report_error(DiagnosticStage::Sema, attr.location, "'@init' takes no arguments");
            }
            if (!decl.params.empty()) {
                diag.report_error(DiagnosticStage::Sema, attr.location, "'@init' functions may not take parameters");
            }
            // A generic function has no single concrete body to call implicitly -- it only
            // exists once monomorphized, and nothing supplies the arguments for an implicit
            // call. '@init' is already rejected on impl/trait-impl methods at declare time
            // for the same "there is no one function to run" reason; this closes the gap for
            // 'fn f[T: type]()'. Without it, validate_init_dependencies_for_program schedules
            // the uninstantiated declaration straight into Program::init_call_order.
            if (!decl.generic_params.empty()) {
                diag.report_error(DiagnosticStage::Sema, attr.location, "'@init' is not allowed on a generic function");
            }
            // Deliberately no 'pub' requirement: '_init' is emitted into the same LLVM module
            // as every other function in the program (this is a whole-program compile, not
            // separate translation units linked afterward), so it can call a private '@init'
            // function directly — and in the common case, a module's initializer is exactly
            // the kind of thing that should NOT be an ordinary externally-callable symbol.
            if (!is_void_or_error_return(fn.return_types, program)) {
                diag.report_error(DiagnosticStage::Sema, attr.location, "'@init' functions must return nothing or 'error(...)'");
            }
            // '?error(...)' resolves to a perfectly ordinary error union, so it passes the check
            // above — but an '@init' function has no caller that could drop anything: the
            // synthesized initializer runner inspects the result itself and terminates the
            // process on failure regardless. Accept it, but say that it buys nothing, since the
            // opposite expectation is otherwise invisible.
            if (!fn.return_types.empty()) {
                const auto *info = fn.return_types.back().kind == TypeKind::Union
                    ? program.union_at(fn.return_types.back().union_index) : nullptr;
                if (info != nullptr && info->is_error_union && info->is_optional) {
                    diag.warn(DiagnosticStage::Sema, attr.location,
                        "'?' has no effect on an '@init' function; the initializer runner terminates the process on failure regardless");
                }
            }
        }
    }

    // Runs after every free function/impl-method/trait-impl-method signature is resolved
    // (called from check_program right after resolve_trait_impl_signatures_for_program), but
    // before body-checking — none of the five attributes' own checks need type-checked
    // bodies, only resolved signatures and raw AST shape (attribute args, param/return lists,
    // the body's top-level statement list for '@naked'). Sibling functions
    // validate_method_attributes_for_module (bare-impl methods) and
    // validate_trait_impl_attributes_for_program (trait-impl methods) below cover the other
    // two declaration kinds attributes are legal on.
    void validate_attributes_for_module(const std::string &module_path, ProgramModule &module, Program &program, DiagnosticEngine &diag) {
        for (auto &[name, sym] : module.symbols) {
            // A bare-import alias shares its 'decl' AST pointer with the origin — the
            // origin module's own pass over this loop already validates its attributes
            // once, correctly, under the origin's context (e.g. '@section's constant-fold
            // must resolve relative to the origin, not the importer).
            if (module.bare_import_origins.contains(name)) continue;
            auto *fn = std::get_if<FunctionSymbol>(&sym);
            if (!fn || !fn->decl || fn->decl->attributes.empty()) continue;
            const auto &attrs = fn->decl->attributes;

            const auto facts = validate_common_attributes(attrs, fn->params, fn->return_types, name,
                                                          is_generic_function(*fn), /*is_method=*/false,
                                                          fn->decl->body, module_path, program, diag);
            fn->no_discard = facts.no_discard;
            fn->export_name = facts.export_name;
            fn->call_conv = facts.call_conv;
            fn->is_test = find_attribute(attrs, "test") != nullptr;

            if (const auto *test_attr = find_attribute(attrs, "test")) {
                validate_test_structural(*test_attr, *fn->decl, *fn, program, diag);

                // '@test' + '@always_inline' is deliberately ABSENT from this list: nothing
                // prevents a test body from being inlined into its synthesized wrapper's
                // call site, so the combination is legal.
                if (find_attribute(attrs, "naked")) {
                    diag.report_error(DiagnosticStage::Sema, test_attr->location,
                        "'@test' and '@naked' cannot be combined: a naked function has no compiler-generated "
                        "body shape, and the harness needs an ordinary calling convention");
                }
                if (find_attribute(attrs, "no_return")) {
                    diag.report_error(DiagnosticStage::Sema, test_attr->location,
                        "'@test' and '@no_return' cannot be combined: a test that never returns cannot report a status");
                }
                if (find_attribute(attrs, "init")) {
                    diag.report_error(DiagnosticStage::Sema, test_attr->location,
                        "'@test' and '@init' cannot be combined: two different automatic-invocation mechanisms");
                }
                if (find_attribute(attrs, "export")) {
                    diag.report_error(DiagnosticStage::Sema, test_attr->location,
                        "'@test' and '@export' cannot be combined: a test is invoked only through its synthesized wrapper");
                }
                if (find_attribute(attrs, "callconv") || find_attribute(attrs, "cdecl")) {
                    diag.report_error(DiagnosticStage::Sema, test_attr->location,
                        "'@test' and '@callconv' cannot be combined: the synthesized wrapper calls the test "
                        "through the Mirage convention");
                }
            }

            if (const auto *init_attr = find_attribute(attrs, "init")) {
                validate_init_structural(*init_attr, *fn->decl, *fn, program, diag);

                if (find_attribute(attrs, "no_return")) {
                    diag.report_error(DiagnosticStage::Sema, init_attr->location,
                        "'@init' and '@no_return' cannot be combined: an initializer must return "
                        "control so the next one can run");
                }
                if (find_attribute(attrs, "naked")) {
                    diag.report_error(DiagnosticStage::Sema, init_attr->location,
                        "'@init' and '@naked' cannot be combined: init functions must be callable normally");
                }
                if (find_attribute(attrs, "always_inline")) {
                    diag.report_error(DiagnosticStage::Sema, init_attr->location,
                        "'@init' and '@always_inline' cannot be combined: init functions are called "
                        "from the generated '_init', inlining them defeats the purpose");
                }
            }
        }
    }

    // Sibling to validate_attributes_for_module, but for bare-impl methods (module.methods).
    // Called per-module at the same pipeline point — resolve_impl_signatures_for_module has
    // already resolved every method's signature by the time check_program calls this. No
    // '@init' handling: it's already a declare-time error for methods (sema_declare.cpp), so
    // it never legitimately reaches here.
    void validate_method_attributes_for_module(const std::string &module_path, ProgramModule &module, Program &program, DiagnosticEngine &diag) {
        for (auto &method_map : module.methods | std::views::values) {
            for (auto &info : method_map | std::views::values) {
                if (!info.is_resolved || !info.decl || info.decl->attributes.empty()) continue;
                // A method of a generic type (or in a generic 'impl' block) is a template in
                // the same sense a generic free function is -- one declaration, many
                // instantiations -- so '@export'/'@callconv' are refused for the same reason.
                const auto is_generic = info.impl_generic_params != nullptr && !info.impl_generic_params->empty();
                const auto facts = validate_common_attributes(info.decl->attributes, info.param_types, info.return_types,
                                                              info.decl->name, is_generic, /*is_method=*/true,
                                                              info.decl->body, module_path, program, diag);
                info.no_discard = facts.no_discard;
                info.export_name = facts.export_name;
                info.call_conv = facts.call_conv;
            }
        }
    }

    // Whole-program collision check over every linker-visible name '@export' introduces.
    // Must run after every per-module and trait-impl attribute pass, since it reads the
    // export names those passes recorded.
    //
    // 'ext fn' names are collected too: they occupy the same flat linker namespace, so
    // '@export("printf")' beside 'ext fn printf' is a genuine clash, just one where only
    // one side carries an attribute.
    void validate_export_names_for_program(Program &program, DiagnosticEngine &diag) {
        // Collected first and sorted by source position before any diagnostic is emitted.
        // Program::modules and each symbol table are unordered_maps, so claiming as we walk
        // would make WHICH of two colliding declarations is reported the winner depend on
        // hash order -- i.e. differ between runs on the same input. Sorted, the first
        // declaration in source order always owns the name and the later one is the error.
        struct Claim {
            std::string name;
            std::string what;
            SourceLocation loc;
        };
        std::vector<Claim> claims;
        // 'ext fn' names are pre-claimed rather than reported on: they occupy the same flat
        // linker namespace, so '@export("printf")' beside 'ext fn printf' is a genuine clash,
        // but two modules declaring the same 'ext fn' is not (they are deduplicated
        // process-globally by name, and a genuine redefinition is caught upstream).
        std::map<std::string, std::pair<std::string, SourceLocation>> seen;

        for (auto &module : program.modules | std::views::values) {
            for (auto &[name, sym] : module.symbols) {
                // A bare-import alias shares the origin's decl; the origin claims the name.
                if (module.bare_import_origins.contains(name)) continue;
                if (const auto *fn = std::get_if<FunctionSymbol>(&sym); fn && fn->export_name) {
                    claims.push_back({*fn->export_name, std::format("function '{}'", name),
                                      fn->decl ? fn->decl->location : SourceLocation{}});
                } else if (const auto *ext = std::get_if<ExtFunctionSymbol>(&sym); ext && ext->decl) {
                    seen.try_emplace(ext->decl->name, std::pair{std::format("'ext fn {}'", ext->decl->name), ext->decl->location});
                }
            }
            for (auto &[type_name, method_map] : module.methods) {
                for (auto &[method_name, info] : method_map) {
                    if (!info.export_name) continue;
                    claims.push_back({*info.export_name, std::format("method '{}.{}'", type_name, method_name),
                                      info.decl ? info.decl->location : SourceLocation{}});
                }
            }
        }

        for (auto &impls : program.trait_impls_by_type | std::views::values) {
            for (auto &impl_info : impls) {
                for (auto &[method_name, info] : impl_info.methods) {
                    if (!info.export_name) continue;
                    claims.push_back({*info.export_name, std::format("trait method '{}.{}'", info.type_name, method_name),
                                      info.decl ? info.decl->location : SourceLocation{}});
                }
            }
        }

        std::ranges::sort(claims, [](const Claim &a, const Claim &b) {
            return std::tie(a.loc.filename, a.loc.line, a.loc.column, a.name) <
                   std::tie(b.loc.filename, b.loc.line, b.loc.column, b.name);
        });

        for (const auto &c : claims) {
            const auto [it, inserted] = seen.try_emplace(c.name, std::pair{c.what, c.loc});
            if (!inserted) {
                diag.report_error(DiagnosticStage::Sema, c.loc, std::format(
                    "duplicate export name '{}': already exported by {} at {}:{}:{}",
                    c.name, it->second.first, it->second.second.filename, it->second.second.line, it->second.second.column));
            }
        }
    }

    // 'ext fn' declarations. The parser accepts an attribute clause before 'ext fn' solely
    // so '@import' can name a wasm import; every OTHER attribute is rejected here by name,
    // which keeps the long-standing "attributes are not for 'ext fn'" rule in force while
    // giving a precise diagnostic instead of a blanket parse error.
    void validate_ext_function_attributes_for_module(const std::string &module_path, ProgramModule &module, Program &program, DiagnosticEngine &diag) {
        for (auto &[name, sym] : module.symbols) {
            // Same bare-import-alias reasoning as validate_attributes_for_module: the origin
            // module's own pass validates the shared decl once, in the right context.
            if (module.bare_import_origins.contains(name)) continue;
            auto *ext = std::get_if<ExtFunctionSymbol>(&sym);
            if (!ext || !ext->decl || ext->decl->attributes.empty()) continue;

            for (const auto &attr : ext->decl->attributes) {
                if (attr.name != "import") {
                    diag.report_error(DiagnosticStage::Sema, attr.location, std::format(
                        "'@{}' is not allowed on an 'ext fn' declaration; only '@import' is", attr.name));
                }
            }

            const auto *import_attr = find_attribute(ext->decl->attributes, "import");
            if (!import_attr) continue;

            if (import_attr->args.empty() || import_attr->args.size() > 2) {
                diag.report_error(DiagnosticStage::Sema, import_attr->location,
                    "'@import' requires one or two string arguments: '@import(\"module\")' or '@import(\"module\", \"name\")'");
                continue;
            }
            const auto module_name = fold_string_attribute_arg(import_attr->args[0], "@import", import_attr->location, module_path, program, diag);
            if (!module_name) continue;

            std::string import_name = ext->decl->name;
            if (import_attr->args.size() == 2) {
                const auto folded = fold_string_attribute_arg(import_attr->args[1], "@import", import_attr->location, module_path, program, diag);
                if (!folded) continue;
                import_name = *folded;
            }

            ext->import_module = *module_name;
            ext->import_name = std::move(import_name);
        }
    }

    // Whole-program pass (not per-module) for trait-impl methods, mirroring why
    // resolve_trait_impl_signatures_for_program is its own whole-program pass distinct from
    // resolve_signatures_for_module: TraitImplInfo::impl_module can differ from the type's own
    // module, so this can't be folded into validate_method_attributes_for_module's per-module
    // loop above.
    void validate_trait_impl_attributes_for_program(Program &program, DiagnosticEngine &diag) {
        for (auto &impls : program.trait_impls_by_type | std::views::values) {
            for (auto &impl_info : impls) {
                for (auto &info : impl_info.methods | std::views::values) {
                    if (!info.is_resolved || !info.decl || info.decl->attributes.empty()) continue;
                    const auto is_generic = info.impl_generic_params != nullptr && !info.impl_generic_params->empty();
                    const auto facts = validate_common_attributes(info.decl->attributes, info.param_types, info.return_types,
                                                                   info.decl->name, is_generic, /*is_method=*/true,
                                                                   info.decl->body, impl_info.impl_module, program, diag);
                    info.no_discard = facts.no_discard;
                    info.export_name = facts.export_name;
                    info.call_conv = facts.call_conv;
                }
            }
        }
    }

    namespace {
        using ForeignRefCallback = std::function<void(const std::string &target_module, const std::string &symbol_name, const SourceLocation &loc)>;

        void walk_expr_for_foreign_refs(const ast::Expr &expr, std::set<std::string> &locals, const std::string &module_path, const Program &program, const ForeignRefCallback &on_foreign_ref);
        void walk_stmt_for_foreign_refs(const ast::Stmt &stmt, std::set<std::string> &locals, const std::string &module_path, const Program &program, const ForeignRefCallback &on_foreign_ref);
        void walk_when_stmt_for_foreign_refs(const ast::WhenStmt &when, std::set<std::string> &locals, const std::string &module_path, const Program &program, const ForeignRefCallback &on_foreign_ref);

        // Thin wrapper over the shared walker (sema.cpp): the foreign-ref walk tracks
        // bound names in a plain std::set rather than a LocalScope (it is a structural
        // reference-finder, not a type checker), so shadowing is expressed as a
        // predicate over that set.
        auto resolve_namespace_chain(const ast::Expr &expr, const std::string &module_path, const std::set<std::string> &locals, const Program &program) -> std::optional<std::string> {
            return resolve_expr_namespace_chain(
                expr, module_path, program,
                [&locals](const std::string &name) { return locals.contains(name); },
                /*follow_member_chains=*/true);
        }

        // Exhaustive walk over every ast::Expr alternative, invoking 'on_foreign_ref' for every
        // '<namespace>.<member>' access whose base resolves (via resolve_namespace_chain) to a
        // module other than 'module_path'. This is a structural reference-finder, not a type
        // checker — it deliberately doesn't need LocalScope/expr_types, since it only cares
        // whether an identifier names an import alias, not what type anything has. 'locals'
        // tracks names bound so far (params, 'mut'/'const' locals, for-loop index/element
        // names) so a shadowing local never gets mistaken for a module-level import alias.
        void walk_expr_for_foreign_refs(const ast::Expr &expr, std::set<std::string> &locals, const std::string &module_path, const Program &program, const ForeignRefCallback &on_foreign_ref) {
            std::visit([&]<typename T>(const T &v) {
                using V = std::decay_t<T>;

                if constexpr (std::is_same_v<V, ast::LiteralIntegerExpr> || std::is_same_v<V, ast::LiteralFloatExpr> ||
                              std::is_same_v<V, ast::LiteralStringExpr> || std::is_same_v<V, ast::LiteralCharExpr> ||
                              std::is_same_v<V, ast::LiteralBoolExpr> || std::is_same_v<V, ast::LiteralNilExpr> ||
                              std::is_same_v<V, ast::ImportBinExpr> ||
                              std::is_same_v<V, ast::IotaExpr> || std::is_same_v<V, ast::DotIdentExpr> ||
                              std::is_same_v<V, ast::DefaultExpr> || std::is_same_v<V, ast::UndefinedExpr> ||
                              std::is_same_v<V, ast::RttiEnabledExpr> ||
                              std::is_same_v<V, std::unique_ptr<ast::TypeExpr>> || std::is_same_v<V, std::unique_ptr<ast::AsmExpr>>) {
                    // Leaves w.r.t. foreign-reference detection: no nested Expr fields relevant
                    // here (TypeExpr wraps a Type, not a value; AsmExpr's operands are
                    // registers/immediates/local variables only, never cross-module symbols).

                } else if constexpr (std::is_same_v<V, ast::IdentExpr>) {
                    // A bare-imported symbol is referenced as a plain identifier but lives in
                    // another module — treating it as a leaf meant '@init' bodies calling one
                    // produced NO dependency edge, and init_call_order could silently run
                    // initializers in the wrong order.
                    if (!locals.contains(v.name)) {
                        if (const auto mod_it = program.modules.find(module_path); mod_it != program.modules.end()) {
                            if (const auto origin = mod_it->second.bare_import_origins.find(v.name);
                                origin != mod_it->second.bare_import_origins.end()) {
                                on_foreign_ref(origin->second.module_path, origin->second.symbol_name, v.location);
                            }
                        }
                    }

                } else if constexpr (std::is_same_v<V, ast::ImportExpr>) {
                    // Handled by the MemberExpr case below (when this IS the '.object' of one) —
                    // a bare 'import(...)' with no '.field' access doesn't reference any symbol.

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::MemberExpr>>) {
                    if (const auto target_module = resolve_namespace_chain(v->object, module_path, locals, program)) {
                        on_foreign_ref(*target_module, v->member, v->location);
                    } else {
                        walk_expr_for_foreign_refs(v->object, locals, module_path, program, on_foreign_ref);
                    }

                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::UnaryExpr>>) {
                    walk_expr_for_foreign_refs(v->operand, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::BinaryExpr>>) {
                    walk_expr_for_foreign_refs(v->lhs, locals, module_path, program, on_foreign_ref);
                    walk_expr_for_foreign_refs(v->rhs, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::TernaryExpr>>) {
                    walk_expr_for_foreign_refs(v->condition, locals, module_path, program, on_foreign_ref);
                    walk_expr_for_foreign_refs(v->then_expr, locals, module_path, program, on_foreign_ref);
                    walk_expr_for_foreign_refs(v->else_expr, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::WhenExpr>>) {
                    walk_expr_for_foreign_refs(v->condition, locals, module_path, program, on_foreign_ref);
                    walk_expr_for_foreign_refs(v->then_expr, locals, module_path, program, on_foreign_ref);
                    walk_expr_for_foreign_refs(v->else_expr, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::AssignExpr>>) {
                    walk_expr_for_foreign_refs(v->target, locals, module_path, program, on_foreign_ref);
                    walk_expr_for_foreign_refs(v->value, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::CallExpr>>) {
                    walk_expr_for_foreign_refs(v->callee, locals, module_path, program, on_foreign_ref);
                    for (const auto &arg : v->args) walk_expr_for_foreign_refs(arg, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::IncrDecrExpr>>) {
                    walk_expr_for_foreign_refs(v->operand, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::SizeOfExpr>>) {
                    walk_expr_for_foreign_refs(v->operand, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::AlignOfExpr>>) {
                    walk_expr_for_foreign_refs(v->operand, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::TypeOfExpr>>) {
                    walk_expr_for_foreign_refs(v->operand, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::TypeInfoOfExpr>>) {
                    walk_expr_for_foreign_refs(v->operand, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::OptionExpr>>) {
                    if (v->default_value) walk_expr_for_foreign_refs(*v->default_value, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::EnvExpr>>) {
                    if (v->default_value) walk_expr_for_foreign_refs(*v->default_value, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::LenExpr>>) {
                    walk_expr_for_foreign_refs(v->operand, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::StackAllocExpr>>) {
                    walk_expr_for_foreign_refs(v->size, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::CastExpr>>) {
                    walk_expr_for_foreign_refs(v->value, locals, module_path, program, on_foreign_ref);
                    if (v->len_expr) walk_expr_for_foreign_refs(*v->len_expr, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::IndexOrInstantiateExpr>>) {
                    walk_expr_for_foreign_refs(v->operand, locals, module_path, program, on_foreign_ref);
                    // Type-tagged args (a generic type argument) name types, not values, so
                    // they can never be a foreign (module-scope) value reference — only
                    // Expr-tagged args (value arguments, or a not-yet-classified index) can be.
                    for (const auto &arg : v->args) {
                        if (const auto *expr_arg = std::get_if<ast::Expr>(&arg.value)) {
                            walk_expr_for_foreign_refs(*expr_arg, locals, module_path, program, on_foreign_ref);
                        }
                    }
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::SliceExpr>>) {
                    walk_expr_for_foreign_refs(v->operand, locals, module_path, program, on_foreign_ref);
                    if (v->start) walk_expr_for_foreign_refs(*v->start, locals, module_path, program, on_foreign_ref);
                    if (v->end) walk_expr_for_foreign_refs(*v->end, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::MatchExpr>>) {
                    walk_expr_for_foreign_refs(v->operand, locals, module_path, program, on_foreign_ref);
                    for (const auto &arm : v->arms) {
                        if (const auto *lit = std::get_if<ast::MatchExpr::LiteralPattern>(&arm.pattern)) {
                            walk_expr_for_foreign_refs(*lit->expr, locals, module_path, program, on_foreign_ref);
                        }
                        walk_expr_for_foreign_refs(arm.value, locals, module_path, program, on_foreign_ref);
                    }
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::BracedInitializerExpr>>) {
                    std::visit([&]<typename BT>(const BT &bv) {
                        using BV = std::decay_t<BT>;
                        if constexpr (std::is_same_v<BV, ast::StructExpr>) {
                            for (const auto &field : bv.fields) walk_expr_for_foreign_refs(field.expr, locals, module_path, program, on_foreign_ref);
                        } else if constexpr (std::is_same_v<BV, ast::ArrayExpr>) {
                            for (const auto &value : bv.values) walk_expr_for_foreign_refs(value, locals, module_path, program, on_foreign_ref);
                        }
                        // EmptyExpr, BitsetExpr: leaves, nothing further to recurse into.
                    }, *v);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::TaggedVariantExpr>>) {
                    for (const auto &field : v->payload.fields) walk_expr_for_foreign_refs(field.expr, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::TryExpr>>) {
                    walk_expr_for_foreign_refs(v->call, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::RangeExpr>>) {
                    if (v->lower) walk_expr_for_foreign_refs(*v->lower, locals, module_path, program, on_foreign_ref);
                    walk_expr_for_foreign_refs(v->upper, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::SpreadExpr>>) {
                    walk_expr_for_foreign_refs(v->operand, locals, module_path, program, on_foreign_ref);
                } else {
                    static_assert(!sizeof(V), "walk_expr_for_foreign_refs: unhandled ast::Expr alternative");
                }
            }, expr);
        }

        void walk_when_stmt_for_foreign_refs(const ast::WhenStmt &when, std::set<std::string> &locals, const std::string &module_path, const Program &program, const ForeignRefCallback &on_foreign_ref) {
            walk_expr_for_foreign_refs(when.condition, locals, module_path, program, on_foreign_ref);
            for (const auto &s : when.then_block.stmts) walk_stmt_for_foreign_refs(s, locals, module_path, program, on_foreign_ref);
            if (when.else_branch) {
                std::visit([&]<typename ET>(const ET &ev) {
                    using EV = std::decay_t<ET>;
                    if constexpr (std::is_same_v<EV, ast::BlockStmt>) {
                        for (const auto &s : ev.stmts) walk_stmt_for_foreign_refs(s, locals, module_path, program, on_foreign_ref);
                    } else if constexpr (std::is_same_v<EV, std::unique_ptr<ast::WhenStmt>>) {
                        walk_when_stmt_for_foreign_refs(*ev, locals, module_path, program, on_foreign_ref);
                    } else {
                        static_assert(!sizeof(EV), "walk_when_stmt_for_foreign_refs: unhandled else_branch alternative");
                    }
                }, *when.else_branch);
            }
        }

        // Exhaustive walk over every ast::Stmt alternative. See walk_expr_for_foreign_refs's
        // doc comment above for the general approach; 'locals' is threaded through and grown
        // as declarations are encountered, in source order, matching how references can only
        // ever target an already-declared local.
        void walk_stmt_for_foreign_refs(const ast::Stmt &stmt, std::set<std::string> &locals, const std::string &module_path, const Program &program, const ForeignRefCallback &on_foreign_ref) {
            std::visit([&]<typename T>(const T &v) {
                using V = std::decay_t<T>;

                if constexpr (std::is_same_v<V, std::unique_ptr<ast::BlockStmt>>) {
                    for (const auto &s : v->stmts) walk_stmt_for_foreign_refs(s, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::IfStmt>>) {
                    walk_expr_for_foreign_refs(v->condition, locals, module_path, program, on_foreign_ref);
                    walk_stmt_for_foreign_refs(v->then_stmt, locals, module_path, program, on_foreign_ref);
                    if (v->else_stmt) walk_stmt_for_foreign_refs(*v->else_stmt, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::WhileStmt>>) {
                    walk_expr_for_foreign_refs(v->condition, locals, module_path, program, on_foreign_ref);
                    walk_stmt_for_foreign_refs(v->body, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::ForInStmt>>) {
                    walk_expr_for_foreign_refs(v->iterable, locals, module_path, program, on_foreign_ref);
                    locals.insert(v->index_name);
                    locals.insert(v->element_name);
                    walk_stmt_for_foreign_refs(v->body, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::SwitchStmt>>) {
                    walk_expr_for_foreign_refs(v->operand, locals, module_path, program, on_foreign_ref);
                    for (const auto &arm : v->arms) {
                        if (const auto *lit = std::get_if<ast::MatchExpr::LiteralPattern>(&arm.pattern)) {
                            walk_expr_for_foreign_refs(*lit->expr, locals, module_path, program, on_foreign_ref);
                        }
                        walk_stmt_for_foreign_refs(arm.body, locals, module_path, program, on_foreign_ref);
                    }
                } else if constexpr (std::is_same_v<V, ast::ExprStmt>) {
                    walk_expr_for_foreign_refs(v.expr, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, ast::VarDeclStmt>) {
                    if (v.init) walk_expr_for_foreign_refs(*v.init, locals, module_path, program, on_foreign_ref);
                    locals.insert(v.name);
                } else if constexpr (std::is_same_v<V, ast::VarDeclGroupStmt>) {
                    walk_expr_for_foreign_refs(v.init, locals, module_path, program, on_foreign_ref);
                    for (const auto &name : v.names) locals.insert(name);
                } else if constexpr (std::is_same_v<V, ast::ContinueStmt> || std::is_same_v<V, ast::BreakStmt>) {
                    // leaves
                } else if constexpr (std::is_same_v<V, ast::ReturnStmt> || std::is_same_v<V, ast::ReturnOkStmt>) {
                    for (const auto &value : v.return_values) walk_expr_for_foreign_refs(value, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, ast::ReturnErrStmt>) {
                    walk_expr_for_foreign_refs(v.error_value, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::DeferStmt>>) {
                    walk_stmt_for_foreign_refs(v->body, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, ast::LinkDecl>) {
                    walk_expr_for_foreign_refs(v.data, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, ast::DiagnosticDecl>) {
                    walk_expr_for_foreign_refs(v.message, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::WhenStmt>>) {
                    walk_when_stmt_for_foreign_refs(*v, locals, module_path, program, on_foreign_ref);
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::AsmStmt>>) {
                    // leaf: asm operands are registers/immediates/local variables only.
                } else {
                    static_assert(!sizeof(V), "walk_stmt_for_foreign_refs: unhandled ast::Stmt alternative");
                }
            }, stmt);
        }
    }

    // Builds the '@init' cross-module dependency graph from ACTUAL symbol references made in
    // '@init' function bodies (not the coarser "module A imports module B" signal), then
    // topologically sorts it into Program::init_call_order. Dependency *nodes* are modules
    // that declare at least one '@init' function (same-module '@init' functions run in
    // source declaration order — no cycle is possible there by construction); an edge
    // 'A -> B' means some '@init' function in module A references a symbol declared in
    // module B, and B itself has '@init' function(s) that must therefore run first.
    // Collects every '@test' function in the program, in the order Test_Info must list them
    // (see Program::discovered_tests). Runs only under 'mirage test'; the whole feature is
    // inert otherwise.
    //
    // One uniform scan over every loaded module, with no special-casing for HOW a module
    // entered the set -- a forced module ('--load') is scanned on exactly the same terms as
    // one reached by an ordinary import. Order comes from ast::Program::module_order, which
    // already appends forced modules after the normal graph.
    void discover_tests_for_program(const ast::Program &ast_program, Program &sema_program) {
        if (!sema_program.options.test_mode) {
            return;
        }

        // Root-relative module names, which is the spelling 'import(...)' paths use and so
        // the one a reader recognizes in the harness's output. A module outside the root
        // (the standard library, reached through another search root) has no root-relative
        // form; fall back to its final path component rather than printing an absolute path.
        const auto display_name = [&](const std::string &path) -> std::string {
            const auto &root = ast_program.root_module_path;
            if (path == root) {
                return std::filesystem::path(root).filename().string();
            }
            if (path.size() > root.size() + 1 && path.starts_with(root) && path[root.size()] == '/') {
                return path.substr(root.size() + 1);
            }
            return std::filesystem::path(path).filename().string();
        };

        for (const auto &module_path : ast_program.module_order) {
            const auto ast_it = ast_program.modules.find(module_path);
            const auto sema_it = sema_program.modules.find(module_path);
            if (ast_it == ast_program.modules.end() || sema_it == sema_program.modules.end()) {
                continue;
            }
            // Walked over the AST rather than the symbol table so declaration order within
            // the module is preserved -- symbols is an unordered_map.
            for (const auto &decl : ast::all_decls(ast_it->second)) {
                const auto *fn_decl = std::get_if<ast::FunctionDecl>(&decl);
                if (!fn_decl || !find_attribute(fn_decl->attributes, "test")) {
                    continue;
                }
                // A bare-import alias shares the origin's decl; the origin's own module
                // already contributed it, and running the same test twice is not the intent.
                if (sema_it->second.bare_import_origins.contains(fn_decl->name)) {
                    continue;
                }
                const auto sym_it = sema_it->second.symbols.find(fn_decl->name);
                if (sym_it == sema_it->second.symbols.end()) {
                    continue;
                }
                const auto *fn = std::get_if<FunctionSymbol>(&sym_it->second);
                // Only the declaration this module actually owns: a '#compile_only_if'
                // excluded file's function never reaches the symbol table under its own decl.
                if (!fn || fn->decl != fn_decl) {
                    continue;
                }
                sema_program.discovered_tests.push_back(Program::TestCase{
                    .module_path = module_path,
                    .module_name = display_name(module_path),
                    .function_name = fn_decl->name,
                });
            }
        }
    }

    void validate_init_dependencies_for_program(const ast::Program &ast_program, Program &sema_program, DiagnosticEngine &diag) {
        struct InitFn {
            std::string name;
            const ast::FunctionDecl *decl = nullptr;
        };

        std::map<std::string, std::vector<InitFn>> by_module; // preserves ast::Module's source order
        std::set<std::string> has_init;

        for (const auto &[module_path, files] : ast_program.modules) {
            for (const auto &file : files) {
                // A '#compile_only_if'-excluded file's '@init' functions were never
                // declared, so they must not enter the init call order either.
                if (const auto mod_it = sema_program.modules.find(module_path);
                    mod_it != sema_program.modules.end() && mod_it->second.excluded_files.contains(file.file_path)) {
                    continue;
                }
                for (const auto &decl : file.declarations) {
                    if (const auto *fn_decl = std::get_if<ast::FunctionDecl>(&decl)) {
                        if (find_attribute(fn_decl->attributes, "init")) {
                            by_module[module_path].push_back(InitFn{.name = fn_decl->name, .decl = fn_decl});
                            has_init.insert(module_path);
                        }
                    }
                }
            }
        }
        if (has_init.empty()) return;

        // module -> (dependency module -> first-seen referencing location)
        std::map<std::string, std::map<std::string, SourceLocation>> edges;
        for (const auto &module_path : has_init) {
            for (const auto &init_fn : by_module.at(module_path)) {
                std::set<std::string> locals;
                walk_stmt_for_foreign_refs(init_fn.decl->body, locals, module_path, sema_program,
                    [&](const std::string &target_module, const std::string &, const SourceLocation &loc) {
                        if (target_module == module_path || !has_init.contains(target_module)) return;
                        auto &module_edges = edges[module_path];
                        module_edges.try_emplace(target_module, loc);
                    });
            }
        }

        // Kahn's algorithm: a module's in-degree is how many other has-init modules IT
        // depends on (edges[m].size()); it becomes ready once all of those have been placed.
        std::map<std::string, int> in_degree;
        for (const auto &m : has_init) in_degree[m] = static_cast<int>(edges[m].size());

        std::map<std::string, std::vector<std::string>> dependents; // B -> every A with edges[A] containing B
        for (const auto &[a, deps] : edges) {
            for (const auto &b : deps | std::views::keys) dependents[b].push_back(a);
        }

        std::vector<std::string> ready;
        for (const auto &[m, deg] : in_degree) {
            if (deg == 0) ready.push_back(m);
        }

        std::vector<std::string> order;
        while (!ready.empty()) {
            std::ranges::sort(ready); // deterministic among mutually-independent modules
            const auto m = ready.front();
            ready.erase(ready.begin());
            order.push_back(m);
            for (const auto &dependent : dependents[m]) {
                if (--in_degree[dependent] == 0) ready.push_back(dependent);
            }
        }

        if (order.size() != has_init.size()) {
            std::vector<std::string> stuck;
            for (const auto &[m, deg] : in_degree) {
                if (deg > 0) stuck.push_back(m);
            }
            std::ranges::sort(stuck);
            const auto &a = stuck.front();
            const auto &[b, loc] = *edges.at(a).begin();
            diag.report_error(DiagnosticStage::Sema, loc, std::format(
                "circular '@init' dependency: module '{}' requires module '{}' to initialize "
                "first, but '{}' also (transitively) requires '{}' to initialize first",
                a, b, b, a));
            return;
        }

        for (const auto &module_path : order) {
            for (const auto &init_fn : by_module.at(module_path)) {
                sema_program.init_call_order.emplace_back(module_path, init_fn.name);
            }
        }
    }
}
