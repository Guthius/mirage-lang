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
    void ensure_module_declared(const ast::Program &program, const std::string &module_path, Program &sema_program, DiagnosticEngine &diag);

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

            default:
                return 0;
            }
        }

        auto primitive_align(const TypeKind kind) -> uint32_t { return primitive_size(kind); }

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
                const auto mod_it = program.modules.find(module_path);
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
                        const auto param_scalar_type = resolve_builtin(std::get<ast::BuiltinType>(param.type).kind);
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

                return instantiate_generic_type(program, diag, target_module, target_name, std::move(resolved_args), named.location);
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

            void layout_struct(const std::string &module_path, const int slot, const std::unique_ptr<ast::StructType> &decl) {
                StructInfo info;
                info.module_path = module_path;
                info.is_packed = decl->is_packed;

                uint32_t offset = 0;
                uint32_t max_align = 1;

                for (auto &field : decl->fields) {
                    auto field_type = resolve_field_type(module_path, field.type, field.location);
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

                    // Explicit generic_args on a field's type ('child: List[i32]') always
                    // routes through resolve_generic_named_type below, which — via
                    // instantiate_generic_type — always produces an already-FULLY-laid-out
                    // result (a fresh instantiation is laid out synchronously, the same way
                    // an anonymous inline struct/enum/union/bitset already is), so it's safe
                    // to use directly as a struct field's type exactly like the plain
                    // resolve_final_full path below.
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

            [[nodiscard]] auto size_of(const std::string &module_path, const ResolvedType &t) const -> uint32_t {
                if (t.kind == TypeKind::Struct) { const auto *info = program.struct_at(t.struct_index); return info ? info->size : 0; }
                if (t.kind == TypeKind::Array) { const auto *info = program.array_at(t.array_index); return info ? info->size : 0; }
                if (t.kind == TypeKind::Slice) return 16;
                if (t.kind == TypeKind::Enum) { const auto *info = program.enum_at(t.enum_index); return info ? primitive_size(info->underlying_type.kind) : 0; }
                if (t.kind == TypeKind::Union) { const auto *info = program.union_at(t.union_index); return info ? info->size : 0; }
                if (t.kind == TypeKind::Bitset) { const auto *info = program.bitset_at(t.bitset_index); return info ? primitive_size(info->storage_type.kind) : 0; }
                return primitive_size(t.kind);
            }

            [[nodiscard]] auto align_of(const std::string &module_path, const ResolvedType &t) const -> uint32_t {
                if (t.kind == TypeKind::Struct) { const auto *info = program.struct_at(t.struct_index); return info ? info->align : 1; }
                if (t.kind == TypeKind::Array) { const auto *info = program.array_at(t.array_index); return info ? info->align : 1; }
                if (t.kind == TypeKind::Slice) return 8;
                if (t.kind == TypeKind::Trait) return 8; // primitive_align would wrongly forward to primitive_size's 16
                if (t.kind == TypeKind::Any) return 8;   // same reasoning as Trait above
                if (t.kind == TypeKind::Enum) { const auto *info = program.enum_at(t.enum_index); return info ? primitive_align(info->underlying_type.kind) : 1; }
                if (t.kind == TypeKind::Union) { const auto *info = program.union_at(t.union_index); return info ? info->align : 1; }
                if (t.kind == TypeKind::Bitset) { const auto *info = program.bitset_at(t.bitset_index); return info ? primitive_align(info->storage_type.kind) : 1; }
                return primitive_align(t.kind);
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

            auto eval_integer_const_expr(const ast::Expr &expr, const std::string &module_path, const std::unordered_map<std::string, const ast::Expr *> &macro_args, uint64_t iota_value = 0) -> std::optional<uint64_t> {
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
                            // A value generic-param reference (e.g. 'N' inside 'fn
                            // make_fixed[N: usize]() -> [N]u8') folds to its bound constant —
                            // checked before macro_args/globals, mirroring how
                            // find_generic_type_binding takes priority for type params.
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

                            if (const auto arg = macro_args.find(v.name); arg != macro_args.end()) {
                                return eval_integer_const_expr(*arg->second, module_path, macro_args);
                            }

                            auto &mod = program.modules.at(module_path);
                            const auto sym_it = mod.symbols.find(v.name);
                            if (sym_it == mod.symbols.end()) return std::nullopt;

                            const auto *global = std::get_if<GlobalSymbol>(&sym_it->second);
                            if (!global || global->is_mut || !global->decl->init) return std::nullopt;
                            resolve_global_symbol(module_path, v.name, program, diag, v.location);
                            return eval_integer_const_expr(*global->decl->init, module_path, macro_args);

                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::SizeOfExpr>>) {
                            return sizeof_expr_operand(module_path, *v);

                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::AlignOfExpr>>) {
                            return align_of_expr_operand(module_path, *v);

                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::CastExpr>>) {
                            return eval_integer_const_expr(v->value, module_path, macro_args);

                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::UnaryExpr>>) {
                            const auto operand = eval_integer_const_expr(v->operand, module_path, macro_args);
                            if (!operand) return std::nullopt;
                            switch (v->op) {
                            case ast::UnaryOp::Negate:     return uint64_t{0} - *operand;
                            case ast::UnaryOp::BitwiseNot: return ~*operand;
                            case ast::UnaryOp::LogicalNot: return *operand == 0 ? 1 : 0;
                            default:                       return std::nullopt;
                            }

                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::BinaryExpr>>) {
                            const auto lhs = eval_integer_const_expr(v->lhs, module_path, macro_args);
                            const auto rhs = eval_integer_const_expr(v->rhs, module_path, macro_args);
                            if (!lhs || !rhs) return std::nullopt;
                            switch (v->op) {
                            case ast::BinaryOp::Add:          return *lhs + *rhs;
                            case ast::BinaryOp::Sub:          return *lhs - *rhs;
                            case ast::BinaryOp::Mul:          return *lhs * *rhs;
                            case ast::BinaryOp::Div:          return *rhs == 0 ? std::nullopt : std::optional<uint64_t>{*lhs / *rhs};
                            case ast::BinaryOp::Mod:          return *rhs == 0 ? std::nullopt : std::optional<uint64_t>{*lhs % *rhs};
                            case ast::BinaryOp::BitwiseAnd:   return *lhs & *rhs;
                            case ast::BinaryOp::BitwiseOr:    return *lhs | *rhs;
                            case ast::BinaryOp::BitwiseXor:   return *lhs ^ *rhs;
                            case ast::BinaryOp::ShiftLeft:    return *rhs >= 64 ? std::nullopt : std::optional<uint64_t>{*lhs << *rhs};
                            case ast::BinaryOp::ShiftRight:   return *rhs >= 64 ? std::nullopt : std::optional<uint64_t>{*lhs >> *rhs};
                            case ast::BinaryOp::Equal:        return *lhs == *rhs ? 1 : 0;
                            case ast::BinaryOp::NotEqual:     return *lhs != *rhs ? 1 : 0;
                            case ast::BinaryOp::Less:         return *lhs < *rhs ? 1 : 0;
                            case ast::BinaryOp::Greater:      return *lhs > *rhs ? 1 : 0;
                            case ast::BinaryOp::LessEqual:    return *lhs <= *rhs ? 1 : 0;
                            case ast::BinaryOp::GreaterEqual: return *lhs >= *rhs ? 1 : 0;
                            case ast::BinaryOp::LogicalAnd:   return (*lhs != 0 && *rhs != 0) ? 1 : 0;
                            case ast::BinaryOp::LogicalOr:    return (*lhs != 0 || *rhs != 0) ? 1 : 0;
                            case ast::BinaryOp::In:           return std::nullopt; // not a compile-time-constant-foldable operator here
                            }
                            return std::nullopt;

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

                            std::unordered_map<std::string, const ast::Expr *> nested_args = macro_args;
                            for (size_t i = 0; i < resolved_macro.decl->params.size(); ++i) {
                                nested_args[resolved_macro.decl->params[i].name] = &v->args[i];
                            }
                            return eval_integer_const_expr(resolved_macro.decl->expr_template, module_path, nested_args);

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

            auto array_len_expr_value(const ast::Expr &expr, const std::string &module_path) -> uint64_t {
                auto locals = generic_env_locals();
                const auto len_type = check_expr(expr, locals, module_path, program, diag, ResolvedType{.kind = TypeKind::USize}, 0);
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
                    // Tagged union: tag (u32) + optional payload
                    // Layout: [u32 tag | padding | max_payload bytes]
                    static constexpr uint32_t TAG_SIZE = 4;
                    static constexpr uint32_t TAG_ALIGN = 4;

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
                                auto payload_type = resolve_field_type(module_path, member.type, member.location);
                                if (payload_type.kind == TypeKind::Struct) {
                                    // Named struct payload — reuse its own slot, no wrapping.
                                    struct_slot = payload_type.struct_index;
                                } else {
                                    // Non-struct payload: create a one-field anonymous struct
                                    struct_slot = static_cast<int>(program.structs.size());
                                    program.structs.push_back(StructInfo{.module_path = module_path});
                                    StructInfo payload_info;
                                    payload_info.module_path = module_path;
                                    const uint32_t p_size = size_of(module_path, payload_type);
                                    const uint32_t p_align = align_of(module_path, payload_type);
                                    payload_info.fields.push_back(StructField{
                                        .name = "v",
                                        .type = payload_type,
                                        .offset = 0,
                                        .init_expr = nullptr,
                                        .location = member.location,
                                    });
                                    payload_info.size = p_size;
                                    payload_info.align = p_align;
                                    payload_info.layout_done = true;
                                    program.structs[struct_slot] = std::move(payload_info);
                                }
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

                    // payload_offset = align_up(TAG_SIZE, max_payload_align)
                    const uint32_t effective_payload_align = std::max(max_payload_align, 1u);
                    info.payload_offset = (TAG_SIZE + effective_payload_align - 1) / effective_payload_align * effective_payload_align;

                    // Total align = max(tag_align, payload_align)
                    info.align = std::max(TAG_ALIGN, max_payload_align);
                    // Total size = align_up(payload_offset + max_payload_size, align)
                    const uint32_t raw_size = info.payload_offset + max_payload_size;
                    info.size = (raw_size + info.align - 1) / info.align * info.align;
                    if (info.size == 0) info.size = TAG_ALIGN; // minimum size for tag-only unions
                } else {
                    uint32_t max_size = 0;
                    uint32_t max_align = 1;

                    for (const auto &member : decl->members) {
                        auto member_type = resolve_field_type(module_path, member.type, member.location);
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

            // Wraps a non-struct payload type in a synthetic one-field struct named "v",
            // exactly like layout_union's non-struct-payload branch above — reusing that
            // struct-payload convention keeps every existing payload-reading consumer
            // (emit_variant_capture, TaggedVariantExpr construction) working unchanged for
            // synthesized error-union payloads. A struct payload type is used verbatim
            // (its own slot), no wrapping.
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
            // whose own payload is, in turn, that member type.
            auto synthesize_error_union(const std::string &module_path, const std::vector<ResolvedType> &sorted_members, const SourceLocation &location) -> UnionInfo {
                static constexpr uint32_t TAG_SIZE = 4;
                static constexpr uint32_t TAG_ALIGN = 4;

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

                    const uint32_t eff_align = std::max(max_payload_align, 1u);
                    inner.payload_offset = (TAG_SIZE + eff_align - 1) / eff_align * eff_align;
                    inner.align = std::max(TAG_ALIGN, max_payload_align);
                    const uint32_t raw_size = inner.payload_offset + max_payload_size;
                    inner.size = (raw_size + inner.align - 1) / inner.align * inner.align;
                    if (inner.size == 0) inner.size = TAG_ALIGN;
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
                const uint32_t eff_align = std::max(max_payload_align, 1u);
                outer.payload_offset = (TAG_SIZE + eff_align - 1) / eff_align * eff_align;
                outer.align = std::max(TAG_ALIGN, max_payload_align);
                const uint32_t raw_size = outer.payload_offset + max_payload_size;
                outer.size = (raw_size + outer.align - 1) / outer.align * outer.align;
                if (outer.size == 0) outer.size = TAG_ALIGN;
                outer.layout_done = true;

                return outer;
            }

            // Interns a (possibly newly-synthesized) error union by the SET of its member
            // types — 'error(A|B)' and 'error(B|A)' intern to the same union. Duplicate
            // members are rejected on the original, unsorted list so the diagnostic
            // reflects the user's actual spelling.
            auto intern_error_union(const std::string &module_path, const std::vector<ResolvedType> &members, const SourceLocation &location) -> ResolvedType {
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

                for (const auto &[key, index] : program.error_unions) {
                    if (key == sorted_members) {
                        return ResolvedType{.kind = TypeKind::Union, .union_index = index};
                    }
                }

                const int slot = static_cast<int>(program.unions.size());
                program.unions.push_back(UnionInfo{});
                program.unions[slot] = synthesize_error_union(module_path, sorted_members, location);
                program.error_unions.emplace_back(sorted_members, slot);
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

                return intern_error_union(module_path, members, decl.location);
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
                            return intern_array(program, element, count, size_of(module_path, element) * count, align_of(module_path, element));

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
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::BitsetType>>) {
                            const int slot = static_cast<int>(program.bitsets.size());
                            program.bitsets.push_back(BitsetInfo{});
                            layout_bitset(module_path, slot, v);
                            return ResolvedType{.kind = TypeKind::Bitset, .bitset_index = slot};
                        } else {
                            // Exhaustiveness guard, matching walk_expr_for_foreign_refs in
                            // sema_attributes.cpp. All 13 ast::Type alternatives are handled
                            // above; a 14th added later would previously have fallen in here and
                            // silently resolved to Invalid, surfacing as a confusing "unknown
                            // type" much further along. Now it is a compile error at the site
                            // that needs updating.
                            static_assert(!sizeof(V), "resolve_type_impl: unhandled ast::Type alternative");
                        }
                    },
                    type);
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

    auto is_assignable(const ResolvedType &from, const ResolvedType &to) -> bool {
        if (from == to) return true;
        if (from.kind == TypeKind::Anyptr && to.kind == TypeKind::Pointer) return true;
        if (from.kind == TypeKind::Pointer && to.kind == TypeKind::Anyptr) return true;
        // nil (Anyptr) is assignable to/from function pointer types
        if (from.kind == TypeKind::Anyptr && to.kind == TypeKind::Function) return true;
        if (from.kind == TypeKind::Function && to.kind == TypeKind::Anyptr) return true;
        if (from.kind == TypeKind::Array && to.kind == TypeKind::Slice) return true;
        if (from.kind == TypeKind::Slice && to.kind == TypeKind::Array) return true;
        if (from.kind == TypeKind::Anyptr && to.kind == TypeKind::Slice) return true;
        if (from.kind == TypeKind::Slice && to.kind == TypeKind::Pointer) return true;
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
                                   const SourceLocation &use_loc) -> ResolvedType {
        const GenericInstanceKey key{.module_path = module_path, .decl_name = decl_name, .args = args};

        for (const auto &[k, ty] : program.generic_type_instance_lookup) {
            if (k == key) return ty;
        }

        for (const auto &resolving_key : program.resolve_state.generic_type_resolving) {
            if (resolving_key == key) {
                diag.report_error(DiagnosticStage::Sema, use_loc,
                    std::format("generic type instantiation cycle detected at '{}'", decl_name));
                return ResolvedType{.kind = TypeKind::Invalid};
            }
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

        // Resolve the declaration's RHS with the substitution env active. Deliberately does
        // NOT pre-allocate a slot first: resolve_type_impl's own StructType/EnumType/
        // UnionType/BitsetType cases already allocate-and-lay-out a fresh slot for any
        // by-value type expression they see (the same path an anonymous inline type takes)
        // — this instantiation just reuses that machinery as-is, then tags the resulting
        // slot below.
        const ScopedResolvePush generic_guard(program.resolve_state.generic_type_resolving, key);
        Resolver inner{program, diag, nullptr, &env, &decl.generic_params};
        const auto result = inner.resolve_type_impl(decl.type, module_path);

        GenericInstanceInfo instance_info{.decl_module = module_path, .decl_name = decl_name, .args = args};
        switch (result.kind) {
        case TypeKind::Struct:
            if (result.struct_index >= 0 && static_cast<size_t>(result.struct_index) < program.structs.size())
                program.structs[result.struct_index].generic_instance = instance_info;
            break;
        case TypeKind::Enum:
            if (result.enum_index >= 0 && static_cast<size_t>(result.enum_index) < program.enums.size())
                program.enums[result.enum_index].generic_instance = instance_info;
            break;
        case TypeKind::Union:
            if (result.union_index >= 0 && static_cast<size_t>(result.union_index) < program.unions.size())
                program.unions[result.union_index].generic_instance = instance_info;
            break;
        case TypeKind::Bitset:
            if (result.bitset_index >= 0 && static_cast<size_t>(result.bitset_index) < program.bitsets.size())
                program.bitsets[result.bitset_index].generic_instance = instance_info;
            break;
        default:
            break;
        }

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

    // Mirrors Resolver::size_of() above (and codegen.cpp's Generator::size_of(), which computes
    // the identical thing over its own sema_program_ member) exactly - kept as a public free
    // function since neither of those is reachable from a caller that only has a Program&, which
    // is all the LSP's hover support for size_of()/len() has available.
    auto resolved_type_size(const ResolvedType &t, const Program &program) -> uint32_t {
        if (t.kind == TypeKind::Struct) { const auto *info = program.struct_at(t.struct_index); return info ? info->size : 0; }
        if (t.kind == TypeKind::Array) { const auto *info = program.array_at(t.array_index); return info ? info->size : 0; }
        if (t.kind == TypeKind::Slice) return 16;
        if (t.kind == TypeKind::Trait) return 16;
        if (t.kind == TypeKind::Enum) { const auto *info = program.enum_at(t.enum_index); return info ? primitive_size(info->underlying_type.kind) : 0; }
        if (t.kind == TypeKind::Union) { const auto *info = program.union_at(t.union_index); return info ? info->size : 0; }
        if (t.kind == TypeKind::Bitset) { const auto *info = program.bitset_at(t.bitset_index); return info ? primitive_size(info->storage_type.kind) : 0; }
        return primitive_size(t.kind);
    }

    // Mirrors resolved_type_size() above exactly, substituting alignment for size - including
    // the Trait/Any override (primitive_align would wrongly forward to primitive_size's 16 for
    // these two 16-byte fat pointers, which are actually only 8-byte aligned).
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
