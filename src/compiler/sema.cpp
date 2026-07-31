#include "sema.hpp"

#include <algorithm>
#include <format>
#include <ranges>

namespace sema {
    void build_symbol_table_for_module(const ast::Program &program, const std::string &module_path, ProgramModule &module, Program &sema_program, const ast::Module &decls, DiagnosticEngine &diagnostics);
    void register_trait_impls_for_program(const ast::Program &ast_program, Program &sema_program, DiagnosticEngine &diag);
    void ensure_module_declared(const ast::Program &program, const std::string &module_path, Program &sema_program, DiagnosticEngine &diag, const SourceLocation &trigger_location = {});
    void validate_attributes_for_module(const std::string &module_path, ProgramModule &module, Program &program, DiagnosticEngine &diag);
    void validate_method_attributes_for_module(const std::string &module_path, ProgramModule &module, Program &program, DiagnosticEngine &diag);
    void validate_trait_impl_attributes_for_program(Program &program, DiagnosticEngine &diag);
    void validate_init_dependencies_for_program(const ast::Program &ast_program, Program &sema_program, DiagnosticEngine &diag);
    void check_generic_templates_for_program(Program &program, DiagnosticEngine &diag);

    // Minimal human-readable rendering of a resolved type, used for trait conformance
    // and default-parameter-value diagnostics (there is no general ResolvedType-to-string
    // formatter elsewhere in sema).
    auto describe_type(const ResolvedType &t, const Program &program) -> std::string {
        switch (t.kind) {
        case TypeKind::Invalid:  return "<invalid>";
        case TypeKind::Void:     return "void";
        case TypeKind::U8:       return "u8";
        case TypeKind::U16:      return "u16";
        case TypeKind::U32:      return "u32";
        case TypeKind::U64:      return "u64";
        case TypeKind::I8:       return "i8";
        case TypeKind::I16:      return "i16";
        case TypeKind::I32:      return "i32";
        case TypeKind::I64:      return "i64";
        case TypeKind::F32:      return "f32";
        case TypeKind::F64:      return "f64";
        case TypeKind::USize:    return "usize";
        case TypeKind::Bool:     return "bool";
        case TypeKind::Anyptr:   return "anyptr";
        case TypeKind::Pointer: {
            const auto *pointee = program.pointee_at(t.pointee_index);
            return "*" + (pointee ? describe_type(*pointee, program) : std::string("?"));
        }
        case TypeKind::Slice: {
            const auto *info = program.slice_at(t.slice_index);
            return "[]" + (info ? describe_type(info->element_type, program) : std::string("?"));
        }
        case TypeKind::Array: {
            const auto *info = program.array_at(t.array_index);
            return info ? std::format("[{}]{}", info->count, describe_type(info->element_type, program)) : "[]?";
        }
        case TypeKind::Union: {
            if (const auto *info = program.union_at(t.union_index); info && info->is_error_union) {
                std::string out = info->is_optional ? "?error(" : "error(";
                for (size_t i = 0; i < info->error_member_types.size(); ++i) {
                    if (i > 0) out += " | ";
                    out += describe_type(info->error_member_types[i], program);
                }
                return out + ")";
            }
            [[fallthrough]];
        }
        case TypeKind::Struct:
        case TypeKind::Enum:
        case TypeKind::Trait: {
            const auto [mod, name] = find_type_module_and_name(t, program);
            return name.empty() ? "<unknown type>" : name;
        }
        case TypeKind::Function: return "fn(...)";
        case TypeKind::Opaque:
            // Shown while eagerly checking a generic declaration, where no concrete type
            // exists yet. Prefer the parameter's own spelling — 'T' reads far better in a
            // diagnostic than any description of it — falling back to the bound, then to a
            // bare placeholder. Mirrors the LSP's type_to_string (type_printer.cpp) exactly;
            // the two format the same types for different audiences and must agree.
            if (const auto *name = program.opaque_param_name_at(t.opaque_param_index)) {
                return *name;
            }
            if (t.trait_index >= 0) {
                if (const auto *info = program.trait_at(t.trait_index)) {
                    return std::format("<generic: {}>", info->name);
                }
            }
            return "<generic>";
        default: return "<type>";
        }
    }

    auto describe_signature(const bool is_mut_self, const std::vector<ResolvedType> &params,
                             const std::vector<ResolvedType> &returns, const Program &program) -> std::string {
        std::string s = is_mut_self ? "(mut self" : "(self";
        for (const auto &p : params) {
            s += ", " + describe_type(p, program);
        }
        s += ")";
        if (!returns.empty()) {
            s += " -> ";
            if (returns.size() == 1) {
                s += describe_type(returns[0], program);
            } else {
                s += "(";
                for (size_t i = 0; i < returns.size(); ++i) {
                    if (i) s += ", ";
                    s += describe_type(returns[i], program);
                }
                s += ")";
            }
        }
        return s;
    }

    // Compares two trait methods' signatures for the purposes of composition flattening
    // (layout_trait, type_resolver.cpp): same self/mut-self, same param types in order, same
    // return types. Default values are deliberately excluded (matching the existing trait-impl
    // conformance check's own omission, sema.cpp's resolve_trait_impl_signatures_for_program) —
    // two methods differing only in a default value merge cleanly, no collision.
    auto trait_methods_conflict(const TraitMethodInfo &a, const TraitMethodInfo &b) -> bool {
        return a.is_mut_self != b.is_mut_self || a.params != b.params || a.return_types != b.return_types;
    }

    namespace {
        // Resolves 'ef's declared param/return types exactly once (idempotent via
        // 'is_resolved'). Extracted out of resolve_signatures_for_module's inline body so
        // a bare-import alias can force the TRUE ORIGIN'S ExtFunctionSymbol to resolve
        // using the origin's own module_path as context (see resolve_signatures_for_module
        // below), instead of resolving relative to whichever module's table happens to be
        // iterating it — a param/return type naming another type local to the origin
        // module must resolve there, not in the importer.
        void resolve_ext_function_symbol(const std::string &module_path, ExtFunctionSymbol &ef, Program &program, DiagnosticEngine &diag) {
            if (ef.is_resolved) return;

            for (auto &p : ef.decl->params) {
                const auto pt = resolve_type(p.type, module_path, program, diag);
                if (p.default_value) {
                    diag.report_error(DiagnosticStage::Sema, p.location,
                        "'ext fn' declarations may not have default parameter values. "
                        "Default arguments have no C ABI representation.");
                }
                if (pt.kind == TypeKind::Union) {
                    diag.report_error(DiagnosticStage::Sema, p.location, "union types are not yet supported in extern function signatures");
                }
                if (pt.kind == TypeKind::Trait) {
                    diag.report_error(DiagnosticStage::Sema, p.location, "trait handles have no C ABI representation and cannot appear in 'ext fn' signatures");
                }
                if (pt.kind == TypeKind::Function) {
                    const auto *sig = program.fn_signature_at(pt.fn_index);
                    if (sig && sig->return_types.size() > 1) {
                        diag.report_error(DiagnosticStage::Sema, p.location, "multi-return function types cannot be used in extern function signatures (no C ABI representation)");
                    }
                }
                ef.params.push_back(pt);
            }

            if (ef.decl->return_type) {
                const auto rt = resolve_type(*ef.decl->return_type, module_path, program, diag);
                if (rt.kind == TypeKind::Union) {
                    diag.report_error(DiagnosticStage::Sema, ef.decl->location, "union types are not yet supported in extern function signatures");
                }
                if (rt.kind == TypeKind::Trait) {
                    diag.report_error(DiagnosticStage::Sema, ef.decl->location, "trait handles have no C ABI representation and cannot appear in 'ext fn' signatures");
                }
                if (rt.kind == TypeKind::Function) {
                    const auto *sig = program.fn_signature_at(rt.fn_index);
                    if (sig && sig->return_types.size() > 1) {
                        diag.report_error(DiagnosticStage::Sema, ef.decl->location, "multi-return function types cannot be used in extern function signatures (no C ABI representation)");
                    }
                }
                ef.return_type = rt;
            }

            ef.is_variadic = ef.decl->is_variadic;
            ef.is_resolved = true;
        }

        void resolve_signatures_for_module(const std::string &module_path, ProgramModule &module, Program &program, DiagnosticEngine &diag) {
            // SEMA-8, resolved: there used to be a type-forcing loop here, kept because it
            // redirected a bare-import alias to its origin's (module_path, symbol_name) --
            // unlike ensure_module_declared's otherwise-identical loop, which always resolves
            // under the importing module's path. Whether that redirect was load-bearing or
            // dead had opposite fixes, and nothing in the corpus could tell them apart.
            //
            // examples/example_bare_import_private_field_type is that discriminating case: a
            // bare-imported concrete type whose field types are PRIVATE to the defining
            // module, so laying it out under the importer's path could not resolve them. It
            // passes with the loop deleted entirely, because the redirect could never fire
            // and was never needed:
            //
            //   - check_program runs ensure_module_declared for EVERY module before this
            //     pass, and that already forces layout for every non-generic type symbol.
            //     Every layout path early-returns on layout_done, so this loop was a pure
            //     repeat.
            //   - the alias is laid out under the ORIGIN's path regardless, because
            //     declare_bare_import calls ensure_module_declared(target) BEFORE reading the
            //     target's pub symbols (sema_declare.cpp) -- the origin has already laid
            //     itself out, in its own resolution context, at the moment the alias is
            //     created. There is nothing left for a redirect to correct.
            //
            // Note this is specific to the TYPE loop. The function/ext-fn loop below and
            // resolve_values_for_module do the same origin lookup but then copy the resolved
            // symbol back into the alias, which is load-bearing and unaffected by layout_done.

            for (auto &[name, sym] : module.symbols) {
                if (auto *fn = std::get_if<FunctionSymbol>(&sym)) {
                    // A generic function's signature has no single "the" resolution to force
                    // eagerly here — 'N' in '-> [N]u8' only means anything once a concrete
                    // instantiation is requested (see instantiate_generic_function,
                    // sema_check.cpp), which resolves each instance's own signature fresh
                    // with its own GenericBindingEnv active.
                    if (fn->decl && !fn->decl->generic_params.empty()) continue;
                    if (const auto origin = module.bare_import_origins.find(name); origin != module.bare_import_origins.end()) {
                        auto &origin_sym = ensure_function_signature_resolved(origin->second.module_path, origin->second.symbol_name, program, diag);
                        *fn = origin_sym;
                        fn->is_pub = false; // never re-export through the alias
                    } else {
                        ensure_function_signature_resolved(module_path, name, program, diag);
                    }
                } else if (auto *ef = std::get_if<ExtFunctionSymbol>(&sym)) {
                    if (const auto origin = module.bare_import_origins.find(name); origin != module.bare_import_origins.end()) {
                        auto &origin_sym = std::get<ExtFunctionSymbol>(program.modules.at(origin->second.module_path).symbols.at(origin->second.symbol_name));
                        resolve_ext_function_symbol(origin->second.module_path, origin_sym, program, diag);
                        *ef = origin_sym;
                        ef->is_pub = false;
                    } else {
                        resolve_ext_function_symbol(module_path, *ef, program, diag);
                    }
                }
            }
        }

        // Snapshots a module's symbol NAMES, for loops whose body can check expressions.
        //
        // A plain 'for (auto &[name, sym] : module.symbols)' around anything that checks an
        // expression is undefined behaviour, because the loop body can INSERT into the very map
        // it is iterating:
        //
        //   - shadow_generic_type_params (sema_check.cpp) writes each generic parameter's name
        //     into module.symbols for the duration of one instantiation, so any body that
        //     instantiates a generic — 'format_int[T]' called from a plain function — mutates
        //     the map mid-iteration;
        //   - ensure_module_declared inserts into Program::modules, which can invalidate the
        //     'ProgramModule &' the loop is reading through.
        //
        // Either insert can rehash, and a rehash invalidates every outstanding iterator. This
        // stayed hidden for a long time because whether an insert actually rehashes depends on
        // the symbol count relative to the bucket load factor: adding one unrelated 'pub' symbol
        // to a bare-imported module was enough to flip a working build into checking exactly one
        // function of a module and silently abandoning the other fourteen — which surfaced much
        // later as an 'unordered_map::at' crash in codegen, emitting a body sema never typed.
        //
        // Callers re-look-up both the module (by path) and each symbol (by name) per iteration —
        // see module_symbol_for below. Same defence find_type_info_union (sema_check.cpp) already
        // uses when iterating Program::modules across a call that can declare a module.
        auto snapshot_symbol_names(const ProgramModule &module) -> std::vector<std::string> {
            std::vector<std::string> names;
            names.reserve(module.symbols.size());
            for (const auto &name : module.symbols | std::views::keys) {
                names.push_back(name);
            }
            return names;
        }

        // Re-resolves (module_path, name) after a snapshot. Yields nulls when the module or the
        // symbol is gone — most obviously a generic-parameter shadow that has since been
        // restored, since those names only exist while one instantiation is in flight.
        auto module_symbol_for(Program &program, const std::string &module_path, const std::string &name)
            -> std::pair<ProgramModule *, Symbol *> {
            const auto mod_it = program.modules.find(module_path);
            if (mod_it == program.modules.end()) return {nullptr, nullptr};
            const auto sym_it = mod_it->second.symbols.find(name);
            if (sym_it == mod_it->second.symbols.end()) return {&mod_it->second, nullptr};
            return {&mod_it->second, &sym_it->second};
        }

        void resolve_values_for_module(const std::string &module_path, ProgramModule &module, Program &program, DiagnosticEngine &diag) {
            // Snapshot + per-iteration re-lookup: a global's initializer is an expression, so
            // resolve_global_symbol can instantiate a generic and shadow its parameter names
            // into this very map. See snapshot_symbol_names.
            for (const auto &name : snapshot_symbol_names(module)) {
                auto [mod, sym] = module_symbol_for(program, module_path, name);
                if (!mod || !sym) continue;

                if (auto *g = std::get_if<GlobalSymbol>(sym)) {
                    const auto loc = g->decl->location;
                    if (const auto origin = mod->bare_import_origins.find(name); origin != mod->bare_import_origins.end()) {
                        const auto origin_module = origin->second.module_path;
                        const auto origin_name = origin->second.symbol_name;
                        resolve_global_symbol(origin_module, origin_name, program, diag, loc);
                        // Re-resolve both sides: the call above can rehash either map.
                        auto [mod_after, sym_after] = module_symbol_for(program, module_path, name);
                        if (!sym_after) continue;
                        auto *g_after = std::get_if<GlobalSymbol>(sym_after);
                        if (!g_after) continue;
                        *g_after = std::get<GlobalSymbol>(program.modules.at(origin_module).symbols.at(origin_name));
                        g_after->is_pub = false;
                    } else {
                        resolve_global_symbol(module_path, name, program, diag, loc);
                    }
                } else if (auto *m = std::get_if<MacroSymbol>(sym)) {
                    const auto loc = m->decl->location;
                    if (const auto origin = mod->bare_import_origins.find(name); origin != mod->bare_import_origins.end()) {
                        const auto origin_module = origin->second.module_path;
                        const auto origin_name = origin->second.symbol_name;
                        const auto origin_sym = resolve_macro_symbol(origin_module, origin_name, program, diag, loc);
                        auto [mod_after, sym_after] = module_symbol_for(program, module_path, name);
                        if (!sym_after) continue;
                        auto *m_after = std::get_if<MacroSymbol>(sym_after);
                        if (!m_after) continue;
                        *m_after = origin_sym;
                        m_after->is_pub = false;
                    } else {
                        resolve_macro_symbol(module_path, name, program, diag, loc);
                    }
                }
            }
        }

        // Whether '(module, name)' names a type declared with generic parameters — one whose
        // methods have no resolvable signature until an instantiation binds them.
        auto is_generic_type_decl(const std::string &module_path, const std::string &name, const Program &program) -> bool {
            const auto mod_it = program.modules.find(module_path);
            if (mod_it == program.modules.end()) return false;
            const auto sym_it = mod_it->second.symbols.find(name);
            if (sym_it == mod_it->second.symbols.end()) return false;
            const auto *ts = std::get_if<TypeSymbol>(&sym_it->second);
            return ts && ts->decl && !ts->decl->generic_params.empty();
        }

    }

    // Declared in sema.hpp — shared with sema_check.cpp's eager template pass, which performs
    // this same check for a generic target once its parameters are bound.
    void report_trait_conformance_mismatch(const MethodInfo &impl_method, const TraitMethodInfo &trait_method,
                                            const std::string &trait_name, const Program &program, DiagnosticEngine &diag) {
            const bool mismatch = impl_method.is_mut_self != trait_method.is_mut_self ||
                                   impl_method.is_variadic ||
                                   impl_method.param_types != trait_method.params ||
                                   impl_method.return_types != trait_method.return_types;
            if (!mismatch) return;
            diag.report_error(DiagnosticStage::Sema, impl_method.decl->location,
                std::format(
                    "method '{}' does not match trait '{}': expected {}, found {}",
                    trait_method.name, trait_name,
                    describe_signature(trait_method.is_mut_self, trait_method.params, trait_method.return_types, program),
                    describe_signature(impl_method.is_mut_self, impl_method.param_types, impl_method.return_types, program)));
    }

    namespace {
        // Resolves one method's self, parameter and return types into 'info'.
        //
        // Stops deliberately short of the DEFAULTS policy, which is the one place the two
        // callers genuinely diverge: a bare 'impl' method's defaults are its own
        // (check_param_defaults), while a trait-impl method must never declare any — all
        // defaulting for a trait-backed method flows from the trait's own TraitMethodInfo. That
        // divergence is real and stays at the call sites; everything above it was two copies.
        void resolve_method_signature(MethodInfo &info, const ResolvedType &self_type, const std::string &module_path,
                                       Program &program, DiagnosticEngine &diag) {
            info.self_type = self_type;

            for (auto &p : info.decl->params) {
                ResolvedType pt;
                if (p.type) {
                    pt = resolve_type(*p.type, module_path, program, diag);
                } else {
                    // ':=' inferred-type param — infer from the (required) default expr.
                    LocalScope empty;
                    pt = check_expr(*p.default_value, empty, module_path, program, diag, std::nullopt, 0);
                }
                if (p.is_variadic) {
                    info.is_variadic = true;
                    info.variadic_element_type = pt;
                    info.param_types.push_back(intern_slice(program, pt));
                } else {
                    info.param_types.push_back(pt);
                }
            }

            for (auto &rt : info.decl->return_types) {
                info.return_types.push_back(resolve_type(rt, module_path, program, diag));
            }
        }

        void resolve_impl_signatures_for_module(const std::string &module_path, ProgramModule &module, Program &program, DiagnosticEngine &diag) {
            for (auto &[type_name, method_map] : module.methods) {
                // A generic type's methods have no single "the" signature to resolve eagerly
                // here — a method's param/return/self types only make sense once a concrete
                // instantiation is known. Leave every MethodInfo in this map untouched
                // (self_type/param_types/return_types/is_resolved all stay at their defaults);
                // instantiate_generic_method builds a fresh, fully-substituted signature
                // directly from 'info.decl' on demand, per concrete instantiation, instead.
                if (const auto sym_it = module.symbols.find(type_name); sym_it != module.symbols.end()) {
                    if (const auto *ts = std::get_if<TypeSymbol>(&sym_it->second)) {
                        if (ts->decl && !ts->decl->generic_params.empty()) continue;
                    }
                }

                // Resolve the self type for this type name
                const auto self_type = resolve_type_symbol(module_path, type_name, program, diag, {});

                for (auto &info : method_map | std::views::values) {
                    if (info.is_resolved) continue;

                    resolve_method_signature(info, self_type, module_path, program, diag);

                    check_param_defaults(info.decl->params, info.param_types, info.required_params,
                                          info.param_default_is_const, module_path, program, diag);

                    info.is_resolved = true;
                }
            }
        }

        // Resolves every trait-impl method's signature and checks it conforms to the
        // trait it implements: same name, same self/mut self, exactly matching resolved
        // param and return types. Conformance is checked against THIS impl block's own
        // methods only (not the type's bare 'impl' block) — trait impls are meant to be
        // an exact mirror of the trait's method set; anything extra belongs in the bare
        // impl instead (see the "extra method" diagnostic below).
        void resolve_trait_impl_signatures_for_program(Program &program, DiagnosticEngine &diag) {
            for (auto &impls : program.trait_impls_by_type | std::views::values) {
                for (auto &impl_info : impls) {
                    // A GENERIC target's methods have no single "the" signature to resolve
                    // here: 'fn draw(self) -> T' only means something once T is bound. Resolving
                    // the bare name 'Box' with no arguments would force its right-hand side to
                    // be laid out with nothing bound, reporting "unknown type 'T'" against the
                    // type DECLARATION. Mirrors resolve_impl_signatures_for_module's identical
                    // guard for bare 'impl' blocks.
                    //
                    // Signatures and bodies for these are resolved by
                    // check_generic_type_method_bodies instead, against a template receiver with
                    // the parameters unbound — which is also where their trait conformance is
                    // checked, since that comparison needs resolved types.
                    const bool target_is_generic = is_generic_type_decl(impl_info.type_module, impl_info.type_name, program);

                    if (!target_is_generic) {
                        const auto self_type = resolve_type_symbol(impl_info.type_module, impl_info.type_name, program, diag, impl_info.location);

                        for (auto &info : impl_info.methods | std::views::values) {
                            if (info.is_resolved) continue;

                            resolve_method_signature(info, self_type, impl_info.impl_module, program, diag);

                            // A trait-impl method must never declare its own defaults — see the
                            // redeclare/add-without-trait validation below, matched against the
                            // trait's own defaults. required_params is always the full count here;
                            // all defaulting for a trait-backed method flows from TraitMethodInfo.
                            info.required_params = info.param_types.size();
                            info.param_default_is_const.assign(info.param_types.size(), false);

                            info.is_resolved = true;
                        }
                    }

                    const auto *trait_info = program.trait_at(impl_info.trait_index);
                    if (!trait_info) continue;

                    for (size_t trait_method_index = 0; trait_method_index < trait_info->methods.size(); ++trait_method_index) {
                        const auto &trait_method = trait_info->methods[trait_method_index];
                        const auto it = impl_info.methods.find(trait_method.name);
                        if (it == impl_info.methods.end()) {
                            diag.report_error(DiagnosticStage::Sema, impl_info.location,
                                std::format("missing implementation of trait method '{}' in 'impl {} for {}'",
                                    trait_method.name, impl_info.trait_name, impl_info.type_name));
                            continue;
                        }

                        auto &impl_method = it->second;
                        impl_method.trait_index = impl_info.trait_index;
                        impl_method.trait_method_index = static_cast<int>(trait_method_index);

                        // The signature comparison needs RESOLVED types, which a generic
                        // target's methods do not have yet. Deferred to
                        // check_generic_type_method_bodies, which has the template receiver;
                        // everything else in this loop is structural and applies to both.
                        if (!target_is_generic) {
                            report_trait_conformance_mismatch(impl_method, trait_method, impl_info.trait_name, program, diag);
                        }

                        for (size_t i = 0; i < impl_method.decl->params.size() && i < trait_method.params.size(); ++i) {
                            if (!impl_method.decl->params[i].default_value) continue;

                            if (i >= trait_method.required_params) {
                                diag.report_error(DiagnosticStage::Sema, impl_method.decl->params[i].location, std::format(
                                    "parameter '{}' in '{}.{}' redeclares a default value already declared by the trait "
                                    "'{}'. Remove the default from the implementation — it is inherited from the trait.",
                                    impl_method.decl->params[i].name, impl_info.type_name, trait_method.name, impl_info.trait_name));
                            } else {
                                diag.report_error(DiagnosticStage::Sema, impl_method.decl->params[i].location, std::format(
                                    "parameter '{}' in '{}.{}' declares a default value but the corresponding trait method "
                                    "'{}.{}' does not. Defaults on trait implementations must match the trait declaration.",
                                    impl_method.decl->params[i].name, impl_info.type_name, trait_method.name,
                                    impl_info.trait_name, trait_method.name));
                            }
                        }
                    }

                    for (const auto &[method_name, impl_method] : impl_info.methods) {
                        const bool declared_by_trait = std::ranges::any_of(trait_info->methods,
                            [&](const TraitMethodInfo &m) { return m.name == method_name; });
                        if (!declared_by_trait) {
                            diag.report_error(DiagnosticStage::Sema, impl_method.decl->location,
                                std::format("extra method '{}' not declared by trait '{}'; move it to 'impl {} {{ }}' instead",
                                    method_name, impl_info.trait_name, impl_info.type_name));
                        }
                    }
                }
            }
        }

        void check_struct_field_defaults_for_module(const std::string &module_path, ProgramModule &module, Program &program, DiagnosticEngine &diag) {
            // Snapshot + per-iteration re-lookup: a field default is an expression, so check_expr
            // below can instantiate a generic and shadow its parameter names into this very map.
            // See snapshot_symbol_names.
            for (const auto &name : snapshot_symbol_names(module)) {
                const auto [mod, sym] = module_symbol_for(program, module_path, name);
                if (!mod || !sym) continue;

                // A bare-import alias shares its struct's global index with the origin —
                // the origin module's own pass over this loop already checks these field
                // defaults once, correctly, under the origin's context.
                if (mod->bare_import_origins.contains(name)) continue;
                const auto *ts = std::get_if<TypeSymbol>(sym);
                if (!ts || !ts->resolved || ts->resolved->kind != TypeKind::Struct) continue;

                const auto *struct_decl = std::get_if<std::unique_ptr<ast::StructType>>(&ts->decl->type);
                if (!struct_decl) continue;

                const auto struct_index = ts->resolved->struct_index;
                LocalScope empty;

                for (size_t i = 0; i < (*struct_decl)->fields.size(); ++i) {
                    const auto &field = (*struct_decl)->fields[i];
                    if (!field.init) continue;

                    // Re-fetch through the index and copy the field type by value on every
                    // iteration, rather than holding a reference into Program::structs across
                    // the loop.
                    //
                    // check_expr takes a mutable Program&, and a field default that is the
                    // first use of some generic instantiation makes instantiate_generic_type
                    // push_back onto program.structs -- a plain std::vector, unlike the
                    // node-stable vector<unique_ptr<...>> generic_fn_instances deliberately
                    // uses to avoid exactly this. That reallocation invalidates any StructInfo
                    // reference held across the call, so the following is_assignable() read
                    // (and every later iteration's) was a use-after-free.
                    const auto *struct_info = program.struct_at(struct_index);
                    if (!struct_info || i >= struct_info->fields.size()) break;
                    const auto field_type = struct_info->fields[i].type;

                    const auto init_ty = check_expr(*field.init, empty, module_path, program, diag, field_type, 0);
                    if (!is_assignable(init_ty, field_type)) {
                        diag.report_error(DiagnosticStage::Sema, field.location,
                                          std::format("default value type mismatch for field '{}'", field.name));
                    }
                }
            }
        }

        auto stmt_always_returns(const ast::Stmt &stmt) -> bool;

        // A block guarantees a return if ANY statement in it does - whatever follows is
        // unreachable, which is a separate (unchecked) concern here.
        auto block_always_returns(const std::vector<ast::Stmt> &stmts) -> bool {
            return std::ranges::any_of(stmts, [](const ast::Stmt &s) { return stmt_always_returns(s); });
        }

        // 'when cond { ... } else (when ... | { ... })' compiles BOTH branches unconditionally
        // (a hard sema requirement - see spec.md's "Compile-Time Configuration"), so unlike a
        // loop or switch this is exactly as safe to treat like if/else: guarantees a return
        // only when an else branch is present and every branch does.
        auto when_always_returns(const ast::WhenStmt &when) -> bool {
            if (!when.else_branch) return false;
            if (!block_always_returns(when.then_block.stmts)) return false;
            return std::visit(
                [&]<typename T>(const T &else_v) -> bool {
                    using V = std::decay_t<T>;
                    if constexpr (std::is_same_v<V, ast::BlockStmt>) {
                        return block_always_returns(else_v.stmts);
                    } else {
                        return when_always_returns(*else_v);
                    }
                },
                *when.else_branch);
        }

        // Conservative "does this statement guarantee a return" check, used only for the
        // early missing-return WARNING below - codegen's own terminator check
        // (report_codegen_error's "may fall through" sites, driven by LLVM's own CFG state
        // after emission) remains the sole authoritative/blocking check; this one exists so
        // the common cases are visible in sema (and therefore the LSP, which never runs
        // codegen - see analysis.cpp) instead of only surfacing at a full compile.
        // Deliberately under-approximates: a loop (WhileStmt/ForInStmt), SwitchStmt, or a
        // MatchExpr used as an ExprStmt is never treated as guaranteed-returning here, even
        // when it provably is (e.g. 'while true {}' with no break, or an exhaustive match
        // where every arm returns) - a false positive (an extra warning codegen would
        // accept) is an acceptable cost of this heuristic; a false negative (silently
        // missing a real fall-through) is not.
        auto stmt_always_returns(const ast::Stmt &stmt) -> bool {
            return std::visit(
                [&]<typename T>(const T &v) -> bool {
                    using V = std::decay_t<T>;
                    if constexpr (std::is_same_v<V, ast::ReturnStmt> || std::is_same_v<V, ast::ReturnErrStmt> ||
                                  std::is_same_v<V, ast::ReturnOkStmt>) {
                        return true;
                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::BlockStmt>>) {
                        return block_always_returns(v->stmts);
                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::IfStmt>>) {
                        return v->else_stmt && stmt_always_returns(v->then_stmt) && stmt_always_returns(*v->else_stmt);
                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::WhenStmt>>) {
                        return when_always_returns(*v);
                    } else {
                        // WhileStmt, ForInStmt, SwitchStmt, DeferStmt, ExprStmt, VarDeclStmt(Group),
                        // Break/Continue, LinkDecl/DiagnosticDecl, AsmStmt - none proven to
                        // guarantee a return by this conservative check.
                        return false;
                    }
                },
                stmt);
        }

        // Emits the same-shaped warning as codegen's authoritative "may fall through" error
        // (see report_codegen_error's three call sites), but as a non-blocking sema WARNING,
        // early enough to reach the LSP - never emitted for a void-returning body, and never
        // itself a compile error (a false positive here must not break an otherwise-valid
        // build; codegen's own check is what actually gates compilation).
        void warn_if_may_fall_through(const std::string &kind, const std::string &name, const ast::Stmt &body,
                                      const std::vector<ResolvedType> &return_types, const SourceLocation &location, DiagnosticEngine &diag) {
            if (return_types.empty() || stmt_always_returns(body)) return;
            diag.warn(DiagnosticStage::Sema, location,
                      std::format("{} '{}' may fall through without returning a value", kind, name));
        }

        // Every body in a module starts with that module's globals in scope.
        auto globals_in_scope(const ProgramModule &module) -> LocalScope {
            LocalScope locals;
            for (const auto &[name, sym] : module.symbols) {
                if (const auto *g = std::get_if<GlobalSymbol>(&sym)) {
                    locals[name] = LocalBinding{.type = g->type, .is_mut = g->is_mut};
                }
            }
            return locals;
        }

        // Type-checks one method body: globals in scope, 'self' bound as a POINTER to the
        // receiver, then the declared parameters.
        //
        // Shared by check_bodies_for_module (for 'impl TYPE') and
        // check_trait_impl_bodies_for_program (for 'impl TRAIT for TYPE'). Those two differ
        // only in where the method table lives and which module supplies the globals — not in
        // what checking a method body means, which is what this being one function asserts.
        void check_method_body(MethodInfo &info, const std::string &module_path, const ProgramModule &module,
                                Program &program, DiagnosticEngine &diag) {
            LocalScope locals = globals_in_scope(module);
            locals["self"] = LocalBinding{.type = intern_pointer(program, info.self_type), .is_mut = info.is_mut_self};

            for (size_t i = 0; i < info.decl->params.size(); ++i) {
                locals[info.decl->params[i].name] = LocalBinding{
                    .type = info.param_types[i],
                    .is_mut = info.decl->params[i].is_mut,
                };
            }

            check_stmt(info.decl->body, locals, module_path, program, diag, info.return_types, 0);
            warn_if_may_fall_through("method", info.decl->name, info.decl->body, info.return_types, info.decl->location, diag);
        }

        void check_bodies_for_module(const std::string &module_path, ProgramModule &module, Program &program, DiagnosticEngine &diag) {
            // Snapshot + per-iteration re-lookup: check_stmt below instantiates generics, which
            // shadow their parameter names into this very map. See snapshot_symbol_names.
            for (const auto &name : snapshot_symbol_names(module)) {
                const auto [mod, sym] = module_symbol_for(program, module_path, name);
                if (!mod || !sym) continue;

                // A bare-import alias shares its 'decl' AST pointer with the origin — the
                // origin module's own pass over this loop already type-checks that body
                // once, correctly, under the origin's context. Re-checking it here (under
                // the importer's context) would write into the WRONG per-module
                // 'expr_types' side table and spuriously fail to resolve any private
                // sibling the origin's body references.
                if (mod->bare_import_origins.contains(name)) continue;
                const auto *fn = std::get_if<FunctionSymbol>(sym);
                if (!fn) {
                    continue;
                }
                // A generic function's body is checked per concrete instantiation, the
                // first time that instantiation is requested — see
                // instantiate_generic_function -> check_generic_function_instance_body
                // (sema_check.cpp). It is ALSO checked once at its own declaration with its
                // parameters unbound, by check_generic_templates_for_program; that is a
                // separate pass, and this loop is only about concrete code.
                if (fn->decl && !fn->decl->generic_params.empty()) continue;

                LocalScope locals = globals_in_scope(*mod);
                for (size_t i = 0; i < fn->decl->params.size(); ++i) {
                    locals[fn->decl->params[i].name] = LocalBinding{.type = fn->params[i], .is_mut = fn->decl->params[i].is_mut};
                }

                // 'fn' points into the map check_stmt may rehash, so copy out everything the
                // call and the warning below need before making it.
                const auto *decl = fn->decl;
                const auto return_types = fn->return_types;

                check_stmt(decl->body, locals, module_path, program, diag, return_types, 0);
                warn_if_may_fall_through("function", decl->name, decl->body, return_types, decl->location, diag);
            }

            // Check impl method bodies. 'methods' is keyed by type name and each MethodInfo is
            // reached by reference, so the same rehash hazard applies — snapshot the keys and
            // re-look-up per iteration.
            std::vector<std::pair<std::string, std::string>> method_keys;
            for (const auto &[type_name, method_map] : module.methods) {
                for (const auto &method_name : method_map | std::views::keys) {
                    method_keys.emplace_back(type_name, method_name);
                }
            }
            for (const auto &[type_name, method_name] : method_keys) {
                const auto mod_it = program.modules.find(module_path);
                if (mod_it == program.modules.end()) continue;
                const auto type_it = mod_it->second.methods.find(type_name);
                if (type_it == mod_it->second.methods.end()) continue;
                const auto method_it = type_it->second.find(method_name);
                if (method_it == type_it->second.end()) continue;
                if (!method_it->second.is_resolved) continue;
                check_method_body(method_it->second, module_path, mod_it->second, program, diag);
            }
        }

        // Trait-impl method bodies live in a Program-level registry (not per-module
        // ProgramModule::methods), so they need their own body-check pass mirroring the
        // tail of check_bodies_for_module above. Locals/globals-in-scope come from the
        // module the 'impl TRAIT for TYPE' block itself is written in.
        void check_trait_impl_bodies_for_program(Program &program, DiagnosticEngine &diag) {
            for (auto &impls : program.trait_impls_by_type | std::views::values) {
                for (auto &impl_info : impls) {
                    const auto mod_it = program.modules.find(impl_info.impl_module);
                    if (mod_it == program.modules.end()) continue;

                    for (auto &info : impl_info.methods | std::views::values) {
                        if (!info.is_resolved) continue;
                        check_method_body(info, impl_info.impl_module, mod_it->second, program, diag);
                    }
                }
            }
        }
    }

    auto check_program(const ast::Program &program, DiagnosticEngine &diag, const Options &options) -> Program {
        if (!program.ok) {
            return {};
        }

        Program out;
        out.options = options;
        seed_builtin_type_ids(out);

        // Reentrant/memoized rather than a flat loop: a module-scope 'when' condition may
        // reference another module's const (e.g. 'opts.target_os'), which needs that
        // module's symbol table built (and its value resolved) on demand, regardless of
        // Program::modules' unordered iteration order — see ensure_module_declared.
        for (const auto &path : program.modules | std::views::keys) {
            ensure_module_declared(program, path, out, diag);
        }

        // Runs after every module's symbol table (types + bare-impl method names) is
        // built, since coherence/collision/duplicate-impl checks need to see across
        // the whole program, not just one module at a time.
        register_trait_impls_for_program(program, out, diag);

        for (const auto &path : program.modules | std::views::keys) {
            resolve_signatures_for_module(path, out.modules.at(path), out, diag);
        }

        for (const auto &path : program.modules | std::views::keys) {
            resolve_impl_signatures_for_module(path, out.modules.at(path), out, diag);
        }

        resolve_trait_impl_signatures_for_program(out, diag);

        // Every free-function/impl-method/trait-impl-method signature is resolved by this
        // point, but body-checking hasn't started — exactly what the five attributes' own
        // checks need (resolved return types, raw AST param/attribute/body shape).
        for (const auto &path : program.modules | std::views::keys) {
            validate_attributes_for_module(path, out.modules.at(path), out, diag);
            validate_method_attributes_for_module(path, out.modules.at(path), out, diag);
        }
        validate_trait_impl_attributes_for_program(out, diag);

        for (const auto &path : program.modules | std::views::keys) {
            resolve_values_for_module(path, out.modules.at(path), out, diag);
        }

        for (const auto &path : program.modules | std::views::keys) {
            check_struct_field_defaults_for_module(path, out.modules.at(path), out, diag);
        }

        for (const auto &path : program.modules | std::views::keys) {
            check_bodies_for_module(path, out.modules.at(path), out, diag);
        }

        check_trait_impl_bodies_for_program(out, diag);

        // Every generic DECLARATION's own body, checked once with its parameters unbound.
        // Position is load-bearing in all four directions:
        //  - after register_trait_impls/resolve_trait_impl_signatures, since a '[T: Trait]'
        //    bound resolves against the trait's flattened method set;
        //  - after resolve_values_for_module, since the pass seeds its LocalScope from every
        //    GlobalSymbol's resolved type;
        //  - after check_bodies_for_module, so it cannot change WHICH instantiation gets
        //    checked first and reorder diagnostics across unrelated code;
        //  - before the generic field-defaults loop below, which walks to a fixed point over
        //    program.structs and would otherwise re-run over slots this pass created.
        check_generic_templates_for_program(out, diag);

        // After the bodies, because that is where most generic instantiations are created —
        // a generic struct's field defaults can only be checked once its type parameters are
        // bound, so unlike check_struct_field_defaults_for_module above this walks
        // monomorphized slots rather than declarations.
        check_generic_struct_field_defaults_for_program(out, diag);

        // Runs last: the cross-module '@init' reference walk doesn't need type-checked
        // bodies, but keeping it after every other pass makes "last" unambiguous.
        validate_init_dependencies_for_program(program, out, diag);

        out.ok = !diag.has_errors();
        for (auto &module : out.modules | std::views::values) {
            module.ok = out.ok;
        }

        return out;
    }

    auto find_type_module_and_name(const ResolvedType &ty, const Program &program) -> std::pair<std::string, std::string> {
        // The defining module is stored directly on the info struct for every kind. Use
        // it to avoid accidentally matching a bare-import TypeSymbol alias in an
        // importing module that resolves to the same ResolvedType (an alias's '.resolved'
        // is a verbatim copy of the origin's, sharing the same global index).
        const std::string *defining_module = nullptr;
        const GenericInstanceInfo *generic_instance = nullptr;
        if (ty.kind == TypeKind::Struct) {
            if (const auto *info = program.struct_at(ty.struct_index)) {
                defining_module = &info->module_path;
                generic_instance = info->generic_instance ? &*info->generic_instance : nullptr;
            }
        } else if (ty.kind == TypeKind::Union) {
            if (const auto *info = program.union_at(ty.union_index)) {
                defining_module = &info->module_path;
                generic_instance = info->generic_instance ? &*info->generic_instance : nullptr;
            }
        } else if (ty.kind == TypeKind::Enum) {
            if (const auto *info = program.enum_at(ty.enum_index)) {
                defining_module = &info->module_path;
                generic_instance = info->generic_instance ? &*info->generic_instance : nullptr;
            }
        } else if (ty.kind == TypeKind::Bitset) {
            if (const auto *info = program.bitset_at(ty.bitset_index)) {
                defining_module = &info->module_path;
                generic_instance = info->generic_instance ? &*info->generic_instance : nullptr;
            }
        } else if (ty.kind == TypeKind::Trait) {
            if (const auto *info = program.trait_at(ty.trait_index)) defining_module = &info->module_path;
        }

        // A monomorphized generic instantiation has no TypeSymbol of its own to find via
        // the scan below — its TypeSymbol::resolved is left nullopt (see declare_type/
        // instantiate_generic_type), since 'List' alone is never itself a valid type.
        // Return the UNSPECIALIZED declaration's own name directly instead: this is what
        // keeps find_method's existing bare-name keying ("List"/"reserve") working
        // completely unchanged for a generic type's methods (per-instantiation
        // substitution happens after that lookup, at the call site).
        if (generic_instance) {
            return {generic_instance->decl_module, generic_instance->decl_name};
        }

        if (defining_module && !defining_module->empty()) {
            if (const auto mod_it = program.modules.find(*defining_module);
                mod_it != program.modules.end()) {
                for (const auto &[name, sym] : mod_it->second.symbols) {
                    if (const auto *ts = std::get_if<TypeSymbol>(&sym))
                        // Skip a generic-parameter shadow: it is an alias for the duration of
                        // one instantiation, not a declaration, and answering 'T' here would
                        // break every name-keyed lookup (trait impls, methods) for the type it
                        // stands in for. See TypeSymbol::is_generic_param_shadow.
                        if (ts->resolved && *ts->resolved == ty && !ts->is_generic_param_shadow)
                            return {*defining_module, name};
                }
            }
        }

        // Fallback for any ResolvedType kind not covered above (or a not-yet-populated
        // module_path). A bare-import alias TypeSymbol can in principle be matched here —
        // acceptable only because every kind that actually needs precise origin
        // attribution (struct/union/enum/bitset/trait, for find_method et al.) is already
        // handled by the fast path above.
        for (const auto &[path, mod] : program.modules) {
            for (const auto &[name, sym] : mod.symbols) {
                if (const auto *ts = std::get_if<TypeSymbol>(&sym))
                    if (ts->resolved && *ts->resolved == ty && !ts->is_generic_param_shadow)
                        return {path, name};
            }
        }
        return {"", ""};
    }

    auto find_method(const ResolvedType &ty, const std::string &method_name, const Program &program) -> const MethodInfo * {
        const auto [mod_path, type_name] = find_type_module_and_name(ty, program);
        if (mod_path.empty()) return nullptr;

        const auto mod_it = program.modules.find(mod_path);
        if (mod_it == program.modules.end()) return nullptr;

        // Tier 1: the type's own bare 'impl' block.
        if (const auto type_it = mod_it->second.methods.find(type_name); type_it != mod_it->second.methods.end()) {
            if (const auto method_it = type_it->second.find(method_name); method_it != type_it->second.end()) {
                return &method_it->second;
            }
        }

        // Tier 2: trait-impl methods for this type, in declaration order. Collision checks at
        // trait-impl declaration time (register_trait_impls_for_program) guarantee at most one
        // trait impl can supply a given method name for a given type, so there's no ambiguity
        // to resolve by order here in a valid program.
        if (const auto trait_impls_it = program.trait_impls_by_type.find({mod_path, type_name});
            trait_impls_it != program.trait_impls_by_type.end()) {
            for (const auto &impl_info : trait_impls_it->second) {
                if (const auto method_it = impl_info.methods.find(method_name); method_it != impl_info.methods.end()) {
                    return &method_it->second;
                }
            }
        }

        return nullptr;
    }
}
