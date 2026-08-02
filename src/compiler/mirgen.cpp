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
                emit_function_bodies();

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
                if (returns.size() == 1) {
                    sig.result = is_scalar(returns.front()) ? scalar_type(returns.front()) : mir::Ty::Ptr;
                } else if (returns.size() > 1) {
                    // Multi-return lowers to an sret pointer parameter. Recorded as such so
                    // the signature is honest even before bodies use it.
                    sig.params.insert(sig.params.begin(), mir::Ty::Ptr);
                }
                return result_.module.intern_signature(std::move(sig));
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

                const auto entry = b.create_block("entry");
                b.set_insert_point(entry);

                // Parameters arrive as block parameters of the entry block and are
                // immediately spilled to slots, matching the memory form the front end
                // emits. promote_slots undoes this for the ones that never escape.
                const auto &sig = result_.module.signatures[result_.module.functions[fn_index].signature];
                for (size_t i = 0; i < fn.params.size() && i < sig.params.size(); ++i) {
                    const auto param_value = b.add_block_param(entry, sig.params[i]);
                    result_.module.functions[fn_index].params.push_back(param_value);

                    const auto &decl_param = fn.decl->params[i];
                    const auto slot = b.add_slot(std::max(1u, size_of(fn.params[i])),
                                                  std::max(1u, align_of(fn.params[i])), decl_param.name);
                    b.store(b.slot_addr(slot), param_value);
                    locals_[decl_param.name] = slot;
                    local_types_[decl_param.name] = fn.params[i];
                }

                emit_stmt(b, fn.decl->body, fn);

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

            void emit_stmt(mir::Builder &b, const ast::Stmt &stmt, const sema::FunctionSymbol &fn) {
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
                            emit_stmt(b, s, fn);
                            if (b.block_is_terminated()) break;
                        }
                        locals_ = saved_locals;
                        local_types_ = saved_types;

                    } else if constexpr (std::is_same_v<V, ast::VarDeclStmt>) {
                        emit_var_decl(b, v);

                    } else if constexpr (std::is_same_v<V, ast::ExprStmt>) {
                        (void) emit_expr(b, v.expr);

                    } else if constexpr (std::is_same_v<V, ast::ReturnStmt>) {
                        emit_return(b, v, fn);

                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::IfStmt>>) {
                        emit_if(b, *v, fn);

                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::WhileStmt>>) {
                        emit_while(b, *v, fn);

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

            void emit_return(mir::Builder &b, const ast::ReturnStmt &stmt, const sema::FunctionSymbol &fn) {
                if (stmt.return_values.empty()) {
                    b.ret();
                    return;
                }
                if (stmt.return_values.size() > 1 || fn.return_types.size() > 1) {
                    unsupported("a multi-return 'return'", stmt.location);
                    b.unreachable();
                    return;
                }
                if (!fn.return_types.empty() && !is_scalar(fn.return_types.front())) {
                    unsupported("returning an aggregate by value", stmt.location);
                    b.unreachable();
                    return;
                }
                const auto value = emit_expr(b, stmt.return_values.front());
                if (value == mir::NO_VALUE) {
                    b.unreachable();
                    return;
                }
                b.ret(coerce(b, value, fn.return_types.front(), expr_type(stmt.return_values.front())));
            }

            void emit_if(mir::Builder &b, const ast::IfStmt &stmt, const sema::FunctionSymbol &fn) {
                const auto cond = emit_condition(b, stmt.condition);
                if (cond == mir::NO_VALUE) return;

                const auto then_block = b.create_block("if.then");
                const auto else_block = b.create_block(stmt.else_stmt ? "if.else" : "if.end");
                // Without an 'else', the false edge IS the join, so no third block is made.
                const auto end_block = stmt.else_stmt ? b.create_block("if.end") : else_block;

                b.branch(cond, then_block, else_block);

                b.set_insert_point(then_block);
                emit_stmt(b, stmt.then_stmt, fn);
                if (!b.block_is_terminated()) b.jump(end_block);

                if (stmt.else_stmt) {
                    b.set_insert_point(else_block);
                    emit_stmt(b, *stmt.else_stmt, fn);
                    if (!b.block_is_terminated()) b.jump(end_block);
                }

                b.set_insert_point(end_block);
            }

            void emit_while(mir::Builder &b, const ast::WhileStmt &stmt, const sema::FunctionSymbol &fn) {
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
                emit_stmt(b, stmt.body, fn);
                if (!b.block_is_terminated()) b.jump(header);

                b.set_insert_point(exit);
            }

            // A condition must be I1. Integer conditions (the truthiness 'when' and 'if'
            // accept) become a non-zero comparison.
            auto emit_condition(mir::Builder &b, const ast::Expr &expr) -> mir::ValueId {
                const auto value = emit_expr(b, expr);
                if (value == mir::NO_VALUE) return mir::NO_VALUE;
                const auto ty = b.value_type(value);
                if (ty == mir::Ty::I1) return value;
                if (mir::is_integer(ty)) {
                    return b.compare(mir::Op::ICmpNe, value, b.const_int(ty, 0));
                }
                unsupported("a non-integer condition", sema::get_expr_location(expr));
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

                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::MemberExpr>> ||
                                          std::is_same_v<V, std::unique_ptr<ast::IndexOrInstantiateExpr>>) {
                        return emit_load_from_address(b, expr, loc, expr_kind_name<V>());

                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::CastExpr>>) {
                        return emit_cast(b, *v, expr);

                    } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::IncrDecrExpr>>) {
                        return emit_incr_decr(b, *v);

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
                    // integer (the truthiness form) it is "== 0".
                    if (ty == mir::Ty::I1) return b.unary(mir::Op::Not, ty, operand);
                    return b.compare(mir::Op::ICmpEq, operand, b.const_int(ty, 0));
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
                    callee_name = ident->name;
                    it = function_index_.find(key(*module_path_, callee_name));
                } else if (const auto *member = std::get_if<std::unique_ptr<ast::MemberExpr>>(&call.callee)) {
                    if (const auto *target = namespace_target((*member)->object)) {
                        callee_name = (*member)->member;
                        it = function_index_.find(key(*target, callee_name));
                    } else {
                        unsupported("a method call", call.location);
                        return mir::NO_VALUE;
                    }
                } else {
                    unsupported("a call through a function pointer", call.location);
                    return mir::NO_VALUE;
                }

                if (it == function_index_.end()) {
                    unsupported(std::format("a call to '{}'", callee_name), call.location);
                    return mir::NO_VALUE;
                }

                const auto &sig = result_.module.signatures[result_.module.functions[it->second].signature];
                std::vector<mir::ValueId> args;
                args.reserve(call.args.size());
                for (size_t i = 0; i < call.args.size(); ++i) {
                    const auto value = emit_expr(b, call.args[i]);
                    if (value == mir::NO_VALUE) return mir::NO_VALUE;
                    args.push_back(i < sig.params.size()
                        ? coerce_to(b, value, sig.params[i], signed_type(expr_type(call.args[i])))
                        : value);
                }
                if (!sig.is_variadic && args.size() != sig.params.size()) {
                    unsupported("a call with defaulted arguments", call.location);
                    return mir::NO_VALUE;
                }
                return b.call(it->second, sig.result, args);
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
