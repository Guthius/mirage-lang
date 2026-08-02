#include "mirgen.hpp"

#include <algorithm>
#include <format>
#include <ranges>
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
                emit_function_bodies();
                emit_method_bodies();

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
            // Enclosing loops, innermost last. 'continue' targets the loop's STEP block, not
            // its condition -- targeting the condition would skip the increment and spin.
            struct LoopTargets { mir::BlockId header; mir::BlockId exit; };
            std::vector<LoopTargets> loop_stack_;

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
                    const auto slot = b.add_slot(std::max(1u, size_of(info.param_types[i])),
                                                  std::max(1u, align_of(info.param_types[i])), decl_param.name);
                    b.store(b.slot_addr(slot), value);
                    locals_[decl_param.name] = slot;
                    local_types_[decl_param.name] = info.param_types[i];
                }

                emit_stmt(b, info.decl->body, info.return_types);

                for (const auto slot : slots_escaping_) {
                    b.mark_slot_escaping(slot);
                }
                if (!b.block_is_terminated()) {
                    if (info.return_types.empty()) b.ret();
                    else b.unreachable();
                }
            }

            // 'self' is a pointer to the receiver type, which is what makes 'self.field'
            // resolve through the same auto-deref path as any other pointer object.
            [[nodiscard]] auto self_pointer_type(const sema::MethodInfo &info) const -> sema::ResolvedType {
                for (size_t i = 0; i < sema_.pointer_pointees.size(); ++i) {
                    if (sema_.pointer_pointees[i] == info.self_type) {
                        return sema::ResolvedType{.kind = sema::TypeKind::Pointer,
                                                   .pointee_index = static_cast<int>(i)};
                    }
                }
                // No interned '*T' for this receiver: nothing in the program took its
                // address. Fall back to the bare type so field offsets still resolve; the
                // address form is what emit_member_address actually uses.
                return info.self_type;
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

                    const auto &decl_param = fn.decl->params[i];
                    const auto slot = b.add_slot(std::max(1u, size_of(fn.params[i])),
                                                  std::max(1u, align_of(fn.params[i])), decl_param.name);
                    b.store(b.slot_addr(slot), param_value);
                    locals_[decl_param.name] = slot;
                    local_types_[decl_param.name] = fn.params[i];
                }

                emit_stmt(b, fn.decl->body, fn.return_types);

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

            void emit_stmt(mir::Builder &b, const ast::Stmt &stmt, const std::vector<sema::ResolvedType> &returns) {
                if (b.block_is_terminated()) {
                    return;
                }
                std::visit([&]<typename T>(const T &v) {
                    using V = std::decay_t<T>;

                    if constexpr (std::is_same_v<V, std::unique_ptr<ast::BlockStmt>>) {
                        // Locals are function-scoped slots, so a block needs no scope of its
                        // own beyond restoring shadowed names.
                        const auto saved_locals = locals_;
                        const auto saved_types = local_types_;
                        for (const auto &s : v->stmts) {
                            emit_stmt(b, s, returns);
                            if (b.block_is_terminated()) break;
                        }
                        locals_ = saved_locals;
                        local_types_ = saved_types;

                    } else if constexpr (std::is_same_v<V, ast::VarDeclStmt>) {
                        emit_var_decl(b, v);

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
                            b.jump(loop_stack_.back().exit);
                        }

                    } else if constexpr (std::is_same_v<V, ast::ContinueStmt>) {
                        if (loop_stack_.empty()) {
                            unsupported("'continue' outside a loop", v.location);
                        } else {
                            b.jump(loop_stack_.back().header);
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
                else if constexpr (std::is_same_v<V, std::unique_ptr<ast::DeferStmt>>) return "'defer'";
                else if constexpr (std::is_same_v<V, std::unique_ptr<ast::WhenStmt>>) return "a 'when' statement";
                else if constexpr (std::is_same_v<V, std::unique_ptr<ast::AsmStmt>>) return "an 'asm' block";
                else if constexpr (std::is_same_v<V, ast::BreakStmt>) return "'break'";
                else if constexpr (std::is_same_v<V, ast::ContinueStmt>) return "'continue'";
                else if constexpr (std::is_same_v<V, ast::VarDeclGroupStmt>) return "a group declaration";
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
                const auto type = decl.init ? expr_type(*decl.init) : sema::ResolvedType{};
                const auto resolved = local_declared_type(decl, type);
                const auto slot = b.add_slot(std::max(1u, size_of(resolved)),
                                              std::max(1u, align_of(resolved)), decl.name);
                locals_[decl.name] = slot;
                local_types_[decl.name] = resolved;

                if (!decl.init) {
                    return;
                }
                if (!is_scalar(resolved)) {
                    // 'default' on an aggregate is a zero fill of the slot: a memset of a
                    // size sema already computed, needing no aggregate value form.
                    if (std::holds_alternative<ast::DefaultExpr>(*decl.init)) {
                        b.mem_set(b.slot_addr(slot), b.const_int(mir::Ty::I8, 0),
                                   b.const_int(usize_ty(), size_of(resolved)));
                        return;
                    }
                    // Any other aggregate initializer is a byte copy from whatever the
                    // initializer produced -- an aggregate expression's value IS its address,
                    // so this is the same operation aggregate assignment performs.
                    const auto source = emit_expr(b, *decl.init);
                    if (source == mir::NO_VALUE) {
                        // emit_expr already reported the construct it could not lower.
                        return;
                    }
                    b.mem_copy(b.slot_addr(slot), source, b.const_int(usize_ty(), size_of(resolved)));
                    return;
                }
                const auto value = emit_expr(b, *decl.init);
                if (value != mir::NO_VALUE) {
                    b.store(b.slot_addr(slot), value);
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
                if (stmt.return_values.empty()) {
                    b.ret();
                    return;
                }
                if (stmt.return_values.size() > 1 || returns.size() > 1) {
                    unsupported("a multi-return 'return'", stmt.location);
                    b.unreachable();
                    return;
                }

                const auto value = emit_expr(b, stmt.return_values.front());
                if (value == mir::NO_VALUE) {
                    b.unreachable();
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
                    b.mem_copy(sret_, value, b.const_int(usize_ty(), size_of(returns.front())));
                    b.ret();
                    return;
                }
                b.ret(coerce(b, value, returns.front(), expr_type(stmt.return_values.front())));
            }

            // 'error(...)' is a tagged union laid out as a byte blob: a u32 tag at offset 0
            // (0 = Ok, non-zero = a Failed variant) with the payload at
            // UnionInfo::payload_offset. Both halves below build it in the caller's sret
            // slot, since an error union is an aggregate and travels that way.

            // 'return_ok' / 'return_ok v'. Ok is tag 0 and carries no payload, so the zero
            // fill IS the value -- nothing else needs writing.
            void emit_return_ok(mir::Builder &b, const ast::ReturnOkStmt &stmt,
                                 const std::vector<sema::ResolvedType> &returns) {
                if (returns.empty()) {
                    unsupported("'return_ok' in a function with no error return", stmt.location);
                    b.unreachable();
                    return;
                }
                // 'return_ok a, b' fills leading value slots as well; that is multi-return,
                // which needs its own lowering.
                if (!stmt.return_values.empty() || returns.size() > 1) {
                    unsupported("'return_ok' with value slots (multi-return)", stmt.location);
                    b.unreachable();
                    return;
                }
                if (sret_ == mir::NO_VALUE) {
                    unsupported("'return_ok' from this function", stmt.location);
                    b.unreachable();
                    return;
                }
                b.mem_set(sret_, b.const_int(mir::Ty::I8, 0),
                           b.const_int(usize_ty(), size_of(returns.front())));
                b.ret();
            }

            // 'return_err .Variant'. Zero the blob, write the Failed tag, then the payload.
            void emit_return_err(mir::Builder &b, const ast::ReturnErrStmt &stmt,
                                  const std::vector<sema::ResolvedType> &returns) {
                if (returns.empty() || returns.size() > 1 || sret_ == mir::NO_VALUE) {
                    unsupported("'return_err' from this function", stmt.location);
                    b.unreachable();
                    return;
                }
                const auto &error_type = returns.front();
                const auto *info = error_type.kind == sema::TypeKind::Union
                    ? sema_.union_at(error_type.union_index) : nullptr;
                if (!info || !info->is_error_union) {
                    unsupported("'return_err' on this return type", stmt.location);
                    b.unreachable();
                    return;
                }

                b.mem_set(sret_, b.const_int(mir::Ty::I8, 0),
                           b.const_int(usize_ty(), size_of(error_type)));

                // The Failed variant's tag. Located by name rather than assumed to be 1, so
                // a change to how the wrapper is synthesized cannot silently invert the
                // Ok/Failed sense.
                int64_t failed_tag = 1;
                for (const auto &variant : info->variants) {
                    if (variant.name != "Ok") {
                        failed_tag = variant.tag_value;
                        break;
                    }
                }
                b.store(sret_, b.const_int(mir::Ty::I32, failed_tag));

                // The error member itself, at the payload offset. A scalar member (the
                // common 'enum(i32)' case) is stored; anything wider is copied.
                const auto member_type = expr_type(stmt.error_value);
                const auto value = emit_expr(b, stmt.error_value);
                if (value == mir::NO_VALUE) {
                    b.ret();
                    return;
                }
                const auto payload = b.ptr_add_const(sret_, info->payload_offset);
                if (is_scalar(member_type)) {
                    b.store(payload, value);
                } else {
                    b.mem_copy(payload, value, b.const_int(usize_ty(), size_of(member_type)));
                }
                b.ret();
            }

            // 'switch' over an integer, bool or enum operand. MIR has a Switch terminator
            // taking (value, block) pairs plus a default, which is exactly this shape.
            //
            // A tagged-union operand switches on the TAG rather than the value, and its
            // payload-capturing arms need the tagged-union work; reported for now.
            void emit_switch(mir::Builder &b, const ast::SwitchStmt &stmt,
                              const std::vector<sema::ResolvedType> &returns) {
                const auto operand_type = expr_type(stmt.operand);
                if (!is_scalar(operand_type)) {
                    unsupported("a 'switch' on a tagged union", stmt.location);
                    return;
                }
                const auto scrutinee = emit_expr(b, stmt.operand);
                if (scrutinee == mir::NO_VALUE) return;

                const auto end_block = b.create_block("switch.end");
                auto default_block = end_block;

                // Arms are laid out first so every case can name its block before the
                // terminator that references them is emitted.
                std::vector<std::pair<int64_t, mir::BlockId>> cases;
                std::vector<std::pair<mir::BlockId, const ast::Stmt *>> bodies;
                bool ok = true;

                for (size_t i = 0; i < stmt.arms.size(); ++i) {
                    const auto &arm = stmt.arms[i];
                    const auto block = b.create_block(std::format("switch.arm{}", i));
                    bodies.emplace_back(block, &arm.body);

                    std::visit([&]<typename P>(const P &pattern) {
                        using PT = std::decay_t<P>;
                        if constexpr (std::is_same_v<PT, ast::MatchExpr::DefaultPattern>) {
                            default_block = block;
                        } else if constexpr (std::is_same_v<PT, ast::MatchExpr::VariantPattern>) {
                            // '.Variant' on an enum operand is its declared value.
                            if (operand_type.kind == sema::TypeKind::Enum) {
                                if (const auto *info = sema_.enum_at(operand_type.enum_index)) {
                                    for (const auto &field : info->fields) {
                                        if (field.name == pattern.name) {
                                            cases.emplace_back(field.value, block);
                                            return;
                                        }
                                    }
                                }
                            }
                            unsupported("a 'switch' arm pattern of this kind", arm.location);
                            ok = false;
                        } else {
                            // A literal pattern: sema guaranteed it is a compile-time
                            // constant of the operand's type.
                            if (const auto value = constant_int(*pattern.expr)) {
                                cases.emplace_back(*value, block);
                            } else {
                                unsupported("a non-constant 'switch' arm pattern", arm.location);
                                ok = false;
                            }
                        }
                    }, arm.pattern);
                }
                if (!ok) return;

                b.switch_on(scrutinee, default_block, cases);

                for (const auto &[block, body] : bodies) {
                    b.set_insert_point(block);
                    emit_stmt(b, *body, returns);
                    // 'switch' arms do not fall through.
                    if (!b.block_is_terminated()) b.jump(end_block);
                }

                b.set_insert_point(end_block);
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
                }
                if (const auto *member = std::get_if<std::unique_ptr<ast::MemberExpr>>(&expr)) {
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
                auto object_type = expr_type(member.object);

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
                if (base == mir::NO_VALUE || object_type.kind != sema::TypeKind::Struct) {
                    return mir::NO_VALUE;
                }

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
                else if constexpr (std::is_same_v<V, std::unique_ptr<ast::SizeOfExpr>>) return "'size_of'";
                else if constexpr (std::is_same_v<V, std::unique_ptr<ast::AlignOfExpr>>) return "'align_of'";
                else if constexpr (std::is_same_v<V, std::unique_ptr<ast::TypeOfExpr>>) return "'type_of'";
                else if constexpr (std::is_same_v<V, std::unique_ptr<ast::TypeInfoOfExpr>>) return "'type_info_of'";
                else if constexpr (std::is_same_v<V, std::unique_ptr<ast::LenExpr>>) return "'len'";
                else if constexpr (std::is_same_v<V, std::unique_ptr<ast::SliceExpr>>) return "a slice expression";
                else if constexpr (std::is_same_v<V, std::unique_ptr<ast::MatchExpr>>) return "a 'match' expression";
                else if constexpr (std::is_same_v<V, std::unique_ptr<ast::TryExpr>>) return "'try'";
                else if constexpr (std::is_same_v<V, std::unique_ptr<ast::TernaryExpr>>) return "a ternary";
                else if constexpr (std::is_same_v<V, std::unique_ptr<ast::WhenExpr>>) return "a 'when' expression";
                else if constexpr (std::is_same_v<V, ast::DotIdentExpr>) return "a '.variant' reference";
                else if constexpr (std::is_same_v<V, std::unique_ptr<ast::TaggedVariantExpr>>) return "a tagged-variant constructor";
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
                        return emit_load_from_address(b, expr, loc, expr_kind_name<V>());

                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::IndexOrInstantiateExpr>>) {
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

                    } else if constexpr (std::is_same_v<V, ast::DotIdentExpr>) {
                        return emit_dot_ident(b, v, expr);

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
                const auto lhs = emit_expr(b, bin.lhs);
                const auto rhs = emit_expr(b, bin.rhs);
                if (lhs == mir::NO_VALUE || rhs == mir::NO_VALUE) return mir::NO_VALUE;

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

            auto emit_assign(mir::Builder &b, const ast::AssignExpr &assign) -> mir::ValueId {
                const auto target_type = lvalue_type(assign.target);
                const auto address = emit_address(b, assign.target);

                if (!is_scalar(target_type)) {
                    // An aggregate assignment is a byte copy of a size sema already computed.
                    // The source's "value" is its address, which is what every aggregate
                    // expression yields.
                    if (address == mir::NO_VALUE) {
                        unsupported("assignment to this target", assign.location);
                        return mir::NO_VALUE;
                    }
                    const auto source = emit_expr(b, assign.value);
                    if (source == mir::NO_VALUE) return mir::NO_VALUE;
                    b.mem_copy(address, source, b.const_int(usize_ty(), size_of(target_type)));
                    return address;
                }

                if (address == mir::NO_VALUE) {
                    unsupported("assignment to this target", assign.location);
                    return mir::NO_VALUE;
                }
                const auto value = emit_expr(b, assign.value);
                if (value == mir::NO_VALUE) return mir::NO_VALUE;

                const auto stored = coerce(b, value, target_type, expr_type(assign.value));
                b.store(address, stored);
                return stored;
            }

            auto emit_call(mir::Builder &b, const ast::CallExpr &call, const ast::Expr &expr) -> mir::ValueId {
                if (!module_path_) {
                    unsupported("this call form", call.location);
                    return mir::NO_VALUE;
                }

                // Two callee shapes lower here: a bare name in this module, and a
                // namespace-qualified 'mod.fn' -- which is an ordinary direct call once the
                // import binding names the target module. Method calls and calls through a
                // function pointer still need their own handling.
                auto it = function_index_.end();
                std::string callee_name;
                if (const auto *ident = std::get_if<ast::IdentExpr>(&call.callee)) {
                    // A LOCAL of function-pointer type shadows any same-named function, and
                    // is an indirect call -- 'const f: fn(i32) -> i32 = add; f(1)'.
                    if (locals_.contains(ident->name)) {
                        return emit_indirect_call(b, call);
                    }
                    callee_name = ident->name;
                    it = function_index_.find(key(*module_path_, callee_name));
                } else if (const auto *member = std::get_if<std::unique_ptr<ast::MemberExpr>>(&call.callee)) {
                    if (const auto *target = namespace_target((*member)->object)) {
                        callee_name = (*member)->member;
                        it = function_index_.find(key(*target, callee_name));
                    } else {
                        return emit_method_call(b, call, **member);
                    }
                } else {
                    return emit_indirect_call(b, call);
                }

                if (it == function_index_.end()) {
                    unsupported(std::format("a call to '{}'", callee_name), call.location);
                    return mir::NO_VALUE;
                }

                const auto &sig = result_.module.signatures[result_.module.functions[it->second].signature];

                // An aggregate result comes back through a slot the CALLER owns and passes
                // in; the call's value is that slot's address. Must agree with
                // returns_via_sret, which decided the signature.
                std::vector<mir::ValueId> args;
                mir::ValueId sret_slot = mir::NO_VALUE;
                const auto result_type = expr_type(expr);
                const bool via_sret = sig.result == mir::Ty::Void && !sig.params.empty() &&
                                       result_type.kind != sema::TypeKind::Void &&
                                       result_type.kind != sema::TypeKind::Invalid &&
                                       !is_scalar(result_type);
                if (via_sret) {
                    const auto slot = b.add_slot(std::max(1u, size_of(result_type)),
                                                  std::max(1u, align_of(result_type)), "ret");
                    sret_slot = b.slot_addr(slot);
                    args.push_back(sret_slot);
                }

                for (size_t i = 0; i < call.args.size(); ++i) {
                    const auto value = emit_expr(b, call.args[i]);
                    if (value == mir::NO_VALUE) return mir::NO_VALUE;
                    const auto slot = args.size();
                    args.push_back(slot < sig.params.size()
                        ? coerce_to(b, value, sig.params[slot], signed_type(expr_type(call.args[i])))
                        : value);
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
            // the representations agree. The three-argument slice form needs the aggregate
            // work and is reported rather than silently producing a bare pointer.
            auto emit_cast(mir::Builder &b, const ast::CastExpr &cast, const ast::Expr &expr) -> mir::ValueId {
                if (cast.len_expr) {
                    unsupported("a slice-forming 'cast'", cast.location);
                    return mir::NO_VALUE;
                }
                const auto target = expr_type(expr);
                if (!is_scalar(target)) {
                    unsupported("a cast to an aggregate type", cast.location);
                    return mir::NO_VALUE;
                }
                const auto value = emit_expr(b, cast.value);
                if (value == mir::NO_VALUE) return mir::NO_VALUE;
                return coerce_to(b, value, scalar_type(target), signed_type(expr_type(cast.value)));
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

                const auto &sig = result_.module.signatures[result_.module.functions[it->second].signature];

                std::vector<mir::ValueId> args;
                mir::ValueId sret_slot = mir::NO_VALUE;
                if (returns_via_sret(info->return_types)) {
                    const auto &ret_type = info->return_types.front();
                    const auto slot = b.add_slot(std::max(1u, size_of(ret_type)),
                                                  std::max(1u, align_of(ret_type)), "ret");
                    sret_slot = b.slot_addr(slot);
                    args.push_back(sret_slot);
                }
                args.push_back(self);

                for (size_t i = 0; i < call.args.size(); ++i) {
                    const auto value = emit_expr(b, call.args[i]);
                    if (value == mir::NO_VALUE) return mir::NO_VALUE;
                    const auto slot = args.size();
                    args.push_back(slot < sig.params.size()
                        ? coerce_to(b, value, sig.params[slot], signed_type(expr_type(call.args[i])))
                        : value);
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
                if (is_scalar(type)) {
                    // A bitset literal lowers to its storage integer, not to memory.
                    unsupported("a bitset literal", sema::get_expr_location(expr));
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
                        const auto *info = sema_.struct_at(type.struct_index);
                        if (!info) { ok = false; return; }
                        for (const auto &field : v.fields) {
                            const auto declared = std::ranges::find(info->fields, field.name, &sema::StructField::name);
                            if (declared == info->fields.end()) { ok = false; continue; }
                            if (!store_element(b, base, declared->offset, declared->type, field.expr)) ok = false;
                        }
                    } else if constexpr (std::is_same_v<B, ast::ArrayExpr>) {
                        const auto *info = sema_.array_at(type.array_index);
                        if (!info) { ok = false; return; }
                        // A trailing '...' fill repeats the last value across the remainder;
                        // that needs a loop, which is a separate increment.
                        if (v.has_fill) {
                            unsupported("an array literal with a '...' fill", v.location);
                            ok = false;
                            return;
                        }
                        const auto stride = size_of(info->element_type);
                        for (size_t i = 0; i < v.values.size(); ++i) {
                            if (!store_element(b, base, static_cast<uint32_t>(i) * stride,
                                                info->element_type, v.values[i])) ok = false;
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

            // One element of a braced initializer: a scalar is stored, an aggregate is
            // copied. Returns false if the element could not be lowered (already reported).
            auto store_element(mir::Builder &b, const mir::ValueId base, const uint32_t offset,
                                const sema::ResolvedType &type, const ast::Expr &value) -> bool {
                const auto address = b.ptr_add_const(base, offset);
                const auto emitted = emit_expr(b, value);
                if (emitted == mir::NO_VALUE) return false;
                if (is_scalar(type)) {
                    b.store(address, coerce(b, emitted, type, expr_type(value)));
                } else {
                    b.mem_copy(address, emitted, b.const_int(usize_ty(), size_of(type)));
                }
                return true;
            }

            // '.Variant' -- a contextual reference whose type comes from the surrounding
            // expectation. For an enum (including a bitset's member enum) it is a compile-time
            // constant: the variant's declared value, in the enum's own storage type.
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
                // A tagged-union variant reference is a value in a byte blob, not a scalar;
                // it belongs with the tagged-union work.
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
                if (returns_via_sret(info->return_types)) {
                    unsupported("an indirect call returning an aggregate", call.location);
                    return mir::NO_VALUE;
                }

                const auto signature = signature_for(info->param_types, info->return_types, info->is_variadic);
                const auto &sig = result_.module.signatures[signature];

                const auto callee = emit_expr(b, call.callee);
                if (callee == mir::NO_VALUE) return mir::NO_VALUE;

                std::vector<mir::ValueId> args;
                for (size_t i = 0; i < call.args.size(); ++i) {
                    const auto value = emit_expr(b, call.args[i]);
                    if (value == mir::NO_VALUE) return mir::NO_VALUE;
                    args.push_back(i < sig.params.size()
                        ? coerce_to(b, value, sig.params[i], signed_type(expr_type(call.args[i])))
                        : value);
                }
                if (!sig.is_variadic && args.size() != sig.params.size()) {
                    unsupported("an indirect call with defaulted arguments", call.location);
                    return mir::NO_VALUE;
                }
                return b.call_indirect(callee, signature, sig.result, args);
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
