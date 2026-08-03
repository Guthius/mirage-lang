#include "mirgen.hpp"

#include <algorithm>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <ranges>
#include <tuple>
#include <unordered_map>

namespace mirgen {
    namespace {
        // Mangling must agree with codegen.cpp's symbol_name: while both backends exist, a
        // program compiled either way has to produce the same symbols, and the eventual
        // differential test compares them directly.
        auto symbol_name(const std::string_view module_path, const std::string_view name,
                          const bool is_entry_symbol = false) -> std::string {
            if (is_entry_symbol) {
                return std::string(name);
            }
            std::string out = "__mir_";
            for (const char c : module_path) {
                out += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
            }
            out += "_";
            out += name;
            return out;
        }

        class Generator {
          public:
            Generator(const ast::Program &ast_program, const sema::Program &sema_program,
                       DiagnosticEngine &diag, const Options &options)
                : ast_(ast_program), sema_(sema_program), diag_(diag), options_(options) {
                result_.module.name = "mirage";
                result_.module.pointer_bits = options.pointer_bits;
            }

            auto run() -> Result {
                declare_globals();
                declare_functions();
                declare_methods();
                declare_trait_methods();
                declare_generic_functions();
                declare_vtables();
                emit_function_bodies();
                emit_method_bodies();
                emit_trait_method_bodies();
                emit_generic_function_bodies();

                // The verifier is the analogue of llvm::verifyModule, which the LLVM path
                // runs on every compile. Running it here means a lowering bug is reported
                // against the construct that produced it rather than surfacing as wrong code
                // several stages later.
                //
                // Skipped once anything has already been diagnosed: emission bails out of a
                // construct it cannot lower, so a module built in an error state is EXPECTED
                // to be incomplete, and verifying it would bury the real diagnostic under
                // consequences of it.
                if (!diag_.has_errors()) {
                    for (const auto &error : mir::verify(result_.module)) {
                        diag_.report_error(DiagnosticStage::Codegen, {},
                            std::format("internal error: generated MIR is malformed: {}", error.message));
                    }
                }

                result_.ok = !diag_.has_errors();
                return std::move(result_);
            }

          private:
            const ast::Program &ast_;
            const sema::Program &sema_;
            DiagnosticEngine &diag_;
            Options options_;
            Result result_;

            // (module path, name) -> index into mir::Module::functions / ::globals.
            std::unordered_map<std::string, uint32_t> function_index_;
            std::unordered_map<std::string, uint32_t> global_index_;
            // (module path, 'Type::method') -> function index. Kept separate from
            // function_index_ so a method and a free function of the same name cannot
            // collide, which is the same reason codegen keys them apart.
            std::unordered_map<std::string, uint32_t> method_index_;
            // (trait_module, trait_name, type_module, type_name) -> the impl's vtable
            // global, and the six-key variant for synthesized composition sub-vtables --
            // both mirroring codegen's vtables_/component_vtables_. Ordered maps: global
            // emission order must be a function of the source, not of hashes.
            std::map<std::tuple<std::string, std::string, std::string, std::string>, uint32_t> vtable_index_;
            std::map<std::tuple<std::string, std::string, std::string, std::string, std::string, std::string>, uint32_t>
                component_vtable_index_;
            size_t vtable_counter_ = 0;

            // Per-function lowering state.
            const std::string *module_path_ = nullptr;
            const sema::ProgramModule *module_ = nullptr;
            const sema::ExprSideTables *exprs_ = nullptr;
            // Local name -> the slot holding it. Every local is a slot; promote_slots
            // (stage 3) turns the ones whose address never escapes back into values.
            std::unordered_map<std::string, uint32_t> locals_;
            std::unordered_map<std::string, sema::ResolvedType> local_types_;
            // Slots whose address was taken somewhere in the body. Collected during emission
            // and applied once at the end rather than marked inline, because the marking
            // walk (mark_root_slot_escaping) has no builder to hand.
            std::set<uint32_t> slots_escaping_;
            // String literals interned by content: one global per distinct string, matching
            // codegen. Module-scoped, not per function.
            std::unordered_map<std::string, uint32_t> string_globals_;
            // The hidden sret pointer of the body being emitted, or NO_VALUE when the
            // return is a plain scalar. Every 'return' of an aggregate writes through it.
            mir::ValueId sret_ = mir::NO_VALUE;
            // The return list of the body being emitted. 'try' needs it from expression
            // position, where emit_stmt's parameter does not reach.
            const std::vector<sema::ResolvedType> *current_returns_ = nullptr;
            // Enclosing loops, innermost last. 'continue' targets the loop's STEP block, not
            // its condition -- targeting the condition would skip the increment and spin.
            struct LoopTargets { mir::BlockId header; mir::BlockId exit; };
            std::vector<LoopTargets> loop_stack_;
            // The macro substitution map for the template currently being expanded. Each
            // argument captures the CALL SITE's context (module, expr tables, and the
            // macro args active there), because a parameter reference inside the template
            // must evaluate the argument back where it was written -- while the template
            // itself emits under the macro's declaring module.
            struct MacroArg {
                const ast::Expr *expr = nullptr;
                const std::string *module_path = nullptr;
                const sema::ProgramModule *module = nullptr;
                const sema::ExprSideTables *exprs = nullptr;
                std::shared_ptr<const std::unordered_map<std::string, MacroArg>> outer_args;
            };
            std::unordered_map<std::string, MacroArg> macro_args_;

            // Registered 'defer' bodies, one scope per block, innermost last. Emitted in
            // LIFO order at every exit: the block's own end, 'return' (all scopes),
            // 'break'/'continue' (scopes down to and including the loop body's).
            struct DeferScope {
                bool is_loop_body = false;
                std::vector<const ast::DeferStmt *> defers;
            };
            std::vector<DeferScope> defer_scopes_;
            // Set by the loop emitters just before their body, consumed by the BlockStmt
            // case: it marks that block's scope as the boundary 'break'/'continue' unwind to.
            bool next_block_is_loop_body_ = false;

            static auto key(const std::string_view module_path, const std::string_view name) -> std::string {
                return std::string(module_path) + "\n" + std::string(name);
            }

            void unsupported(const std::string &what, const SourceLocation &loc) {
                result_.unsupported.insert(what);
                diag_.report_error(DiagnosticStage::Codegen, loc, std::format(
                    "the native backend cannot lower {} yet; use '--backend=llvm' for this program", what));
            }

            // ---- types -------------------------------------------------------------

            [[nodiscard]] auto size_of(const sema::ResolvedType &type) const -> uint32_t {
                return sema::resolved_type_size(type, sema_);
            }
            [[nodiscard]] auto align_of(const sema::ResolvedType &type) const -> uint32_t {
                return sema::resolved_type_align(type, sema_);
            }

            // The MIR type a sema type lowers to, or Void for anything that lives in memory.
            // Aggregates deliberately have no MIR type: a MIR value is one machine scalar,
            // and everything wider is reached through a pointer (see mir.hpp).
            [[nodiscard]] auto scalar_type(const sema::ResolvedType &type) const -> mir::Ty {
                using K = sema::TypeKind;
                switch (type.kind) {
                case K::Bool:   return mir::Ty::I1;
                case K::U8: case K::I8:   return mir::Ty::I8;
                case K::U16: case K::I16: return mir::Ty::I16;
                case K::U32: case K::I32: return mir::Ty::I32;
                case K::U64: case K::I64: return mir::Ty::I64;
                case K::USize:  return options_.pointer_bits == 64 ? mir::Ty::I64 : mir::Ty::I32;
                case K::F32:    return mir::Ty::F32;
                case K::F64:    return mir::Ty::F64;
                case K::Pointer: case K::Anyptr: case K::Function: return mir::Ty::Ptr;
                case K::Type:   return mir::Ty::I64;
                case K::Enum: {
                    const auto *info = sema_.enum_at(type.enum_index);
                    return info ? scalar_type(info->underlying_type) : mir::Ty::Void;
                }
                case K::Bitset: {
                    const auto *info = sema_.bitset_at(type.bitset_index);
                    return info ? scalar_type(info->storage_type) : mir::Ty::Void;
                }
                default:
                    // Struct, Array, Slice, Trait, Union, Any, Void, Opaque, ...
                    return mir::Ty::Void;
                }
            }

            [[nodiscard]] auto is_scalar(const sema::ResolvedType &type) const -> bool {
                return scalar_type(type) != mir::Ty::Void;
            }

            [[nodiscard]] auto signed_type(const sema::ResolvedType &type) const -> bool {
                using K = sema::TypeKind;
                switch (type.kind) {
                case K::I8: case K::I16: case K::I32: case K::I64: return true;
                case K::Enum: {
                    const auto *info = sema_.enum_at(type.enum_index);
                    return info && signed_type(info->underlying_type);
                }
                default: return false;
                }
            }

            [[nodiscard]] auto expr_type(const ast::Expr &expr) const -> sema::ResolvedType {
                if (!exprs_) return sema::ResolvedType{.kind = sema::TypeKind::Invalid};
                const auto it = exprs_->expr_types.find(sema::get_expr_key(expr));
                return it == exprs_->expr_types.end()
                    ? sema::ResolvedType{.kind = sema::TypeKind::Invalid}
                    : it->second;
            }

            // ---- declarations ------------------------------------------------------

            // Modules in resolver order, matching codegen's modules_in_order(): emission
            // order is a function of the source rather than of an unordered_map's hashes, so
            // MIR text is diffable between builds.
            [[nodiscard]] auto modules_in_order() const
                -> std::vector<std::pair<const std::string *, const sema::ProgramModule *>> {
                std::vector<std::pair<const std::string *, const sema::ProgramModule *>> out;
                std::set<std::string> seen;
                for (const auto &path : ast_.module_order) {
                    const auto it = sema_.modules.find(path);
                    if (it == sema_.modules.end()) continue;
                    out.emplace_back(&it->first, &it->second);
                    seen.insert(path);
                }
                for (const auto &entry : sema_.modules) {
                    if (!seen.contains(entry.first)) out.emplace_back(&entry.first, &entry.second);
                }
                return out;
            }

            void declare_globals() {
                for (const auto &[path_ptr, mod_ptr] : modules_in_order()) {
                    const auto &path = *path_ptr;
                    for (const auto &[name, sym] : mod_ptr->symbols) {
                        if (mod_ptr->bare_import_origins.contains(name)) continue;
                        const auto *global = std::get_if<sema::GlobalSymbol>(&sym);
                        if (!global) continue;

                        mir::Global g;
                        g.name = global->export_name ? *global->export_name : symbol_name(path, name);
                        g.linkage = global->is_pub || global->export_name
                            ? mir::Linkage::External : mir::Linkage::Internal;
                        g.size = size_of(global->type);
                        g.align = std::max(1u, align_of(global->type));
                        g.is_constant = !global->is_mut;
                        // Initializers are lowered in a later increment; a zero-initialized
                        // global is correct for everything sema accepted as 'default'.
                        result_.module.globals.push_back(std::move(g));
                        global_index_[key(path, name)] = static_cast<uint32_t>(result_.module.globals.size() - 1);
                    }
                }
            }

            // The MIR signature for a Mirage function. Aggregates are passed and returned by
            // pointer, which is what "values are scalars" means at a call boundary; the C
            // ABI's own coercion rules are a backend concern applied later, not here.
            [[nodiscard]] auto signature_for(const std::vector<sema::ResolvedType> &params,
                                              const std::vector<sema::ResolvedType> &returns,
                                              const bool is_variadic) -> uint32_t {
                mir::Signature sig;
                sig.is_variadic = is_variadic;
                for (const auto &p : params) {
                    sig.params.push_back(is_scalar(p) ? scalar_type(p) : mir::Ty::Ptr);
                }
                // An aggregate return travels through a hidden leading pointer the CALLER
                // owns. Returning the address of a callee slot instead would dangle the
                // moment the frame went away -- the bug this shape exists to prevent.
                if (returns.size() == 1 && is_scalar(returns.front())) {
                    sig.result = scalar_type(returns.front());
                } else if (!returns.empty()) {
                    sig.params.insert(sig.params.begin(), mir::Ty::Ptr);
                }
                return result_.module.intern_signature(std::move(sig));
            }

            // Whether a return list crosses the boundary through a hidden sret pointer: any
            // aggregate return, and any multi-return. Asked in three places (signature
            // building, parameter binding, call sites) which must agree exactly.
            [[nodiscard]] auto returns_via_sret(const std::vector<sema::ResolvedType> &returns) const -> bool {
                if (returns.empty()) return false;
                return returns.size() > 1 || !is_scalar(returns.front());
            }

            // Byte layout of a return list inside the caller-owned sret blob: each value at
            // its naturally-aligned offset, in declaration order. Sema has no tuple type for
            // a return list, so this is the ONE place that decides the layout; every writer
            // ('return', 'return_ok', 'return_err') and every reader (group declarations,
            // forwarded returns, 'try') must go through it or they disagree silently.
            struct ReturnLayout {
                std::vector<uint32_t> offsets;
                uint32_t size = 0;
                uint32_t align = 1;
            };
            [[nodiscard]] auto multi_return_layout(const std::vector<sema::ResolvedType> &returns) const -> ReturnLayout {
                ReturnLayout out;
                uint32_t cursor = 0;
                for (const auto &ret : returns) {
                    const auto align = std::max(1u, align_of(ret));
                    cursor = (cursor + align - 1) & ~(align - 1);
                    out.offsets.push_back(cursor);
                    cursor += std::max(1u, size_of(ret));
                    out.align = std::max(out.align, align);
                }
                out.size = std::max(1u, (cursor + out.align - 1) & ~(out.align - 1));
                return out;
            }

            // The sema return list of a named function or 'ext fn' in 'path', or nullopt when
            // the name resolves to neither. An 'ext fn' declares at most one return.
            [[nodiscard]] auto symbol_return_types(const std::string &path, const std::string &name) const
                -> std::optional<std::vector<sema::ResolvedType>> {
                const auto mod = sema_.modules.find(path);
                if (mod == sema_.modules.end()) return std::nullopt;
                const auto it = mod->second.symbols.find(name);
                if (it == mod->second.symbols.end()) return std::nullopt;
                if (const auto *fn = std::get_if<sema::FunctionSymbol>(&it->second)) {
                    return fn->return_types;
                }
                if (const auto *ext = std::get_if<sema::ExtFunctionSymbol>(&it->second)) {
                    std::vector<sema::ResolvedType> returns;
                    if (ext->return_type) returns.push_back(*ext->return_type);
                    return returns;
                }
                return std::nullopt;
            }

            // The callee's sema return list, resolved the same way emit_call routes the call:
            // a bare name (with local function-pointer shadowing), 'mod.fn', a method, or a
            // function-pointer value. Needed because a MULTI-return call expression has no
            // recorded expr_type -- only group declarations, forwarded returns and 'try'
            // consume one -- so the sret blob's size cannot come from expr_type there.
            [[nodiscard]] auto callee_return_types(const ast::CallExpr &call) const
                -> std::optional<std::vector<sema::ResolvedType>> {
                // A generic call's return list lives on its resolved INSTANCE; checked
                // first, mirroring emit_call's routing order.
                if (const auto instance = generic_instance_for_call(call, nullptr)) {
                    return sema_.generic_fn_instances[*instance]->return_types;
                }
                const auto through_pointer = [&]() -> std::optional<std::vector<sema::ResolvedType>> {
                    const auto callee_type = lvalue_type(call.callee);
                    if (callee_type.kind != sema::TypeKind::Function) return std::nullopt;
                    const auto *info = sema_.fn_signature_at(callee_type.fn_index);
                    if (!info) return std::nullopt;
                    return info->return_types;
                };

                if (const auto *ident = std::get_if<ast::IdentExpr>(&call.callee)) {
                    if (locals_.contains(ident->name)) return through_pointer();
                    return module_path_ ? symbol_return_types(*module_path_, ident->name) : std::nullopt;
                }
                if (const auto *member = std::get_if<std::unique_ptr<ast::MemberExpr>>(&call.callee)) {
                    if (const auto *target = namespace_target((*member)->object)) {
                        return symbol_return_types(*target, (*member)->member);
                    }
                    const auto receiver_type = expr_type((*member)->object);
                    if (receiver_type.kind == sema::TypeKind::Trait) return std::nullopt;
                    const auto *info = sema::find_method(receiver_type, (*member)->member, sema_);
                    if (!info || !info->is_resolved) return std::nullopt;
                    return info->return_types;
                }
                return through_pointer();
            }

            // The sema parameter list of a named function or 'ext fn', mirroring
            // symbol_return_types; nullopt when the name resolves to neither.
            [[nodiscard]] auto symbol_param_types(const std::string &path, const std::string &name) const
                -> const std::vector<sema::ResolvedType> * {
                const auto mod = sema_.modules.find(path);
                if (mod == sema_.modules.end()) return nullptr;
                const auto it = mod->second.symbols.find(name);
                if (it == mod->second.symbols.end()) return nullptr;
                if (const auto *fn = std::get_if<sema::FunctionSymbol>(&it->second)) return &fn->params;
                if (const auto *ext = std::get_if<sema::ExtFunctionSymbol>(&it->second)) return &ext->params;
                return nullptr;
            }

            [[nodiscard]] auto symbol_function(const std::string &path, const std::string &name) const
                -> const sema::FunctionSymbol * {
                const auto mod = sema_.modules.find(path);
                if (mod == sema_.modules.end()) return nullptr;
                const auto it = mod->second.symbols.find(name);
                if (it == mod->second.symbols.end()) return nullptr;
                return std::get_if<sema::FunctionSymbol>(&it->second);
            }

            // Emits a defaulted argument's expression at the call site, in the CALLEE's own
            // context: the default lives in -- and was checked into the tables of -- the
            // declaring module, with no caller locals in scope (a caller local sharing a
            // name with something the default references must not shadow it).
            auto emit_default_arg(mir::Builder &b, const ast::Expr &default_expr,
                                   const std::string &declaring_module,
                                   const sema::ExprSideTables *exprs_override) -> mir::ValueId {
                const auto mod = sema_.modules.find(declaring_module);
                if (mod == sema_.modules.end()) return mir::NO_VALUE;
                const auto *saved_path = module_path_;
                const auto *saved_module = module_;
                const auto *saved_exprs = exprs_;
                auto saved_locals = std::move(locals_);
                auto saved_types = std::move(local_types_);
                module_path_ = &mod->first;
                module_ = &mod->second;
                exprs_ = exprs_override ? exprs_override : &mod->second.exprs;
                locals_.clear();
                local_types_.clear();
                const auto value = emit_expr(b, default_expr);
                module_path_ = saved_path;
                module_ = saved_module;
                exprs_ = saved_exprs;
                locals_ = std::move(saved_locals);
                local_types_ = std::move(saved_types);
                return value;
            }

            // Representation changes between an argument's own type and the parameter's,
            // the argument-position half of store_aggregate_value: an array where a slice
            // is expected materializes a (data, len) header; a slice where a bare pointer
            // is expected passes its data word. Scalar width stays coerce_to's job.
            auto coerce_arg(mir::Builder &b, const mir::ValueId value, const sema::ResolvedType &param,
                             const sema::ResolvedType &arg) -> mir::ValueId {
                if (param.kind == sema::TypeKind::Slice && arg.kind == sema::TypeKind::Array) {
                    const auto *info = sema_.array_at(arg.array_index);
                    if (!info) return value;
                    const auto slot = b.add_slot(pointer_bytes() * 2, pointer_bytes(), "slice");
                    const auto base = b.slot_addr(slot);
                    b.store(base, value);
                    b.store(b.ptr_add_const(base, pointer_bytes()),
                             b.const_int(usize_ty(), static_cast<int64_t>(info->count)));
                    return base;
                }
                if ((param.kind == sema::TypeKind::Pointer || param.kind == sema::TypeKind::Anyptr) &&
                    arg.kind == sema::TypeKind::Slice) {
                    return b.load(mir::Ty::Ptr, value);
                }
                return value;
            }

            // The monomorphized instance a call resolved to, if any. Sema keys the record
            // two ways -- get_expr_key's VARIANT-SLOT address for a value call, the
            // CallExpr's own address for a group/forwarded/'try' call, which never sees the
            // outer Expr variant -- so both are consulted (codegen reads each back where it
            // was written, split across two sites; one funnel here instead).
            [[nodiscard]] auto generic_instance_for_call(const ast::CallExpr &call, const ast::Expr *expr) const
                -> std::optional<size_t> {
                if (!exprs_) return std::nullopt;
                const auto &table = exprs_->expr_generic_fn_instance;
                if (expr) {
                    if (const auto it = table.find(sema::get_expr_key(*expr));
                        it != table.end() && it->second < sema_.generic_fn_instances.size()) {
                        return it->second;
                    }
                }
                if (const auto it = table.find(&call);
                    it != table.end() && it->second < sema_.generic_fn_instances.size()) {
                    return it->second;
                }
                return std::nullopt;
            }

            [[nodiscard]] auto is_error_union(const sema::ResolvedType &type) const -> bool {
                const auto *info = type.kind == sema::TypeKind::Union ? sema_.union_at(type.union_index) : nullptr;
                return info && info->is_error_union;
            }

            void declare_functions() {
                for (const auto &[path_ptr, mod_ptr] : modules_in_order()) {
                    const auto &path = *path_ptr;
                    for (const auto &[name, sym] : mod_ptr->symbols) {
                        if (mod_ptr->bare_import_origins.contains(name)) continue;

                        if (const auto *fn = std::get_if<sema::FunctionSymbol>(&sym)) {
                            if (fn->decl && !fn->decl->generic_params.empty()) continue;
                            if (fn->is_test) continue;

                            const bool entry = path == ast_.root_module_path && name == "main";
                            mir::Function f;
                            f.name = fn->export_name ? *fn->export_name : symbol_name(path, name, entry);
                            f.linkage = fn->is_pub || entry || fn->export_name
                                ? mir::Linkage::External : mir::Linkage::Internal;
                            f.conv = fn->call_conv == sema::CallConv::C ? mir::CallConv::C : mir::CallConv::Mirage;
                            f.signature = signature_for(fn->params, fn->return_types, fn->is_variadic);
                            f.has_body = true;
                            result_.module.functions.push_back(std::move(f));
                            function_index_[key(path, name)] =
                                static_cast<uint32_t>(result_.module.functions.size() - 1);

                        } else if (const auto *ext = std::get_if<sema::ExtFunctionSymbol>(&sym)) {
                            // 'ext fn's are process-globally deduplicated by bare name, as in
                            // codegen: two modules declaring the same one must not produce two
                            // MIR functions, or the linker sees a duplicate.
                            if (const auto seen = std::ranges::find(result_.module.functions, ext->decl->name,
                                                                      &mir::Function::name);
                                seen != result_.module.functions.end()) {
                                function_index_[key(path, name)] =
                                    static_cast<uint32_t>(std::distance(result_.module.functions.begin(), seen));
                                continue;
                            }
                            mir::Function f;
                            f.name = ext->decl->name;
                            f.linkage = mir::Linkage::External;
                            f.conv = mir::CallConv::C;
                            std::vector<sema::ResolvedType> returns;
                            if (ext->return_type) returns.push_back(*ext->return_type);
                            f.signature = signature_for(ext->params, returns, ext->is_variadic);
                            f.has_body = false;
                            f.import_module = ext->import_module;
                            f.import_name = ext->import_name;
                            result_.module.functions.push_back(std::move(f));
                            function_index_[key(path, name)] =
                                static_cast<uint32_t>(result_.module.functions.size() - 1);
                        }
                    }
                }
            }

            // ---- bodies ------------------------------------------------------------
            // Bare-impl methods ('impl T { fn f(self) }'). Mangled exactly as codegen does --
            // 'Type::method' inside the module's symbol name -- so both backends emit the
            // same symbol and the eventual differential test can compare them directly.
            //
            // Trait-impl methods use a distinct key ('Type::Trait::method') so an inherent
            // and a trait method of the same name on the same type cannot collide; those are
            // declared here too, since a trait impl is still a concrete function even before
            // dynamic dispatch is lowered.
            static auto method_key(const std::string &type_name, const std::string &method_name) -> std::string {
                return type_name + "::" + method_name;
            }
            static auto trait_method_key(const std::string &type_name, const std::string &trait_name,
                                          const std::string &method_name) -> std::string {
                return type_name + "::" + trait_name + "::" + method_name;
            }
            static auto key_for_method(const sema::MethodInfo &info) -> std::string {
                return info.trait_name
                    ? trait_method_key(info.type_name, *info.trait_name, info.decl->name)
                    : method_key(info.type_name, info.decl->name);
            }

            // A method's MIR signature: the receiver is a leading pointer parameter, which is
            // how 'self' crosses the boundary regardless of whether the receiver is a value
            // or a pointer at the source level.
            [[nodiscard]] auto method_signature(const sema::MethodInfo &info) -> uint32_t {
                mir::Signature sig;
                sig.params.push_back(mir::Ty::Ptr); // self
                for (const auto &p : info.param_types) {
                    sig.params.push_back(is_scalar(p) ? scalar_type(p) : mir::Ty::Ptr);
                }
                if (info.return_types.size() == 1 && is_scalar(info.return_types.front())) {
                    sig.result = scalar_type(info.return_types.front());
                } else if (!info.return_types.empty()) {
                    sig.params.insert(sig.params.begin(), mir::Ty::Ptr); // sret
                }
                return result_.module.intern_signature(std::move(sig));
            }

            void declare_methods() {
                for (const auto &[path_ptr, mod_ptr] : modules_in_order()) {
                    const auto &path = *path_ptr;
                    for (const auto &[type_name, method_map] : mod_ptr->methods) {
                        for (const auto &info : method_map | std::views::values) {
                            // A generic type's methods are never signature-resolved; each
                            // instantiation is a separate function, which generics support
                            // will declare.
                            if (!info.is_resolved || !info.decl) continue;

                            const auto k = key_for_method(info);
                            mir::Function f;
                            f.name = info.export_name ? *info.export_name : symbol_name(path, k);
                            f.linkage = info.export_name ? mir::Linkage::External : mir::Linkage::Internal;
                            f.signature = method_signature(info);
                            f.has_body = true;
                            result_.module.functions.push_back(std::move(f));
                            method_index_[key(path, k)] = static_cast<uint32_t>(result_.module.functions.size() - 1);
                        }
                    }
                }
            }

            // Monomorphized generic instances, keyed by their index into
            // Program::generic_fn_instances. Only the instances sema marked as needed are
            // declared -- generic_fn_instances_needed is a std::set, so declaration order
            // is deterministic. Mangling must match codegen's:
            // symbol_name(module, instance.mangled_name).
            std::unordered_map<size_t, uint32_t> generic_instance_index_;

            void declare_generic_functions() {
                for (const auto idx : sema_.generic_fn_instances_needed) {
                    const auto &instance = *sema_.generic_fn_instances[idx];

                    mir::Signature sig;
                    if (returns_via_sret(instance.return_types)) {
                        sig.params.push_back(mir::Ty::Ptr); // sret
                    } else if (!instance.return_types.empty()) {
                        sig.result = scalar_type(instance.return_types.front());
                    }
                    if (instance.impl_decl) {
                        sig.params.push_back(mir::Ty::Ptr); // self
                    }
                    for (const auto &p : instance.param_types) {
                        sig.params.push_back(is_scalar(p) ? scalar_type(p) : mir::Ty::Ptr);
                    }

                    mir::Function f;
                    f.name = symbol_name(instance.module_path, instance.mangled_name);
                    f.linkage = mir::Linkage::Internal;
                    f.signature = result_.module.intern_signature(std::move(sig));
                    f.has_body = true;
                    result_.module.functions.push_back(std::move(f));
                    generic_instance_index_[idx] = static_cast<uint32_t>(result_.module.functions.size() - 1);
                }
            }

            void emit_generic_function_bodies() {
                for (const auto idx : sema_.generic_fn_instances_needed) {
                    const auto it = generic_instance_index_.find(idx);
                    const auto &instance = *sema_.generic_fn_instances[idx];
                    const auto mod = sema_.modules.find(instance.module_path);
                    if (it == generic_instance_index_.end() || mod == sema_.modules.end()) continue;

                    module_path_ = &mod->first;
                    module_ = &mod->second;
                    // The whole body was checked into THIS instance's records, not the
                    // module's -- every monomorphization of one generic walks the same AST
                    // nodes, so per-node tables must be per-instance.
                    exprs_ = sema_.find_fn_instance_exprs(idx);
                    if (!exprs_) exprs_ = &mod->second.exprs;

                    // The ambient env serves the few sites (a local's own type annotation,
                    // e.g. 'mut buf: [N]u8') that re-derive a type from an ast::Type node
                    // via resolve_declared_type instead of reading expr_types.
                    const auto &generic_params = instance.decl
                        ? instance.decl->generic_params : *instance.generic_params_for_method;
                    const auto env = sema::build_generic_binding_env(generic_params, instance.args);
                    const sema::ActiveGenericEnvStack::PushGuard env_guard(
                        const_cast<sema::Program &>(sema_).active_generic_env_stack, &env);

                    emit_generic_instance_body(it->second, instance);
                }
                module_path_ = nullptr;
                module_ = nullptr;
                exprs_ = nullptr;
            }

            void emit_generic_instance_body(const uint32_t fn_index, const sema::GenericFunctionInstance &instance) {
                mir::Builder b(result_.module, fn_index);
                locals_.clear();
                local_types_.clear();
                slots_escaping_.clear();
                sret_ = mir::NO_VALUE;
                loop_stack_.clear();
                defer_scopes_.clear();
                next_block_is_loop_body_ = false;
                macro_args_.clear();

                const auto entry = b.create_block("entry");
                b.set_insert_point(entry);
                const auto &sig = result_.module.signatures[result_.module.functions[fn_index].signature];

                size_t first_param = 0;
                if (returns_via_sret(instance.return_types)) {
                    sret_ = b.add_block_param(entry, mir::Ty::Ptr);
                    result_.module.functions[fn_index].params.push_back(sret_);
                    first_param = 1;
                }
                if (instance.impl_decl && instance.self_type) {
                    // 'self' arrives as a pointer and is bound as one, exactly as in
                    // emit_method_body.
                    const auto self_value = b.add_block_param(entry, mir::Ty::Ptr);
                    result_.module.functions[fn_index].params.push_back(self_value);
                    const auto self_slot = b.add_slot(pointer_bytes(), pointer_bytes(), "self");
                    b.store(b.slot_addr(self_slot), self_value);
                    locals_["self"] = self_slot;
                    local_types_["self"] = pointer_type_to(*instance.self_type);
                    first_param += 1;
                }

                // FunctionDecl::Param and ImplDecl::Function::Param are distinct types;
                // only the names are needed here.
                std::vector<const std::string *> param_names;
                if (instance.decl) {
                    for (const auto &p : instance.decl->params) param_names.push_back(&p.name);
                } else {
                    for (const auto &p : instance.impl_decl->params) param_names.push_back(&p.name);
                }
                for (size_t i = 0; i < instance.param_types.size() && i + first_param < sig.params.size(); ++i) {
                    const auto value = b.add_block_param(entry, sig.params[i + first_param]);
                    result_.module.functions[fn_index].params.push_back(value);
                    if (i < param_names.size()) {
                        bind_param(b, *param_names[i], instance.param_types[i], value);
                    }
                }

                current_returns_ = &instance.return_types;
                emit_stmt(b, instance.decl ? instance.decl->body : instance.impl_decl->body,
                           instance.return_types);
                current_returns_ = nullptr;

                for (const auto slot : slots_escaping_) {
                    b.mark_slot_escaping(slot);
                }
                if (!b.block_is_terminated()) {
                    if (instance.return_types.empty()) b.ret();
                    else b.unreachable();
                }
            }

            // Trait-impl methods live ONLY in Program::trait_impls_by_type -- an
            // 'impl TRAIT for TYPE' block's methods are never entered into
            // ProgramModule::methods -- so they need their own declare/emit passes,
            // mirroring codegen's declare_trait_methods/emit_trait_methods. The
            // method_index_ guard keeps a method that IS also reachable through the module
            // map from being declared twice under the same symbol.
            std::vector<std::pair<const sema::TraitImplInfo *, const sema::MethodInfo *>> trait_method_bodies_;

            void declare_trait_methods() {
                for (const auto &impls : sema_.trait_impls_by_type | std::views::values) {
                    for (const auto &impl_info : impls) {
                        for (const auto &info : impl_info.methods | std::views::values) {
                            if (!info.is_resolved || !info.decl) continue;
                            const auto k = key(impl_info.impl_module, key_for_method(info));
                            if (method_index_.contains(k)) continue;

                            mir::Function f;
                            f.name = info.export_name
                                ? *info.export_name
                                : symbol_name(impl_info.impl_module, key_for_method(info));
                            f.linkage = info.export_name ? mir::Linkage::External : mir::Linkage::Internal;
                            f.signature = method_signature(info);
                            f.has_body = true;
                            result_.module.functions.push_back(std::move(f));
                            method_index_[k] = static_cast<uint32_t>(result_.module.functions.size() - 1);
                            trait_method_bodies_.emplace_back(&impl_info, &info);
                        }
                    }
                }
            }

            void emit_trait_method_bodies() {
                for (const auto &[impl_info, info] : trait_method_bodies_) {
                    const auto mod = sema_.modules.find(impl_info->impl_module);
                    if (mod == sema_.modules.end()) continue;
                    module_path_ = &mod->first;
                    module_ = &mod->second;
                    exprs_ = &mod->second.exprs;
                    const auto it = method_index_.find(key(impl_info->impl_module, key_for_method(*info)));
                    if (it != method_index_.end()) {
                        emit_method_body(it->second, *info);
                    }
                }
                module_path_ = nullptr;
                module_ = nullptr;
                exprs_ = nullptr;
            }

            // Vtable globals, one per 'impl TRAIT for TYPE' plus one synthesized
            // sub-vtable per composed component (codegen's declare_vtables): an array of
            // code pointers in TraitInfo::methods order -- that order IS the vtable layout
            // and is never re-derived -- followed by one back-pointer slot per component
            // trait, in component_traits order. MIR globals carry every entry as a
            // relocation, since no address exists until layout; a missing method leaves a
            // null slot, which only happens when sema already reported the conformance
            // error.
            void declare_vtables() {
                for (const auto &impls : sema_.trait_impls_by_type | std::views::values) {
                    for (const auto &impl_info : impls) {
                        const auto *trait_info = sema_.trait_at(impl_info.trait_index);
                        if (!trait_info) continue;

                        struct FamilyMember {
                            const sema::TraitInfo *info;
                            std::string module_path, name;
                            int trait_index;
                            uint32_t global_index = 0;
                        };
                        std::vector<FamilyMember> family;
                        family.push_back({trait_info, impl_info.trait_module, impl_info.trait_name,
                                           impl_info.trait_index});
                        for (const auto &c : trait_info->component_traits) {
                            const auto *c_info = sema_.trait_at(c.trait_index);
                            if (!c_info) continue;
                            family.push_back({c_info, c.module_path, c.name, c.trait_index});
                        }

                        // Pass 1: create every family member's global first, so Pass 2 can
                        // freely reference any other member's address.
                        for (auto &fm : family) {
                            const auto slots = static_cast<uint32_t>(
                                fm.info->methods.size() + fm.info->component_traits.size());
                            mir::Global g;
                            g.name = std::format(".vtable.{}", vtable_counter_++);
                            g.linkage = mir::Linkage::Internal;
                            g.is_constant = true;
                            g.size = std::max(1u, slots * pointer_bytes());
                            g.align = pointer_bytes();
                            g.init.assign(g.size, 0);
                            result_.module.globals.push_back(std::move(g));
                            fm.global_index = static_cast<uint32_t>(result_.module.globals.size() - 1);

                            if (fm.trait_index == impl_info.trait_index) {
                                vtable_index_[{impl_info.trait_module, impl_info.trait_name,
                                                impl_info.type_module, impl_info.type_name}] = fm.global_index;
                            } else {
                                component_vtable_index_[{impl_info.trait_module, impl_info.trait_name,
                                                          fm.module_path, fm.name,
                                                          impl_info.type_module, impl_info.type_name}] = fm.global_index;
                            }
                        }

                        // Pass 2: the entries, as relocations.
                        for (const auto &fm : family) {
                            auto &g = result_.module.globals[fm.global_index];
                            uint32_t offset = 0;
                            for (const auto &trait_method : fm.info->methods) {
                                if (const auto method_it = impl_info.methods.find(trait_method.name);
                                    method_it != impl_info.methods.end()) {
                                    if (const auto fn_it = method_index_.find(
                                            key(impl_info.impl_module, key_for_method(method_it->second)));
                                        fn_it != method_index_.end()) {
                                        g.relocations.push_back({.kind = mir::Relocation::Kind::FunctionAddr,
                                                                  .offset = offset, .target = fn_it->second});
                                    }
                                }
                                offset += pointer_bytes();
                            }
                            for (const auto &c : fm.info->component_traits) {
                                uint32_t sub = UINT32_MAX;
                                if (c.trait_index == impl_info.trait_index) {
                                    if (const auto it = vtable_index_.find({impl_info.trait_module, impl_info.trait_name,
                                                                             impl_info.type_module, impl_info.type_name});
                                        it != vtable_index_.end()) {
                                        sub = it->second;
                                    }
                                } else if (const auto it = component_vtable_index_.find(
                                               {impl_info.trait_module, impl_info.trait_name, c.module_path, c.name,
                                                impl_info.type_module, impl_info.type_name});
                                           it != component_vtable_index_.end()) {
                                    sub = it->second;
                                }
                                if (sub != UINT32_MAX) {
                                    g.relocations.push_back({.kind = mir::Relocation::Kind::GlobalAddr,
                                                              .offset = offset, .target = sub});
                                }
                                offset += pointer_bytes();
                            }
                        }
                    }
                }
            }

            void emit_method_bodies() {
                for (const auto &[path_ptr, mod_ptr] : modules_in_order()) {
                    const auto &path = *path_ptr;
                    module_path_ = &path;
                    module_ = mod_ptr;
                    exprs_ = &mod_ptr->exprs;

                    for (const auto &[type_name, method_map] : mod_ptr->methods) {
                        for (const auto &info : method_map | std::views::values) {
                            if (!info.is_resolved || !info.decl) continue;
                            const auto it = method_index_.find(key(path, key_for_method(info)));
                            if (it == method_index_.end()) continue;
                            emit_method_body(it->second, info);
                        }
                    }
                }
                module_path_ = nullptr;
                module_ = nullptr;
                exprs_ = nullptr;
            }

            void emit_method_body(const uint32_t fn_index, const sema::MethodInfo &info) {
                mir::Builder b(result_.module, fn_index);
                locals_.clear();
                local_types_.clear();
                slots_escaping_.clear();
                sret_ = mir::NO_VALUE;
                loop_stack_.clear();
                defer_scopes_.clear();
                next_block_is_loop_body_ = false;
                macro_args_.clear();

                const auto entry = b.create_block("entry");
                b.set_insert_point(entry);

                const auto &sig = result_.module.signatures[result_.module.functions[fn_index].signature];

                size_t first_param = 0;
                if (returns_via_sret(info.return_types)) {
                    sret_ = b.add_block_param(entry, mir::Ty::Ptr);
                    result_.module.functions[fn_index].params.push_back(sret_);
                    first_param = 1;
                }

                // 'self' arrives as a pointer. It is bound directly to that pointer rather
                // than spilled: the receiver IS an address, and member access on it goes
                // through the same pointer-object path 'p.x' on a '*Point' uses.
                const auto self_value = b.add_block_param(entry, mir::Ty::Ptr);
                result_.module.functions[fn_index].params.push_back(self_value);
                const auto self_slot = b.add_slot(pointer_bytes(), pointer_bytes(), "self");
                b.store(b.slot_addr(self_slot), self_value);
                locals_["self"] = self_slot;
                local_types_["self"] = self_pointer_type(info);

                for (size_t i = 0; i < info.param_types.size() && i + first_param + 1 < sig.params.size(); ++i) {
                    const auto value = b.add_block_param(entry, sig.params[i + first_param + 1]);
                    result_.module.functions[fn_index].params.push_back(value);
                    const auto &decl_param = info.decl->params[i];
                    bind_param(b, decl_param.name, info.param_types[i], value);
                }

                current_returns_ = &info.return_types;
                emit_stmt(b, info.decl->body, info.return_types);
                current_returns_ = nullptr;

                for (const auto slot : slots_escaping_) {
                    b.mark_slot_escaping(slot);
                }
                if (!b.block_is_terminated()) {
                    if (info.return_types.empty()) b.ret();
                    else b.unreachable();
                }
            }

            // The interned '*T' for a pointee, or the bare type when the program never
            // formed that pointer type -- field offsets still resolve either way, and the
            // address form is what emit_member_address actually uses. Shared by 'self'
            // binding and by-reference match captures, which are the same trick: a local
            // whose slot holds an address.
            [[nodiscard]] auto pointer_type_to(const sema::ResolvedType &pointee) const -> sema::ResolvedType {
                for (size_t i = 0; i < sema_.pointer_pointees.size(); ++i) {
                    if (sema_.pointer_pointees[i] == pointee) {
                        return sema::ResolvedType{.kind = sema::TypeKind::Pointer,
                                                   .pointee_index = static_cast<int>(i)};
                    }
                }
                return pointee;
            }

            // 'self' is a pointer to the receiver type, which is what makes 'self.field'
            // resolve through the same auto-deref path as any other pointer object.
            [[nodiscard]] auto self_pointer_type(const sema::MethodInfo &info) const -> sema::ResolvedType {
                return pointer_type_to(info.self_type);
            }



            void emit_function_bodies() {
                for (const auto &[path_ptr, mod_ptr] : modules_in_order()) {
                    const auto &path = *path_ptr;
                    module_path_ = &path;
                    module_ = mod_ptr;
                    exprs_ = &mod_ptr->exprs;

                    for (const auto &[name, sym] : mod_ptr->symbols) {
                        if (mod_ptr->bare_import_origins.contains(name)) continue;
                        const auto *fn = std::get_if<sema::FunctionSymbol>(&sym);
                        if (!fn || !fn->decl) continue;
                        if (!fn->decl->generic_params.empty() || fn->is_test) continue;

                        const auto it = function_index_.find(key(path, name));
                        if (it == function_index_.end()) continue;
                        emit_body(it->second, *fn);
                    }
                }
                module_path_ = nullptr;
                module_ = nullptr;
                exprs_ = nullptr;
            }

            void emit_body(const uint32_t fn_index, const sema::FunctionSymbol &fn) {
                mir::Builder b(result_.module, fn_index);
                locals_.clear();
                local_types_.clear();
                slots_escaping_.clear();
                sret_ = mir::NO_VALUE;
                loop_stack_.clear();
                defer_scopes_.clear();
                next_block_is_loop_body_ = false;
                macro_args_.clear();

                const auto entry = b.create_block("entry");
                b.set_insert_point(entry);

                // Parameters arrive as block parameters of the entry block and are
                // immediately spilled to slots, matching the memory form the front end
                // emits. promote_slots undoes this for the ones that never escape.
                const auto &sig = result_.module.signatures[result_.module.functions[fn_index].signature];

                size_t first_param = 0;
                if (returns_via_sret(fn.return_types)) {
                    sret_ = b.add_block_param(entry, mir::Ty::Ptr);
                    result_.module.functions[fn_index].params.push_back(sret_);
                    first_param = 1;
                }

                for (size_t i = 0; i < fn.params.size() && i + first_param < sig.params.size(); ++i) {
                    const auto param_value = b.add_block_param(entry, sig.params[i + first_param]);
                    result_.module.functions[fn_index].params.push_back(param_value);
                    bind_param(b, fn.decl->params[i].name, fn.params[i], param_value);
                }

                current_returns_ = &fn.return_types;
                emit_stmt(b, fn.decl->body, fn.return_types);
                current_returns_ = nullptr;

                for (const auto slot : slots_escaping_) {
                    b.mark_slot_escaping(slot);
                }

                if (!b.block_is_terminated()) {
                    if (fn.return_types.empty()) {
                        b.ret();
                    } else {
                        // sema already reports a genuinely missing return; reaching here means
                        // the fall-through is unreachable (both branches returned) or a
                        // construct mirgen skipped left the block open.
                        b.unreachable();
                    }
                }
            }

            void emit_defers_in_scope(mir::Builder &b, const DeferScope &scope,
                                       const std::vector<sema::ResolvedType> &returns) {
                for (const auto &deferred : scope.defers | std::views::reverse) {
                    emit_stmt(b, deferred->body, returns);
                }
            }

            void emit_defers_for_return(mir::Builder &b, const std::vector<sema::ResolvedType> &returns) {
                // Snapshot: a defer body's own blocks push and pop scopes while this walks.
                const auto scopes = defer_scopes_;
                for (const auto &scope : scopes | std::views::reverse) {
                    emit_defers_in_scope(b, scope, returns);
                }
            }

            void emit_defers_for_loop_exit(mir::Builder &b, const std::vector<sema::ResolvedType> &returns) {
                const auto scopes = defer_scopes_;
                for (const auto &scope : scopes | std::views::reverse) {
                    emit_defers_in_scope(b, scope, returns);
                    if (scope.is_loop_body) break;
                }
            }

            // Binds one incoming parameter to a fresh local slot. A scalar arrives as its
            // value and is stored; an aggregate arrives as a POINTER to the caller's copy
            // (see signature_for) and its BYTES are copied in -- storing the pointer itself
            // into an aggregate-sized slot left every later read (which treats the slot as
            // holding the aggregate) reading the pointer's bytes as data.
            void bind_param(mir::Builder &b, const std::string &name, const sema::ResolvedType &type,
                             const mir::ValueId value) {
                const auto slot = b.add_slot(std::max(1u, size_of(type)),
                                              std::max(1u, align_of(type)), name);
                if (is_scalar(type)) {
                    b.store(b.slot_addr(slot), value);
                } else {
                    b.mem_copy(b.slot_addr(slot), value, b.const_int(usize_ty(), size_of(type)));
                }
                locals_[name] = slot;
                local_types_[name] = type;
            }

            void emit_stmt(mir::Builder &b, const ast::Stmt &stmt, const std::vector<sema::ResolvedType> &returns) {
                if (b.block_is_terminated()) {
                    return;
                }
                std::visit([&]<typename T>(const T &v) {
                    using V = std::decay_t<T>;

                    if constexpr (std::is_same_v<V, std::unique_ptr<ast::BlockStmt>>) {
                        // Locals are function-scoped slots, so a block needs no scope of its
                        // own beyond restoring shadowed names -- and a defer scope.
                        const auto saved_locals = locals_;
                        const auto saved_types = local_types_;
                        const bool is_loop_body = next_block_is_loop_body_;
                        next_block_is_loop_body_ = false;
                        defer_scopes_.push_back(DeferScope{.is_loop_body = is_loop_body});
                        for (const auto &s : v->stmts) {
                            emit_stmt(b, s, returns);
                            if (b.block_is_terminated()) break;
                        }
                        // Falling off the block's end runs its defers; a terminated block
                        // already ran them at whatever terminated it. Emitted BEFORE the
                        // locals are restored: a defer body references the block's own names.
                        if (!b.block_is_terminated()) {
                            emit_defers_in_scope(b, defer_scopes_.back(), returns);
                        }
                        defer_scopes_.pop_back();
                        locals_ = saved_locals;
                        local_types_ = saved_types;

                    } else if constexpr (std::is_same_v<V, ast::VarDeclStmt>) {
                        emit_var_decl(b, v);

                    } else if constexpr (std::is_same_v<V, ast::VarDeclGroupStmt>) {
                        emit_var_decl_group(b, v);

                    } else if constexpr (std::is_same_v<V, ast::ExprStmt>) {
                        (void) emit_expr(b, v.expr);

                    } else if constexpr (std::is_same_v<V, ast::ReturnStmt>) {
                        emit_return(b, v, returns);

                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::IfStmt>>) {
                        emit_if(b, *v, returns);

                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::WhileStmt>>) {
                        emit_while(b, *v, returns);

                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::ForInStmt>>) {
                        emit_for_in(b, *v, returns);

                    } else if constexpr (std::is_same_v<V, ast::BreakStmt>) {
                        if (loop_stack_.empty()) {
                            unsupported("'break' outside a loop", v.location);
                        } else {
                            emit_defers_for_loop_exit(b, returns);
                            b.jump(loop_stack_.back().exit);
                        }

                    } else if constexpr (std::is_same_v<V, ast::ContinueStmt>) {
                        if (loop_stack_.empty()) {
                            unsupported("'continue' outside a loop", v.location);
                        } else {
                            emit_defers_for_loop_exit(b, returns);
                            b.jump(loop_stack_.back().header);
                        }

                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::DeferStmt>>) {
                        // Registered now, emitted at scope exit. A function body outside
                        // any block scope cannot happen -- the body IS a BlockStmt.
                        if (defer_scopes_.empty()) {
                            unsupported("'defer' at this position", v->location);
                        } else {
                            defer_scopes_.back().defers.push_back(v.get());
                        }

                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::WhenStmt>>) {
                        emit_when(b, *v, returns);

                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::SwitchStmt>>) {
                        emit_switch(b, *v, returns);

                    } else if constexpr (std::is_same_v<V, ast::ReturnOkStmt>) {
                        emit_return_ok(b, v, returns);

                    } else if constexpr (std::is_same_v<V, ast::ReturnErrStmt>) {
                        emit_return_err(b, v, returns);

                    } else {
                        unsupported(stmt_kind_name<V>(), stmt_location(stmt));
                    }
                }, stmt);
            }

            template <typename V>
            static auto stmt_kind_name() -> const char * {
                if constexpr (std::is_same_v<V, std::unique_ptr<ast::ForInStmt>>) return "a 'for-in' loop";
                else if constexpr (std::is_same_v<V, std::unique_ptr<ast::SwitchStmt>>) return "a 'switch' statement";
                else if constexpr (std::is_same_v<V, std::unique_ptr<ast::WhenStmt>>) return "a 'when' statement";
                else if constexpr (std::is_same_v<V, std::unique_ptr<ast::AsmStmt>>) return "an 'asm' block";
                else if constexpr (std::is_same_v<V, ast::BreakStmt>) return "'break'";
                else if constexpr (std::is_same_v<V, ast::ContinueStmt>) return "'continue'";
                else if constexpr (std::is_same_v<V, ast::LinkDecl>) return "'#link'";
                else if constexpr (std::is_same_v<V, ast::ReturnErrStmt>) return "'return_err'";
                else if constexpr (std::is_same_v<V, ast::ReturnOkStmt>) return "'return_ok'";
                else if constexpr (std::is_same_v<V, ast::DiagnosticDecl>) return "'#error'/'#warn'";
                else return "an unrecognized statement";
            }

            static auto stmt_location(const ast::Stmt &stmt) -> SourceLocation {
                return std::visit([](const auto &v) -> SourceLocation {
                    if constexpr (requires { v->location; }) return v->location;
                    else return v.location;
                }, stmt);
            }

            void emit_var_decl(mir::Builder &b, const ast::VarDeclStmt &decl) {
                auto type = decl.init ? expr_type(*decl.init) : sema::ResolvedType{};
                // An implicit tagged-union coercion on the initializer: the LOCAL is the
                // union, even though the initializer expression's own recorded type is the
                // payload -- expr_types is left untouched by sema, the coercion is a side
                // table (see emit_expr's wrapping).
                if (decl.init && exprs_) {
                    if (const auto it = exprs_->expr_variant_coercions.find(sema::get_expr_key(*decl.init));
                        it != exprs_->expr_variant_coercions.end()) {
                        type = it->second.union_type;
                    }
                }
                // A declared type wins over the initializer's own: 'mut out: [20]u8 = s'
                // declares an ARRAY however the slice initializer was typed. Resolved the
                // way codegen does it -- a no-op re-walk by now, since sema interned every
                // type this can name, which is what makes the const_cast safe.
                auto resolved = local_declared_type(decl, type);
                if (decl.type && module_path_) {
                    if (const auto declared = sema::resolve_declared_type(
                            decl.type, decl.init, *module_path_,
                            const_cast<sema::Program &>(sema_), diag_, decl.location)) {
                        resolved = *declared;
                    }
                }
                const auto slot = b.add_slot(std::max(1u, size_of(resolved)),
                                              std::max(1u, align_of(resolved)), decl.name);
                locals_[decl.name] = slot;
                local_types_[decl.name] = resolved;

                if (!decl.init) {
                    // No initializer means zero-valued (codegen's emit_default_value);
                    // leaving the slot's garbage in place would silently diverge.
                    zero_slot(b, slot, resolved);
                    return;
                }
                // 'undefined' is the explicit opt-out: the slot stays uninitialized.
                if (std::holds_alternative<ast::UndefinedExpr>(*decl.init)) {
                    return;
                }
                if (!is_scalar(resolved)) {
                    // 'default' on an aggregate is a zero fill of the slot: a memset of a
                    // size sema already computed, needing no aggregate value form.
                    if (std::holds_alternative<ast::DefaultExpr>(*decl.init)) {
                        zero_slot(b, slot, resolved);
                        return;
                    }
                    // Any other aggregate initializer writes through the same coercion-aware
                    // path aggregate assignment uses.
                    const auto source = emit_expr(b, *decl.init);
                    if (source == mir::NO_VALUE) {
                        // emit_expr already reported the construct it could not lower.
                        return;
                    }
                    store_aggregate_value(b, b.slot_addr(slot), resolved, source, type);
                    return;
                }
                const auto value = emit_expr(b, *decl.init);
                if (value != mir::NO_VALUE) {
                    b.store(b.slot_addr(slot), coerce(b, value, resolved, type));
                }
            }

            void zero_slot(mir::Builder &b, const uint32_t slot, const sema::ResolvedType &type) {
                if (is_scalar(type)) {
                    const auto ty = scalar_type(type);
                    mir::ValueId zero;
                    if (ty == mir::Ty::Ptr) zero = b.const_null();
                    else if (mir::is_float(ty)) zero = b.const_float(ty, 0.0);
                    else zero = b.const_int(ty, 0);
                    b.store(b.slot_addr(slot), zero);
                    return;
                }
                b.mem_set(b.slot_addr(slot), b.const_int(mir::Ty::I8, 0),
                           b.const_int(usize_ty(), std::max(1u, size_of(type))));
            }

            // Central writer for aggregate destinations, honoring the representation
            // changes 'assignable' permits between arrays and slices; a same-representation
            // source is one byte copy. Every aggregate store (assignment, initialization,
            // return slots) funnels through here so the coercion set cannot drift per site.
            void store_aggregate_value(mir::Builder &b, const mir::ValueId dest, const sema::ResolvedType &target,
                                        const mir::ValueId source, const sema::ResolvedType &source_type) {
                const auto usize = usize_ty();

                // slice -> array: copy min(len, count) elements, zero-fill the tail (an
                // empty slice clears the array) -- codegen's emit_slice_to_array, verbatim.
                if (target.kind == sema::TypeKind::Array && source_type.kind == sema::TypeKind::Slice) {
                    const auto *info = sema_.array_at(target.array_index);
                    if (!info) return;
                    const auto data = b.load(mir::Ty::Ptr, source);
                    const auto len = b.load(usize, b.ptr_add_const(source, pointer_bytes()));
                    const auto count = b.const_int(usize, static_cast<int64_t>(info->count));
                    const auto n = b.select(b.compare(mir::Op::ICmpUlt, len, count), len, count, usize);
                    const auto stride = std::max(1u, size_of(info->element_type));
                    const auto bytes = stride == 1
                        ? n : b.binary(mir::Op::Mul, usize, n, b.const_int(usize, stride));
                    const auto total = b.const_int(usize, size_of(target));
                    b.mem_copy(dest, data, bytes);
                    b.mem_set(b.ptr_add(dest, bytes), b.const_int(mir::Ty::I8, 0),
                               b.binary(mir::Op::Sub, usize, total, bytes));
                    return;
                }

                // array -> slice: a (data, len) header over the array's own storage.
                if (target.kind == sema::TypeKind::Slice && source_type.kind == sema::TypeKind::Array) {
                    const auto *info = sema_.array_at(source_type.array_index);
                    if (!info) return;
                    b.store(dest, source);
                    b.store(b.ptr_add_const(dest, pointer_bytes()),
                             b.const_int(usize, static_cast<int64_t>(info->count)));
                    return;
                }

                b.mem_copy(dest, source, b.const_int(usize, std::max(1u, size_of(target))));
            }

            // 'const a, b := f()' / 'try a, b := f()' -- the callee's sret blob is
            // destructured into fresh locals, one copy per bound name, at the layout
            // offsets. '_' slots are simply not bound; the blob still holds them. The 'try'
            // form first branches on the trailing error slot and propagates it, then binds
            // only the value slots.
            void emit_var_decl_group(mir::Builder &b, const ast::VarDeclGroupStmt &decl) {
                const auto *call = std::get_if<std::unique_ptr<ast::CallExpr>>(&decl.init);
                const ast::TryExpr *tr = nullptr;
                if (!call) {
                    if (const auto *t = std::get_if<std::unique_ptr<ast::TryExpr>>(&decl.init)) {
                        tr = t->get();
                        call = std::get_if<std::unique_ptr<ast::CallExpr>>(&tr->call);
                    }
                }
                if (!call) {
                    unsupported("a group declaration of this form", decl.location);
                    return;
                }
                if (exprs_ && exprs_->call_dropped_optional_error.contains(call->get())) {
                    unsupported("a call dropping an ignorable error", decl.location);
                    return;
                }
                const auto callee_returns = callee_return_types(**call);
                const size_t bound = tr ? decl.names.size() + 1 : decl.names.size();
                if (!callee_returns || callee_returns->size() != bound) {
                    unsupported("a group declaration", decl.location);
                    return;
                }

                const auto blob = emit_expr(b, tr ? tr->call : decl.init);
                if (blob == mir::NO_VALUE) return;

                const auto layout = multi_return_layout(*callee_returns);
                if (tr) {
                    if (!is_error_union(callee_returns->back())) {
                        unsupported("'try' on this callee", decl.location);
                        return;
                    }
                    const auto err_addr = layout.offsets.back() == 0
                        ? blob : b.ptr_add_const(blob, layout.offsets.back());
                    if (!emit_try_propagation(b, err_addr, callee_returns->back(), decl.location)) {
                        return;
                    }
                }
                for (size_t i = 0; i < decl.names.size(); ++i) {
                    const auto &name = decl.names[i];
                    if (name.empty() || name == "_") continue;
                    const auto &type = (*callee_returns)[i];
                    const auto slot = b.add_slot(std::max(1u, size_of(type)),
                                                  std::max(1u, align_of(type)), name);
                    const auto source = layout.offsets[i] == 0
                        ? blob : b.ptr_add_const(blob, layout.offsets[i]);
                    if (is_scalar(type)) {
                        b.store(b.slot_addr(slot), b.load(scalar_type(type), source));
                    } else {
                        b.mem_copy(b.slot_addr(slot), source, b.const_int(usize_ty(), size_of(type)));
                    }
                    locals_[name] = slot;
                    local_types_[name] = type;
                }
            }

            // sema recorded the initializer's type; a declaration with an explicit type but
            // no initializer has none, in which case fall back to whatever sema resolved for
            // the local. Kept in one place so the "which type is this local" question has one
            // answer.
            [[nodiscard]] auto local_declared_type(const ast::VarDeclStmt &decl,
                                                    const sema::ResolvedType &init_type) const -> sema::ResolvedType {
                if (init_type.kind != sema::TypeKind::Invalid && init_type.kind != sema::TypeKind::Void) {
                    return init_type;
                }
                return sema::ResolvedType{.kind = sema::TypeKind::I64};
            }

            void emit_return(mir::Builder &b, const ast::ReturnStmt &stmt, const std::vector<sema::ResolvedType> &returns) {
                // Return values are evaluated (and, for sret shapes, written) BEFORE the
                // defers run; only the 'ret' itself comes after -- codegen's order.
                if (stmt.return_values.empty()) {
                    emit_defers_for_return(b, returns);
                    b.ret();
                    return;
                }

                // Multi-return: every slot is written into the caller-owned sret blob at its
                // layout offset and the function returns void.
                if (returns.size() > 1) {
                    if (sret_ == mir::NO_VALUE || stmt.return_values.size() > returns.size()) {
                        unsupported("a multi-return 'return'", stmt.location);
                        b.unreachable();
                        return;
                    }
                    const auto layout = multi_return_layout(returns);

                    // 'return f()' forwards the callee's whole return list. Only the
                    // exact-match forward is one blob copy; a forward whose individual slots
                    // coerce (array->slice, subset error unions) rebuilds slot by slot,
                    // which is reported until that lowering exists. A dropped trailing
                    // '?error(...)' needs the runtime unhandled-error check, which does not
                    // exist in MIR yet either.
                    if (stmt.return_values.size() == 1) {
                        const auto *call = std::get_if<std::unique_ptr<ast::CallExpr>>(&stmt.return_values.front());
                        if (!call) {
                            unsupported("a multi-return 'return'", stmt.location);
                            b.unreachable();
                            return;
                        }
                        const auto callee_returns = callee_return_types(**call);
                        if (!callee_returns || *callee_returns != returns) {
                            unsupported("a forwarded multi-return with slot coercions", (*call)->location);
                            b.unreachable();
                            return;
                        }
                        const auto blob = emit_expr(b, stmt.return_values.front());
                        if (blob == mir::NO_VALUE) {
                            b.unreachable();
                            return;
                        }
                        b.mem_copy(sret_, blob, b.const_int(usize_ty(), layout.size));
                        emit_defers_for_return(b, returns);
                        b.ret();
                        return;
                    }

                    for (size_t i = 0; i < returns.size(); ++i) {
                        const auto dest = layout.offsets[i] == 0
                            ? sret_ : b.ptr_add_const(sret_, layout.offsets[i]);
                        if (!store_return_slot(b, dest, returns[i], stmt.return_values[i],
                                                i + 1 == returns.size(), stmt.location)) {
                            b.unreachable();
                            return;
                        }
                    }
                    emit_defers_for_return(b, returns);
                    b.ret();
                    return;
                }

                // An aggregate is copied through the caller's sret pointer and the function
                // returns void; only a scalar is returned by value.
                if (returns_via_sret(returns)) {
                    if (sret_ == mir::NO_VALUE) {
                        unsupported("returning an aggregate from this function", stmt.location);
                        b.unreachable();
                        return;
                    }
                    if (!store_return_slot(b, sret_, returns.front(), stmt.return_values.front(),
                                            true, stmt.location)) {
                        b.unreachable();
                        return;
                    }
                    emit_defers_for_return(b, returns);
                    b.ret();
                    return;
                }

                const auto value = emit_expr(b, stmt.return_values.front());
                if (value == mir::NO_VALUE) {
                    b.unreachable();
                    return;
                }
                const auto coerced = coerce(b, value, returns.front(), expr_type(stmt.return_values.front()));
                emit_defers_for_return(b, returns);
                b.ret(coerced);
            }

            // One slot of a 'return' list. The LAST slot of a fallible function accepts the
            // error-member sugar ('return n, .NotFound'), whose operand sema typed as the
            // concrete MEMBER type -- that needs Ok/Failed wrapping, not a plain store.
            auto store_return_slot(mir::Builder &b, const mir::ValueId dest, const sema::ResolvedType &slot_type,
                                    const ast::Expr &value, const bool is_last, const SourceLocation &loc) -> bool {
                if (is_last && is_error_union(slot_type)) {
                    return emit_error_value_into(b, dest, slot_type, value, loc);
                }
                const auto emitted = emit_expr(b, value);
                if (emitted == mir::NO_VALUE) return false;
                if (is_scalar(slot_type)) {
                    b.store(dest, coerce(b, emitted, slot_type, expr_type(value)));
                } else {
                    store_aggregate_value(b, dest, slot_type, emitted, expr_type(value));
                }
                return true;
            }

            // 'error(...)' is a tagged union laid out as a byte blob: a u32 tag at offset 0
            // (0 = Ok, non-zero = a Failed variant) with the payload at
            // UnionInfo::payload_offset. Both halves below build it in the caller's sret
            // slot, since an error union is an aggregate and travels that way.

            // 'return_ok' / 'return_ok a, b'. Ok is tag 0 and carries no payload, so zeroing
            // the blob IS the Ok value; the leading value slots are then written over it at
            // their layout offsets.
            void emit_return_ok(mir::Builder &b, const ast::ReturnOkStmt &stmt,
                                 const std::vector<sema::ResolvedType> &returns) {
                if (returns.empty()) {
                    unsupported("'return_ok' in a function with no error return", stmt.location);
                    b.unreachable();
                    return;
                }
                if (sret_ == mir::NO_VALUE) {
                    unsupported("'return_ok' from this function", stmt.location);
                    b.unreachable();
                    return;
                }
                const auto layout = multi_return_layout(returns);
                b.mem_set(sret_, b.const_int(mir::Ty::I8, 0), b.const_int(usize_ty(), layout.size));
                for (size_t i = 0; i < stmt.return_values.size() && i + 1 < returns.size(); ++i) {
                    const auto dest = layout.offsets[i] == 0
                        ? sret_ : b.ptr_add_const(sret_, layout.offsets[i]);
                    if (!store_return_slot(b, dest, returns[i], stmt.return_values[i],
                                            false, stmt.location)) {
                        b.unreachable();
                        return;
                    }
                }
                emit_defers_for_return(b, returns);
                b.ret();
            }

            // 'return_err .Variant'. The error goes in the LAST slot; leading value slots
            // are zeroed -- codegen leaves them undef, but a deterministic blob costs one
            // memset and keeps '--emit-mir' output stable.
            void emit_return_err(mir::Builder &b, const ast::ReturnErrStmt &stmt,
                                  const std::vector<sema::ResolvedType> &returns) {
                if (returns.empty() || sret_ == mir::NO_VALUE) {
                    unsupported("'return_err' from this function", stmt.location);
                    b.unreachable();
                    return;
                }
                const auto layout = multi_return_layout(returns);
                if (layout.offsets.back() > 0) {
                    b.mem_set(sret_, b.const_int(mir::Ty::I8, 0),
                               b.const_int(usize_ty(), layout.offsets.back()));
                }
                const auto dest = layout.offsets.back() == 0
                    ? sret_ : b.ptr_add_const(sret_, layout.offsets.back());
                if (!emit_error_value_into(b, dest, returns.back(), stmt.error_value, stmt.location)) {
                    b.unreachable();
                    return;
                }
                emit_defers_for_return(b, returns);
                b.ret();
            }

            // The propagate-or-continue skeleton every 'try' form shares (codegen's
            // emit_try_propagation): branch on the callee error's tag; on failure write the
            // error into the caller's own LAST return slot -- zeroing the value slots for
            // determinism, where codegen leaves them undef -- and return; on success fall
            // through with the insert point in the ok block. False means the propagation
            // itself cannot be lowered (already reported).
            auto emit_try_propagation(mir::Builder &b, const mir::ValueId err_addr,
                                       const sema::ResolvedType &callee_error, const SourceLocation &loc) -> bool {
                if (!current_returns_ || current_returns_->empty() || sret_ == mir::NO_VALUE ||
                    !is_error_union(current_returns_->back())) {
                    unsupported("'try' in this function", loc);
                    return false;
                }
                const auto &caller_error = current_returns_->back();
                const auto *callee_info = callee_error.kind == sema::TypeKind::Union
                    ? sema_.union_at(callee_error.union_index) : nullptr;
                const auto *caller_info = sema_.union_at(caller_error.union_index);
                if (!callee_info || !caller_info) {
                    unsupported("'try' on this callee", loc);
                    return false;
                }
                const auto propagate = b.create_block("try.propagate");
                const auto ok = b.create_block("try.ok");
                const auto tag = b.load(mir::Ty::I32, err_addr);
                b.branch(b.compare(mir::Op::ICmpNe, tag, b.const_int(mir::Ty::I32, 0)), propagate, ok);

                b.set_insert_point(propagate);
                const auto layout = multi_return_layout(*current_returns_);
                if (layout.offsets.back() > 0) {
                    b.mem_set(sret_, b.const_int(mir::Ty::I8, 0),
                               b.const_int(usize_ty(), layout.offsets.back()));
                }
                const auto dest = layout.offsets.back() == 0
                    ? sret_ : b.ptr_add_const(sret_, layout.offsets.back());
                if (callee_info->error_member_types == caller_info->error_member_types) {
                    // Identical member lists are byte-identical blobs -- one copy.
                    b.mem_copy(dest, err_addr, b.const_int(usize_ty(), size_of(caller_error)));
                } else if (!emit_error_retag(b, dest, caller_error, err_addr, callee_error, loc)) {
                    b.unreachable();
                    b.set_insert_point(ok);
                    return false;
                }
                emit_defers_for_return(b, *current_returns_);
                b.ret();

                b.set_insert_point(ok);
                return true;
            }

            // Re-tags a FAILED error value of 'callee_error' as one of 'caller_error',
            // where the callee's member list is a strict subset of the caller's (sema's
            // error_union_is_subset). Payload bytes for a given member are identical
            // between any two error unions carrying it -- both wrap it via the one-field
            // struct convention -- so only the dispatch TAGS differ (codegen's
            // translate_error_value): a single-member callee re-tags at compile time, a
            // multi-member callee switches on its inner tag at runtime.
            auto emit_error_retag(mir::Builder &b, const mir::ValueId dest, const sema::ResolvedType &caller_error,
                                   const mir::ValueId src, const sema::ResolvedType &callee_error,
                                   const SourceLocation &loc) -> bool {
                const auto *callee_info = sema_.union_at(callee_error.union_index);
                const auto *caller_info = sema_.union_at(caller_error.union_index);
                if (!callee_info || !caller_info ||
                    callee_info->variants.size() < 2 || caller_info->variants.size() < 2 ||
                    caller_info->error_member_types.size() < 2) {
                    // A single-member caller cannot be a strict superset of anything.
                    unsupported("an error propagation between different 'error(...)' types", loc);
                    return false;
                }

                b.mem_set(dest, b.const_int(mir::Ty::I8, 0), b.const_int(usize_ty(), size_of(caller_error)));
                const auto &caller_failed = caller_info->variants[1];
                b.store(dest, b.const_int(mir::Ty::I32, caller_failed.tag_value));

                const auto &callee_failed = callee_info->variants[1];
                const auto src_payload_at = variant_payload_offset(*callee_info, callee_failed);
                const auto dest_payload_at = variant_payload_offset(*caller_info, caller_failed);
                const auto *caller_inner = caller_failed.payload_type.kind == sema::TypeKind::Union
                    ? sema_.union_at(caller_failed.payload_type.union_index) : nullptr;
                if (!caller_inner) {
                    unsupported("an error propagation between different 'error(...)' types", loc);
                    return false;
                }
                const auto dest_inner_tag_addr = dest_payload_at == 0
                    ? dest : b.ptr_add_const(dest, dest_payload_at);

                // Writes one member: the caller-side inner tag, then the member bytes.
                const auto write_member = [&](const sema::ResolvedType &member_type,
                                               const uint32_t src_member_at) -> bool {
                    const auto found = std::ranges::find(caller_inner->variants, member_type,
                                                          &sema::TaggedUnionVariant::payload_type);
                    if (found == caller_inner->variants.end()) return false;
                    b.store(dest_inner_tag_addr, b.const_int(mir::Ty::I32, found->tag_value));
                    const auto dest_member_at = dest_payload_at + caller_inner->payload_offset +
                                                 wrapper_field_offset(found->payload_struct_index);
                    b.mem_copy(b.ptr_add_const(dest, dest_member_at),
                                src_member_at == 0 ? src : b.ptr_add_const(src, src_member_at),
                                b.const_int(usize_ty(), std::max(1u, size_of(member_type))));
                    return true;
                };

                if (callee_info->error_member_types.size() == 1) {
                    // The member type is fixed at compile time; no runtime dispatch.
                    if (!write_member(callee_failed.payload_type, src_payload_at)) {
                        unsupported("an error propagation between different 'error(...)' types", loc);
                        return false;
                    }
                    return true;
                }

                // Multi-member callee: dispatch on its inner tag, one case per POSSIBLE
                // member (a compile-time-known list).
                const auto *callee_inner = callee_failed.payload_type.kind == sema::TypeKind::Union
                    ? sema_.union_at(callee_failed.payload_type.union_index) : nullptr;
                if (!callee_inner) {
                    unsupported("an error propagation between different 'error(...)' types", loc);
                    return false;
                }
                const auto src_tag = b.load(mir::Ty::I32, src_payload_at == 0
                    ? src : b.ptr_add_const(src, src_payload_at));

                const auto merge = b.create_block("retag.merge");
                // sema fixed the possible tags; anything else cannot occur at runtime.
                const auto dead = b.create_block("retag.unreachable");
                std::vector<std::pair<int64_t, mir::BlockId>> cases;
                std::vector<std::pair<mir::BlockId, const sema::TaggedUnionVariant *>> arms;
                for (const auto &variant : callee_inner->variants) {
                    const auto block = b.create_block(std::format("retag.{}", variant.name));
                    cases.emplace_back(variant.tag_value, block);
                    arms.emplace_back(block, &variant);
                }
                b.switch_on(src_tag, dead, cases);
                b.set_insert_point(dead);
                b.unreachable();

                for (const auto &[block, variant] : arms) {
                    b.set_insert_point(block);
                    const auto src_member_at = src_payload_at + callee_inner->payload_offset +
                                                wrapper_field_offset(variant->payload_struct_index);
                    if (!write_member(variant->payload_type, src_member_at)) {
                        unsupported("an error propagation between different 'error(...)' types", loc);
                        return false;
                    }
                    b.jump(merge);
                }
                b.set_insert_point(merge);
                return true;
            }

            // 'try f()' in expression position: call, propagate on failure, and on the ok
            // path yield the surviving value -- nothing for a bare 'error(...)' callee
            // (statement position), the first slot otherwise.
            auto emit_try(mir::Builder &b, const ast::TryExpr &tr, const SourceLocation &loc) -> mir::ValueId {
                const auto *call = std::get_if<std::unique_ptr<ast::CallExpr>>(&tr.call);
                if (!call) {
                    unsupported("'try' on this operand", loc);
                    return mir::NO_VALUE;
                }
                const auto callee_returns = callee_return_types(**call);
                if (!callee_returns || callee_returns->empty() || !is_error_union(callee_returns->back())) {
                    unsupported("'try' on this callee", loc);
                    return mir::NO_VALUE;
                }

                const auto blob = emit_expr(b, tr.call);
                if (blob == mir::NO_VALUE) return mir::NO_VALUE;

                const auto layout = multi_return_layout(*callee_returns);
                const auto err_addr = layout.offsets.back() == 0
                    ? blob : b.ptr_add_const(blob, layout.offsets.back());
                if (!emit_try_propagation(b, err_addr, callee_returns->back(), loc)) {
                    return mir::NO_VALUE;
                }

                if (callee_returns->size() == 1) {
                    // Statement position; the value is never consumed, but NO_VALUE means
                    // "failed and reported", so a throwaway constant stands in.
                    return b.const_int(mir::Ty::I8, 0);
                }
                const auto &value_type = callee_returns->front();
                return is_scalar(value_type) ? b.load(scalar_type(value_type), blob) : blob;
            }

            // The one-field '{v: T}' wrapper struct convention puts the wrapped value at its
            // single field's sema offset (0 in practice, but read rather than assumed).
            [[nodiscard]] auto wrapper_field_offset(const int struct_index) const -> uint32_t {
                const auto *info = sema_.struct_at(struct_index);
                return info && !info->fields.empty() ? info->fields.front().offset : 0;
            }

            // Writes an 'error(...)' value into 'dest' (the blob's base address). Zeroes the
            // blob first, so Ok-state padding and short payloads are deterministic. Operand
            // shapes, per sema's resolve_return_err_member_type:
            //   - a value already of this union type (or a '?error' twin with the same
            //     member list, which is byte-identical): one blob copy
            //   - a concrete error MEMBER value: Failed tag, then the member at the payload
            //     offset -- through the inner dispatch union's tag when there are 2+ members
            //   - a SUBSET error union, which would need runtime tag translation: reported
            auto emit_error_value_into(mir::Builder &b, const mir::ValueId dest, const sema::ResolvedType &error_type,
                                        const ast::Expr &operand, const SourceLocation &loc) -> bool {
                const auto *info = error_type.kind == sema::TypeKind::Union
                    ? sema_.union_at(error_type.union_index) : nullptr;
                if (!info || !info->is_error_union || info->variants.size() < 2) {
                    unsupported("an error return of this type", loc);
                    return false;
                }

                b.mem_set(dest, b.const_int(mir::Ty::I8, 0), b.const_int(usize_ty(), size_of(error_type)));

                const auto operand_type = expr_type(operand);
                if (operand_type.kind == sema::TypeKind::Union) {
                    const auto *operand_info = sema_.union_at(operand_type.union_index);
                    if (operand_info && operand_info->is_error_union) {
                        const auto value = emit_expr(b, operand);
                        if (value == mir::NO_VALUE) return false;
                        // Same member list means byte-identical representation ('error(E)'
                        // vs '?error(E)' included) -- see codegen's
                        // error_unions_interchangeable for why this is load-bearing. A
                        // subset union re-tags instead.
                        if (operand_info->error_member_types == info->error_member_types) {
                            b.mem_copy(dest, value, b.const_int(usize_ty(), size_of(operand_type)));
                            return true;
                        }
                        return emit_error_retag(b, dest, error_type, value, operand_type, loc);
                    }
                }

                // A concrete member. 'Failed' is always variants[1] by construction
                // (synthesize_error_union in type_resolver.cpp).
                const auto &failed = info->variants[1];
                b.store(dest, b.const_int(mir::Ty::I32, failed.tag_value));

                auto at = info->payload_offset + wrapper_field_offset(failed.payload_struct_index);
                if (info->error_member_types.size() > 1) {
                    // 2+ members: the Failed payload is an inner dispatch union; write ITS
                    // tag for this member, then aim at the inner payload.
                    const auto *inner = failed.payload_type.kind == sema::TypeKind::Union
                        ? sema_.union_at(failed.payload_type.union_index) : nullptr;
                    const sema::TaggedUnionVariant *member_variant = nullptr;
                    if (inner) {
                        for (const auto &variant : inner->variants) {
                            if (variant.payload_type == operand_type) {
                                member_variant = &variant;
                                break;
                            }
                        }
                    }
                    if (!member_variant) {
                        unsupported("an error return of this member type", loc);
                        return false;
                    }
                    b.store(at == 0 ? dest : b.ptr_add_const(dest, at),
                             b.const_int(mir::Ty::I32, member_variant->tag_value));
                    at += inner->payload_offset + wrapper_field_offset(member_variant->payload_struct_index);
                }

                const auto value = emit_expr(b, operand);
                if (value == mir::NO_VALUE) return false;
                const auto payload = at == 0 ? dest : b.ptr_add_const(dest, at);
                if (is_scalar(operand_type)) {
                    b.store(payload, value);
                } else {
                    b.mem_copy(payload, value, b.const_int(usize_ty(), size_of(operand_type)));
                }
                return true;
            }

            // The dispatch skeleton 'match' and 'switch' share, mirroring codegen's
            // emit_arm_dispatch: operand typing with the transparent error-value unwrap, the
            // union/enum/scalar classification, the MIR switch terminator with per-family
            // case values, and per-arm payload-capture binding. 'emit_arm' runs with the
            // insert point inside the arm's block, locals freshly restored, and any capture
            // already bound. Returns false (with the construct reported) when something in
            // the dispatch itself cannot be lowered.
            template <typename ArmT>
            auto emit_arm_dispatch(mir::Builder &b, const ast::Expr &operand_expr, const std::vector<ArmT> &arms,
                                    const std::string_view noun, const mir::BlockId default_block,
                                    const ArmT *default_arm, const SourceLocation &loc,
                                    const std::function<void(const ArmT &)> &emit_arm) -> bool {
                auto operand_type = expr_type(operand_expr);
                mir::ValueId operand_addr = mir::NO_VALUE;

                // Transparent error matching: sema recorded that the arms dispatch against
                // the wrapper's Failed payload, not the wrapper. In memory form the unwrap
                // is pure address arithmetic into the wrapper blob.
                if (exprs_) {
                    if (const auto it = exprs_->expr_error_match_unwrap.find(sema::get_expr_key(operand_expr));
                        it != exprs_->expr_error_match_unwrap.end()) {
                        const auto *wrapper = it->second.wrapper_type.kind == sema::TypeKind::Union
                            ? sema_.union_at(it->second.wrapper_type.union_index) : nullptr;
                        const auto wrapper_addr = emit_expr(b, operand_expr);
                        if (!wrapper || wrapper->variants.size() < 2 || wrapper_addr == mir::NO_VALUE) {
                            unsupported(std::format("a '{}' on this error operand", noun), loc);
                            return false;
                        }
                        operand_type = it->second.effective_type;
                        const auto at = variant_payload_offset(*wrapper, wrapper->variants[1]);
                        operand_addr = at == 0 ? wrapper_addr : b.ptr_add_const(wrapper_addr, at);
                    }
                }

                mir::ValueId scrutinee = mir::NO_VALUE;
                mir::ValueId union_base = mir::NO_VALUE;
                const sema::UnionInfo *union_info = nullptr;

                if (operand_type.kind == sema::TypeKind::Union) {
                    union_info = sema_.union_at(operand_type.union_index);
                    if (!union_info || !union_info->is_tagged) {
                        unsupported(std::format("a '{}' on this operand", noun), loc);
                        return false;
                    }
                    if (operand_addr == mir::NO_VALUE) {
                        operand_addr = emit_expr(b, operand_expr);
                        if (operand_addr == mir::NO_VALUE) return false;
                    }
                    // The operand is copied into a fresh slot and captures -- by-ref
                    // included -- bind into the COPY, matching codegen: mutating a by-ref
                    // capture must not write through to the original value.
                    const auto size = std::max(1u, size_of(operand_type));
                    const auto slot = b.add_slot(size, std::max(1u, align_of(operand_type)),
                                                  std::format("{}.union", noun));
                    union_base = b.slot_addr(slot);
                    b.mem_copy(union_base, operand_addr, b.const_int(usize_ty(), size));
                    // A by-ref capture stores this slot's address; promote_slots must keep
                    // its hands off it either way.
                    slots_escaping_.insert(slot);
                    scrutinee = b.load(mir::Ty::I32, union_base);
                } else if (operand_addr != mir::NO_VALUE) {
                    // Unwrapped single-member error: the payload IS the scalar member.
                    if (!is_scalar(operand_type)) {
                        unsupported(std::format("a '{}' on this error operand", noun), loc);
                        return false;
                    }
                    scrutinee = b.load(scalar_type(operand_type), operand_addr);
                } else {
                    scrutinee = emit_expr(b, operand_expr);
                    if (scrutinee == mir::NO_VALUE) return false;
                }

                // Arms are laid out first so every case can name its block before the
                // terminator that references them is emitted.
                struct PendingArm {
                    mir::BlockId block;
                    const ArmT *arm;
                    const ast::MatchExpr::VariantPattern *capture_pattern = nullptr;
                    const sema::TaggedUnionVariant *variant = nullptr;
                };
                std::vector<std::pair<int64_t, mir::BlockId>> cases;
                std::vector<PendingArm> pending;

                for (size_t i = 0; i < arms.size(); ++i) {
                    const auto &arm = arms[i];
                    if (std::holds_alternative<ast::MatchExpr::DefaultPattern>(arm.pattern)) continue;

                    if (union_info) {
                        const auto *vp = std::get_if<ast::MatchExpr::VariantPattern>(&arm.pattern);
                        const sema::TaggedUnionVariant *variant = nullptr;
                        if (vp) {
                            const auto found = std::ranges::find(union_info->variants, vp->name,
                                                                  &sema::TaggedUnionVariant::name);
                            if (found != union_info->variants.end()) variant = &*found;
                        }
                        if (!vp || !variant) {
                            unsupported(std::format("a '{}' arm pattern of this kind", noun), arm.location);
                            return false;
                        }
                        const auto block = b.create_block(std::format("{}.arm.{}", noun, vp->name));
                        cases.emplace_back(variant->tag_value, block);
                        pending.push_back({block, &arm, vp, variant});
                        continue;
                    }

                    const auto block = b.create_block(std::format("{}.arm{}", noun, i));
                    if (const auto *vp = std::get_if<ast::MatchExpr::VariantPattern>(&arm.pattern)) {
                        // '.Variant' on an enum operand is its declared value.
                        bool found = false;
                        if (operand_type.kind == sema::TypeKind::Enum) {
                            if (const auto *info = sema_.enum_at(operand_type.enum_index)) {
                                for (const auto &field : info->fields) {
                                    if (field.name == vp->name) {
                                        cases.emplace_back(field.value, block);
                                        found = true;
                                        break;
                                    }
                                }
                            }
                        }
                        if (!found) {
                            unsupported(std::format("a '{}' arm pattern of this kind", noun), arm.location);
                            return false;
                        }
                    } else if (const auto *lp = std::get_if<ast::MatchExpr::LiteralPattern>(&arm.pattern)) {
                        // A literal pattern: sema guaranteed it is a compile-time constant
                        // of the operand's type.
                        if (const auto value = constant_int(*lp->expr)) {
                            cases.emplace_back(*value, block);
                        } else {
                            unsupported(std::format("a non-constant '{}' arm pattern", noun), arm.location);
                            return false;
                        }
                    }
                    pending.push_back({block, &arm});
                }

                b.switch_on(scrutinee, default_block, cases);

                // Captures are arm-scoped: each arm starts from the locals as they were at
                // the dispatch, never from a sibling arm's bindings.
                const auto saved_locals = locals_;
                const auto saved_types = local_types_;
                for (const auto &p : pending) {
                    b.set_insert_point(p.block);
                    locals_ = saved_locals;
                    local_types_ = saved_types;
                    if (p.capture_pattern && p.capture_pattern->capture_name && p.variant &&
                        p.variant->payload_struct_index >= 0) {
                        bind_variant_capture(b, union_base, *union_info, *p.variant, *p.capture_pattern);
                    }
                    emit_arm(*p.arm);
                }
                if (default_arm) {
                    b.set_insert_point(default_block);
                    locals_ = saved_locals;
                    local_types_ = saved_types;
                    emit_arm(*default_arm);
                }
                locals_ = saved_locals;
                local_types_ = saved_types;
                return true;
            }

            // A '.Variant(v)' / '.Variant(&v)' payload capture, bound for one arm's body.
            // By value: a fresh local holding a copy of the payload. By reference: a
            // pointer local aimed INTO the dispatch's union copy.
            void bind_variant_capture(mir::Builder &b, const mir::ValueId union_base, const sema::UnionInfo &info,
                                       const sema::TaggedUnionVariant &variant,
                                       const ast::MatchExpr::VariantPattern &vp) {
                const auto &payload_type = variant.payload_type;
                const auto at = variant_payload_offset(info, variant);
                const auto payload_addr = at == 0 ? union_base : b.ptr_add_const(union_base, at);

                if (vp.capture_by_ref) {
                    const auto slot = b.add_slot(pointer_bytes(), pointer_bytes(), *vp.capture_name);
                    b.store(b.slot_addr(slot), payload_addr);
                    locals_[*vp.capture_name] = slot;
                    local_types_[*vp.capture_name] = pointer_type_to(payload_type);
                    return;
                }
                const auto slot = b.add_slot(std::max(1u, size_of(payload_type)),
                                              std::max(1u, align_of(payload_type)), *vp.capture_name);
                if (is_scalar(payload_type)) {
                    b.store(b.slot_addr(slot), b.load(scalar_type(payload_type), payload_addr));
                } else {
                    b.mem_copy(b.slot_addr(slot), payload_addr,
                                b.const_int(usize_ty(), size_of(payload_type)));
                }
                locals_[*vp.capture_name] = slot;
                local_types_[*vp.capture_name] = payload_type;
            }

            // 'switch' -- a statement: arms do not fall through, an unmatched value falls
            // out of the switch, and a '_' arm catches it when present.
            void emit_switch(mir::Builder &b, const ast::SwitchStmt &stmt,
                              const std::vector<sema::ResolvedType> &returns) {
                const auto end_block = b.create_block("switch.end");

                const ast::SwitchStmt::Arm *default_arm = nullptr;
                for (const auto &arm : stmt.arms) {
                    if (std::holds_alternative<ast::MatchExpr::DefaultPattern>(arm.pattern)) {
                        default_arm = &arm;
                        break;
                    }
                }
                const auto default_block = default_arm ? b.create_block("switch.default") : end_block;

                emit_arm_dispatch<ast::SwitchStmt::Arm>(b, stmt.operand, stmt.arms, "switch",
                    default_block, default_arm, stmt.location,
                    [&](const ast::SwitchStmt::Arm &arm) {
                        emit_stmt(b, arm.body, returns);
                        if (!b.block_is_terminated()) b.jump(end_block);
                    });

                b.set_insert_point(end_block);
            }

            // 'match' -- an expression: every arm yields a value of the match's own type.
            // A scalar result merges through a block parameter (the same mechanism the
            // short-circuit operators use); an aggregate result is copied into one result
            // slot per arm, whose address is the match's value.
            auto emit_match(mir::Builder &b, const ast::MatchExpr &match, const ast::Expr &expr) -> mir::ValueId {
                const auto result_type = expr_type(expr);
                const bool has_value = result_type.kind != sema::TypeKind::Void &&
                                        result_type.kind != sema::TypeKind::Invalid;
                const bool scalar_result = has_value && is_scalar(result_type);

                const auto end_block = b.create_block("match.end");
                mir::ValueId result_param = mir::NO_VALUE;
                mir::ValueId result_slot = mir::NO_VALUE;
                if (scalar_result) {
                    result_param = b.add_block_param(end_block, scalar_type(result_type));
                } else if (has_value) {
                    const auto slot = b.add_slot(std::max(1u, size_of(result_type)),
                                                  std::max(1u, align_of(result_type)), "match.result");
                    result_slot = b.slot_addr(slot);
                }

                const ast::MatchExpr::Arm *default_arm = nullptr;
                for (const auto &arm : match.arms) {
                    if (std::holds_alternative<ast::MatchExpr::DefaultPattern>(arm.pattern)) {
                        default_arm = &arm;
                        break;
                    }
                }
                // sema guarantees exhaustiveness, so the no-default sentinel is never
                // entered at runtime.
                const auto default_block = b.create_block(default_arm ? "match.default" : "match.unreachable");

                bool arms_ok = true;
                const bool ok = emit_arm_dispatch<ast::MatchExpr::Arm>(b, match.operand, match.arms, "match",
                    default_block, default_arm, match.location,
                    [&](const ast::MatchExpr::Arm &arm) {
                        const auto value = emit_expr(b, arm.value);
                        if (value == mir::NO_VALUE) {
                            arms_ok = false;
                            b.unreachable();
                            return;
                        }
                        if (b.block_is_terminated()) return;
                        if (scalar_result) {
                            b.jump(end_block, {coerce(b, value, result_type, expr_type(arm.value))});
                        } else {
                            if (has_value) {
                                b.mem_copy(result_slot, value, b.const_int(usize_ty(), size_of(result_type)));
                            }
                            b.jump(end_block);
                        }
                    });

                if (!default_arm) {
                    b.set_insert_point(default_block);
                    b.unreachable();
                }

                b.set_insert_point(end_block);
                if (!ok || !arms_ok) return mir::NO_VALUE;
                if (scalar_result) return result_param;
                if (has_value) return result_slot;
                // A void match in statement position: there is nothing to yield, but
                // NO_VALUE means "failed and reported" to every caller, so a throwaway
                // constant stands in -- the analogue of codegen's UndefValue.
                return b.const_int(mir::Ty::I32, 0);
            }

            // A compile-time integer constant, for switch/match arm patterns. Only the
            // shapes a pattern can actually be: an integer or character literal, a bool, or
            // an enum variant reference.
            [[nodiscard]] auto constant_int(const ast::Expr &expr) const -> std::optional<int64_t> {
                if (const auto *i = std::get_if<ast::LiteralIntegerExpr>(&expr)) {
                    return static_cast<int64_t>(i->value);
                }
                if (const auto *c = std::get_if<ast::LiteralCharExpr>(&expr)) {
                    return static_cast<int64_t>(c->value);
                }
                if (const auto *bl = std::get_if<ast::LiteralBoolExpr>(&expr)) {
                    return bl->value ? 1 : 0;
                }
                if (const auto *u = std::get_if<std::unique_ptr<ast::UnaryExpr>>(&expr)) {
                    if ((*u)->op == ast::UnaryOp::Negate) {
                        if (const auto inner = constant_int((*u)->operand)) return -*inner;
                    }
                }
                if (const auto *dot = std::get_if<ast::DotIdentExpr>(&expr)) {
                    const auto type = expr_type(expr);
                    if (type.kind == sema::TypeKind::Enum) {
                        if (const auto *info = sema_.enum_at(type.enum_index)) {
                            for (const auto &field : info->fields) {
                                if (field.name == dot->name) return field.value;
                            }
                        }
                    }
                }
                // Anything else (a const-global reference, arithmetic on constants) folds
                // through the SAME evaluator sema validated the pattern with -- a case label
                // sema never checked must not be invented here. const_cast because the
                // folder may force resolution of a referenced global, a no-op by now
                // (codegen's scalar switch arm does exactly this).
                if (module_path_) {
                    if (const auto value = sema::evaluate_integer_constant(
                            expr, *module_path_, const_cast<sema::Program &>(sema_), diag_)) {
                        return *value;
                    }
                }
                return std::nullopt;
            }

            // 'when' is resolved entirely at compile time: sema folded the condition and
            // recorded which branch was selected, so lowering emits ONLY that branch and no
            // control flow at all.
            //
            // The unselected branch is deliberately not emitted even though sema
            // type-checked it (the "both branches are always checked" rule) -- checking and
            // emitting are different questions, and emitting the dead side is exactly what
            // '#compile_only_if'/'when' exist to avoid.
            void emit_when(mir::Builder &b, const ast::WhenStmt &stmt,
                            const std::vector<sema::ResolvedType> &returns) {
                const auto it = exprs_ ? exprs_->when_stmt_selected.find(&stmt)
                                        : std::unordered_map<const ast::WhenStmt *, bool>::const_iterator{};
                if (!exprs_ || it == exprs_->when_stmt_selected.end()) {
                    // sema records this for every 'when' it checked; a miss means the
                    // statement was never checked, which is a bug rather than a shape mirgen
                    // does not handle.
                    unsupported("a 'when' whose condition sema did not fold", stmt.location);
                    return;
                }

                if (it->second) {
                    for (const auto &s : stmt.then_block.stmts) {
                        emit_stmt(b, s, returns);
                        if (b.block_is_terminated()) break;
                    }
                    return;
                }
                if (!stmt.else_branch) {
                    return;
                }
                std::visit([&]<typename E>(const E &branch) {
                    if constexpr (std::is_same_v<std::decay_t<E>, ast::BlockStmt>) {
                        for (const auto &s : branch.stmts) {
                            emit_stmt(b, s, returns);
                            if (b.block_is_terminated()) break;
                        }
                    } else {
                        // 'else when' — the same fold decides the chain one link down.
                        emit_when(b, *branch, returns);
                    }
                }, *stmt.else_branch);
            }

            void emit_if(mir::Builder &b, const ast::IfStmt &stmt, const std::vector<sema::ResolvedType> &returns) {
                const auto cond = emit_condition(b, stmt.condition);
                if (cond == mir::NO_VALUE) return;

                const auto then_block = b.create_block("if.then");
                const auto else_block = b.create_block(stmt.else_stmt ? "if.else" : "if.end");
                // Without an 'else', the false edge IS the join, so no third block is made.
                const auto end_block = stmt.else_stmt ? b.create_block("if.end") : else_block;

                b.branch(cond, then_block, else_block);

                b.set_insert_point(then_block);
                emit_stmt(b, stmt.then_stmt, returns);
                if (!b.block_is_terminated()) b.jump(end_block);

                if (stmt.else_stmt) {
                    b.set_insert_point(else_block);
                    emit_stmt(b, *stmt.else_stmt, returns);
                    if (!b.block_is_terminated()) b.jump(end_block);
                }

                b.set_insert_point(end_block);
            }

            void emit_while(mir::Builder &b, const ast::WhileStmt &stmt, const std::vector<sema::ResolvedType> &returns) {
                const auto header = b.create_block("while.cond");
                const auto body = b.create_block("while.body");
                const auto exit = b.create_block("while.end");

                b.jump(header);
                b.set_insert_point(header);
                const auto cond = emit_condition(b, stmt.condition);
                if (cond == mir::NO_VALUE) {
                    b.jump(exit);
                } else {
                    b.branch(cond, body, exit);
                }

                b.set_insert_point(body);
                loop_stack_.push_back({header, exit});
                next_block_is_loop_body_ = true;
                emit_stmt(b, stmt.body, returns);
                loop_stack_.pop_back();
                if (!b.block_is_terminated()) b.jump(header);

                b.set_insert_point(exit);
            }

            // 'for elem in xs' / 'for i, elem in xs' over an array or a slice, lowered to a
            // counted loop: an index slot, a bound, and a per-iteration element address.
            //
            // 'continue' jumps to the STEP block rather than the condition, or the index
            // would never advance and the loop would spin -- the classic desugaring bug.
            void emit_for_in(mir::Builder &b, const ast::ForInStmt &stmt,
                              const std::vector<sema::ResolvedType> &returns) {
                if (const auto *range = std::get_if<std::unique_ptr<ast::RangeExpr>>(&stmt.iterable)) {
                    emit_for_in_range(b, stmt, **range, returns);
                    return;
                }
                const auto iterable_type = expr_type(stmt.iterable);
                const auto usize = usize_ty();

                sema::ResolvedType element{};
                mir::ValueId data = mir::NO_VALUE;
                mir::ValueId count = mir::NO_VALUE;

                if (iterable_type.kind == sema::TypeKind::Array) {
                    const auto *info = sema_.array_at(iterable_type.array_index);
                    if (!info) { unsupported("a 'for-in' over this operand", stmt.location); return; }
                    element = info->element_type;
                    data = emit_address(b, stmt.iterable);
                    count = b.const_int(usize, static_cast<int64_t>(info->count));
                } else if (iterable_type.kind == sema::TypeKind::Slice) {
                    const auto *info = sema_.slice_at(iterable_type.slice_index);
                    if (!info) { unsupported("a 'for-in' over this operand", stmt.location); return; }
                    element = info->element_type;
                    // A slice's two words: data pointer, then length.
                    auto slice_addr = emit_address(b, stmt.iterable);
                    if (slice_addr == mir::NO_VALUE) slice_addr = emit_expr(b, stmt.iterable);
                    if (slice_addr == mir::NO_VALUE) { unsupported("a 'for-in' over this operand", stmt.location); return; }
                    data = b.load(mir::Ty::Ptr, slice_addr);
                    count = b.load(usize, b.ptr_add_const(slice_addr, pointer_bytes()));
                } else {
                    unsupported("a 'for-in' over this operand", stmt.location);
                    return;
                }
                if (data == mir::NO_VALUE) { unsupported("a 'for-in' over this operand", stmt.location); return; }

                const auto index_slot = b.add_slot(pointer_bytes(), pointer_bytes(),
                                                    stmt.index_name == "_" ? "for.i" : stmt.index_name);
                b.store(b.slot_addr(index_slot), b.const_int(usize, 0));

                // The element binding is a slot holding either the value or its address, per
                // '&val'. Both are ordinary locals from the body's point of view.
                const auto element_is_ref = stmt.element_by_ref;
                const auto element_type = element_is_ref ? pointer_to(element) : element;
                const auto element_slot = b.add_slot(
                    std::max(1u, element_is_ref ? pointer_bytes() : size_of(element)),
                    std::max(1u, element_is_ref ? pointer_bytes() : align_of(element)),
                    stmt.element_name);

                const auto header = b.create_block("for.cond");
                const auto body = b.create_block("for.body");
                const auto step = b.create_block("for.step");
                const auto exit = b.create_block("for.end");

                b.jump(header);
                b.set_insert_point(header);
                const auto i = b.load(usize, b.slot_addr(index_slot));
                b.branch(b.compare(mir::Op::ICmpUlt, i, count), body, exit);

                b.set_insert_point(body);
                const auto offset = b.binary(mir::Op::Mul, usize, b.load(usize, b.slot_addr(index_slot)),
                                              b.const_int(usize, size_of(element)));
                const auto element_address = b.ptr_add(data, offset);
                if (element_is_ref || !is_scalar(element)) {
                    b.store(b.slot_addr(element_slot), element_address);
                } else {
                    b.store(b.slot_addr(element_slot), b.load(scalar_type(element), element_address));
                }

                const auto saved_locals = locals_;
                const auto saved_types = local_types_;
                if (stmt.index_name != "_") {
                    locals_[stmt.index_name] = index_slot;
                    local_types_[stmt.index_name] = sema::ResolvedType{.kind = sema::TypeKind::USize};
                }
                locals_[stmt.element_name] = element_slot;
                local_types_[stmt.element_name] = element_type;

                loop_stack_.push_back({step, exit});
                next_block_is_loop_body_ = true;
                emit_stmt(b, stmt.body, returns);
                loop_stack_.pop_back();
                locals_ = saved_locals;
                local_types_ = saved_types;

                if (!b.block_is_terminated()) b.jump(step);

                b.set_insert_point(step);
                b.store(b.slot_addr(index_slot),
                         b.binary(mir::Op::Add, usize, b.load(usize, b.slot_addr(index_slot)),
                                   b.const_int(usize, 1)));
                b.jump(header);

                b.set_insert_point(exit);
            }

            // 'for i, x in a..b' -- a counting loop. The counter runs at usize width (sema
            // binds the index as usize; a narrower slot would put stack garbage in a body
            // read's high half), while the ELEMENT carries the range's own value type,
            // narrowed back per iteration. Mirrors codegen's RangeExpr arm exactly.
            void emit_for_in_range(mir::Builder &b, const ast::ForInStmt &stmt, const ast::RangeExpr &range,
                                    const std::vector<sema::ResolvedType> &returns) {
                const auto usize = usize_ty();
                const auto upper_type = expr_type(range.upper);
                const bool is_signed = signed_type(upper_type);
                const auto value_ty = is_scalar(upper_type) ? scalar_type(upper_type) : usize;

                const auto index_slot = b.add_slot(pointer_bytes(), pointer_bytes(),
                                                    stmt.index_name == "_" ? "for.idx" : stmt.index_name);
                mir::ValueId lower = range.lower ? emit_expr(b, *range.lower) : b.const_int(value_ty, 0);
                if (lower == mir::NO_VALUE) return;
                b.store(b.slot_addr(index_slot), coerce_to(b, lower, usize, is_signed));
                const auto upper_raw = emit_expr(b, range.upper);
                if (upper_raw == mir::NO_VALUE) return;
                const auto upper = coerce_to(b, upper_raw, usize, is_signed);

                uint32_t element_slot = UINT32_MAX;
                if (stmt.element_name != "_") {
                    element_slot = b.add_slot(std::max(1u, size_of(upper_type)),
                                               std::max(1u, align_of(upper_type)), stmt.element_name);
                }

                const auto header = b.create_block("for.cond");
                const auto body = b.create_block("for.body");
                const auto step = b.create_block("for.step");
                const auto exit = b.create_block("for.end");

                b.jump(header);
                b.set_insert_point(header);
                const auto i = b.load(usize, b.slot_addr(index_slot));
                b.branch(b.compare(is_signed ? mir::Op::ICmpSlt : mir::Op::ICmpUlt, i, upper), body, exit);

                b.set_insert_point(body);
                if (element_slot != UINT32_MAX) {
                    const auto current = b.load(usize, b.slot_addr(index_slot));
                    b.store(b.slot_addr(element_slot), coerce_to(b, current, value_ty, is_signed));
                }

                const auto saved_locals = locals_;
                const auto saved_types = local_types_;
                if (stmt.index_name != "_") {
                    locals_[stmt.index_name] = index_slot;
                    local_types_[stmt.index_name] = sema::ResolvedType{.kind = sema::TypeKind::USize};
                }
                if (element_slot != UINT32_MAX) {
                    locals_[stmt.element_name] = element_slot;
                    local_types_[stmt.element_name] = upper_type;
                }

                loop_stack_.push_back({step, exit});
                next_block_is_loop_body_ = true;
                emit_stmt(b, stmt.body, returns);
                loop_stack_.pop_back();
                locals_ = saved_locals;
                local_types_ = saved_types;
                if (!b.block_is_terminated()) b.jump(step);

                b.set_insert_point(step);
                b.store(b.slot_addr(index_slot),
                         b.binary(mir::Op::Add, usize, b.load(usize, b.slot_addr(index_slot)),
                                   b.const_int(usize, 1)));
                b.jump(header);

                b.set_insert_point(exit);
            }

            // The interned '*T' for an element type, used for a '&val' binding. Falls back to
            // a bare Anyptr when nothing in the program interned that pointer type -- only
            // the ADDRESS is ever used, so the pointee is informational here.
            [[nodiscard]] auto pointer_to(const sema::ResolvedType &pointee) const -> sema::ResolvedType {
                for (size_t i = 0; i < sema_.pointer_pointees.size(); ++i) {
                    if (sema_.pointer_pointees[i] == pointee) {
                        return sema::ResolvedType{.kind = sema::TypeKind::Pointer,
                                                   .pointee_index = static_cast<int>(i)};
                    }
                }
                return sema::ResolvedType{.kind = sema::TypeKind::Anyptr};
            }

            // A condition must be I1. Integer conditions (the truthiness 'when' and 'if'
            // accept) become a non-zero comparison.
            auto emit_condition(mir::Builder &b, const ast::Expr &expr) -> mir::ValueId {
                // An error value in boolean context tests its Ok/Failed tag (spec §16). The
                // error union is an aggregate, so its "value" is an address and the tag is
                // the u32 at offset 0 -- 'Failed' being non-zero is what makes 'if err' read
                // as "if it failed".
                const auto type = expr_type(expr);
                if (type.kind == sema::TypeKind::Union) {
                    if (const auto *info = sema_.union_at(type.union_index); info && info->is_error_union) {
                        const auto address = emit_expr(b, expr);
                        if (address == mir::NO_VALUE) return mir::NO_VALUE;
                        return b.compare(mir::Op::ICmpNe, b.load(mir::Ty::I32, address),
                                          b.const_int(mir::Ty::I32, 0));
                    }
                }

                const auto value = emit_expr(b, expr);
                if (value == mir::NO_VALUE) return mir::NO_VALUE;
                const auto ty = b.value_type(value);
                if (ty == mir::Ty::I1) return value;
                if (mir::is_integer(ty)) {
                    return b.compare(mir::Op::ICmpNe, value, b.const_int(ty, 0));
                }
                if (ty == mir::Ty::Ptr) {
                    // A pointer in boolean context is a null test.
                    return b.compare(mir::Op::ICmpNe, value, b.const_null());
                }
                unsupported("a condition of this type", sema::get_expr_location(expr));
                return mir::NO_VALUE;
            }

            // ---- lvalues -----------------------------------------------------------

            // The type an lvalue denotes. sema records expr_types for values, but an
            // assignment TARGET is not one -- so 'v = ...' would otherwise read Invalid and
            // be mistaken for an aggregate. Mirrors emit_address's shape exactly; the two
            // must agree about what each form denotes.
            [[nodiscard]] auto lvalue_type(const ast::Expr &expr) const -> sema::ResolvedType {
                if (const auto recorded = expr_type(expr); recorded.kind != sema::TypeKind::Invalid) {
                    return recorded;
                }
                if (const auto *ident = std::get_if<ast::IdentExpr>(&expr)) {
                    if (const auto it = local_types_.find(ident->name); it != local_types_.end()) {
                        return it->second;
                    }
                    // A module-scope global: its declared type, from the symbol table --
                    // sema records expr_types for reads but an assignment TARGET is not one.
                    if (module_) {
                        if (const auto it = module_->symbols.find(ident->name); it != module_->symbols.end()) {
                            if (const auto *global = std::get_if<sema::GlobalSymbol>(&it->second)) {
                                return global->type;
                            }
                        }
                    }
                }
                if (const auto *member = std::get_if<std::unique_ptr<ast::MemberExpr>>(&expr)) {
                    // 'mod.g' -- a global reached through a namespace, same rule as above.
                    if (const auto *target = namespace_target((*member)->object)) {
                        if (const auto mod = sema_.modules.find(*target); mod != sema_.modules.end()) {
                            if (const auto it = mod->second.symbols.find((*member)->member);
                                it != mod->second.symbols.end()) {
                                if (const auto *global = std::get_if<sema::GlobalSymbol>(&it->second)) {
                                    return global->type;
                                }
                            }
                        }
                    }
                    auto object_type = lvalue_type((*member)->object);
                    if (object_type.kind == sema::TypeKind::Pointer) {
                        if (const auto *pointee = sema_.pointee_at(object_type.pointee_index)) {
                            object_type = *pointee;
                        }
                    }
                    if (object_type.kind == sema::TypeKind::Struct) {
                        if (const auto *info = sema_.struct_at(object_type.struct_index)) {
                            for (const auto &field : info->fields) {
                                if (field.name == (*member)->member) return field.type;
                            }
                        }
                    }
                    // An untagged union member reads its own type out of the shared storage.
                    if (object_type.kind == sema::TypeKind::Union) {
                        if (const auto *info = sema_.union_at(object_type.union_index);
                            info && !info->is_tagged) {
                            for (const auto &m : info->members) {
                                if (m.name == (*member)->member) return m.type;
                            }
                        }
                    }
                }
                if (const auto *index = std::get_if<std::unique_ptr<ast::IndexOrInstantiateExpr>>(&expr)) {
                    const auto operand_type = lvalue_type((*index)->operand);
                    if (operand_type.kind == sema::TypeKind::Array) {
                        if (const auto *info = sema_.array_at(operand_type.array_index)) return info->element_type;
                    } else if (operand_type.kind == sema::TypeKind::Slice) {
                        if (const auto *info = sema_.slice_at(operand_type.slice_index)) return info->element_type;
                    } else if (operand_type.kind == sema::TypeKind::Pointer) {
                        if (const auto *pointee = sema_.pointee_at(operand_type.pointee_index)) return *pointee;
                    }
                }
                if (const auto *unary = std::get_if<std::unique_ptr<ast::UnaryExpr>>(&expr)) {
                    if ((*unary)->op == ast::UnaryOp::Deref) {
                        const auto operand_type = lvalue_type((*unary)->operand);
                        if (operand_type.kind == sema::TypeKind::Pointer) {
                            if (const auto *pointee = sema_.pointee_at(operand_type.pointee_index)) return *pointee;
                        }
                    }
                }
                return sema::ResolvedType{.kind = sema::TypeKind::Invalid};
            }


            // The ADDRESS of an expression, or NO_VALUE if it has none (a temporary).
            //
            // This is the foundation the aggregate work sits on: because MIR values are
            // scalars and everything wider lives in memory, "read a struct field" and
            // "assign to a struct field" are the same address computation followed by a load
            // or a store. Keeping that in one place is what stops the two from drifting.
            //
            // Every offset here comes from sema (StructInfo::fields[].offset, ArrayInfo),
            // which is why no layout logic is needed — see mir.hpp's opening note.
            auto emit_address(mir::Builder &b, const ast::Expr &expr) -> mir::ValueId {
                if (const auto *ident = std::get_if<ast::IdentExpr>(&expr)) {
                    if (const auto it = locals_.find(ident->name); it != locals_.end()) {
                        return b.slot_addr(it->second);
                    }
                    if (module_path_) {
                        if (const auto g = global_index_.find(key(*module_path_, ident->name));
                            g != global_index_.end()) {
                            return b.global_addr(g->second);
                        }
                    }
                    return mir::NO_VALUE;
                }

                if (const auto *member = std::get_if<std::unique_ptr<ast::MemberExpr>>(&expr)) {
                    return emit_member_address(b, **member);
                }

                if (const auto *index = std::get_if<std::unique_ptr<ast::IndexOrInstantiateExpr>>(&expr)) {
                    return emit_index_address(b, **index);
                }

                if (const auto *unary = std::get_if<std::unique_ptr<ast::UnaryExpr>>(&expr)) {
                    // 'p.*' — the address IS the pointer value.
                    if ((*unary)->op == ast::UnaryOp::Deref) {
                        return emit_expr(b, (*unary)->operand);
                    }
                }

                return mir::NO_VALUE;
            }

            // 'obj.field', where 'obj' may be a struct value, a struct behind a pointer, or a
            // module namespace. Auto-deref matches the language: 'p.x' on a '*Point' reads
            // through the pointer without an explicit '.*'.
            auto emit_member_address(mir::Builder &b, const ast::MemberExpr &member) -> mir::ValueId {
                // 'mod.g' -- a global reached through a module namespace.
                if (const auto *target = namespace_target(member.object)) {
                    if (const auto g = global_index_.find(key(*target, member.member));
                        g != global_index_.end()) {
                        return b.global_addr(g->second);
                    }
                    return mir::NO_VALUE;
                }

                auto object_type = lvalue_type(member.object);

                mir::ValueId base = mir::NO_VALUE;
                if (object_type.kind == sema::TypeKind::Pointer) {
                    // A pointer object: its VALUE is the base address.
                    base = emit_expr(b, member.object);
                    if (const auto *pointee = sema_.pointee_at(object_type.pointee_index)) {
                        object_type = *pointee;
                    }
                } else {
                    base = emit_address(b, member.object);
                }
                if (base == mir::NO_VALUE) return mir::NO_VALUE;

                // An UNTAGGED union's members all alias its storage: every member lives at
                // offset 0 (codegen's emit_union_expr_value stores there too).
                if (object_type.kind == sema::TypeKind::Union) {
                    const auto *info = sema_.union_at(object_type.union_index);
                    if (info && !info->is_tagged) {
                        for (const auto &m : info->members) {
                            if (m.name == member.member) return base;
                        }
                    }
                    return mir::NO_VALUE;
                }
                if (object_type.kind != sema::TypeKind::Struct) return mir::NO_VALUE;

                const auto *info = sema_.struct_at(object_type.struct_index);
                if (!info) return mir::NO_VALUE;
                for (const auto &field : info->fields) {
                    if (field.name == member.member) {
                        return b.ptr_add_const(base, field.offset);
                    }
                }
                return mir::NO_VALUE;
            }

            // 'arr[i]' on an array or a pointer. A slice's data pointer lives at offset 0 of
            // its two-word representation, so indexing one loads that first.
            auto emit_index_address(mir::Builder &b, const ast::IndexOrInstantiateExpr &index) -> mir::ValueId {
                if (index.args.size() != 1) return mir::NO_VALUE;
                const auto *arg = std::get_if<ast::Expr>(&index.args.front().value);
                if (!arg) return mir::NO_VALUE;

                const auto operand_type = expr_type(index.operand);
                const auto usize = options_.pointer_bits == 64 ? mir::Ty::I64 : mir::Ty::I32;

                mir::ValueId base = mir::NO_VALUE;
                sema::ResolvedType element{};
                if (operand_type.kind == sema::TypeKind::Array) {
                    const auto *info = sema_.array_at(operand_type.array_index);
                    if (!info) return mir::NO_VALUE;
                    element = info->element_type;
                    base = emit_address(b, index.operand);
                } else if (operand_type.kind == sema::TypeKind::Pointer) {
                    const auto *pointee = sema_.pointee_at(operand_type.pointee_index);
                    if (!pointee) return mir::NO_VALUE;
                    element = *pointee;
                    base = emit_expr(b, index.operand);
                } else if (operand_type.kind == sema::TypeKind::Slice) {
                    const auto *info = sema_.slice_at(operand_type.slice_index);
                    if (!info) return mir::NO_VALUE;
                    element = info->element_type;
                    const auto slice_addr = emit_address(b, index.operand);
                    if (slice_addr == mir::NO_VALUE) return mir::NO_VALUE;
                    base = b.load(mir::Ty::Ptr, slice_addr); // data pointer at offset 0
                } else {
                    return mir::NO_VALUE;
                }
                if (base == mir::NO_VALUE) return mir::NO_VALUE;

                const auto idx = emit_expr(b, *arg);
                if (idx == mir::NO_VALUE) return mir::NO_VALUE;
                const auto scaled = b.binary(mir::Op::Mul, usize,
                                              coerce_to(b, idx, usize, signed_type(expr_type(*arg))),
                                              b.const_int(usize, size_of(element)));
                return b.ptr_add(base, scaled);
            }

            // ---- expressions -------------------------------------------------------

            // Names an expression form for the coverage summary. Worth spelling out rather
            // than reporting "an expression": the summary is how stage 2's remaining work is
            // prioritised, and an undifferentiated bucket hides which construct is actually
            // blocking the corpus.
            template <typename V>
            static auto expr_kind_name() -> const char * {
                if constexpr (std::is_same_v<V, ast::LiteralStringExpr>) return "a string literal";
                else if constexpr (std::is_same_v<V, ast::LiteralCharExpr>) return "a character literal";
                else if constexpr (std::is_same_v<V, std::unique_ptr<ast::MemberExpr>>) return "a member access";
                else if constexpr (std::is_same_v<V, std::unique_ptr<ast::IndexOrInstantiateExpr>>) return "an index or instantiation";
                else if constexpr (std::is_same_v<V, std::unique_ptr<ast::CastExpr>>) return "a cast";
                else if constexpr (std::is_same_v<V, std::unique_ptr<ast::BracedInitializerExpr>>) return "a braced initializer";
                else if constexpr (std::is_same_v<V, std::unique_ptr<ast::TypeOfExpr>>) return "'type_of'";
                else if constexpr (std::is_same_v<V, std::unique_ptr<ast::TypeInfoOfExpr>>) return "'type_info_of'";
                else if constexpr (std::is_same_v<V, std::unique_ptr<ast::LenExpr>>) return "'len'";
                else if constexpr (std::is_same_v<V, std::unique_ptr<ast::SliceExpr>>) return "a slice expression";
                else if constexpr (std::is_same_v<V, ast::DotIdentExpr>) return "a '.variant' reference";
                else if constexpr (std::is_same_v<V, ast::DefaultExpr>) return "'default'";
                else if constexpr (std::is_same_v<V, ast::UndefinedExpr>) return "'undefined'";
                else if constexpr (std::is_same_v<V, std::unique_ptr<ast::IncrDecrExpr>>) return "'++' / '--'";
                else if constexpr (std::is_same_v<V, std::unique_ptr<ast::StackAllocExpr>>) return "'stackalloc'";
                else if constexpr (std::is_same_v<V, std::unique_ptr<ast::AsmExpr>>) return "an 'asm' expression";
                else if constexpr (std::is_same_v<V, std::unique_ptr<ast::OptionExpr>>) return "'$option'";
                else if constexpr (std::is_same_v<V, std::unique_ptr<ast::EnvExpr>>) return "'$env'";
                else if constexpr (std::is_same_v<V, ast::RttiEnabledExpr>) return "'$rtti_enabled'";
                else if constexpr (std::is_same_v<V, ast::IotaExpr>) return "'iota'";
                else if constexpr (std::is_same_v<V, ast::ImportExpr> || std::is_same_v<V, ast::ImportBinExpr>) return "an import expression";
                else if constexpr (std::is_same_v<V, std::unique_ptr<ast::SpreadExpr>>) return "an argument spread";
                else if constexpr (std::is_same_v<V, std::unique_ptr<ast::TypeExpr>>) return "a type in expression position";
                else return "an expression";
            }

            auto emit_expr(mir::Builder &b, const ast::Expr &expr) -> mir::ValueId {
                const auto natural = emit_expr_natural(b, expr);
                if (natural == mir::NO_VALUE || !exprs_) return natural;
                // Implicit coercions sema recorded for THIS expression -- tagged-union
                // wrapping, pointer-to-trait-handle, handle-to-handle narrowing. Applied at
                // the one funnel every context goes through, rather than re-implemented per
                // context the way codegen's emit_value_as does.
                const auto expr_key = sema::get_expr_key(expr);
                if (const auto it = exprs_->expr_variant_coercions.find(expr_key);
                    it != exprs_->expr_variant_coercions.end()) {
                    return emit_variant_coercion(b, natural, expr, it->second);
                }
                if (const auto it = exprs_->expr_trait_coercions.find(expr_key);
                    it != exprs_->expr_trait_coercions.end()) {
                    return emit_trait_coercion(b, natural, expr, it->second);
                }
                if (const auto it = exprs_->expr_trait_handle_coercions.find(expr_key);
                    it != exprs_->expr_trait_handle_coercions.end()) {
                    return emit_trait_handle_coercion(b, natural, it->second);
                }
                return natural;
            }

            // A two-word {data, vtable} trait handle built in a fresh slot; an aggregate,
            // so its value is the slot's address.
            auto build_trait_handle(mir::Builder &b, const mir::ValueId data, const mir::ValueId vtable) -> mir::ValueId {
                const auto slot = b.add_slot(pointer_bytes() * 2, pointer_bytes(), "handle");
                const auto base = b.slot_addr(slot);
                b.store(base, data);
                b.store(b.ptr_add_const(base, pointer_bytes()), vtable);
                return base;
            }

            // '*T -> Trait' (sema::TraitCoercion): word 0 is the source pointer as it is,
            // word 1 the impl's vtable global -- the direct one, or the synthesized
            // sub-vtable when the coercion resolved through a composing trait.
            auto emit_trait_coercion(mir::Builder &b, const mir::ValueId data, const ast::Expr &expr,
                                      const sema::TraitCoercion &tc) -> mir::ValueId {
                const auto loc = sema::get_expr_location(expr);
                const auto from = expr_type(expr);
                if (from.kind != sema::TypeKind::Pointer) {
                    unsupported("a trait coercion of this operand", loc);
                    return mir::NO_VALUE;
                }
                const auto *pointee = sema_.pointee_at(from.pointee_index);
                if (!pointee) {
                    unsupported("a trait coercion of this operand", loc);
                    return mir::NO_VALUE;
                }
                const auto [pointee_module, pointee_name] = sema::find_type_module_and_name(*pointee, sema_);
                const sema::ResolvedType trait_ty{.kind = sema::TypeKind::Trait, .trait_index = tc.trait_index};
                const auto [trait_module, trait_name] = sema::find_type_module_and_name(trait_ty, sema_);

                uint32_t vtable = UINT32_MAX;
                if (tc.provider_trait_index == tc.trait_index) {
                    if (const auto it = vtable_index_.find({trait_module, trait_name, pointee_module, pointee_name});
                        it != vtable_index_.end()) {
                        vtable = it->second;
                    }
                } else {
                    const sema::ResolvedType provider_ty{.kind = sema::TypeKind::Trait,
                                                          .trait_index = tc.provider_trait_index};
                    const auto [provider_module, provider_name] = sema::find_type_module_and_name(provider_ty, sema_);
                    if (const auto it = component_vtable_index_.find(
                            {provider_module, provider_name, trait_module, trait_name, pointee_module, pointee_name});
                        it != component_vtable_index_.end()) {
                        vtable = it->second;
                    }
                }
                if (vtable == UINT32_MAX) {
                    unsupported("a trait coercion without a vtable", loc);
                    return mir::NO_VALUE;
                }
                return build_trait_handle(b, data, b.global_addr(vtable));
            }

            // Handle-to-handle composed-trait narrowing (sema::TraitHandleCoercion): the
            // data word passes through; the new vtable is loaded from the source vtable's
            // pre-computed trailing slot. Zero runtime checks.
            auto emit_trait_handle_coercion(mir::Builder &b, const mir::ValueId handle,
                                             const sema::TraitHandleCoercion &thc) -> mir::ValueId {
                const auto data = b.load(mir::Ty::Ptr, handle);
                const auto vtable = b.load(mir::Ty::Ptr, b.ptr_add_const(handle, pointer_bytes()));
                const auto sub = b.load(mir::Ty::Ptr,
                    b.ptr_add_const(vtable, static_cast<int64_t>(thc.slot_index) * pointer_bytes()));
                return build_trait_handle(b, data, sub);
            }

            // Materializes a sema::VariantCoercion over an already-emitted value: tag at 0,
            // the value at the variant's payload location -- verbatim if the expression's own
            // type is already the payload struct, wrapped into its single field otherwise.
            // The variant choice comes from sema's record; this never re-scans variants.
            auto emit_variant_coercion(mir::Builder &b, const mir::ValueId value, const ast::Expr &expr,
                                        const sema::VariantCoercion &vc) -> mir::ValueId {
                const auto *info = vc.union_type.kind == sema::TypeKind::Union
                    ? sema_.union_at(vc.union_type.union_index) : nullptr;
                if (!info) {
                    unsupported("a tagged-union coercion", sema::get_expr_location(expr));
                    return mir::NO_VALUE;
                }
                const auto slot = b.add_slot(std::max(1u, size_of(vc.union_type)),
                                              std::max(1u, align_of(vc.union_type)), "variant");
                const auto base = b.slot_addr(slot);
                b.mem_set(base, b.const_int(mir::Ty::I8, 0), b.const_int(usize_ty(), size_of(vc.union_type)));
                b.store(base, b.const_int(mir::Ty::I32, vc.tag_value));

                if (vc.payload_struct_index >= 0) {
                    const auto from = expr_type(expr);
                    const bool struct_payload = from.kind == sema::TypeKind::Struct &&
                                                 from.struct_index == vc.payload_struct_index;
                    const auto at = info->payload_offset +
                                     (struct_payload ? 0 : wrapper_field_offset(vc.payload_struct_index));
                    const auto dest = at == 0 ? base : b.ptr_add_const(base, at);
                    if (is_scalar(from)) {
                        b.store(dest, value);
                    } else {
                        b.mem_copy(dest, value, b.const_int(usize_ty(), size_of(from)));
                    }
                }
                return base;
            }

            auto emit_expr_natural(mir::Builder &b, const ast::Expr &expr) -> mir::ValueId {
                const auto loc = sema::get_expr_location(expr);
                b.set_location(loc.line, loc.column);

                return std::visit([&]<typename T>(const T &v) -> mir::ValueId {
                    using V = std::decay_t<T>;

                    if constexpr (std::is_same_v<V, ast::LiteralIntegerExpr>) {
                        const auto ty = expr_type(expr);
                        const auto mir_ty = is_scalar(ty) ? scalar_type(ty) : mir::Ty::I64;
                        return b.const_int(mir_ty, static_cast<int64_t>(v.value));

                    } else if constexpr (std::is_same_v<V, ast::LiteralBoolExpr>) {
                        return b.const_int(mir::Ty::I1, v.value ? 1 : 0);

                    } else if constexpr (std::is_same_v<V, ast::LiteralFloatExpr>) {
                        const auto ty = expr_type(expr);
                        return b.const_float(is_scalar(ty) ? scalar_type(ty) : mir::Ty::F64, v.value);

                    } else if constexpr (std::is_same_v<V, ast::LiteralNilExpr>) {
                        return b.const_null();

                    } else if constexpr (std::is_same_v<V, ast::IdentExpr>) {
                        return emit_ident(b, v, expr);

                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::BinaryExpr>>) {
                        return emit_binary(b, *v, expr);

                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::UnaryExpr>>) {
                        return emit_unary(b, *v, expr);

                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::AssignExpr>>) {
                        return emit_assign(b, *v);

                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::CallExpr>>) {
                        return emit_call(b, *v, expr);

                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::MemberExpr>>) {
                        // 'Dir.South' names an enum variant through its TYPE, which is a
                        // constant -- unlike 'p.kind', which reads a field that happens to
                        // be enum-typed. The object decides which, so it is what we test.
                        if (const auto value = enum_member_constant(b, *v, expr); value != mir::NO_VALUE) {
                            return value;
                        }
                        // 'U.None' -- a payload-free tagged-union variant through its type.
                        if (const auto type = expr_type(expr);
                            type.kind == sema::TypeKind::Union && names_a_type(v->object)) {
                            if (const auto value = emit_tag_only_variant(b, type, v->member);
                                value != mir::NO_VALUE) {
                                return value;
                            }
                        }
                        return emit_load_from_address(b, expr, loc, expr_kind_name<V>());

                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::IndexOrInstantiateExpr>>) {
                        // 'fnv1a[i32]' as a VALUE names a monomorphized instance as a
                        // function pointer; sema recorded which one.
                        if (exprs_ && expr_type(expr).kind == sema::TypeKind::Function) {
                            if (const auto it = exprs_->expr_generic_fn_instance.find(sema::get_expr_key(expr));
                                it != exprs_->expr_generic_fn_instance.end()) {
                                if (const auto fn = generic_instance_index_.find(it->second);
                                    fn != generic_instance_index_.end()) {
                                    return b.func_addr(fn->second);
                                }
                            }
                        }
                        return emit_load_from_address(b, expr, loc, expr_kind_name<V>());

                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::CastExpr>>) {
                        return emit_cast(b, *v, expr);

                    } else if constexpr (std::is_same_v<V, ast::LiteralCharExpr>) {
                        // A character literal is a 'u8' value; the lexer already rejected
                        // anything wider than one byte.
                        return b.const_int(mir::Ty::I8, static_cast<int64_t>(v.value));

                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::TypeOfExpr>>) {
                        // 'type_of(T)' is a compile-time constant: the type's interned id,
                        // which sema assigned. Nothing is computed at runtime.
                        return emit_type_id(b, expr, v->location);

                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::OptionExpr>> ||
                                          std::is_same_v<V, std::unique_ptr<ast::EnvExpr>>) {
                        // '$option'/'$env' resolved at compile time; sema cached the value
                        // on the MODULE (it is instantiation-independent by design).
                        if (module_) {
                            if (const auto it = module_->expr_option_values.find(sema::get_expr_key(expr));
                                it != module_->expr_option_values.end()) {
                                const auto type = expr_type(expr);
                                if (const auto *num = std::get_if<int64_t>(&it->second); num && is_scalar(type)) {
                                    const auto ty = scalar_type(type);
                                    return mir::is_float(ty)
                                        ? b.const_float(ty, static_cast<double>(*num))
                                        : b.const_int(ty, *num);
                                }
                                if (const auto *str = std::get_if<std::string>(&it->second)) {
                                    const auto slice = emit_string_literal(b, *str);
                                    if (type.kind == sema::TypeKind::Slice) return slice;
                                    // A '*u8' target takes the interned literal's data word.
                                    if (is_scalar(type) && scalar_type(type) == mir::Ty::Ptr) {
                                        return b.load(mir::Ty::Ptr, slice);
                                    }
                                }
                            }
                        }
                        unsupported(expr_kind_name<V>(), loc);
                        return mir::NO_VALUE;

                    } else if constexpr (std::is_same_v<V, ast::DefaultExpr>) {
                        // In expression position (an assignment's RHS, a return value): a
                        // scalar is its zero; an aggregate is a fresh zeroed slot whose
                        // address is the value, like any other aggregate expression.
                        const auto type = expr_type(expr);
                        if (is_scalar(type)) {
                            const auto ty = scalar_type(type);
                            if (ty == mir::Ty::Ptr) return b.const_null();
                            if (mir::is_float(ty)) return b.const_float(ty, 0.0);
                            return b.const_int(ty, 0);
                        }
                        if (type.kind != sema::TypeKind::Invalid && type.kind != sema::TypeKind::Void) {
                            const auto slot = b.add_slot(std::max(1u, size_of(type)),
                                                          std::max(1u, align_of(type)), "default");
                            const auto base = b.slot_addr(slot);
                            b.mem_set(base, b.const_int(mir::Ty::I8, 0),
                                       b.const_int(usize_ty(), size_of(type)));
                            return base;
                        }
                        unsupported("'default' in this position", loc);
                        return mir::NO_VALUE;

                    } else if constexpr (std::is_same_v<V, ast::DotIdentExpr>) {
                        return emit_dot_ident(b, v, expr);

                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::TaggedVariantExpr>>) {
                        return emit_tagged_variant(b, *v, expr);

                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::MatchExpr>>) {
                        return emit_match(b, *v, expr);

                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::TryExpr>>) {
                        return emit_try(b, *v, loc);

                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::TernaryExpr>>) {
                        return emit_ternary_shape(b, v->condition, v->then_expr, v->else_expr, expr);

                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::WhenExpr>>) {
                        return emit_when_expr(b, *v, expr);

                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::SizeOfExpr>>) {
                        // Compile-time constants: sema owns layout, so the answer is a
                        // lookup, never a computation.
                        if (const auto type = operand_named_type(v->operand)) {
                            return b.const_int(usize_ty(), static_cast<int64_t>(size_of(*type)));
                        }
                        unsupported("'size_of' on this operand", loc);
                        return mir::NO_VALUE;

                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::AlignOfExpr>>) {
                        if (const auto type = operand_named_type(v->operand)) {
                            return b.const_int(usize_ty(), static_cast<int64_t>(align_of(*type)));
                        }
                        unsupported("'align_of' on this operand", loc);
                        return mir::NO_VALUE;

                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::BracedInitializerExpr>>) {
                        return emit_braced_initializer(b, *v, expr);

                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::IncrDecrExpr>>) {
                        return emit_incr_decr(b, *v);

                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::SliceExpr>>) {
                        return emit_slice_expr(b, *v);

                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::LenExpr>>) {
                        return emit_len(b, *v);

                    } else if constexpr (std::is_same_v<V, ast::LiteralStringExpr>) {
                        return emit_string_literal(b, v.value);

                    } else {
                        unsupported(expr_kind_name<V>(), loc);
                        return mir::NO_VALUE;
                    }
                }, expr);
            }

            auto emit_ident(mir::Builder &b, const ast::IdentExpr &ident, const ast::Expr &expr) -> mir::ValueId {
                // A macro parameter shadows everything inside its template.
                if (const auto macro = macro_args_.find(ident.name); macro != macro_args_.end()) {
                    return emit_macro_arg(b, macro->second);
                }
                if (const auto it = locals_.find(ident.name); it != locals_.end()) {
                    const auto type = local_types_.at(ident.name);
                    if (!is_scalar(type)) {
                        // An aggregate local IS its address; loading it would need a type MIR
                        // does not have.
                        return b.slot_addr(it->second);
                    }
                    return b.load(scalar_type(type), b.slot_addr(it->second));
                }

                if (module_path_) {
                    if (const auto it = global_index_.find(key(*module_path_, ident.name));
                        it != global_index_.end()) {
                        const auto type = expr_type(expr);
                        const auto addr = b.global_addr(it->second);
                        return is_scalar(type) ? b.load(scalar_type(type), addr) : addr;
                    }
                    if (const auto it = function_index_.find(key(*module_path_, ident.name));
                        it != function_index_.end()) {
                        return b.func_addr(it->second);
                    }
                }

                unsupported(std::format("a reference to '{}'", ident.name), ident.location);
                return mir::NO_VALUE;
            }

            auto emit_binary(mir::Builder &b, const ast::BinaryExpr &bin, const ast::Expr &expr) -> mir::ValueId {
                using Bop = ast::BinaryOp;

                // Short-circuit operators are control flow, not arithmetic: the right operand
                // must not be evaluated when the left decides the answer. Block parameters
                // carry the result, which is the case phi nodes existed for.
                if (bin.op == Bop::LogicalAnd || bin.op == Bop::LogicalOr) {
                    const auto lhs = emit_condition(b, bin.lhs);
                    if (lhs == mir::NO_VALUE) return mir::NO_VALUE;

                    const auto rhs_block = b.create_block(bin.op == Bop::LogicalAnd ? "and.rhs" : "or.rhs");
                    const auto join = b.create_block(bin.op == Bop::LogicalAnd ? "and.end" : "or.end");
                    const auto merged = b.add_block_param(join, mir::Ty::I1);
                    const auto shortcut = b.create_block("logic.short");

                    if (bin.op == Bop::LogicalAnd) {
                        b.branch(lhs, rhs_block, shortcut);
                    } else {
                        b.branch(lhs, shortcut, rhs_block);
                    }

                    b.set_insert_point(shortcut);
                    b.jump(join, {b.const_int(mir::Ty::I1, bin.op == Bop::LogicalAnd ? 0 : 1)});

                    b.set_insert_point(rhs_block);
                    const auto rhs = emit_condition(b, bin.rhs);
                    // Even when the right operand could not be lowered (an unsupported
                    // construct, already diagnosed), this block must still be TERMINATED and
                    // must still pass the join its parameter -- otherwise the failure leaves
                    // a malformed module behind and every later diagnostic is about that
                    // instead of about the real cause.
                    b.jump(join, {rhs == mir::NO_VALUE ? b.const_int(mir::Ty::I1, 0) : rhs});
                    if (rhs == mir::NO_VALUE) {
                        b.set_insert_point(join);
                        return merged;
                    }

                    b.set_insert_point(join);
                    return merged;
                }

                const auto lhs_type = expr_type(bin.lhs);
                const auto rhs_type = expr_type(bin.rhs);
                const auto lhs = emit_expr(b, bin.lhs);
                const auto rhs = emit_expr(b, bin.rhs);
                if (lhs == mir::NO_VALUE || rhs == mir::NO_VALUE) return mir::NO_VALUE;

                // Pointer +/- integer is address arithmetic stepping by the pointee's size
                // (1 for anyptr), never an integer op -- an 'add' on a Ptr operand is
                // ill-typed MIR, which the verifier rejects.
                const auto pointer_like = [](const sema::ResolvedType &t) {
                    return t.kind == sema::TypeKind::Pointer || t.kind == sema::TypeKind::Anyptr;
                };
                if ((bin.op == Bop::Add || bin.op == Bop::Sub) &&
                    pointer_like(lhs_type) && rhs_type.is_integer()) {
                    return emit_pointer_offset(b, lhs, rhs, lhs_type, rhs_type, bin.op == Bop::Sub);
                }
                if (bin.op == Bop::Add && pointer_like(rhs_type) && lhs_type.is_integer()) {
                    return emit_pointer_offset(b, rhs, lhs, rhs_type, lhs_type, false);
                }

                // Trait-handle comparison ('h == nil', 'h != other'): a handle is a
                // {data, vtable} aggregate, so the DATA words are compared -- object
                // identity, and a nil handle's data word is null. Comparing the vtable
                // word instead would falsely equate two objects of the same concrete type.
                if ((bin.op == Bop::Equal || bin.op == Bop::NotEqual) &&
                    (lhs_type.kind == sema::TypeKind::Trait || rhs_type.kind == sema::TypeKind::Trait)) {
                    const auto data_word = [&](const ast::Expr &operand, const mir::ValueId value,
                                                const sema::ResolvedType &type) {
                        // A 'nil' literal is contextually trait-typed, but its emitted value
                        // IS the null data word already -- loading through it would crash.
                        if (std::holds_alternative<ast::LiteralNilExpr>(operand)) return value;
                        return type.kind == sema::TypeKind::Trait ? b.load(mir::Ty::Ptr, value) : value;
                    };
                    return b.compare(bin.op == Bop::Equal ? mir::Op::ICmpEq : mir::Op::ICmpNe,
                                      data_word(bin.lhs, lhs, lhs_type), data_word(bin.rhs, rhs, rhs_type));
                }

                const auto ty = b.value_type(lhs);
                const bool is_signed = signed_type(lhs_type);
                const bool is_float = mir::is_float(ty);

                const auto arith = [&](const mir::Op int_op, const mir::Op float_op) {
                    return b.binary(is_float ? float_op : int_op, ty, lhs, rhs);
                };

                switch (bin.op) {
                case Bop::Add: return arith(mir::Op::Add, mir::Op::FAdd);
                case Bop::Sub: return arith(mir::Op::Sub, mir::Op::FSub);
                case Bop::Mul: return arith(mir::Op::Mul, mir::Op::FMul);
                case Bop::Div:
                    return b.binary(is_float ? mir::Op::FDiv : (is_signed ? mir::Op::SDiv : mir::Op::UDiv), ty, lhs, rhs);
                case Bop::Mod:
                    return b.binary(is_float ? mir::Op::FRem : (is_signed ? mir::Op::SRem : mir::Op::URem), ty, lhs, rhs);
                case Bop::BitwiseAnd: return b.binary(mir::Op::And, ty, lhs, rhs);
                case Bop::BitwiseOr:  return b.binary(mir::Op::Or, ty, lhs, rhs);
                case Bop::BitwiseXor: return b.binary(mir::Op::Xor, ty, lhs, rhs);
                case Bop::ShiftLeft:  return b.binary(mir::Op::Shl, ty, lhs, rhs);
                case Bop::ShiftRight:
                    return b.binary(is_signed ? mir::Op::AShr : mir::Op::LShr, ty, lhs, rhs);
                case Bop::Equal:
                    return b.compare(is_float ? mir::Op::FCmpOeq : mir::Op::ICmpEq, lhs, rhs);
                case Bop::NotEqual:
                    return b.compare(is_float ? mir::Op::FCmpOne : mir::Op::ICmpNe, lhs, rhs);
                case Bop::Less:
                    return b.compare(is_float ? mir::Op::FCmpOlt : (is_signed ? mir::Op::ICmpSlt : mir::Op::ICmpUlt), lhs, rhs);
                case Bop::LessEqual:
                    return b.compare(is_float ? mir::Op::FCmpOle : (is_signed ? mir::Op::ICmpSle : mir::Op::ICmpUle), lhs, rhs);
                case Bop::Greater:
                    return b.compare(is_float ? mir::Op::FCmpOgt : (is_signed ? mir::Op::ICmpSgt : mir::Op::ICmpUgt), lhs, rhs);
                case Bop::GreaterEqual:
                    return b.compare(is_float ? mir::Op::FCmpOge : (is_signed ? mir::Op::ICmpSge : mir::Op::ICmpUge), lhs, rhs);
                case Bop::In: {
                    // Bitset membership: single-member '.A in modes' and subset
                    // '{.A,.B} in modes' both reduce to '(rhs & mask) == mask' -- a single
                    // member IS its one-bit mask, and for one bit the forms are equivalent.
                    const auto masked = b.binary(mir::Op::And, ty, rhs, lhs);
                    return b.compare(mir::Op::ICmpEq, masked, lhs);
                }
                default:
                    unsupported("this binary operator", bin.location);
                    return mir::NO_VALUE;
                }
            }

            auto emit_unary(mir::Builder &b, const ast::UnaryExpr &un, const ast::Expr &expr) -> mir::ValueId {
                if (un.op == ast::UnaryOp::AddressOf) {
                    // Taking a local's address is exactly what stops promote_slots from
                    // touching it, so mark it before computing anything.
                    mark_root_slot_escaping(un.operand);
                    if (const auto address = emit_address(b, un.operand); address != mir::NO_VALUE) {
                        return address;
                    }
                    unsupported("taking the address of this expression", un.location);
                    return mir::NO_VALUE;
                }

                if (un.op == ast::UnaryOp::Deref) {
                    // 'p.*' is a load through the pointer, not an operation on it.
                    const auto result_type = expr_type(expr);
                    const auto address = emit_expr(b, un.operand);
                    if (address == mir::NO_VALUE) return mir::NO_VALUE;
                    if (!is_scalar(result_type)) {
                        // An aggregate behind a pointer IS its address; there is no wider
                        // MIR value to load it into.
                        return address;
                    }
                    return b.load(scalar_type(result_type), address);
                }

                const auto operand = emit_expr(b, un.operand);
                if (operand == mir::NO_VALUE) return mir::NO_VALUE;
                const auto ty = b.value_type(operand);

                switch (un.op) {
                case ast::UnaryOp::Negate:
                    return b.unary(mir::is_float(ty) ? mir::Op::FNeg : mir::Op::Neg, ty, operand);
                case ast::UnaryOp::LogicalNot:
                    // On a bool this is Op::Not, which is width-correct for I1. On an
                    // integer (the truthiness form) it is "== 0", and on a pointer it is a
                    // null test -- comparing a pointer against a const.int would have been
                    // ill-typed, which is what the MIR verifier caught.
                    if (ty == mir::Ty::I1) return b.unary(mir::Op::Not, ty, operand);
                    if (ty == mir::Ty::Ptr) return b.compare(mir::Op::ICmpEq, operand, b.const_null());
                    if (mir::is_integer(ty)) return b.compare(mir::Op::ICmpEq, operand, b.const_int(ty, 0));
                    unsupported("'!' on this operand type", un.location);
                    return mir::NO_VALUE;
                case ast::UnaryOp::BitwiseNot:
                    return b.unary(mir::Op::Not, ty, operand);
                default:
                    unsupported("this unary operator", un.location);
                    return mir::NO_VALUE;
                }
            }

            // 'p + n' / 'p - n' / 'n + p': the index is widened to the target word, scaled
            // by the pointee's size, and applied as byte arithmetic (codegen's
            // emit_pointer_offset, minus the GEP).
            auto emit_pointer_offset(mir::Builder &b, const mir::ValueId ptr, mir::ValueId amount,
                                      const sema::ResolvedType &ptr_type, const sema::ResolvedType &amount_type,
                                      const bool subtract) -> mir::ValueId {
                const auto usize = usize_ty();
                amount = coerce_to(b, amount, usize, signed_type(amount_type));
                uint32_t step = 1;
                if (ptr_type.kind == sema::TypeKind::Pointer) {
                    if (const auto *pointee = sema_.pointee_at(ptr_type.pointee_index)) {
                        step = std::max(1u, size_of(*pointee));
                    }
                }
                if (step != 1) {
                    amount = b.binary(mir::Op::Mul, usize, amount, b.const_int(usize, step));
                }
                if (subtract) {
                    amount = b.unary(mir::Op::Neg, usize, amount);
                }
                return b.ptr_add(ptr, amount);
            }

            auto emit_assign(mir::Builder &b, const ast::AssignExpr &assign) -> mir::ValueId {
                const auto target_type = lvalue_type(assign.target);
                const auto address = emit_address(b, assign.target);

                if (!is_scalar(target_type)) {
                    // An aggregate assignment writes through the shared coercion-aware
                    // path; the source's "value" is its address, which is what every
                    // aggregate expression yields.
                    if (address == mir::NO_VALUE) {
                        unsupported("assignment to this target", assign.location);
                        return mir::NO_VALUE;
                    }
                    const auto source = emit_expr(b, assign.value);
                    if (source == mir::NO_VALUE) return mir::NO_VALUE;
                    store_aggregate_value(b, address, target_type, source, expr_type(assign.value));
                    return address;
                }

                if (address == mir::NO_VALUE) {
                    unsupported("assignment to this target", assign.location);
                    return mir::NO_VALUE;
                }
                const auto value = emit_expr(b, assign.value);
                if (value == mir::NO_VALUE) return mir::NO_VALUE;

                // A compound assignment reads the old value and applies the operation; the
                // RHS is used RAW for a pointer step ('p += n' scales by the pointee, and
                // coercing n to Ptr first would be nonsense).
                if (assign.op != ast::AssignOp::Assign) {
                    const auto result = compound_assign_value(b, assign, address, value, target_type);
                    if (result == mir::NO_VALUE) return mir::NO_VALUE;
                    b.store(address, result);
                    return result;
                }

                const auto stored = coerce(b, value, target_type, expr_type(assign.value));
                b.store(address, stored);
                return stored;
            }

            // 'target OP= value' lowered as load/apply/store. Bitset '+=' and '-=' are the
            // SET operations (union, difference), '~=' is toggle; a pointer steps by its
            // pointee's size; everything else is the corresponding scalar arithmetic with
            // the target's signedness.
            auto compound_assign_value(mir::Builder &b, const ast::AssignExpr &assign, const mir::ValueId address,
                                        const mir::ValueId value, const sema::ResolvedType &target_type) -> mir::ValueId {
                using A = ast::AssignOp;
                const auto value_type = expr_type(assign.value);

                if ((target_type.kind == sema::TypeKind::Pointer || target_type.kind == sema::TypeKind::Anyptr) &&
                    (assign.op == A::AddAssign || assign.op == A::SubAssign)) {
                    const auto old = b.load(mir::Ty::Ptr, address);
                    return emit_pointer_offset(b, old, value, target_type, value_type,
                                                assign.op == A::SubAssign);
                }

                const auto ty = scalar_type(target_type);
                if (ty == mir::Ty::Void) {
                    unsupported("a compound assignment to this target", assign.location);
                    return mir::NO_VALUE;
                }
                const auto old = b.load(ty, address);
                const auto rhs = coerce_to(b, value, ty, signed_type(value_type));

                if (target_type.kind == sema::TypeKind::Bitset) {
                    switch (assign.op) {
                    case A::AddAssign: case A::OrAssign:
                        return b.binary(mir::Op::Or, ty, old, rhs);
                    case A::SubAssign:
                        return b.binary(mir::Op::And, ty, old, b.unary(mir::Op::Not, ty, rhs));
                    case A::ToggleAssign: case A::XorAssign:
                        return b.binary(mir::Op::Xor, ty, old, rhs);
                    case A::AndAssign:
                        return b.binary(mir::Op::And, ty, old, rhs);
                    default:
                        unsupported("this compound assignment on a bitset", assign.location);
                        return mir::NO_VALUE;
                    }
                }

                if (mir::is_float(ty)) {
                    switch (assign.op) {
                    case A::AddAssign: return b.binary(mir::Op::FAdd, ty, old, rhs);
                    case A::SubAssign: return b.binary(mir::Op::FSub, ty, old, rhs);
                    case A::MulAssign: return b.binary(mir::Op::FMul, ty, old, rhs);
                    case A::DivAssign: return b.binary(mir::Op::FDiv, ty, old, rhs);
                    default:
                        unsupported("this compound assignment on a float", assign.location);
                        return mir::NO_VALUE;
                    }
                }

                const bool is_signed = signed_type(target_type);
                switch (assign.op) {
                case A::AddAssign: return b.binary(mir::Op::Add, ty, old, rhs);
                case A::SubAssign: return b.binary(mir::Op::Sub, ty, old, rhs);
                case A::MulAssign: return b.binary(mir::Op::Mul, ty, old, rhs);
                case A::DivAssign: return b.binary(is_signed ? mir::Op::SDiv : mir::Op::UDiv, ty, old, rhs);
                case A::AndAssign: return b.binary(mir::Op::And, ty, old, rhs);
                case A::OrAssign:  return b.binary(mir::Op::Or, ty, old, rhs);
                case A::XorAssign: return b.binary(mir::Op::Xor, ty, old, rhs);
                case A::ShlAssign: return b.binary(mir::Op::Shl, ty, old, rhs);
                case A::ShrAssign: return b.binary(is_signed ? mir::Op::AShr : mir::Op::LShr, ty, old, rhs);
                default:
                    unsupported("this compound assignment", assign.location);
                    return mir::NO_VALUE;
                }
            }

            auto emit_call(mir::Builder &b, const ast::CallExpr &call, const ast::Expr &expr) -> mir::ValueId {
                if (!module_path_) {
                    unsupported("this call form", call.location);
                    return mir::NO_VALUE;
                }

                // A call sema resolved to a monomorphized generic instance -- checked FIRST,
                // before any shape-based routing: the same call node resolves to a different
                // instance per enclosing instantiation, so nothing here may re-derive it.
                if (const auto instance = generic_instance_for_call(call, &expr)) {
                    return emit_generic_call(b, call, *instance);
                }
                // Dynamic dispatch through a trait handle, likewise decided by sema and
                // keyed by the call node.
                if (exprs_) {
                    if (const auto it = exprs_->expr_trait_dispatch.find(&call);
                        it != exprs_->expr_trait_dispatch.end()) {
                        return emit_trait_dispatch(b, call, it->second);
                    }
                }

                // Two callee shapes lower here: a bare name in this module, and a
                // namespace-qualified 'mod.fn' -- which is an ordinary direct call once the
                // import binding names the target module. Method calls and calls through a
                // function pointer still need their own handling.
                auto it = function_index_.end();
                std::string callee_name;
                const std::string *callee_module = module_path_;
                if (const auto *ident = std::get_if<ast::IdentExpr>(&call.callee)) {
                    // A LOCAL of function-pointer type shadows any same-named function, and
                    // is an indirect call -- 'const f: fn(i32) -> i32 = add; f(1)'.
                    if (locals_.contains(ident->name)) {
                        return emit_indirect_call(b, call);
                    }
                    callee_name = ident->name;
                    // A bare import is an alias: the function itself was declared (once)
                    // under its ORIGIN module, so the call redirects there.
                    if (module_) {
                        if (const auto origin = module_->bare_import_origins.find(callee_name);
                            origin != module_->bare_import_origins.end()) {
                            callee_module = &origin->second.module_path;
                            callee_name = origin->second.symbol_name;
                        }
                    }
                    it = function_index_.find(key(*callee_module, callee_name));
                } else if (const auto *member = std::get_if<std::unique_ptr<ast::MemberExpr>>(&call.callee)) {
                    if (const auto *target = namespace_target((*member)->object)) {
                        callee_name = (*member)->member;
                        callee_module = target;
                        it = function_index_.find(key(*target, callee_name));
                    } else {
                        return emit_method_call(b, call, **member);
                    }
                } else {
                    return emit_indirect_call(b, call);
                }

                if (it == function_index_.end()) {
                    // A macro is expression-template expansion, not a call.
                    if (const auto mod = sema_.modules.find(*callee_module); mod != sema_.modules.end()) {
                        if (const auto sym = mod->second.symbols.find(callee_name);
                            sym != mod->second.symbols.end()) {
                            if (const auto *macro = std::get_if<sema::MacroSymbol>(&sym->second)) {
                                return emit_macro_call(b, call, *macro, mod->first, mod->second);
                            }
                        }
                    }
                    unsupported(std::format("a call to '{}'", callee_name), call.location);
                    return mir::NO_VALUE;
                }

                // A dropped trailing '?error(...)' slot needs the runtime unhandled-error
                // check; without it the blob would silently stand in for the surviving value.
                if (exprs_ && exprs_->call_dropped_optional_error.contains(&call)) {
                    unsupported("a call dropping an ignorable error", call.location);
                    return mir::NO_VALUE;
                }

                const auto &sig = result_.module.signatures[result_.module.functions[it->second].signature];

                // An aggregate or multi-return result comes back through a slot the CALLER
                // owns and passes in; the call's value is that slot's address. The slot is
                // sized from the callee's sema return list -- a multi-return call expression
                // has no recorded expr_type, so expr_type cannot answer here -- and must
                // agree with returns_via_sret, which decided the signature.
                std::vector<mir::ValueId> args;
                mir::ValueId sret_slot = mir::NO_VALUE;
                const auto callee_returns = symbol_return_types(*callee_module, callee_name);
                const bool via_sret = callee_returns && returns_via_sret(*callee_returns);
                if (via_sret) {
                    const auto layout = multi_return_layout(*callee_returns);
                    const auto slot = b.add_slot(layout.size, layout.align, "ret");
                    sret_slot = b.slot_addr(slot);
                    args.push_back(sret_slot);
                }

                // A MIRAGE-native variadic ('fn f(xs: ...T)') receives its tail as ONE
                // slice, collected here; only a C 'ext fn' variadic passes raw trailing
                // arguments. Getting this wrong is silent: the callee reads a slice header
                // out of whatever scalar landed in that position.
                const auto *fn_sym = symbol_function(*callee_module, callee_name);
                if (sig.is_variadic && fn_sym && !fn_sym->params.empty()) {
                    const size_t fixed = fn_sym->params.size() - 1;
                    if (call.args.size() < fixed) {
                        unsupported("a variadic call with missing fixed arguments", call.location);
                        return mir::NO_VALUE;
                    }
                    for (size_t i = 0; i < fixed; ++i) {
                        auto value = emit_expr(b, call.args[i]);
                        if (value == mir::NO_VALUE) return mir::NO_VALUE;
                        value = coerce_arg(b, value, fn_sym->params[i], expr_type(call.args[i]));
                        const auto slot = args.size();
                        args.push_back(slot < sig.params.size()
                            ? coerce_to(b, value, sig.params[slot], signed_type(expr_type(call.args[i])))
                            : value);
                    }
                    const auto tail = emit_variadic_tail(b, call, fixed, fn_sym->params.back());
                    if (tail == mir::NO_VALUE) return mir::NO_VALUE;
                    args.push_back(tail);
                    const auto result = b.call(it->second, sig.result, args);
                    return via_sret ? sret_slot : result;
                }

                const auto *callee_params = symbol_param_types(*callee_module, callee_name);
                for (size_t i = 0; i < call.args.size(); ++i) {
                    auto value = emit_expr(b, call.args[i]);
                    if (value == mir::NO_VALUE) return mir::NO_VALUE;
                    if (callee_params && i < callee_params->size()) {
                        value = coerce_arg(b, value, (*callee_params)[i], expr_type(call.args[i]));
                    }
                    const auto slot = args.size();
                    args.push_back(slot < sig.params.size()
                        ? coerce_to(b, value, sig.params[slot], signed_type(expr_type(call.args[i])))
                        : value);
                }
                // Trailing defaulted parameters, evaluated here in the callee's own context.
                if (!sig.is_variadic && args.size() < sig.params.size()) {
                    if (const auto *fn_sym = symbol_function(*callee_module, callee_name);
                        fn_sym && fn_sym->decl) {
                        const size_t implicit = via_sret ? 1 : 0;
                        while (args.size() < sig.params.size()) {
                            const size_t i = args.size() - implicit;
                            if (i >= fn_sym->decl->params.size() ||
                                !fn_sym->decl->params[i].default_value) {
                                break;
                            }
                            const auto value = emit_default_arg(
                                b, *fn_sym->decl->params[i].default_value, *callee_module, nullptr);
                            if (value == mir::NO_VALUE) return mir::NO_VALUE;
                            args.push_back(coerce_to(b, value, sig.params[args.size()],
                                                      i < fn_sym->params.size() && signed_type(fn_sym->params[i])));
                        }
                    }
                }
                if (!sig.is_variadic && args.size() != sig.params.size()) {
                    unsupported("a call with defaulted arguments", call.location);
                    return mir::NO_VALUE;
                }
                const auto result = b.call(it->second, sig.result, args);
                return via_sret ? sret_slot : result;
            }

            // Reads through an lvalue. An aggregate has no MIR value form, so its "value" IS
            // its address; a scalar is loaded.
            auto emit_load_from_address(mir::Builder &b, const ast::Expr &expr,
                                         const SourceLocation &loc, const char *what) -> mir::ValueId {
                const auto address = emit_address(b, expr);
                if (address == mir::NO_VALUE) {
                    unsupported(what, loc);
                    return mir::NO_VALUE;
                }
                const auto type = expr_type(expr);
                return is_scalar(type) ? b.load(scalar_type(type), address) : address;
            }

            // Walks to the identifier an lvalue is rooted at and marks its slot escaping.
            // '&p.field[i]' pins the whole local, not just the byte range: promote_slots
            // works per slot, and a pointer into one keeps all of it in memory.
            void mark_root_slot_escaping(const ast::Expr &expr) {
                const ast::Expr *cursor = &expr;
                while (true) {
                    if (const auto *member = std::get_if<std::unique_ptr<ast::MemberExpr>>(cursor)) {
                        cursor = &(*member)->object;
                    } else if (const auto *index = std::get_if<std::unique_ptr<ast::IndexOrInstantiateExpr>>(cursor)) {
                        cursor = &(*index)->operand;
                    } else {
                        break;
                    }
                }
                if (const auto *ident = std::get_if<ast::IdentExpr>(cursor)) {
                    if (const auto it = locals_.find(ident->name); it != locals_.end()) {
                        slots_escaping_.insert(it->second);
                    }
                }
            }

            // 'cast(value, T)'. sema already decided the conversion is legal; this only makes
            // the representations agree. A slice target builds the (data, len) header; the
            // explicit length wins over the operand's own extent for EVERY operand shape
            // (see codegen's emit_slice_cast and mirage303's ISSUES.md #6).
            auto emit_cast(mir::Builder &b, const ast::CastExpr &cast, const ast::Expr &expr) -> mir::ValueId {
                const auto target = expr_type(expr);
                if (target.kind == sema::TypeKind::Slice) {
                    return emit_slice_cast(b, cast, target);
                }
                if (cast.len_expr || !is_scalar(target)) {
                    unsupported("a cast to an aggregate type", cast.location);
                    return mir::NO_VALUE;
                }
                const auto from = expr_type(cast.value);
                const auto value = emit_expr(b, cast.value);
                if (value == mir::NO_VALUE) return mir::NO_VALUE;
                // A slice or array operand casting to a pointer hands over its data word /
                // base address, not the header's address.
                if (scalar_type(target) == mir::Ty::Ptr &&
                    (from.kind == sema::TypeKind::Slice || from.kind == sema::TypeKind::Array)) {
                    return coerce_arg(b, value, target, from);
                }
                return coerce_to(b, value, scalar_type(target), signed_type(from));
            }

            auto emit_slice_cast(mir::Builder &b, const ast::CastExpr &cast, const sema::ResolvedType &target) -> mir::ValueId {
                const auto usize = usize_ty();
                const auto from = expr_type(cast.value);

                const auto source = emit_expr(b, cast.value);
                if (source == mir::NO_VALUE) return mir::NO_VALUE;

                mir::ValueId explicit_len = mir::NO_VALUE;
                if (cast.len_expr) {
                    const auto len = emit_expr(b, *cast.len_expr);
                    if (len == mir::NO_VALUE) return mir::NO_VALUE;
                    explicit_len = coerce_to(b, len, usize, signed_type(expr_type(*cast.len_expr)));
                }

                mir::ValueId data = source;
                mir::ValueId count = explicit_len;
                if (from.kind == sema::TypeKind::Array) {
                    // The array's address IS the data pointer; its declared count is the
                    // default length.
                    if (count == mir::NO_VALUE) {
                        const auto *info = sema_.array_at(from.array_index);
                        count = b.const_int(usize, info ? static_cast<int64_t>(info->count) : 0);
                    }
                } else if (from.kind == sema::TypeKind::Slice) {
                    // A same-representation reinterpret with no explicit length passes the
                    // header through verbatim, count field included -- codegen does the same.
                    if (count == mir::NO_VALUE) return source;
                    data = b.load(mir::Ty::Ptr, source);
                } else {
                    // A bare pointer: zero-length unless told otherwise.
                    if (count == mir::NO_VALUE) count = b.const_int(usize, 0);
                }

                const auto slot = b.add_slot(pointer_bytes() * 2, pointer_bytes(), "slice");
                const auto base = b.slot_addr(slot);
                b.store(base, data);
                b.store(b.ptr_add_const(base, pointer_bytes()), count);
                return base;
            }

            // 'len(x)'. An array's length is a compile-time constant; a slice carries it in
            // the second word of its two-word representation (data pointer first).
            auto emit_len(mir::Builder &b, const ast::LenExpr &len) -> mir::ValueId {
                const auto usize = usize_ty();
                const auto operand_type = expr_type(len.operand);

                if (operand_type.kind == sema::TypeKind::Array) {
                    if (const auto *info = sema_.array_at(operand_type.array_index)) {
                        return b.const_int(usize, static_cast<int64_t>(info->count));
                    }
                } else if (operand_type.kind == sema::TypeKind::Slice) {
                    const auto address = emit_address(b, len.operand);
                    if (address != mir::NO_VALUE) {
                        return b.load(usize, b.ptr_add_const(address, pointer_bytes()));
                    }
                    // A slice temporary (a literal, a call result) has no address; its value
                    // IS the address of its two-word representation.
                    const auto value = emit_expr(b, len.operand);
                    if (value != mir::NO_VALUE) {
                        return b.load(usize, b.ptr_add_const(value, pointer_bytes()));
                    }
                }
                unsupported("'len' on this operand", len.location);
                return mir::NO_VALUE;
            }

            // A string literal is a '[]u8' — a two-word (data, length) value. The bytes go in
            // a private constant global; the slice itself is built in a slot, because an
            // aggregate has no MIR value form.
            //
            // Interned by content, matching codegen: the same literal appearing twice must
            // not produce two globals.
            auto emit_string_literal(mir::Builder &b, const std::string &text) -> mir::ValueId {
                uint32_t data_index = 0;
                if (const auto it = string_globals_.find(text); it != string_globals_.end()) {
                    data_index = it->second;
                } else {
                    mir::Global g;
                    g.name = std::format(".str.{}", string_globals_.size());
                    g.linkage = mir::Linkage::Internal;
                    g.is_constant = true;
                    g.init.assign(text.begin(), text.end());
                    // NUL-terminated so the same global can back a '*u8' passed to C, which
                    // is what codegen does and what every 'ext fn' string argument expects.
                    g.init.push_back(0);
                    g.size = static_cast<uint32_t>(g.init.size());
                    g.align = 1;
                    result_.module.globals.push_back(std::move(g));
                    data_index = static_cast<uint32_t>(result_.module.globals.size() - 1);
                    string_globals_[text] = data_index;
                }

                const auto usize = usize_ty();
                const auto slot = b.add_slot(pointer_bytes() * 2, pointer_bytes(), "str");
                const auto base = b.slot_addr(slot);
                b.store(base, b.global_addr(data_index));
                b.store(b.ptr_add_const(base, pointer_bytes()),
                         b.const_int(usize, static_cast<int64_t>(text.size())));
                return base;
            }

            [[nodiscard]] auto usize_ty() const -> mir::Ty {
                return options_.pointer_bits == 64 ? mir::Ty::I64 : mir::Ty::I32;
            }
            [[nodiscard]] auto pointer_bytes() const -> uint32_t { return options_.pointer_bits / 8; }

            // The module a namespace-qualified name refers to, e.g. the 'io' in 'io.print'.
            // An import binds a name to a module path in the symbol table; nothing else can
            // produce a Namespace-typed identifier.
            [[nodiscard]] auto namespace_target(const ast::Expr &object) const -> const std::string * {
                const auto *ident = std::get_if<ast::IdentExpr>(&object);
                if (!ident || !module_) return nullptr;
                const auto it = module_->symbols.find(ident->name);
                if (it == module_->symbols.end()) return nullptr;
                const auto *import = std::get_if<sema::ImportSymbol>(&it->second);
                return import ? &import->module_path : nullptr;
            }

            // '++' / '--'. A read-modify-write through an lvalue, which emit_address already
            // provides. A pointer steps by its pointee's size; everything else by one.
            auto emit_incr_decr(mir::Builder &b, const ast::IncrDecrExpr &incr) -> mir::ValueId {
                const auto type = lvalue_type(incr.operand);
                if (!is_scalar(type)) {
                    unsupported("'++' / '--' on this operand", incr.location);
                    return mir::NO_VALUE;
                }
                const auto address = emit_address(b, incr.operand);
                if (address == mir::NO_VALUE) {
                    unsupported("'++' / '--' on this operand", incr.location);
                    return mir::NO_VALUE;
                }

                const auto ty = scalar_type(type);
                const auto old_value = b.load(ty, address);

                int64_t step = 1;
                if (type.kind == sema::TypeKind::Pointer) {
                    if (const auto *pointee = sema_.pointee_at(type.pointee_index)) {
                        step = std::max(1u, size_of(*pointee));
                    }
                }

                mir::ValueId updated;
                if (ty == mir::Ty::Ptr) {
                    // Pointer arithmetic is byte arithmetic on an address, not an integer op.
                    updated = b.ptr_add_const(old_value, incr.is_increment ? step : -step);
                } else {
                    updated = b.binary(incr.is_increment ? mir::Op::Add : mir::Op::Sub, ty,
                                        old_value, b.const_int(ty, step));
                }
                b.store(address, updated);
                // Mirage's '++'/'--' are statements in practice; yielding the NEW value is
                // the conservative choice and matches what an assignment expression returns.
                return updated;
            }

            // A call to a monomorphized generic instance -- a direct call to the function
            // declare_generic_functions made for it; only the receiver (for a generic
            // method) needs the same address-taking a concrete method call does.
            auto emit_generic_call(mir::Builder &b, const ast::CallExpr &call, const size_t instance_idx) -> mir::ValueId {
                const auto it = generic_instance_index_.find(instance_idx);
                if (it == generic_instance_index_.end() ||
                    instance_idx >= sema_.generic_fn_instances.size()) {
                    unsupported("a call to an undeclared generic instance", call.location);
                    return mir::NO_VALUE;
                }
                if (exprs_ && exprs_->call_dropped_optional_error.contains(&call)) {
                    unsupported("a call dropping an ignorable error", call.location);
                    return mir::NO_VALUE;
                }
                const auto &instance = *sema_.generic_fn_instances[instance_idx];
                const auto &sig = result_.module.signatures[result_.module.functions[it->second].signature];

                std::vector<mir::ValueId> args;
                mir::ValueId sret_slot = mir::NO_VALUE;
                if (returns_via_sret(instance.return_types)) {
                    const auto layout = multi_return_layout(instance.return_types);
                    const auto slot = b.add_slot(layout.size, layout.align, "ret");
                    sret_slot = b.slot_addr(slot);
                    args.push_back(sret_slot);
                }

                if (instance.impl_decl) {
                    const auto *member = std::get_if<std::unique_ptr<ast::MemberExpr>>(&call.callee);
                    if (!member) {
                        unsupported("a generic method call of this form", call.location);
                        return mir::NO_VALUE;
                    }
                    const auto obj_type = expr_type((*member)->object);
                    mir::ValueId self;
                    if (obj_type.kind == sema::TypeKind::Pointer) {
                        self = emit_expr(b, (*member)->object);
                    } else {
                        mark_root_slot_escaping((*member)->object);
                        self = emit_address(b, (*member)->object);
                    }
                    if (self == mir::NO_VALUE) {
                        unsupported("a generic method call on this receiver", call.location);
                        return mir::NO_VALUE;
                    }
                    args.push_back(self);
                }

                for (size_t i = 0; i < call.args.size(); ++i) {
                    auto value = emit_expr(b, call.args[i]);
                    if (value == mir::NO_VALUE) return mir::NO_VALUE;
                    if (i < instance.param_types.size()) {
                        value = coerce_arg(b, value, instance.param_types[i], expr_type(call.args[i]));
                    }
                    const auto slot = args.size();
                    args.push_back(slot < sig.params.size()
                        ? coerce_to(b, value, sig.params[slot], signed_type(expr_type(call.args[i])))
                        : value);
                }
                // A generic's defaulted argument may reference the declaration's own generic
                // parameters ('n := size_of(K)'); materialized HERE, so the instance's
                // substitution env must be active for just these expressions, and the
                // records live in the instance's own expr tables.
                if (!sig.is_variadic && args.size() < sig.params.size()) {
                    const auto &generic_params = instance.decl
                        ? instance.decl->generic_params : *instance.generic_params_for_method;
                    const auto env = sema::build_generic_binding_env(generic_params, instance.args);
                    const sema::ActiveGenericEnvStack::PushGuard env_guard(
                        const_cast<sema::Program &>(sema_).active_generic_env_stack, &env);
                    const auto *instance_exprs = sema_.find_fn_instance_exprs(instance_idx);
                    const size_t implicit = (sret_slot != mir::NO_VALUE ? 1 : 0) + (instance.impl_decl ? 1 : 0);
                    while (args.size() < sig.params.size()) {
                        const size_t i = args.size() - implicit;
                        const ast::Expr *default_expr = nullptr;
                        if (instance.decl && i < instance.decl->params.size() &&
                            instance.decl->params[i].default_value) {
                            default_expr = &*instance.decl->params[i].default_value;
                        } else if (instance.impl_decl && i < instance.impl_decl->params.size() &&
                                    instance.impl_decl->params[i].default_value) {
                            default_expr = &*instance.impl_decl->params[i].default_value;
                        }
                        if (!default_expr) break;
                        const auto value = emit_default_arg(b, *default_expr, instance.module_path, instance_exprs);
                        if (value == mir::NO_VALUE) return mir::NO_VALUE;
                        args.push_back(coerce_to(b, value, sig.params[args.size()],
                                                  i < instance.param_types.size() &&
                                                      signed_type(instance.param_types[i])));
                    }
                }
                if (!sig.is_variadic && args.size() != sig.params.size()) {
                    unsupported("a call with defaulted arguments", call.location);
                    return mir::NO_VALUE;
                }
                const auto result = b.call(it->second, sig.result, args);
                return sret_slot != mir::NO_VALUE ? sret_slot : result;
            }

            // A '.method()' call through a dyn-handle receiver: an indirect call through
            // the handle's vtable slot at method_order_index (codegen's
            // emit_trait_handle_dispatch). The receiver may be the handle itself or a
            // '*Trait' -- sema's dispatch decision auto-derefs one pointer level, and in
            // memory form both spellings emit the SAME value: the blob's address (an
            // aggregate's value IS its address; a pointer's value is the address it holds).
            //
            // A nil-handle call is UB with no runtime check, per spec: the vtable word is
            // null and the load/call simply crashes.
            auto emit_trait_dispatch(mir::Builder &b, const ast::CallExpr &call,
                                      const sema::TraitDispatchInfo &dispatch) -> mir::ValueId {
                const auto *trait_info = sema_.trait_at(dispatch.trait_index);
                const auto *member = std::get_if<std::unique_ptr<ast::MemberExpr>>(&call.callee);
                if (!trait_info || !member || dispatch.method_order_index < 0 ||
                    static_cast<size_t>(dispatch.method_order_index) >= trait_info->methods.size()) {
                    unsupported("a trait-handle method call", call.location);
                    return mir::NO_VALUE;
                }
                if (exprs_ && exprs_->call_dropped_optional_error.contains(&call)) {
                    unsupported("a call dropping an ignorable error", call.location);
                    return mir::NO_VALUE;
                }
                const auto &trait_method = trait_info->methods[dispatch.method_order_index];

                const auto handle = emit_expr(b, (*member)->object);
                if (handle == mir::NO_VALUE) return mir::NO_VALUE;

                const auto data = b.load(mir::Ty::Ptr, handle);
                const auto vtable = b.load(mir::Ty::Ptr, b.ptr_add_const(handle, pointer_bytes()));
                const auto callee = b.load(mir::Ty::Ptr, b.ptr_add_const(vtable,
                    static_cast<int64_t>(dispatch.method_order_index) * pointer_bytes()));

                // The signature mirrors method_signature: optional sret, then self, then
                // the declared parameters.
                mir::Signature raw;
                if (returns_via_sret(trait_method.return_types)) {
                    raw.params.push_back(mir::Ty::Ptr);
                } else if (!trait_method.return_types.empty()) {
                    raw.result = scalar_type(trait_method.return_types.front());
                }
                raw.params.push_back(mir::Ty::Ptr); // self
                for (const auto &p : trait_method.params) {
                    raw.params.push_back(is_scalar(p) ? scalar_type(p) : mir::Ty::Ptr);
                }
                const auto signature = result_.module.intern_signature(std::move(raw));
                const auto &sig = result_.module.signatures[signature];

                std::vector<mir::ValueId> args;
                mir::ValueId sret_slot = mir::NO_VALUE;
                if (returns_via_sret(trait_method.return_types)) {
                    const auto layout = multi_return_layout(trait_method.return_types);
                    const auto slot = b.add_slot(layout.size, layout.align, "ret");
                    sret_slot = b.slot_addr(slot);
                    args.push_back(sret_slot);
                }
                args.push_back(data);

                for (size_t i = 0; i < call.args.size(); ++i) {
                    auto value = emit_expr(b, call.args[i]);
                    if (value == mir::NO_VALUE) return mir::NO_VALUE;
                    if (i < trait_method.params.size()) {
                        value = coerce_arg(b, value, trait_method.params[i], expr_type(call.args[i]));
                    }
                    const auto slot = args.size();
                    args.push_back(slot < sig.params.size()
                        ? coerce_to(b, value, sig.params[slot], signed_type(expr_type(call.args[i])))
                        : value);
                }
                if (args.size() != sig.params.size()) {
                    unsupported("a trait method call with defaulted arguments", call.location);
                    return mir::NO_VALUE;
                }
                const auto result = b.call_indirect(callee, signature, sig.result, args);
                return sret_slot != mir::NO_VALUE ? sret_slot : result;
            }

            // The variadic tail of a Mirage-native call, as one slice: an 'xs...' spread
            // forwards an existing slice verbatim; otherwise the trailing arguments are
            // stored into a fresh array slot and a (data, len) header is built over it
            // (codegen's emit_variadic_tail_slice). An empty tail is a zero slice.
            auto emit_variadic_tail(mir::Builder &b, const ast::CallExpr &call, const size_t fixed,
                                     const sema::ResolvedType &slice_type) -> mir::ValueId {
                const auto usize = usize_ty();
                if (call.args.size() == fixed + 1) {
                    if (const auto *spread = std::get_if<std::unique_ptr<ast::SpreadExpr>>(&call.args[fixed])) {
                        return emit_expr(b, (*spread)->operand);
                    }
                }
                const auto *info = slice_type.kind == sema::TypeKind::Slice
                    ? sema_.slice_at(slice_type.slice_index) : nullptr;
                if (!info) {
                    unsupported("a variadic call of this shape", call.location);
                    return mir::NO_VALUE;
                }
                const size_t n = call.args.size() - fixed;
                const auto header = b.add_slot(pointer_bytes() * 2, pointer_bytes(), "variadic");
                const auto header_base = b.slot_addr(header);
                if (n == 0) {
                    b.store(header_base, b.const_null());
                    b.store(b.ptr_add_const(header_base, pointer_bytes()), b.const_int(usize, 0));
                    return header_base;
                }
                const auto stride = std::max(1u, size_of(info->element_type));
                const auto backing = b.add_slot(static_cast<uint32_t>(stride * n),
                                                 std::max(1u, align_of(info->element_type)), "variadic.tmp");
                const auto base = b.slot_addr(backing);
                for (size_t i = 0; i < n; ++i) {
                    if (!store_element(b, base, static_cast<uint32_t>(i * stride),
                                        info->element_type, call.args[fixed + i])) {
                        return mir::NO_VALUE;
                    }
                }
                b.store(header_base, base);
                b.store(b.ptr_add_const(header_base, pointer_bytes()),
                         b.const_int(usize, static_cast<int64_t>(n)));
                return header_base;
            }

            // A macro call: bind each argument with the CALLER's context captured, then
            // emit the expression template under the macro's own declaring module -- its
            // nodes were type-checked there (bare imports included: the caller's alias
            // already redirected to the origin module before this ran).
            auto emit_macro_call(mir::Builder &b, const ast::CallExpr &call, const sema::MacroSymbol &macro,
                                  const std::string &macro_module, const sema::ProgramModule &macro_mod) -> mir::ValueId {
                if (!macro.decl || call.args.size() != macro.decl->params.size()) {
                    unsupported("a macro call of this form", call.location);
                    return mir::NO_VALUE;
                }

                auto saved_args = macro_args_;
                const auto outer = std::make_shared<const std::unordered_map<std::string, MacroArg>>(saved_args);
                for (size_t i = 0; i < macro.decl->params.size(); ++i) {
                    macro_args_[macro.decl->params[i].name] = MacroArg{
                        .expr = &call.args[i],
                        .module_path = module_path_,
                        .module = module_,
                        .exprs = exprs_,
                        .outer_args = outer,
                    };
                }

                const auto *saved_path = module_path_;
                const auto *saved_module = module_;
                const auto *saved_exprs = exprs_;
                module_path_ = &macro_module;
                module_ = &macro_mod;
                exprs_ = &macro_mod.exprs;
                const auto value = emit_expr(b, macro.decl->expr_template);
                module_path_ = saved_path;
                module_ = saved_module;
                exprs_ = saved_exprs;
                macro_args_ = std::move(saved_args);
                return value;
            }

            // A macro parameter reference inside a template: the ARGUMENT expression,
            // emitted back in its own captured call-site context (module, expr tables, and
            // the macro args that were active there -- nested macros restore their own).
            auto emit_macro_arg(mir::Builder &b, const MacroArg &arg) -> mir::ValueId {
                auto saved_args = std::move(macro_args_);
                const auto *saved_path = module_path_;
                const auto *saved_module = module_;
                const auto *saved_exprs = exprs_;
                module_path_ = arg.module_path;
                module_ = arg.module;
                exprs_ = arg.exprs;
                macro_args_ = arg.outer_args ? *arg.outer_args : std::unordered_map<std::string, MacroArg>{};
                const auto value = emit_expr(b, *arg.expr);
                module_path_ = saved_path;
                module_ = saved_module;
                exprs_ = saved_exprs;
                macro_args_ = std::move(saved_args);
                return value;
            }

            // 'receiver.method(args)' on a concrete type. The receiver is passed as a
            // POINTER, which is why a value receiver needs an address: taking one is what
            // makes 'self' work uniformly whether the method reads or mutates.
            //
            // Trait-handle dispatch is deliberately not handled here -- it is an indirect
            // call through a vtable slot, a different shape entirely.
            auto emit_method_call(mir::Builder &b, const ast::CallExpr &call, const ast::MemberExpr &member) -> mir::ValueId {
                auto receiver_type = expr_type(member.object);
                if (receiver_type.kind == sema::TypeKind::Trait) {
                    unsupported("a trait-handle method call", call.location);
                    return mir::NO_VALUE;
                }

                const auto *info = sema::find_method(receiver_type, member.member, sema_);
                if (!info || !info->is_resolved) {
                    unsupported(std::format("a call to method '{}'", member.member), call.location);
                    return mir::NO_VALUE;
                }
                const auto it = method_index_.find(key(info->impl_module, key_for_method(*info)));
                if (it == method_index_.end()) {
                    unsupported(std::format("a call to method '{}'", member.member), call.location);
                    return mir::NO_VALUE;
                }

                // The receiver's address. A pointer receiver already IS one; a value
                // receiver must be addressable, and taking its address pins its slot.
                mir::ValueId self;
                if (receiver_type.kind == sema::TypeKind::Pointer) {
                    self = emit_expr(b, member.object);
                } else {
                    mark_root_slot_escaping(member.object);
                    self = emit_address(b, member.object);
                }
                if (self == mir::NO_VALUE) {
                    unsupported("a method call on this receiver", call.location);
                    return mir::NO_VALUE;
                }

                if (exprs_ && exprs_->call_dropped_optional_error.contains(&call)) {
                    unsupported("a call dropping an ignorable error", call.location);
                    return mir::NO_VALUE;
                }

                const auto &sig = result_.module.signatures[result_.module.functions[it->second].signature];

                std::vector<mir::ValueId> args;
                mir::ValueId sret_slot = mir::NO_VALUE;
                if (returns_via_sret(info->return_types)) {
                    // Sized from the whole return list, not its first entry -- a
                    // multi-return method's blob is wider than its first slot.
                    const auto layout = multi_return_layout(info->return_types);
                    const auto slot = b.add_slot(layout.size, layout.align, "ret");
                    sret_slot = b.slot_addr(slot);
                    args.push_back(sret_slot);
                }
                args.push_back(self);

                for (size_t i = 0; i < call.args.size(); ++i) {
                    auto value = emit_expr(b, call.args[i]);
                    if (value == mir::NO_VALUE) return mir::NO_VALUE;
                    if (i < info->param_types.size()) {
                        value = coerce_arg(b, value, info->param_types[i], expr_type(call.args[i]));
                    }
                    const auto slot = args.size();
                    args.push_back(slot < sig.params.size()
                        ? coerce_to(b, value, sig.params[slot], signed_type(expr_type(call.args[i])))
                        : value);
                }
                if (args.size() < sig.params.size() && info->decl) {
                    const size_t implicit = (sret_slot != mir::NO_VALUE ? 1 : 0) + 1; // sret + self
                    while (args.size() < sig.params.size()) {
                        const size_t i = args.size() - implicit;
                        if (i >= info->decl->params.size() || !info->decl->params[i].default_value) break;
                        const auto value = emit_default_arg(
                            b, *info->decl->params[i].default_value, info->impl_module, nullptr);
                        if (value == mir::NO_VALUE) return mir::NO_VALUE;
                        args.push_back(coerce_to(b, value, sig.params[args.size()],
                                                  i < info->param_types.size() && signed_type(info->param_types[i])));
                    }
                }
                if (args.size() != sig.params.size()) {
                    unsupported("a method call with defaulted arguments", call.location);
                    return mir::NO_VALUE;
                }
                const auto result = b.call(it->second, sig.result, args);
                return sret_slot != mir::NO_VALUE ? sret_slot : result;
            }

            // '{.x = 1, .y = 2}' and '{1, 2, 3}'. Built into a fresh slot, because an
            // aggregate has no MIR value form -- the literal's "value" is that slot's
            // address, which is what every aggregate expression yields.
            //
            // Zero-filled first, then each provided element written at its sema-computed
            // offset. The memset is what makes an omitted field default to zero without
            // needing to know which fields were omitted.
            auto emit_braced_initializer(mir::Builder &b, const ast::BracedInitializerExpr &init,
                                          const ast::Expr &expr) -> mir::ValueId {
                const auto type = expr_type(expr);
                if (const auto *bitset = std::get_if<ast::BitsetExpr>(&init)) {
                    // A bitset literal folds to its storage integer: each member's mask is
                    // '1 << (enum value + 1)' by construction (codegen's bitset_member_mask).
                    if (type.kind == sema::TypeKind::Bitset) {
                        if (const auto *info = sema_.bitset_at(type.bitset_index)) {
                            if (const auto *members = sema_.enum_at(info->member_enum_type.enum_index)) {
                                uint64_t folded = 0;
                                for (const auto &member : bitset->members) {
                                    const auto field = std::ranges::find(members->fields, member.name,
                                                                          &sema::EnumFieldInfo::name);
                                    if (field != members->fields.end()) {
                                        folded |= uint64_t{1} << (field->value + 1);
                                    }
                                }
                                return b.const_int(scalar_type(type), static_cast<int64_t>(folded));
                            }
                        }
                    }
                    unsupported("a bitset literal of this type", bitset->location);
                    return mir::NO_VALUE;
                }
                if (is_scalar(type)) {
                    unsupported("this braced initializer", sema::get_expr_location(expr));
                    return mir::NO_VALUE;
                }
                // An UNTAGGED union literal has exactly one member field (sema enforces);
                // its value lands at offset 0 over a zeroed blob.
                if (type.kind == sema::TypeKind::Union) {
                    const auto *info = sema_.union_at(type.union_index);
                    const auto *fields = std::get_if<ast::StructExpr>(&init);
                    if (info && !info->is_tagged && fields && fields->fields.size() == 1) {
                        const auto member = std::ranges::find(info->members, fields->fields.front().name,
                                                               &sema::UnionMember::name);
                        if (member != info->members.end()) {
                            const auto slot = b.add_slot(std::max(1u, size_of(type)),
                                                          std::max(1u, align_of(type)), "union");
                            const auto base = b.slot_addr(slot);
                            b.mem_set(base, b.const_int(mir::Ty::I8, 0), b.const_int(usize_ty(), size_of(type)));
                            if (!store_element(b, base, 0, member->type, fields->fields.front().expr)) {
                                return mir::NO_VALUE;
                            }
                            return base;
                        }
                    }
                    unsupported("this braced initializer", sema::get_expr_location(expr));
                    return mir::NO_VALUE;
                }
                if (type.kind != sema::TypeKind::Struct && type.kind != sema::TypeKind::Array) {
                    unsupported("this braced initializer", sema::get_expr_location(expr));
                    return mir::NO_VALUE;
                }

                const auto slot = b.add_slot(std::max(1u, size_of(type)), std::max(1u, align_of(type)), "lit");
                const auto base = b.slot_addr(slot);
                b.mem_set(base, b.const_int(mir::Ty::I8, 0), b.const_int(usize_ty(), size_of(type)));

                bool ok = true;
                std::visit([&]<typename BV>(const BV &v) {
                    using B = std::decay_t<BV>;
                    if constexpr (std::is_same_v<B, ast::StructExpr>) {
                        ok = store_struct_fields(b, base, type.struct_index, v);
                    } else if constexpr (std::is_same_v<B, ast::ArrayExpr>) {
                        const auto *info = sema_.array_at(type.array_index);
                        if (!info) { ok = false; return; }
                        const auto stride = size_of(info->element_type);
                        const size_t plain = v.values.size() - (v.has_fill ? 1 : 0);
                        for (size_t i = 0; i < plain; ++i) {
                            if (!store_element(b, base, static_cast<uint32_t>(i) * stride,
                                                info->element_type, v.values[i])) ok = false;
                        }
                        // A trailing '...' fill: the value is evaluated ONCE and repeated
                        // across the remainder (codegen's emit_array_expr_value). A
                        // 'default'/'undefined' fill is the zero fill above.
                        if (v.has_fill && !v.values.empty()) {
                            const auto &last = v.values.back();
                            if (!std::holds_alternative<ast::DefaultExpr>(last) &&
                                !std::holds_alternative<ast::UndefinedExpr>(last)) {
                                const auto emitted = emit_expr(b, last);
                                if (emitted == mir::NO_VALUE) { ok = false; return; }
                                const bool scalar = is_scalar(info->element_type);
                                const auto value = scalar
                                    ? coerce(b, emitted, info->element_type, expr_type(last))
                                    : emitted;
                                for (size_t i = plain; i < info->count; ++i) {
                                    const auto addr = b.ptr_add_const(base, static_cast<int64_t>(i) * stride);
                                    if (scalar) {
                                        b.store(addr, value);
                                    } else {
                                        b.mem_copy(addr, value, b.const_int(usize_ty(), stride));
                                    }
                                }
                            }
                        }
                    } else if constexpr (std::is_same_v<B, ast::EmptyExpr>) {
                        // '{}' -- the zero fill above is the whole answer.
                    } else {
                        unsupported("this braced initializer", v.location);
                        ok = false;
                    }
                }, init);

                return ok ? base : mir::NO_VALUE;
            }

            // Named fields of a struct literal, written at their sema offsets over an
            // already-zeroed destination. Shared by braced initializers and tagged-variant
            // payloads, which are the same operation at different base addresses.
            auto store_struct_fields(mir::Builder &b, const mir::ValueId base, const int struct_index,
                                      const ast::StructExpr &fields) -> bool {
                const auto *info = sema_.struct_at(struct_index);
                if (!info) return false;
                bool ok = true;
                for (const auto &field : fields.fields) {
                    const auto declared = std::ranges::find(info->fields, field.name, &sema::StructField::name);
                    if (declared == info->fields.end()) { ok = false; continue; }
                    if (!store_element(b, base, declared->offset, declared->type, field.expr)) ok = false;
                }
                return ok;
            }

            // One element of a braced initializer: a scalar is stored, an aggregate is
            // copied. Returns false if the element could not be lowered (already reported).
            auto store_element(mir::Builder &b, const mir::ValueId base, const uint32_t offset,
                                const sema::ResolvedType &type, const ast::Expr &value) -> bool {
                // 'default' is what the enclosing zero fill already produced; 'undefined'
                // deliberately leaves the element unspecified. Neither needs a store.
                if (std::holds_alternative<ast::DefaultExpr>(value) ||
                    std::holds_alternative<ast::UndefinedExpr>(value)) {
                    return true;
                }
                const auto address = b.ptr_add_const(base, offset);
                const auto emitted = emit_expr(b, value);
                if (emitted == mir::NO_VALUE) return false;
                if (is_scalar(type)) {
                    b.store(address, coerce(b, emitted, type, expr_type(value)));
                } else {
                    store_aggregate_value(b, address, type, emitted, expr_type(value));
                }
                return true;
            }

            // Where a variant's payload VALUE lives relative to the union's base: the shared
            // payload offset, plus -- when the payload is a bare (non-struct) type wrapped in
            // the one-field '{v: T}' convention -- that wrapper's field offset. A struct
            // payload is the struct itself (layout_union reuses it verbatim), so nothing is
            // added. Writers (constructors, coercions) and readers (captures) must agree.
            [[nodiscard]] auto variant_payload_offset(const sema::UnionInfo &info,
                                                       const sema::TaggedUnionVariant &variant) const -> uint32_t {
                const bool struct_payload = variant.payload_type.kind == sema::TypeKind::Struct &&
                                             variant.payload_type.struct_index == variant.payload_struct_index;
                return info.payload_offset +
                       (struct_payload ? 0 : wrapper_field_offset(variant.payload_struct_index));
            }

            // A payload-free tagged-union variant value: a zeroed blob carrying just the
            // tag. NO_VALUE when 'name' is not one of the union's variants -- the caller
            // falls back to its own path or diagnostic.
            auto emit_tag_only_variant(mir::Builder &b, const sema::ResolvedType &type,
                                        const std::string &name) -> mir::ValueId {
                const auto *info = type.kind == sema::TypeKind::Union ? sema_.union_at(type.union_index) : nullptr;
                if (!info || !info->is_tagged) return mir::NO_VALUE;
                const auto variant = std::ranges::find(info->variants, name, &sema::TaggedUnionVariant::name);
                if (variant == info->variants.end()) return mir::NO_VALUE;

                const auto slot = b.add_slot(std::max(1u, size_of(type)), std::max(1u, align_of(type)), "variant");
                const auto base = b.slot_addr(slot);
                b.mem_set(base, b.const_int(mir::Ty::I8, 0), b.const_int(usize_ty(), size_of(type)));
                b.store(base, b.const_int(mir::Ty::I32, variant->tag_value));
                return base;
            }

            // '.Variant{...}' / 'Type.Variant{...}' -- a tagged union built in a slot:
            // [tag:u32 | padding | payload]. Zero-filled first so padding and omitted
            // payload fields are deterministic.
            auto emit_tagged_variant(mir::Builder &b, const ast::TaggedVariantExpr &tv,
                                      const ast::Expr &expr) -> mir::ValueId {
                const auto type = expr_type(expr);
                const auto *info = type.kind == sema::TypeKind::Union ? sema_.union_at(type.union_index) : nullptr;
                if (!info || !info->is_tagged) {
                    unsupported("a tagged-variant constructor of this type", tv.location);
                    return mir::NO_VALUE;
                }
                const auto variant = std::ranges::find(info->variants, tv.variant_name, &sema::TaggedUnionVariant::name);
                if (variant == info->variants.end()) {
                    unsupported(std::format("a '.{}' constructor of this type", tv.variant_name), tv.location);
                    return mir::NO_VALUE;
                }

                const auto slot = b.add_slot(std::max(1u, size_of(type)), std::max(1u, align_of(type)), "variant");
                const auto base = b.slot_addr(slot);
                b.mem_set(base, b.const_int(mir::Ty::I8, 0), b.const_int(usize_ty(), size_of(type)));
                b.store(base, b.const_int(mir::Ty::I32, variant->tag_value));

                if (variant->payload_struct_index >= 0) {
                    // The payload struct's fields land at payload_offset + their own sema
                    // offsets; the zero fill above covers whatever the literal omitted.
                    const auto payload_base = info->payload_offset == 0
                        ? base : b.ptr_add_const(base, info->payload_offset);
                    if (!store_struct_fields(b, payload_base, variant->payload_struct_index, tv.payload)) {
                        unsupported(std::format("a '.{}' constructor payload", tv.variant_name), tv.location);
                        return mir::NO_VALUE;
                    }
                }
                return base;
            }

            // '.Variant' -- a contextual reference whose type comes from the surrounding
            // expectation. For an enum (including a bitset's member enum) it is a compile-time
            // constant: the variant's declared value, in the enum's own storage type. For a
            // tagged union it is a payload-free variant: a blob carrying just the tag.
            auto emit_dot_ident(mir::Builder &b, const ast::DotIdentExpr &dot, const ast::Expr &expr) -> mir::ValueId {
                const auto type = expr_type(expr);
                if (type.kind == sema::TypeKind::Enum) {
                    if (const auto *info = sema_.enum_at(type.enum_index)) {
                        for (const auto &field : info->fields) {
                            if (field.name == dot.name) {
                                return b.const_int(scalar_type(type), field.value);
                            }
                        }
                    }
                }
                // A bitset-typed member reference IS its one-bit mask, so '.A in modes' and
                // 'flags += .A' need no special casing at their operators.
                if (type.kind == sema::TypeKind::Bitset) {
                    if (const auto *info = sema_.bitset_at(type.bitset_index)) {
                        if (const auto *members = sema_.enum_at(info->member_enum_type.enum_index)) {
                            const auto field = std::ranges::find(members->fields, dot.name,
                                                                  &sema::EnumFieldInfo::name);
                            if (field != members->fields.end()) {
                                return b.const_int(scalar_type(type),
                                    static_cast<int64_t>(uint64_t{1} << (field->value + 1)));
                            }
                        }
                    }
                }
                if (const auto value = emit_tag_only_variant(b, type, dot.name); value != mir::NO_VALUE) {
                    return value;
                }
                unsupported(std::format("a '.{}' reference of this type", dot.name), dot.location);
                return mir::NO_VALUE;
            }

            // 'Dir.South' / 'mod.Dir.South' -- an enum variant reached through its type
            // rather than through a value. Returns NO_VALUE when the object is a VALUE (a
            // struct whose field happens to be enum-typed), which must go through the
            // ordinary address path instead.
            auto enum_member_constant(mir::Builder &b, const ast::MemberExpr &member,
                                       const ast::Expr &expr) -> mir::ValueId {
                const auto type = expr_type(expr);
                if (type.kind != sema::TypeKind::Enum) return mir::NO_VALUE;
                if (!names_a_type(member.object)) return mir::NO_VALUE;

                if (const auto *info = sema_.enum_at(type.enum_index)) {
                    for (const auto &field : info->fields) {
                        if (field.name == member.member) {
                            return b.const_int(scalar_type(type), field.value);
                        }
                    }
                }
                return mir::NO_VALUE;
            }

            // Whether an expression names a TYPE rather than a value: a bare identifier
            // bound to a TypeSymbol, or such a name reached through a module namespace.
            [[nodiscard]] auto names_a_type(const ast::Expr &expr) const -> bool {
                if (const auto *ident = std::get_if<ast::IdentExpr>(&expr)) {
                    if (!module_) return false;
                    // A local shadows a type name, and a local is always a value.
                    if (locals_.contains(ident->name)) return false;
                    const auto it = module_->symbols.find(ident->name);
                    return it != module_->symbols.end() && std::holds_alternative<sema::TypeSymbol>(it->second);
                }
                if (const auto *member = std::get_if<std::unique_ptr<ast::MemberExpr>>(&expr)) {
                    if (const auto *target = namespace_target((*member)->object)) {
                        const auto mod = sema_.modules.find(*target);
                        if (mod == sema_.modules.end()) return false;
                        const auto it = mod->second.symbols.find((*member)->member);
                        return it != mod->second.symbols.end() &&
                               std::holds_alternative<sema::TypeSymbol>(it->second);
                    }
                }
                return false;
            }

            // The resolved type a 'size_of'/'align_of' operand names: a bare type name (or
            // 'mod.Type') has no recorded expr_type, so the symbol table answers first;
            // anything else is an expression whose recorded type is the answer (codegen's
            // resolve_operand_type).
            [[nodiscard]] auto operand_named_type(const ast::Expr &operand) const
                -> std::optional<sema::ResolvedType> {
                if (const auto *ident = std::get_if<ast::IdentExpr>(&operand)) {
                    if (module_ && !locals_.contains(ident->name)) {
                        if (const auto it = module_->symbols.find(ident->name); it != module_->symbols.end()) {
                            if (const auto *ts = std::get_if<sema::TypeSymbol>(&it->second);
                                ts && ts->resolved) {
                                return *ts->resolved;
                            }
                        }
                    }
                    // 'size_of(T)' inside a generic instantiation: 'T' is never a module
                    // symbol by now and never gets its own expr_types entry -- it lives on
                    // the active substitution env, exactly as sema resolved it (codegen's
                    // resolve_operand_type, case 2).
                    if (!sema_.active_generic_env_stack.empty()) {
                        for (const auto &binding : *sema_.active_generic_env_stack.current()) {
                            if (binding.is_type && binding.param_name == ident->name) {
                                return binding.type_value;
                            }
                        }
                    }
                }
                if (const auto *member = std::get_if<std::unique_ptr<ast::MemberExpr>>(&operand)) {
                    if (const auto *target = namespace_target((*member)->object)) {
                        if (const auto mod = sema_.modules.find(*target); mod != sema_.modules.end()) {
                            if (const auto it = mod->second.symbols.find((*member)->member);
                                it != mod->second.symbols.end()) {
                                if (const auto *ts = std::get_if<sema::TypeSymbol>(&it->second);
                                    ts && ts->resolved) {
                                    return *ts->resolved;
                                }
                            }
                        }
                    }
                }
                const auto recorded = expr_type(operand);
                if (recorded.kind != sema::TypeKind::Invalid) return recorded;
                return std::nullopt;
            }

            // The then/else/join shape TernaryExpr and a non-folded WhenExpr share. Control
            // flow, never Select: the unchosen side's effects must not run. A scalar result
            // merges through a block parameter; an aggregate through a result slot.
            auto emit_ternary_shape(mir::Builder &b, const ast::Expr &cond, const ast::Expr &then_e,
                                     const ast::Expr &else_e, const ast::Expr &expr) -> mir::ValueId {
                const auto result_type = expr_type(expr);
                const bool has_value = result_type.kind != sema::TypeKind::Void &&
                                        result_type.kind != sema::TypeKind::Invalid;
                const bool scalar_result = has_value && is_scalar(result_type);

                const auto condition = emit_condition(b, cond);
                if (condition == mir::NO_VALUE) return mir::NO_VALUE;

                const auto then_block = b.create_block("ternary.then");
                const auto else_block = b.create_block("ternary.else");
                const auto join = b.create_block("ternary.end");
                mir::ValueId result_param = mir::NO_VALUE;
                mir::ValueId result_slot = mir::NO_VALUE;
                if (scalar_result) {
                    result_param = b.add_block_param(join, scalar_type(result_type));
                } else if (has_value) {
                    const auto slot = b.add_slot(std::max(1u, size_of(result_type)),
                                                  std::max(1u, align_of(result_type)), "ternary.result");
                    result_slot = b.slot_addr(slot);
                }
                b.branch(condition, then_block, else_block);

                bool ok = true;
                const auto emit_side = [&](const mir::BlockId block, const ast::Expr &side) {
                    b.set_insert_point(block);
                    const auto value = emit_expr(b, side);
                    if (value == mir::NO_VALUE) {
                        // Already reported; the block still needs a terminator so the
                        // failure does not bury itself under malformed-module noise.
                        ok = false;
                        b.unreachable();
                        return;
                    }
                    if (b.block_is_terminated()) return;
                    if (scalar_result) {
                        b.jump(join, {coerce(b, value, result_type, expr_type(side))});
                    } else {
                        if (has_value) {
                            store_aggregate_value(b, result_slot, result_type, value, expr_type(side));
                        }
                        b.jump(join);
                    }
                };
                emit_side(then_block, then_e);
                emit_side(else_block, else_e);

                b.set_insert_point(join);
                if (!ok) return mir::NO_VALUE;
                if (scalar_result) return result_param;
                if (has_value) return result_slot;
                return b.const_int(mir::Ty::I32, 0);
            }

            // A 'when' EXPRESSION: sema folded the condition and recorded the selected
            // side; only that side is emitted, with no control flow. The (theoretical)
            // unfolded case degrades to the ternary shape, exactly as codegen's
            // emit_when_expr does.
            auto emit_when_expr(mir::Builder &b, const ast::WhenExpr &when, const ast::Expr &expr) -> mir::ValueId {
                if (exprs_) {
                    if (const auto it = exprs_->expr_when_selected.find(sema::get_expr_key(expr));
                        it != exprs_->expr_when_selected.end()) {
                        const auto &side = it->second ? when.then_expr : when.else_expr;
                        const auto value = emit_expr(b, side);
                        if (value == mir::NO_VALUE) return mir::NO_VALUE;
                        const auto result_type = expr_type(expr);
                        if (is_scalar(result_type)) {
                            return coerce(b, value, result_type, expr_type(side));
                        }
                        return coerce_arg(b, value, result_type, expr_type(side));
                    }
                }
                return emit_ternary_shape(b, when.condition, when.then_expr, when.else_expr, expr);
            }

            // 'type_of(x)' -- the operand's interned type id, a compile-time constant of
            // type 'type' (a u64). sema recorded the operand's resolved type; the id comes
            // from Program::type_ids, which sema owns and codegen only reads.
            auto emit_type_id(mir::Builder &b, const ast::Expr &expr, const SourceLocation &loc) -> mir::ValueId {
                if (const auto *recorded = exprs_ ? find_operand_type(expr) : nullptr) {
                    if (const auto it = sema_.type_ids.find(*recorded); it != sema_.type_ids.end()) {
                        return b.const_int(mir::Ty::I64, static_cast<int64_t>(it->second));
                    }
                }
                unsupported("'type_of' on this operand", loc);
                return mir::NO_VALUE;
            }

            [[nodiscard]] auto find_operand_type(const ast::Expr &expr) const -> const sema::ResolvedType * {
                const auto it = exprs_->expr_type_of_operand_type.find(sema::get_expr_key(expr));
                return it == exprs_->expr_type_of_operand_type.end() ? nullptr : &it->second;
            }

            // A call through a function-pointer VALUE. wasm's call_indirect needs the
            // signature as a type index, so MIR carries it explicitly rather than inferring
            // it from the callee -- which is why the signature is interned here even though
            // x86-64 would not need it.
            auto emit_indirect_call(mir::Builder &b, const ast::CallExpr &call) -> mir::ValueId {
                // lvalue_type, not expr_type: sema records no type for a call's CALLEE
                // identifier, so a local holding a function pointer has to be looked up in
                // the local table the same way any other read of it would be.
                const auto callee_type = lvalue_type(call.callee);
                if (callee_type.kind != sema::TypeKind::Function) {
                    unsupported("this call form", call.location);
                    return mir::NO_VALUE;
                }
                const auto *info = sema_.fn_signature_at(callee_type.fn_index);
                if (!info) {
                    unsupported("a call through a function pointer", call.location);
                    return mir::NO_VALUE;
                }
                if (exprs_ && exprs_->call_dropped_optional_error.contains(&call)) {
                    unsupported("a call dropping an ignorable error", call.location);
                    return mir::NO_VALUE;
                }

                const auto signature = signature_for(info->param_types, info->return_types, info->is_variadic);
                const auto &sig = result_.module.signatures[signature];

                const auto callee = emit_expr(b, call.callee);
                if (callee == mir::NO_VALUE) return mir::NO_VALUE;

                // An aggregate or multi-return result travels through a caller-owned sret
                // slot, exactly as in a direct call; signature_for already put the hidden
                // pointer parameter first.
                std::vector<mir::ValueId> args;
                mir::ValueId sret_slot = mir::NO_VALUE;
                if (returns_via_sret(info->return_types)) {
                    const auto layout = multi_return_layout(info->return_types);
                    const auto slot = b.add_slot(layout.size, layout.align, "ret");
                    sret_slot = b.slot_addr(slot);
                    args.push_back(sret_slot);
                }

                for (size_t i = 0; i < call.args.size(); ++i) {
                    auto value = emit_expr(b, call.args[i]);
                    if (value == mir::NO_VALUE) return mir::NO_VALUE;
                    if (i < info->param_types.size()) {
                        value = coerce_arg(b, value, info->param_types[i], expr_type(call.args[i]));
                    }
                    const auto slot = args.size();
                    args.push_back(slot < sig.params.size()
                        ? coerce_to(b, value, sig.params[slot], signed_type(expr_type(call.args[i])))
                        : value);
                }
                if (!sig.is_variadic && args.size() != sig.params.size()) {
                    unsupported("an indirect call with defaulted arguments", call.location);
                    return mir::NO_VALUE;
                }
                const auto result = b.call_indirect(callee, signature, sig.result, args);
                return sret_slot != mir::NO_VALUE ? sret_slot : result;
            }

            // 'xs[..]', 'xs[a..b]' -- builds a two-word (data, length) slice in a slot, since
            // a slice is an aggregate and has no MIR value form. Bounds are NOT checked here:
            // that is a runtime concern the LLVM backend also leaves to the operand's own
            // guarantees, and adding it silently would change semantics between backends.
            auto emit_slice_expr(mir::Builder &b, const ast::SliceExpr &slice) -> mir::ValueId {
                const auto usize = usize_ty();
                const auto operand_type = expr_type(slice.operand);

                sema::ResolvedType element{};
                mir::ValueId data = mir::NO_VALUE;
                mir::ValueId total = mir::NO_VALUE;

                if (operand_type.kind == sema::TypeKind::Array) {
                    const auto *info = sema_.array_at(operand_type.array_index);
                    if (!info) { unsupported("a slice of this operand", slice.location); return mir::NO_VALUE; }
                    element = info->element_type;
                    data = emit_address(b, slice.operand);
                    total = b.const_int(usize, static_cast<int64_t>(info->count));
                } else if (operand_type.kind == sema::TypeKind::Slice) {
                    const auto *info = sema_.slice_at(operand_type.slice_index);
                    if (!info) { unsupported("a slice of this operand", slice.location); return mir::NO_VALUE; }
                    element = info->element_type;
                    auto address = emit_address(b, slice.operand);
                    if (address == mir::NO_VALUE) address = emit_expr(b, slice.operand);
                    if (address == mir::NO_VALUE) { unsupported("a slice of this operand", slice.location); return mir::NO_VALUE; }
                    data = b.load(mir::Ty::Ptr, address);
                    total = b.load(usize, b.ptr_add_const(address, pointer_bytes()));
                } else {
                    unsupported("a slice of this operand", slice.location);
                    return mir::NO_VALUE;
                }
                if (data == mir::NO_VALUE) { unsupported("a slice of this operand", slice.location); return mir::NO_VALUE; }

                // Omitted bounds mean the whole operand: start 0, end == its length.
                mir::ValueId start = b.const_int(usize, 0);
                if (slice.start) {
                    const auto value = emit_expr(b, *slice.start);
                    if (value == mir::NO_VALUE) return mir::NO_VALUE;
                    start = coerce_to(b, value, usize, signed_type(expr_type(*slice.start)));
                }
                mir::ValueId end = total;
                if (slice.end) {
                    const auto value = emit_expr(b, *slice.end);
                    if (value == mir::NO_VALUE) return mir::NO_VALUE;
                    end = coerce_to(b, value, usize, signed_type(expr_type(*slice.end)));
                }

                const auto stride = b.const_int(usize, size_of(element));
                const auto slot = b.add_slot(pointer_bytes() * 2, pointer_bytes(), "slice");
                const auto base = b.slot_addr(slot);
                b.store(base, b.ptr_add(data, b.binary(mir::Op::Mul, usize, start, stride)));
                b.store(b.ptr_add_const(base, pointer_bytes()),
                         b.binary(mir::Op::Sub, usize, end, start));
                return base;
            }

            // ---- coercion ----------------------------------------------------------

            // 'source' is the SEMA type the value came from, needed only for its
            // signedness — see coerce_to.
            auto coerce(mir::Builder &b, const mir::ValueId value, const sema::ResolvedType &target,
                         const sema::ResolvedType &source = {}) -> mir::ValueId {
                return is_scalar(target) ? coerce_to(b, value, scalar_type(target), signed_type(source)) : value;
            }

            // Width/representation adjustment between two MIR scalars. sema already accepted
            // the conversion; this only makes the representations agree.
            //
            // 'source_is_signed' cannot be inferred from the MIR type: MIR integers are
            // sign-agnostic (I8 is eight bits, not 'i8' or 'u8'), so the source LANGUAGE
            // type decides whether a widening is a sign- or zero-extend. Defaulting it to
            // signed silently miscompiled every unsigned widening -- 'cast(u8(200), i64)'
            // produced -56.
            auto coerce_to(mir::Builder &b, const mir::ValueId value, const mir::Ty target,
                            const bool source_is_signed) -> mir::ValueId {
                const auto from = b.value_type(value);
                if (from == target || target == mir::Ty::Void) return value;

                const auto from_bits = mir::type_bits(from, options_.pointer_bits);
                const auto to_bits = mir::type_bits(target, options_.pointer_bits);

                if (mir::is_integer(from) && mir::is_integer(target)) {
                    if (to_bits < from_bits) return b.convert(mir::Op::Trunc, target, value);
                    // An I1 always zero-extends: the front end has no signed one-bit type,
                    // so 'true' must widen to 1 rather than -1.
                    const bool sign_extend = source_is_signed && from != mir::Ty::I1;
                    return b.convert(sign_extend ? mir::Op::SExt : mir::Op::ZExt, target, value);
                }
                if (mir::is_float(from) && mir::is_float(target)) {
                    return b.convert(to_bits < from_bits ? mir::Op::FPTrunc : mir::Op::FPExt, target, value);
                }
                if (mir::is_integer(from) && target == mir::Ty::Ptr) {
                    return b.convert(mir::Op::IntToPtr, target, value);
                }
                if (from == mir::Ty::Ptr && mir::is_integer(target)) {
                    return b.convert(mir::Op::PtrToInt, target, value);
                }
                return b.convert(mir::Op::Bitcast, target, value);
            }
        };
    }

    auto generate(const ast::Program &ast_program, const sema::Program &sema_program,
                   DiagnosticEngine &diag, const Options &options) -> Result {
        Generator generator(ast_program, sema_program, diag, options);
        return generator.run();
    }
}
