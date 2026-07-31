#include "sema.hpp"
#include "module_resolver.hpp"

#include <algorithm>
#include <filesystem>
#include <format>
#include <unordered_map>

namespace sema {
    // Ensures 'module_path' has been declared (build_symbol_table_for_module has run
    // for it), declared in sema_declare.cpp — see its doc comment there. Called from
    // this file so a cross-module named type reached before Program::modules'
    // unordered iteration order gets to that module can declare it on demand instead
    // of failing outright.
    void ensure_module_declared(const ast::Program &program, const std::string &module_path, Program &sema_program, DiagnosticEngine &diag, const SourceLocation &trigger_location = {});

    // See sema.hpp's doc comment. Mirrors Resolver::as_named_member's dotted-chain walk below
    // exactly, but starting from an arbitrary Expr (which may be a bare IdentExpr, not just a
    // MemberExpr) and producing a full ast::Type rather than a bare ast::NamedType.
    auto reinterpret_expr_as_type_name(const ast::Expr &expr) -> std::optional<ast::Type> {
        if (const auto *ident = std::get_if<ast::IdentExpr>(&expr)) {
            return ast::Type{ast::NamedType{.name = ident->name, .location = ident->location}};
        }

        const auto *member = std::get_if<std::unique_ptr<ast::MemberExpr>>(&expr);
        if (!member) return std::nullopt;

        std::vector<std::pair<std::string, SourceLocation>> parts;
        const auto collect = [&](this const auto &self, const ast::Expr &e) -> bool {
            if (const auto *inner_ident = std::get_if<ast::IdentExpr>(&e)) {
                parts.emplace_back(inner_ident->name, inner_ident->location);
                return true;
            }
            if (const auto *inner_member = std::get_if<std::unique_ptr<ast::MemberExpr>>(&e)) {
                if (!self((*inner_member)->object)) return false;
                parts.emplace_back((*inner_member)->member, (*inner_member)->location);
                return true;
            }
            return false;
        };

        if (!collect((*member)->object)) return std::nullopt;
        parts.emplace_back((*member)->member, (*member)->location);

        ast::NamedType result{.name = parts.front().first, .location = parts.front().second};
        ast::NamedType *tail = &result;
        for (size_t i = 1; i < parts.size(); ++i) {
            tail->member = std::make_unique<ast::NamedType>(ast::NamedType{.name = parts[i].first, .location = parts[i].second});
            tail = tail->member.get();
        }

        return ast::Type{std::move(result)};
    }

    // Dedupes on ResolvedType::operator==, which ignores opaque_param_index. So in a
    // multi-parameter generic ('fn f[K: type, V: type]'), '*K' and '*V' land on the same
    // interned pointee slot and both print under whichever name was interned first. Known and
    // deliberate: keying interning on the display name instead would make '*K' and '*V'
    // distinct types, which is precisely the strict-vs-permissive change the eager generic
    // check must not make (see TypeKind::Opaque). intern_slice and array interning below share
    // this behaviour for the same reason.
    auto intern_pointer(Program &program, const ResolvedType &pointee) -> ResolvedType {
        for (size_t i = 0; i < program.pointer_pointees.size(); ++i) {
            if (program.pointer_pointees[i] == pointee) {
                return ResolvedType{.kind = TypeKind::Pointer, .pointee_index = static_cast<int>(i)};
            }
        }
        program.pointer_pointees.push_back(pointee);
        return ResolvedType{.kind = TypeKind::Pointer, .pointee_index = static_cast<int>(program.pointer_pointees.size()) - 1};
    }

    auto intern_function_type(Program &program, FunctionTypeInfo sig) -> ResolvedType {
        for (size_t i = 0; i < program.fn_signatures.size(); ++i) {
            const auto &s = program.fn_signatures[i];
            if (s.is_variadic == sig.is_variadic &&
                s.param_types == sig.param_types &&
                s.return_types == sig.return_types) {
                return ResolvedType{.kind = TypeKind::Function, .fn_index = static_cast<int>(i)};
            }
        }
        program.fn_signatures.push_back(std::move(sig));
        return ResolvedType{.kind = TypeKind::Function, .fn_index = static_cast<int>(program.fn_signatures.size()) - 1};
    }

    // Shared tail of the three *_function_pointer_type_of helpers below: interns
    // 'fn(params) -> returns', carrying the declaration's parameter names through as the
    // cosmetic-only FunctionTypeInfo::param_names (they don't participate in interning
    // equality, so a name mismatch never splits a signature into two entries).
    namespace {
        // Every tagged union in the language is laid out as [u32 tag | padding | payload],
        // and this is the one place that says so. Three sites computed it independently
        // before (layout_union's tagged branch, and synthesize_error_union's inner and outer
        // unions), which is a lot of surface for a rule with four interacting parts — the
        // tag's own alignment, the payload's, the offset the first forces on the second, and
        // the tail padding the whole thing rounds up to.
        //
        // 'max_payload_align' of 0 is treated as 1 (a union with no payload at all), and a
        // union that would otherwise be zero-sized gets TAG_ALIGN, so a tag-only union still
        // occupies its tag.
        struct TaggedUnionLayout {
            uint32_t payload_offset;
            uint32_t align;
            uint32_t size;
        };

        constexpr uint32_t TAGGED_UNION_TAG_SIZE = 4;
        constexpr uint32_t TAGGED_UNION_TAG_ALIGN = 4;

        constexpr auto compute_tagged_union_layout(const uint32_t max_payload_size,
                                                    const uint32_t max_payload_align) -> TaggedUnionLayout {
            const uint32_t effective_align = std::max(max_payload_align, 1u);
            const uint32_t payload_offset =
                (TAGGED_UNION_TAG_SIZE + effective_align - 1) / effective_align * effective_align;
            const uint32_t align = std::max(TAGGED_UNION_TAG_ALIGN, max_payload_align);
            const uint32_t raw_size = payload_offset + max_payload_size;
            uint32_t size = (raw_size + align - 1) / align * align;
            if (size == 0) size = TAGGED_UNION_TAG_ALIGN;
            return {.payload_offset = payload_offset, .align = align, .size = size};
        }

        auto intern_decayed_fn_type(Program &program, std::vector<ResolvedType> params,
                                     std::vector<std::string> names, std::vector<ResolvedType> returns,
                                     const bool is_variadic) -> ResolvedType {
            names.resize(params.size());
            return intern_function_type(program, FunctionTypeInfo{
                .param_types = std::move(params),
                .param_names = std::move(names),
                .return_types = std::move(returns),
                .is_variadic = is_variadic,
            });
        }

        template <typename ParamT>
        auto param_names_of(const std::vector<ParamT> &params) -> std::vector<std::string> {
            std::vector<std::string> names;
            names.reserve(params.size());
            for (const auto &p : params) names.push_back(p.name);
            return names;
        }
    }

    auto function_pointer_type_of(const FunctionSymbol &sym, Program &program) -> ResolvedType {
        return intern_decayed_fn_type(program, sym.params,
                                       sym.decl ? param_names_of(sym.decl->params) : std::vector<std::string>{},
                                       sym.return_types, /*is_variadic=*/false);
    }

    auto ext_function_pointer_type_of(const ExtFunctionSymbol &sym, Program &program) -> ResolvedType {
        std::vector<ResolvedType> returns;
        if (sym.return_type) returns.push_back(*sym.return_type);
        return intern_decayed_fn_type(program, sym.params,
                                       sym.decl ? param_names_of(sym.decl->params) : std::vector<std::string>{},
                                       std::move(returns), sym.is_variadic);
    }

    auto generic_instance_function_pointer_type(const GenericFunctionInstance &instance, Program &program) -> ResolvedType {
        auto names = instance.decl      ? param_names_of(instance.decl->params)
                     : instance.impl_decl ? param_names_of(instance.impl_decl->params)
                                          : std::vector<std::string>{};
        return intern_decayed_fn_type(program, instance.param_types, std::move(names),
                                       instance.return_types, /*is_variadic=*/false);
    }

    auto intern_slice(Program &program, const ResolvedType &element) -> ResolvedType {
        for (size_t i = 0; i < program.slices.size(); ++i) {
            if (program.slices[i].element_type == element) {
                return ResolvedType{.kind = TypeKind::Slice, .slice_index = static_cast<int>(i)};
            }
        }
        program.slices.push_back(SliceInfo{.element_type = element});
        return ResolvedType{.kind = TypeKind::Slice, .slice_index = static_cast<int>(program.slices.size()) - 1};
    }

    auto intern_type_id(Program &program, const ResolvedType &t) -> uint64_t {
        if (const auto it = program.type_ids.find(t); it != program.type_ids.end()) {
            return it->second;
        }
        const auto id = program.next_type_id++;
        program.type_ids[t] = id;
        return id;
    }

    void seed_builtin_type_ids(Program &program) {
        // Fixed, compiler-internal numbering for the 15 builtin scalar kinds that can never
        // carry a struct/union/enum-style layout worth reflecting on — distinct from (and
        // unrelated to) the Mirage-level 'runtime/type_info' module's own Type_Kind enum,
        // which numbers its OWN 1-15 range independently for a different purpose (labeling
        // Type_Info variant shapes, not type identity).
        static constexpr TypeKind kBuiltinOrder[] = {
            TypeKind::U8, TypeKind::U16, TypeKind::U32, TypeKind::U64,
            TypeKind::I8, TypeKind::I16, TypeKind::I32, TypeKind::I64,
            TypeKind::F32, TypeKind::F64, TypeKind::USize, TypeKind::Bool,
            TypeKind::Anyptr, TypeKind::Void, TypeKind::Type,
        };
        uint64_t id = 1;
        for (const auto kind : kBuiltinOrder) {
            program.type_ids[ResolvedType{.kind = kind}] = id++;
        }
        program.next_type_id = 16;
    }

    namespace {
        auto intern_array(Program &program, const ResolvedType &element, const uint64_t count, const uint32_t size, const uint32_t align) -> ResolvedType {
            for (size_t i = 0; i < program.arrays.size(); ++i) {
                if (program.arrays[i].element_type == element && program.arrays[i].count == count) {
                    return ResolvedType{.kind = TypeKind::Array, .array_index = static_cast<int>(i)};
                }
            }
            program.arrays.push_back(ArrayInfo{.element_type = element, .count = count, .size = size, .align = align});
            return ResolvedType{.kind = TypeKind::Array, .array_index = static_cast<int>(program.arrays.size()) - 1};
        }

    }

    // Declared in sema.hpp. Public (rather than file-local, as it was) so codegen.cpp can
    // drop its own byte-identical copy and share this one.
    auto primitive_size(const TypeKind kind) -> uint32_t {
        switch (kind) {
        case TypeKind::U8:
        case TypeKind::I8:
        case TypeKind::Bool:
            return 1;

        case TypeKind::U16:
        case TypeKind::I16:
            return 2;

        case TypeKind::U32:
        case TypeKind::I32:
        case TypeKind::F32:
            return 4;

        case TypeKind::U64:
        case TypeKind::I64:
        case TypeKind::F64:
        case TypeKind::USize:
        case TypeKind::Pointer:
        case TypeKind::Anyptr:
        case TypeKind::Function: // code pointer, 8 bytes
        case TypeKind::Type:     // compile-time-unique type identifier, backed by u64
            return 8;

        case TypeKind::Slice:
        case TypeKind::Trait: // fat pointer: {data: anyptr, vtable: *const VTable}, 16 bytes
        case TypeKind::Any:   // fat pointer: {id: type, data: anyptr}, 16 bytes
            return 16;

        // Not a value type and so has no size. Reaching here means a stage above failed to
        // reject it: 'size_of(some_import)' used to land here and silently evaluate to 0.
        // sema_check.cpp's SizeOfExpr/AlignOfExpr cases now report that, so this stays 0
        // purely as the error-recovery value rather than as a meaningful answer.
        case TypeKind::Namespace:
            return 0;

        // An unbound generic parameter has no layout until it is substituted. 0 is the
        // intended answer, not a fallthrough: layout_struct/layout_union already tolerate
        // a zero-size, zero-align member (they seed max_align at 1 and guard on
        // 'f_align > 0'), and no instantiation carrying one is ever emitted.
        case TypeKind::Opaque:
            return 0;

        default:
            return 0;
        }
    }

    auto primitive_align(const TypeKind kind) -> uint32_t { return primitive_size(kind); }

    namespace {
        auto error(DiagnosticEngine &diag, const SourceLocation &loc, std::string msg) -> ResolvedType {
            diag.report_error(DiagnosticStage::Sema, loc, std::move(msg));
            return ResolvedType{
                .kind = TypeKind::Invalid,
            };
        }

        auto expr_location(const ast::Expr &expr) -> SourceLocation {
            return std::visit(
                []<typename T>(const T &v) -> SourceLocation {
                    using V = std::decay_t<T>;
                    if constexpr (requires { v.location; }) {
                        return v.location;
                    } else if constexpr (requires { v->location; }) {
                        return v->location;
                    } else {
                        return {};
                    }
                },
                expr);
        }

        struct ChainTarget {
            std::string module_path;
            std::string name;
            bool crossed_boundary;
            SourceLocation location;
        };

        auto walk_namespace_chain(const std::string &start_module, const ast::NamedType &named, Program &program, DiagnosticEngine &diag, const ast::Program *ast_program) -> std::optional<ChainTarget> {
            std::string current_module = start_module;
            const auto *current = &named;
            bool crossed = false;

            while (current->member != nullptr) {
                auto mod_it = program.modules.find(current_module);
                if (mod_it == program.modules.end() && ast_program) {
                    ensure_module_declared(*ast_program, current_module, program, diag);
                    mod_it = program.modules.find(current_module);
                }
                if (mod_it == program.modules.end()) {
                    diag.report_error(DiagnosticStage::Sema, current->location, std::format("internal error: module '{}' not found", current_module));
                    return std::nullopt;
                }

                auto sym_it = mod_it->second.symbols.find(current->name);
                if (sym_it == mod_it->second.symbols.end()) {
                    diag.report_error(DiagnosticStage::Sema, current->location, std::format("unknown identifier '{}'", current->name));
                    return std::nullopt;
                }

                const auto *imp = std::get_if<ImportSymbol>(&sym_it->second);
                if (!imp) {
                    diag.report_error(DiagnosticStage::Sema, current->location, std::format("'{}' is not a namespace", current->name));
                    return std::nullopt;
                }

                if (crossed && !imp->is_pub) {
                    diag.report_error(DiagnosticStage::Sema, current->location, std::format("'{}' is not pub", current->name));
                    return std::nullopt;
                }

                current_module = imp->module_path;
                current = current->member.get();
                crossed = true;
            }

            return ChainTarget{current_module, current->name, crossed, current->location};
        }

        struct Resolver {
            Program &program;
            DiagnosticEngine &diag;
            // Non-null only when reached from a declare-phase call site that may run
            // before every module is guaranteed declared — see resolve_type's doc
            // comment in sema.hpp. Lets a cross-module reference declare its target
            // module on demand instead of failing on iteration-order bad luck.
            const ast::Program *ast_program = nullptr;
            // Non-null only while resolving inside a specific generic instantiation's own
            // body/signature (see instantiate_generic_type/instantiate_generic_function) —
            // binds each of that instantiation's generic param names to its concrete
            // type/value. Consulted by resolve_type_impl's NamedType case (a bare param
            // reference like 'T') and eval_integer_const_expr's IdentExpr case (a value
            // param reference like 'N').
            const GenericBindingEnv *generic_env = nullptr;
            // Non-null only while resolving inside a declaration that itself carries
            // generic_params (a generic type/fn/impl's own body/signature, NOT one of its
            // instantiations) — supports implicit self-instantiation: a bare reference to a
            // generic name whose arity matches this list is sugar for applying these same
            // params, in order. Mutually exclusive with generic_env being non-null in
            // practice (an instantiation's body has concrete bindings, not its own
            // still-abstract params) but kept as a separate field since they answer
            // different questions (what IS this name bound to vs. what are the enclosing
            // decl's OWN param names, for implicit self-instantiation sugar).
            const std::vector<ast::GenericParam> *enclosing_generic_params = nullptr;

            auto find_type_symbol(ProgramModule &mod, const std::string &name, const SourceLocation &loc) const -> TypeSymbol * {
                const auto it = mod.symbols.find(name);
                if (it == mod.symbols.end()) {
                    error(diag, loc, std::format("unknown type '{}'", name));
                    return nullptr;
                }

                auto *ts = std::get_if<TypeSymbol>(&it->second);
                if (!ts) {
                    error(diag, loc, std::format("'{}' is not a type", name));
                    return nullptr;
                }

                return ts;
            }

            // Non-diagnosing lookup (unlike find_type_symbol above) — nullptr for "not a
            // type symbol" or "not found", used purely to decide whether a NamedType names a
            // generic declaration before committing to a resolution strategy.
            auto find_type_decl_for(const std::string &module_path, const std::string &name) const -> const ast::TypeDecl * {
                auto mod_it = program.modules.find(module_path);
                if (mod_it == program.modules.end() && ast_program) {
                    // Same on-demand fallback as resolve_final_shallow/walk_namespace_chain, and
                    // load-bearing for exactly the same reason. A module reached only through a
                    // NAMED import ('const list := import("core/list")') is never force-declared
                    // the way declare_bare_import's target is, so "absent from program.modules"
                    // here means "not declared YET", not "not generic". Reading it as the latter
                    // sends an instantiation ('list.List[anyptr]') down the non-generic path in
                    // the caller, which lays out the template's struct body with no
                    // GenericBindingEnv ("unknown type 'T'") and caches that in
                    // TypeSymbol::resolved, poisoning every later use of the type.
                    ensure_module_declared(*ast_program, module_path, program, diag);
                    mod_it = program.modules.find(module_path);
                }
                if (mod_it == program.modules.end()) return nullptr;
                const auto sym_it = mod_it->second.symbols.find(name);
                if (sym_it == mod_it->second.symbols.end()) return nullptr;
                const auto *ts = std::get_if<TypeSymbol>(&sym_it->second);
                return ts ? ts->decl : nullptr;
            }

            // A bare (undotted, no generic_args) reference to the active generic_env's own
            // type-param binding (e.g. 'T' inside 'fn make_list[T: type](...)', or a struct
            // field 'data: *T' inside 'type List[T: type] = struct {...}') resolves directly
            // to the bound concrete type. Returns nullopt for anything else (no active
            // generic_env, a dotted/generic_args-bearing name, or a name generic_env doesn't
            // bind), letting the caller fall through to ordinary module-symbol resolution.
            [[nodiscard]] auto find_generic_type_binding(const ast::NamedType &named) const -> std::optional<ResolvedType> {
                if (!generic_env || named.member != nullptr || !named.generic_args.empty()) {
                    return std::nullopt;
                }
                for (const auto &binding : *generic_env) {
                    if (binding.is_type && binding.param_name == named.name) {
                        return binding.type_value;
                    }
                }
                return std::nullopt;
            }

            // Resolves a NamedType known to be either an explicit generic-argument
            // instantiation ('List[i32]') or a bare reference to a generic declaration
            // relying on implicit self-instantiation ('List' inside 'fn make_list[T: type]
            // (...) -> List'). Shared by resolve_type_impl's own NamedType case and
            // resolve_field_type below (a struct field's type needs the exact same logic,
            // just reached via a different caller that also wants full, forced layout —
            // which instantiate_generic_type always provides, since a fresh instantiation is
            // laid out synchronously the same way an anonymous inline struct/enum/union/
            // bitset type already is).
            auto resolve_generic_named_type(const ast::NamedType &named, const std::string &module_path,
                                             const std::string &target_module, const std::string &target_name,
                                             const ast::TypeDecl &target_decl) -> ResolvedType {
                const auto &params = target_decl.generic_params;

                std::vector<const ast::GenericArg *> args_to_resolve;
                // Owns any synthesized (implicit-self-instantiation) GenericArgs for the
                // duration of this call only — a plain local, not shared/static, so nested
                // recursive calls (e.g. resolving one generic arg that itself triggers
                // another implicit self-instantiation) each get their own storage.
                std::vector<ast::GenericArg> synthesized;
                if (!named.generic_args.empty()) {
                    for (const auto &a : named.generic_args) args_to_resolve.push_back(a.get());
                } else if (enclosing_generic_params && enclosing_generic_params->size() == params.size()) {
                    // Implicit self-instantiation: synthesize one GenericArg per enclosing
                    // param, referencing that same param by name — reusing the exact same
                    // resolution logic below (rather than a separate code path) for both the
                    // "already had a binding" (generic_env swap-through) and "needs its own
                    // fresh evaluation" cases.
                    synthesized.reserve(enclosing_generic_params->size());
                    for (const auto &ep : *enclosing_generic_params) {
                        if (is_generic_type_param(ep.type)) {
                            synthesized.push_back(ast::GenericArg{
                                .value = ast::Type{ast::NamedType{.name = ep.name, .member = nullptr, .generic_args = {}, .location = named.location}},
                                .location = named.location,
                            });
                        } else {
                            synthesized.push_back(ast::GenericArg{
                                .value = ast::Expr{ast::IdentExpr{.name = ep.name, .location = named.location}},
                                .location = named.location,
                            });
                        }
                    }
                    for (auto &a : synthesized) args_to_resolve.push_back(&a);
                } else {
                    return error(diag, named.location, std::format(
                        "'{}' used without generic arguments — expected {} ('{}[...]')",
                        target_name, params.size(), target_name));
                }

                if (args_to_resolve.size() != params.size()) {
                    return error(diag, named.location, std::format(
                        "'{}' expects {} generic argument(s), got {}",
                        target_name, params.size(), args_to_resolve.size()));
                }

                std::vector<GenericArgValue> resolved_args;
                resolved_args.reserve(params.size());
                bool arg_error = false;
                for (size_t i = 0; i < params.size(); ++i) {
                    const auto &param = params[i];
                    const auto &arg = *args_to_resolve[i];

                    if (is_generic_type_param(param.type)) {
                        const auto *type_arg = std::get_if<ast::Type>(&arg.value);
                        // A bare 'T' (or dotted 'a.b.T') generic arg always parses as an Expr,
                        // never a Type (starts_type_only has no way to know it names a type at
                        // parse time - see reinterpret_expr_as_type_name's doc comment); this is
                        // the common "forward an enclosing generic's own type param" shape, e.g.
                        // 'fn wrap[T: type]() -> List[T]'.
                        std::optional<ast::Type> reinterpreted;
                        if (!type_arg) {
                            if (const auto *expr_arg = std::get_if<ast::Expr>(&arg.value)) {
                                reinterpreted = reinterpret_expr_as_type_name(*expr_arg);
                                if (reinterpreted) type_arg = &*reinterpreted;
                            }
                        }
                        if (!type_arg) {
                            error(diag, arg.location, std::format(
                                "generic argument {} for '{}' must be a type (parameter '{}: type')",
                                i + 1, target_name, param.name));
                            arg_error = true;
                            continue;
                        }
                        resolved_args.push_back(GenericArgValue{.is_type = true, .type_arg = resolve_type_impl(*type_arg, module_path)});
                    } else {
                        const auto *expr_arg = std::get_if<ast::Expr>(&arg.value);
                        if (!expr_arg) {
                            error(diag, arg.location, std::format(
                                "generic argument {} for '{}' must be a compile-time constant expression",
                                i + 1, target_name));
                            arg_error = true;
                            continue;
                        }
                        // An illegal value-param type ('[N: *i32]') was reported by
                        // validate_generic_param_types, but the declaration stays registered
                        // — so this must degrade instead of std::get-ing an alternative that
                        // isn't there (which aborted the compiler, and the LSP with it).
                        const auto *param_builtin = std::get_if<ast::BuiltinType>(&param.type);
                        if (!param_builtin) {
                            arg_error = true;
                            continue;
                        }
                        const auto param_scalar_type = resolve_builtin(param_builtin->kind);
                        const auto value = eval_integer_const_expr(*expr_arg, module_path, {});
                        if (!value) {
                            error(diag, arg.location, std::format(
                                "generic argument {} for '{}' must be a compile-time constant expression of type '{}'",
                                i + 1, target_name, describe_type(param_scalar_type, program)));
                            arg_error = true;
                            continue;
                        }
                        resolved_args.push_back(GenericArgValue{
                            .is_type = false,
                            .value_arg = static_cast<int64_t>(*value),
                            .value_arg_scalar_type = param_scalar_type,
                        });
                    }
                }
                if (arg_error) return ResolvedType{.kind = TypeKind::Invalid};

                return instantiate_generic_type(program, diag, target_module, target_name, std::move(resolved_args),
                                                 named.location, ast_program);
            }

            // Resolves a NamedType appearing in an ordinary type-position (a pointer
            // pointee, resolve_type_impl's own NamedType case) — handles the generic_env
            // binding, explicit/implicit generic instantiation, and plain-alias cases, then
            // falls through to resolve_final_shallow for the ordinary non-generic case
            // (lazy — does not force full layout; callers needing full layout, like a
            // by-value struct field, use resolve_field_type instead).
            auto resolve_named_type_shallow(const ast::NamedType &named, const std::string &module_path) -> ResolvedType {
                if (const auto binding = find_generic_type_binding(named)) {
                    return *binding;
                }

                auto target = walk_namespace_chain(module_path, named, program, diag, ast_program);
                if (!target) return ResolvedType{.kind = TypeKind::Invalid};

                if (const auto *target_decl = find_type_decl_for(target->module_path, target->name);
                    target_decl && !target_decl->generic_params.empty()) {
                    // generic_args are only ever parsed onto the LEAF segment of a dotted
                    // chain (see NamedType::generic_args' doc comment) - 'named' here is the
                    // chain's own root ('list' in 'list.List[i32]'), whose own generic_args is
                    // always empty, so resolve_generic_named_type must be given the leaf
                    // ('List[i32]') instead, or an explicit dotted instantiation would always
                    // look argument-less.
                    const ast::NamedType *leaf = &named;
                    while (leaf->member) leaf = leaf->member.get();
                    return resolve_generic_named_type(*leaf, module_path, target->module_path, target->name, *target_decl);
                }

                return resolve_final_shallow(target->module_path, target->name, target->crossed_boundary, target->location);
            }

            [[nodiscard]] auto resolve_final_shallow(const std::string &module_path, const std::string &name, const bool check_pub, const SourceLocation &loc) const -> ResolvedType {
                auto mod_it = program.modules.find(module_path);
                if (mod_it == program.modules.end() && ast_program) {
                    ensure_module_declared(*ast_program, module_path, program, diag);
                    mod_it = program.modules.find(module_path);
                }
                if (mod_it == program.modules.end()) {
                    return error(diag, loc, std::format("internal error: module '{}' not found", module_path));
                }

                auto &mod = mod_it->second;

                auto *ts = find_type_symbol(mod, name, loc);
                if (!ts) {
                    return ResolvedType{.kind = TypeKind::Invalid};
                }

                if (check_pub && !ts->is_pub) {
                    return error(diag, loc, std::format("'{}' is not pub", name));
                }

                if (ts->resolved) {
                    return *ts->resolved;
                }

                // A generic DECLARATION has no standalone ResolvedType to produce — 'List'
                // alone is never a type, only 'List[...]' is (see declare_type, which never
                // fills TypeSymbol::resolved for one, and ensure_module_declared's force-layout
                // loop, which skips them). Every legitimate route to a generic name goes through
                // resolve_generic_named_type -> instantiate_generic_type instead, so arriving
                // here means some caller mistook the template for a plain type. Refuse rather
                // than resolve: laying out the RHS with no GenericBindingEnv active reports
                // "unknown type 'T'" against the declaration itself and then CACHES the bogus
                // result below, so every later use of the type inherits the corruption instead
                // of just this one reference failing.
                if (ts->decl && !ts->decl->generic_params.empty()) {
                    return error(diag, loc, std::format(
                        "'{}' used without generic arguments — expected {} ('{}[...]')",
                        name, ts->decl->generic_params.size(), name));
                }

                const auto key = std::make_pair(module_path, name);
                if (program.resolve_state.alias_resolving.contains(key)) {
                    return error(diag, loc, std::format("type alias cycle detected at '{}'", name));
                }

                const ScopedResolveMark resolving_guard(program.resolve_state.alias_resolving, key);
                Resolver inner{program, diag, ast_program};
                auto resolved = inner.resolve_type_impl(ts->decl->type, module_path);

                ts->resolved = resolved;
                return resolved;
            }

            [[nodiscard]] auto resolve_final_full(const std::string &module_path, const std::string &name, const bool check_pub, const SourceLocation &loc) const -> ResolvedType {
                auto mod_it = program.modules.find(module_path);
                if (mod_it == program.modules.end() && ast_program) {
                    ensure_module_declared(*ast_program, module_path, program, diag);
                    mod_it = program.modules.find(module_path);
                }
                if (mod_it == program.modules.end()) {
                    return error(diag, loc, std::format("internal error: module '{}' not found", module_path));
                }
                ProgramModule &mod = mod_it->second;

                const TypeSymbol *ts = find_type_symbol(mod, name, loc);
                if (!ts) return ResolvedType{.kind = TypeKind::Invalid};
                if (check_pub && !ts->is_pub) {
                    return error(diag, loc, std::format("'{}' is not pub", name));
                }

                if (ts->resolved && ts->resolved->kind == TypeKind::Struct) {
                    const int slot = ts->resolved->struct_index;
                    const auto *info = program.struct_at(slot);
                    if (!info) return error(diag, loc, std::format("internal error: invalid struct index for '{}'", name));
                    if (info->layout_done) return *ts->resolved;

                    const auto key = std::make_pair(module_path, name);
                    if (program.resolve_state.struct_resolving.contains(key)) {
                        return error(diag, loc, std::format("by-value struct cycle detected at '{}'", name));
                    }

                    const ScopedResolveMark resolving_guard(program.resolve_state.struct_resolving, key);
                    Resolver inner{program, diag, ast_program};
                    inner.layout_struct(module_path, slot, std::get<std::unique_ptr<ast::StructType>>(ts->decl->type));

                    return *ts->resolved;
                }

                if (ts->resolved && ts->resolved->kind == TypeKind::Enum) {
                    const int slot = ts->resolved->enum_index;
                    const auto *info = program.enum_at(slot);
                    if (!info) return error(diag, loc, std::format("internal error: invalid enum index for '{}'", name));
                    if (info->layout_done) return *ts->resolved;

                    Resolver inner{program, diag, ast_program};
                    inner.layout_enum(module_path, slot, std::get<std::unique_ptr<ast::EnumType>>(ts->decl->type));
                    return *ts->resolved;
                }

                if (ts->resolved && ts->resolved->kind == TypeKind::Union) {
                    const int slot = ts->resolved->union_index;
                    const auto *info = program.union_at(slot);
                    if (!info) return error(diag, loc, std::format("internal error: invalid union index for '{}'", name));
                    if (info->layout_done) return *ts->resolved;

                    const auto key = std::make_pair(module_path, name);
                    if (program.resolve_state.union_resolving.contains(key)) {
                        return error(diag, loc, std::format("by-value union cycle detected at '{}'", name));
                    }

                    const ScopedResolveMark resolving_guard(program.resolve_state.union_resolving, key);
                    Resolver inner{program, diag, ast_program};
                    inner.layout_union(module_path, slot, std::get<std::unique_ptr<ast::UnionType>>(ts->decl->type));

                    return *ts->resolved;
                }

                if (ts->resolved && ts->resolved->kind == TypeKind::Bitset) {
                    const int slot = ts->resolved->bitset_index;
                    const auto *info = program.bitset_at(slot);
                    if (!info) return error(diag, loc, std::format("internal error: invalid bitset index for '{}'", name));
                    if (info->layout_done) return *ts->resolved;

                    const auto key = std::make_pair(module_path, name);
                    if (program.resolve_state.bitset_resolving.contains(key)) {
                        return error(diag, loc, std::format("bitset cycle detected at '{}'", name));
                    }

                    const ScopedResolveMark resolving_guard(program.resolve_state.bitset_resolving, key);
                    Resolver inner{program, diag, ast_program};
                    inner.layout_bitset(module_path, slot, std::get<std::unique_ptr<ast::BitsetType>>(ts->decl->type));

                    return *ts->resolved;
                }

                if (ts->resolved && ts->resolved->kind == TypeKind::Trait) {
                    const int slot = ts->resolved->trait_index;
                    const auto *info = program.trait_at(slot);
                    if (!info) return error(diag, loc, std::format("internal error: invalid trait index for '{}'", name));
                    if (info->layout_done) return *ts->resolved;

                    // Unlike struct/union/bitset, a trait value is a fixed-size fat pointer
                    // (kind + trait_index, see size_of/align_of above) whose ResolvedType never
                    // depends on TraitInfo::methods being resolved. So a trait referencing itself
                    // (or another trait referencing it back) in a method signature isn't a real
                    // cycle — just return the already-known handle type instead of recursing.
                    const auto key = std::make_pair(module_path, name);
                    if (program.resolve_state.trait_resolving.contains(key)) {
                        return *ts->resolved;
                    }

                    const ScopedResolveMark resolving_guard(program.resolve_state.trait_resolving, key);
                    Resolver inner{program, diag, ast_program};
                    inner.layout_trait(module_path, slot, std::get<std::unique_ptr<ast::TraitType>>(ts->decl->type));

                    return *ts->resolved;
                }

                return resolve_final_shallow(module_path, name, check_pub, loc);
            }

            // Whether 'ty' is an aggregate whose layout has not finished — i.e. a type that
            // has a slot but no size yet, because we are inside the very call computing it.
            // Storing one BY VALUE is a type that contains itself, which has no finite size.
            //
            // Looks through arrays (a '[3]T' member is still T stored by value, three times)
            // but deliberately NOT through pointers or slices: those are a fixed-size handle
            // whose size never depends on the pointee's, which is exactly what makes
            // 'next: *node[T]' legal and is the entire point of the in-progress slot.
            [[nodiscard]] auto incomplete_aggregate_name(const ResolvedType &ty) const -> std::optional<std::string> {
                switch (ty.kind) {
                case TypeKind::Struct:
                    if (const auto *info = program.struct_at(ty.struct_index); info && !info->layout_done) break;
                    return std::nullopt;
                case TypeKind::Union:
                    if (const auto *info = program.union_at(ty.union_index); info && !info->layout_done) break;
                    return std::nullopt;
                case TypeKind::Array:
                    if (const auto *info = program.array_at(ty.array_index)) return incomplete_aggregate_name(info->element_type);
                    return std::nullopt;
                default:
                    return std::nullopt;
                }
                const auto [mod, name] = find_type_module_and_name(ty, program);
                return name.empty() ? std::optional<std::string>{"<anonymous>"} : std::optional{name};
            }

            // Rejects a by-value member of a type that is still being laid out. Returns true if
            // it reported.
            //
            // This is where by-value cycle detection lives for GENERIC types. The non-generic
            // path catches its own at resolve_final_full's struct_resolving/union_resolving
            // guard, before any layout starts, because a named type's layout is forced through
            // that function. A generic instantiation has no equivalent chokepoint — a slot is
            // published before its RHS is resolved so that pointer self-reference works, and at
            // that moment "reached through a pointer" and "reached by value" look identical.
            // Only layout can tell them apart: the pointer never asks for a size.
            [[nodiscard]] auto reject_incomplete_member(const ResolvedType &ty, const SourceLocation &loc,
                                                         const std::string_view what, const std::string_view member_name) -> bool {
                const auto incomplete = incomplete_aggregate_name(ty);
                if (!incomplete) return false;
                diag.report_error(DiagnosticStage::Sema, loc, std::format(
                    "{} '{}' stores '{}' by value, which is not yet a complete type; a type cannot "
                    "contain itself by value — store it behind a pointer",
                    what, member_name, *incomplete));
                return true;
            }

            void layout_struct(const std::string &module_path, const int slot, const std::unique_ptr<ast::StructType> &decl) {
                StructInfo info;
                info.module_path = module_path;
                info.is_packed = decl->is_packed;

                uint32_t offset = 0;
                uint32_t max_align = 1;

                for (auto &field : decl->fields) {
                    auto field_type = resolve_field_type(module_path, field.type, field.location);
                    if (reject_incomplete_member(field_type, field.location, "field", field.name)) {
                        field_type = ResolvedType{.kind = TypeKind::Invalid};
                    }
                    const uint32_t f_size = size_of(module_path, field_type);
                    uint32_t f_align = decl->is_packed ? 1 : align_of(module_path, field_type);

                    if (!decl->is_packed && f_align > 0) {
                        offset = (offset + f_align - 1) / f_align * f_align;
                    }

                    info.fields.push_back(StructField{
                        .name = field.name,
                        .type = field_type,
                        .offset = offset,
                        .init_expr = field.init ? &field.init.value() : nullptr,
                        .location = field.location,
                    });
                    offset += f_size;
                    max_align = std::max(max_align, f_align);
                }

                info.size = decl->is_packed ? offset : (offset + max_align - 1) / max_align * max_align;
                info.align = decl->is_packed ? 1 : max_align;
                info.layout_done = true;

                program.structs[slot] = std::move(info);
            }

            auto resolve_field_type(const std::string &module_path, const ast::Type &field_type, SourceLocation loc) -> ResolvedType {
                if (auto *named = std::get_if<ast::NamedType>(&field_type)) {
                    if (const auto binding = find_generic_type_binding(*named)) {
                        return *binding;
                    }

                    // Explicit generic_args on a field's type ('child: List[i32]') route
                    // through resolve_generic_named_type below.
                    //
                    // That normally yields a fully-laid-out instantiation, but NOT always: if
                    // this field is inside the very instantiation being requested
                    // ('next: node[T]' within 'node[T]'), instantiate_generic_type hands back
                    // its own in-progress slot, which has no layout yet. Callers must therefore
                    // treat the result as possibly incomplete — layout_struct/layout_union do,
                    // via reject_incomplete_member, and that is where a by-value cycle is
                    // reported. Do not reintroduce an assumption of completeness here.
                    if (!named->generic_args.empty()) {
                        return resolve_type_impl(field_type, module_path);
                    }

                    const auto target = walk_namespace_chain(module_path, *named, program, diag, ast_program);
                    if (!target) {
                        return ResolvedType{.kind = TypeKind::Invalid};
                    }

                    if (const auto *target_decl = find_type_decl_for(target->module_path, target->name);
                        target_decl && !target_decl->generic_params.empty()) {
                        // Bare reference to a generic type as a field's type — only legal via
                        // implicit self-instantiation (or else a sema error) — both handled by
                        // resolve_type_impl's own NamedType case; reuse it rather than
                        // duplicating that decision here.
                        return resolve_type_impl(field_type, module_path);
                    }

                    return resolve_final_full(target->module_path, target->name, target->crossed_boundary, target->location);
                }
                return resolve_type_impl(field_type, module_path);
            }

            // Thin forwarders to the one implementation (sema.hpp). 'module_path' is unused
            // and always has been - size is a property of the resolved type alone - but the
            // parameter is kept because every layout site already passes one and threading it
            // out would touch far more code than it clarifies.
            [[nodiscard]] auto size_of(const std::string &, const ResolvedType &t) const -> uint32_t {
                return resolved_type_size(t, program);
            }

            [[nodiscard]] auto align_of(const std::string &, const ResolvedType &t) const -> uint32_t {
                return resolved_type_align(t, program);
            }

            auto sizeof_expr_operand(const std::string &module_path, const ast::SizeOfExpr &expr) -> uint64_t {
                if (const auto *ident = std::get_if<ast::IdentExpr>(&expr.operand)) {
                    auto &mod = program.modules.at(module_path);
                    if (const auto it = mod.symbols.find(ident->name); it != mod.symbols.end()) {
                        if (std::holds_alternative<TypeSymbol>(it->second)) {
                            return size_of(module_path, resolve_type_symbol(module_path, ident->name, program, diag, ident->location));
                        }
                    }
                }

                if (const auto *member = std::get_if<std::unique_ptr<ast::MemberExpr>>(&expr.operand)) {
                    if (const auto target = walk_namespace_chain(module_path, as_named_member(**member), program, diag, ast_program)) {
                        const auto &mod = program.modules.at(target->module_path);
                        if (const auto it = mod.symbols.find(target->name); it != mod.symbols.end()) {
                            if (std::holds_alternative<TypeSymbol>(it->second)) {
                                return size_of(target->module_path, resolve_type_symbol(target->module_path, target->name, program, diag, (*member)->location));
                            }
                        }
                    }
                }

                LocalScope no_locals;
                const auto operand_type = check_expr(expr.operand, no_locals, module_path, program, diag, std::nullopt, 0);
                return size_of(module_path, operand_type);
            }

            // Mirrors sizeof_expr_operand() above exactly, substituting align_of() for size_of().
            auto align_of_expr_operand(const std::string &module_path, const ast::AlignOfExpr &expr) -> uint64_t {
                if (const auto *ident = std::get_if<ast::IdentExpr>(&expr.operand)) {
                    auto &mod = program.modules.at(module_path);
                    if (const auto it = mod.symbols.find(ident->name); it != mod.symbols.end()) {
                        if (std::holds_alternative<TypeSymbol>(it->second)) {
                            return align_of(module_path, resolve_type_symbol(module_path, ident->name, program, diag, ident->location));
                        }
                    }
                }

                if (const auto *member = std::get_if<std::unique_ptr<ast::MemberExpr>>(&expr.operand)) {
                    if (const auto target = walk_namespace_chain(module_path, as_named_member(**member), program, diag, ast_program)) {
                        const auto &mod = program.modules.at(target->module_path);
                        if (const auto it = mod.symbols.find(target->name); it != mod.symbols.end()) {
                            if (std::holds_alternative<TypeSymbol>(it->second)) {
                                return align_of(target->module_path, resolve_type_symbol(target->module_path, target->name, program, diag, (*member)->location));
                            }
                        }
                    }
                }

                LocalScope no_locals;
                const auto operand_type = check_expr(expr.operand, no_locals, module_path, program, diag, std::nullopt, 0);
                return align_of(module_path, operand_type);
            }

            static auto as_named_member(const ast::MemberExpr &member) -> ast::NamedType {
                std::vector<std::pair<std::string, SourceLocation>> parts;
                const auto collect = [&](this const auto &self, const ast::Expr &expr) -> bool {
                    if (const auto *ident = std::get_if<ast::IdentExpr>(&expr)) {
                        parts.emplace_back(ident->name, ident->location);
                        return true;
                    }
                    if (const auto *inner = std::get_if<std::unique_ptr<ast::MemberExpr>>(&expr)) {
                        if (!self((*inner)->object)) return false;
                        parts.emplace_back((*inner)->member, (*inner)->location);
                        return true;
                    }
                    return false;
                };

                if (!collect(member.object)) return ast::NamedType{.name = member.member, .location = member.location};
                parts.emplace_back(member.member, member.location);

                ast::NamedType result{
                    .name = parts.front().first,
                    .location = parts.front().second,
                };
                ast::NamedType *tail = &result;
                for (size_t i = 1; i < parts.size(); ++i) {
                    tail->member = std::make_unique<ast::NamedType>(ast::NamedType{
                        .name = parts[i].first,
                        .location = parts[i].second,
                    });
                    tail = tail->member.get();
                }

                return result;
            }

            static auto contains_iota(const ast::Expr &expr) -> bool {
                return std::visit(
                    []<typename T>(const T &v) -> bool {
                        using V = std::decay_t<T>;
                        if constexpr (std::is_same_v<V, ast::IotaExpr>) {
                            return true;
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::UnaryExpr>>) {
                            return contains_iota(v->operand);
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::BinaryExpr>>) {
                            return contains_iota(v->lhs) || contains_iota(v->rhs);
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::TernaryExpr>>) {
                            return contains_iota(v->condition) || contains_iota(v->then_expr) || contains_iota(v->else_expr);
                        } else {
                            return false;
                        }
                    },
                    expr);
            }

            auto eval_integer_const_expr(const ast::Expr &expr, const std::string &module_path, const std::unordered_map<std::string, uint64_t> &macro_args, uint64_t iota_value = 0) -> std::optional<uint64_t> {
                return std::visit(
                    [&]<typename T>(const T &v) -> std::optional<uint64_t> {
                        using V = std::decay_t<T>;

                        if constexpr (std::is_same_v<V, ast::IotaExpr>) {
                            return iota_value;

                        } else if constexpr (std::is_same_v<V, ast::LiteralIntegerExpr>) {
                            return v.value;

                        } else if constexpr (std::is_same_v<V, ast::LiteralBoolExpr>) {
                            return v.value ? 1 : 0;

                        } else if constexpr (std::is_same_v<V, ast::IdentExpr>) {
                            // Innermost binding first: a macro parameter reference resolves to
                            // its already-evaluated argument, then the active generic env's
                            // value params, then module globals — the same order as
                            // evaluate_const_value's IdentExpr case, so the two evaluators
                            // cannot disagree on a shadowed name.
                            if (const auto arg = macro_args.find(v.name); arg != macro_args.end()) {
                                return arg->second;
                            }

                            if (generic_env) {
                                for (const auto &binding : *generic_env) {
                                    if (!binding.is_type && binding.param_name == v.name) {
                                        if (const auto *iv = std::get_if<int64_t>(&binding.const_value)) {
                                            return static_cast<uint64_t>(*iv);
                                        }
                                        return std::nullopt;
                                    }
                                }
                            }

                            auto &mod = program.modules.at(module_path);
                            const auto sym_it = mod.symbols.find(v.name);
                            if (sym_it == mod.symbols.end()) return std::nullopt;

                            const auto *global = std::get_if<GlobalSymbol>(&sym_it->second);
                            if (!global || global->is_mut || !global->decl->init) return std::nullopt;
                            resolve_global_symbol(module_path, v.name, program, diag, v.location);
                            // A global's initializer is a closed expression — never evaluated
                            // with the current template's parameter bindings.
                            return eval_integer_const_expr(*global->decl->init, module_path, {});

                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::SizeOfExpr>>) {
                            return sizeof_expr_operand(module_path, *v);

                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::AlignOfExpr>>) {
                            return align_of_expr_operand(module_path, *v);

                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::CastExpr>>) {
                            return eval_integer_const_expr(v->value, module_path, macro_args);

                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::UnaryExpr>>) {
                            const auto operand = eval_integer_const_expr(v->operand, module_path, macro_args);
                            if (!operand) return std::nullopt;
                            return fold_unary_op(v->op, *operand, ConstFoldSign::Unsigned);

                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::BinaryExpr>>) {
                            const auto lhs = eval_integer_const_expr(v->lhs, module_path, macro_args);
                            const auto rhs = eval_integer_const_expr(v->rhs, module_path, macro_args);
                            if (!lhs || !rhs) return std::nullopt;
                            // UNSIGNED: this evaluator serves type contexts — array lengths,
                            // enum values, generic value arguments — where '/', '%', '>>' and
                            // the relational operators must read their operands as uint64.
                            // The value-context evaluator (evaluate_const_value) folds the
                            // same operators as signed int64; see ConstFoldSign.
                            return fold_binary_op(v->op, *lhs, *rhs, ConstFoldSign::Unsigned);

                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::TernaryExpr>>) {
                            const auto cond = eval_integer_const_expr(v->condition, module_path, macro_args);
                            if (!cond) return std::nullopt;
                            return eval_integer_const_expr(*cond != 0 ? v->then_expr : v->else_expr, module_path, macro_args);

                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::CallExpr>>) {
                            const auto *callee = std::get_if<ast::IdentExpr>(&v->callee);
                            if (!callee) return std::nullopt;
                            auto &mod = program.modules.at(module_path);
                            const auto sym_it = mod.symbols.find(callee->name);
                            if (sym_it == mod.symbols.end()) return std::nullopt;
                            auto *macro = std::get_if<MacroSymbol>(&sym_it->second);
                            if (!macro) return std::nullopt;
                            auto &resolved_macro = resolve_macro_symbol(module_path, callee->name, program, diag, callee->location);
                            // This evaluator also runs on expressions a prior diagnostic
                            // already flagged (an arity mismatch used to index v->args out of
                            // bounds here and abort the compiler) — degrade to nullopt on any
                            // shape that isn't a fully-resolved, correctly-called macro.
                            if (!resolved_macro.decl || !resolved_macro.is_resolved) return std::nullopt;
                            if (v->args.size() != resolved_macro.decl->params.size()) return std::nullopt;

                            // Arguments are bound as *evaluated values*, not expressions: an
                            // argument that is itself an identifier colliding with the
                            // parameter name ('inc(x)' passing a global 'x') would otherwise
                            // resolve to its own binding and recurse without bound.
                            std::unordered_map<std::string, uint64_t> bound;
                            for (size_t i = 0; i < v->args.size(); ++i) {
                                const auto arg_value = eval_integer_const_expr(v->args[i], module_path, macro_args);
                                if (!arg_value) return std::nullopt;
                                bound.emplace(resolved_macro.decl->params[i].name, *arg_value);
                            }
                            return eval_integer_const_expr(resolved_macro.decl->expr_template, module_path, bound);

                        } else {
                            return std::nullopt;
                        }
                    },
                    expr);
            }

            // A LocalScope containing one immutable entry per VALUE binding in the active
            // generic_env, so ordinary check_expr identifier lookup resolves a value
            // generic-param reference (e.g. 'N' in '[N]u8') exactly like any other in-scope
            // constant — no change to check_expr itself needed. Empty when generic_env is
            // null (the ordinary, non-generic case).
            [[nodiscard]] auto generic_env_locals() const -> LocalScope {
                LocalScope locals;
                if (generic_env) {
                    for (const auto &binding : *generic_env) {
                        if (!binding.is_type) {
                            locals[binding.param_name] = LocalBinding{.type = binding.const_value_type, .is_mut = false};
                        }
                    }
                }
                return locals;
            }

            // The active generic_env's value-param names, for is_constant_expr's
            // 'extra_const_names' — a value generic-param is always a compile-time constant
            // by construction. Empty when generic_env is null.
            [[nodiscard]] auto generic_env_const_names() const -> std::unordered_set<std::string> {
                std::unordered_set<std::string> names;
                if (generic_env) {
                    for (const auto &binding : *generic_env) {
                        if (!binding.is_type) names.insert(binding.param_name);
                    }
                }
                return names;
            }

            // nullopt means "the length is an unbound generic value parameter" — not an error,
            // just not knowable yet. The caller degrades the whole array type to Opaque.
            auto array_len_expr_value(const ast::Expr &expr, const std::string &module_path) -> std::optional<uint64_t> {
                auto locals = generic_env_locals();
                const auto len_type = check_expr(expr, locals, module_path, program, diag, ResolvedType{.kind = TypeKind::USize}, 0);
                // Checked before is_integer(), which Opaque fails: '[N]u8' while eagerly
                // checking 'fn buf[N: usize]()' has no count to fold, and reporting
                // "could not be evaluated" here would flag correct code.
                if (len_type.kind == TypeKind::Opaque) return std::nullopt;
                if (!len_type.is_integer()) {
                    diag.report_error(DiagnosticStage::Sema, expr_location(expr), "array length must be an integer constant expression");
                    return 0;
                }
                if (!is_constant_expr(expr, module_path, program, generic_env_const_names())) {
                    diag.report_error(DiagnosticStage::Sema, expr_location(expr), "array length must be a compile-time constant expression");
                    return 0;
                }
                if (const auto value = eval_integer_const_expr(expr, module_path, {})) {
                    return *value;
                }
                diag.report_error(DiagnosticStage::Sema, expr_location(expr), "array length constant expression could not be evaluated");
                return 0;
            }

            void layout_enum(const std::string &module_path, const int slot, const std::unique_ptr<ast::EnumType> &decl) {
                auto &mod = program.modules.at(module_path);

                // Resolve underlying type (default: i32)
                ResolvedType underlying;
                if (decl->underlying_type) {
                    underlying = resolve_type_impl(*decl->underlying_type, module_path);
                } else {
                    underlying = ResolvedType{.kind = TypeKind::I32};
                }

                EnumInfo info;
                info.module_path = module_path;
                info.underlying_type = underlying;

                uint64_t iota_counter = 0;
                const ast::Expr *iota_template = nullptr; // nullptr = just use iota value directly

                for (const auto &field : decl->fields) {
                    int64_t field_value;
                    if (field.init) {
                        const auto result = eval_integer_const_expr(*field.init, module_path, {}, iota_counter);
                        if (!result) {
                            diag.report_error(DiagnosticStage::Sema, field.location, std::format("enum field '{}' is not a compile-time constant", field.name));
                            field_value = static_cast<int64_t>(iota_counter);
                        } else {
                            field_value = static_cast<int64_t>(*result);
                        }
                        if (contains_iota(*field.init)) {
                            iota_template = &*field.init;
                        } else {
                            iota_template = nullptr;
                        }
                    } else {
                        if (iota_template) {
                            const auto result = eval_integer_const_expr(*iota_template, module_path, {}, iota_counter);
                            field_value = result ? static_cast<int64_t>(*result) : static_cast<int64_t>(iota_counter);
                        } else {
                            field_value = static_cast<int64_t>(iota_counter);
                        }
                    }

                    for (const auto &existing : info.fields) {
                        if (existing.name == field.name) {
                            diag.report_error(DiagnosticStage::Sema, field.location, std::format("duplicate enum field name '{}'", field.name));
                        }
                    }

                    info.fields.push_back(EnumFieldInfo{.name = field.name, .value = field_value});
                    ++iota_counter;
                }

                info.layout_done = true;
                program.enums[slot] = std::move(info);
            }

            static auto storage_type_name(const TypeKind kind) -> const char * {
                switch (kind) {
                case TypeKind::U8:  return "u8";
                case TypeKind::U16: return "u16";
                case TypeKind::U64: return "u64";
                default:            return "u32";
                }
            }

            void layout_bitset(const std::string &module_path, const int slot, const std::unique_ptr<ast::BitsetType> &decl) {
                const auto target = walk_namespace_chain(module_path, decl->member_type, program, diag, ast_program);
                if (!target) {
                    program.bitsets[slot] = BitsetInfo{.module_path = module_path, .layout_done = true};
                    return;
                }

                // No requirement that the enum spell out an explicit backing type here —
                // an enum with no '(...)' backing already defaults to i32 (see layout_enum),
                // and that resolved underlying_type is all layout_bitset actually needs.
                const auto member_ty = resolve_final_full(target->module_path, target->name, target->crossed_boundary, target->location);
                if (member_ty.kind != TypeKind::Enum) {
                    diag.report_error(DiagnosticStage::Sema, decl->location, "bitset member type must be an enum type");
                    program.bitsets[slot] = BitsetInfo{.module_path = module_path, .layout_done = true};
                    return;
                }

                ResolvedType storage = decl->storage_type
                                            ? resolve_type_impl(*decl->storage_type, module_path)
                                            : ResolvedType{.kind = TypeKind::U32};
                if (storage.kind != TypeKind::U8 && storage.kind != TypeKind::U16 &&
                    storage.kind != TypeKind::U32 && storage.kind != TypeKind::U64) {
                    diag.report_error(DiagnosticStage::Sema, decl->location, "bitset storage type must be one of u8, u16, u32, u64");
                    storage = ResolvedType{.kind = TypeKind::U32};
                }
                const uint32_t storage_bits = primitive_size(storage.kind) * 8;

                if (const auto *enum_info = program.enum_at(member_ty.enum_index)) {
                    for (const auto &field : enum_info->fields) {
                        const int64_t bit_index = field.value + 1;
                        if (bit_index < 0 || static_cast<uint64_t>(bit_index) >= storage_bits) {
                            const int64_t mask = (bit_index >= 0 && bit_index < 63) ? (int64_t{1} << bit_index) : 0;
                            diag.report_error(DiagnosticStage::Sema, decl->location, std::format(
                                "bitset variant '{}.{}' has value {}, producing bit index {} (1 << {} = {}), "
                                "which does not fit in the storage type '{}' ({} bits). "
                                "Use a wider storage type or reduce the enum variant values.",
                                target->name, field.name, field.value, bit_index, bit_index, mask,
                                storage_type_name(storage.kind), storage_bits));
                        }
                    }
                }

                program.bitsets[slot] = BitsetInfo{
                    .module_path = module_path,
                    .member_enum_type = member_ty,
                    .storage_type = storage,
                    .storage_bits = storage_bits,
                    .layout_done = true,
                };
            }

            void layout_union(const std::string &module_path, const int slot, const std::unique_ptr<ast::UnionType> &decl) {
                UnionInfo info;
                info.module_path = module_path;
                info.is_tagged = decl->is_tagged;

                if (decl->is_tagged) {
                    uint32_t max_payload_size = 0;
                    uint32_t max_payload_align = 1;

                    for (int32_t i = 0; i < static_cast<int32_t>(decl->members.size()); ++i) {
                        const auto &member = decl->members[i];
                        TaggedUnionVariant variant;
                        variant.name = member.name;
                        variant.tag_value = i;
                        variant.payload_struct_index = -1;

                        if (!std::holds_alternative<std::monostate>(member.type)) {
                            // Variant has a payload type. Struct payloads (whether an inline
                            // `struct{...}` or a reference to a separately-named struct type) use
                            // their own fields directly for ergonomics; any other payload type is
                            // wrapped in a synthetic one-field struct named "v".
                            int struct_slot;

                            if (const auto *st = std::get_if<std::unique_ptr<ast::StructType>>(&member.type)) {
                                // Inline struct payload — allocate an anonymous struct slot for it.
                                struct_slot = static_cast<int>(program.structs.size());
                                program.structs.push_back(StructInfo{.module_path = module_path});
                                layout_struct(module_path, struct_slot, *st);
                                variant.payload_type = ResolvedType{.kind = TypeKind::Struct, .struct_index = struct_slot};
                            } else {
                                // Named struct payload reuses its own slot; anything else gets
                                // the one-field "v" wrapper. wrap_payload_in_struct below is
                                // that whole rule — it was written later, for error unions, as
                                // a copy of the code that used to sit here.
                                auto payload_type = resolve_field_type(module_path, member.type, member.location);
                                if (reject_incomplete_member(payload_type, member.location, "variant", member.name)) {
                                    payload_type = ResolvedType{.kind = TypeKind::Invalid};
                                }
                                struct_slot = wrap_payload_in_struct(module_path, payload_type, member.location);
                                variant.payload_type = payload_type;
                            }

                            variant.payload_struct_index = struct_slot;
                            if (const auto *payload_struct = program.struct_at(struct_slot)) {
                                max_payload_size = std::max(max_payload_size, payload_struct->size);
                                max_payload_align = std::max(max_payload_align, payload_struct->align);
                            } else {
                                diag.report_error(DiagnosticStage::Sema, member.location,
                                    std::format("internal error: invalid payload struct index for variant '{}'", member.name));
                            }
                        }

                        info.variants.push_back(std::move(variant));
                    }

                    const auto layout = compute_tagged_union_layout(max_payload_size, max_payload_align);
                    info.payload_offset = layout.payload_offset;
                    info.align = layout.align;
                    info.size = layout.size;
                } else {
                    uint32_t max_size = 0;
                    uint32_t max_align = 1;

                    for (const auto &member : decl->members) {
                        auto member_type = resolve_field_type(module_path, member.type, member.location);
                        if (reject_incomplete_member(member_type, member.location, "union member", member.name)) {
                            member_type = ResolvedType{.kind = TypeKind::Invalid};
                        }
                        const uint32_t m_size = size_of(module_path, member_type);
                        const uint32_t m_align = align_of(module_path, member_type);

                        info.members.push_back(UnionMember{
                            .name = member.name,
                            .type = member_type,
                            .location = member.location,
                        });
                        max_size = std::max(max_size, m_size);
                        max_align = std::max(max_align, m_align);
                    }

                    // Union size = largest member size, rounded up to alignment
                    info.size = (max_size + max_align - 1) / max_align * max_align;
                    info.align = max_align;
                }

                info.layout_done = true;
                program.unions[slot] = std::move(info);
            }

            // Valid 'error(...)' member types: enum(i32) or any tagged union that is not
            // itself an already-synthesized error union (error(...) types can't nest).
            [[nodiscard]] auto is_valid_error_member(const ResolvedType &ty) const -> bool {
                if (ty.kind == TypeKind::Enum) {
                    const auto *info = program.enum_at(ty.enum_index);
                    return info != nullptr && info->underlying_type.kind == TypeKind::I32;
                }
                if (ty.kind == TypeKind::Union) {
                    const auto *info = program.union_at(ty.union_index);
                    return info != nullptr && info->is_tagged && !info->is_error_union;
                }
                return false;
            }

            // THE tagged-union payload convention, used by both layout_union (for a
            // user-written 'union(enum)') and synthesize_error_union (for the compiler's own
            // Ok/Failed wrappers): a struct payload is used verbatim, in its own slot, so its
            // fields stay directly addressable; anything else is wrapped in a synthetic
            // one-field struct named "v".
            //
            // Every payload-reading consumer — emit_variant_capture, TaggedVariantExpr
            // construction — is written against that convention, which is why the two
            // producers must not diverge on it.
            [[nodiscard]] auto wrap_payload_in_struct(const std::string &module_path, const ResolvedType &payload_type, const SourceLocation &location) -> int {
                if (payload_type.kind == TypeKind::Struct) {
                    return payload_type.struct_index;
                }

                const int struct_slot = static_cast<int>(program.structs.size());
                program.structs.push_back(StructInfo{.module_path = module_path});

                StructInfo payload_info;
                payload_info.module_path = module_path;
                payload_info.fields.push_back(StructField{
                    .name = "v",
                    .type = payload_type,
                    .offset = 0,
                    .init_expr = nullptr,
                    .location = location,
                });
                payload_info.size = size_of(module_path, payload_type);
                payload_info.align = align_of(module_path, payload_type);
                payload_info.layout_done = true;
                program.structs[struct_slot] = std::move(payload_info);
                return struct_slot;
            }

            // Builds the compiler-synthesized Ok/Failed tagged union for a distinct
            // 'error(...)' spelling. 'sorted_members' is already in canonical (interning)
            // order. Single member: Failed's payload IS that member type directly (wrapped
            // in the usual one-field struct convention) — no inner union at all. 2+
            // members: Failed's payload is a second synthesized tagged union (one variant
            // per member type, named after that member type, in the same canonical order)
            // whose own payload is, in turn, that member type. 'is_optional' ('?error(...)')
            // is recorded on the OUTER wrapper only — the inner dispatch union is an
            // implementation detail nobody can name or call.
            auto synthesize_error_union(const std::string &module_path, const std::vector<ResolvedType> &sorted_members, bool is_optional, const SourceLocation &location) -> UnionInfo {
                ResolvedType failed_payload_source;
                if (sorted_members.size() == 1) {
                    failed_payload_source = sorted_members[0];
                } else {
                    UnionInfo inner;
                    inner.module_path = module_path;
                    inner.is_tagged = true;
                    inner.is_error_union = true;
                    inner.error_member_types = sorted_members;

                    uint32_t max_payload_size = 0;
                    uint32_t max_payload_align = 1;
                    for (int32_t i = 0; i < static_cast<int32_t>(sorted_members.size()); ++i) {
                        const auto &member = sorted_members[i];
                        TaggedUnionVariant variant;
                        variant.name = find_type_module_and_name(member, program).second;
                        variant.tag_value = i;
                        variant.payload_struct_index = wrap_payload_in_struct(module_path, member, location);
                        variant.payload_type = member;
                        if (const auto *payload_struct = program.struct_at(variant.payload_struct_index)) {
                            max_payload_size = std::max(max_payload_size, payload_struct->size);
                            max_payload_align = std::max(max_payload_align, payload_struct->align);
                        }
                        inner.variants.push_back(std::move(variant));
                    }

                    const auto inner_layout = compute_tagged_union_layout(max_payload_size, max_payload_align);
                    inner.payload_offset = inner_layout.payload_offset;
                    inner.align = inner_layout.align;
                    inner.size = inner_layout.size;
                    inner.layout_done = true;

                    const int inner_slot = static_cast<int>(program.unions.size());
                    program.unions.push_back(std::move(inner));
                    failed_payload_source = ResolvedType{.kind = TypeKind::Union, .union_index = inner_slot};
                }

                UnionInfo outer;
                outer.module_path = module_path;
                outer.is_tagged = true;
                outer.is_error_union = true;
                outer.error_member_types = sorted_members;
                outer.is_optional = is_optional;

                TaggedUnionVariant ok_variant;
                ok_variant.name = "Ok";
                ok_variant.tag_value = 0;
                ok_variant.payload_struct_index = -1;

                TaggedUnionVariant failed_variant;
                failed_variant.name = "Failed";
                failed_variant.tag_value = 1;
                failed_variant.payload_struct_index = wrap_payload_in_struct(module_path, failed_payload_source, location);
                failed_variant.payload_type = failed_payload_source;

                outer.variants.push_back(ok_variant);
                outer.variants.push_back(failed_variant);

                const auto *failed_struct = program.struct_at(failed_variant.payload_struct_index);
                const uint32_t max_payload_size = failed_struct ? failed_struct->size : 0;
                const uint32_t max_payload_align = failed_struct ? failed_struct->align : 1;
                const auto outer_layout = compute_tagged_union_layout(max_payload_size, max_payload_align);
                outer.payload_offset = outer_layout.payload_offset;
                outer.align = outer_layout.align;
                outer.size = outer_layout.size;
                outer.layout_done = true;

                return outer;
            }

            // Interns a (possibly newly-synthesized) error union by the SET of its member
            // types plus its '?' marking — 'error(A|B)' and 'error(B|A)' intern to the same
            // union, but 'error(A)' and '?error(A)' deliberately do NOT (see
            // UnionInfo::is_optional). Duplicate members are rejected on the original,
            // unsorted list so the diagnostic reflects the user's actual spelling.
            auto intern_error_union(const std::string &module_path, const std::vector<ResolvedType> &members, bool is_optional, const SourceLocation &location) -> ResolvedType {
                for (size_t i = 0; i < members.size(); ++i) {
                    for (size_t j = i + 1; j < members.size(); ++j) {
                        if (members[i] == members[j]) {
                            diag.report_error(DiagnosticStage::Sema, location, "error(...) member types must be distinct");
                        }
                    }
                }

                std::vector<ResolvedType> sorted_members = members;
                std::ranges::sort(sorted_members, [](const ResolvedType &a, const ResolvedType &b) {
                    if (a.kind != b.kind) return a.kind < b.kind;
                    if (a.kind == TypeKind::Enum) return a.enum_index < b.enum_index;
                    return a.union_index < b.union_index;
                });

                // Drop the duplicates the loop above reported. Keeping them built a union with
                // two identical variants, so the error-recovery path carried a type that could
                // never be valid. No correct program observes this (ok is already false by
                // now), but it keeps the recovery type internally consistent -- and it means
                // 'error(A|A)' interns to the same entry as 'error(A)' rather than a second,
                // parallel one.
                const auto duplicates = std::ranges::unique(sorted_members);
                sorted_members.erase(duplicates.begin(), duplicates.end());

                const Program::ErrorUnionKey key{.members = sorted_members, .is_optional = is_optional};
                for (const auto &[existing, index] : program.error_unions) {
                    if (existing == key) {
                        return ResolvedType{.kind = TypeKind::Union, .union_index = index};
                    }
                }

                const int slot = static_cast<int>(program.unions.size());
                program.unions.push_back(UnionInfo{});
                program.unions[slot] = synthesize_error_union(module_path, sorted_members, is_optional, location);
                program.error_unions.emplace_back(key, slot);
                return ResolvedType{.kind = TypeKind::Union, .union_index = slot};
            }

            auto resolve_error_type(const ast::ErrorType &decl, const std::string &module_path) -> ResolvedType {
                std::vector<ResolvedType> members;
                members.reserve(decl.members.size());

                for (const auto &named : decl.members) {
                    const auto target = walk_namespace_chain(module_path, named, program, diag, ast_program);
                    if (!target) {
                        members.push_back(ResolvedType{.kind = TypeKind::Invalid});
                        continue;
                    }

                    auto resolved = resolve_final_full(target->module_path, target->name, target->crossed_boundary, target->location);
                    if (!is_valid_error_member(resolved)) {
                        diag.report_error(DiagnosticStage::Sema, named.location,
                            std::format("'{}' is not a valid error member type; expected an 'enum(i32)' or 'union(enum)' type", named.name));
                    }
                    members.push_back(resolved);
                }

                return intern_error_union(module_path, members, /*is_optional=*/false, decl.location);
            }

            // '?E' — the same error type as 'error(E)' but marked ignorable, so a caller may
            // drop it and let codegen synthesize the failure check (see sema's
            // call_dropped_optional_error). Three spellings resolve here:
            //
            //   ?error(A | B)  — inner is an ErrorType; re-intern its member set as optional
            //   ?Alias         — inner names a 'type Alias = error(A | B)'; same treatment
            //   ?E             — inner names a bare enum(i32)/union(enum); sugar for '?error(E)'
            //
            // Re-interning through the member LIST (rather than flipping a flag on the
            // resolved union) is what keeps '?error(A|B)' and 'error(A|B)' two distinct
            // interned unions instead of one mutated one.
            auto resolve_optional_error_type(const ast::OptionalErrorType &decl, const std::string &module_path) -> ResolvedType {
                // A NAMED inner has to go through the full resolution path, exactly like
                // resolve_error_type's members do: is_valid_error_member reads an enum's
                // underlying_type, which only exists once layout_enum has run, and the shallow
                // resolution resolve_type_impl uses for a NamedType does not force that. The
                // difference is invisible for a free function (whose signature is resolved late
                // enough that the enum is already laid out) but not for a trait method, which
                // is resolved during layout_trait.
                ResolvedType inner;
                if (const auto *named = std::get_if<ast::NamedType>(&decl.inner)) {
                    const auto target = walk_namespace_chain(module_path, *named, program, diag, ast_program);
                    if (!target) return ResolvedType{.kind = TypeKind::Invalid};
                    inner = resolve_final_full(target->module_path, target->name, target->crossed_boundary, target->location);
                } else {
                    inner = resolve_type_impl(decl.inner, module_path);
                }
                if (inner.kind == TypeKind::Invalid) return inner;

                if (inner.kind == TypeKind::Union) {
                    if (const auto *info = program.union_at(inner.union_index); info && info->is_error_union) {
                        if (info->is_optional) return inner;
                        return intern_error_union(module_path, info->error_member_types, /*is_optional=*/true, decl.location);
                    }
                }

                if (!is_valid_error_member(inner)) {
                    diag.report_error(DiagnosticStage::Sema, decl.location,
                        std::format("'?' requires an error type; expected 'error(...)', an 'enum(i32)', a 'union(enum)', or a type alias to one, got '{}'",
                                    describe_type(inner, program)));
                    return ResolvedType{.kind = TypeKind::Invalid};
                }

                return intern_error_union(module_path, {inner}, /*is_optional=*/true, decl.location);
            }

            // Resolves each trait method's non-self param/return types, in declaration
            // order, then resolves this trait's composition list (if any) — flattening
            // composed traits' methods in, detecting composition cycles, and computing the
            // transitive component-trait set. TraitInfo::methods ends up holding the FULL
            // FLATTENED set (own methods followed by each composed trait's own flattened
            // methods) — that order IS the vtable layout and is recorded here once; codegen
            // and every other consumer must read it, never re-derive it.
            void layout_trait(const std::string &module_path, const int slot, const std::unique_ptr<ast::TraitType> &decl) {
                TraitInfo info;
                info.module_path = module_path;
                info.name = program.traits[slot].name; // set at declare time (sema_declare.cpp); preserved here since
                                                        // we fully replace program.traits[slot] at the end.

                for (auto &method : decl->methods) {
                    TraitMethodInfo m;
                    m.name = method.name;
                    m.is_mut_self = method.is_mut_self;
                    m.location = method.location;
                    m.decl = &method;
                    for (auto &p : method.params) {
                        if (p.type) {
                            m.params.push_back(resolve_field_type(module_path, *p.type, p.location));
                        } else {
                            // ':=' inferred-type param — infer from the (parser-guaranteed) default expr.
                            LocalScope empty;
                            m.params.push_back(check_expr(*p.default_value, empty, module_path, program, diag, std::nullopt, 0));
                        }
                    }
                    for (auto &rt : method.return_types) {
                        m.return_types.push_back(resolve_field_type(module_path, rt, method.location));
                    }
                    check_param_defaults(method.params, m.params, m.required_params, m.param_default_is_const, module_path, program, diag);
                    info.methods.push_back(std::move(m));
                }

                // ---- Trait composition: resolve, detect cycles, flatten ----
                //
                // 'contributor[method_name]' is "" for a method 'info' already declares itself,
                // else the name of whichever composed trait first contributed it — used only to
                // pick the right collision-message wording (own-vs-composed vs composed-vs-composed).
                std::unordered_map<std::string, std::string> contributor;
                for (const auto &m : info.methods) contributor[m.name] = "";

                std::vector<ComposedTraitRef> direct_composed;
                std::vector<ComposedTraitRef> component_traits;
                std::vector<int> seen_direct;
                std::vector<int> seen_component;

                const ScopedResolvePush composition_guard(program.resolve_state.trait_composition_stack, slot);

                for (const auto &named : decl->composed_traits) {
                    const auto target = walk_namespace_chain(module_path, named, program, diag, ast_program);
                    if (!target) continue; // walk_namespace_chain already reported its own diagnostic

                    const auto rt = resolve_final_full(target->module_path, target->name, target->crossed_boundary, target->location);
                    if (rt.kind == TypeKind::Invalid) continue; // already diagnosed

                    if (rt.kind != TypeKind::Trait) {
                        diag.report_error(DiagnosticStage::Sema, named.location, std::format("'{}' is not a trait", named.name));
                        continue;
                    }

                    const int composed_slot = rt.trait_index;

                    if (const auto it = std::ranges::find(program.resolve_state.trait_composition_stack, composed_slot);
                        it != program.resolve_state.trait_composition_stack.end()) {
                        // Genuine composition cycle: build the full chain from where
                        // 'composed_slot' first appears on the ancestor stack through the
                        // current top, closing with a repeat of that first name.
                        std::vector<std::string> chain;
                        for (auto s = it; s != program.resolve_state.trait_composition_stack.end(); ++s) {
                            const auto *ancestor = program.trait_at(*s);
                            chain.push_back(ancestor ? ancestor->name : "?");
                        }
                        chain.push_back(chain.front());

                        std::string message = std::format("circular trait composition: '{}' composes '{}'", chain[0], chain[1]);
                        for (size_t i = 2; i < chain.size(); ++i) {
                            message += std::format(", which composes '{}'", chain[i]);
                        }
                        diag.report_error(DiagnosticStage::Sema, named.location, message);
                        continue;
                    }

                    if (std::ranges::find(seen_direct, composed_slot) != seen_direct.end()) {
                        diag.report_error(DiagnosticStage::Sema, named.location,
                            std::format("duplicate trait '{}' in composition list of '{}'", named.name, info.name));
                        continue;
                    }
                    seen_direct.push_back(composed_slot);

                    const auto *composed_info = program.trait_at(composed_slot);
                    if (!composed_info) continue;
                    direct_composed.push_back(ComposedTraitRef{
                        .module_path = composed_info->module_path, .name = named.name, .trait_index = composed_slot});

                    for (const auto &m : composed_info->methods) {
                        const auto existing = std::ranges::find_if(info.methods, [&](const TraitMethodInfo &e) { return e.name == m.name; });
                        if (existing == info.methods.end()) {
                            info.methods.push_back(m);
                            contributor[m.name] = named.name;
                            continue;
                        }
                        if (!trait_methods_conflict(*existing, m)) continue; // identical signature — diamond-safe merge, no-op

                        const auto &prior = contributor.at(m.name);
                        if (prior.empty()) {
                            diag.report_error(DiagnosticStage::Sema, decl->location, std::format(
                                "trait '{}' declares '{}' itself and also composes '{}', which declares '{}' with an "
                                "incompatible signature ('fn{}' vs 'fn{}'). Rename one of them to disambiguate.",
                                info.name, m.name, named.name, m.name,
                                describe_signature(existing->is_mut_self, existing->params, existing->return_types, program),
                                describe_signature(m.is_mut_self, m.params, m.return_types, program)));
                        } else {
                            diag.report_error(DiagnosticStage::Sema, decl->location, std::format(
                                "trait '{}' composes both '{}' and '{}', which each declare '{}' with incompatible "
                                "signatures ('fn{}' vs 'fn{}'). Rename one of them to disambiguate.",
                                info.name, prior, named.name, m.name,
                                describe_signature(existing->is_mut_self, existing->params, existing->return_types, program),
                                describe_signature(m.is_mut_self, m.params, m.return_types, program)));
                        }
                    }

                    for (const auto &c : composed_info->component_traits) {
                        if (std::ranges::find(seen_component, c.trait_index) == seen_component.end()) {
                            seen_component.push_back(c.trait_index);
                            component_traits.push_back(c);
                        }
                    }
                    if (std::ranges::find(seen_component, composed_slot) == seen_component.end()) {
                        seen_component.push_back(composed_slot);
                        component_traits.push_back(ComposedTraitRef{
                            .module_path = composed_info->module_path, .name = named.name, .trait_index = composed_slot});
                    }
                }


                if (direct_composed.size() == 1 && decl->methods.empty()) {
                    diag.warn(DiagnosticStage::Sema, decl->location, std::format(
                        "trait '{}' composes only '{}' and declares no methods of its own, making it identical to "
                        "'{}'. Either remove '{}' and use '{}' directly, or declare it as a type alias:\n"
                        "    pub type {} = {}",
                        info.name, direct_composed[0].name, direct_composed[0].name, info.name,
                        direct_composed[0].name, info.name, direct_composed[0].name));
                }

                info.composed_traits = std::move(direct_composed);
                info.component_traits = std::move(component_traits);
                info.layout_done = true;
                info.composition_done = true;
                program.traits[slot] = std::move(info);
            }

            auto resolve_type_impl(const ast::Type &type, const std::string &module_path) -> ResolvedType {
                return std::visit(
                    [&]<typename T>(const T &v) -> ResolvedType {
                        using V = std::decay_t<T>;

                        if constexpr (std::is_same_v<V, std::monostate>) {
                            return ResolvedType{.kind = TypeKind::Invalid};

                        } else if constexpr (std::is_same_v<V, ast::BuiltinType>) {
                            return resolve_builtin(v.kind);

                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::PointerType>>) {
                            ResolvedType pointee;
                            if (auto *named = std::get_if<ast::NamedType>(&v->pointee)) {
                                pointee = resolve_named_type_shallow(*named, module_path);
                            } else {
                                pointee = resolve_type_impl(v->pointee, module_path);
                            }
                            return intern_pointer(program, pointee);

                        } else if constexpr (std::is_same_v<V, ast::NamedType>) {
                            return resolve_named_type_shallow(v, module_path);

                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::StructType>>) {
                            int slot = static_cast<int>(program.structs.size());
                            program.structs.push_back(StructInfo{.module_path = module_path});
                            layout_struct(module_path, slot, v);
                            return ResolvedType{.kind = TypeKind::Struct, .struct_index = slot};

                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::ArrayType>>) {
                            if (!v->size.has_value()) {
                                return error(diag, v->location,
                                    "array type '[?]T' can only be used as the declared type of a 'const'/'let' declaration with an array literal initializer");
                            }
                            auto element = resolve_type_impl(v->base_type, module_path);
                            const auto count = array_len_expr_value(*v->size, module_path);
                            // Degrade to plain Opaque when the length or the element is an
                            // unbound generic parameter, rather than interning an array with a
                            // placeholder count. intern_array dedups on (element, count) only,
                            // so an opaque '[N]u8' recorded as count 0 would silently alias a
                            // real '[0]u8' — a miscompile waiting to happen. Opaque is already
                            // permissive for indexing, 'len', and assignment, which is exactly
                            // what a not-yet-known array needs.
                            if (!count || element.kind == TypeKind::Opaque) {
                                return ResolvedType{.kind = TypeKind::Opaque};
                            }
                            return intern_array(program, element, *count, size_of(module_path, element) * *count, align_of(module_path, element));

                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::SliceType>>) {
                            auto element = resolve_type_impl(v->base_type, module_path);
                            return intern_slice(program, element);

                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::EnumType>>) {
                            const int slot = static_cast<int>(program.enums.size());
                            program.enums.push_back(EnumInfo{});
                            layout_enum(module_path, slot, v);
                            return ResolvedType{.kind = TypeKind::Enum, .enum_index = slot};
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::UnionType>>) {
                            const int slot = static_cast<int>(program.unions.size());
                            program.unions.push_back(UnionInfo{.module_path = module_path});
                            layout_union(module_path, slot, v);
                            return ResolvedType{.kind = TypeKind::Union, .union_index = slot};
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::FunctionType>>) {
                            FunctionTypeInfo sig;
                            sig.is_variadic = v->is_variadic;
                            sig.param_names = v->param_names; // cosmetic only; NOT compared in intern_function_type
                            for (const auto &pt : v->param_types) {
                                sig.param_types.push_back(resolve_type_impl(pt, module_path));
                            }
                            for (const auto &rt : v->return_types) {
                                sig.return_types.push_back(resolve_type_impl(rt, module_path));
                            }
                            return intern_function_type(program, std::move(sig));
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::TraitType>>) {
                            // Anonymous trait types have no name to 'impl', making them useless as
                            // handle targets — traits only exist for named, impl-able dispatch surfaces.
                            return error(diag, v->location, "trait types must be declared via 'type Name = trait { ... }'; anonymous trait types are not supported");
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::ErrorType>>) {
                            return resolve_error_type(*v, module_path);
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::OptionalErrorType>>) {
                            return resolve_optional_error_type(*v, module_path);
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::BitsetType>>) {
                            const int slot = static_cast<int>(program.bitsets.size());
                            program.bitsets.push_back(BitsetInfo{});
                            layout_bitset(module_path, slot, v);
                            return ResolvedType{.kind = TypeKind::Bitset, .bitset_index = slot};
                        } else {
                            // Exhaustiveness guard, matching walk_expr_for_foreign_refs in
                            // sema_attributes.cpp. All 14 ast::Type alternatives are handled
                            // above; a 14th added later would previously have fallen in here and
                            // silently resolved to Invalid, surfacing as a confusing "unknown
                            // type" much further along. Now it is a compile error at the site
                            // that needs updating.
                            static_assert(!sizeof(V), "resolve_type_impl: unhandled ast::Type alternative");
                        }
                    },
                    type);
            }

            // Allocates the slot an aggregate type expression will occupy, WITHOUT laying it
            // out, so a handle to it exists before its own contents are resolved. Returns
            // nullopt for every non-aggregate type expression, which has no slot to allocate.
            //
            // Only instantiate_generic_type needs this. A named non-generic type gets the
            // same thing for free at declare time (declare_type fills TypeSymbol::resolved
            // before any layout runs); a generic declaration deliberately has no "the" type
            // to fill in, so its instantiation has to do it here instead.
            auto preallocate_aggregate(const ast::Type &type, const std::string &module_path) -> std::optional<ResolvedType> {
                if (std::holds_alternative<std::unique_ptr<ast::StructType>>(type)) {
                    program.structs.push_back(StructInfo{.module_path = module_path});
                    return ResolvedType{.kind = TypeKind::Struct, .struct_index = static_cast<int>(program.structs.size()) - 1};
                }
                if (std::holds_alternative<std::unique_ptr<ast::EnumType>>(type)) {
                    program.enums.push_back(EnumInfo{});
                    return ResolvedType{.kind = TypeKind::Enum, .enum_index = static_cast<int>(program.enums.size()) - 1};
                }
                if (std::holds_alternative<std::unique_ptr<ast::UnionType>>(type)) {
                    program.unions.push_back(UnionInfo{.module_path = module_path});
                    return ResolvedType{.kind = TypeKind::Union, .union_index = static_cast<int>(program.unions.size()) - 1};
                }
                if (std::holds_alternative<std::unique_ptr<ast::BitsetType>>(type)) {
                    program.bitsets.push_back(BitsetInfo{});
                    return ResolvedType{.kind = TypeKind::Bitset, .bitset_index = static_cast<int>(program.bitsets.size()) - 1};
                }
                return std::nullopt;
            }

            // Lays out a slot obtained from preallocate_aggregate. Split from it so a handle
            // to the slot can be published (and so become reachable through a self-referential
            // pointer field) while this runs.
            void layout_preallocated(const ast::Type &type, const std::string &module_path, const ResolvedType &slot) {
                switch (slot.kind) {
                case TypeKind::Struct:
                    layout_struct(module_path, slot.struct_index, std::get<std::unique_ptr<ast::StructType>>(type));
                    break;
                case TypeKind::Enum:
                    layout_enum(module_path, slot.enum_index, std::get<std::unique_ptr<ast::EnumType>>(type));
                    break;
                case TypeKind::Union:
                    layout_union(module_path, slot.union_index, std::get<std::unique_ptr<ast::UnionType>>(type));
                    break;
                case TypeKind::Bitset:
                    layout_bitset(module_path, slot.bitset_index, std::get<std::unique_ptr<ast::BitsetType>>(type));
                    break;
                default:
                    break;
                }
            }

            static auto resolve_builtin(const ast::BuiltinTypeKind kind) -> ResolvedType {
                switch (kind) {
                case ast::BuiltinTypeKind::U8:     return ResolvedType{.kind = TypeKind::U8};
                case ast::BuiltinTypeKind::U16:    return ResolvedType{.kind = TypeKind::U16};
                case ast::BuiltinTypeKind::U32:    return ResolvedType{.kind = TypeKind::U32};
                case ast::BuiltinTypeKind::U64:    return ResolvedType{.kind = TypeKind::U64};
                case ast::BuiltinTypeKind::I8:     return ResolvedType{.kind = TypeKind::I8};
                case ast::BuiltinTypeKind::I16:    return ResolvedType{.kind = TypeKind::I16};
                case ast::BuiltinTypeKind::I32:    return ResolvedType{.kind = TypeKind::I32};
                case ast::BuiltinTypeKind::I64:    return ResolvedType{.kind = TypeKind::I64};
                case ast::BuiltinTypeKind::F32:    return ResolvedType{.kind = TypeKind::F32};
                case ast::BuiltinTypeKind::F64:    return ResolvedType{.kind = TypeKind::F64};
                case ast::BuiltinTypeKind::Usize:  return ResolvedType{.kind = TypeKind::USize};
                case ast::BuiltinTypeKind::Bool:   return ResolvedType{.kind = TypeKind::Bool};
                case ast::BuiltinTypeKind::Byte:   return ResolvedType{.kind = TypeKind::U8};
                case ast::BuiltinTypeKind::Anyptr: return ResolvedType{.kind = TypeKind::Anyptr};
                case ast::BuiltinTypeKind::Type:   return ResolvedType{.kind = TypeKind::Type};
                case ast::BuiltinTypeKind::Any:    return ResolvedType{.kind = TypeKind::Any};
                }

                return ResolvedType{.kind = TypeKind::Invalid};
            }
        };
    }

    auto find_trait_provider(const Program &program, const std::string &type_module, const std::string &type_name,
                              const int trait_index, std::vector<std::string> *ambiguous_via) -> int {
        const auto it = program.trait_impls_by_type.find({type_module, type_name});
        if (it == program.trait_impls_by_type.end()) return -1;

        // Tier 1: an exact, directly-written 'impl D for TYPE' always wins over any
        // composed-derived route to D — this is what keeps every pre-existing direct-impl
        // program byte-for-byte unaffected by the Tier 2 search below.
        for (const auto &impl_info : it->second) {
            if (impl_info.trait_index == trait_index) return trait_index;
        }

        // Tier 2: no exact impl — any impl on this type whose trait COMPOSES the wanted one
        // (directly or transitively) provides it. Two such routes is an ambiguity the caller
        // reports; collecting the names is optional since not every caller needs the message.
        int provider = -1;
        for (const auto &impl_info : it->second) {
            const auto *candidate = program.trait_at(impl_info.trait_index);
            if (!candidate) continue;
            const bool reaches = std::ranges::any_of(candidate->component_traits,
                [&](const auto &c) { return c.trait_index == trait_index; });
            if (!reaches) continue;
            if (provider < 0) provider = impl_info.trait_index;
            if (ambiguous_via) ambiguous_via->push_back(impl_info.trait_name);
        }
        return provider;
    }

    auto generic_param_bound_trait_index(const ast::GenericParam &param, const std::string &module_path,
                                          Program &program, DiagnosticEngine &diag, const bool report) -> int {
        // '[T: type]' is a BuiltinType and carries no bound; only the '[T: SomeTrait]' spelling
        // (a NamedType, possibly dotted) does. Anything else was already rejected by
        // validate_generic_param_types on shape.
        const auto *named = std::get_if<ast::NamedType>(&param.type);
        if (!named) return -1;
        const auto resolved = resolve_type(param.type, module_path, program, diag);
        if (resolved.kind == TypeKind::Trait) return resolved.trait_index;
        // 'report' is set by exactly one caller — the eager pass, which visits each generic
        // DECLARATION once. Every other caller runs per-instantiation and would repeat the
        // message once per use site.
        if (report && resolved.kind != TypeKind::Invalid) { // Invalid was already diagnosed
            diag.report_error(DiagnosticStage::Sema, param.location, std::format(
                "generic parameter '{}' is bound by '{}', which is not a trait; a bound must name a trait",
                param.name, describe_type(resolved, program)));
        }
        return -1;
    }

    namespace {
        // 'visiting' holds the struct indexes on the current descent. A struct can only reach
        // itself through a pointer (nothing else could have a finite layout), so without this
        // guard 'struct Node { next: *Node }' recurses until the stack runs out — which it did,
        // for any 'type_of'/'any' use of a self-referential type. Re-entering a struct proves
        // the cycle closed without finding an Opaque, so the answer for that edge is false.
        auto contains_opaque_impl(const ResolvedType &ty, const Program &program, std::vector<int> &visiting) -> bool {
            switch (ty.kind) {
            case TypeKind::Opaque:
                return true;
            case TypeKind::Pointer: {
                const auto *pointee = program.pointee_at(ty.pointee_index);
                return pointee && contains_opaque_impl(*pointee, program, visiting);
            }
            case TypeKind::Array: {
                const auto *info = program.array_at(ty.array_index);
                return info && contains_opaque_impl(info->element_type, program, visiting);
            }
            case TypeKind::Slice: {
                const auto *info = program.slice_at(ty.slice_index);
                return info && contains_opaque_impl(info->element_type, program, visiting);
            }
            case TypeKind::Struct: {
                const auto *info = program.struct_at(ty.struct_index);
                if (!info) return false;
                if (std::ranges::find(visiting, ty.struct_index) != visiting.end()) return false;
                visiting.push_back(ty.struct_index);
                const bool found = std::ranges::any_of(info->fields,
                    [&](const auto &f) { return contains_opaque_impl(f.type, program, visiting); });
                visiting.pop_back();
                return found;
            }
            default:
                return false;
            }
        }
    }

    auto contains_opaque(const ResolvedType &ty, const Program &program) -> bool {
        std::vector<int> visiting;
        return contains_opaque_impl(ty, program, visiting);
    }

    auto is_opaque_template_instance(const std::optional<GenericInstanceInfo> &generic_instance,
                                      const Program &program) -> bool {
        return generic_instance && args_contain_opaque(generic_instance->args, program);
    }

    auto args_contain_opaque(const std::vector<GenericArgValue> &args, const Program &program) -> bool {
        return std::ranges::any_of(args, [&](const GenericArgValue &a) {
            // A VALUE parameter carries its opaqueness on the scalar type rather than the
            // folded value: binding 'N' to an Opaque-typed local makes every use of 'N' Opaque,
            // without needing an extra ConstFoldValue alternative threaded through every
            // const-folding site in the compiler.
            return a.is_type ? contains_opaque(a.type_arg, program)
                              : a.value_arg_scalar_type.kind == TypeKind::Opaque;
        });
    }

    auto is_assignable(const ResolvedType &from, const ResolvedType &to, const Program &program) -> bool {
        if (from == to) return true;
        // An unbound generic parameter is assignment-compatible with everything, in both
        // directions. Whether a real 'T' actually is depends on the instantiation, which by
        // definition isn't known here — so the eager pass stays silent and the real check
        // happens per-instantiation. One short-circuit covers every caller of this and of
        // assignable_in_module. Assignment/copy is part of the universal core a trait-bounded
        // parameter keeps too, so this deliberately does not consult 'trait_index'.
        if (from.kind == TypeKind::Opaque || to.kind == TypeKind::Opaque) return true;
        if (from.kind == TypeKind::Anyptr && to.kind == TypeKind::Pointer) return true;
        if (from.kind == TypeKind::Pointer && to.kind == TypeKind::Anyptr) return true;
        // nil (Anyptr) is assignable to/from function pointer types
        if (from.kind == TypeKind::Anyptr && to.kind == TypeKind::Function) return true;
        if (from.kind == TypeKind::Function && to.kind == TypeKind::Anyptr) return true;

        // The container conversions compare element/pointee types. These rows were once
        // element-blind, which let 'const S: []u8 = A' with 'A: [3]i32' through sema and
        // into an LLVM verifier failure with no source location.
        const auto element_of = [&](const ResolvedType &ty) -> const ResolvedType * {
            if (ty.kind == TypeKind::Array) {
                const auto *info = program.array_at(ty.array_index);
                return info ? &info->element_type : nullptr;
            }
            const auto *info = program.slice_at(ty.slice_index);
            return info ? &info->element_type : nullptr;
        };
        if ((from.kind == TypeKind::Array && to.kind == TypeKind::Slice) ||
            (from.kind == TypeKind::Slice && to.kind == TypeKind::Array)) {
            const auto *from_elem = element_of(from);
            const auto *to_elem = element_of(to);
            return from_elem && to_elem && *from_elem == *to_elem;
        }
        if (from.kind == TypeKind::Slice && to.kind == TypeKind::Pointer) {
            const auto *from_elem = element_of(from);
            const auto *pointee = program.pointee_at(to.pointee_index);
            return from_elem && pointee && *from_elem == *pointee;
        }
        if (from.kind == TypeKind::Anyptr && to.kind == TypeKind::Slice) return true;
        if (from.kind == TypeKind::Slice && to.kind == TypeKind::Anyptr) return true;
        return false;
    }

    // 'try' is legal when every error member type the callee can produce is also a
    // member of the caller's declared error(...) union — i.e. callee's error set is a
    // subset of (or equal to) caller's.
    auto error_union_is_subset(const ResolvedType &callee, const ResolvedType &caller, const Program &program) -> bool {
        if (callee == caller) return true;
        if (callee.kind != TypeKind::Union || caller.kind != TypeKind::Union) return false;

        const auto *callee_info = program.union_at(callee.union_index);
        const auto *caller_info = program.union_at(caller.union_index);
        if (!callee_info || !caller_info || !callee_info->is_error_union || !caller_info->is_error_union) return false;

        for (const auto &member : callee_info->error_member_types) {
            if (!std::ranges::any_of(caller_info->error_member_types, [&](const auto &m) { return m == member; })) {
                return false;
            }
        }
        return true;
    }

    auto function_params_compatible(const std::vector<ResolvedType> &actual, const std::vector<ResolvedType> &expected) -> bool {
        if (actual.size() != expected.size()) return false;
        for (size_t i = 0; i < actual.size(); ++i) {
            if (actual[i] == expected[i]) continue;
            // A typed pointer parameter may decay to an `anyptr` parameter in the
            // target function type (C-style void* callback pattern) — both lower
            // to the same opaque `ptr` at the ABI level.
            if (actual[i].kind == TypeKind::Pointer && expected[i].kind == TypeKind::Anyptr) continue;
            return false;
        }
        return true;
    }

    auto resolve_type(const ast::Type &type, const std::string &module_path, Program &program, DiagnosticEngine &diag, const ast::Program *ast_program) -> ResolvedType {
        // Falls back to the ambient 'currently checking this generic instance's body'
        // env (see Program::active_generic_env_stack's doc comment) when no explicit
        // generic_env is available at this call site — this is what makes a local var
        // decl's type annotation, a cast target, size_of's operand, etc. inside a generic
        // function/method body resolve a bare type/value generic-param reference correctly,
        // without threading a new parameter through check_expr/check_stmt.
        const auto *env = program.active_generic_env_stack.empty() ? nullptr : program.active_generic_env_stack.back();
        Resolver resolver{program, diag, ast_program, env, nullptr};
        return resolver.resolve_type_impl(type, module_path);
    }

    // Folds a 'size_of(X)' / 'align_of(X)' operand, where X is either a type name or a value
    // expression. Exported so value_resolver.cpp's evaluators can fold these too: they used
    // to report them as constant (is_constant_expr_impl returns true) while having no case to
    // actually evaluate them, so a 'when size_of(T) > 4' condition silently folded to false
    // and took the wrong branch. type_resolver's own eval_integer_const_expr has always
    // handled them, which is why 'const N := size_of(T)' and '[size_of(T)]u8' worked.
    auto eval_size_of_operand(const ast::SizeOfExpr &expr, const std::string &module_path, Program &program,
                              DiagnosticEngine &diag) -> uint64_t {
        const auto *env = program.active_generic_env_stack.empty() ? nullptr : program.active_generic_env_stack.back();
        Resolver resolver{program, diag, nullptr, env, nullptr};
        return resolver.sizeof_expr_operand(module_path, expr);
    }

    auto eval_align_of_operand(const ast::AlignOfExpr &expr, const std::string &module_path, Program &program,
                               DiagnosticEngine &diag) -> uint64_t {
        const auto *env = program.active_generic_env_stack.empty() ? nullptr : program.active_generic_env_stack.back();
        Resolver resolver{program, diag, nullptr, env, nullptr};
        return resolver.align_of_expr_operand(module_path, expr);
    }

    // Like resolve_type, but with 'env' active — used to resolve a generic function/method
    // instance's own param/return types (e.g. 'N' in '-> [N]u8'), reusing the exact same
    // generic_env machinery instantiate_generic_type already uses for a generic type's
    // fields.
    auto resolve_type_with_generic_env(const ast::Type &type, const std::string &module_path, Program &program,
                                        DiagnosticEngine &diag, const GenericBindingEnv &env,
                                        const std::vector<ast::GenericParam> *enclosing_generic_params) -> ResolvedType {
        Resolver resolver{program, diag, nullptr, &env, enclosing_generic_params};
        return resolver.resolve_type_impl(type, module_path);
    }

    auto instantiate_generic_type(Program &program, DiagnosticEngine &diag, const std::string &module_path,
                                   const std::string &decl_name, std::vector<GenericArgValue> args,
                                   const SourceLocation &use_loc, const ast::Program *ast_program) -> ResolvedType {
        // NOTE: an instantiation whose arguments are still unbound ('Slot[K, V]' written
        // inside a generic declaration) IS built here, as an ordinary slot — it is simply
        // tagged as a template by is_opaque_template_instance and never emitted.
        //
        // Building it is what makes eager checking useful for containers. A generic struct's
        // fields are usually only PARTLY dependent on its parameters — 'capacity: usize' and
        // 'hash: Hash_Fn[K]' (returning a concrete 'u64') stay fully known even when K is
        // not. Collapsing the whole aggregate to Opaque, as an earlier version did, threw all
        // of that away and made every expression through 'self' unverifiable.
        const GenericInstanceKey key{.module_path = module_path, .decl_name = decl_name, .args = args};

        for (const auto &[k, ty] : program.generic_type_instance_lookup) {
            if (k == key) return ty;
        }

        // Already being instantiated further up the stack. If a slot was pre-allocated for it,
        // hand that back: the reference is legal exactly when it does not need the layout, and
        // the only such reference is through a pointer. A by-value one gets a slot with no
        // layout, which layout_struct/layout_union reject by name a moment later — deliberately
        // there rather than here, because THIS function cannot tell the two apart.
        //
        // Without a slot (a non-aggregate RHS, 'type Ptr[T: type] = *T') there is nothing to
        // hand back and self-reference is a genuine infinite regress.
        for (const auto &in_progress : program.resolve_state.generic_type_resolving) {
            if (in_progress.key != key) continue;
            if (in_progress.slot) return *in_progress.slot;
            diag.report_error(DiagnosticStage::Sema, use_loc,
                std::format("generic type instantiation cycle detected at '{}'", decl_name));
            return ResolvedType{.kind = TypeKind::Invalid};
        }

        const auto mod_it = program.modules.find(module_path);
        if (mod_it == program.modules.end()) {
            diag.report_error(DiagnosticStage::Sema, use_loc, std::format("internal error: module '{}' not found", module_path));
            return ResolvedType{.kind = TypeKind::Invalid};
        }
        const auto sym_it = mod_it->second.symbols.find(decl_name);
        if (sym_it == mod_it->second.symbols.end()) {
            diag.report_error(DiagnosticStage::Sema, use_loc, std::format("unknown type '{}'", decl_name));
            return ResolvedType{.kind = TypeKind::Invalid};
        }
        const auto *ts = std::get_if<TypeSymbol>(&sym_it->second);
        if (!ts || !ts->decl) {
            diag.report_error(DiagnosticStage::Sema, use_loc, std::format("'{}' is not a type", decl_name));
            return ResolvedType{.kind = TypeKind::Invalid};
        }
        const ast::TypeDecl &decl = *ts->decl;

        if (args.size() != decl.generic_params.size()) {
            diag.report_error(DiagnosticStage::Sema, use_loc, std::format(
                "'{}' expects {} generic argument(s), got {}", decl_name, decl.generic_params.size(), args.size()));
            return ResolvedType{.kind = TypeKind::Invalid};
        }

        // Bind each declared param name to its concrete argument, matched in declared order.
        GenericBindingEnv env;
        env.reserve(args.size());
        for (size_t i = 0; i < args.size(); ++i) {
            const auto &param = decl.generic_params[i];
            const auto &arg = args[i];
            env.push_back(GenericBinding{
                .param_name = param.name,
                .is_type = arg.is_type,
                .type_value = arg.type_arg,
                .const_value = arg.value_arg,
                .const_value_type = arg.value_arg_scalar_type,
            });
        }

        // Resolve the declaration's RHS with the substitution env active.
        //
        // An aggregate RHS gets its slot allocated FIRST and published on the in-progress
        // stack, so a self-referential field ('next: *node[T]') resolving back to this same
        // key finds a handle to point at rather than a cycle error. That mirrors what the
        // non-generic path already gets from declare_type filling TypeSymbol::resolved
        // up-front — the reason 'next: *block_header' has always worked.
        //
        // Anything else (a type alias: '= *T', '= []T', '= u32') is resolved whole, exactly
        // as before, with no slot on the stack.
        Resolver inner{program, diag, ast_program, &env, &decl.generic_params};
        const auto preallocated = inner.preallocate_aggregate(decl.type, module_path);

        const GenericInstanceInfo instance_info{.decl_module = module_path, .decl_name = decl_name, .args = args};
        const auto stamp_instance = [&](const ResolvedType &ty) {
            switch (ty.kind) {
            case TypeKind::Struct:
                if (ty.struct_index >= 0 && static_cast<size_t>(ty.struct_index) < program.structs.size())
                    program.structs[ty.struct_index].generic_instance = instance_info;
                break;
            case TypeKind::Enum:
                if (ty.enum_index >= 0 && static_cast<size_t>(ty.enum_index) < program.enums.size())
                    program.enums[ty.enum_index].generic_instance = instance_info;
                break;
            case TypeKind::Union:
                if (ty.union_index >= 0 && static_cast<size_t>(ty.union_index) < program.unions.size())
                    program.unions[ty.union_index].generic_instance = instance_info;
                break;
            case TypeKind::Bitset:
                if (ty.bitset_index >= 0 && static_cast<size_t>(ty.bitset_index) < program.bitsets.size())
                    program.bitsets[ty.bitset_index].generic_instance = instance_info;
                break;
            default:
                break;
            }
        };

        ResolvedType result;
        {
            const ScopedResolvePush generic_guard(program.resolve_state.generic_type_resolving,
                                                   ResolveState::GenericTypeInProgress{.key = key, .slot = preallocated});
            if (preallocated) {
                // Stamped BEFORE layout as well as after: find_type_module_and_name reads this
                // tag to name the type, and layout is exactly when a by-value cycle in it gets
                // reported. Without it the diagnostic can only say "<anonymous>". Each layout_*
                // assigns a freshly built Info over the slot wholesale, so the tag does not
                // survive — hence stamping twice rather than once.
                stamp_instance(*preallocated);
                inner.layout_preallocated(decl.type, module_path, *preallocated);
                result = *preallocated;
            } else {
                result = inner.resolve_type_impl(decl.type, module_path);
            }
        }

        stamp_instance(result);
        program.generic_type_instance_lookup.push_back({key, result});

        return result;
    }

    auto resolve_declared_type(const std::optional<ast::Type> &type, const std::optional<ast::Expr> &init,
                                const std::string &module_path, Program &program, DiagnosticEngine &diag,
                                const SourceLocation &decl_loc) -> std::optional<ResolvedType> {
        if (!type) return std::nullopt;

        const auto *array_type = std::get_if<std::unique_ptr<ast::ArrayType>>(&*type);
        if (!array_type || (*array_type)->size.has_value()) {
            return resolve_type(*type, module_path, program, diag); // unchanged behavior
        }

        // '[?]T': infer the element count from a literal array initializer.
        if (!init) {
            return error(diag, decl_loc, "cannot infer array length: '[?]' array declaration requires an initializer");
        }
        const auto *braced = std::get_if<std::unique_ptr<ast::BracedInitializerExpr>>(&*init);
        const auto *array_lit = braced ? std::get_if<ast::ArrayExpr>(braced->get()) : nullptr;
        if (!array_lit) {
            return error(diag, expr_location(*init), "cannot infer array length: initializer for a '[?]' array type must be an array literal");
        }
        if (array_lit->has_fill) {
            return error(diag, array_lit->location, "cannot infer array length: initializer must not use '...' to fill remaining elements");
        }

        const auto *env = program.active_generic_env_stack.empty() ? nullptr : program.active_generic_env_stack.back();
        Resolver resolver{program, diag, nullptr, env, nullptr};
        const auto element = resolver.resolve_type_impl((*array_type)->base_type, module_path);
        const auto count = static_cast<uint64_t>(array_lit->values.size());
        return intern_array(program, element, count,
                             resolver.size_of(module_path, element) * static_cast<uint32_t>(count),
                             resolver.align_of(module_path, element));
    }

    auto resolve_import_bin_type(const std::string &module_path, const std::string &path, const SourceLocation &loc,
                                  Program &program, DiagnosticEngine &diag) -> ResolvedType {
        const auto resolved = ast::resolve_contained_path(module_path, path);
        if (resolved.empty()) {
            return error(diag, loc, std::format("import_bin path '{}' escapes the module directory", path));
        }

        std::error_code ec;
        if (!std::filesystem::is_regular_file(resolved, ec) || ec) {
            return error(diag, loc, std::format("import_bin: file not found: '{}'", path));
        }

        const auto size = std::filesystem::file_size(resolved, ec);
        if (ec) {
            return error(diag, loc, std::format("import_bin: cannot read file: '{}'", path));
        }

        if (size > 1024 * 1024) {
            diag.warn(DiagnosticStage::Sema, loc, std::format("import_bin: '{}' is {} bytes, exceeding 1 MiB", path, size));
        }

        return intern_array(program, ResolvedType{.kind = TypeKind::U8}, size, static_cast<uint32_t>(size), 1);
    }

    auto resolve_type_symbol(const std::string &module_path, const std::string &name, Program &program, DiagnosticEngine &diag, const SourceLocation &loc, const ast::Program *ast_program) -> ResolvedType {
        const Resolver resolver{program, diag, ast_program};
        return resolver.resolve_final_full(module_path, name, false, loc);
    }

    // THE definition of what 'size_of(T)' evaluates to. Resolver::size_of (above) and
    // codegen.cpp's Generator::size_of both forward here; see sema.hpp.
    //
    // Only the five indexed kinds need Program at all - everything else, fat pointers
    // included, is a fixed width primitive_size already knows.
    auto resolved_type_size(const ResolvedType &t, const Program &program) -> uint32_t {
        if (t.kind == TypeKind::Struct) { const auto *info = program.struct_at(t.struct_index); return info ? info->size : 0; }
        if (t.kind == TypeKind::Array) { const auto *info = program.array_at(t.array_index); return info ? info->size : 0; }
        if (t.kind == TypeKind::Enum) { const auto *info = program.enum_at(t.enum_index); return info ? primitive_size(info->underlying_type.kind) : 0; }
        if (t.kind == TypeKind::Union) { const auto *info = program.union_at(t.union_index); return info ? info->size : 0; }
        if (t.kind == TypeKind::Bitset) { const auto *info = program.bitset_at(t.bitset_index); return info ? primitive_size(info->storage_type.kind) : 0; }
        return primitive_size(t.kind);
    }

    // Mirrors resolved_type_size() above, substituting alignment for size - with one real
    // difference, not a formatting one: Slice/Trait/Any are 16 bytes but only 8-byte aligned,
    // so they must NOT fall through to primitive_align (which forwards to primitive_size and
    // would answer 16). Those three overrides are the entire reason this is a separate
    // function rather than a parameter on the one above.
    auto resolved_type_align(const ResolvedType &t, const Program &program) -> uint32_t {
        if (t.kind == TypeKind::Struct) { const auto *info = program.struct_at(t.struct_index); return info ? info->align : 1; }
        if (t.kind == TypeKind::Array) { const auto *info = program.array_at(t.array_index); return info ? info->align : 1; }
        if (t.kind == TypeKind::Slice) return 8;
        if (t.kind == TypeKind::Trait) return 8;
        if (t.kind == TypeKind::Any) return 8;
        if (t.kind == TypeKind::Enum) { const auto *info = program.enum_at(t.enum_index); return info ? primitive_align(info->underlying_type.kind) : 1; }
        if (t.kind == TypeKind::Union) { const auto *info = program.union_at(t.union_index); return info ? info->align : 1; }
        if (t.kind == TypeKind::Bitset) { const auto *info = program.bitset_at(t.bitset_index); return info ? primitive_align(info->storage_type.kind) : 1; }
        return primitive_align(t.kind);
    }
}
