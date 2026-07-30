#include "codegen.hpp"

#include <llvm/ADT/APFloat.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/ConstantFold.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Verifier.h>

#include <algorithm>
#include <cctype>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <ranges>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace codegen {
    namespace {
        struct LocalValue {
            llvm::AllocaInst *alloca = nullptr;
            sema::ResolvedType type;
            std::string type_module;
        };

        struct MacroArg {
            const ast::Expr *expr = nullptr;
            const std::string *module_path = nullptr;
            const sema::ProgramModule *module = nullptr;
            // The call site's per-node records, captured alongside its module: a macro invoked
            // from inside a generic instantiation has its ARGUMENT expressions checked into
            // that instance's tables, not the module's, so restoring the module alone would
            // look them up in the wrong place.
            const sema::ExprSideTables *exprs = nullptr;
            // The macro parameter's declared type/module, so a reference to this argument
            // inside the macro body can go through emit_value_as() and pick up whatever
            // coercion (e.g. concrete pointer -> trait handle) sema recorded for the
            // call-site expression against the declared parameter type.
            const sema::ResolvedType *param_type = nullptr;
            const std::string *param_module = nullptr;
            // The macro_args_ substitution map as it existed at the call site, before this
            // macro's own parameters were bound. Restored while evaluating 'expr' so that an
            // argument expression referencing an identifier with the same name as this macro's
            // parameter (e.g. calling 'is_alpha(c)' where the macro parameter is itself 'c')
            // resolves against the caller's scope instead of recursing into itself forever.
            std::shared_ptr<const std::unordered_map<std::string, MacroArg>> outer_args;
        };

        struct LValue {
            llvm::Value *ptr = nullptr;
            sema::ResolvedType type;
            std::string type_module;
            llvm::Type *storage_type = nullptr;
        };

        struct StructLowering {
            llvm::StructType *type = nullptr;
            std::vector<unsigned> field_indices;
        };

        // A scalar leaf field within a struct, used only for System V x86-64 eightbyte
        // classification (see classify_aggregate_eightbytes) — offset is relative to the
        // classified struct's own start, not any enclosing type.
        struct AbiLeaf {
            uint32_t offset = 0;
            uint32_t size = 0;
            bool is_float = false;
        };

        // Result of classifying a struct type for passing/returning by value across an
        // 'ext fn' boundary, per the System V x86-64 ABI: either it must be passed
        // indirectly via a hidden pointer (MEMORY class - struct larger than 16 bytes, or
        // containing a union we don't attempt to merge-classify), or it fits in one or two
        // coerced scalar/vector registers ('parts').
        struct AbiClassification {
            bool indirect = false;
            llvm::Type *raw_type = nullptr; // the existing Mirage aggregate type (struct or array)
            uint32_t raw_align = 1;
            std::vector<llvm::Type *> parts; // 1-2 entries when !indirect
        };

        struct FunctionKey {
            std::string module_path;
            std::string name;

            auto operator==(const FunctionKey &other) const -> bool {
                return module_path == other.module_path && name == other.name;
            }
        };

        struct FunctionKeyHash {
            auto operator()(const FunctionKey &key) const -> size_t {
                return std::hash<std::string>{}(key.module_path) ^ (std::hash<std::string>{}(key.name) << 1);
            }
        };

        auto report_codegen_error(DiagnosticEngine &diag, const SourceLocation &loc, std::string msg) -> void {
            diag.report_error(DiagnosticStage::Codegen, loc, std::move(msg));
        }

        auto symbol_name(std::string_view module_path, std::string_view name, bool is_entry_symbol = false) -> std::string {
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

        auto int_bits(const sema::ResolvedType &type) -> unsigned {
            switch (type.kind) {
            case sema::TypeKind::U8:
            case sema::TypeKind::I8:
            case sema::TypeKind::Bool:
                return 8;
            case sema::TypeKind::U16:
            case sema::TypeKind::I16:
                return 16;
            case sema::TypeKind::U32:
            case sema::TypeKind::I32:
                return 32;
            case sema::TypeKind::U64:
            case sema::TypeKind::I64:
            case sema::TypeKind::USize:
            case sema::TypeKind::Pointer:
            case sema::TypeKind::Anyptr:
                return 64;
            default:
                return 0;
            }
        }

        auto primitive_size(const sema::TypeKind kind) -> uint32_t {
            switch (kind) {
            case sema::TypeKind::U8:
            case sema::TypeKind::I8:
            case sema::TypeKind::Bool:
                return 1;
            case sema::TypeKind::U16:
            case sema::TypeKind::I16:
                return 2;
            case sema::TypeKind::U32:
            case sema::TypeKind::I32:
            case sema::TypeKind::F32:
                return 4;
            case sema::TypeKind::U64:
            case sema::TypeKind::I64:
            case sema::TypeKind::F64:
            case sema::TypeKind::USize:
            case sema::TypeKind::Pointer:
            case sema::TypeKind::Anyptr:
            case sema::TypeKind::Function:
            case sema::TypeKind::Type: // compile-time-unique type identifier, backed by u64
                return 8;
            case sema::TypeKind::Slice:
            case sema::TypeKind::Trait: // fat pointer: {data: anyptr, vtable: *const VTable}, 16 bytes
            case sema::TypeKind::Any:  // fat pointer: {id: type, data: anyptr}, 16 bytes
                return 16;
            default:
                return 0;
            }
        }

        auto primitive_align(const sema::TypeKind kind) -> uint32_t { return primitive_size(kind); }

        auto is_pointer_like(const sema::ResolvedType &type) -> bool {
            return type.kind == sema::TypeKind::Pointer || type.kind == sema::TypeKind::Anyptr;
        }

        class Generator {
          public:
            Generator(const ast::Program &ast_program, const sema::Program &sema_program, DiagnosticEngine &diag, const Options &options)
                : ast_program_(ast_program),
                  sema_program_(sema_program),
                  diag_(diag),
                  options_(options),
                  context_(std::make_unique<llvm::LLVMContext>()),
                  module_(std::make_unique<llvm::Module>("mirage", *context_)),
                  builder_(*context_) {}

            static auto method_fn_key(const std::string &type_name, const std::string &method_name) -> std::string {
                return type_name + "::" + method_name;
            }

            // Distinct mangling scheme for trait-impl methods, so a trait-impl method and an
            // inherent (bare-impl) method of the same name on the same type never collide in
            // functions_/FunctionKey — method_fn_key() alone (just "Type::method") cannot tell
            // them apart.
            static auto trait_method_fn_key(const std::string &type_name, const std::string &trait_name, const std::string &method_name) -> std::string {
                return type_name + "::" + trait_name + "::" + method_name;
            }

            // The correct functions_ key for any MethodInfo, whether it came from a bare
            // 'impl TYPE {}' block or an 'impl TRAIT for TYPE {}' block — see MethodInfo::trait_name's
            // doc comment in sema.hpp.
            static auto method_fn_key_for(const sema::MethodInfo &info) -> std::string {
                return info.trait_name ? trait_method_fn_key(info.type_name, *info.trait_name, info.decl->name)
                                        : method_fn_key(info.type_name, info.decl->name);
            }

            auto run() -> std::unique_ptr<llvm::Module> {
                declare_unions();
                declare_structs();
                declare_globals_and_functions();
                declare_methods();
                declare_trait_methods();
                declare_generic_functions();
                declare_vtables();
                declare_type_info_globals();
                const sema::FunctionSymbol *entry_main = nullptr;
                if (!options_.freestanding) {
                    entry_main = validate_hosted_main();
                }
                emit_global_initializers();
                emit_functions();
                emit_methods();
                emit_trait_methods();
                emit_generic_functions();
                // Generated in BOTH hosted and freestanding builds whenever at least one
                // '@init' function exists (and '--noinit' wasn't passed) — freestanding just
                // doesn't get a synthesized '_start' to call it from (unchanged below), so the
                // user's own '_start' is responsible for calling '_init' itself.
                if (!options_.noinit && !sema_program_.init_call_order.empty()) {
                    emit_init();
                }
                if (!options_.freestanding && entry_main) {
                    emit_start(*entry_main);
                }

                if (diag_.has_errors()) {
                    return nullptr;
                }

                if (llvm::verifyModule(*module_, &llvm::errs())) {
                    report_codegen_error(diag_, {}, "LLVM module verification failed");
                    return nullptr;
                }

                static std::vector<std::unique_ptr<llvm::LLVMContext>> retained_contexts;
                retained_contexts.push_back(std::move(context_));
                return std::move(module_);
            }

          private:
            const ast::Program &ast_program_;
            const sema::Program &sema_program_;
            DiagnosticEngine &diag_;
            Options options_;
            std::unique_ptr<llvm::LLVMContext> context_;
            std::unique_ptr<llvm::Module> module_;
            llvm::IRBuilder<> builder_;

            struct DeferScope {
                std::vector<const ast::DeferStmt *> defers; // emit LIFO
                bool is_loop_body = false;
            };

            const std::string *current_module_path_ = nullptr;
            const sema::ProgramModule *current_module_ = nullptr;
            // Which set of per-node records the AST under the cursor was checked into. Normally
            // 'current_module_->exprs', but a generic instantiation's body/defaults were checked
            // into that INSTANCE's own tables — see sema::ExprSideTables. Kept as a separate
            // pointer rather than derived from current_module_ precisely because the two differ
            // exactly when emitting a monomorphization, which is the bug this fixes.
            const sema::ExprSideTables *current_exprs_ = nullptr;
            llvm::Function *current_function_ = nullptr;
            std::vector<llvm::BasicBlock *> continue_targets_;
            std::vector<llvm::BasicBlock *> break_targets_;
            std::vector<sema::ResolvedType> current_returns_;
            std::vector<DeferScope> defer_scopes_;
            bool next_block_is_loop_body_ = false;
            std::unordered_map<std::string, LocalValue> locals_;
            std::unordered_map<std::string, MacroArg> macro_args_;

            std::unordered_map<std::string, llvm::GlobalVariable *> globals_;
            std::unordered_map<FunctionKey, llvm::Function *, FunctionKeyHash> functions_;
            std::unordered_map<std::string, llvm::Function *> ext_functions_;
            std::unordered_map<int, StructLowering> structs_;
            std::unordered_map<int, llvm::Type *> unions_; // union_index -> [N x i8] ArrayType
            // (trait_module, trait_name, type_module, type_name) -> the impl's vtable global,
            // a constant array of function pointers in TraitInfo::methods declaration order.
            // When the impl'd trait composes others, this array's tail additionally carries one
            // pointer per TraitInfo::component_traits entry, pointing at that component's own
            // synthesized sub-vtable in component_vtables_ below — see declare_vtables().
            std::map<std::tuple<std::string, std::string, std::string, std::string>, llvm::GlobalVariable *> vtables_;
            // (composing_trait_module, composing_trait_name, component_trait_module,
            // component_trait_name, type_module, type_name) -> a synthesized sub-vtable, shaped
            // to the component trait's own (flattened) method list, populated from the SAME
            // impl's underlying functions. Keyed by the composing trait too (not just the
            // component+type) so this stays independent from any separate standalone
            // 'impl ComponentTrait for TYPE' — see declare_vtables().
            std::map<std::tuple<std::string, std::string, std::string, std::string, std::string, std::string>, llvm::GlobalVariable *> component_vtables_;
            size_t string_counter_ = 0;
            size_t vtable_counter_ = 0;

            // 'type_info_of' support (Part 4/5): every distinct ResolvedType a '*Type_Info'
            // global has been emitted for so far (keyed by the type itself, not by
            // Program::type_ids' id, since most references — a struct field's type, a
            // pointer's pointee — are reached recursively and were never independently
            // 'type_of'/any-coerced, so have no id of their own to key on).
            std::unordered_map<sema::ResolvedType, llvm::Constant *> type_info_globals_by_type_;
            // Cycle guard: types currently being emitted, so a self-referential struct (a
            // pointer field pointing back to its own struct type) resolves to nil instead of
            // infinite recursion — see type_info_ptr_for.
            std::unordered_set<sema::ResolvedType> type_info_emitting_;
            llvm::GlobalVariable *type_info_table_global_ = nullptr;
            size_t type_info_table_size_ = 0;

            auto module_for(const std::string &path) const -> const sema::ProgramModule & {
                return sema_program_.modules.at(path);
            }

            // RAII swap of "which module is the AST being emitted written in". Emitting a
            // cross-module default argument, a struct field's default initializer, or a macro
            // body re-points the AST under the cursor at another module's nodes, and every
            // lookup keyed on the current module (llvm_type/module_for resolution, and the
            // per-module expression side tables) has to follow it. The two fields must always
            // move together: a half-swapped context reads one module's side tables while
            // resolving names against another's, which fails silently rather than loudly.
            // Replaces the hand-written save/restore pairs this file used to repeat ~15 times.
            //
            // 'path' is stored by address, so it must outlive the guard — pass a module_path
            // field on a Program-owned info struct, or a local declared BEFORE the guard (it
            // then outlives it, since destruction is reverse declaration order). Never a
            // temporary.
            class ScopedEmitModule {
              public:
                ScopedEmitModule(Generator &gen, const std::string &path)
                    : ScopedEmitModule(gen, &path, &gen.module_for(path)) {}
                // For callers already holding the ProgramModule: skips the map lookup, and is
                // required where the module was reached by pointer rather than by path.
                ScopedEmitModule(Generator &gen, const std::string &path, const sema::ProgramModule &module)
                    : ScopedEmitModule(gen, &path, &module) {}
                ScopedEmitModule(Generator &gen, const std::string *path, const sema::ProgramModule *module)
                    : gen_(gen), saved_path_(gen.current_module_path_), saved_module_(gen.current_module_),
                      saved_exprs_(gen.current_exprs_) {
                    gen_.current_module_path_ = path;
                    gen_.current_module_ = module;
                    gen_.current_exprs_ = module ? &module->exprs : nullptr;
                }
                ~ScopedEmitModule() {
                    gen_.current_module_path_ = saved_path_;
                    gen_.current_module_ = saved_module_;
                    gen_.current_exprs_ = saved_exprs_;
                }

                ScopedEmitModule(const ScopedEmitModule &) = delete;
                auto operator=(const ScopedEmitModule &) -> ScopedEmitModule & = delete;

              private:
                Generator &gen_;
                const std::string *saved_path_;
                const sema::ProgramModule *saved_module_;
                const sema::ExprSideTables *saved_exprs_;
            };

            // A monomorphized struct's field-default init exprs are shared across every
            // instantiation of that struct, so sema checked them into per-slot records
            // (check_generic_struct_field_defaults_for_program). Point lookups at those.
            // No-op for an ordinary non-generic struct. Must be called AFTER the
            // ScopedEmitModule for the struct's module — that guard's destructor undoes this.
            void use_struct_instance_exprs(int struct_index) {
                const auto *info = sema_program_.struct_at(struct_index);
                if (!info || !info->generic_instance) return;
                if (const auto *exprs = sema_program_.find_type_instance_exprs(struct_index)) {
                    current_exprs_ = exprs;
                }
            }

            // Re-points ONLY the per-node records, leaving the module context alone. Needed
            // wherever the AST being walked belongs to a generic instantiation while the
            // surrounding module is unchanged: an instance body, an instance's defaulted
            // argument (whose expression physically lives in the generic decl's module but was
            // checked per-instance), and a monomorphized struct's field initializers.
            // Must be constructed AFTER any ScopedEmitModule it is meant to override.
            class ScopedEmitExprs {
              public:
                ScopedEmitExprs(Generator &gen, const sema::ExprSideTables &exprs)
                    : gen_(gen), saved_exprs_(gen.current_exprs_) {
                    gen_.current_exprs_ = &exprs;
                }
                ~ScopedEmitExprs() { gen_.current_exprs_ = saved_exprs_; }

                ScopedEmitExprs(const ScopedEmitExprs &) = delete;
                auto operator=(const ScopedEmitExprs &) -> ScopedEmitExprs & = delete;

              private:
                Generator &gen_;
                const sema::ExprSideTables *saved_exprs_;
            };

            auto named_type_module(const std::string &start_module, const ast::NamedType &named) const -> std::string {
                std::string module_path = start_module;
                const auto *current = &named;

                while (current->member) {
                    const auto &mod = module_for(module_path);
                    const auto sym_it = mod.symbols.find(current->name);
                    if (sym_it == mod.symbols.end()) {
                        return start_module;
                    }

                    const auto *import = std::get_if<sema::ImportSymbol>(&sym_it->second);
                    if (!import) {
                        return start_module;
                    }

                    module_path = import->module_path;
                    current = current->member.get();
                }

                return module_path;
            }

            auto type_module_for_ast_type(const ast::Type &type, const std::string &context_module, const sema::ResolvedType &resolved) const -> std::string {
                if (resolved.kind != sema::TypeKind::Struct && resolved.kind != sema::TypeKind::Pointer &&
                    resolved.kind != sema::TypeKind::Array && resolved.kind != sema::TypeKind::Slice &&
                    resolved.kind != sema::TypeKind::Union) {
                    return context_module;
                }

                return std::visit(
                    [&]<typename T>(const T &v) -> std::string {
                        using V = std::decay_t<T>;

                        if constexpr (std::is_same_v<V, ast::NamedType>) {
                            return named_type_module(context_module, v);
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::PointerType>>) {
                            return type_module_for_ast_type(v->pointee, context_module, resolved);
                        } else {
                            return context_module;
                        }
                    },
                    type);
            }

            auto llvm_type_for(const sema::ResolvedType &type, const std::string &type_module) -> llvm::Type * {
                return llvm_type(
                    type.kind == sema::TypeKind::Struct || type.kind == sema::TypeKind::Array ||
                            type.kind == sema::TypeKind::Slice || type.kind == sema::TypeKind::Enum ||
                            type.kind == sema::TypeKind::Union || type.kind == sema::TypeKind::Trait ||
                            type.kind == sema::TypeKind::Bitset
                        ? type_module
                        : *current_module_path_,
                    type);
            }

            auto expr_type(const ast::Expr &expr) const -> sema::ResolvedType {
                const auto it = current_exprs_->expr_types.find(sema::get_expr_key(expr));
                if (it == current_exprs_->expr_types.end()) {
                    return sema::ResolvedType{.kind = sema::TypeKind::Invalid};
                }
                return it->second;
            }

            auto size_of(const std::string &module_path, const sema::ResolvedType &type) const -> uint32_t {
                if (type.kind == sema::TypeKind::Struct) {
                    return sema_program_.structs.at(type.struct_index).size;
                }
                if (type.kind == sema::TypeKind::Array) {
                    return sema_program_.arrays.at(type.array_index).size;
                }
                if (type.kind == sema::TypeKind::Slice) {
                    return 16;
                }
                if (type.kind == sema::TypeKind::Trait) {
                    return 16;
                }
                if (type.kind == sema::TypeKind::Enum) {
                    return primitive_size(sema_program_.enums.at(type.enum_index).underlying_type.kind);
                }
                if (type.kind == sema::TypeKind::Union) {
                    return sema_program_.unions.at(type.union_index).size;
                }
                if (type.kind == sema::TypeKind::Bitset) {
                    return primitive_size(sema_program_.bitsets.at(type.bitset_index).storage_type.kind);
                }
                return primitive_size(type.kind);
            }

            // Mirrors size_of() above exactly, substituting alignment for size — including the
            // Trait/Any override (primitive_align would wrongly forward to primitive_size's 16
            // for these two 16-byte fat pointers, which are actually only 8-byte aligned).
            auto align_of(const std::string &module_path, const sema::ResolvedType &type) const -> uint32_t {
                if (type.kind == sema::TypeKind::Struct) {
                    return sema_program_.structs.at(type.struct_index).align;
                }
                if (type.kind == sema::TypeKind::Array) {
                    return sema_program_.arrays.at(type.array_index).align;
                }
                if (type.kind == sema::TypeKind::Slice) {
                    return 8;
                }
                if (type.kind == sema::TypeKind::Trait) {
                    return 8;
                }
                if (type.kind == sema::TypeKind::Any) {
                    return 8;
                }
                if (type.kind == sema::TypeKind::Enum) {
                    return primitive_align(sema_program_.enums.at(type.enum_index).underlying_type.kind);
                }
                if (type.kind == sema::TypeKind::Union) {
                    return sema_program_.unions.at(type.union_index).align;
                }
                if (type.kind == sema::TypeKind::Bitset) {
                    return primitive_align(sema_program_.bitsets.at(type.bitset_index).storage_type.kind);
                }
                return primitive_align(type.kind);
            }

            auto llvm_type(const std::string &module_path, const sema::ResolvedType &type) -> llvm::Type * {
                switch (type.kind) {
                case sema::TypeKind::Void:    return llvm::Type::getVoidTy(*context_);
                case sema::TypeKind::Bool:    return llvm::Type::getInt1Ty(*context_);
                case sema::TypeKind::U8:
                case sema::TypeKind::I8:      return llvm::Type::getInt8Ty(*context_);
                case sema::TypeKind::U16:
                case sema::TypeKind::I16:     return llvm::Type::getInt16Ty(*context_);
                case sema::TypeKind::U32:
                case sema::TypeKind::I32:     return llvm::Type::getInt32Ty(*context_);
                case sema::TypeKind::U64:
                case sema::TypeKind::I64:
                case sema::TypeKind::USize:   return llvm::Type::getInt64Ty(*context_);
                case sema::TypeKind::F32:     return llvm::Type::getFloatTy(*context_);
                case sema::TypeKind::F64:     return llvm::Type::getDoubleTy(*context_);
                case sema::TypeKind::Pointer:
                case sema::TypeKind::Anyptr:  return llvm::PointerType::getUnqual(*context_);
                case sema::TypeKind::Struct:  return struct_lowering(type.struct_index).type;
                case sema::TypeKind::Array:
                    {
                        const auto &array = sema_program_.arrays.at(type.array_index);
                        return llvm::ArrayType::get(llvm_type(module_path, array.element_type), array.count);
                    }
                case sema::TypeKind::Slice:
                    return llvm::StructType::get(*context_, {llvm::PointerType::getUnqual(*context_), llvm::Type::getInt64Ty(*context_)});
                case sema::TypeKind::Trait:
                    // Fat pointer handle: {data: anyptr, vtable: *const VTable}. Same "literal
                    // anonymous struct, no forward-declare needed" pattern as Slice above — only
                    // the vtable globals/impl functions need declare passes, not the handle type.
                    return llvm::StructType::get(*context_, {llvm::PointerType::getUnqual(*context_), llvm::PointerType::getUnqual(*context_)});
                case sema::TypeKind::Enum:
                    {
                        const auto &enum_info = sema_program_.enums.at(type.enum_index);
                        return llvm_type(module_path, enum_info.underlying_type);
                    }
                case sema::TypeKind::Union:
                    return unions_.at(type.union_index);
                case sema::TypeKind::Bitset:
                    {
                        const auto &bitset_info = sema_program_.bitsets.at(type.bitset_index);
                        return llvm_type(module_path, bitset_info.storage_type);
                    }
                case sema::TypeKind::Function:
                    return llvm::PointerType::getUnqual(*context_);
                case sema::TypeKind::Type:
                    return llvm::Type::getInt64Ty(*context_);
                case sema::TypeKind::Any:
                    // Fat pointer: {id: type, data: anyptr} - note the field order is the
                    // opposite of Trait's {data, vtable} above, per the language spec.
                    return llvm::StructType::get(*context_, {llvm::Type::getInt64Ty(*context_), llvm::PointerType::getUnqual(*context_)});
                case sema::TypeKind::Opaque:
                    // Unreachable by construction: an unbound generic parameter only exists
                    // while eagerly checking a generic DECLARATION, and no instantiation
                    // carrying one is ever registered for emission (see the Opaque-argument
                    // guard in instantiate_generic_function). Reaching here means that guard
                    // was bypassed — report rather than silently lowering to void, which would
                    // produce a miscompile instead of a message.
                    report_codegen_error(diag_, {}, "internal error: unbound generic type parameter reached codegen");
                    return llvm::Type::getVoidTy(*context_);
                default: return llvm::Type::getVoidTy(*context_);
                }
            }

            // Bit mask for a single named member of a bitset's underlying enum:
            // '1 << (variant.value + 1)' — see BitsetInfo's bit-index formula.
            // Returns nullopt (and reports a codegen error) if 'name' isn't a member.
            auto bitset_member_mask(const sema::BitsetInfo &bitset_info, const std::string &name, const SourceLocation &loc) -> std::optional<uint64_t> {
                const auto &enum_info = sema_program_.enums.at(bitset_info.member_enum_type.enum_index);
                const auto it = std::ranges::find(enum_info.fields, name, &sema::EnumFieldInfo::name);
                if (it == enum_info.fields.end()) {
                    report_codegen_error(diag_, loc, std::format("unknown bitset member '{}'", name));
                    return std::nullopt;
                }
                return uint64_t{1} << (it->value + 1);
            }

            // Build an llvm::FunctionType for an indirect call through a sema function type.
            auto llvm_function_type(const sema::FunctionTypeInfo &sig) -> llvm::FunctionType * {
                std::vector<llvm::Type *> params;
                for (const auto &pt : sig.param_types) {
                    params.push_back(llvm_type(*current_module_path_, pt));
                }
                auto *ret = return_type(*current_module_path_, sig.return_types);
                return llvm::FunctionType::get(ret, params, sig.is_variadic);
            }

            // Dispatch a '.method()' call through an actual dyn-handle receiver (TypeKind::Trait),
            // using the (trait_index, method_order_index) sema already decided in
            // expr_trait_dispatch — never re-derived here. Not built on llvm_function_type()/
            // emit_indirect_call(), since those don't prepend a self param (they're for plain
            // fn-pointer values with no implicit self); this prepends 'ptr' manually, mirroring
            // declare_methods()'s self-param handling.
            auto emit_trait_handle_dispatch(const ast::CallExpr &call, const ast::Expr &receiver_expr, const sema::TraitDispatchInfo &dispatch) -> llvm::Value * {
                const auto *trait_info = sema_program_.trait_at(dispatch.trait_index);
                if (!trait_info || dispatch.method_order_index < 0 ||
                    static_cast<size_t>(dispatch.method_order_index) >= trait_info->methods.size()) {
                    report_codegen_error(diag_, call.location, "internal error: invalid trait dispatch info");
                    return nullptr;
                }
                const auto &trait_method = trait_info->methods[dispatch.method_order_index];

                // receiver_expr may be a *Trait (e.g. a '&child' loop variable, or a pointer
                // field) rather than the trait handle value itself — sema's dispatch decision
                // auto-derefs one pointer level (see try_trait_handle_dispatch), so codegen must
                // mirror that: load the fat-pointer struct through the pointer before extracting.
                const auto receiver_static_type = current_exprs_->expr_types.at(sema::get_expr_key(receiver_expr));
                llvm::Value *handle;
                if (receiver_static_type.kind == sema::TypeKind::Pointer) {
                    auto *handle_ptr = emit_expr(receiver_expr);
                    auto *handle_ty = llvm::StructType::get(*context_, {llvm::PointerType::getUnqual(*context_), llvm::PointerType::getUnqual(*context_)});
                    handle = builder_.CreateLoad(handle_ty, handle_ptr);
                } else {
                    handle = emit_expr(receiver_expr);
                }
                auto *data_ptr = builder_.CreateExtractValue(handle, {0});
                auto *vtable_ptr = builder_.CreateExtractValue(handle, {1});

                std::vector<llvm::Type *> param_types;
                param_types.push_back(llvm::PointerType::getUnqual(*context_)); // self
                for (const auto &pt : trait_method.params) {
                    param_types.push_back(llvm_type(*current_module_path_, pt));
                }
                auto *fn_type = llvm::FunctionType::get(return_type(*current_module_path_, trait_method.return_types), param_types, false);

                // Nil-handle call is UB with no runtime check emitted (per spec) — a nil handle's
                // vtable word is null, so this load/call simply crashes; that's intentional.
                auto *slot_ptr = builder_.CreateInBoundsGEP(llvm::PointerType::getUnqual(*context_), vtable_ptr, {builder_.getInt32(dispatch.method_order_index)});
                auto *fn_ptr = builder_.CreateLoad(llvm::PointerType::getUnqual(*context_), slot_ptr);

                std::vector<llvm::Value *> args;
                args.push_back(data_ptr);
                for (size_t i = 0; i < trait_method.params.size(); ++i) {
                    if (i < call.args.size()) {
                        args.push_back(emit_value_as(call.args[i], trait_method.params[i], *current_module_path_));
                    } else {
                        // Omitted trailing arg: the caller has no visibility into which
                        // concrete impl will be dispatched to, so the default always
                        // comes from the trait's own method signature.
                        args.push_back(emit_default_arg(*trait_method.decl->params[i].default_value,
                            trait_method.param_default_is_const[i], trait_info->module_path, trait_method.params[i]));
                    }
                }

                return builder_.CreateCall(fn_type, fn_ptr, args);
            }

            // Emit an indirect function-pointer call.
            auto emit_indirect_call(const ast::CallExpr &call, llvm::Value *fn_ptr, const sema::FunctionTypeInfo &sig) -> llvm::Value * {
                std::vector<llvm::Value *> args;
                for (size_t i = 0; i < call.args.size(); ++i) {
                    if (i < sig.param_types.size()) {
                        args.push_back(emit_value_as(call.args[i], sig.param_types[i], *current_module_path_));
                    } else {
                        args.push_back(emit_expr(call.args[i]));
                    }
                }
                return builder_.CreateCall(llvm_function_type(sig), fn_ptr, args);
            }

            auto return_type(const std::string &module_path, const std::vector<sema::ResolvedType> &returns) -> llvm::Type * {
                if (returns.empty()) {
                    return llvm::Type::getVoidTy(*context_);
                }
                if (returns.size() == 1) {
                    return llvm_type(module_path, returns.front());
                }

                std::vector<llvm::Type *> elements;
                for (const auto &ret : returns) {
                    elements.push_back(llvm_type(module_path, ret));
                }
                return llvm::StructType::get(*context_, elements, false);
            }

            auto struct_lowering(int index) -> StructLowering & {
                return structs_.at(index);
            }

            void declare_structs() {
                for (size_t i = 0; i < sema_program_.structs.size(); ++i) {
                    const auto &info = sema_program_.structs[i];
                    structs_[static_cast<int>(i)].type = llvm::StructType::create(
                        *context_, symbol_name(info.module_path, std::format("struct_{}", i)));
                }

                for (size_t i = 0; i < sema_program_.structs.size(); ++i) {
                    const auto &info = sema_program_.structs[i];
                    // A template placeholder built purely so the eager pass could check a
                    // generic type's impl methods (see is_opaque_template_instance). Its
                    // parameter-dependent fields have no layout, and no emitted code can
                    // reference it. The opaque named StructType was still created above, so
                    // anything that incidentally looks up its lowering finds an empty body
                    // rather than throwing.
                    if (sema::is_opaque_template_instance(info.generic_instance, sema_program_)) continue;
                    const auto &path = info.module_path;
                    std::vector<llvm::Type *> elements;
                    uint32_t cursor = 0;
                    auto &lowering = struct_lowering(static_cast<int>(i));

                    for (const auto &field : info.fields) {
                        if (field.offset > cursor) {
                            elements.push_back(llvm::ArrayType::get(llvm::Type::getInt8Ty(*context_), field.offset - cursor));
                        }
                        lowering.field_indices.push_back(static_cast<unsigned>(elements.size()));
                        elements.push_back(llvm_type(path, field.type));
                        cursor = field.offset + size_of(path, field.type);
                    }

                    if (info.size > cursor) {
                        elements.push_back(llvm::ArrayType::get(llvm::Type::getInt8Ty(*context_), info.size - cursor));
                    }

                    lowering.type->setBody(elements, true);
                }
            }

            // Recursively flattens 'type' (placed at 'base_offset' bytes into whatever it's
            // nested inside) into scalar leaves for System V x86-64 eightbyte classification.
            // Unions aren't merge-classified (SysV's real rule for them is more involved than
            // any struct in these bindings needs) - encountering one just sets 'saw_union' so
            // the caller falls back to passing the whole aggregate indirectly, which is always
            // ABI-legal, just occasionally more conservative than necessary.
            void collect_abi_leaves(const sema::ResolvedType &type, uint32_t base_offset, std::vector<AbiLeaf> &out, bool &saw_union) {
                switch (type.kind) {
                case sema::TypeKind::Struct:
                    for (const auto &field : sema_program_.structs.at(type.struct_index).fields) {
                        collect_abi_leaves(field.type, base_offset + field.offset, out, saw_union);
                    }
                    return;
                case sema::TypeKind::Array:
                    {
                        const auto &array = sema_program_.arrays.at(type.array_index);
                        const uint32_t elem_size = size_of(*current_module_path_, array.element_type);
                        for (uint64_t i = 0; i < array.count; ++i) {
                            collect_abi_leaves(array.element_type, base_offset + static_cast<uint32_t>(i) * elem_size, out, saw_union);
                        }
                    }
                    return;
                case sema::TypeKind::Union:
                    saw_union = true;
                    return;
                case sema::TypeKind::Slice:
                case sema::TypeKind::Trait:
                    // Fat pointer: {data: anyptr, vtable/len: i64}, two non-float eightbytes.
                    out.push_back({.offset = base_offset, .size = 8, .is_float = false});
                    out.push_back({.offset = base_offset + 8, .size = 8, .is_float = false});
                    return;
                case sema::TypeKind::Enum:
                    collect_abi_leaves(sema_program_.enums.at(type.enum_index).underlying_type, base_offset, out, saw_union);
                    return;
                case sema::TypeKind::Bitset:
                    collect_abi_leaves(sema_program_.bitsets.at(type.bitset_index).storage_type, base_offset, out, saw_union);
                    return;
                default:
                    {
                        const uint32_t size = primitive_size(type.kind);
                        if (size == 0) {
                            return;
                        }
                        out.push_back({.offset = base_offset, .size = size, .is_float = type.kind == sema::TypeKind::F32 || type.kind == sema::TypeKind::F64});
                    }
                    return;
                }
            }

            // Classifies a struct type per the System V x86-64 ABI's eightbyte rules, for
            // passing/returning it by value across an 'ext fn' boundary. Real C functions
            // (raylib, libc, ...) expect small structs to arrive coerced into packed
            // integer/SSE registers rather than as a raw LLVM aggregate - without this, LLVM's
            // default lowering of a directly-passed aggregate splits it into one leaf register
            // per field, which doesn't match what the real callee reads. See the plan doc for
            // the worked Color/Vector3/Rectangle examples this produces.
            // SysV x86-64 eightbyte classification for any by-value aggregate crossing an
            // 'ext fn' boundary, not just a struct. A bare '[2]f32' parameter is the same eight
            // bytes as a 'struct { f32 x, y; }' and clang lowers both to <2 x float>, but only
            // the struct was being coerced -- the array was handed to LLVM raw, whose default
            // aggregate lowering does not follow the ABI. collect_abi_leaves already walked
            // arrays and unions; only this entry point was struct-shaped.
            auto classify_aggregate_eightbytes(const sema::ResolvedType &type, const std::string &module_path) -> AbiClassification {
                AbiClassification cls;
                cls.raw_type = llvm_type(module_path, type);
                const uint32_t size = size_of(module_path, type);
                cls.raw_align = std::max<uint32_t>(1, align_of(module_path, type));

                // A MEMORY-class argument is passed on the stack at 8-byte alignment minimum,
                // regardless of the type's own alignment, so a packed struct passed byval must
                // still be declared 'align 8' -- which is what clang emits for the equivalent C
                // type. Declaring the natural alignment (1, for packed) would let LLVM place the
                // copy at an offset the callee does not expect.
                const auto mark_indirect = [&] {
                    cls.indirect = true;
                    cls.raw_align = std::max<uint32_t>(cls.raw_align, 8);
                    return cls;
                };

                if (size == 0 || size > 16) {
                    return mark_indirect();
                }

                std::vector<AbiLeaf> leaves;
                bool saw_union = false;
                collect_abi_leaves(type, 0, leaves, saw_union);
                if (saw_union) {
                    // A union's active member is not knowable statically, so SysV's merge rules
                    // cannot be applied; MEMORY class is the safe answer.
                    return mark_indirect();
                }

                // A field crossing an eightbyte boundary forces the whole aggregate to MEMORY
                // class. Only a packed struct can produce one -- natural alignment guarantees a
                // scalar sits wholly inside one eightbyte -- and the per-slot loop below would
                // otherwise classify the two halves of the straddling field independently and
                // conclude it can travel in registers.
                //
                // Verified against clang for 'struct(packed) { a: u8, b: u64 }': clang passes it
                // as 'ptr byval(...)', while this classifier previously produced '{ i64, i8 }',
                // i.e. two registers. Nothing on the C side would have read the argument
                // correctly.
                for (const auto &leaf : leaves) {
                    if (leaf.size == 0) continue;
                    if (leaf.offset / 8 != (leaf.offset + leaf.size - 1) / 8) {
                        return mark_indirect();
                    }
                }

                const uint32_t info_size = size;
                const uint32_t num_slots = (info_size + 7) / 8;
                for (uint32_t slot = 0; slot < num_slots; ++slot) {
                    const uint32_t slot_start = slot * 8;
                    const uint32_t slot_end = std::min(slot_start + 8, info_size);
                    const uint32_t n = slot_end - slot_start;

                    bool any_leaf = false;
                    bool all_float = true;
                    uint32_t float_bytes = 0;
                    bool has_eightbyte_float_leaf = false;
                    for (const auto &leaf : leaves) {
                        const uint32_t overlap_start = std::max(leaf.offset, slot_start);
                        const uint32_t overlap_end = std::min(leaf.offset + leaf.size, slot_end);
                        if (overlap_start >= overlap_end) {
                            continue;
                        }
                        any_leaf = true;
                        if (!leaf.is_float) {
                            all_float = false;
                        } else {
                            float_bytes += overlap_end - overlap_start;
                            if (leaf.size == 8) {
                                has_eightbyte_float_leaf = true;
                            }
                        }
                    }

                    if (any_leaf && all_float && n == 8 && float_bytes == 8) {
                        cls.parts.push_back(has_eightbyte_float_leaf
                                                 ? static_cast<llvm::Type *>(llvm::Type::getDoubleTy(*context_))
                                                 : static_cast<llvm::Type *>(llvm::FixedVectorType::get(llvm::Type::getFloatTy(*context_), 2)));
                    } else if (any_leaf && all_float && n == 4 && float_bytes == 4) {
                        cls.parts.push_back(llvm::Type::getFloatTy(*context_));
                    } else {
                        // INTEGER class (or a mixed/odd-shaped slot we don't try to represent
                        // as float) - coerce to the exact-width integer covering this slot.
                        cls.parts.push_back(llvm::IntegerType::get(*context_, n * 8));
                    }
                }

                return cls;
            }

            // The number of pointer slots in a trait's vtable: one per declared method, plus one
            // trailing back-pointer per component trait (trait composition). Sizing the array
            // type and filling its initializer used to recompute this formula independently; a
            // change to one and not the other would silently desync the declared length from the
            // element count, corrupting every method-slot and back-pointer index that
            // emit_trait_handle_dispatch / emit_trait_handle_coercion compute.
            static auto vtable_slot_count(const sema::TraitInfo &info) -> size_t {
                return info.methods.size() + info.component_traits.size();
            }

            void declare_unions() {
                for (size_t i = 0; i < sema_program_.unions.size(); ++i) {
                    const auto &info = sema_program_.unions[i];
                    unions_[static_cast<int>(i)] = llvm::ArrayType::get(llvm::Type::getInt8Ty(*context_), info.size);
                }
            }

            auto function_type(const std::string &module_path, const std::vector<sema::ResolvedType> &params, const std::vector<sema::ResolvedType> &returns, const bool is_vararg = false) -> llvm::FunctionType * {
                std::vector<llvm::Type *> param_types;
                for (const auto &param : params) {
                    param_types.push_back(llvm_type(module_path, param));
                }
                return llvm::FunctionType::get(return_type(module_path, returns), param_types, is_vararg);
            }

            // Like llvm_type(), but for a struct crossing an 'ext fn' boundary: returns the
            // System V x86-64 ABI-coerced representation instead of the raw Mirage struct
            // type (a real C callee never sees the latter directly). Non-struct types are
            // unaffected.
            // Which by-value types need SysV eightbyte coercion at an 'ext fn' boundary.
            // Bitset and Enum are deliberately absent: both already lower to their storage
            // integer, so they are scalars by the time they get here and need no coercion.
            static auto needs_ext_abi_coercion(const sema::ResolvedType &type) -> bool {
                return type.kind == sema::TypeKind::Struct || type.kind == sema::TypeKind::Array ||
                       type.kind == sema::TypeKind::Union;
            }

            auto ext_abi_param_type(const sema::ResolvedType &type, const std::string &module_path) -> llvm::Type * {
                if (!needs_ext_abi_coercion(type)) {
                    return llvm_type(module_path, type);
                }
                const auto cls = classify_aggregate_eightbytes(type, module_path);
                if (cls.indirect) {
                    return llvm::PointerType::getUnqual(*context_);
                }
                if (cls.parts.size() == 1) {
                    return cls.parts[0];
                }
                return llvm::StructType::get(*context_, cls.parts, false);
            }

            // Builds the llvm::FunctionType for an 'ext fn' declaration, ABI-coercing any
            // struct-by-value parameter or return type. Mirror this file's function_type()
            // for everything else - kept separate (rather than adding a flag to
            // function_type()) since internal Mirage-to-Mirage calls must keep passing raw
            // aggregates: the compiler controls both sides of those calls, so they're
            // self-consistent regardless of real C ABI rules, and coercing them would only
            // add risk with no benefit.
            auto ext_function_type(const std::string &module_path, const std::vector<sema::ResolvedType> &params, const std::optional<sema::ResolvedType> &ret, const bool is_vararg) -> llvm::FunctionType * {
                std::vector<llvm::Type *> param_types;
                llvm::Type *ret_type = llvm::Type::getVoidTy(*context_);

                if (ret) {
                    if (needs_ext_abi_coercion(*ret)) {
                        const auto cls = classify_aggregate_eightbytes(*ret, module_path);
                        if (cls.indirect) {
                            param_types.push_back(llvm::PointerType::getUnqual(*context_)); // sret
                        } else if (cls.parts.size() == 1) {
                            ret_type = cls.parts[0];
                        } else {
                            ret_type = llvm::StructType::get(*context_, cls.parts, false);
                        }
                    } else {
                        ret_type = llvm_type(module_path, *ret);
                    }
                }

                for (const auto &param : params) {
                    param_types.push_back(ext_abi_param_type(param, module_path));
                }

                return llvm::FunctionType::get(ret_type, param_types, is_vararg);
            }

            // Maps a declaration attribute name to the plain LLVM function attribute it
            // corresponds to. 'section' and 'init' aren't ordinary FnAttrs — 'section' calls
            // llvm::Function::setSection directly (see declare_globals_and_functions below),
            // and 'init' attaches no per-function attribute at all (it only affects which
            // functions the synthesized '_init' calls — see emit_init).
            static auto attribute_kind_for_name(const std::string_view name) -> std::optional<llvm::Attribute::AttrKind> {
                if (name == "no_return") return llvm::Attribute::NoReturn;
                if (name == "always_inline") return llvm::Attribute::AlwaysInline;
                if (name == "naked") return llvm::Attribute::Naked;
                return std::nullopt;
            }

            // Applies the codegen-visible subset of a declaration's attributes to an
            // already-created llvm::Function. Shared by declare_globals_and_functions (free
            // functions), declare_methods (bare-impl methods), and declare_trait_methods
            // (trait-impl methods) — 'module_path' is the only call-site-specific piece,
            // needed to constant-fold '@section's argument in the right module context.
            void apply_function_attributes(llvm::Function *fn, const std::vector<ast::Attribute> &attrs, const std::string &module_path) {
                for (const auto &attr : attrs) {
                    if (const auto kind = attribute_kind_for_name(attr.name)) {
                        fn->addFnAttr(*kind);
                    } else if (attr.name == "section" && attr.args.size() == 1) {
                        // Re-folds the already-sema-validated constant '[]u8' argument (sema
                        // only validated it, it didn't stash the folded string anywhere) —
                        // evaluate_const_value is a pure fold with no side effects for a plain
                        // string literal, the only shape sema accepted here.
                        if (const auto folded = sema::evaluate_const_value(attr.args[0], module_path, const_cast<sema::Program &>(sema_program_), diag_)) {
                            if (const auto *str = std::get_if<std::string>(&*folded)) {
                                fn->setSection(*str);
                            }
                        }
                    }
                }
            }

            void declare_globals_and_functions() {
                for (const auto &[path, mod] : sema_program_.modules) {
                    for (const auto &[name, sym] : mod.symbols) {
                        // A bare-import alias's GlobalSymbol/FunctionSymbol shares its
                        // underlying decl (and, for a function, its already-checked body)
                        // with the origin — declaring/emitting it again here would create a
                        // SECOND llvm::GlobalVariable (two addresses for one 'mut' global)
                        // or a SECOND llvm::Function whose body emission (emit_functions,
                        // below) would read 'expr_types' that were only ever populated under
                        // the origin's own ProgramModule. The second pass below (after this
                        // loop) instead registers the origin's already-created LLVM value
                        // under the importer's own key. ExtFunctionSymbol aliases need no
                        // such guard: 'ext fn's are already deduplicated process-globally by
                        // their raw C name via 'module_->getFunction()' below, so an alias
                        // naturally converges on the same llvm::Function with no special-casing.
                        if ((std::holds_alternative<sema::GlobalSymbol>(sym) || std::holds_alternative<sema::FunctionSymbol>(sym)) &&
                            mod.bare_import_origins.contains(name)) {
                            continue;
                        }
                        if (const auto *global = std::get_if<sema::GlobalSymbol>(&sym)) {
                            const auto gname = symbol_name(path, name);
                            globals_[path + "\n" + name] = new llvm::GlobalVariable(
                                *module_, llvm_type(path, global->type), !global->is_mut,
                                global->is_pub ? llvm::GlobalValue::ExternalLinkage : llvm::GlobalValue::InternalLinkage,
                                nullptr, gname);
                        } else if (const auto *fn = std::get_if<sema::FunctionSymbol>(&sym)) {
                            // A generic function has no single "the" signature to declare here
                            // — 'fn->params'/'fn->return_types' were never resolved for it (see
                            // sema.cpp's resolve_signatures_for_module skip); each concrete
                            // instantiation gets its own llvm::Function via
                            // declare_generic_functions() instead.
                            if (fn->decl && !fn->decl->generic_params.empty()) continue;
                            const bool entry_symbol = path == ast_program_.root_module_path && (name == "main" || (options_.freestanding && name == "_start"));
                            const auto fname = symbol_name(path, name, entry_symbol);
                            auto *llvm_fn = llvm::Function::Create(
                                function_type(path, fn->params, fn->return_types),
                                fn->is_pub || entry_symbol ? llvm::GlobalValue::ExternalLinkage : llvm::GlobalValue::InternalLinkage,
                                fname, *module_);
                            functions_[FunctionKey{path, name}] = llvm_fn;

                            if (fn->decl) {
                                apply_function_attributes(llvm_fn, fn->decl->attributes, path);
                            }
                        } else if (const auto *ef = std::get_if<sema::ExtFunctionSymbol>(&sym)) {
                            // External C symbols are process-global, not module-scoped: if another
                            // module already declared the same name, reuse that declaration instead
                            // of creating a second one (LLVM would otherwise rename it to `name.N`,
                            // leaving the real symbol unresolved at link time).
                            auto *llvm_fn = module_->getFunction(name);
                            if (!llvm_fn) {
                                llvm_fn = llvm::Function::Create(
                                    ext_function_type(path, ef->params, ef->return_type, ef->is_variadic),
                                    llvm::GlobalValue::ExternalLinkage,
                                    name, *module_);

                                unsigned param_idx = 0;
                                if (ef->return_type && needs_ext_abi_coercion(*ef->return_type)) {
                                    const auto cls = classify_aggregate_eightbytes(*ef->return_type, path);
                                    if (cls.indirect) {
                                        llvm_fn->addParamAttr(0, llvm::Attribute::getWithStructRetType(*context_, cls.raw_type));
                                        llvm_fn->addParamAttr(0, llvm::Attribute::getWithAlignment(*context_, llvm::Align(cls.raw_align)));
                                        param_idx = 1;
                                    }
                                }
                                for (const auto &param : ef->params) {
                                    if (needs_ext_abi_coercion(param)) {
                                        const auto cls = classify_aggregate_eightbytes(param, path);
                                        if (cls.indirect) {
                                            llvm_fn->addParamAttr(param_idx, llvm::Attribute::getWithByValType(*context_, cls.raw_type));
                                            llvm_fn->addParamAttr(param_idx, llvm::Attribute::getWithAlignment(*context_, llvm::Align(cls.raw_align)));
                                        }
                                    }
                                    ++param_idx;
                                }
                            }
                            ext_functions_[path + "\n" + name] = llvm_fn;
                        }
                    }
                }

                // Pass 2: register bare-import aliases' llvm::GlobalVariable*/Function* —
                // created above under the ORIGIN's own key — a second time under each
                // IMPORTING module's key too. Every unqualified use site inside an importer's
                // own function bodies (emit_call's/emit_lvalue's plain-IdentExpr cases) looks
                // its callee/lvalue up purely as FunctionKey{*current_module_path_, name} /
                // global_key(*current_module_path_, name) — i.e. keyed off the CALL SITE'S OWN
                // module, never off any "true origin" concept — so without this, those lookups
                // would throw for any reference to a bare-imported fn/global.
                for (const auto &[path, mod] : sema_program_.modules) {
                    for (const auto &[alias_name, origin] : mod.bare_import_origins) {
                        const auto &origin_path = origin.module_path;
                        const auto &origin_name = origin.symbol_name;
                        const auto &origin_sym = sema_program_.modules.at(origin_path).symbols.at(origin_name);
                        if (std::holds_alternative<sema::GlobalSymbol>(origin_sym)) {
                            globals_[global_key(path, alias_name)] = globals_.at(global_key(origin_path, origin_name));
                        } else if (const auto *ofn = std::get_if<sema::FunctionSymbol>(&origin_sym)) {
                            // A GENERIC origin has no llvm::Function to alias: pass 1 skipped it
                            // (see the generic_params check there), and each instantiation is
                            // declared separately by declare_generic_functions() under the
                            // ORIGIN module's key — which is the same key a call through this
                            // alias resolves to, because instantiate_generic_function() redirects
                            // bare-import aliases to their origin before interning the instance.
                            // So there is nothing to register here, and the .at() below would throw.
                            if (ofn->decl && !ofn->decl->generic_params.empty()) continue;
                            functions_[FunctionKey{path, alias_name}] = functions_.at(FunctionKey{origin_path, origin_name});
                        }
                        // TypeSymbol: no codegen table to alias — struct/enum/union/bitset/
                        // trait layout is driven entirely by the GLOBAL, index-based
                        // sema_program_.structs/enums/unions/etc. vectors (declare_structs()
                        // et al.), never by ProgramModule::symbols.
                        // MacroSymbol: no bulk emission pass exists — expanded inline at each
                        // use site from whichever MacroSymbol the caller's own module lookup
                        // found (the alias itself, already fully resolved by sema).
                        // ExtFunctionSymbol: see the note in the main loop above — already
                        // naturally deduplicated, no redirect needed here.
                    }
                }

                // Declared here (not just in emit_init) so '_start', declared later in run(),
                // can already reference it. Uses a sentinel empty module path as its
                // FunctionKey since '_init' isn't scoped to any one Mirage module. Generated
                // whenever at least one '@init' function exists AND '--noinit' wasn't passed —
                // independent of '--freestanding' (a freestanding build still gets '_init', for
                // the user's own '_start' to call; see run()'s emit_start gating, unchanged).
                if (!options_.noinit && !sema_program_.init_call_order.empty()) {
                    auto *init_ty = llvm::FunctionType::get(llvm::Type::getVoidTy(*context_), {}, false);
                    functions_[FunctionKey{"", "_init"}] = llvm::Function::Create(
                        init_ty, llvm::GlobalValue::ExternalLinkage, "_init", *module_);
                }
            }

            static auto global_key(const std::string &module_path, const std::string &name) -> std::string {
                return module_path + "\n" + name;
            }

            auto zero_value(const std::string &module_path, const sema::ResolvedType &type) -> llvm::Constant * {
                return llvm::Constant::getNullValue(llvm_type(module_path, type));
            }

            // Emit a value that default-initializes `type` in `type_module`.
            // Applies struct field-level init exprs recursively; zeros everything else.
            auto emit_default_value(const std::string &type_module, const sema::ResolvedType &type) -> llvm::Value * {
                if (type.kind == sema::TypeKind::Struct) {
                    const auto &info = sema_program_.structs.at(type.struct_index);
                    const auto &struct_module = info.module_path;
                    auto *ll_ty = llvm_type(struct_module, type);
                    llvm::Value *agg = llvm::Constant::getNullValue(ll_ty);
                    const auto &lowering = struct_lowering(type.struct_index);

                    const ScopedEmitModule module_scope(*this, struct_module);
                    use_struct_instance_exprs(type.struct_index);

                    for (size_t i = 0; i < info.fields.size(); ++i) {
                        const auto &field = info.fields[i];
                        const auto ll_idx = lowering.field_indices.at(i);
                        llvm::Value *fval;
                        if (field.init_expr) {
                            fval = emit_expr(*field.init_expr);
                        } else {
                            fval = emit_default_value(struct_module, field.type);
                        }
                        agg = builder_.CreateInsertValue(agg, fval, {ll_idx});
                    }

                    return agg;
                }
                if (type.kind == sema::TypeKind::Array) {
                    const auto &array_info = sema_program_.arrays.at(type.array_index);
                    auto *ll_ty = llvm_type(type_module, type);
                    llvm::Value *agg = llvm::Constant::getNullValue(ll_ty);
                    for (uint64_t i = 0; i < array_info.count; ++i) {
                        auto *elem = emit_default_value(type_module, array_info.element_type);
                        agg = builder_.CreateInsertValue(agg, elem, {static_cast<unsigned>(i)});
                    }
                    return agg;
                }
                if (type.kind == sema::TypeKind::Union) {
                    return llvm::Constant::getNullValue(unions_.at(type.union_index));
                }
                return zero_value(type_module, type);
            }

            // Constant analogue of emit_default_value, for use inside emit_const_or_runtime
            // (i.e. global initializers), where there is no valid instruction insertion point.
            // Recurses via emit_const_or_runtime instead of emit_expr for field-level defaults.
            auto emit_const_default_value(const std::string &type_module, const sema::ResolvedType &type) -> llvm::Constant * {
                if (type.kind == sema::TypeKind::Struct) {
                    const auto &info = sema_program_.structs.at(type.struct_index);
                    const auto &struct_module = info.module_path;
                    auto *ll_ty = llvm_type(struct_module, type);
                    llvm::Value *agg = llvm::Constant::getNullValue(ll_ty);
                    const auto &lowering = struct_lowering(type.struct_index);

                    const ScopedEmitModule module_scope(*this, struct_module);
                    use_struct_instance_exprs(type.struct_index);

                    for (size_t i = 0; i < info.fields.size(); ++i) {
                        const auto &field = info.fields[i];
                        const auto ll_idx = lowering.field_indices.at(i);
                        llvm::Constant *fval = field.init_expr
                                                    ? llvm::cast<llvm::Constant>(emit_const_or_runtime(*field.init_expr, true))
                                                    : emit_const_default_value(struct_module, field.type);
                        agg = builder_.CreateInsertValue(agg, fval, {ll_idx});
                    }

                    return llvm::cast<llvm::Constant>(agg);
                }
                if (type.kind == sema::TypeKind::Array) {
                    const auto &array_info = sema_program_.arrays.at(type.array_index);
                    auto *ll_ty = llvm_type(type_module, type);
                    llvm::Value *agg = llvm::Constant::getNullValue(ll_ty);
                    for (uint64_t i = 0; i < array_info.count; ++i) {
                        auto *elem = emit_const_default_value(type_module, array_info.element_type);
                        agg = builder_.CreateInsertValue(agg, elem, {static_cast<unsigned>(i)});
                    }
                    return llvm::cast<llvm::Constant>(agg);
                }
                if (type.kind == sema::TypeKind::Union) {
                    return llvm::Constant::getNullValue(unions_.at(type.union_index));
                }
                return zero_value(type_module, type);
            }

            // Materializes an omitted trailing call argument from its default-parameter
            // expression. Mirrors emit_default_value/emit_const_default_value's dual
            // path: a compile-time-constant default folds via emit_constant_expr, no
            // runtime evaluation code emitted; anything else is evaluated inline at the
            // call site (same insertion point as the surrounding call) with
            // current_module_path_/current_module_ swapped to the CALLEE's (or trait's)
            // declaring module — not the caller's — since the default expression's AST
            // node physically lives there and may reference symbols private to that
            // module. 'declaring_module' must be a stable reference (e.g. a module_path
            // field on a Program-owned info struct), not a temporary.
            auto emit_default_arg(const ast::Expr &default_expr, bool is_const, const std::string &declaring_module, const sema::ResolvedType &param_type) -> llvm::Value * {
                // The module swap applies to the constant-folding path too: emit_constant_expr
                // reaches lookups keyed on current_module_ (sizeof_operand's type-symbol and
                // expr_types lookups, for instance), and those must resolve against the module
                // the expression was written and sema-checked in, not the caller's.
                const ScopedEmitModule module_scope(*this, declaring_module);
                return is_const ? emit_constant_expr(default_expr)
                                : emit_value_as(default_expr, param_type, declaring_module);
            }

            // As emit_default_arg, for a GENERIC instance's omitted trailing argument. A default
            // expression may reference the enclosing declaration's own generic parameters
            // (spec.md §22) — 'fn f[K: type](n := size_of(K))' — but it is materialized here at
            // the CALL site, where active_generic_env_stack holds the caller's bindings, or none
            // at all. Push the callee instance's substitution env for just this expression so the
            // generic-param fallbacks in sizeof_operand/align_of_operand and friends resolve
            // against the right instantiation. Scoped per-argument rather than around the whole
            // call so the caller's own explicit arguments still emit in the caller's scope.
            auto emit_generic_default_arg(size_t instance_idx, const ast::Expr &default_expr,
                                           bool is_const, const sema::ResolvedType &param_type) -> llvm::Value * {
                const auto &instance = *sema_program_.generic_fn_instances[instance_idx];
                const auto &generic_params = instance.decl ? instance.decl->generic_params
                                                             : *instance.generic_params_for_method;
                const auto env = sema::build_generic_binding_env(generic_params, instance.args);
                const sema::ScopedResolvePush guard(
                    const_cast<sema::Program &>(sema_program_).active_generic_env_stack, &env);
                // The default's AST node lives in the generic decl's module, but sema checked
                // it per-instance (instantiate_generic_function holds the instance's scope
                // across its whole signature region). emit_default_arg is about to swap the
                // module context — which would also point the records at that MODULE's tables —
                // so re-point them at the instance's afterwards. Declared after the call would
                // be too late, so the override is scoped around it here.
                const ScopedEmitModule module_scope(*this, instance.module_path);
                // Assigned directly rather than via ScopedEmitExprs: module_scope's destructor
                // already restores current_exprs_ to its pre-swap value, so this is undone on
                // every exit path for free.
                if (const auto *instance_exprs = sema_program_.find_fn_instance_exprs(instance_idx)) {
                    current_exprs_ = instance_exprs;
                }
                return is_const ? emit_constant_expr(default_expr)
                                : emit_value_as(default_expr, param_type, instance.module_path);
            }

            // As emit_default_arg, but for a call resolved via MethodInfo (static/inherent
            // dispatch, not a dyn Trait handle — see emit_trait_handle_dispatch for that
            // path). A trait-impl method never declares its own defaults, so when this
            // method backs a trait impl the default always comes from the trait's own
            // TraitMethodInfo instead — mirrors sema's method_required_params.
            auto emit_method_trailing_arg(const sema::MethodInfo &method, size_t i) -> llvm::Value * {
                if (method.trait_name) {
                    const auto *trait_info = sema_program_.trait_at(method.trait_index);
                    const auto &trait_method = trait_info->methods[method.trait_method_index];
                    return emit_default_arg(*trait_method.decl->params[i].default_value,
                        trait_method.param_default_is_const[i], trait_info->module_path, trait_method.params[i]);
                }
                return emit_default_arg(*method.decl->params[i].default_value,
                    method.param_default_is_const[i], method.impl_module, method.param_types[i]);
            }

            // Emit a struct value from an explicit StructExpr, filling in field defaults
            // for any fields not provided in the initializer.
            auto emit_struct_expr_value(const ast::StructExpr &se, const sema::ResolvedType &ty) -> llvm::Value * {
                const auto &info = sema_program_.structs.at(ty.struct_index);
                const auto &struct_module = info.module_path;
                auto *ll_ty = llvm_type(struct_module, ty);
                llvm::Value *agg = llvm::Constant::getNullValue(ll_ty);
                const auto &lowering = struct_lowering(ty.struct_index);

                // Track which fields are covered by the StructExpr
                std::unordered_map<std::string, const ast::StructExpr::Field *> provided;
                for (const auto &sf : se.fields) {
                    provided[sf.name] = &sf;
                }

                // Insert fields in StructExpr order
                for (const auto &sf : se.fields) {
                    if (std::holds_alternative<ast::UndefinedExpr>(sf.expr)) continue;
                    const auto it = std::ranges::find(info.fields, sf.name, &sema::StructField::name);
                    if (it == info.fields.end()) continue;
                    const auto field_pos = static_cast<size_t>(std::distance(info.fields.begin(), it));
                    const auto ll_idx = lowering.field_indices.at(field_pos);
                    agg = builder_.CreateInsertValue(agg, emit_value_as(sf.expr, it->type, struct_module), {ll_idx});
                }

                // Insert remaining fields from their default init exprs (in declaration order)
                const ScopedEmitModule module_scope(*this, struct_module);
                use_struct_instance_exprs(ty.struct_index);

                for (size_t i = 0; i < info.fields.size(); ++i) {
                    const auto &field = info.fields[i];
                    if (provided.contains(field.name)) continue;
                    if (!field.init_expr) continue;
                    const auto ll_idx = lowering.field_indices.at(i);
                    agg = builder_.CreateInsertValue(agg, emit_expr(*field.init_expr), {ll_idx});
                }

                return agg;
            }

            // Emit a struct value from a positional ArrayExpr, assigning values to
            // fields by declaration order and filling remaining fields from their
            // default init exprs.
            auto emit_positional_struct_expr_value(const ast::ArrayExpr &ae, const sema::ResolvedType &ty) -> llvm::Value * {
                const auto &info = sema_program_.structs.at(ty.struct_index);
                const auto &struct_module = info.module_path;
                auto *ll_ty = llvm_type(struct_module, ty);
                llvm::Value *agg = llvm::Constant::getNullValue(ll_ty);
                const auto &lowering = struct_lowering(ty.struct_index);

                for (size_t i = 0; i < ae.values.size(); ++i) {
                    if (std::holds_alternative<ast::UndefinedExpr>(ae.values[i])) continue;
                    const auto ll_idx = lowering.field_indices.at(i);
                    agg = builder_.CreateInsertValue(agg, emit_value_as(ae.values[i], info.fields[i].type, struct_module), {ll_idx});
                }

                const ScopedEmitModule module_scope(*this, struct_module);
                use_struct_instance_exprs(ty.struct_index);

                for (size_t i = ae.values.size(); i < info.fields.size(); ++i) {
                    const auto &field = info.fields[i];
                    if (!field.init_expr) continue;
                    const auto ll_idx = lowering.field_indices.at(i);
                    agg = builder_.CreateInsertValue(agg, emit_expr(*field.init_expr), {ll_idx});
                }

                return agg;
            }

            // Emit an array value from an explicit ArrayExpr; trailing elements are
            // default-initialized (per rule 1) if not all provided, unless the last
            // provided value ends with '...' (has_fill), in which case it is repeated
            // (evaluated once) to fill the remaining elements instead.
            auto emit_array_expr_value(const ast::ArrayExpr &ae, const sema::ResolvedType &ty) -> llvm::Value * {
                const auto &array_info = sema_program_.arrays.at(ty.array_index);
                auto *ll_ty = llvm_type(*current_module_path_, ty);
                llvm::Value *agg = llvm::Constant::getNullValue(ll_ty);
                llvm::Value *fill_elem = nullptr;
                for (size_t i = 0; i < ae.values.size(); ++i) {
                    if (std::holds_alternative<ast::UndefinedExpr>(ae.values[i])) continue;
                    auto *elem = emit_value_as(ae.values[i], array_info.element_type, *current_module_path_);
                    agg = builder_.CreateInsertValue(agg, elem, {static_cast<unsigned>(i)});
                    if (ae.has_fill && i == ae.values.size() - 1) {
                        fill_elem = elem;
                    }
                }
                for (uint64_t i = ae.values.size(); i < array_info.count; ++i) {
                    auto *elem = fill_elem != nullptr ? fill_elem : emit_default_value(*current_module_path_, array_info.element_type);
                    agg = builder_.CreateInsertValue(agg, elem, {static_cast<unsigned>(i)});
                }
                return agg;
            }

            // Emit a bitset literal '{.A, .B}': OR together each named member's one-bit mask.
            auto emit_bitset_expr_value(const ast::BitsetExpr &be, const sema::ResolvedType &ty) -> llvm::Value * {
                const auto &bitset_info = sema_program_.bitsets.at(ty.bitset_index);
                uint64_t folded = 0;
                for (const auto &name : be.members) {
                    folded |= bitset_member_mask(bitset_info, name, be.location).value_or(0);
                }
                return llvm::ConstantInt::get(llvm_type(*current_module_path_, ty), folded, false);
            }

            // Emit a union value from a single-member StructExpr initializer.
            // Strategy: alloca the union storage, store the member value at offset 0, then load.
            auto emit_union_expr_value(const ast::StructExpr &se, const sema::ResolvedType &ty) -> llvm::Value * {
                const auto &union_info = sema_program_.unions.at(ty.union_index);
                auto *ll_ty = unions_.at(ty.union_index);
                auto *slot = create_entry_alloca(builder_.GetInsertBlock()->getParent(), ll_ty);
                // se has exactly one field (enforced by sema)
                if (!se.fields.empty()) {
                    const auto &sf = se.fields[0];
                    const auto it = std::ranges::find(union_info.members, sf.name, &sema::UnionMember::name);
                    if (it != union_info.members.end()) {
                        // Opaque pointers: store the member value directly into the union alloca.
                        // The store uses the value's type; the union's [N x i8] storage holds it.
                        auto *member_val = emit_value_as(sf.expr, it->type, union_info.module_path);
                        builder_.CreateStore(member_val, slot);
                    }
                }
                return builder_.CreateLoad(ll_ty, slot);
            }

            // Emit a tagged union value for a TaggedVariantExpr.
            // Layout: [tag:u32 | padding | payload_bytes]
            auto emit_tagged_variant_expr(const ast::TaggedVariantExpr &expr, const sema::ResolvedType &ty) -> llvm::Value * {
                const auto &union_info = sema_program_.unions.at(ty.union_index);
                auto *ll_ty = unions_.at(ty.union_index); // [total_size x i8]
                auto *slot = create_entry_alloca(builder_.GetInsertBlock()->getParent(), ll_ty);

                // Find the variant
                const auto variant_it = std::ranges::find(union_info.variants, expr.variant_name, &sema::TaggedUnionVariant::name);
                if (variant_it == union_info.variants.end()) {
                    report_codegen_error(diag_, {}, std::format("internal error: unknown variant '{}'", expr.variant_name));
                    return llvm::UndefValue::get(ll_ty);
                }

                // Store tag (u32) at byte 0
                auto *tag_val = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), static_cast<uint64_t>(variant_it->tag_value), false);
                builder_.CreateStore(tag_val, slot);

                // Store payload at payload_offset if present
                if (variant_it->payload_struct_index >= 0 && expr.payload.has_value()) {
                    const sema::ResolvedType payload_ty{.kind = sema::TypeKind::Struct, .struct_index = variant_it->payload_struct_index};
                    const auto &payload_struct = sema_program_.structs.at(variant_it->payload_struct_index);
                    const auto &struct_module = payload_struct.module_path;

                    auto *struct_val = emit_struct_expr_value(*expr.payload, payload_ty);

                    // GEP to payload_offset
                    auto *payload_ptr = builder_.CreateConstInBoundsGEP1_64(
                        llvm::Type::getInt8Ty(*context_), slot, union_info.payload_offset);

                    // Temporarily switch module context for the store
                    const ScopedEmitModule module_scope(*this, struct_module);

                    // The store types itself from struct_val; nothing here needs the payload's
                    // llvm::Type. The module swap around it is still required, since
                    // llvm_type/module_for lookups inside CreateStore's operands resolve
                    // against the payload struct's own declaring module.
                    builder_.CreateStore(struct_val, payload_ptr);
                }

                return builder_.CreateLoad(ll_ty, slot);
            }

            // Materializes an implicit tagged-union coercion (see sema::VariantCoercion): wraps the
            // already-typechecked scalar expression in the single-field payload struct of the variant
            // sema already decided, and stores it into the union's tag+payload layout. The variant
            // choice (tag_value/payload_struct_index) comes verbatim from sema's output — this never
            // re-scans UnionInfo::variants itself.
            auto emit_variant_coercion(const ast::Expr &expr, const sema::VariantCoercion &vc) -> llvm::Value * {
                auto *ll_ty = unions_.at(vc.union_type.union_index); // [total_size x i8]
                auto *slot = create_entry_alloca(builder_.GetInsertBlock()->getParent(), ll_ty);

                auto *tag_val = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), static_cast<uint64_t>(vc.tag_value), false);
                builder_.CreateStore(tag_val, slot);

                // Evaluate the scalar in the caller's own module context first (it may reference
                // caller locals) — emit_expr, not emit_value_as, to avoid re-entering this branch;
                // the coercion match was exact, so no conversion is needed.
                auto *field_val = emit_expr(expr);

                // Whether the source expression's own (pre-coercion) type is already exactly the
                // payload struct itself — e.g. an inline `struct{...}` or named-struct payload
                // (layout_union reuses that struct's slot verbatim, no wrapping, see
                // type_resolver.cpp) coerced from a value of that exact struct type. If not, sema
                // matched via the single-field fallback (bare scalar/slice/etc. into a one-field
                // payload struct), and the value still needs wrapping into field 0.
                const auto from_ty = current_exprs_->expr_types.at(sema::get_expr_key(expr));
                const bool is_struct_payload = from_ty.kind == sema::TypeKind::Struct && from_ty.struct_index == vc.payload_struct_index;

                const sema::ResolvedType payload_ty{.kind = sema::TypeKind::Struct, .struct_index = vc.payload_struct_index};
                const auto &payload_struct = sema_program_.structs.at(vc.payload_struct_index);
                const auto &struct_module = payload_struct.module_path;
                const auto &lowering = struct_lowering(vc.payload_struct_index);
                const auto &union_info = sema_program_.unions.at(vc.union_type.union_index);

                {
                    const ScopedEmitModule module_scope(*this, struct_module);

                    auto *struct_ll_ty = llvm_type(struct_module, payload_ty);

                    llvm::Value *struct_agg;
                    if (is_struct_payload) {
                        struct_agg = field_val;
                    } else {
                        struct_agg = llvm::Constant::getNullValue(struct_ll_ty);
                        struct_agg = builder_.CreateInsertValue(struct_agg, field_val, {lowering.field_indices.at(0)});
                    }

                    auto *payload_ptr = builder_.CreateConstInBoundsGEP1_64(
                        llvm::Type::getInt8Ty(*context_), slot, union_info.payload_offset);
                    builder_.CreateStore(struct_agg, payload_ptr);
                }

                return builder_.CreateLoad(ll_ty, slot);
            }

            // Materializes a pointer-to-trait-handle coercion sema already decided (see
            // TraitCoercion's doc comment in sema.hpp): word 0 = the source pointer (already
            // an opaque 'ptr' — pointer-like casts are no-ops at the IR level, see
            // is_pointer_like's callers — so no cast instruction is needed), word 1 = the
            // right vtable global's address. 'provider_trait_index' tells us which: equal to
            // 'trait_index' for a direct exact-match impl (today's ordinary vtables_ lookup,
            // unchanged), or the composing trait actually impl'd when this was resolved
            // indirectly through composition — in which case the SAME synthesized sub-vtable
            // handle-narrowing uses (component_vtables_, built by declare_vtables()) is reused
            // here directly, so the two coercion paths share one mechanism with no duplication.
            auto emit_trait_coercion(const ast::Expr &expr, const sema::TraitCoercion &tc) -> llvm::Value * {
                auto *data_ptr = emit_expr(expr);

                const auto from = current_exprs_->expr_types.at(sema::get_expr_key(expr));
                const auto &pointee_ty = sema_program_.pointer_pointees.at(from.pointee_index);
                const auto [pointee_module, pointee_name] = sema::find_type_module_and_name(pointee_ty, sema_program_);

                const sema::ResolvedType trait_ty{.kind = sema::TypeKind::Trait, .trait_index = tc.trait_index};
                const auto [trait_module, trait_name] = sema::find_type_module_and_name(trait_ty, sema_program_);

                llvm::GlobalVariable *vtable = nullptr;
                if (tc.provider_trait_index == tc.trait_index) {
                    if (const auto it = vtables_.find({trait_module, trait_name, pointee_module, pointee_name}); it != vtables_.end()) {
                        vtable = it->second;
                    }
                } else {
                    const sema::ResolvedType provider_ty{.kind = sema::TypeKind::Trait, .trait_index = tc.provider_trait_index};
                    const auto [provider_module, provider_name] = sema::find_type_module_and_name(provider_ty, sema_program_);
                    if (const auto it = component_vtables_.find({provider_module, provider_name, trait_module, trait_name, pointee_module, pointee_name});
                        it != component_vtables_.end()) {
                        vtable = it->second;
                    }
                }

                if (!vtable) {
                    report_codegen_error(diag_, {}, "internal error: missing vtable for trait coercion");
                    return llvm::UndefValue::get(llvm_type(*current_module_path_, trait_ty));
                }

                return build_trait_handle_value(data_ptr, vtable, trait_ty, *current_module_path_);
            }

            // Materializes a handle-to-handle composed-trait narrowing coercion sema already
            // decided (see TraitHandleCoercion's doc comment in sema.hpp): the source handle's
            // data pointer passes through unchanged; the vtable pointer is replaced by loading
            // the address stored at the source vtable's pre-computed trailing slot (built by
            // declare_vtables() — see component_vtables_). Zero runtime checks.
            auto emit_trait_handle_coercion(const ast::Expr &expr, const sema::TraitHandleCoercion &thc) -> llvm::Value * {
                auto *handle = emit_expr(expr);
                auto *data_ptr = builder_.CreateExtractValue(handle, {0});
                auto *vtable_ptr = builder_.CreateExtractValue(handle, {1});

                auto *ptr_ty = llvm::PointerType::getUnqual(*context_);
                auto *slot_ptr = builder_.CreateInBoundsGEP(ptr_ty, vtable_ptr, {builder_.getInt32(thc.slot_index)});
                auto *sub_vtable_ptr = builder_.CreateLoad(ptr_ty, slot_ptr);

                const sema::ResolvedType to_trait_ty{.kind = sema::TypeKind::Trait, .trait_index = thc.to_trait_index};
                return build_trait_handle_value(data_ptr, sub_vtable_ptr, to_trait_ty, *current_module_path_);
            }

            // Materializes an implicit value-to-'any' coercion sema already decided (see
            // AnyCoercion's doc comment in sema.hpp): word 0 = the source type's id, word 1 =
            // the 'data' pointer — the value's own address, except when the source is already
            // an 'anyptr' (used directly, per the language spec, since it already IS the data
            // pointer being erased). Sema already validated addressability (is_addressable_shape)
            // before recording this coercion, so emit_lvalue is always safe to call here.
            auto emit_any_coercion(const ast::Expr &expr, const sema::AnyCoercion &ac) -> llvm::Value * {
                llvm::Value *data_ptr = ac.source_type.kind == sema::TypeKind::Anyptr
                    ? emit_expr(expr)
                    : emit_lvalue(expr).ptr;

                const auto type_id = sema_program_.type_ids.at(ac.source_type);
                const sema::ResolvedType any_ty{.kind = sema::TypeKind::Any};
                llvm::Value *any_val = llvm::UndefValue::get(llvm_type(*current_module_path_, any_ty));
                any_val = builder_.CreateInsertValue(any_val, builder_.getInt64(type_id), {0});
                any_val = builder_.CreateInsertValue(any_val, data_ptr, {1});
                return any_val;
            }

            auto validate_hosted_main() const -> const sema::FunctionSymbol * {
                const auto root_it = sema_program_.modules.find(ast_program_.root_module_path);
                if (root_it == sema_program_.modules.end()) {
                    report_codegen_error(diag_, {}, "internal error: root module not found during codegen");
                    return nullptr;
                }

                const auto sym_it = root_it->second.symbols.find("main");
                if (sym_it == root_it->second.symbols.end()) {
                    report_codegen_error(diag_, {}, "hosted build requires 'pub fn main()' or 'pub fn main() -> i32' in the entry module");
                    return nullptr;
                }

                const auto *main_fn = std::get_if<sema::FunctionSymbol>(&sym_it->second);
                if (!main_fn) {
                    report_codegen_error(diag_, {}, "'main' in the entry module must be a function");
                    return nullptr;
                }

                const auto &decl = *main_fn->decl;
                if (!main_fn->is_pub) {
                    report_codegen_error(diag_, decl.location, "hosted entry point must be declared 'pub fn main'");
                    return nullptr;
                }

                if (!main_fn->params.empty()) {
                    report_codegen_error(diag_, decl.location, "hosted entry point must not have parameters");
                    return nullptr;
                }

                const bool returns_void = main_fn->return_types.empty();
                const bool returns_i32 = main_fn->return_types.size() == 1 && main_fn->return_types.front().kind == sema::TypeKind::I32;
                const bool returns_error = main_fn->return_types.size() == 1 && main_fn->return_types.front().kind == sema::TypeKind::Union &&
                    sema_program_.unions.at(main_fn->return_types.front().union_index).is_error_union;
                if (!returns_void && !returns_i32 && !returns_error) {
                    report_codegen_error(diag_, decl.location, "hosted entry point must return either no value, i32, or error");
                    return nullptr;
                }

                return main_fn;
            }

            void emit_global_initializers() {
                for (const auto &[path, mod] : sema_program_.modules) {
                    const ScopedEmitModule module_scope(*this, path, mod);

                    for (const auto &[name, sym] : mod.symbols) {
                        // A bare-import alias's storage was already initialized once, under
                        // the origin's own key, by the origin module's own iteration.
                        if (mod.bare_import_origins.contains(name)) continue;
                        const auto *global = std::get_if<sema::GlobalSymbol>(&sym);
                        if (!global) {
                            continue;
                        }

                        llvm::Constant *init = global->decl->init ? emit_constant_expr(*global->decl->init) : zero_value(path, global->type);
                        globals_.at(global_key(path, name))->setInitializer(init);
                    }
                }
            }

            void emit_functions() {
                for (const auto &[path, mod] : sema_program_.modules) {
                    const ScopedEmitModule module_scope(*this, path, mod);

                    for (const auto &[name, sym] : mod.symbols) {
                        // A bare-import alias's body was already emitted once, under the
                        // origin's own key, by the origin module's own iteration — emitting
                        // it again here would read 'current_exprs_->expr_types' (this
                        // importer's, populated only for names/expressions actually type-
                        // checked in THIS module) for AST nodes that were only ever
                        // type-checked under the origin's ProgramModule, crashing or
                        // producing wrong code.
                        if (mod.bare_import_origins.contains(name)) continue;
                        const auto *fn = std::get_if<sema::FunctionSymbol>(&sym);
                        if (!fn) {
                            continue;
                        }
                        // See the identical skip in declare_globals_and_functions() above —
                        // a generic function's body is emitted per-instantiation instead, by
                        // emit_generic_functions().
                        if (fn->decl && !fn->decl->generic_params.empty()) continue;
                        emit_function(path, name, *fn);
                    }
                }
            }

            void declare_methods() {
                for (const auto &[path, mod] : sema_program_.modules) {
                    for (const auto &[type_name, method_map] : mod.methods) {
                        for (const auto &[method_name, info] : method_map) {
                            if (!info.is_resolved) continue;

                            // First param: opaque pointer (self)
                            std::vector<llvm::Type *> param_types;
                            param_types.push_back(llvm::PointerType::getUnqual(*context_));
                            for (const auto &p : info.param_types) {
                                param_types.push_back(llvm_type(path, p));
                            }

                            auto *fn_type = llvm::FunctionType::get(return_type(path, info.return_types), param_types, false);
                            const auto fname = symbol_name(path, method_fn_key(type_name, method_name));
                            auto *llvm_fn = llvm::Function::Create(
                                fn_type, llvm::GlobalValue::InternalLinkage, fname, *module_);
                            functions_[FunctionKey{path, method_fn_key(type_name, method_name)}] = llvm_fn;

                            if (info.decl) {
                                apply_function_attributes(llvm_fn, info.decl->attributes, path);
                            }
                        }
                    }
                }
            }

            // Same shape as declare_methods() above, but sourced from the Program-level
            // trait-impl registry instead of ProgramModule::methods, and keyed via
            // method_fn_key_for() (Type::Trait::method) so a trait-impl method never
            // collides with a same-named inherent method in functions_/FunctionKey.
            void declare_trait_methods() {
                for (const auto &impls : sema_program_.trait_impls_by_type | std::views::values) {
                    for (const auto &impl_info : impls) {
                        for (const auto &info : impl_info.methods | std::views::values) {
                            if (!info.is_resolved) continue;

                            // First param: opaque pointer (self) — identical shape to declare_methods().
                            std::vector<llvm::Type *> param_types;
                            param_types.push_back(llvm::PointerType::getUnqual(*context_));
                            for (const auto &p : info.param_types) {
                                param_types.push_back(llvm_type(impl_info.impl_module, p));
                            }

                            auto *fn_type = llvm::FunctionType::get(return_type(impl_info.impl_module, info.return_types), param_types, false);
                            const auto key = method_fn_key_for(info);
                            const auto fname = symbol_name(impl_info.impl_module, key);
                            auto *llvm_fn = llvm::Function::Create(
                                fn_type, llvm::GlobalValue::InternalLinkage, fname, *module_);
                            functions_[FunctionKey{impl_info.impl_module, key}] = llvm_fn;

                            if (info.decl) {
                                apply_function_attributes(llvm_fn, info.decl->attributes, impl_info.impl_module);
                            }
                        }
                    }
                }
            }

            // Declares one llvm::Function per reachable generic function/method instantiation
            // — mirrors declare_globals_and_functions()/declare_methods() exactly, just
            // walking sema_program_.generic_fn_instances_needed (the "sema decided this is
            // live" set; note the type side has no equivalent gating — see the note where
            // that set would live in sema.hpp) instead of ProgramModule::symbols/methods. Only
            // ever emits the instantiations sema actually recorded as reached — the same
            // "unselected branch never visited" posture 'when' uses for dead branches. Note
            // "not emitted" no longer implies "not checked": the DECLARATION's body is checked
            // once regardless, by check_generic_templates_for_program (see spec.md §22, "Eager
            // Checking of Generic Declarations").
            void declare_generic_functions() {
                for (const size_t idx : sema_program_.generic_fn_instances_needed) {
                    const auto &instance = *sema_program_.generic_fn_instances[idx];
                    const auto &module_path = instance.module_path;

                    std::vector<llvm::Type *> param_types;
                    if (instance.impl_decl) {
                        param_types.push_back(llvm::PointerType::getUnqual(*context_)); // self
                    }
                    for (const auto &p : instance.param_types) {
                        param_types.push_back(llvm_type(module_path, p));
                    }
                    auto *fn_type = llvm::FunctionType::get(return_type(module_path, instance.return_types), param_types, false);
                    const auto fname = symbol_name(module_path, instance.mangled_name);
                    auto *llvm_fn = llvm::Function::Create(fn_type, llvm::GlobalValue::InternalLinkage, fname, *module_);
                    functions_[FunctionKey{module_path, instance.mangled_name}] = llvm_fn;
                }
            }

            // One constant global per (trait, type) impl: an array of function pointers in
            // the TRAIT's method declaration order (TraitInfo::methods — read, never
            // re-derived). Each entry is the SAME llvm::Function* that static dispatch calls
            // (declared just above by declare_trait_methods()) — no wrapper, no duplication.
            // Must run after declare_trait_methods() so those Function*s already exist; may
            // run before any body is emitted, since a vtable's constant array only needs
            // stable function addresses, not defined bodies (mirrors emit_string_literal's
            // data global).
            void declare_vtables() {
                for (const auto &impls : sema_program_.trait_impls_by_type | std::views::values) {
                    for (const auto &impl_info : impls) {
                        const auto *trait_info = sema_program_.trait_at(impl_info.trait_index);
                        if (!trait_info) continue;

                        // The "family" for this impl: the impl'd trait itself, plus one entry
                        // per trait it composes (direct or transitive — TraitInfo::component_traits
                        // is already the deduped transitive closure). For a non-composing trait
                        // this is just {impl'd trait}, so the loop below degenerates to exactly
                        // today's single-vtable behavior with zero extra tail slots.
                        struct FamilyMember {
                            const sema::TraitInfo *info;
                            std::string module_path, name;
                            int trait_index;
                            llvm::GlobalVariable *global = nullptr;
                        };
                        std::vector<FamilyMember> family;
                        family.push_back({trait_info, impl_info.trait_module, impl_info.trait_name, impl_info.trait_index});
                        for (const auto &c : trait_info->component_traits) {
                            const auto *c_info = sema_program_.trait_at(c.trait_index);
                            if (!c_info) continue;
                            family.push_back({c_info, c.module_path, c.name, c.trait_index});
                        }

                        // Pass 1: create every family member's global up front (undef body for
                        // now) so Pass 2 can freely forward-reference any other family member's
                        // address — safe since every element type here is opaque 'ptr'.
                        for (auto &fm : family) {
                            const size_t n = vtable_slot_count(*fm.info);
                            auto *array_ty = llvm::ArrayType::get(llvm::PointerType::getUnqual(*context_), n);
                            fm.global = new llvm::GlobalVariable(
                                *module_, array_ty, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
                                /*Initializer=*/nullptr, std::format(".vtable.{}", vtable_counter_++));

                            if (fm.trait_index == impl_info.trait_index) {
                                // The impl'd trait's own vtable — existing key shape, unchanged,
                                // so every existing lookup (dispatch, direct pointer coercion)
                                // keeps working untouched.
                                vtables_[{impl_info.trait_module, impl_info.trait_name, impl_info.type_module, impl_info.type_name}] = fm.global;
                            } else {
                                // A synthesized sub-vtable, keyed by the ORIGINATING composing
                                // trait too — this is what keeps it independent from any separate
                                // standalone 'impl ComponentTrait for TYPE' on the same type.
                                component_vtables_[{impl_info.trait_module, impl_info.trait_name, fm.module_path, fm.name,
                                                     impl_info.type_module, impl_info.type_name}] = fm.global;
                            }
                        }

                        // Pass 2: fill in each family member's real constant array.
                        for (const auto &fm : family) {
                            std::vector<llvm::Constant *> elements;
                            elements.reserve(vtable_slot_count(*fm.info));

                            for (const auto &trait_method : fm.info->methods) {
                                llvm::Constant *entry = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context_));
                                if (const auto method_it = impl_info.methods.find(trait_method.name); method_it != impl_info.methods.end()) {
                                    const auto key = method_fn_key_for(method_it->second);
                                    if (const auto fn_it = functions_.find(FunctionKey{impl_info.impl_module, key}); fn_it != functions_.end()) {
                                        entry = fn_it->second;
                                    }
                                }
                                // A missing entry here means sema already reported a conformance
                                // error for this impl (missing method / signature mismatch) — the
                                // null placeholder just keeps the vtable's shape well-formed so
                                // codegen doesn't crash; diag_.has_errors() aborts the compile
                                // before this module is ever handed back to the caller.
                                elements.push_back(entry);
                            }

                            // Trailing back-pointer slots, one per this family member's own
                            // component_traits, in that vector's order — each one is the address
                            // of that (further-nested) component's own family global, always
                            // already built by Pass 1 above.
                            for (const auto &c : fm.info->component_traits) {
                                llvm::Constant *sub = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context_));
                                if (c.trait_index == impl_info.trait_index) {
                                    if (const auto it = vtables_.find({impl_info.trait_module, impl_info.trait_name, impl_info.type_module, impl_info.type_name});
                                        it != vtables_.end()) {
                                        sub = it->second;
                                    }
                                } else if (const auto it = component_vtables_.find({impl_info.trait_module, impl_info.trait_name, c.module_path,
                                                                                    c.name, impl_info.type_module, impl_info.type_name});
                                           it != component_vtables_.end()) {
                                    sub = it->second;
                                }
                                elements.push_back(sub);
                            }

                            auto *array_ty = llvm::ArrayType::get(llvm::PointerType::getUnqual(*context_), elements.size());
                            fm.global->setInitializer(llvm::ConstantArray::get(array_ty, elements));
                        }
                    }
                }
            }

            // ---- type_info_of / runtime/type_info support (Part 4/5) ----

            // Read-only lookup for 'pub type Type_Info = union(enum) {...}', wherever it's
            // declared — sema already forced its resolution (see find_type_info_union in
            // sema_check.cpp) for any program that got this far, so this never re-resolves,
            // only reads back what's already there.
            auto find_type_info_union() const -> const sema::UnionInfo * {
                for (const auto &mod : sema_program_.modules | std::views::values) {
                    if (const auto it = mod.symbols.find("Type_Info"); it != mod.symbols.end()) {
                        if (const auto *ts = std::get_if<sema::TypeSymbol>(&it->second);
                            ts && ts->resolved && ts->resolved->kind == sema::TypeKind::Union) {
                            return sema_program_.union_at(ts->resolved->union_index);
                        }
                    }
                }
                return nullptr;
            }

            auto find_resolved_type_by_name(const std::string &module_path, const std::string &name) const -> std::optional<sema::ResolvedType> {
                const auto mod_it = sema_program_.modules.find(module_path);
                if (mod_it == sema_program_.modules.end()) return std::nullopt;
                const auto sym_it = mod_it->second.symbols.find(name);
                if (sym_it == mod_it->second.symbols.end()) return std::nullopt;
                const auto *ts = std::get_if<sema::TypeSymbol>(&sym_it->second);
                if (!ts || !ts->resolved) return std::nullopt;
                return *ts->resolved;
            }

            auto named_struct_llvm_type(const std::string &module_path, const std::string &name) -> llvm::Type * {
                const auto ty = find_resolved_type_by_name(module_path, name);
                return ty ? llvm_type(module_path, *ty) : llvm::Type::getInt8Ty(*context_);
            }

            static auto find_type_info_variant(const sema::UnionInfo &u, const std::string &name) -> const sema::TaggedUnionVariant * {
                for (const auto &v : u.variants) {
                    if (v.name == name) return &v;
                }
                return nullptr;
            }

            // Fixed numbering from runtime/type_info.mir's own Type_Kind enum — distinct from
            // (and unrelated to) Program::type_ids' compiler-internal numbering. Only the
            // scalar kinds that can legally appear as an Enum/Bitset's storage type are ever
            // passed here.
            static auto mirage_type_kind_value(sema::TypeKind kind) -> uint32_t {
                switch (kind) {
                case sema::TypeKind::U8:     return 1;
                case sema::TypeKind::U16:    return 2;
                case sema::TypeKind::U32:    return 3;
                case sema::TypeKind::U64:    return 4;
                case sema::TypeKind::I8:     return 5;
                case sema::TypeKind::I16:    return 6;
                case sema::TypeKind::I32:    return 7;
                case sema::TypeKind::I64:    return 8;
                case sema::TypeKind::F32:    return 9;
                case sema::TypeKind::F64:    return 10;
                case sema::TypeKind::USize:  return 11;
                case sema::TypeKind::Bool:   return 12;
                case sema::TypeKind::Anyptr: return 14;
                default:                     return 0;
                }
            }

            struct TypeInfoFieldAssignment {
                std::string name;
                llvm::Constant *value = nullptr;
            };

            // Assembles a payload struct's field-value vector in DECLARATION order, dispatching
            // each named assignment to its field's actual index (never assumed position) —
            // robust to the payload struct's own field order as declared in runtime/type_info.mir.
            auto build_type_info_payload_values(const sema::StructInfo &payload_struct, const std::vector<TypeInfoFieldAssignment> &assignments) -> std::vector<llvm::Constant *> {
                std::vector<llvm::Constant *> values(payload_struct.fields.size(), nullptr);
                for (const auto &a : assignments) {
                    const auto it = std::ranges::find(payload_struct.fields, a.name, &sema::StructField::name);
                    if (it != payload_struct.fields.end()) {
                        values[static_cast<size_t>(it - payload_struct.fields.begin())] = a.value;
                    }
                }
                for (size_t i = 0; i < values.size(); ++i) {
                    if (!values[i]) {
                        values[i] = llvm::Constant::getNullValue(llvm_type(payload_struct.module_path, payload_struct.fields[i].type));
                    }
                }
                return values;
            }

            // Builds a ConstantStruct for 'struct_index' from already-computed field values (in
            // declaration order) — replicates declare_structs()'s exact padding-interleaving so
            // the result matches struct_lowering(struct_index).type's real LLVM element layout.
            // Never recomputes offsets — reads them straight from sema's StructInfo.
            auto build_struct_constant(int struct_index, const std::vector<llvm::Constant *> &field_values) -> llvm::Constant * {
                const auto &info = sema_program_.structs.at(struct_index);
                const auto &path = info.module_path;

                std::vector<llvm::Constant *> elements;
                uint32_t cursor = 0;
                for (size_t i = 0; i < info.fields.size(); ++i) {
                    const auto &field = info.fields[i];
                    if (field.offset > cursor) {
                        elements.push_back(llvm::Constant::getNullValue(llvm::ArrayType::get(llvm::Type::getInt8Ty(*context_), field.offset - cursor)));
                    }
                    elements.push_back(field_values.at(i));
                    cursor = field.offset + size_of(path, field.type);
                }
                if (info.size > cursor) {
                    elements.push_back(llvm::Constant::getNullValue(llvm::ArrayType::get(llvm::Type::getInt8Ty(*context_), info.size - cursor)));
                }
                return llvm::ConstantStruct::get(struct_lowering(struct_index).type, elements);
            }

            // Builds a private constant array of 'elements' plus a {ptr, len} slice constant
            // pointing at it — the same construction emit_string_literal() uses for []u8,
            // generalized to any element type. Empty slices use the all-zero {nil, 0}
            // representation, matching how 'nil' coerced to a slice type is represented
            // elsewhere (LiteralNilExpr's Slice case).
            auto emit_constant_slice(llvm::Type *element_ty, const std::vector<llvm::Constant *> &elements, const char *global_name_prefix) -> llvm::Constant * {
                auto *slice_ty = llvm::StructType::get(*context_, {llvm::PointerType::getUnqual(*context_), llvm::Type::getInt64Ty(*context_)});
                if (elements.empty()) {
                    return llvm::ConstantAggregateZero::get(slice_ty);
                }
                auto *array_ty = llvm::ArrayType::get(element_ty, elements.size());
                auto *init = llvm::ConstantArray::get(array_ty, elements);
                auto *global = new llvm::GlobalVariable(
                    *module_, array_ty, true, llvm::GlobalValue::PrivateLinkage, init,
                    std::format("{}.{}", global_name_prefix, string_counter_++));
                llvm::Constant *indices[] = {builder_.getInt32(0), builder_.getInt32(0)};
                auto *ptr = llvm::ConstantExpr::getInBoundsGetElementPtr(array_ty, global, llvm::ArrayRef(indices));
                auto *len = llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), elements.size());
                return llvm::ConstantStruct::get(slice_ty, {ptr, len});
            }

            auto build_type_info_field_entry(const std::string &module_path, const std::string &name, const sema::ResolvedType &type, uint64_t offset) -> llvm::Constant * {
                const auto ty = find_resolved_type_by_name(module_path, "Type_Info_Field");
                if (!ty || ty->kind != sema::TypeKind::Struct) return nullptr;
                auto values = build_type_info_payload_values(sema_program_.structs.at(ty->struct_index), {
                    {"name", emit_string_literal(name)},
                    {"type_info", type_info_ptr_for(type)},
                    {"offset", builder_.getInt64(offset)},
                });
                return build_struct_constant(ty->struct_index, values);
            }

            auto build_type_info_enum_variant_entry(const std::string &module_path, const std::string &name, int64_t value) -> llvm::Constant * {
                const auto ty = find_resolved_type_by_name(module_path, "Type_Info_Enum_Variant");
                if (!ty || ty->kind != sema::TypeKind::Struct) return nullptr;
                auto values = build_type_info_payload_values(sema_program_.structs.at(ty->struct_index), {
                    {"name", emit_string_literal(name)},
                    {"value", builder_.getInt64(static_cast<uint64_t>(value))},
                });
                return build_struct_constant(ty->struct_index, values);
            }

            auto build_type_info_tagged_variant_entry(const std::string &module_path, const std::string &name, uint32_t tag_value, llvm::Constant *payload_ptr) -> llvm::Constant * {
                const auto ty = find_resolved_type_by_name(module_path, "Type_Info_Tagged_Variant");
                if (!ty || ty->kind != sema::TypeKind::Struct) return nullptr;
                auto values = build_type_info_payload_values(sema_program_.structs.at(ty->struct_index), {
                    {"name", emit_string_literal(name)},
                    {"tag_value", builder_.getInt32(tag_value)},
                    {"payload", payload_ptr},
                });
                return build_struct_constant(ty->struct_index, values);
            }

            auto build_type_info_param_entry(const std::string &module_path, const std::string &name, const sema::ResolvedType &type) -> llvm::Constant * {
                const auto ty = find_resolved_type_by_name(module_path, "Type_Info_Param");
                if (!ty || ty->kind != sema::TypeKind::Struct) return nullptr;
                auto values = build_type_info_payload_values(sema_program_.structs.at(ty->struct_index), {
                    {"name", emit_string_literal(name)},
                    {"type_info", type_info_ptr_for(type)},
                });
                return build_struct_constant(ty->struct_index, values);
            }

            auto build_type_info_method_entry(const std::string &module_path, const std::string &name, llvm::Constant *type_info_ptr) -> llvm::Constant * {
                const auto ty = find_resolved_type_by_name(module_path, "Type_Info_Method");
                if (!ty || ty->kind != sema::TypeKind::Struct) return nullptr;
                auto values = build_type_info_payload_values(sema_program_.structs.at(ty->struct_index), {
                    {"name", emit_string_literal(name)},
                    {"type_info", type_info_ptr},
                });
                return build_struct_constant(ty->struct_index, values);
            }

            auto build_type_info_generic_arg_entry(const std::string &module_path, const sema::GenericArgValue &arg) -> llvm::Constant * {
                const auto ty = find_resolved_type_by_name(module_path, "Type_Info_Generic_Arg");
                if (!ty || ty->kind != sema::TypeKind::Struct) return nullptr;
                auto *null_ptr = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context_));
                int64_t raw_value = 0;
                if (!arg.is_type) {
                    if (const auto *iv = std::get_if<int64_t>(&arg.value_arg)) raw_value = *iv;
                }
                auto values = build_type_info_payload_values(sema_program_.structs.at(ty->struct_index), {
                    {"is_type", builder_.getInt1(arg.is_type)},
                    {"type_arg", arg.is_type ? type_info_ptr_for(arg.type_arg) : null_ptr},
                    {"value_arg", builder_.getInt64(static_cast<uint64_t>(raw_value))},
                    {"value_arg_type", arg.is_type ? null_ptr : type_info_ptr_for(arg.value_arg_scalar_type)},
                });
                return build_struct_constant(ty->struct_index, values);
            }

            // Pushes 'is_generic'/'generic_args' onto 'assignments' for a Struct/Enum/Union/
            // Tagged_Union/Bitset Type_Info payload — shared by every aggregate case in
            // emit_type_info_global below, since all five gained these fields identically
            // (see runtime/type_info/type_info.mir). Empty/false for a non-generic type,
            // matching this file's existing empty-slice conventions elsewhere.
            void push_generic_args_assignments(const std::string &module_path, const std::optional<sema::GenericInstanceInfo> &generic_instance,
                                                std::vector<TypeInfoFieldAssignment> &assignments) {
                std::vector<llvm::Constant *> arg_entries;
                if (generic_instance) {
                    for (const auto &arg : generic_instance->args) {
                        arg_entries.push_back(build_type_info_generic_arg_entry(module_path, arg));
                    }
                }
                assignments.push_back({"is_generic", builder_.getInt1(generic_instance.has_value())});
                assignments.push_back({"generic_args", emit_constant_slice(named_struct_llvm_type(module_path, "Type_Info_Generic_Arg"), arg_entries, ".type_info_generic_args")});
            }

            // Builds the Type_Info union's own constant value: {tag, [pad], payload, [pad]} as
            // a bespoke packed struct type sized/offset exactly per UnionInfo (D4 — never
            // reused/named, just a one-off literal LLVM type; safe because every consumer only
            // ever touches this global through an untyped 'ptr', same as every other anyptr in
            // this compiler).
            auto build_type_info_union_constant(const sema::UnionInfo &u, const sema::TaggedUnionVariant &variant, llvm::Constant *payload_value) -> llvm::Constant * {
                std::vector<llvm::Type *> elem_types;
                std::vector<llvm::Constant *> elem_values;

                elem_types.push_back(llvm::Type::getInt32Ty(*context_));
                elem_values.push_back(builder_.getInt32(static_cast<uint32_t>(variant.tag_value)));
                uint32_t cursor = 4; // tag is always u32 (sema::TAG_TYPE)

                if (u.payload_offset > cursor) {
                    const auto pad = u.payload_offset - cursor;
                    auto *pad_ty = llvm::ArrayType::get(llvm::Type::getInt8Ty(*context_), pad);
                    elem_types.push_back(pad_ty);
                    elem_values.push_back(llvm::Constant::getNullValue(pad_ty));
                    cursor += pad;
                }

                if (variant.payload_struct_index >= 0 && payload_value) {
                    elem_types.push_back(struct_lowering(variant.payload_struct_index).type);
                    elem_values.push_back(payload_value);
                    cursor += sema_program_.structs.at(variant.payload_struct_index).size;
                }

                if (u.size > cursor) {
                    const auto pad = u.size - cursor;
                    auto *pad_ty = llvm::ArrayType::get(llvm::Type::getInt8Ty(*context_), pad);
                    elem_types.push_back(pad_ty);
                    elem_values.push_back(llvm::Constant::getNullValue(pad_ty));
                }

                auto *bespoke_ty = llvm::StructType::get(*context_, elem_types, /*isPacked=*/true);
                return llvm::ConstantStruct::get(bespoke_ty, elem_values);
            }

            // Entry point for every '*Type_Info' reference (top-level or nested, e.g. a
            // pointer's base_type) — memoized by the ResolvedType itself (not by
            // Program::type_ids' id, since most references are reached recursively and were
            // never independently 'type_of'/any-coerced, so have no id of their own).
            auto type_info_ptr_for(const sema::ResolvedType &ty) -> llvm::Constant * {
                switch (ty.kind) {
                case sema::TypeKind::Pointer:
                case sema::TypeKind::Slice:
                case sema::TypeKind::Array:
                case sema::TypeKind::Struct:
                case sema::TypeKind::Enum:
                case sema::TypeKind::Union:
                case sema::TypeKind::Bitset:
                case sema::TypeKind::Function:
                case sema::TypeKind::Trait:
                    break;
                default:
                    // Builtin scalars (ids 1-15) and anything else Type_Info has no shape for.
                    return llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context_));
                }
                if (const auto it = type_info_globals_by_type_.find(ty); it != type_info_globals_by_type_.end()) {
                    return it->second;
                }
                if (type_info_emitting_.contains(ty)) {
                    // Cycle (e.g. a self-referential struct reached through a pointer field) —
                    // break it with nil rather than recursing forever; the outer type's own
                    // Type_Info is still correct, just this one back-reference is unavailable.
                    return llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context_));
                }
                const auto *type_info_union = find_type_info_union();
                if (!type_info_union) {
                    return llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context_));
                }
                return emit_type_info_global(ty, *type_info_union);
            }

            auto emit_type_info_global(const sema::ResolvedType &ty, const sema::UnionInfo &type_info_union) -> llvm::Constant * {
                type_info_emitting_.insert(ty);
                const auto &module_path = type_info_union.module_path;

                std::string variant_name;
                std::vector<TypeInfoFieldAssignment> assignments;

                switch (ty.kind) {
                case sema::TypeKind::Pointer: {
                    variant_name = "Pointer";
                    assignments.push_back({"base_type", type_info_ptr_for(sema_program_.pointer_pointees.at(ty.pointee_index))});
                    break;
                }
                case sema::TypeKind::Slice: {
                    variant_name = "Slice";
                    assignments.push_back({"base_type", type_info_ptr_for(sema_program_.slices.at(ty.slice_index).element_type)});
                    break;
                }
                case sema::TypeKind::Array: {
                    variant_name = "Array";
                    const auto &arr = sema_program_.arrays.at(ty.array_index);
                    assignments.push_back({"base_type", type_info_ptr_for(arr.element_type)});
                    assignments.push_back({"length", builder_.getInt64(arr.count)});
                    break;
                }
                case sema::TypeKind::Struct: {
                    variant_name = "Struct";
                    const auto &info = sema_program_.structs.at(ty.struct_index);
                    const auto [tmod, tname] = sema::find_type_module_and_name(ty, sema_program_);
                    std::vector<llvm::Constant *> field_entries;
                    for (const auto &f : info.fields) {
                        field_entries.push_back(build_type_info_field_entry(module_path, f.name, f.type, f.offset));
                    }
                    assignments.push_back({"name", emit_string_literal(tname)});
                    assignments.push_back({"fields", emit_constant_slice(named_struct_llvm_type(module_path, "Type_Info_Field"), field_entries, ".type_info_fields")});
                    assignments.push_back({"size", builder_.getInt64(info.size)});
                    assignments.push_back({"align", builder_.getInt64(info.align)});
                    assignments.push_back({"packed", builder_.getInt1(info.is_packed)});
                    push_generic_args_assignments(module_path, info.generic_instance, assignments);
                    break;
                }
                case sema::TypeKind::Enum: {
                    variant_name = "Enum";
                    const auto &info = sema_program_.enums.at(ty.enum_index);
                    const auto [tmod, tname] = sema::find_type_module_and_name(ty, sema_program_);
                    std::vector<llvm::Constant *> variant_entries;
                    for (const auto &f : info.fields) {
                        variant_entries.push_back(build_type_info_enum_variant_entry(module_path, f.name, f.value));
                    }
                    assignments.push_back({"name", emit_string_literal(tname)});
                    assignments.push_back({"variants", emit_constant_slice(named_struct_llvm_type(module_path, "Type_Info_Enum_Variant"), variant_entries, ".type_info_variants")});
                    assignments.push_back({"storage_kind", builder_.getInt32(mirage_type_kind_value(info.underlying_type.kind))});
                    assignments.push_back({"size", builder_.getInt64(primitive_size(info.underlying_type.kind))});
                    push_generic_args_assignments(module_path, info.generic_instance, assignments);
                    break;
                }
                case sema::TypeKind::Union: {
                    const auto &info = sema_program_.unions.at(ty.union_index);
                    const auto [tmod, tname] = sema::find_type_module_and_name(ty, sema_program_);
                    if (info.is_error_union) {
                        variant_name = "Error_Type";
                        std::vector<llvm::Constant *> members;
                        for (const auto &m : info.error_member_types) {
                            members.push_back(type_info_ptr_for(m));
                        }
                        assignments.push_back({"members", emit_constant_slice(llvm::PointerType::getUnqual(*context_), members, ".type_info_err_members")});
                    } else if (info.is_tagged) {
                        variant_name = "Tagged_Union";
                        std::vector<llvm::Constant *> variant_entries;
                        for (const auto &v : info.variants) {
                            llvm::Constant *payload_ptr = v.payload_struct_index >= 0
                                ? type_info_ptr_for(sema::ResolvedType{.kind = sema::TypeKind::Struct, .struct_index = v.payload_struct_index})
                                : llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context_));
                            variant_entries.push_back(build_type_info_tagged_variant_entry(module_path, v.name, static_cast<uint32_t>(v.tag_value), payload_ptr));
                        }
                        assignments.push_back({"name", emit_string_literal(tname)});
                        assignments.push_back({"variants", emit_constant_slice(named_struct_llvm_type(module_path, "Type_Info_Tagged_Variant"), variant_entries, ".type_info_tagged_variants")});
                        assignments.push_back({"size", builder_.getInt64(info.size)});
                        assignments.push_back({"align", builder_.getInt64(info.align)});
                        push_generic_args_assignments(module_path, info.generic_instance, assignments);
                    } else {
                        variant_name = "Union";
                        std::vector<llvm::Constant *> member_entries;
                        for (const auto &m : info.members) {
                            member_entries.push_back(build_type_info_field_entry(module_path, m.name, m.type, 0));
                        }
                        assignments.push_back({"name", emit_string_literal(tname)});
                        assignments.push_back({"members", emit_constant_slice(named_struct_llvm_type(module_path, "Type_Info_Field"), member_entries, ".type_info_umembers")});
                        assignments.push_back({"size", builder_.getInt64(info.size)});
                        assignments.push_back({"align", builder_.getInt64(info.align)});
                        push_generic_args_assignments(module_path, info.generic_instance, assignments);
                    }
                    break;
                }
                case sema::TypeKind::Bitset: {
                    variant_name = "Bitset";
                    const auto &info = sema_program_.bitsets.at(ty.bitset_index);
                    const auto [tmod, tname] = sema::find_type_module_and_name(ty, sema_program_);
                    assignments.push_back({"name", emit_string_literal(tname)});
                    assignments.push_back({"member_type", type_info_ptr_for(info.member_enum_type)});
                    assignments.push_back({"storage_kind", builder_.getInt32(mirage_type_kind_value(info.storage_type.kind))});
                    assignments.push_back({"size", builder_.getInt64(primitive_size(info.storage_type.kind))});
                    push_generic_args_assignments(module_path, info.generic_instance, assignments);
                    break;
                }
                case sema::TypeKind::Function: {
                    variant_name = "Function";
                    const auto &info = sema_program_.fn_signatures.at(ty.fn_index);
                    std::vector<llvm::Constant *> param_entries;
                    for (size_t i = 0; i < info.param_types.size(); ++i) {
                        const std::string pname = i < info.param_names.size() ? info.param_names[i] : std::string{};
                        param_entries.push_back(build_type_info_param_entry(module_path, pname, info.param_types[i]));
                    }
                    std::vector<llvm::Constant *> return_ptrs;
                    for (const auto &rt : info.return_types) {
                        return_ptrs.push_back(type_info_ptr_for(rt));
                    }
                    assignments.push_back({"params", emit_constant_slice(named_struct_llvm_type(module_path, "Type_Info_Param"), param_entries, ".type_info_params")});
                    assignments.push_back({"return_types", emit_constant_slice(llvm::PointerType::getUnqual(*context_), return_ptrs, ".type_info_returns")});
                    assignments.push_back({"is_variadic", builder_.getInt1(info.is_variadic)});
                    break;
                }
                case sema::TypeKind::Trait: {
                    variant_name = "Trait";
                    const auto *info = sema_program_.trait_at(ty.trait_index);
                    const auto [tmod, tname] = sema::find_type_module_and_name(ty, sema_program_);
                    std::vector<llvm::Constant *> method_entries;
                    if (info) {
                        for (const auto &m : info->methods) {
                            // Trait methods don't carry a separately-interned function-type
                            // ResolvedType to recurse into — nil 'type_info', name is still
                            // reflected.
                            method_entries.push_back(build_type_info_method_entry(
                                module_path, m.name, llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context_))));
                        }
                    }
                    assignments.push_back({"name", emit_string_literal(tname)});
                    assignments.push_back({"methods", emit_constant_slice(named_struct_llvm_type(module_path, "Type_Info_Method"), method_entries, ".type_info_methods")});
                    break;
                }
                default:
                    break;
                }

                type_info_emitting_.erase(ty);

                if (variant_name.empty()) {
                    return llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context_));
                }

                const auto *variant = find_type_info_variant(type_info_union, variant_name);
                if (!variant || variant->payload_struct_index < 0) {
                    report_codegen_error(diag_, {}, std::format(
                        "internal error: runtime/type_info's Type_Info union has no '{}' variant", variant_name));
                    return llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context_));
                }

                const auto &payload_struct = sema_program_.structs.at(variant->payload_struct_index);
                auto payload_values = build_type_info_payload_values(payload_struct, assignments);
                auto *payload_value = build_struct_constant(variant->payload_struct_index, payload_values);
                auto *union_const = build_type_info_union_constant(type_info_union, *variant, payload_value);

                auto *global = new llvm::GlobalVariable(
                    *module_, union_const->getType(), true, llvm::GlobalValue::PrivateLinkage, union_const,
                    std::format(".type_info.{}", type_info_globals_by_type_.size()));
                type_info_globals_by_type_[ty] = global;
                return global;
            }

            auto type_info_table_entry_ty() -> llvm::StructType * {
                return llvm::StructType::get(*context_, {llvm::Type::getInt64Ty(*context_), llvm::PointerType::getUnqual(*context_)});
            }

            // Emits a '*Type_Info' global for every type the program's 'type_info_of' calls
            // and 'any' coercions need (Program::types_needing_info — see D5 in the reflection
            // plan), plus the sorted '__mirage_type_info_table' binary-search table over all of
            // them. A strict no-op (early return) for any program that never imports
            // runtime/type_info — types_needing_info is only ever populated by sema's
            // TypeInfoOfExpr/'any'-coercion handling.
            void declare_type_info_globals() {
                if (sema_program_.types_needing_info.empty()) return;
                const auto *type_info_union = find_type_info_union();
                if (!type_info_union) return;

                for (const auto id : sema_program_.types_needing_info) {
                    const auto ty_it = sema_program_.types_needing_info_types.find(id);
                    if (ty_it == sema_program_.types_needing_info_types.end()) continue;
                    type_info_ptr_for(ty_it->second);
                }

                if (type_info_globals_by_type_.empty()) return;

                std::vector<llvm::Constant *> table_entries;
                for (const auto id : sema_program_.types_needing_info) {
                    const auto ty_it = sema_program_.types_needing_info_types.find(id);
                    if (ty_it == sema_program_.types_needing_info_types.end()) continue;
                    const auto global_it = type_info_globals_by_type_.find(ty_it->second);
                    if (global_it == type_info_globals_by_type_.end()) continue;
                    table_entries.push_back(llvm::ConstantStruct::get(type_info_table_entry_ty(), {builder_.getInt64(id), global_it->second}));
                }
                if (table_entries.empty()) return;

                auto *arr_ty = llvm::ArrayType::get(type_info_table_entry_ty(), table_entries.size());
                auto *init = llvm::ConstantArray::get(arr_ty, table_entries);
                type_info_table_global_ = new llvm::GlobalVariable(
                    *module_, arr_ty, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage, init, "__mirage_type_info_table");
                type_info_table_size_ = table_entries.size();
            }

            // Inline binary search over '__mirage_type_info_table' (never a runtime function
            // call, per the reflection design) using this codebase's existing alloca+load/store
            // loop idiom (see emit_while) rather than raw SSA/PHI, to stay consistent with the
            // rest of this file. Returns nil if the key isn't found (e.g. a builtin scalar's id
            // at runtime, which never gets a table entry).
            auto emit_type_info_runtime_lookup(const ast::TypeInfoOfExpr &expr) -> llvm::Value * {
                auto *ptr_ty = llvm::PointerType::getUnqual(*context_);
                if (!type_info_table_global_ || type_info_table_size_ == 0) {
                    return llvm::ConstantPointerNull::get(ptr_ty);
                }

                const auto operand_ty = current_exprs_->expr_types.at(sema::get_expr_key(expr.operand));
                llvm::Value *key = operand_ty.kind == sema::TypeKind::Any
                    ? builder_.CreateExtractValue(emit_expr(expr.operand), {0})
                    : emit_expr(expr.operand); // already an i64 'type' id value

                auto *fn = builder_.GetInsertBlock()->getParent();
                auto *i64_ty = llvm::Type::getInt64Ty(*context_);
                auto *entry_ty = type_info_table_entry_ty();
                auto *table_ty = llvm::ArrayType::get(entry_ty, type_info_table_size_);

                auto *lo_slot = create_entry_alloca(fn, i64_ty, "tinfo.lo");
                auto *hi_slot = create_entry_alloca(fn, i64_ty, "tinfo.hi");
                auto *result_slot = create_entry_alloca(fn, ptr_ty, "tinfo.result");
                builder_.CreateStore(llvm::ConstantInt::get(i64_ty, 0), lo_slot);
                builder_.CreateStore(llvm::ConstantInt::get(i64_ty, type_info_table_size_ - 1), hi_slot);
                builder_.CreateStore(llvm::ConstantPointerNull::get(ptr_ty), result_slot);

                auto *cond_bb = llvm::BasicBlock::Create(*context_, "tinfo.cond", fn);
                auto *body_bb = llvm::BasicBlock::Create(*context_, "tinfo.body", fn);
                auto *found_bb = llvm::BasicBlock::Create(*context_, "tinfo.found", fn);
                auto *checklt_bb = llvm::BasicBlock::Create(*context_, "tinfo.checklt", fn);
                auto *narrow_hi_bb = llvm::BasicBlock::Create(*context_, "tinfo.narrow_hi", fn);
                auto *narrow_lo_bb = llvm::BasicBlock::Create(*context_, "tinfo.narrow_lo", fn);
                auto *end_bb = llvm::BasicBlock::Create(*context_, "tinfo.end", fn);
                builder_.CreateBr(cond_bb);

                builder_.SetInsertPoint(cond_bb);
                auto *lo = builder_.CreateLoad(i64_ty, lo_slot);
                auto *hi = builder_.CreateLoad(i64_ty, hi_slot);
                builder_.CreateCondBr(builder_.CreateICmpSLE(lo, hi), body_bb, end_bb);

                builder_.SetInsertPoint(body_bb);
                auto *lo2 = builder_.CreateLoad(i64_ty, lo_slot);
                auto *hi2 = builder_.CreateLoad(i64_ty, hi_slot);
                auto *mid = builder_.CreateAdd(lo2, builder_.CreateUDiv(builder_.CreateSub(hi2, lo2), llvm::ConstantInt::get(i64_ty, 2)));
                auto *entry_ptr = builder_.CreateInBoundsGEP(table_ty, type_info_table_global_, {llvm::ConstantInt::get(i64_ty, 0), mid});
                auto *entry_id = builder_.CreateLoad(i64_ty, builder_.CreateStructGEP(entry_ty, entry_ptr, 0));
                builder_.CreateCondBr(builder_.CreateICmpEQ(key, entry_id), found_bb, checklt_bb);

                builder_.SetInsertPoint(found_bb);
                builder_.CreateStore(builder_.CreateLoad(ptr_ty, builder_.CreateStructGEP(entry_ty, entry_ptr, 1)), result_slot);
                builder_.CreateBr(end_bb);

                builder_.SetInsertPoint(checklt_bb);
                builder_.CreateCondBr(builder_.CreateICmpULT(key, entry_id), narrow_hi_bb, narrow_lo_bb);

                builder_.SetInsertPoint(narrow_hi_bb);
                builder_.CreateStore(builder_.CreateSub(mid, llvm::ConstantInt::get(i64_ty, 1)), hi_slot);
                builder_.CreateBr(cond_bb);

                builder_.SetInsertPoint(narrow_lo_bb);
                builder_.CreateStore(builder_.CreateAdd(mid, llvm::ConstantInt::get(i64_ty, 1)), lo_slot);
                builder_.CreateBr(cond_bb);

                builder_.SetInsertPoint(end_bb);
                return builder_.CreateLoad(ptr_ty, result_slot);
            }

            // Find the pointer ResolvedType for self_type in the current module's pointees.
            auto find_self_ptr_type(const sema::ResolvedType &self_type) const -> sema::ResolvedType {
                for (size_t i = 0; i < sema_program_.pointer_pointees.size(); ++i) {
                    if (sema_program_.pointer_pointees[i] == self_type) {
                        return sema::ResolvedType{.kind = sema::TypeKind::Pointer, .pointee_index = static_cast<int>(i)};
                    }
                }
                return sema::ResolvedType{.kind = sema::TypeKind::Pointer, .pointee_index = 0};
            }

            void emit_method(const std::string &module_path, const std::string &type_name, const sema::MethodInfo &info) {
                const auto key = method_fn_key_for(info);
                current_function_ = functions_.at(FunctionKey{module_path, key});
                current_returns_ = info.return_types;
                locals_.clear();
                macro_args_.clear();
                continue_targets_.clear();
                break_targets_.clear();
                defer_scopes_.clear();
                next_block_is_loop_body_ = false;

                auto *entry = llvm::BasicBlock::Create(*context_, "entry", current_function_);
                builder_.SetInsertPoint(entry);

                // First arg is self (a pointer to the struct/enum)
                auto arg_it = current_function_->arg_begin();
                arg_it->setName("self");
                auto *self_slot = create_entry_alloca(current_function_, llvm::PointerType::getUnqual(*context_), "self");
                builder_.CreateStore(arg_it, self_slot);
                locals_["self"] = LocalValue{
                    .alloca = self_slot,
                    .type = find_self_ptr_type(info.self_type),
                    .type_module = module_path,
                };
                ++arg_it;

                // Remaining args
                size_t index = 0;
                for (; arg_it != current_function_->arg_end(); ++arg_it, ++index) {
                    const auto &param = info.decl->params[index];
                    arg_it->setName(param.name);
                    auto *slot = create_entry_alloca(current_function_, llvm_type(module_path, info.param_types[index]), param.name);
                    builder_.CreateStore(arg_it, slot);
                    locals_[param.name] = LocalValue{
                        .alloca = slot,
                        .type = info.param_types[index],
                        .type_module = param.type
                                           ? type_module_for_ast_type(*param.type, module_path, info.param_types[index])
                                           : module_path,
                    };
                }

                emit_stmt(info.decl->body);

                if (!builder_.GetInsertBlock()->getTerminator()) {
                    if (info.return_types.empty()) {
                        builder_.CreateRetVoid();
                    } else {
                        report_codegen_error(diag_, info.decl->location, std::format("method '{}' may fall through without returning a value", info.decl->name));
                        builder_.CreateUnreachable();
                    }
                }
            }

            void emit_methods() {
                for (const auto &[path, mod] : sema_program_.modules) {
                    const ScopedEmitModule module_scope(*this, path, mod);
                    for (const auto &[type_name, method_map] : mod.methods) {
                        for (const auto &info : method_map | std::views::values) {
                            if (!info.is_resolved) continue;
                            emit_method(path, type_name, info);
                        }
                    }
                }
            }

            // Fills trait-impl method bodies via emit_method() verbatim — it's already
            // generic over (module_path, type_name, MethodInfo) and picks the right
            // functions_ key on its own via method_fn_key_for().
            void emit_trait_methods() {
                for (const auto &impls : sema_program_.trait_impls_by_type | std::views::values) {
                    for (const auto &impl_info : impls) {
                        const ScopedEmitModule module_scope(*this, impl_info.impl_module);
                        for (const auto &info : impl_info.methods | std::views::values) {
                            if (!info.is_resolved) continue;
                            emit_method(impl_info.impl_module, impl_info.type_name, info);
                        }
                    }
                }
            }

            // Emits every reachable generic function/method instantiation's body, mirroring
            // emit_functions()/emit_methods() exactly — the ONE piece of extra machinery a
            // generic instance's body needs beyond an ordinary function/method's is pushing
            // its own substitution env onto Program::active_generic_env_stack first: sema's
            // check_generic_function_instance_body already populated this instance's own
            // 'expr_types' (and every other side table emit_stmt/emit_expr read from) with
            // fully-CONCRETE ResolvedTypes during body-checking, so ordinary emit_stmt/
            // emit_expr work completely unchanged here — the ambient stack is needed only
            // for the few codegen call sites (a local var decl's own type annotation, e.g.
            // 'mut buf: [N]u8 = default') that re-derive a type directly from an ast::Type
            // node via sema::resolve_type/resolve_declared_type instead of reading
            // 'expr_types' — see those two free functions' own doc comments in
            // type_resolver.cpp for why they consult this same stack.
            void emit_generic_functions() {
                for (const size_t idx : sema_program_.generic_fn_instances_needed) {
                    emit_generic_function_instance(idx);
                }
            }

            void emit_generic_function_instance(size_t idx) {
                auto &instance = *sema_program_.generic_fn_instances[idx];
                const auto &module_path = instance.module_path;
                const ScopedEmitModule module_scope(*this, module_path);
                // The whole body was checked into THIS instance's records, not the module's —
                // every monomorphization of one generic walks the same AST nodes. Undone by
                // module_scope's destructor.
                if (const auto *instance_exprs = sema_program_.find_fn_instance_exprs(idx)) {
                    current_exprs_ = instance_exprs;
                }
                current_function_ = functions_.at(FunctionKey{module_path, instance.mangled_name});
                current_returns_ = instance.return_types;
                locals_.clear();
                macro_args_.clear();
                continue_targets_.clear();
                break_targets_.clear();
                defer_scopes_.clear();
                next_block_is_loop_body_ = false;

                auto *entry = llvm::BasicBlock::Create(*context_, "entry", current_function_);
                builder_.SetInsertPoint(entry);

                const auto &generic_params = instance.decl ? instance.decl->generic_params
                                                             : *instance.generic_params_for_method;
                const auto env = build_generic_binding_env(generic_params, instance.args);
                const_cast<sema::Program &>(sema_program_).active_generic_env_stack.push_back(&env);

                auto arg_it = current_function_->arg_begin();
                if (instance.impl_decl) {
                    arg_it->setName("self");
                    auto *self_slot = create_entry_alloca(current_function_, llvm::PointerType::getUnqual(*context_), "self");
                    builder_.CreateStore(arg_it, self_slot);
                    locals_["self"] = LocalValue{
                        .alloca = self_slot,
                        .type = find_self_ptr_type(*instance.self_type),
                        .type_module = module_path,
                    };
                    ++arg_it;

                    size_t index = 0;
                    for (; arg_it != current_function_->arg_end(); ++arg_it, ++index) {
                        const auto &param = instance.impl_decl->params[index];
                        arg_it->setName(param.name);
                        auto *slot = create_entry_alloca(current_function_, llvm_type(module_path, instance.param_types[index]), param.name);
                        builder_.CreateStore(arg_it, slot);
                        locals_[param.name] = LocalValue{
                            .alloca = slot,
                            .type = instance.param_types[index],
                            .type_module = param.type
                                               ? type_module_for_ast_type(*param.type, module_path, instance.param_types[index])
                                               : module_path,
                        };
                    }
                    emit_stmt(instance.impl_decl->body);
                } else {
                    size_t index = 0;
                    for (; arg_it != current_function_->arg_end(); ++arg_it, ++index) {
                        const auto &param = instance.decl->params[index];
                        arg_it->setName(param.name);
                        auto *slot = create_entry_alloca(current_function_, llvm_type(module_path, instance.param_types[index]), param.name);
                        builder_.CreateStore(arg_it, slot);
                        locals_[param.name] = LocalValue{
                            .alloca = slot,
                            .type = instance.param_types[index],
                            .type_module = param.type
                                               ? type_module_for_ast_type(*param.type, module_path, instance.param_types[index])
                                               : module_path,
                        };
                    }
                    emit_stmt(instance.decl->body);
                }

                if (!builder_.GetInsertBlock()->getTerminator()) {
                    if (instance.return_types.empty()) {
                        builder_.CreateRetVoid();
                    } else {
                        report_codegen_error(diag_, {}, std::format("generic instantiation '{}' may fall through without returning a value", instance.mangled_name));
                        builder_.CreateUnreachable();
                    }
                }

                const_cast<sema::Program &>(sema_program_).active_generic_env_stack.pop_back();
            }

            // Pass by value: emit_stmt called inside can push/pop defer_scopes_,
            // so we must not hold a reference into defer_scopes_ across those calls.
            void emit_defers_in_scope(DeferScope scope) {
                for (auto it = scope.defers.rbegin(); it != scope.defers.rend(); ++it) {
                    emit_stmt((*it)->body);
                }
            }

            void emit_defers_for_return() {
                // Snapshot scopes so that inner push/pop during emission cannot invalidate iteration.
                const auto scopes = defer_scopes_;
                for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
                    emit_defers_in_scope(*it);
                }
            }

            void emit_defers_for_loop_exit() {
                const auto scopes = defer_scopes_;
                for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
                    emit_defers_in_scope(*it);
                    if (it->is_loop_body) break;
                }
            }

            // Emits the process-exit sequence for 'exit_code_i32' (a raw syscall in
            // freestanding builds, libc exit() otherwise) and terminates the current basic
            // block with 'unreachable'. Shared by emit_start's main-return handling and
            // emit_init's per-'@init'-function failure handling below.
            void emit_process_exit(llvm::Value *exit_code_i32) {
                if (options_.freestanding) {
                    auto *exit_code_i64 = builder_.CreateSExt(exit_code_i32, llvm::Type::getInt64Ty(*context_));
                    auto *syscall_ty = llvm::FunctionType::get(
                        llvm::Type::getVoidTy(*context_),
                        {llvm::Type::getInt64Ty(*context_), llvm::Type::getInt64Ty(*context_)},
                        false);
                    auto *syscall = llvm::InlineAsm::get(
                        syscall_ty,
                        "syscall",
                        "{rax},{rdi},~{rcx},~{r11},~{memory}",
                        true);
                    builder_.CreateCall(syscall, {builder_.getInt64(231), exit_code_i64});
                } else {
                    // Call libc exit() so that stdio buffers are flushed and atexit handlers
                    // run. Reuses an existing declaration if one's already been emitted (this
                    // helper may run more than once per module, e.g. once per failing '@init'
                    // check plus once for emit_start) rather than declaring 'exit' repeatedly.
                    auto *exit_ty = llvm::FunctionType::get(
                        llvm::Type::getVoidTy(*context_),
                        {llvm::Type::getInt32Ty(*context_)},
                        false);
                    auto *exit_fn = module_->getFunction("exit");
                    if (!exit_fn) {
                        exit_fn = llvm::Function::Create(exit_ty, llvm::GlobalValue::ExternalLinkage, "exit", *module_);
                        exit_fn->setDoesNotReturn();
                    }
                    builder_.CreateCall(exit_fn, {exit_code_i32});
                }
                builder_.CreateUnreachable();
            }

            // Synthesized initializer-runner: calls every '@init' function across the program
            // in Program::init_call_order (already topologically sorted by
            // validate_init_dependencies_for_program, sema_attributes.cpp). Generated in both
            // hosted and freestanding builds (see run()'s gating) whenever at least one
            // '@init' function exists and '--noinit' wasn't passed; already declared by
            // declare_globals_and_functions so '_init' itself is a fully hoisted function.
            void emit_init() {
                auto *init_fn = functions_.at(FunctionKey{"", "_init"});
                auto *entry = llvm::BasicBlock::Create(*context_, "entry", init_fn);
                builder_.SetInsertPoint(entry);

                for (const auto &[mod_path, fn_name] : sema_program_.init_call_order) {
                    auto *target = functions_.at(FunctionKey{mod_path, fn_name});
                    const auto &fn_sym = std::get<sema::FunctionSymbol>(sema_program_.modules.at(mod_path).symbols.at(fn_name));
                    auto *call_result = builder_.CreateCall(target);

                    if (!fn_sym.return_types.empty()) {
                        // error(...): the outer Ok/Failed tag is a u32 at byte offset 0. A
                        // failing initializer immediately terminates the process with the
                        // fixed sentinel exit code 1 — not the raw error value, since
                        // error(T) has no defined integer representation outside the
                        // function's own context.
                        auto *slot = create_entry_alloca(builder_.GetInsertBlock()->getParent(), call_result->getType());
                        builder_.CreateStore(call_result, slot);
                        auto *tag = builder_.CreateLoad(llvm::Type::getInt32Ty(*context_), slot);
                        auto *is_failed = builder_.CreateICmpNE(tag, builder_.getInt32(0));

                        auto *fail_block = llvm::BasicBlock::Create(*context_, "init_failed", init_fn);
                        auto *ok_block = llvm::BasicBlock::Create(*context_, "init_ok", init_fn);
                        builder_.CreateCondBr(is_failed, fail_block, ok_block);

                        builder_.SetInsertPoint(fail_block);
                        emit_process_exit(builder_.getInt32(1));

                        builder_.SetInsertPoint(ok_block);
                    }
                }

                builder_.CreateRetVoid();
            }

            void emit_start(const sema::FunctionSymbol &main_fn) {
                auto *start_ty = llvm::FunctionType::get(llvm::Type::getVoidTy(*context_), {}, false);
                auto *start = llvm::Function::Create(start_ty, llvm::GlobalValue::ExternalLinkage, "_start", *module_);
                start->setDoesNotReturn();

                auto *entry = llvm::BasicBlock::Create(*context_, "entry", start);
                builder_.SetInsertPoint(entry);

                // _start is an OS entry point: %rsp is 16-byte aligned on entry, not 8-byte
                // as LLVM assumes for normal functions. Force realignment so that calling main
                // (which pops %rsp by 8 via the call instruction) satisfies the x86-64 ABI.
                auto *align_ty = llvm::FunctionType::get(llvm::Type::getVoidTy(*context_), {}, false);
                auto *align_asm = llvm::InlineAsm::get(align_ty, "and $$-16, %rsp",
                    "~{rsp},~{dirflag},~{fpsr},~{flags}", true);
                builder_.CreateCall(align_asm);

                // Call the synthesized '_init' (if any '@init' function exists and
                // '--noinit' wasn't passed) before 'main', so module initializers have run by
                // the time user code starts.
                if (!options_.noinit && !sema_program_.init_call_order.empty()) {
                    builder_.CreateCall(functions_.at(FunctionKey{"", "_init"}));
                }

                auto *main = functions_.at(FunctionKey{ast_program_.root_module_path, "main"});
                llvm::Value *exit_code_i32 = nullptr;
                if (main_fn.return_types.empty()) {
                    builder_.CreateCall(main);
                    exit_code_i32 = builder_.getInt32(0);
                } else if (!main_fn.return_types.empty() && main_fn.return_types.front().kind == sema::TypeKind::Union &&
                           sema_program_.unions.at(main_fn.return_types.front().union_index).is_error_union) {
                    // error(...): the outer Ok/Failed tag is a u32 at byte offset 0 of the
                    // returned tagged-union value. Exit 0 on Ok, 1 on Failed — a single
                    // process exit code can't meaningfully recover which variant failed.
                    auto *result_val = builder_.CreateCall(main);
                    auto *slot = create_entry_alloca(builder_.GetInsertBlock()->getParent(), result_val->getType());
                    builder_.CreateStore(result_val, slot);
                    auto *tag = builder_.CreateLoad(llvm::Type::getInt32Ty(*context_), slot);
                    auto *is_failed = builder_.CreateICmpNE(tag, builder_.getInt32(0));
                    exit_code_i32 = builder_.CreateZExt(is_failed, llvm::Type::getInt32Ty(*context_));
                } else {
                    exit_code_i32 = builder_.CreateCall(main);
                }

                emit_process_exit(exit_code_i32);
            }

            // Allocates in 'fn's entry block so the slot is one fixed frame object, rather than
            // a stack adjustment repeated on every execution of an enclosing loop.
            //
            // Callers inside a function body pass current_function_. Callers in helpers that can
            // also run while a *different* function is being emitted (the synthesized '_init'
            // and '_start', global-constant emission) must pass
            // builder_.GetInsertBlock()->getParent() instead: current_function_ is stale there,
            // and hoisting into the wrong function's entry block produces an instruction used
            // outside its own function, which fails LLVM module verification rather than
            // misbehaving quietly.
            static auto create_entry_alloca(llvm::Function *fn, llvm::Type *type, llvm::StringRef name = "") -> llvm::AllocaInst * {
                llvm::IRBuilder tmp(&fn->getEntryBlock(), fn->getEntryBlock().begin());
                return tmp.CreateAlloca(type, nullptr, name);
            }

            void emit_function(const std::string &module_path, const std::string &name, const sema::FunctionSymbol &fn) {
                current_function_ = functions_.at(FunctionKey{module_path, name});
                current_returns_ = fn.return_types;
                locals_.clear();
                macro_args_.clear();
                continue_targets_.clear();
                break_targets_.clear();
                defer_scopes_.clear();
                next_block_is_loop_body_ = false;

                auto *entry = llvm::BasicBlock::Create(*context_, "entry", current_function_);
                builder_.SetInsertPoint(entry);

                size_t index = 0;
                for (auto &arg : current_function_->args()) {
                    const auto &param = fn.decl->params[index];
                    arg.setName(param.name);
                    auto *slot = create_entry_alloca(current_function_, llvm_type(module_path, fn.params[index]), param.name);
                    builder_.CreateStore(&arg, slot);
                    locals_[param.name] = LocalValue{
                        .alloca = slot,
                        .type = fn.params[index],
                        .type_module = param.type
                                           ? type_module_for_ast_type(*param.type, module_path, fn.params[index])
                                           : module_path,
                    };
                    ++index;
                }

                emit_stmt(fn.decl->body);

                if (!builder_.GetInsertBlock()->getTerminator()) {
                    if (fn.return_types.empty()) {
                        builder_.CreateRetVoid();
                    } else {
                        report_codegen_error(diag_, fn.decl->location, std::format("function '{}' may fall through without returning a value", name));
                        builder_.CreateUnreachable();
                    }
                }
            }

            auto coerce_to_bool(llvm::Value *value, const sema::ResolvedType &type) -> llvm::Value * {
                if (type.kind == sema::TypeKind::Bool) {
                    return value;
                }
                if (type.kind == sema::TypeKind::Union && sema_program_.union_at(type.union_index) &&
                    sema_program_.union_at(type.union_index)->is_error_union) {
                    // error(...): Failed (outer tag != 0) = true, Ok (0) = false.
                    return builder_.CreateICmpNE(extract_error_tag(value), builder_.getInt32(0));
                }
                if (type.is_float()) {
                    return builder_.CreateFCmpONE(value, llvm::ConstantFP::get(llvm_type(*current_module_path_, type), 0.0));
                }
                if (is_pointer_like(type)) {
                    return builder_.CreateICmpNE(builder_.CreatePtrToInt(value, llvm::Type::getInt64Ty(*context_)), builder_.getInt64(0));
                }
                return builder_.CreateICmpNE(value, llvm::ConstantInt::get(llvm_type(*current_module_path_, type), 0));
            }

            static auto signedness(const sema::ResolvedType &type) -> bool {
                return type.is_signed();
            }

            auto integer_cast(llvm::Value *value, llvm::Type *to, const sema::ResolvedType &from_type) -> llvm::Value * {
                const auto from_bits = value->getType()->getIntegerBitWidth();
                const auto to_bits = to->getIntegerBitWidth();
                if (from_bits == to_bits) return value;
                if (from_bits > to_bits) return builder_.CreateTrunc(value, to);
                return signedness(from_type) ? builder_.CreateSExt(value, to) : builder_.CreateZExt(value, to);
            }

            auto emit_cast(llvm::Value *value, const sema::ResolvedType &from, const sema::ResolvedType &to) -> llvm::Value * {
                auto *to_ty = llvm_type(*current_module_path_, to);
                if (from == to) {
                    return value;
                }

                // Enums have no distinct LLVM representation - they're their underlying
                // integer type (see llvm_type's TypeKind::Enum case) - so unwrap and
                // reuse the existing integer/float/bool cast paths below.
                if (from.kind == sema::TypeKind::Enum) {
                    return emit_cast(value, sema_program_.enums.at(from.enum_index).underlying_type, to);
                }
                if (to.kind == sema::TypeKind::Enum) {
                    return emit_cast(value, from, sema_program_.enums.at(to.enum_index).underlying_type);
                }

                // Bitsets are likewise their storage integer type at the LLVM level (see
                // llvm_type's TypeKind::Bitset case). Sema already rejects a cast between
                // two different bitset types, so at most one side is ever Bitset here.
                if (from.kind == sema::TypeKind::Bitset) {
                    return emit_cast(value, sema_program_.bitsets.at(from.bitset_index).storage_type, to);
                }
                if (to.kind == sema::TypeKind::Bitset) {
                    return emit_cast(value, from, sema_program_.bitsets.at(to.bitset_index).storage_type);
                }

                if (from.kind == sema::TypeKind::Bool && to.kind != sema::TypeKind::Bool && to.is_integer()) {
                    return builder_.CreateZExt(value, to_ty);
                }
                if (from.is_integer() && to.kind == sema::TypeKind::Bool) {
                    return builder_.CreateICmpNE(value, llvm::ConstantInt::get(value->getType(), 0));
                }
                if (from.is_integer() && to.is_integer()) {
                    return integer_cast(value, to_ty, from);
                }
                if (from.is_float() && to.is_float()) {
                    if (value->getType() == to_ty) return value;
                    return value->getType()->isDoubleTy() ? builder_.CreateFPTrunc(value, to_ty) : builder_.CreateFPExt(value, to_ty);
                }
                if (from.is_integer() && to.is_float()) {
                    return signedness(from) ? builder_.CreateSIToFP(value, to_ty) : builder_.CreateUIToFP(value, to_ty);
                }
                if (from.is_float() && to.is_integer()) {
                    return signedness(to) ? builder_.CreateFPToSI(value, to_ty) : builder_.CreateFPToUI(value, to_ty);
                }
                if (is_pointer_like(from) && to.is_integer()) {
                    return integer_cast(builder_.CreatePtrToInt(value, llvm::Type::getInt64Ty(*context_)), to_ty, sema::ResolvedType{.kind = sema::TypeKind::USize});
                }
                if (from.is_integer() && is_pointer_like(to)) {
                    auto *wide = integer_cast(value, llvm::Type::getInt64Ty(*context_), from);
                    return builder_.CreateIntToPtr(wide, to_ty);
                }
                if (is_pointer_like(from) && is_pointer_like(to)) {
                    return value;
                }
                // Function pointers and anyptr are both opaque pointers in LLVM
                if ((from.kind == sema::TypeKind::Function && to.kind == sema::TypeKind::Anyptr) ||
                    (from.kind == sema::TypeKind::Anyptr && to.kind == sema::TypeKind::Function) ||
                    (from.kind == sema::TypeKind::Function && to.kind == sema::TypeKind::Function)) {
                    return value;
                }
                if (from.kind == sema::TypeKind::Bool && to.is_float()) {
                    return builder_.CreateUIToFP(builder_.CreateZExt(value, llvm::Type::getInt8Ty(*context_)), to_ty);
                }
                if (from.is_float() && to.kind == sema::TypeKind::Bool) {
                    return builder_.CreateFCmpONE(value, llvm::ConstantFP::get(value->getType(), 0.0));
                }

                report_codegen_error(diag_, {}, "unsupported scalar cast");
                return llvm::UndefValue::get(to_ty);
            }

            auto emit_int_arith(ast::BinaryOp op, llvm::Value *lhs, llvm::Value *rhs, const sema::ResolvedType &type) -> llvm::Value * {
                switch (op) {
                case ast::BinaryOp::Add:        return builder_.CreateAdd(lhs, rhs);
                case ast::BinaryOp::Sub:        return builder_.CreateSub(lhs, rhs);
                case ast::BinaryOp::Mul:        return builder_.CreateMul(lhs, rhs);
                case ast::BinaryOp::Div:        return signedness(type) ? builder_.CreateSDiv(lhs, rhs) : builder_.CreateUDiv(lhs, rhs);
                case ast::BinaryOp::Mod:        return signedness(type) ? builder_.CreateSRem(lhs, rhs) : builder_.CreateURem(lhs, rhs);
                case ast::BinaryOp::BitwiseAnd: return builder_.CreateAnd(lhs, rhs);
                case ast::BinaryOp::BitwiseOr:  return builder_.CreateOr(lhs, rhs);
                case ast::BinaryOp::BitwiseXor: return builder_.CreateXor(lhs, rhs);
                case ast::BinaryOp::ShiftLeft:  return builder_.CreateShl(lhs, rhs);
                case ast::BinaryOp::ShiftRight: return signedness(type) ? builder_.CreateAShr(lhs, rhs) : builder_.CreateLShr(lhs, rhs);
                default:                        return nullptr;
                }
            }

            auto emit_compare(ast::BinaryOp op, llvm::Value *lhs, llvm::Value *rhs, const sema::ResolvedType &type) -> llvm::Value * {
                if (type.is_float()) {
                    switch (op) {
                    case ast::BinaryOp::Equal:        return builder_.CreateFCmpOEQ(lhs, rhs);
                    case ast::BinaryOp::NotEqual:     return builder_.CreateFCmpONE(lhs, rhs);
                    case ast::BinaryOp::Less:         return builder_.CreateFCmpOLT(lhs, rhs);
                    case ast::BinaryOp::Greater:      return builder_.CreateFCmpOGT(lhs, rhs);
                    case ast::BinaryOp::LessEqual:    return builder_.CreateFCmpOLE(lhs, rhs);
                    case ast::BinaryOp::GreaterEqual: return builder_.CreateFCmpOGE(lhs, rhs);
                    default:                          return nullptr;
                    }
                }

                if (is_pointer_like(type) || type.kind == sema::TypeKind::Function) {
                    lhs = builder_.CreatePtrToInt(lhs, llvm::Type::getInt64Ty(*context_));
                    rhs = builder_.CreatePtrToInt(rhs, llvm::Type::getInt64Ty(*context_));
                }

                switch (op) {
                case ast::BinaryOp::Equal:        return builder_.CreateICmpEQ(lhs, rhs);
                case ast::BinaryOp::NotEqual:     return builder_.CreateICmpNE(lhs, rhs);
                case ast::BinaryOp::Less:         return signedness(type) ? builder_.CreateICmpSLT(lhs, rhs) : builder_.CreateICmpULT(lhs, rhs);
                case ast::BinaryOp::Greater:      return signedness(type) ? builder_.CreateICmpSGT(lhs, rhs) : builder_.CreateICmpUGT(lhs, rhs);
                case ast::BinaryOp::LessEqual:    return signedness(type) ? builder_.CreateICmpSLE(lhs, rhs) : builder_.CreateICmpULE(lhs, rhs);
                case ast::BinaryOp::GreaterEqual: return signedness(type) ? builder_.CreateICmpSGE(lhs, rhs) : builder_.CreateICmpUGE(lhs, rhs);
                default:                          return nullptr;
                }
            }

            auto pointer_step(const sema::ResolvedType &type) const -> uint64_t {
                if (type.kind == sema::TypeKind::Anyptr) {
                    return 1;
                }
                const auto &pointee = sema_program_.pointer_pointees.at(type.pointee_index);
                return size_of(*current_module_path_, pointee);
            }

            auto emit_pointer_offset(llvm::Value *ptr, llvm::Value *amount, const sema::ResolvedType &ptr_type, bool subtract) -> llvm::Value * {
                auto *i64 = llvm::Type::getInt64Ty(*context_);
                if (amount->getType()->getIntegerBitWidth() != 64) {
                    amount = integer_cast(amount, i64, sema::ResolvedType{.kind = sema::TypeKind::USize});
                }
                if (const auto step = pointer_step(ptr_type); step != 1) {
                    amount = builder_.CreateMul(amount, builder_.getInt64(step));
                }
                if (subtract) {
                    amount = builder_.CreateNeg(amount);
                }
                auto *base = builder_.CreatePtrToInt(ptr, i64);
                return builder_.CreateIntToPtr(builder_.CreateAdd(base, amount), llvm_type(*current_module_path_, ptr_type));
            }

            auto emit_binary_expr(const ast::BinaryExpr &expr) -> llvm::Value * {
                if (expr.op == ast::BinaryOp::LogicalAnd || expr.op == ast::BinaryOp::LogicalOr) {
                    return emit_logical(expr);
                }

                const auto lhs_type = current_exprs_->expr_types.at(sema::get_expr_key(expr.lhs));
                const auto rhs_type = current_exprs_->expr_types.at(sema::get_expr_key(expr.rhs));
                auto *lhs = emit_expr(expr.lhs);
                auto *rhs = emit_expr(expr.rhs);

                if (expr.op == ast::BinaryOp::In) {
                    // Single-member '.A in modes' and subset '{.A,.B} in modes' both reduce to
                    // the same formula: a single member is itself emitted as its one-bit mask,
                    // and '(rhs & mask) == mask' is equivalent to '(rhs & mask) != 0' whenever
                    // mask has exactly one bit set — so no AST-shape branching is needed here.
                    auto *anded = builder_.CreateAnd(rhs, lhs);
                    return builder_.CreateICmpEQ(anded, lhs);
                }

                if (lhs_type.kind == sema::TypeKind::Bitset) {
                    switch (expr.op) {
                    case ast::BinaryOp::Add:
                    case ast::BinaryOp::BitwiseOr: // union synonym
                        return builder_.CreateOr(lhs, rhs);
                    case ast::BinaryOp::Sub:
                        return builder_.CreateAnd(lhs, builder_.CreateNot(rhs));
                    case ast::BinaryOp::BitwiseAnd:
                    case ast::BinaryOp::BitwiseXor: // infix '~' desugars here, and raw '^' too
                        return emit_int_arith(expr.op, lhs, rhs, lhs_type);
                    default:
                        break; // Equal/NotEqual fall through to the generic comparison
                               // handling below; sema rejects every other bitset binary op.
                    }
                }

                if (is_pointer_like(lhs_type) && rhs_type.is_integer() && (expr.op == ast::BinaryOp::Add || expr.op == ast::BinaryOp::Sub)) {
                    return emit_pointer_offset(lhs, rhs, lhs_type, expr.op == ast::BinaryOp::Sub);
                }
                if (is_pointer_like(rhs_type) && lhs_type.is_integer() && expr.op == ast::BinaryOp::Add) {
                    return emit_pointer_offset(rhs, lhs, rhs_type, false);
                }

                if (lhs_type.kind == sema::TypeKind::Trait || rhs_type.kind == sema::TypeKind::Trait) {
                    // Sema allows '==' / '!=' against 'nil' or another handle of the same
                    // trait type. A trait handle is a {data, vtable} aggregate, not a
                    // scalar/pointer — ICmp can't compare it directly. Comparing the
                    // data-pointer word (word 0) checks object identity directly, and also
                    // correctly detects nil since a nil handle lowers to an all-zero
                    // aggregate (both words null). Comparing the vtable word instead would be
                    // wrong here: two different objects of the same concrete type share the
                    // same vtable pointer, so they'd falsely compare equal.
                    auto *lhs_data = builder_.CreateExtractValue(lhs, {0});
                    auto *rhs_data = builder_.CreateExtractValue(rhs, {0});
                    return emit_compare(expr.op, lhs_data, rhs_data, sema::ResolvedType{.kind = sema::TypeKind::Anyptr});
                }

                if (expr.op == ast::BinaryOp::Equal || expr.op == ast::BinaryOp::NotEqual ||
                    expr.op == ast::BinaryOp::Less || expr.op == ast::BinaryOp::Greater ||
                    expr.op == ast::BinaryOp::LessEqual || expr.op == ast::BinaryOp::GreaterEqual) {
                    // All pointer-like and function types are opaque pointers in LLVM; no explicit cast needed
                    return emit_compare(expr.op, lhs, rhs, lhs_type);
                }

                if (lhs_type.is_float()) {
                    switch (expr.op) {
                    case ast::BinaryOp::Add: return builder_.CreateFAdd(lhs, rhs);
                    case ast::BinaryOp::Sub: return builder_.CreateFSub(lhs, rhs);
                    case ast::BinaryOp::Mul: return builder_.CreateFMul(lhs, rhs);
                    case ast::BinaryOp::Div: return builder_.CreateFDiv(lhs, rhs);
                    case ast::BinaryOp::Mod: return builder_.CreateFRem(lhs, rhs);
                    default:                 break;
                    }
                }

                return emit_int_arith(expr.op, lhs, rhs, lhs_type);
            }

            auto emit_logical(const ast::BinaryExpr &expr) -> llvm::Value * {
                auto *fn = builder_.GetInsertBlock()->getParent();
                auto *rhs_bb = llvm::BasicBlock::Create(*context_, "logic.rhs", fn);
                auto *merge_bb = llvm::BasicBlock::Create(*context_, "logic.end", fn);

                auto *lhs = coerce_to_bool(emit_expr(expr.lhs), current_exprs_->expr_types.at(sema::get_expr_key(expr.lhs)));
                auto *lhs_done = builder_.GetInsertBlock();
                if (expr.op == ast::BinaryOp::LogicalAnd) {
                    builder_.CreateCondBr(lhs, rhs_bb, merge_bb);
                } else {
                    builder_.CreateCondBr(lhs, merge_bb, rhs_bb);
                }

                builder_.SetInsertPoint(rhs_bb);
                auto *rhs = coerce_to_bool(emit_expr(expr.rhs), current_exprs_->expr_types.at(sema::get_expr_key(expr.rhs)));
                auto *rhs_done = builder_.GetInsertBlock();
                builder_.CreateBr(merge_bb);

                builder_.SetInsertPoint(merge_bb);
                auto *phi = builder_.CreatePHI(llvm::Type::getInt1Ty(*context_), 2);
                phi->addIncoming(expr.op == ast::BinaryOp::LogicalAnd ? builder_.getFalse() : builder_.getTrue(), lhs_done);
                phi->addIncoming(rhs, rhs_done);
                return phi;
            }

            auto emit_array_to_slice(const ast::Expr &expr, const sema::ResolvedType &array_type, const sema::ResolvedType &slice_type, const std::string &array_module) -> llvm::Value * {
                auto lv = emit_lvalue(expr);
                const auto &array = sema_program_.arrays.at(array_type.array_index);
                auto *ptr = builder_.CreateInBoundsGEP(
                    llvm_type(array_module, array_type),
                    lv.ptr,
                    {builder_.getInt32(0), builder_.getInt64(0)});
                llvm::Value *slice = llvm::UndefValue::get(llvm_type(array_module, slice_type));
                slice = builder_.CreateInsertValue(slice, ptr, {0});
                slice = builder_.CreateInsertValue(slice, builder_.getInt64(array.count), {1});
                return slice;
            }

            // Assigns a slice into a fixed-size array by value: copies min(len, count)
            // elements and zero-fills any remaining tail (an empty slice clears the array).
            auto emit_slice_to_array(const ast::Expr &expr, const sema::ResolvedType &array_type, const std::string &array_module) -> llvm::Value * {
                auto *slice_val = emit_expr(expr);
                auto *src_ptr = builder_.CreateExtractValue(slice_val, {0});
                auto *src_len = builder_.CreateExtractValue(slice_val, {1});

                const auto &array_info = sema_program_.arrays.at(array_type.array_index);
                auto *ll_arr_ty = llvm_type(array_module, array_type);
                auto *scratch = create_entry_alloca(builder_.GetInsertBlock()->getParent(), ll_arr_ty);

                auto *total_count = builder_.getInt64(array_info.count);
                auto *copy_count = builder_.CreateSelect(builder_.CreateICmpULT(src_len, total_count), src_len, total_count);
                auto *elem_size = builder_.getInt64(size_of(array_module, array_info.element_type));
                auto *copy_bytes = builder_.CreateMul(copy_count, elem_size);
                auto *total_bytes = builder_.getInt64(array_info.size);
                auto *remaining_bytes = builder_.CreateSub(total_bytes, copy_bytes);

                const auto align = llvm::Align(array_info.align);
                builder_.CreateMemCpy(scratch, align, src_ptr, align, copy_bytes);
                auto *tail_ptr = builder_.CreateGEP(llvm::Type::getInt8Ty(*context_), scratch, copy_bytes);
                builder_.CreateMemSet(tail_ptr, builder_.getInt8(0), remaining_bytes, align);

                return builder_.CreateLoad(ll_arr_ty, scratch);
            }

            auto emit_value_as(const ast::Expr &expr, const sema::ResolvedType &target, const std::string &target_module) -> llvm::Value * {
                if (const auto it = current_exprs_->expr_variant_coercions.find(sema::get_expr_key(expr)); it != current_exprs_->expr_variant_coercions.end()) {
                    return emit_variant_coercion(expr, it->second);
                }
                if (const auto it = current_exprs_->expr_trait_coercions.find(sema::get_expr_key(expr)); it != current_exprs_->expr_trait_coercions.end()) {
                    return emit_trait_coercion(expr, it->second);
                }
                if (const auto it = current_exprs_->expr_trait_handle_coercions.find(sema::get_expr_key(expr)); it != current_exprs_->expr_trait_handle_coercions.end()) {
                    return emit_trait_handle_coercion(expr, it->second);
                }
                if (const auto it = current_exprs_->expr_any_coercions.find(sema::get_expr_key(expr)); it != current_exprs_->expr_any_coercions.end()) {
                    return emit_any_coercion(expr, it->second);
                }
                const auto from = current_exprs_->expr_types.at(sema::get_expr_key(expr));
                if (from.kind == sema::TypeKind::Array && target.kind == sema::TypeKind::Slice) {
                    return emit_array_to_slice(expr, from, target, expr_type_module_hint(expr));
                }
                if (from.kind == sema::TypeKind::Slice && target.kind == sema::TypeKind::Array) {
                    return emit_slice_to_array(expr, target, target_module);
                }
                if (from.kind == sema::TypeKind::Array && target.kind == sema::TypeKind::Pointer) {
                    auto lv = emit_lvalue(expr);
                    const auto array_module = expr_type_module_hint(expr);
                    return builder_.CreateInBoundsGEP(
                        llvm_type(array_module, from),
                        lv.ptr,
                        {builder_.getInt32(0), builder_.getInt64(0)});
                }
                if (from.kind == sema::TypeKind::Slice &&
                    (target.kind == sema::TypeKind::Pointer || target.kind == sema::TypeKind::Anyptr)) {
                    return builder_.CreateExtractValue(emit_expr(expr), {0});
                }
                return emit_expr(expr);
            }

            // Like emit_value_as(), but for 'ext fn' call arguments: struct-typed arguments
            // are coerced to their System V x86-64 ABI representation (see
            // classify_aggregate_eightbytes) instead of being passed as a raw aggregate, which is
            // what a real C callee (raylib, libc, ...) actually expects. Non-struct arguments
            // are unaffected. When the argument is a struct, its classification is written to
            // '*out_cls' so the caller can attach matching byval/sret attributes to the actual
            // CallInst - LLVM requires those on the call site itself, not just the callee
            // declaration.
            auto emit_ext_call_arg(const ast::Expr &expr, const sema::ResolvedType &param_type, const std::string &param_module, AbiClassification *out_cls = nullptr) -> llvm::Value * {
                if (!needs_ext_abi_coercion(param_type)) {
                    return emit_value_as(expr, param_type, param_module);
                }

                const auto cls = classify_aggregate_eightbytes(param_type, *current_module_path_);
                if (out_cls) {
                    *out_cls = cls;
                }

                llvm::Value *ptr;
                if (is_addressable_expr(expr)) {
                    ptr = emit_lvalue(expr).ptr;
                } else {
                    // A temporary struct value with no address of its own (e.g. a call's
                    // return value) - spill it so the coercion below has memory to read from.
                    auto *value = emit_expr(expr);
                    auto *slot = create_entry_alloca(current_function_, cls.raw_type, "ext.arg");
                    builder_.CreateStore(value, slot);
                    ptr = slot;
                }

                if (cls.indirect) {
                    return ptr;
                }
                if (cls.parts.size() == 1) {
                    return builder_.CreateLoad(cls.parts[0], ptr);
                }

                auto *low = builder_.CreateLoad(cls.parts[0], ptr);
                auto *high_ptr = builder_.CreateConstGEP1_32(llvm::Type::getInt8Ty(*context_), ptr, 8);
                auto *high = builder_.CreateLoad(cls.parts[1], high_ptr);
                llvm::Value *agg = llvm::UndefValue::get(llvm::StructType::get(*context_, cls.parts, false));
                agg = builder_.CreateInsertValue(agg, low, {0});
                agg = builder_.CreateInsertValue(agg, high, {1});
                return agg;
            }

            auto emit_macro_arg(const MacroArg &arg) -> llvm::Value * {
                auto saved_macro_args = std::move(macro_args_);
                const ScopedEmitModule module_scope(*this, arg.module_path, arg.module);
                if (arg.exprs) current_exprs_ = arg.exprs; // undone by module_scope
                macro_args_ = arg.outer_args ? *arg.outer_args : std::unordered_map<std::string, MacroArg>{};
                auto *value = emit_value_as(*arg.expr, *arg.param_type, *arg.param_module);
                macro_args_ = std::move(saved_macro_args);
                return value;
            }

            auto emit_const_macro_arg(const MacroArg &arg) -> llvm::Value * {
                auto saved_macro_args = std::move(macro_args_);
                const ScopedEmitModule module_scope(*this, arg.module_path, arg.module);
                if (arg.exprs) current_exprs_ = arg.exprs; // undone by module_scope
                macro_args_ = arg.outer_args ? *arg.outer_args : std::unordered_map<std::string, MacroArg>{};
                auto *value = emit_const_or_runtime(*arg.expr, true);
                macro_args_ = std::move(saved_macro_args);
                return value;
            }

            auto expr_type_module_hint(const ast::Expr &expr) -> std::string {
                return std::visit(
                    [&]<typename T>(const T &v) -> std::string {
                        using V = std::decay_t<T>;
                        if constexpr (std::is_same_v<V, ast::IdentExpr>) {
                            if (const auto it = locals_.find(v.name); it != locals_.end()) {
                                return it->second.type_module;
                            }
                            if (const auto sym = current_module_->symbols.find(v.name); sym != current_module_->symbols.end()) {
                                if (const auto *g = std::get_if<sema::GlobalSymbol>(&sym->second)) {
                                    if (g->decl->type) {
                                        return type_module_for_ast_type(*g->decl->type, *current_module_path_, g->type);
                                    }
                                }
                            }
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::UnaryExpr>>) {
                            if (v->op == ast::UnaryOp::AddressOf || v->op == ast::UnaryOp::Deref) {
                                return expr_type_module_hint(v->operand);
                            }
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::MemberExpr>>) {
                            return expr_type_module_hint(v->object);
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::IndexOrInstantiateExpr>>) {
                            const auto operand_ty = current_exprs_->expr_types.at(sema::get_expr_key(v->operand));
                            if (operand_ty.kind == sema::TypeKind::Pointer || operand_ty.kind == sema::TypeKind::Array || operand_ty.kind == sema::TypeKind::Slice) {
                                return expr_type_module_hint(v->operand);
                            }
                        }
                        return *current_module_path_;
                    },
                    expr);
            }

            // Whether `expr` has a memory address emit_lvalue() can compute directly (a local/
            // global, a pointer deref, or a chain of member/index access rooted in one of
            // those) as opposed to a temporary value with no address of its own (e.g. a call's
            // return value). Field access on the latter is still legal - sema permits reading
            // (but not writing/addressing) a field of a temporary struct/union - so
            // emit_member_lvalue() spills such objects to a stack slot rather than routing them
            // through emit_lvalue(), which has no case for them.
            auto is_addressable_expr(const ast::Expr &expr) -> bool {
                return std::visit(
                    [&]<typename T>(const T &v) -> bool {
                        using V = std::decay_t<T>;
                        if constexpr (std::is_same_v<V, ast::IdentExpr>) {
                            return true;
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::UnaryExpr>>) {
                            return v->op == ast::UnaryOp::Deref;
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::MemberExpr>>) {
                            return true;
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::IndexOrInstantiateExpr>>) {
                            return true;
                        } else {
                            return false;
                        }
                    },
                    expr);
            }

            auto emit_lvalue(const ast::Expr &expr) -> LValue {
                return std::visit(
                    [&]<typename T>(const T &v) -> LValue {
                        using V = std::decay_t<T>;
                        if constexpr (std::is_same_v<V, ast::IdentExpr>) {
                            if (const auto macro = macro_args_.find(v.name); macro != macro_args_.end()) {
                                report_codegen_error(diag_, v.location, "macro argument is not assignable");
                                return {};
                            }
                            if (const auto it = locals_.find(v.name); it != locals_.end()) {
                                return LValue{.ptr = it->second.alloca, .type = it->second.type, .type_module = it->second.type_module, .storage_type = llvm_type_for(it->second.type, it->second.type_module)};
                            }
                            if (auto sym = current_module_->symbols.find(v.name); sym != current_module_->symbols.end()) {
                                if (const auto *g = std::get_if<sema::GlobalSymbol>(&sym->second)) {
                                    const auto type_module = g->decl->type
                                                                 ? type_module_for_ast_type(*g->decl->type, *current_module_path_, g->type)
                                                                 : *current_module_path_;
                                    return LValue{.ptr = globals_.at(global_key(*current_module_path_, v.name)), .type = g->type, .type_module = type_module, .storage_type = llvm_type_for(g->type, type_module)};
                                }
                            }
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::UnaryExpr>>) {
                            if (v->op == ast::UnaryOp::Deref) {
                                const auto ptr_type = current_exprs_->expr_types.at(sema::get_expr_key(v->operand));
                                const auto pointee = sema_program_.pointer_pointees.at(ptr_type.pointee_index);
                                const auto pointee_module = expr_type_module_hint(v->operand);
                                return LValue{.ptr = emit_expr(v->operand), .type = pointee, .type_module = pointee_module, .storage_type = llvm_type_for(pointee, pointee_module)};
                            }
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::MemberExpr>>) {
                            return emit_member_lvalue(*v);

                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::IndexOrInstantiateExpr>>) {
                            return emit_index_lvalue(*v);
                        }

                        report_codegen_error(diag_, {}, "unsupported lvalue in codegen");
                        return {};
                    },
                    expr);
            }

            auto emit_member_lvalue(const ast::MemberExpr &member) -> LValue {
                if (const auto target_module = try_namespace_chain(member.object)) {
                    const auto &target = module_for(*target_module);
                    if (const auto sym = target.symbols.find(member.member); sym != target.symbols.end()) {
                        if (const auto *g = std::get_if<sema::GlobalSymbol>(&sym->second)) {
                            const auto type_module = g->decl->type
                                                         ? type_module_for_ast_type(*g->decl->type, *target_module, g->type)
                                                         : *target_module;
                            return LValue{.ptr = globals_.at(global_key(*target_module, member.member)), .type = g->type, .type_module = type_module, .storage_type = llvm_type_for(g->type, type_module)};
                        }
                    }
                }

                const auto object_type = current_exprs_->expr_types.at(sema::get_expr_key(member.object));
                sema::ResolvedType struct_type;
                std::string struct_module = *current_module_path_;
                llvm::Value *base = nullptr;
                if (object_type.kind == sema::TypeKind::Pointer) {
                    struct_type = sema_program_.pointer_pointees.at(object_type.pointee_index);
                    struct_module = expr_type_module_hint(member.object);
                    base = emit_expr(member.object);
                } else if (is_addressable_expr(member.object)) {
                    auto lv = emit_lvalue(member.object);
                    struct_type = lv.type;
                    struct_module = lv.type_module;
                    base = lv.ptr;
                } else {
                    // A temporary struct/union value (e.g. a call's return value) with no
                    // address of its own - spill it to a stack slot so the field GEP below has
                    // something to index into. Read-only: nothing writes back through `base`.
                    struct_type = object_type;
                    struct_module = expr_type_module_hint(member.object);
                    auto *value = emit_expr(member.object);
                    base = create_entry_alloca(builder_.GetInsertBlock()->getParent(), llvm_type(struct_module, struct_type));
                    builder_.CreateStore(value, base);
                }

                if (struct_type.kind == sema::TypeKind::Union) {
                    const auto &union_info = sema_program_.unions.at(struct_type.union_index);
                    const auto member_it = std::ranges::find(union_info.members, member.member, &sema::UnionMember::name);
                    if (member_it == union_info.members.end()) {
                        report_codegen_error(diag_, member.location, std::format("unknown union member '{}'", member.member));
                        return {};
                    }
                    // All union members reside at offset 0; return a pointer to the union storage.
                    return LValue{.ptr = base, .type = member_it->type, .type_module = struct_module, .storage_type = llvm_type_for(member_it->type, struct_module)};
                }

                const auto &info = sema_program_.structs.at(struct_type.struct_index);
                const auto field_it = std::ranges::find(info.fields, member.member, &sema::StructField::name);
                if (field_it == info.fields.end()) {
                    report_codegen_error(diag_, member.location, std::format("unknown struct field '{}'", member.member));
                    return {};
                }
                const auto field_pos = static_cast<size_t>(std::distance(info.fields.begin(), field_it));
                const auto llvm_index = struct_lowering(struct_type.struct_index).field_indices.at(field_pos);
                auto *ptr = builder_.CreateStructGEP(llvm_type(struct_module, struct_type), base, llvm_index);
                return LValue{.ptr = ptr, .type = field_it->type, .type_module = struct_module, .storage_type = llvm_type_for(field_it->type, struct_module)};
            }

            // 'index.args' is expected to be exactly one Expr-tagged element here: of the shapes
            // sema's IndexOrInstantiateExpr classification lets reach codegen (see check_expr),
            // only the ordinary-index one is an LVALUE. A generic TYPE instantiation is resolved
            // to a concrete type entirely within sema; a generic FUNCTION instantiation names a
            // monomorphized instance's address, which emit_expr handles directly and which has
            // no storage to take the address of — so reaching here with one is a bug, not
            // something to GEP into.
            auto emit_index_lvalue(const ast::IndexOrInstantiateExpr &index) -> LValue {
                if (index.args.size() != 1 || !std::holds_alternative<ast::Expr>(index.args[0].value)) {
                    report_codegen_error(diag_, index.location,
                        "internal error: generic instantiation reached the index lvalue path");
                    return LValue{};
                }
                const auto &index_expr = std::get<ast::Expr>(index.args[0].value);
                const auto operand_type = current_exprs_->expr_types.at(sema::get_expr_key(index.operand));
                const auto type_module = expr_type_module_hint(index.operand);
                auto *idx = emit_expr(index_expr);
                if (!idx->getType()->isIntegerTy(64)) {
                    idx = integer_cast(idx, llvm::Type::getInt64Ty(*context_), current_exprs_->expr_types.at(sema::get_expr_key(index_expr)));
                }

                if (operand_type.kind == sema::TypeKind::Pointer) {
                    const auto element = sema_program_.pointer_pointees.at(operand_type.pointee_index);
                    auto *ptr = builder_.CreateInBoundsGEP(llvm_type_for(element, type_module), emit_expr(index.operand), idx);
                    return LValue{.ptr = ptr, .type = element, .type_module = type_module, .storage_type = llvm_type_for(element, type_module)};
                }
                if (operand_type.kind == sema::TypeKind::Array) {
                    auto base = emit_lvalue(index.operand);
                    const auto element = sema_program_.arrays.at(operand_type.array_index).element_type;
                    auto *ptr = builder_.CreateInBoundsGEP(llvm_type(type_module, operand_type), base.ptr, {builder_.getInt32(0), idx});
                    return LValue{.ptr = ptr, .type = element, .type_module = type_module, .storage_type = llvm_type_for(element, type_module)};
                }
                if (operand_type.kind == sema::TypeKind::Slice) {
                    auto *slice = emit_expr(index.operand);
                    auto *base = builder_.CreateExtractValue(slice, {0});
                    const auto element = sema_program_.slices.at(operand_type.slice_index).element_type;
                    auto *ptr = builder_.CreateInBoundsGEP(llvm_type_for(element, type_module), base, idx);
                    return LValue{.ptr = ptr, .type = element, .type_module = type_module, .storage_type = llvm_type_for(element, type_module)};
                }
                report_codegen_error(diag_, index.location, "unsupported index operand in codegen");
                return {};
            }

            auto try_namespace_chain(const ast::Expr &expr) const -> std::optional<std::string> {
                // Inline 'import("...")' used directly as (part of) a MemberExpr chain's base,
                // e.g. 'import("...").target_os' — mirrors sema's try_resolve_namespace_chain
                // (sema_check.cpp), resolved and cached by declare_global at sema time since
                // that's the only point ast::Program::module_imports is available.
                if (const auto *imp = std::get_if<ast::ImportExpr>(&expr)) {
                    const auto path_it = current_module_->inline_import_paths.find(imp);
                    if (path_it == current_module_->inline_import_paths.end()) {
                        return std::nullopt;
                    }
                    return path_it->second;
                }

                if (const auto *ident = std::get_if<ast::IdentExpr>(&expr)) {
                    if (locals_.contains(ident->name) || macro_args_.contains(ident->name)) {
                        return std::nullopt;
                    }
                    const auto it = current_module_->symbols.find(ident->name);
                    if (it == current_module_->symbols.end()) {
                        return std::nullopt;
                    }
                    if (const auto *imp = std::get_if<sema::ImportSymbol>(&it->second)) {
                        return imp->module_path;
                    }
                    return std::nullopt;
                }

                if (const auto *member = std::get_if<std::unique_ptr<ast::MemberExpr>>(&expr)) {
                    auto inner = try_namespace_chain((*member)->object);
                    if (!inner) {
                        return std::nullopt;
                    }
                    const auto &mod = module_for(*inner);
                    const auto it = mod.symbols.find((*member)->member);
                    if (it == mod.symbols.end()) {
                        return std::nullopt;
                    }
                    if (const auto *imp = std::get_if<sema::ImportSymbol>(&it->second)) {
                        return imp->module_path;
                    }
                }
                return std::nullopt;
            }

            // Resolves an expression as a type reference. Returns the ResolvedType if expr names a
            // type (locally or via module chain), nullopt otherwise.
            auto try_type_chain(const ast::Expr &expr) const -> std::optional<sema::ResolvedType> {
                if (const auto *ident = std::get_if<ast::IdentExpr>(&expr)) {
                    if (locals_.contains(ident->name) || macro_args_.contains(ident->name)) {
                        return std::nullopt;
                    }
                    const auto it = current_module_->symbols.find(ident->name);
                    if (it == current_module_->symbols.end()) {
                        return std::nullopt;
                    }
                    if (const auto *ts = std::get_if<sema::TypeSymbol>(&it->second)) {
                        return ts->resolved;
                    }
                    return std::nullopt;
                }
                if (const auto *mem = std::get_if<std::unique_ptr<ast::MemberExpr>>(&expr)) {
                    const auto inner_module = try_namespace_chain((*mem)->object);
                    if (!inner_module) {
                        return std::nullopt;
                    }
                    const auto &mod = module_for(*inner_module);
                    const auto it = mod.symbols.find((*mem)->member);
                    if (it == mod.symbols.end()) {
                        return std::nullopt;
                    }
                    if (const auto *ts = std::get_if<sema::TypeSymbol>(&it->second)) {
                        return ts->resolved;
                    }
                    return std::nullopt;
                }
                return std::nullopt;
            }

            // Emits a call already resolved by sema to a specific generic function/method
            // instantiation (see ProgramModule::expr_generic_fn_instance) — bypasses
            // emit_call's entire name-based resolution below, since the callee is already
            // known exactly: 'instance.mangled_name' was declared as an ordinary
            // llvm::Function by declare_generic_functions(). A method instance's callee AST
            // shape is always a MemberExpr (self.reserve(...)); a free-function instance's is
            // an IdentExpr or IndexOrInstantiateExpr (make_fixed()/make_fixed[16]()) — self
            // is only ever computed for the former.
            auto emit_generic_call(const ast::CallExpr &call, size_t instance_idx) -> llvm::Value * {
                const auto &instance = *sema_program_.generic_fn_instances[instance_idx];
                auto *llvm_fn = functions_.at(FunctionKey{instance.module_path, instance.mangled_name});

                std::vector<llvm::Value *> args;
                if (instance.impl_decl) {
                    const auto *member = std::get_if<std::unique_ptr<ast::MemberExpr>>(&call.callee);
                    const auto obj_type = current_exprs_->expr_types.at(sema::get_expr_key((*member)->object));
                    llvm::Value *self_ptr = obj_type.kind == sema::TypeKind::Pointer
                        ? emit_expr((*member)->object)
                        : emit_lvalue((*member)->object).ptr;
                    args.push_back(self_ptr);
                    for (size_t i = 0; i < instance.param_types.size(); ++i) {
                        args.push_back(i < call.args.size()
                            ? emit_value_as(call.args[i], instance.param_types[i], instance.module_path)
                            : emit_generic_default_arg(instance_idx, *instance.impl_decl->params[i].default_value, instance.param_default_is_const[i], instance.param_types[i]));
                    }
                } else {
                    for (size_t i = 0; i < instance.param_types.size(); ++i) {
                        args.push_back(i < call.args.size()
                            ? emit_value_as(call.args[i], instance.param_types[i], instance.module_path)
                            : emit_generic_default_arg(instance_idx, *instance.decl->params[i].default_value, instance.param_default_is_const[i], instance.param_types[i]));
                    }
                }
                return builder_.CreateCall(llvm_fn, args);
            }

            auto emit_call(const ast::CallExpr &call) -> llvm::Value * {
                // A call resolved by sema to a specific generic function/method instantiation,
                // reached here directly (bypassing emit_expr's own get_expr_key(expr)-keyed
                // dispatch just below emit_call's other caller) via 'try'/a multi-return group
                // declaration - check_group_call_returns keys its own expr_generic_fn_instance
                // entries by '&call' (the CallExpr's own stable address, same convention
                // expr_trait_dispatch already uses), since it only ever sees the unwrapped
                // CallExpr, never the outer Expr variant get_expr_key needs.
                if (const auto inst_it = current_exprs_->expr_generic_fn_instance.find(&call); inst_it != current_exprs_->expr_generic_fn_instance.end()) {
                    return emit_generic_call(call, inst_it->second);
                }

                std::string target_module = *current_module_path_;
                std::string name;

                if (const auto *ident = std::get_if<ast::IdentExpr>(&call.callee)) {
                    // Local variable with function type → indirect call
                    if (const auto it = locals_.find(ident->name); it != locals_.end()) {
                        if (it->second.type.kind == sema::TypeKind::Function) {
                            const auto &sig = sema_program_.fn_signatures.at(it->second.type.fn_index);
                            auto *fn_ptr = builder_.CreateLoad(llvm::PointerType::getUnqual(*context_), it->second.alloca);
                            return emit_indirect_call(call, fn_ptr, sig);
                        }
                    }
                    name = ident->name;
                } else if (const auto *member = std::get_if<std::unique_ptr<ast::MemberExpr>>(&call.callee)) {
                    if (auto ns = try_namespace_chain((*member)->object)) {
                        target_module = *ns;
                        name = (*member)->member;
                    } else {
                        // Method call on a value or pointer
                        const auto obj_type = current_exprs_->expr_types.at(sema::get_expr_key((*member)->object));
                        sema::ResolvedType receiver_type = obj_type;
                        if (obj_type.kind == sema::TypeKind::Pointer) {
                            receiver_type = sema_program_.pointer_pointees.at(obj_type.pointee_index);
                        }
                        if (const auto dispatch_it = current_exprs_->expr_trait_dispatch.find(&call); dispatch_it != current_exprs_->expr_trait_dispatch.end()) {
                            return emit_trait_handle_dispatch(call, (*member)->object, dispatch_it->second);
                        }
                        const auto *method = sema::find_method(receiver_type, (*member)->member, sema_program_);
                        if (!method) {
                            // Struct field with function type → indirect call through field
                            if (receiver_type.kind == sema::TypeKind::Struct) {
                                for (const auto &field : sema_program_.structs.at(receiver_type.struct_index).fields) {
                                    if (field.name == (*member)->member && field.type.kind == sema::TypeKind::Function) {
                                        const auto &sig = sema_program_.fn_signatures.at(field.type.fn_index);
                                        auto field_lv = emit_lvalue(call.callee);
                                        auto *fn_ptr = builder_.CreateLoad(llvm::PointerType::getUnqual(*context_), field_lv.ptr);
                                        return emit_indirect_call(call, fn_ptr, sig);
                                    }
                                }
                            }
                            report_codegen_error(diag_, call.location, std::format("no method '{}' on type", (*member)->member));
                            return nullptr;
                        }

                        // Get self pointer
                        llvm::Value *self_ptr;
                        if (obj_type.kind == sema::TypeKind::Pointer) {
                            self_ptr = emit_expr((*member)->object);
                        } else {
                            self_ptr = emit_lvalue((*member)->object).ptr;
                        }

                        std::vector<llvm::Value *> args;
                        args.push_back(self_ptr);
                        if (method->is_variadic) {
                            const size_t fixed_count = method->param_types.size() - 1;
                            for (size_t i = 0; i < fixed_count; ++i) {
                                args.push_back(emit_value_as(call.args[i], method->param_types[i], method->impl_module));
                            }
                            const auto &slice_ty = method->param_types.back();
                            if (call.args.size() == fixed_count + 1) {
                                if (const auto *spread = std::get_if<std::unique_ptr<ast::SpreadExpr>>(&call.args[fixed_count])) {
                                    args.push_back(emit_value_as((*spread)->operand, slice_ty, method->impl_module));
                                    return builder_.CreateCall(
                                        functions_.at(FunctionKey{method->impl_module, method_fn_key_for(*method)}),
                                        args);
                                }
                            }
                            args.push_back(emit_variadic_tail_slice(call.args, fixed_count, slice_ty, method->impl_module));
                            return builder_.CreateCall(
                                functions_.at(FunctionKey{method->impl_module, method_fn_key_for(*method)}),
                                args);
                        }
                        for (size_t i = 0; i < method->param_types.size(); ++i) {
                            args.push_back(i < call.args.size()
                                ? emit_value_as(call.args[i], method->param_types[i], *current_module_path_)
                                : emit_method_trailing_arg(*method, i));
                        }
                        return builder_.CreateCall(
                            functions_.at(FunctionKey{method->impl_module, method_fn_key_for(*method)}),
                            args);
                    }
                } else {
                    // General expression callee (e.g. indexed array of fn ptrs, dereferenced ptr-to-fn)
                    const auto callee_ty = current_exprs_->expr_types.at(sema::get_expr_key(call.callee));
                    if (callee_ty.kind == sema::TypeKind::Function) {
                        const auto &sig = sema_program_.fn_signatures.at(callee_ty.fn_index);
                        auto *fn_ptr = emit_expr(call.callee);
                        return emit_indirect_call(call, fn_ptr, sig);
                    }
                    report_codegen_error(diag_, call.location, "unsupported call target");
                    return llvm::UndefValue::get(llvm::PointerType::getUnqual(*context_));
                }

                if (name.empty()) {
                    report_codegen_error(diag_, call.location, "unsupported call target");
                    return llvm::UndefValue::get(llvm::PointerType::getUnqual(*context_));
                }

                const auto &target = module_for(target_module);
                const auto sym_it = target.symbols.find(name);
                if (sym_it == target.symbols.end()) {
                    report_codegen_error(diag_, call.location, std::format("unknown callable '{}'", name));
                    return nullptr;
                }

                if (const auto *macro = std::get_if<sema::MacroSymbol>(&sym_it->second)) {
                    auto saved = macro_args_;
                    auto outer = std::make_shared<const std::unordered_map<std::string, MacroArg>>(saved);
                    for (size_t i = 0; i < macro->decl->params.size(); ++i) {
                        macro_args_[macro->decl->params[i].name] = MacroArg{.expr = &call.args[i], .module_path = current_module_path_, .module = current_module_, .exprs = current_exprs_, .param_type = &macro->params[i], .param_module = &target_module, .outer_args = outer};
                    }
                    // A bare-import alias's 'expr_template' AST nodes were type-checked (and
                    // their 'expr_types' entries populated) under the TRUE ORIGIN module (see
                    // sema.cpp's resolve_values_for_module redirect-and-copy-back for
                    // MacroSymbol), not 'target_module' — which, for an unqualified bare-import
                    // call, defaults to the CALLING module, not the macro's declaring one.
                    const auto origin = target.bare_import_origins.find(name);
                    const bool via_origin = origin != target.bare_import_origins.end();
                    const ScopedEmitModule module_scope(
                        *this, via_origin ? origin->second.module_path : target_module,
                        via_origin ? sema_program_.modules.at(origin->second.module_path) : target);
                    auto *value = emit_expr(macro->decl->expr_template);
                    macro_args_ = std::move(saved);
                    return value;
                }

                std::vector<llvm::Value *> args;
                if (const auto *fn = std::get_if<sema::FunctionSymbol>(&sym_it->second)) {
                    if (fn->is_variadic) {
                        const size_t fixed_count = fn->params.size() - 1;
                        for (size_t i = 0; i < fixed_count; ++i) {
                            args.push_back(emit_value_as(call.args[i], fn->params[i], target_module));
                        }
                        const auto &slice_ty = fn->params.back();
                        if (call.args.size() == fixed_count + 1) {
                            if (const auto *spread = std::get_if<std::unique_ptr<ast::SpreadExpr>>(&call.args[fixed_count])) {
                                args.push_back(emit_value_as((*spread)->operand, slice_ty, target_module));
                                return builder_.CreateCall(functions_.at(FunctionKey{target_module, name}), args);
                            }
                        }
                        args.push_back(emit_variadic_tail_slice(call.args, fixed_count, slice_ty, target_module));
                        return builder_.CreateCall(functions_.at(FunctionKey{target_module, name}), args);
                    }
                    for (size_t i = 0; i < fn->params.size(); ++i) {
                        args.push_back(i < call.args.size()
                            ? emit_value_as(call.args[i], fn->params[i], *current_module_path_)
                            : emit_default_arg(*fn->decl->params[i].default_value, fn->param_default_is_const[i], target_module, fn->params[i]));
                    }
                    return builder_.CreateCall(functions_.at(FunctionKey{target_module, name}), args);
                }
                if (const auto *ef = std::get_if<sema::ExtFunctionSymbol>(&sym_it->second)) {
                    // Aggregate-by-value args/return crossing into real C code need System V
                    // x86-64 ABI coercion (see classify_aggregate_eightbytes) - LLVM's default
                    // lowering of a raw aggregate argument doesn't match what a real C callee
                    // (raylib, libc, ...) reads. byval/sret attributes must be attached to the
                    // CallInst itself, not just the callee's declaration, so track which
                    // argument positions need them as they're built.
                    std::vector<std::pair<unsigned, AbiClassification>> byval_args;
                    for (size_t i = 0; i < call.args.size(); ++i) {
                        if (i < ef->params.size()) {
                            AbiClassification cls;
                            auto *value = emit_ext_call_arg(call.args[i], ef->params[i], *current_module_path_, &cls);
                            if (needs_ext_abi_coercion(ef->params[i]) && cls.indirect) {
                                byval_args.emplace_back(static_cast<unsigned>(args.size()), cls);
                            }
                            args.push_back(value);
                        } else {
                            args.push_back(emit_expr(call.args[i]));
                        }
                    }

                    auto *callee = ext_functions_.at(global_key(target_module, name));
                    const auto apply_byval_attrs = [&](llvm::CallInst *call_inst, const unsigned index_offset) {
                        for (const auto &[index, cls] : byval_args) {
                            call_inst->addParamAttr(index + index_offset, llvm::Attribute::getWithByValType(*context_, cls.raw_type));
                            call_inst->addParamAttr(index + index_offset, llvm::Attribute::getWithAlignment(*context_, llvm::Align(cls.raw_align)));
                        }
                    };

                    if (ef->return_type && needs_ext_abi_coercion(*ef->return_type)) {
                        const auto ret_cls = classify_aggregate_eightbytes(*ef->return_type, *current_module_path_);
                        if (ret_cls.indirect) {
                            auto *ret_slot = create_entry_alloca(current_function_, ret_cls.raw_type, "ext.ret");
                            std::vector<llvm::Value *> full_args;
                            full_args.push_back(ret_slot);
                            full_args.insert(full_args.end(), args.begin(), args.end());
                            auto *call_inst = builder_.CreateCall(callee, full_args);
                            call_inst->addParamAttr(0, llvm::Attribute::getWithStructRetType(*context_, ret_cls.raw_type));
                            call_inst->addParamAttr(0, llvm::Attribute::getWithAlignment(*context_, llvm::Align(ret_cls.raw_align)));
                            apply_byval_attrs(call_inst, 1);
                            return builder_.CreateLoad(ret_cls.raw_type, ret_slot);
                        }

                        auto *call_inst = builder_.CreateCall(callee, args);
                        apply_byval_attrs(call_inst, 0);
                        // Round-trip the coerced return value through memory to reinterpret its
                        // bytes back into the raw Mirage struct representation.
                        auto *coerced_slot = create_entry_alloca(current_function_, call_inst->getType(), "ext.ret.coerced");
                        builder_.CreateStore(call_inst, coerced_slot);
                        return builder_.CreateLoad(ret_cls.raw_type, coerced_slot);
                    }

                    auto *call_inst = builder_.CreateCall(callee, args);
                    apply_byval_attrs(call_inst, 0);
                    return call_inst;
                }
                return nullptr;
            }

            auto call_return_types(const ast::CallExpr &call) -> std::pair<std::string, std::vector<sema::ResolvedType>> {
                // A call resolved by sema to a specific generic function/method instantiation
                // (see emit_call's identical check and its own doc comment for why '&call' is
                // the right key here) - the TEMPLATE's own return_types (found via the plain
                // name/method lookups below) is always empty for a generic decl, so this must
                // be checked first or a 'try'/multi-return grouped call on one would compute
                // the wrong return arity (e.g. 0 instead of 1), corrupting emit_try_expr's own
                // CreateExtractValue indexing downstream.
                if (const auto inst_it = current_exprs_->expr_generic_fn_instance.find(&call); inst_it != current_exprs_->expr_generic_fn_instance.end()) {
                    const auto &instance = *sema_program_.generic_fn_instances[inst_it->second];
                    return {instance.module_path, instance.return_types};
                }

                std::string target_module = *current_module_path_;
                std::string name;

                if (const auto *ident = std::get_if<ast::IdentExpr>(&call.callee)) {
                    // Local fn-ptr
                    if (const auto it = locals_.find(ident->name); it != locals_.end()) {
                        if (it->second.type.kind == sema::TypeKind::Function) {
                            return {*current_module_path_, sema_program_.fn_signatures.at(it->second.type.fn_index).return_types};
                        }
                    }
                    name = ident->name;
                } else if (const auto *member = std::get_if<std::unique_ptr<ast::MemberExpr>>(&call.callee)) {
                    if (auto ns = try_namespace_chain((*member)->object)) {
                        target_module = *ns;
                        name = (*member)->member;
                    } else {
                        // Method call or struct-field fn-ptr
                        if (const auto dispatch_it = current_exprs_->expr_trait_dispatch.find(&call); dispatch_it != current_exprs_->expr_trait_dispatch.end()) {
                            const auto *trait_info = sema_program_.trait_at(dispatch_it->second.trait_index);
                            if (trait_info && dispatch_it->second.method_order_index >= 0 &&
                                static_cast<size_t>(dispatch_it->second.method_order_index) < trait_info->methods.size()) {
                                return {*current_module_path_, trait_info->methods[dispatch_it->second.method_order_index].return_types};
                            }
                        }
                        const auto obj_type = current_exprs_->expr_types.at(sema::get_expr_key((*member)->object));
                        sema::ResolvedType receiver_type = obj_type;
                        if (obj_type.kind == sema::TypeKind::Pointer) {
                            receiver_type = sema_program_.pointer_pointees.at(obj_type.pointee_index);
                        }
                        if (const auto *method = sema::find_method(receiver_type, (*member)->member, sema_program_)) {
                            return {*current_module_path_, method->return_types};
                        }
                        // Struct field fn-ptr
                        if (receiver_type.kind == sema::TypeKind::Struct) {
                            for (const auto &field : sema_program_.structs.at(receiver_type.struct_index).fields) {
                                if (field.name == (*member)->member && field.type.kind == sema::TypeKind::Function) {
                                    return {*current_module_path_, sema_program_.fn_signatures.at(field.type.fn_index).return_types};
                                }
                            }
                        }
                        report_codegen_error(diag_, call.location, "unsupported grouped declaration call target");
                        return {*current_module_path_, {}};
                    }
                } else {
                    // General expression callee
                    const auto callee_ty = current_exprs_->expr_types.at(sema::get_expr_key(call.callee));
                    if (callee_ty.kind == sema::TypeKind::Function) {
                        return {*current_module_path_, sema_program_.fn_signatures.at(callee_ty.fn_index).return_types};
                    }
                    report_codegen_error(diag_, call.location, "unsupported grouped declaration call target");
                    return {*current_module_path_, {}};
                }

                if (name.empty()) {
                    report_codegen_error(diag_, call.location, "unsupported grouped declaration call target");
                    return {*current_module_path_, {}};
                }

                const auto &target = module_for(target_module);
                const auto sym_it = target.symbols.find(name);
                if (sym_it == target.symbols.end()) {
                    report_codegen_error(diag_, call.location, std::format("unknown callable '{}'", name));
                    return {target_module, {}};
                }

                if (const auto *fn = std::get_if<sema::FunctionSymbol>(&sym_it->second)) {
                    return {target_module, fn->return_types};
                }
                if (const auto *ef = std::get_if<sema::ExtFunctionSymbol>(&sym_it->second)) {
                    std::vector<sema::ResolvedType> returns;
                    if (ef->return_type) returns.push_back(*ef->return_type);
                    return {target_module, std::move(returns)};
                }

                report_codegen_error(diag_, call.location, "grouped declaration initializer must be a function call");
                return {target_module, {}};
            }

            // Reads the outer Ok(0)/Failed(1) tag (a u32 at byte offset 0) out of an
            // error(...) union value. The value arrives as a first-class LLVM aggregate
            // (the union's [size x i8] representation), so it's spilled to a scratch
            // alloca first — there's no way to GEP into a non-lvalue SSA value directly.
            auto extract_error_tag(llvm::Value *err_val) -> llvm::Value * {
                auto *slot = create_entry_alloca(builder_.GetInsertBlock()->getParent(), err_val->getType());
                builder_.CreateStore(err_val, slot);
                return builder_.CreateLoad(llvm::Type::getInt32Ty(*context_), slot);
            }

            // Given an already-evaluated error(...) wrapper VALUE (known Failed — sema's
            // typestate precondition guarantees this at every match/switch call site), reads
            // out the inner 'effective_type' bytes: the single error member type directly, or
            // the synthesized inner dispatch union for 2+ members (see sema::ErrorMatchUnwrap).
            // Both live at the wrapper's Failed payload offset — the {v:T} wrapping struct's
            // one field is always at offset 0 within it, so no further GEP is needed once
            // there.
            auto unwrap_failed_error_value(llvm::Value *wrapper_val, const sema::ResolvedType &wrapper_type,
                                            const sema::ResolvedType &effective_type) -> llvm::Value * {
                const auto &wrapper = sema_program_.unions.at(wrapper_type.union_index);
                auto *wrapper_ll_ty = unions_.at(wrapper_type.union_index);
                auto *slot = create_entry_alloca(builder_.GetInsertBlock()->getParent(), wrapper_ll_ty);
                builder_.CreateStore(wrapper_val, slot);
                auto *payload_ptr = builder_.CreateConstInBoundsGEP1_64(llvm::Type::getInt8Ty(*context_), slot, wrapper.payload_offset);
                auto *effective_ll_ty = llvm_type(wrapper.module_path, effective_type);
                return builder_.CreateLoad(effective_ll_ty, payload_ptr);
            }

            // Builds a Failed-tagged value of 'error_union_type' carrying 'member_val' (of
            // sema type 'member_type', one of that error type's declared members). Shared by
            // return_err (member_val comes from evaluating the operand expression) and
            // try's cross-union translation (member_val comes from a runtime-dispatched
            // extraction out of a differently-shaped callee error value — see
            // translate_error_value).
            auto build_error_failed_value(const sema::ResolvedType &member_type, llvm::Value *member_val,
                                           const sema::ResolvedType &error_union_type) -> llvm::Value * {
                const auto &wrapper = sema_program_.unions.at(error_union_type.union_index);
                auto *wrapper_ll_ty = unions_.at(error_union_type.union_index);
                auto *outer_slot = create_entry_alloca(builder_.GetInsertBlock()->getParent(), wrapper_ll_ty);
                builder_.CreateStore(builder_.getInt32(1), outer_slot); // outer tag = Failed

                // 'Failed' is always variants[1] by construction (see synthesize_error_union
                // in type_resolver.cpp).
                const auto &failed_variant = wrapper.variants[1];
                auto *failed_struct_ll_ty = struct_lowering(failed_variant.payload_struct_index).type;
                const unsigned failed_field_idx = struct_lowering(failed_variant.payload_struct_index).field_indices.at(0);

                llvm::Value *failed_payload_val;
                if (wrapper.error_member_types.size() > 1) {
                    // 2+ error members: build the inner dispatch union {tag=member's inner
                    // tag, payload={v:member_val}} first, then wrap THAT inner union value in
                    // the outer Failed payload struct {v: InnerUnionType}.
                    const auto &inner = sema_program_.unions.at(failed_variant.payload_type.union_index);
                    auto *inner_ll_ty = unions_.at(failed_variant.payload_type.union_index);
                    const auto inner_variant_it = std::ranges::find_if(inner.variants,
                        [&](const auto &variant) { return variant.payload_type == member_type; });

                    auto *inner_slot = create_entry_alloca(builder_.GetInsertBlock()->getParent(), inner_ll_ty);
                    builder_.CreateStore(
                        llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), static_cast<uint64_t>(inner_variant_it->tag_value), false),
                        inner_slot);

                    auto *inner_payload_struct_ll_ty = struct_lowering(inner_variant_it->payload_struct_index).type;
                    const unsigned inner_field_idx = struct_lowering(inner_variant_it->payload_struct_index).field_indices.at(0);
                    llvm::Value *inner_payload_val = llvm::Constant::getNullValue(inner_payload_struct_ll_ty);
                    inner_payload_val = builder_.CreateInsertValue(inner_payload_val, member_val, {inner_field_idx});

                    auto *inner_payload_ptr = builder_.CreateConstInBoundsGEP1_64(llvm::Type::getInt8Ty(*context_), inner_slot, inner.payload_offset);
                    builder_.CreateStore(inner_payload_val, inner_payload_ptr);
                    auto *inner_val = builder_.CreateLoad(inner_ll_ty, inner_slot);

                    failed_payload_val = llvm::Constant::getNullValue(failed_struct_ll_ty);
                    failed_payload_val = builder_.CreateInsertValue(failed_payload_val, inner_val, {failed_field_idx});
                } else {
                    // Single error member: Failed's payload struct wraps the member value directly.
                    failed_payload_val = llvm::Constant::getNullValue(failed_struct_ll_ty);
                    failed_payload_val = builder_.CreateInsertValue(failed_payload_val, member_val, {failed_field_idx});
                }

                auto *outer_payload_ptr = builder_.CreateConstInBoundsGEP1_64(llvm::Type::getInt8Ty(*context_), outer_slot, wrapper.payload_offset);
                builder_.CreateStore(failed_payload_val, outer_payload_ptr);
                return builder_.CreateLoad(wrapper_ll_ty, outer_slot);
            }

            // Translates a Failed value from callee_error_ty's representation into
            // caller_error_ty's representation. Only called when the two are DIFFERENT
            // synthesized unions (see emit_try_propagation's identical-union fast path) —
            // i.e. the callee's error(...) spelling is a strict subset of the caller's.
            // Payload bytes for a given member type are identical between any two error
            // unions that both carry it (both wrap it via the same one-field struct
            // convention); only the dispatch TAG differs, so this only ever needs to
            // re-tag, never convert, the payload.
            auto translate_error_value(llvm::Value *err_val, const sema::ResolvedType &callee_error_ty,
                                        const sema::ResolvedType &caller_error_ty) -> llvm::Value * {
                const auto &callee_wrapper = sema_program_.unions.at(callee_error_ty.union_index);
                auto *callee_ll_ty = unions_.at(callee_error_ty.union_index);
                auto *caller_ll_ty = unions_.at(caller_error_ty.union_index);

                auto *slot = create_entry_alloca(builder_.GetInsertBlock()->getParent(), callee_ll_ty);
                builder_.CreateStore(err_val, slot);
                auto *failed_payload_ptr = builder_.CreateConstInBoundsGEP1_64(llvm::Type::getInt8Ty(*context_), slot, callee_wrapper.payload_offset);

                const auto &failed_variant = callee_wrapper.variants[1];

                if (callee_wrapper.error_member_types.size() == 1) {
                    // Single-member callee: the member type is fixed at codegen-authoring
                    // time, no runtime dispatch needed.
                    const auto &member_type = failed_variant.payload_type;
                    auto *member_ll_ty = llvm_type(callee_wrapper.module_path, member_type);
                    auto *member_val = builder_.CreateLoad(member_ll_ty, failed_payload_ptr);
                    return build_error_failed_value(member_type, member_val, caller_error_ty);
                }

                // Multi-member callee: dispatch at runtime on the inner tag. One switch case
                // per POSSIBLE callee member (a compile-time-known list), each using its own
                // concrete member type to drive build_error_failed_value.
                const auto &inner = sema_program_.unions.at(failed_variant.payload_type.union_index);
                auto *inner_tag = builder_.CreateLoad(llvm::Type::getInt32Ty(*context_), failed_payload_ptr);

                auto *fn = builder_.GetInsertBlock()->getParent();
                auto *merge_bb = llvm::BasicBlock::Create(*context_, "try.translate.merge", fn);
                auto *unreachable_bb = llvm::BasicBlock::Create(*context_, "try.translate.unreachable", fn);
                auto *result_slot = create_entry_alloca(builder_.GetInsertBlock()->getParent(), caller_ll_ty);
                auto *sw = builder_.CreateSwitch(inner_tag, unreachable_bb, static_cast<unsigned>(inner.variants.size()));

                for (const auto &variant : inner.variants) {
                    auto *case_bb = llvm::BasicBlock::Create(*context_, "try.translate." + variant.name, fn);
                    sw->addCase(llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), static_cast<uint64_t>(variant.tag_value), false), case_bb);
                    builder_.SetInsertPoint(case_bb);
                    auto *inner_payload_ptr = builder_.CreateConstInBoundsGEP1_64(llvm::Type::getInt8Ty(*context_), failed_payload_ptr, inner.payload_offset);
                    auto *member_ll_ty = llvm_type(inner.module_path, variant.payload_type);
                    auto *member_val = builder_.CreateLoad(member_ll_ty, inner_payload_ptr);
                    builder_.CreateStore(build_error_failed_value(variant.payload_type, member_val, caller_error_ty), result_slot);
                    builder_.CreateBr(merge_bb);
                }

                builder_.SetInsertPoint(unreachable_bb);
                builder_.CreateUnreachable();

                builder_.SetInsertPoint(merge_bb);
                return builder_.CreateLoad(caller_ll_ty, result_slot);
            }

            // Returns err_val (or, if the callee's error(...) union differs from the
            // enclosing function's, its translation — see translate_error_value) as the
            // enclosing function's error result, leaving any non-error slots undefined.
            // Defers run first. Shared by `try` propagation and `return_err` so both lower
            // identically.
            void emit_error_return(llvm::Value *err_val) {
                emit_defers_for_return();

                if (current_returns_.empty()) {
                    // Sema should have caught this; emit unreachable as a safeguard
                    builder_.CreateUnreachable();
                } else if (current_returns_.size() == 1) {
                    // Enclosing fn returns just error
                    builder_.CreateRet(err_val);
                } else {
                    // Enclosing fn returns (T1, ..., error): non-error slots are left
                    // undefined, propagate error
                    auto *agg_ty = llvm::cast<llvm::StructType>(return_type(*current_module_path_, current_returns_));
                    llvm::Value *agg = llvm::UndefValue::get(agg_ty);
                    agg = builder_.CreateInsertValue(agg, err_val, {static_cast<unsigned>(current_returns_.size() - 1)});
                    builder_.CreateRet(agg);
                }
            }

            auto emit_try_propagation(llvm::Value *err_val, const sema::ResolvedType &callee_error_ty, const SourceLocation &loc) -> llvm::BasicBlock * {
                auto *fn = builder_.GetInsertBlock()->getParent();
                auto *propagate_bb = llvm::BasicBlock::Create(*context_, "try.propagate", fn);
                auto *ok_bb = llvm::BasicBlock::Create(*context_, "try.ok", fn);
                auto *tag = extract_error_tag(err_val);
                auto *is_err = builder_.CreateICmpNE(tag, builder_.getInt32(0));
                builder_.CreateCondBr(is_err, propagate_bb, ok_bb);

                builder_.SetInsertPoint(propagate_bb);
                const auto &caller_error_ty = current_returns_.back();
                llvm::Value *propagated = (callee_error_ty == caller_error_ty)
                    ? err_val
                    : translate_error_value(err_val, callee_error_ty, caller_error_ty);
                emit_error_return(propagated);

                builder_.SetInsertPoint(ok_bb);
                return ok_bb;
            }

            auto emit_try_expr(const ast::TryExpr &expr) -> llvm::Value * {
                const auto *call = std::get_if<std::unique_ptr<ast::CallExpr>>(&expr.call);
                auto *result = emit_call(**call);
                const auto [type_module, returns] = call_return_types(**call);

                // Extract the error value (last return slot)
                llvm::Value *err_val;
                if (returns.size() == 1) {
                    err_val = result; // f() -> error(...): result IS the error
                } else {
                    err_val = builder_.CreateExtractValue(result, {static_cast<unsigned>(returns.size() - 1)});
                }

                emit_try_propagation(err_val, returns.back(), expr.location);

                // On the ok path: return the non-error value (if any)
                if (returns.size() == 1) {
                    // f() -> error(...): expression is void; return a dummy value
                    return llvm::UndefValue::get(llvm::Type::getInt8Ty(*context_));
                }
                if (returns.size() == 2) {
                    // f() -> T, error(...): extract T from the aggregate
                    return builder_.CreateExtractValue(result, {0});
                }
                // Multi-value try in expression position: sema rejects this; return first value
                return builder_.CreateExtractValue(result, {0});
            }

            auto emit_expr(const ast::Expr &expr) -> llvm::Value * {
                return std::visit(
                    [&]<typename T>(const T &v) -> llvm::Value * {
                        using V = std::decay_t<T>;
                        const auto ty = expr_type(expr);

                        if constexpr (std::is_same_v<V, ast::LiteralIntegerExpr>) {
                            if (ty.is_float()) {
                                return llvm::ConstantFP::get(llvm_type(*current_module_path_, ty), static_cast<double>(v.value));
                            }
                            return llvm::ConstantInt::get(llvm_type(*current_module_path_, ty), v.value, ty.is_signed());
                        } else if constexpr (std::is_same_v<V, ast::LiteralFloatExpr>) {
                            return llvm::ConstantFP::get(llvm_type(*current_module_path_, ty), v.value);
                        } else if constexpr (std::is_same_v<V, ast::LiteralStringExpr>) {
                            return emit_string_literal(v.value);
                        } else if constexpr (std::is_same_v<V, ast::LiteralCharExpr>) {
                            return builder_.getInt8(v.value);
                        } else if constexpr (std::is_same_v<V, ast::LiteralBoolExpr>) {
                            return builder_.getInt1(v.value);
                        } else if constexpr (std::is_same_v<V, ast::LiteralNilExpr>) {
                            if (ty.kind == sema::TypeKind::Slice || ty.kind == sema::TypeKind::Trait)
                                return llvm::ConstantAggregateZero::get(llvm_type(*current_module_path_, ty));
                            return llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context_));
                        } else if constexpr (std::is_same_v<V, ast::IdentExpr>) {
                            if (const auto macro = macro_args_.find(v.name); macro != macro_args_.end()) {
                                return emit_macro_arg(macro->second);
                            }
                            // Taking a named function as a value (fn-ptr)
                            if (ty.kind == sema::TypeKind::Function) {
                                const auto sym_it = current_module_->symbols.find(v.name);
                                if (sym_it != current_module_->symbols.end()) {
                                    if (std::holds_alternative<sema::FunctionSymbol>(sym_it->second)) {
                                        return functions_.at(FunctionKey{*current_module_path_, v.name});
                                    }
                                    if (std::holds_alternative<sema::ExtFunctionSymbol>(sym_it->second)) {
                                        return ext_functions_.at(global_key(*current_module_path_, v.name));
                                    }
                                }
                            }
                            const auto lv = emit_lvalue(expr);
                            return builder_.CreateLoad(lv.storage_type, lv.ptr);
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::UnaryExpr>>) {
                            if (v->op == ast::UnaryOp::AddressOf) {
                                return emit_lvalue(v->operand).ptr;
                            }
                            if (v->op == ast::UnaryOp::Deref) {
                                const auto lv = emit_lvalue(expr);
                                return builder_.CreateLoad(lv.storage_type, lv.ptr);
                            }
                            auto *operand = emit_expr(v->operand);
                            const auto operand_ty = current_exprs_->expr_types.at(sema::get_expr_key(v->operand));
                            switch (v->op) {
                            case ast::UnaryOp::Negate:     return operand_ty.is_float() ? builder_.CreateFNeg(operand) : builder_.CreateNeg(operand);
                            case ast::UnaryOp::LogicalNot: return builder_.CreateNot(coerce_to_bool(operand, operand_ty));
                            case ast::UnaryOp::BitwiseNot: return builder_.CreateNot(operand);
                            default:                       return operand;
                            }
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::BinaryExpr>>) {
                            return emit_binary_expr(*v);
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::TernaryExpr>>) {
                            return emit_ternary(*v, ty);
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::AssignExpr>>) {
                            return emit_assign(*v);
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::CallExpr>>) {
                            if (const auto inst_it = current_exprs_->expr_generic_fn_instance.find(sema::get_expr_key(expr));
                                inst_it != current_exprs_->expr_generic_fn_instance.end()) {
                                return emit_generic_call(*v, inst_it->second);
                            }
                            return emit_call(*v);
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::IncrDecrExpr>>) {
                            return emit_incr_decr(*v);
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::SizeOfExpr>>) {
                            return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), sizeof_operand(*v));
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::AlignOfExpr>>) {
                            return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), align_of_operand(*v));
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::TypeOfExpr>>) {
                            // Unlike a generic value operand, an operand that names a TYPE (e.g.
                            // 'type_of(Point)') never went through check_expr (see sema's mirrored
                            // ident/member special-case) and so has no expr_types entry of its own
                            // - .find() rather than .at() here, matching type_of_operand's own
                            // ident/member-vs-generic split below.
                            const auto ty_it = current_exprs_->expr_types.find(sema::get_expr_key(v->operand));
                            if (ty_it != current_exprs_->expr_types.end() && ty_it->second.kind == sema::TypeKind::Any) {
                                // Runtime load of the any value's 'id' field (word 0).
                                return builder_.CreateExtractValue(emit_expr(v->operand), {0});
                            }
                            return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), type_of_operand(*v));
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::TypeInfoOfExpr>>) {
                            // Fast path: 'type_info_of(type_of(T))' — sema already folded T's id
                            // at check_expr time (see expr_type_info_const_id). Nil for builtin
                            // scalar ids (1-15); else the (already emitted) '__type_info_<id>'
                            // global's address.
                            if (const auto it = current_exprs_->expr_type_info_const_id.find(sema::get_expr_key(expr)); it != current_exprs_->expr_type_info_const_id.end()) {
                                const auto id = it->second;
                                if (id >= 1 && id <= 15) {
                                    return llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context_));
                                }
                                const auto ty_it = sema_program_.types_needing_info_types.find(id);
                                if (ty_it != sema_program_.types_needing_info_types.end()) {
                                    return type_info_ptr_for(ty_it->second);
                                }
                                return llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context_));
                            }
                            // Runtime path: a non-constant 'type' value, or an 'any' — inline
                            // binary search over '__mirage_type_info_table'.
                            return emit_type_info_runtime_lookup(*v);
                        } else if constexpr (std::is_same_v<V, ast::ImportBinExpr>) {
                            return emit_import_bin(v);
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::LenExpr>>) {
                            const auto operand_type = current_exprs_->expr_types.at(sema::get_expr_key(v->operand));
                            if (operand_type.kind == sema::TypeKind::Array) {
                                return builder_.getInt64(sema_program_.arrays.at(operand_type.array_index).count);
                            }
                            auto *slice = emit_expr(v->operand);
                            return builder_.CreateExtractValue(slice, {1});
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::StackAllocExpr>>) {
                            const auto size_from = current_exprs_->expr_types.at(sema::get_expr_key(v->size));
                            auto *size = emit_cast(emit_expr(v->size), size_from, sema::ResolvedType{.kind = sema::TypeKind::USize});
                            auto *alloc = builder_.CreateAlloca(llvm::Type::getInt8Ty(*context_), size, "stackalloc");
                            alloc->setAlignment(llvm::Align(16));
                            return alloc;
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::AsmExpr>>) {
                            return emit_asm_expr(*v, ty);
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::CastExpr>>) {
                            const auto from = current_exprs_->expr_types.at(sema::get_expr_key(v->value));
                            if (ty.kind == sema::TypeKind::Slice) {
                                return emit_slice_cast(*v, from, ty);
                            }
                            if (from.kind == sema::TypeKind::Array && is_pointer_like(ty)) {
                                auto lv = emit_lvalue(v->value);
                                return builder_.CreateInBoundsGEP(
                                    llvm_type(*current_module_path_, from),
                                    lv.ptr,
                                    {builder_.getInt32(0), builder_.getInt64(0)});
                            }
                            if (from.kind == sema::TypeKind::Slice && is_pointer_like(ty)) {
                                return builder_.CreateExtractValue(emit_expr(v->value), {0});
                            }
                            if (from.kind == sema::TypeKind::Any) {
                                // 'any' -> pointer/anyptr (sema guarantees 'ty' is one of these):
                                // extract the fat value's data word (word 1 of {id, data}).
                                return builder_.CreateExtractValue(emit_expr(v->value), {1});
                            }
                            return emit_cast(emit_expr(v->value), from, ty);
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::IndexOrInstantiateExpr>>) {
                            // A generic function instantiation naming a monomorphized instance as
                            // a function-pointer value ('fnv1a[i32]') — sema recorded which
                            // instance in expr_generic_fn_instance (see
                            // try_resolve_generic_fn_instantiation_value), and
                            // declare_generic_functions() already created the llvm::Function, so
                            // this is just the address of that. Mirrors the IdentExpr fn-ptr path.
                            if (ty.kind == sema::TypeKind::Function) {
                                const auto &table = current_exprs_->expr_generic_fn_instance;
                                if (const auto it = table.find(sema::get_expr_key(expr)); it != table.end()) {
                                    const auto &instance = *sema_program_.generic_fn_instances[it->second];
                                    return functions_.at(FunctionKey{instance.module_path, instance.mangled_name});
                                }
                            }
                            const auto lv = emit_lvalue(expr);
                            return builder_.CreateLoad(lv.storage_type, lv.ptr);
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::SliceExpr>>) {
                            return emit_slice_expr(*v, ty);
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::MemberExpr>>) {
                            // Cross-module function pointer taking: mod.fn_name
                            if (ty.kind == sema::TypeKind::Function) {
                                if (const auto target_mod = try_namespace_chain(v->object)) {
                                    const auto &target = module_for(*target_mod);
                                    const auto sym_it = target.symbols.find(v->member);
                                    if (sym_it != target.symbols.end()) {
                                        if (std::holds_alternative<sema::FunctionSymbol>(sym_it->second)) {
                                            return functions_.at(FunctionKey{*target_mod, v->member});
                                        }
                                        if (std::holds_alternative<sema::ExtFunctionSymbol>(sym_it->second)) {
                                            return ext_functions_.at(global_key(*target_mod, v->member));
                                        }
                                    }
                                }
                            }
                            // Handle fully-qualified enum field: e.g. EnumType.field or module.EnumType.field
                            if (ty.kind == sema::TypeKind::Enum && try_type_chain(v->object)) {
                                const auto &enum_info = sema_program_.enums.at(ty.enum_index);
                                for (const auto &field : enum_info.fields) {
                                    if (field.name == v->member) {
                                        return llvm::ConstantInt::get(llvm_type(*current_module_path_, ty),
                                                                      static_cast<uint64_t>(field.value),
                                                                      enum_info.underlying_type.is_signed());
                                    }
                                }
                                report_codegen_error(diag_, v->location, std::format("unknown enum field '{}'", v->member));
                                return llvm::UndefValue::get(llvm_type(*current_module_path_, ty));
                            }
                            // Fully-qualified bitset member: e.g. BitsetType.field
                            if (ty.kind == sema::TypeKind::Bitset && try_type_chain(v->object)) {
                                const auto &bitset_info = sema_program_.bitsets.at(ty.bitset_index);
                                const auto mask = bitset_member_mask(bitset_info, v->member, v->location);
                                return llvm::ConstantInt::get(llvm_type(*current_module_path_, ty), mask.value_or(0), false);
                            }
                            const auto lv = emit_lvalue(expr);
                            return builder_.CreateLoad(lv.storage_type, lv.ptr);
                        } else if constexpr (std::is_same_v<V, ast::DotIdentExpr>) {
                            if (ty.kind == sema::TypeKind::Union) {
                                // Payload-free tagged union variant: .variant_name
                                const auto &union_info = sema_program_.unions.at(ty.union_index);
                                const auto variant_it = std::ranges::find(union_info.variants, v.name, &sema::TaggedUnionVariant::name);
                                if (variant_it == union_info.variants.end()) {
                                    report_codegen_error(diag_, v.location, std::format("unknown variant '{}'", v.name));
                                    return llvm::UndefValue::get(unions_.at(ty.union_index));
                                }
                                // Emit as a tagged variant with no payload
                                auto *ll_ty = unions_.at(ty.union_index);
                                auto *slot = create_entry_alloca(builder_.GetInsertBlock()->getParent(), ll_ty);
                                auto *tag_val = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_),
                                                                        static_cast<uint64_t>(variant_it->tag_value), false);
                                builder_.CreateStore(tag_val, slot);
                                return builder_.CreateLoad(ll_ty, slot);
                            }
                            if (ty.kind == sema::TypeKind::Bitset) {
                                // Bitset member literal: .field_name — folds to its one-bit mask
                                const auto &bitset_info = sema_program_.bitsets.at(ty.bitset_index);
                                const auto mask = bitset_member_mask(bitset_info, v.name, v.location);
                                return llvm::ConstantInt::get(llvm_type(*current_module_path_, ty), mask.value_or(0), false);
                            }
                            // Enum field literal: .field_name
                            // ty is the enum type; look up the field value
                            const auto &enum_info = sema_program_.enums.at(ty.enum_index);
                            for (const auto &field : enum_info.fields) {
                                if (field.name == v.name) {
                                    return llvm::ConstantInt::get(llvm_type(*current_module_path_, ty),
                                                                  static_cast<uint64_t>(field.value),
                                                                  enum_info.underlying_type.is_signed());
                                }
                            }
                            report_codegen_error(diag_, v.location, std::format("unknown enum field '{}'", v.name));
                            return llvm::UndefValue::get(llvm_type(*current_module_path_, ty));
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::MatchExpr>>) {
                            return emit_match(*v, ty);
                        } else if constexpr (std::is_same_v<V, ast::IotaExpr>) {
                            report_codegen_error(diag_, v.location, "iota is not valid in this context");
                            return llvm::UndefValue::get(llvm_type(*current_module_path_, ty));
                        } else if constexpr (std::is_same_v<V, ast::DefaultExpr>) {
                            const std::string tm =
                                ty.kind == sema::TypeKind::Struct ? sema_program_.structs.at(ty.struct_index).module_path
                                : ty.kind == sema::TypeKind::Union ? sema_program_.unions.at(ty.union_index).module_path
                                : *current_module_path_;
                            return emit_default_value(tm, ty);
                        } else if constexpr (std::is_same_v<V, ast::UndefinedExpr>) {
                            return llvm::UndefValue::get(llvm_type(*current_module_path_, ty));
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::TaggedVariantExpr>>) {
                            return emit_tagged_variant_expr(*v, ty);
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::TryExpr>>) {
                            return emit_try_expr(*v);
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::BracedInitializerExpr>>) {
                            return std::visit(
                                [&]<typename BV>(const BV &bv) -> llvm::Value * {
                                    using BVT = std::decay_t<BV>;
                                    if constexpr (std::is_same_v<BVT, ast::EmptyExpr>) {
                                        const std::string tm =
                                            ty.kind == sema::TypeKind::Struct ? sema_program_.structs.at(ty.struct_index).module_path
                                            : ty.kind == sema::TypeKind::Union ? sema_program_.unions.at(ty.union_index).module_path
                                            : *current_module_path_;
                                        return emit_default_value(tm, ty);
                                    } else if constexpr (std::is_same_v<BVT, ast::StructExpr>) {
                                        if (ty.kind == sema::TypeKind::Union) {
                                            return emit_union_expr_value(bv, ty);
                                        }
                                        return emit_struct_expr_value(bv, ty);
                                    } else if constexpr (std::is_same_v<BVT, ast::BitsetExpr>) {
                                        return emit_bitset_expr_value(bv, ty);
                                    } else {  // ast::ArrayExpr
                                        if (ty.kind == sema::TypeKind::Struct) {
                                            return emit_positional_struct_expr_value(bv, ty);
                                        }
                                        return emit_array_expr_value(bv, ty);
                                    }
                                },
                                *v);
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::OptionExpr>> ||
                                              std::is_same_v<V, std::unique_ptr<ast::EnvExpr>>) {
                            return emit_option_value(expr, ty);
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::WhenExpr>>) {
                            return emit_when_expr(*v, expr, ty);
                        } else {
                            report_codegen_error(diag_, {}, "unsupported expression in codegen");
                            return llvm::UndefValue::get(llvm_type(*current_module_path_, ty));
                        }
                    },
                    expr);
            }

            auto emit_string_literal(const std::string &value) -> llvm::Constant * {
                std::vector<uint8_t> bytes(value.begin(), value.end());
                bytes.push_back(0);
                auto *array_ty = llvm::ArrayType::get(llvm::Type::getInt8Ty(*context_), bytes.size());
                std::vector<llvm::Constant *> chars;
                for (uint8_t byte : bytes) {
                    chars.push_back(builder_.getInt8(byte));
                }
                auto *init = llvm::ConstantArray::get(array_ty, chars);
                auto *global = new llvm::GlobalVariable(
                    *module_, array_ty, true, llvm::GlobalValue::PrivateLinkage, init,
                    std::format(".str.{}", string_counter_++));
                llvm::Constant *indices[] = {builder_.getInt32(0), builder_.getInt32(0)};
                auto *ptr = llvm::ConstantExpr::getInBoundsGetElementPtr(
                    array_ty, global, llvm::ArrayRef(indices));
                auto *len = llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), value.size());
                auto *slice_ty = llvm::StructType::get(*context_,
                    {llvm::PointerType::getUnqual(*context_), llvm::Type::getInt64Ty(*context_)});
                return llvm::ConstantStruct::get(slice_ty, {ptr, len});
            }

            auto build_slice_value(llvm::Value *ptr, llvm::Value *count, const sema::ResolvedType &slice_type, const std::string &type_module) -> llvm::Value * {
                if (!count->getType()->isIntegerTy(64)) {
                    count = integer_cast(count, llvm::Type::getInt64Ty(*context_), sema::ResolvedType{.kind = sema::TypeKind::USize});
                }
                llvm::Value *slice = llvm::UndefValue::get(llvm_type(type_module, slice_type));
                slice = builder_.CreateInsertValue(slice, ptr, {0});
                slice = builder_.CreateInsertValue(slice, count, {1});
                return slice;
            }

            // Builds a trait handle's two-word value: word 0 = data pointer (the source *T,
            // already an opaque 'ptr' — no cast instruction needed, see is_pointer_like's
            // callers), word 1 = the (trait, type) vtable global's address. Mirrors
            // build_slice_value above exactly.
            auto build_trait_handle_value(llvm::Value *data_ptr, llvm::Value *vtable_ptr, const sema::ResolvedType &trait_type, const std::string &type_module) -> llvm::Value * {
                llvm::Value *handle = llvm::UndefValue::get(llvm_type(type_module, trait_type));
                handle = builder_.CreateInsertValue(handle, data_ptr, {0});
                handle = builder_.CreateInsertValue(handle, vtable_ptr, {1});
                return handle;
            }

            // Builds the slice passed for the trailing loose arguments of a native '...T' variadic
            // call (i.e. NOT the spread-passthrough case, which forwards an existing slice directly).
            // Stack-allocates a '[N x T]' array in the caller, stores each argument into it left to
            // right, then wraps it in a slice header. N=0 reuses the ordinary nil-slice convention.
            auto emit_variadic_tail_slice(const std::vector<ast::Expr> &args, size_t fixed_count, const sema::ResolvedType &slice_ty, const std::string &callee_module) -> llvm::Value * {
                const size_t n = args.size() - fixed_count;
                if (n == 0) {
                    return llvm::ConstantAggregateZero::get(llvm_type(callee_module, slice_ty));
                }
                const auto elem_ty = sema_program_.slices.at(slice_ty.slice_index).element_type;
                auto *elem_ll = llvm_type(callee_module, elem_ty);
                auto *arr_ll = llvm::ArrayType::get(elem_ll, n);
                llvm::Value *agg = llvm::Constant::getNullValue(arr_ll);
                for (size_t i = 0; i < n; ++i) {
                    auto *elem_val = emit_value_as(args[fixed_count + i], elem_ty, callee_module);
                    agg = builder_.CreateInsertValue(agg, elem_val, {static_cast<unsigned>(i)});
                }
                auto *slot = create_entry_alloca(current_function_, arr_ll, "variadic.tmp");
                builder_.CreateStore(agg, slot);
                auto *ptr = builder_.CreateInBoundsGEP(arr_ll, slot, {builder_.getInt32(0), builder_.getInt32(0)});
                return build_slice_value(ptr, builder_.getInt64(n), slice_ty, callee_module);
            }

            auto emit_slice_cast(const ast::CastExpr &expr, const sema::ResolvedType &from, const sema::ResolvedType &to) -> llvm::Value * {
                const auto type_module = type_module_for_ast_type(expr.as_type, *current_module_path_, to);
                if (from.kind == sema::TypeKind::Array) {
                    return emit_array_to_slice(expr.value, from, to, expr_type_module_hint(expr.value));
                }
                if (from.kind == sema::TypeKind::Slice) {
                    return emit_expr(expr.value);
                }
                auto *ptr = emit_expr(expr.value);
                auto *count = expr.len_expr ? emit_expr(*expr.len_expr) : builder_.getInt64(0);
                return build_slice_value(ptr, count, to, type_module);
            }

            auto emit_slice_expr(const ast::SliceExpr &expr, const sema::ResolvedType &result_type) -> llvm::Value * {
                const auto operand_type = current_exprs_->expr_types.at(sema::get_expr_key(expr.operand));
                const auto type_module = expr_type_module_hint(expr.operand);
                auto *start = emit_expr(expr.start);
                auto *end = emit_expr(expr.end);
                if (!start->getType()->isIntegerTy(64)) start = integer_cast(start, llvm::Type::getInt64Ty(*context_), current_exprs_->expr_types.at(sema::get_expr_key(expr.start)));
                if (!end->getType()->isIntegerTy(64)) end = integer_cast(end, llvm::Type::getInt64Ty(*context_), current_exprs_->expr_types.at(sema::get_expr_key(expr.end)));
                auto *count = builder_.CreateSub(end, start);

                if (operand_type.kind == sema::TypeKind::Array) {
                    auto base = emit_lvalue(expr.operand);
                    auto *ptr = builder_.CreateInBoundsGEP(llvm_type(type_module, operand_type), base.ptr, {builder_.getInt32(0), start});
                    return build_slice_value(ptr, count, result_type, type_module);
                }

                auto *slice = emit_expr(expr.operand);
                auto *base = builder_.CreateExtractValue(slice, {0});
                const auto element = sema_program_.slices.at(operand_type.slice_index).element_type;
                auto *ptr = builder_.CreateInBoundsGEP(llvm_type_for(element, type_module), base, start);
                return build_slice_value(ptr, count, result_type, type_module);
            }

            // Emit a tagged-union payload capture binding for a match/switch arm.
            // Returns the arm_locals (with capture bound if present).
            auto emit_variant_capture(
                    llvm::Function *fn,
                    llvm::Value *union_slot,
                    const sema::UnionInfo &union_info,
                    const sema::TaggedUnionVariant &variant,
                    const ast::MatchExpr::VariantPattern &vp) -> void {
                if (!vp.capture_name || variant.payload_struct_index < 0) return;
                const sema::ResolvedType payload_ty = variant.payload_type;
                const auto &struct_module = union_info.module_path;
                auto *payload_ll_ty = llvm_type(struct_module, payload_ty);
                auto *payload_ptr = builder_.CreateConstInBoundsGEP1_64(
                    llvm::Type::getInt8Ty(*context_), union_slot, union_info.payload_offset, "match.payload");
                if (vp.capture_by_ref) {
                    auto *ref_slot = create_entry_alloca(fn, llvm::PointerType::getUnqual(*context_), *vp.capture_name);
                    builder_.CreateStore(payload_ptr, ref_slot);
                    const auto ptr_ty = find_self_ptr_type(payload_ty);
                    // struct_module (the payload's own declaring module), not the match site's:
                    // the by-value branch below already uses it, and type_module is meant to name
                    // where '.type' was declared. No observable difference today, since aggregate
                    // ResolvedTypes resolve through a global index rather than by
                    // (module_path, name) -- but a latent trap for any future path that starts
                    // trusting LocalValue::type_module for name-based lookup.
                    locals_[*vp.capture_name] = LocalValue{.alloca = ref_slot, .type = ptr_ty, .type_module = struct_module};
                } else {
                    auto *val_slot = create_entry_alloca(fn, payload_ll_ty, *vp.capture_name);
                    auto *loaded = builder_.CreateLoad(payload_ll_ty, payload_ptr);
                    builder_.CreateStore(loaded, val_slot);
                    locals_[*vp.capture_name] = LocalValue{.alloca = val_slot, .type = payload_ty, .type_module = struct_module};
                }
            }

            auto emit_match(const ast::MatchExpr &expr, const sema::ResolvedType &result_type) -> llvm::Value * {
                auto *fn = builder_.GetInsertBlock()->getParent();
                auto *merge_bb = llvm::BasicBlock::Create(*context_, "match.end", fn);

                auto operand_type = current_exprs_->expr_types.at(sema::get_expr_key(expr.operand));

                // Transparent error-value matching: substitute the wrapper's Ok/Failed type
                // for the inner representation sema actually dispatched arms against, and
                // pre-evaluate the unwrapped value once (every branch below reads it via
                // emit_operand() rather than re-evaluating expr.operand as the wrapper type).
                llvm::Value *unwrapped_operand_val = nullptr;
                if (const auto it = current_exprs_->expr_error_match_unwrap.find(sema::get_expr_key(expr.operand));
                    it != current_exprs_->expr_error_match_unwrap.end()) {
                    auto *wrapper_val = emit_expr(expr.operand);
                    unwrapped_operand_val = unwrap_failed_error_value(wrapper_val, it->second.wrapper_type, it->second.effective_type);
                    operand_type = it->second.effective_type;
                }
                const auto emit_operand = [&]() -> llvm::Value * {
                    return unwrapped_operand_val ? unwrapped_operand_val : emit_expr(expr.operand);
                };

                // Find default arm (if any)
                const ast::MatchExpr::Arm *default_arm = nullptr;
                for (const auto &arm : expr.arms) {
                    if (std::holds_alternative<ast::MatchExpr::DefaultPattern>(arm.pattern)) {
                        default_arm = &arm;
                        break;
                    }
                }
                // Default successor: the default arm's BB, or an unreachable sentinel
                auto *default_bb = default_arm
                    ? llvm::BasicBlock::Create(*context_, "match.default", fn)
                    : llvm::BasicBlock::Create(*context_, "match.unreachable", fn);

                std::vector<std::pair<llvm::BasicBlock *, llvm::Value *>> arm_results;
                const unsigned non_default_count = static_cast<unsigned>(
                    std::ranges::count_if(expr.arms, [](const auto &a) {
                        return !std::holds_alternative<ast::MatchExpr::DefaultPattern>(a.pattern);
                    }));

                // ---- Tagged union match ----
                if (operand_type.kind == sema::TypeKind::Union) {
                    const auto &union_info = sema_program_.unions.at(operand_type.union_index);
                    auto *union_ll_ty = unions_.at(operand_type.union_index);
                    // Hoisted to the entry block, like every other scratch slot in this file.
                    // Allocated at the current insertion point, this sits inside any
                    // enclosing loop body, and LLVM lowers a non-entry alloca as a dynamic
                    // stack adjustment that is only reclaimed when the function returns --
                    // so each iteration grew the frame. A 2M-iteration loop switching on a
                    // tagged union exhausted the stack and died with SIGSEGV.
                    auto *union_slot = create_entry_alloca(fn, union_ll_ty, "match.union");
                    auto *operand_val = emit_operand();
                    builder_.CreateStore(operand_val, union_slot);
                    auto *tag = builder_.CreateLoad(llvm::Type::getInt32Ty(*context_), union_slot, "match.tag");
                    auto *sw = builder_.CreateSwitch(tag, default_bb, non_default_count);

                    const auto saved_locals = locals_;
                    for (const auto &arm : expr.arms) {
                        if (std::holds_alternative<ast::MatchExpr::DefaultPattern>(arm.pattern)) continue;
                        const auto &vp = std::get<ast::MatchExpr::VariantPattern>(arm.pattern);
                        int32_t tag_val = 0;
                        const sema::TaggedUnionVariant *matched_variant = nullptr;
                        for (const auto &variant : union_info.variants) {
                            if (variant.name == vp.name) {
                                tag_val = variant.tag_value;
                                matched_variant = &variant;
                                break;
                            }
                        }
                        auto *arm_bb = llvm::BasicBlock::Create(*context_, std::format("match.arm.{}", vp.name), fn);
                        sw->addCase(llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), static_cast<uint64_t>(tag_val), false), arm_bb);
                        builder_.SetInsertPoint(arm_bb);
                        if (matched_variant) emit_variant_capture(fn, union_slot, union_info, *matched_variant, vp);
                        auto *arm_val = emit_value_as(arm.value, result_type, *current_module_path_);
                        auto *arm_done = builder_.GetInsertBlock();
                        builder_.CreateBr(merge_bb);
                        arm_results.emplace_back(arm_done, arm_val);
                        locals_ = saved_locals;
                    }
                    // Emit default arm or unreachable
                    builder_.SetInsertPoint(default_bb);
                    if (default_arm) {
                        auto *arm_val = emit_value_as(default_arm->value, result_type, *current_module_path_);
                        auto *arm_done = builder_.GetInsertBlock();
                        builder_.CreateBr(merge_bb);
                        arm_results.emplace_back(arm_done, arm_val);
                    } else {
                        builder_.CreateUnreachable();
                    }
                    builder_.SetInsertPoint(merge_bb);
                    if (result_type.kind == sema::TypeKind::Void || arm_results.empty()) {
                        return llvm::UndefValue::get(llvm::Type::getInt32Ty(*context_));
                    }
                    auto *phi = builder_.CreatePHI(llvm_type(*current_module_path_, result_type), static_cast<unsigned>(arm_results.size()));
                    for (auto &[bb, val] : arm_results) phi->addIncoming(val, bb);
                    return phi;
                }

                // ---- Enum match ----
                if (operand_type.kind == sema::TypeKind::Enum) {
                    auto *operand = emit_operand();
                    const auto &enum_info = sema_program_.enums.at(operand_type.enum_index);
                    auto *underlying_llvm_ty = llvm_type(*current_module_path_, enum_info.underlying_type);
                    auto *sw = builder_.CreateSwitch(operand, default_bb, non_default_count);

                    for (const auto &arm : expr.arms) {
                        if (std::holds_alternative<ast::MatchExpr::DefaultPattern>(arm.pattern)) continue;
                        const auto &vp = std::get<ast::MatchExpr::VariantPattern>(arm.pattern);
                        int64_t field_val = 0;
                        for (const auto &field : enum_info.fields) {
                            if (field.name == vp.name) { field_val = field.value; break; }
                        }
                        auto *arm_bb = llvm::BasicBlock::Create(*context_, std::format("match.arm.{}", vp.name), fn);
                        sw->addCase(llvm::cast<llvm::ConstantInt>(llvm::ConstantInt::get(underlying_llvm_ty, static_cast<uint64_t>(field_val), enum_info.underlying_type.is_signed())), arm_bb);
                        builder_.SetInsertPoint(arm_bb);
                        auto *arm_val = emit_value_as(arm.value, result_type, *current_module_path_);
                        auto *arm_done = builder_.GetInsertBlock();
                        builder_.CreateBr(merge_bb);
                        arm_results.emplace_back(arm_done, arm_val);
                    }
                    builder_.SetInsertPoint(default_bb);
                    if (default_arm) {
                        auto *arm_val = emit_value_as(default_arm->value, result_type, *current_module_path_);
                        auto *arm_done = builder_.GetInsertBlock();
                        builder_.CreateBr(merge_bb);
                        arm_results.emplace_back(arm_done, arm_val);
                    } else {
                        builder_.CreateUnreachable();
                    }
                    builder_.SetInsertPoint(merge_bb);
                    if (result_type.kind == sema::TypeKind::Void || arm_results.empty()) {
                        return llvm::UndefValue::get(llvm::Type::getInt32Ty(*context_));
                    }
                    auto *phi = builder_.CreatePHI(llvm_type(*current_module_path_, result_type), static_cast<unsigned>(arm_results.size()));
                    for (auto &[bb, val] : arm_results) phi->addIncoming(val, bb);
                    return phi;
                }

                // ---- Scalar match (integer or bool) ----
                auto *operand = emit_operand();
                auto *operand_ll_ty = llvm_type(*current_module_path_, operand_type);
                auto *sw = builder_.CreateSwitch(operand, default_bb, non_default_count);

                for (const auto &arm : expr.arms) {
                    if (std::holds_alternative<ast::MatchExpr::DefaultPattern>(arm.pattern)) continue;
                    const auto &lp = std::get<ast::MatchExpr::LiteralPattern>(arm.pattern);
                    // Evaluate the constant for the LLVM switch case
                    const auto case_val = sema::evaluate_integer_constant(*lp.expr, *current_module_path_, sema_program_);
                    if (!case_val) {
                        // sema rejects a non-evaluatable pattern before codegen runs, so this is
                        // an internal inconsistency rather than a user error -- but it used to be
                        // an unchecked dereference of an empty optional, which aborts.
                        report_codegen_error(diag_, arm.location,
                            "internal error: match arm pattern could not be evaluated to a constant");
                        continue;
                    }
                    auto *case_ci = llvm::ConstantInt::get(operand_ll_ty, static_cast<uint64_t>(*case_val), operand_type.is_signed());
                    auto *arm_bb = llvm::BasicBlock::Create(*context_, "match.arm", fn);
                    sw->addCase(llvm::cast<llvm::ConstantInt>(case_ci), arm_bb);
                    builder_.SetInsertPoint(arm_bb);
                    auto *arm_val = emit_value_as(arm.value, result_type, *current_module_path_);
                    auto *arm_done = builder_.GetInsertBlock();
                    builder_.CreateBr(merge_bb);
                    arm_results.emplace_back(arm_done, arm_val);
                }
                builder_.SetInsertPoint(default_bb);
                if (default_arm) {
                    auto *arm_val = emit_value_as(default_arm->value, result_type, *current_module_path_);
                    auto *arm_done = builder_.GetInsertBlock();
                    builder_.CreateBr(merge_bb);
                    arm_results.emplace_back(arm_done, arm_val);
                } else {
                    builder_.CreateUnreachable();
                }
                builder_.SetInsertPoint(merge_bb);
                if (result_type.kind == sema::TypeKind::Void || arm_results.empty()) {
                    return llvm::UndefValue::get(llvm::Type::getInt32Ty(*context_));
                }
                auto *phi = builder_.CreatePHI(llvm_type(*current_module_path_, result_type), static_cast<unsigned>(arm_results.size()));
                for (auto &[bb, val] : arm_results) phi->addIncoming(val, bb);
                return phi;
            }

            void emit_switch_stmt(const ast::SwitchStmt &stmt) {
                auto *fn = builder_.GetInsertBlock()->getParent();
                auto *after_bb = llvm::BasicBlock::Create(*context_, "switch.end", fn);

                auto operand_type = current_exprs_->expr_types.at(sema::get_expr_key(stmt.operand));

                // Transparent error-value matching — see the identical comment in emit_match.
                llvm::Value *unwrapped_operand_val = nullptr;
                if (const auto it = current_exprs_->expr_error_match_unwrap.find(sema::get_expr_key(stmt.operand));
                    it != current_exprs_->expr_error_match_unwrap.end()) {
                    auto *wrapper_val = emit_expr(stmt.operand);
                    unwrapped_operand_val = unwrap_failed_error_value(wrapper_val, it->second.wrapper_type, it->second.effective_type);
                    operand_type = it->second.effective_type;
                }
                const auto emit_operand = [&]() -> llvm::Value * {
                    return unwrapped_operand_val ? unwrapped_operand_val : emit_expr(stmt.operand);
                };

                // Find default arm
                const ast::SwitchStmt::Arm *default_arm = nullptr;
                for (const auto &arm : stmt.arms) {
                    if (std::holds_alternative<ast::MatchExpr::DefaultPattern>(arm.pattern)) {
                        default_arm = &arm;
                        break;
                    }
                }
                auto *default_bb = default_arm
                    ? llvm::BasicBlock::Create(*context_, "switch.default", fn)
                    : after_bb; // unmatched falls through to after-switch

                const unsigned non_default_count = static_cast<unsigned>(
                    std::ranges::count_if(stmt.arms, [](const auto &a) {
                        return !std::holds_alternative<ast::MatchExpr::DefaultPattern>(a.pattern);
                    }));

                const auto saved_locals = locals_;

                // ---- Tagged union switch ----
                if (operand_type.kind == sema::TypeKind::Union) {
                    const auto &union_info = sema_program_.unions.at(operand_type.union_index);
                    auto *union_ll_ty = unions_.at(operand_type.union_index);
                    // Hoisted to the entry block, like every other scratch slot in this file.
                    // Allocated at the current insertion point, this sits inside any
                    // enclosing loop body, and LLVM lowers a non-entry alloca as a dynamic
                    // stack adjustment that is only reclaimed when the function returns --
                    // so each iteration grew the frame. A 2M-iteration loop switching on a
                    // tagged union exhausted the stack and died with SIGSEGV.
                    auto *union_slot = create_entry_alloca(fn, union_ll_ty, "switch.union");
                    auto *operand_val = emit_operand();
                    builder_.CreateStore(operand_val, union_slot);
                    auto *tag = builder_.CreateLoad(llvm::Type::getInt32Ty(*context_), union_slot, "switch.tag");
                    auto *sw = builder_.CreateSwitch(tag, default_bb, non_default_count);

                    for (const auto &arm : stmt.arms) {
                        if (std::holds_alternative<ast::MatchExpr::DefaultPattern>(arm.pattern)) continue;
                        const auto &vp = std::get<ast::MatchExpr::VariantPattern>(arm.pattern);
                        int32_t tag_val = 0;
                        const sema::TaggedUnionVariant *matched_variant = nullptr;
                        for (const auto &variant : union_info.variants) {
                            if (variant.name == vp.name) {
                                tag_val = variant.tag_value;
                                matched_variant = &variant;
                                break;
                            }
                        }
                        auto *arm_bb = llvm::BasicBlock::Create(*context_, std::format("switch.arm.{}", vp.name), fn);
                        sw->addCase(llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), static_cast<uint64_t>(tag_val), false), arm_bb);
                        builder_.SetInsertPoint(arm_bb);
                        locals_ = saved_locals;
                        if (matched_variant) emit_variant_capture(fn, union_slot, union_info, *matched_variant, vp);
                        emit_stmt(arm.body);
                        if (!builder_.GetInsertBlock()->getTerminator()) builder_.CreateBr(after_bb);
                    }
                    if (default_arm) {
                        builder_.SetInsertPoint(default_bb);
                        locals_ = saved_locals;
                        emit_stmt(default_arm->body);
                        if (!builder_.GetInsertBlock()->getTerminator()) builder_.CreateBr(after_bb);
                    }
                    locals_ = saved_locals;
                    builder_.SetInsertPoint(after_bb);
                    return;
                }

                // ---- Enum switch ----
                if (operand_type.kind == sema::TypeKind::Enum) {
                    auto *operand = emit_operand();
                    const auto &enum_info = sema_program_.enums.at(operand_type.enum_index);
                    auto *underlying_llvm_ty = llvm_type(*current_module_path_, enum_info.underlying_type);
                    auto *sw = builder_.CreateSwitch(operand, default_bb, non_default_count);

                    for (const auto &arm : stmt.arms) {
                        if (std::holds_alternative<ast::MatchExpr::DefaultPattern>(arm.pattern)) continue;
                        const auto &vp = std::get<ast::MatchExpr::VariantPattern>(arm.pattern);
                        int64_t field_val = 0;
                        for (const auto &field : enum_info.fields) {
                            if (field.name == vp.name) { field_val = field.value; break; }
                        }
                        auto *arm_bb = llvm::BasicBlock::Create(*context_, std::format("switch.arm.{}", vp.name), fn);
                        sw->addCase(llvm::cast<llvm::ConstantInt>(llvm::ConstantInt::get(underlying_llvm_ty, static_cast<uint64_t>(field_val), enum_info.underlying_type.is_signed())), arm_bb);
                        builder_.SetInsertPoint(arm_bb);
                        locals_ = saved_locals;
                        emit_stmt(arm.body);
                        if (!builder_.GetInsertBlock()->getTerminator()) builder_.CreateBr(after_bb);
                    }
                    if (default_arm) {
                        builder_.SetInsertPoint(default_bb);
                        locals_ = saved_locals;
                        emit_stmt(default_arm->body);
                        if (!builder_.GetInsertBlock()->getTerminator()) builder_.CreateBr(after_bb);
                    }
                    locals_ = saved_locals;
                    builder_.SetInsertPoint(after_bb);
                    return;
                }

                // ---- Scalar switch (integer or bool) ----
                auto *operand = emit_operand();
                auto *operand_ll_ty = llvm_type(*current_module_path_, operand_type);
                auto *sw = builder_.CreateSwitch(operand, default_bb, non_default_count);

                for (const auto &arm : stmt.arms) {
                    if (std::holds_alternative<ast::MatchExpr::DefaultPattern>(arm.pattern)) continue;
                    const auto &lp = std::get<ast::MatchExpr::LiteralPattern>(arm.pattern);
                    const auto case_val = sema::evaluate_integer_constant(*lp.expr, *current_module_path_, sema_program_);
                    if (!case_val) {
                        // See the matching guard in emit_match above.
                        report_codegen_error(diag_, arm.location,
                            "internal error: switch arm pattern could not be evaluated to a constant");
                        continue;
                    }
                    auto *case_ci = llvm::ConstantInt::get(operand_ll_ty, static_cast<uint64_t>(*case_val), operand_type.is_signed());
                    auto *arm_bb = llvm::BasicBlock::Create(*context_, "switch.arm", fn);
                    sw->addCase(llvm::cast<llvm::ConstantInt>(case_ci), arm_bb);
                    builder_.SetInsertPoint(arm_bb);
                    locals_ = saved_locals;
                    emit_stmt(arm.body);
                    if (!builder_.GetInsertBlock()->getTerminator()) builder_.CreateBr(after_bb);
                }
                if (default_arm) {
                    builder_.SetInsertPoint(default_bb);
                    locals_ = saved_locals;
                    emit_stmt(default_arm->body);
                    if (!builder_.GetInsertBlock()->getTerminator()) builder_.CreateBr(after_bb);
                }
                locals_ = saved_locals;
                builder_.SetInsertPoint(after_bb);
            }

            // Materializes an '$option(...)'/'$env(...)' expression's compile-time-resolved
            // value (cached by sema in ProgramModule::expr_option_values — see
            // sema::resolve_option_expr/resolve_env_expr) as an LLVM constant of its
            // resolved type 'ty'.
            auto emit_option_value(const ast::Expr &expr, const sema::ResolvedType &ty) -> llvm::Value * {
                const auto it = current_module_->expr_option_values.find(sema::get_expr_key(expr));
                if (it == current_module_->expr_option_values.end()) {
                    report_codegen_error(diag_, {}, "internal error: '$option'/'$env' value was not resolved by sema");
                    return llvm::UndefValue::get(llvm_type(*current_module_path_, ty));
                }
                return std::visit(
                    [&]<typename VT>(const VT &val) -> llvm::Value * {
                        using VTT = std::decay_t<VT>;
                        if constexpr (std::is_same_v<VTT, int64_t>) {
                            if (ty.is_float()) {
                                return llvm::ConstantFP::get(llvm_type(*current_module_path_, ty), static_cast<double>(val));
                            }
                            return llvm::ConstantInt::get(llvm_type(*current_module_path_, ty), static_cast<uint64_t>(val), ty.is_signed());
                        } else { // std::string
                            return emit_string_literal(val);
                        }
                    },
                    it->second);
            }

            // Template so the identical then/else/PHI shape below serves both TernaryExpr
            // and WhenExpr's runtime-condition path (their condition/then_expr/else_expr
            // fields are structurally identical) without duplicating the emission logic.
            template <typename TernaryLikeExpr>
            auto emit_ternary(const TernaryLikeExpr &expr, const sema::ResolvedType &result_type) -> llvm::Value * {
                auto *fn = builder_.GetInsertBlock()->getParent();
                auto *then_bb = llvm::BasicBlock::Create(*context_, "ternary.then", fn);
                auto *else_bb = llvm::BasicBlock::Create(*context_, "ternary.else", fn);
                auto *merge_bb = llvm::BasicBlock::Create(*context_, "ternary.end", fn);

                auto *cond = coerce_to_bool(emit_expr(expr.condition), current_exprs_->expr_types.at(sema::get_expr_key(expr.condition)));
                builder_.CreateCondBr(cond, then_bb, else_bb);

                builder_.SetInsertPoint(then_bb);
                auto *then_value = emit_value_as(expr.then_expr, result_type, *current_module_path_);
                auto *then_done = builder_.GetInsertBlock();
                builder_.CreateBr(merge_bb);

                builder_.SetInsertPoint(else_bb);
                auto *else_value = emit_value_as(expr.else_expr, result_type, *current_module_path_);
                auto *else_done = builder_.GetInsertBlock();
                builder_.CreateBr(merge_bb);

                builder_.SetInsertPoint(merge_bb);
                auto *phi = builder_.CreatePHI(llvm_type(*current_module_path_, result_type), 2);
                phi->addIncoming(then_value, then_done);
                phi->addIncoming(else_value, else_done);
                return phi;
            }

            // 'then_val when cond else else_val'. When sema recorded a compile-time-constant
            // condition (ProgramModule::expr_when_selected), no branch/PHI is emitted at
            // all — just the selected branch's value, coerced to the unified result type
            // (mirrors emit_const_or_runtime's existing TernaryExpr const-fold arm, but
            // generalized to a runtime-value context: only the CHOICE of branch is
            // compile-time here, not necessarily the branch's own value). Otherwise this
            // behaves exactly like an ordinary runtime ternary (shared emit_ternary).
            auto emit_when_expr(const ast::WhenExpr &expr, const ast::Expr &self_expr, const sema::ResolvedType &result_type) -> llvm::Value * {
                if (const auto it = current_exprs_->expr_when_selected.find(sema::get_expr_key(self_expr));
                    it != current_exprs_->expr_when_selected.end()) {
                    return emit_value_as(it->second ? expr.then_expr : expr.else_expr, result_type, *current_module_path_);
                }
                return emit_ternary(expr, result_type);
            }

            static auto compound_op(ast::AssignOp op) -> ast::BinaryOp {
                switch (op) {
                case ast::AssignOp::AddAssign: return ast::BinaryOp::Add;
                case ast::AssignOp::SubAssign: return ast::BinaryOp::Sub;
                case ast::AssignOp::MulAssign: return ast::BinaryOp::Mul;
                case ast::AssignOp::DivAssign: return ast::BinaryOp::Div;
                case ast::AssignOp::AndAssign: return ast::BinaryOp::BitwiseAnd;
                case ast::AssignOp::OrAssign:  return ast::BinaryOp::BitwiseOr;
                case ast::AssignOp::XorAssign: return ast::BinaryOp::BitwiseXor;
                case ast::AssignOp::ShlAssign: return ast::BinaryOp::ShiftLeft;
                case ast::AssignOp::ShrAssign: return ast::BinaryOp::ShiftRight;
                case ast::AssignOp::ToggleAssign: return ast::BinaryOp::BitwiseXor;
                case ast::AssignOp::Assign:    return ast::BinaryOp::Add;
                }
                return ast::BinaryOp::Add;
            }

            auto emit_assign(const ast::AssignExpr &expr) -> llvm::Value * {
                auto lv = emit_lvalue(expr.target);
                auto *value = emit_value_as(expr.value, lv.type, lv.type_module);

                if (expr.op != ast::AssignOp::Assign) {
                    auto *old = builder_.CreateLoad(lv.storage_type, lv.ptr);
                    const auto op = compound_op(expr.op);
                    if (lv.type.kind == sema::TypeKind::Bitset) {
                        // '+=' (set) -> OR, '-=' (clear) -> AND-NOT; '~=' (toggle) already maps
                        // to BitwiseXor via compound_op, which emit_int_arith lowers correctly.
                        if (op == ast::BinaryOp::Add) {
                            value = builder_.CreateOr(old, value);
                        } else if (op == ast::BinaryOp::Sub) {
                            value = builder_.CreateAnd(old, builder_.CreateNot(value));
                        } else {
                            value = emit_int_arith(op, old, value, lv.type);
                        }
                    } else if (is_pointer_like(lv.type) && (op == ast::BinaryOp::Add || op == ast::BinaryOp::Sub)) {
                        value = emit_pointer_offset(old, value, lv.type, op == ast::BinaryOp::Sub);
                    } else if (lv.type.is_float()) {
                        switch (op) {
                        case ast::BinaryOp::Add: value = builder_.CreateFAdd(old, value); break;
                        case ast::BinaryOp::Sub: value = builder_.CreateFSub(old, value); break;
                        case ast::BinaryOp::Mul: value = builder_.CreateFMul(old, value); break;
                        case ast::BinaryOp::Div: value = builder_.CreateFDiv(old, value); break;
                        default:                 break;
                        }
                    } else {
                        value = emit_int_arith(op, old, value, lv.type);
                    }
                }

                builder_.CreateStore(value, lv.ptr);
                return value;
            }

            auto emit_incr_decr(const ast::IncrDecrExpr &expr) -> llvm::Value * {
                auto lv = emit_lvalue(expr.operand);
                auto *old = builder_.CreateLoad(lv.storage_type, lv.ptr);
                llvm::Value *next = nullptr;
                if (is_pointer_like(lv.type)) {
                    next = emit_pointer_offset(old, builder_.getInt64(1), lv.type, !expr.is_increment);
                } else {
                    auto *one = llvm::ConstantInt::get(lv.storage_type, 1);
                    next = expr.is_increment ? builder_.CreateAdd(old, one) : builder_.CreateSub(old, one);
                }
                builder_.CreateStore(next, lv.ptr);
                return expr.is_prefix ? next : old;
            }

            auto sizeof_operand(const ast::SizeOfExpr &expr) const -> uint64_t {
                if (const auto *ident = std::get_if<ast::IdentExpr>(&expr.operand)) {
                    if (const auto it = current_module_->symbols.find(ident->name); it != current_module_->symbols.end()) {
                        if (const auto *ts = std::get_if<sema::TypeSymbol>(&it->second); ts && ts->resolved) {
                            return size_of(*current_module_path_, *ts->resolved);
                        }
                    }
                    // A bare reference to the CURRENTLY-EMITTING generic instance's own type
                    // param (e.g. 'T' inside 'size_of(T)') - never a module symbol by the time
                    // codegen runs (shadow_generic_type_params' temporary module.symbols entry
                    // is long restored/removed by then - see check_generic_function_instance_
                    // body), and never given its own expr_types entry either (sema's own
                    // SizeOfExpr case resolves 'T' via that same temporary shadow and returns
                    // early, without ever recursing check_expr onto the operand) - resolve it
                    // the same way sema did, straight off the active substitution env
                    // (Program::active_generic_env_stack, pushed by emit_generic_function_
                    // instance/emit_generic_method_instance for the exact duration of this
                    // instance's own body emission).
                    if (!sema_program_.active_generic_env_stack.empty()) {
                        for (const auto &binding : *sema_program_.active_generic_env_stack.back()) {
                            if (binding.is_type && binding.param_name == ident->name) {
                                return size_of(*current_module_path_, binding.type_value);
                            }
                        }
                    }
                }
                if (const auto *member = std::get_if<std::unique_ptr<ast::MemberExpr>>(&expr.operand)) {
                    if (const auto ns = try_namespace_chain((*member)->object)) {
                        const auto &mod = module_for(*ns);
                        if (const auto it = mod.symbols.find((*member)->member); it != mod.symbols.end()) {
                            if (const auto *ts = std::get_if<sema::TypeSymbol>(&it->second); ts && ts->resolved) {
                                return size_of(*ns, *ts->resolved);
                            }
                        }
                    }
                }
                return size_of(*current_module_path_, current_exprs_->expr_types.at(sema::get_expr_key(expr.operand)));
            }

            // Mirrors sizeof_operand() above exactly, substituting align_of() for size_of().
            auto align_of_operand(const ast::AlignOfExpr &expr) const -> uint64_t {
                if (const auto *ident = std::get_if<ast::IdentExpr>(&expr.operand)) {
                    if (const auto it = current_module_->symbols.find(ident->name); it != current_module_->symbols.end()) {
                        if (const auto *ts = std::get_if<sema::TypeSymbol>(&it->second); ts && ts->resolved) {
                            return align_of(*current_module_path_, *ts->resolved);
                        }
                    }
                    // See sizeof_operand's identical case above for why this is needed.
                    if (!sema_program_.active_generic_env_stack.empty()) {
                        for (const auto &binding : *sema_program_.active_generic_env_stack.back()) {
                            if (binding.is_type && binding.param_name == ident->name) {
                                return align_of(*current_module_path_, binding.type_value);
                            }
                        }
                    }
                }
                if (const auto *member = std::get_if<std::unique_ptr<ast::MemberExpr>>(&expr.operand)) {
                    if (const auto ns = try_namespace_chain((*member)->object)) {
                        const auto &mod = module_for(*ns);
                        if (const auto it = mod.symbols.find((*member)->member); it != mod.symbols.end()) {
                            if (const auto *ts = std::get_if<sema::TypeSymbol>(&it->second); ts && ts->resolved) {
                                return align_of(*ns, *ts->resolved);
                            }
                        }
                    }
                }
                return align_of(*current_module_path_, current_exprs_->expr_types.at(sema::get_expr_key(expr.operand)));
            }

            // Mirrors sizeof_operand() above, but never invents a new id - every id a program
            // can ever reference was already assigned synchronously during sema's whole-program
            // check_expr pass (see intern_type_id), so this only ever reads sema_program_.type_ids.
            auto type_of_operand(const ast::TypeOfExpr &expr) const -> uint64_t {
                if (const auto *ident = std::get_if<ast::IdentExpr>(&expr.operand)) {
                    if (const auto it = current_module_->symbols.find(ident->name); it != current_module_->symbols.end()) {
                        if (const auto *ts = std::get_if<sema::TypeSymbol>(&it->second); ts && ts->resolved) {
                            return sema_program_.type_ids.at(*ts->resolved);
                        }
                    }
                }
                if (const auto *member = std::get_if<std::unique_ptr<ast::MemberExpr>>(&expr.operand)) {
                    if (const auto ns = try_namespace_chain((*member)->object)) {
                        const auto &mod = module_for(*ns);
                        if (const auto it = mod.symbols.find((*member)->member); it != mod.symbols.end()) {
                            if (const auto *ts = std::get_if<sema::TypeSymbol>(&it->second); ts && ts->resolved) {
                                return sema_program_.type_ids.at(*ts->resolved);
                            }
                        }
                    }
                }
                return sema_program_.type_ids.at(current_exprs_->expr_types.at(sema::get_expr_key(expr.operand)));
            }

            // Sema has already validated the path (containment, existence) by the time codegen
            // runs, so this just re-reads the bytes — no error reporting needed here, same trust
            // relationship sizeof_operand has with its sema-side counterpart.
            auto emit_import_bin(const ast::ImportBinExpr &expr) const -> llvm::Constant * {
                const auto resolved = ast::resolve_contained_path(*current_module_path_, expr.path);
                std::ifstream file(resolved, std::ios::binary);
                const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

                if (bytes.empty()) {
                    // ConstantDataArray can't represent a zero-length array.
                    return llvm::ConstantAggregateZero::get(llvm::ArrayType::get(llvm::Type::getInt8Ty(*context_), 0));
                }

                return llvm::ConstantDataArray::get(*context_, bytes);
            }

            auto emit_constant_expr(const ast::Expr &expr) -> llvm::Constant * {
                return llvm::dyn_cast<llvm::Constant>(emit_const_or_runtime(expr, true));
            }

            auto emit_const_or_runtime(const ast::Expr &expr, bool constant) -> llvm::Value * {
                if (!constant) {
                    return emit_expr(expr);
                }
                return std::visit(
                    [&]<typename T>(const T &v) -> llvm::Value * {
                        using V = std::decay_t<T>;
                        const auto ty = expr_type(expr);
                        if constexpr (std::is_same_v<V, ast::LiteralIntegerExpr>) {
                            if (ty.is_float()) {
                                return llvm::ConstantFP::get(llvm_type(*current_module_path_, ty), static_cast<double>(v.value));
                            }
                            return llvm::ConstantInt::get(llvm_type(*current_module_path_, ty), v.value, ty.is_signed());
                        } else if constexpr (std::is_same_v<V, ast::LiteralFloatExpr>) {
                            return llvm::ConstantFP::get(llvm_type(*current_module_path_, ty), v.value);
                        } else if constexpr (std::is_same_v<V, ast::LiteralStringExpr>) {
                            return emit_string_literal(v.value);
                        } else if constexpr (std::is_same_v<V, ast::LiteralCharExpr>) {
                            return builder_.getInt8(v.value);
                        } else if constexpr (std::is_same_v<V, ast::LiteralBoolExpr>) {
                            return builder_.getInt1(v.value);
                        } else if constexpr (std::is_same_v<V, ast::LiteralNilExpr>) {
                            if (ty.kind == sema::TypeKind::Slice || ty.kind == sema::TypeKind::Trait)
                                return llvm::ConstantAggregateZero::get(llvm_type(*current_module_path_, ty));
                            return llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context_));
                        } else if constexpr (std::is_same_v<V, ast::IdentExpr>) {
                            if (const auto macro = macro_args_.find(v.name); macro != macro_args_.end()) {
                                return emit_const_macro_arg(macro->second);
                            }
                            // A bare reference to the active generic instance's own VALUE
                            // parameter, e.g. the default in 'fn f[N: usize](times := N)'. sema's
                            // is_constant_expr counts such a name as constant (it consults the
                            // same env — see its doc comment), so this path must be able to fold
                            // it too, or the two disagree and an otherwise valid program dies
                            // with "unsupported global constant initializer" below. Mirrors
                            // sizeof_operand's identical generic-param fallback.
                            if (!sema_program_.active_generic_env_stack.empty()) {
                                for (const auto &binding : *sema_program_.active_generic_env_stack.back()) {
                                    if (binding.is_type || binding.param_name != v.name) continue;
                                    if (const auto *n = std::get_if<int64_t>(&binding.const_value)) {
                                        return llvm::ConstantInt::get(llvm_type(*current_module_path_, ty),
                                                                       static_cast<uint64_t>(*n), ty.is_signed());
                                    }
                                }
                            }
                            const auto sym = current_module_->symbols.find(v.name);
                            if (sym != current_module_->symbols.end()) {
                                if (const auto *g = std::get_if<sema::GlobalSymbol>(&sym->second); g && !g->is_mut && g->decl->init) {
                                    return emit_const_or_runtime(*g->decl->init, true);
                                }
                                // Function pointer constant initializer
                                if (ty.kind == sema::TypeKind::Function) {
                                    if (std::holds_alternative<sema::FunctionSymbol>(sym->second)) {
                                        return functions_.at(FunctionKey{*current_module_path_, v.name});
                                    }
                                    if (std::holds_alternative<sema::ExtFunctionSymbol>(sym->second)) {
                                        return ext_functions_.at(global_key(*current_module_path_, v.name));
                                    }
                                }
                            }
                        } else if constexpr (std::is_same_v<V, ast::DotIdentExpr>) {
                            if (ty.kind == sema::TypeKind::Enum) {
                                const auto &enum_info = sema_program_.enums.at(ty.enum_index);
                                for (const auto &field : enum_info.fields) {
                                    if (field.name == v.name) {
                                        return llvm::ConstantInt::get(llvm_type(*current_module_path_, ty),
                                                                      static_cast<uint64_t>(field.value),
                                                                      enum_info.underlying_type.is_signed());
                                    }
                                }
                                report_codegen_error(diag_, v.location, std::format("unknown enum field '{}'", v.name));
                                return llvm::UndefValue::get(llvm_type(*current_module_path_, ty));
                            }
                            if (ty.kind == sema::TypeKind::Bitset) {
                                const auto &bitset_info = sema_program_.bitsets.at(ty.bitset_index);
                                const auto mask = bitset_member_mask(bitset_info, v.name, v.location);
                                return llvm::ConstantInt::get(llvm_type(*current_module_path_, ty), mask.value_or(0), false);
                            }
                            if (ty.kind == sema::TypeKind::Union) {
                                // A payload-free tagged-union variant IS constant-foldable: it is
                                // just the tag word, with the payload bytes zero. sema's
                                // is_constant_expr already accepts a bare '.name' (DotIdentExpr),
                                // so rejecting it here made the two disagree -- 'const A: Opt =
                                // .None' passed sema and then failed with "unsupported global
                                // constant initializer".
                                //
                                // A payload-BEARING variant ('.Some(7)') is a different matter and
                                // is deliberately not handled: sema classifies TaggedVariantExpr
                                // as non-constant ("has runtime stores"), so it is rejected before
                                // reaching codegen and never lands in the fall-through below.
                                const auto &union_info = sema_program_.unions.at(ty.union_index);
                                for (const auto &variant : union_info.variants) {
                                    if (variant.name != v.name) continue;
                                    if (variant.payload_struct_index >= 0) break; // has a payload

                                    // Unions lower to [size x i8] (declare_unions), with the u32
                                    // tag at offset 0 — little-endian, matching the target.
                                    auto *i8_ty = llvm::Type::getInt8Ty(*context_);
                                    std::vector<llvm::Constant *> bytes(union_info.size,
                                                                        llvm::ConstantInt::get(i8_ty, 0));
                                    const auto tag = static_cast<uint32_t>(variant.tag_value);
                                    for (uint32_t b = 0; b < 4 && b < union_info.size; ++b) {
                                        bytes[b] = llvm::ConstantInt::get(i8_ty, (tag >> (8 * b)) & 0xFF);
                                    }
                                    return llvm::ConstantArray::get(
                                        llvm::cast<llvm::ArrayType>(llvm_type(*current_module_path_, ty)), bytes);
                                }
                            }
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::SizeOfExpr>>) {
                            return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), sizeof_operand(*v));
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::AlignOfExpr>>) {
                            return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), align_of_operand(*v));
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::TypeOfExpr>>) {
                            // constant=true here means sema already confirmed this is eligible as
                            // a compile-time constant (is_constant_expr), which for TypeOfExpr
                            // means the operand is never 'any' — see is_constant_expr_impl.
                            return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), type_of_operand(*v));
                        } else if constexpr (std::is_same_v<V, ast::ImportBinExpr>) {
                            return emit_import_bin(v);
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::CastExpr>>) {
                            auto *value = llvm::cast<llvm::Constant>(emit_const_or_runtime(v->value, true));
                            const auto from = current_exprs_->expr_types.at(sema::get_expr_key(v->value));
                            return llvm::ConstantFoldCastInstruction(cast_opcode(from, ty), value, llvm_type(*current_module_path_, ty));
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::UnaryExpr>>) {
                            auto *operand = llvm::cast<llvm::Constant>(emit_const_or_runtime(v->operand, true));
                            switch (v->op) {
                            case ast::UnaryOp::Negate:
                                return ty.is_float()
                                           ? llvm::ConstantFoldUnaryInstruction(llvm::Instruction::FNeg, operand)
                                           : llvm::ConstantExpr::getNeg(operand);
                            case ast::UnaryOp::BitwiseNot:
                            case ast::UnaryOp::LogicalNot:
                                return llvm::ConstantExpr::getNot(operand);
                            default:
                                break;
                            }
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::BinaryExpr>>) {
                            auto *lhs = llvm::cast<llvm::Constant>(emit_const_or_runtime(v->lhs, true));
                            auto *rhs = llvm::cast<llvm::Constant>(emit_const_or_runtime(v->rhs, true));
                            return const_binary(*v, lhs, rhs);
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::TernaryExpr>>) {
                            auto *cond = llvm::cast<llvm::ConstantInt>(emit_const_or_runtime(v->condition, true));
                            return emit_const_or_runtime(cond->isZero() ? v->else_expr : v->then_expr, true);
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::OptionExpr>> ||
                                              std::is_same_v<V, std::unique_ptr<ast::EnvExpr>>) {
                            return llvm::cast<llvm::Constant>(emit_option_value(expr, ty));
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::WhenExpr>>) {
                            // Only reachable for a compile-time-constant condition — sema's
                            // is_constant_expr_impl requires the condition (along with both
                            // branches) to itself be constant for a WhenExpr to ever be
                            // considered a constant expression in the first place, so
                            // expr_when_selected is always populated here.
                            if (const auto it = current_exprs_->expr_when_selected.find(sema::get_expr_key(expr));
                                it != current_exprs_->expr_when_selected.end()) {
                                return emit_const_or_runtime(it->second ? v->then_expr : v->else_expr, true);
                            }
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::CallExpr>>) {
                            std::string target_module = *current_module_path_;
                            std::string name;
                            if (const auto *ident = std::get_if<ast::IdentExpr>(&v->callee)) {
                                name = ident->name;
                            } else if (const auto *member = std::get_if<std::unique_ptr<ast::MemberExpr>>(&v->callee)) {
                                if (auto ns = try_namespace_chain((*member)->object)) {
                                    target_module = *ns;
                                    name = (*member)->member;
                                }
                            }

                            if (!name.empty()) {
                                const auto &target = module_for(target_module);
                                const auto sym_it = target.symbols.find(name);
                                if (sym_it != target.symbols.end()) {
                                    if (const auto *macro = std::get_if<sema::MacroSymbol>(&sym_it->second)) {
                                        auto saved_args = macro_args_;
                                        auto outer = std::make_shared<const std::unordered_map<std::string, MacroArg>>(saved_args);
                                        for (size_t i = 0; i < macro->decl->params.size(); ++i) {
                                            macro_args_[macro->decl->params[i].name] = MacroArg{.expr = &v->args[i], .module_path = current_module_path_, .module = current_module_, .exprs = current_exprs_, .param_type = &macro->params[i], .param_module = &target_module, .outer_args = outer};
                                        }
                                        // See the identical fix in emit_call's macro-expansion
                                        // branch above for why a bare-import alias needs the
                                        // TRUE ORIGIN module here, not 'target_module'.
                                        const auto origin = target.bare_import_origins.find(name);
                                        const bool via_origin = origin != target.bare_import_origins.end();
                                        const ScopedEmitModule module_scope(
                                            *this, via_origin ? origin->second.module_path : target_module,
                                            via_origin ? sema_program_.modules.at(origin->second.module_path) : target);
                                        auto *value = emit_const_or_runtime(macro->decl->expr_template, true);
                                        macro_args_ = std::move(saved_args);
                                        return value;
                                    }
                                }
                            }
                        } else if constexpr (std::is_same_v<V, ast::DefaultExpr>) {
                            return emit_const_default_value(*current_module_path_, ty);
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::BracedInitializerExpr>>) {
                            return std::visit(
                                [&]<typename BV>(const BV &bv) -> llvm::Value * {
                                    using BVT = std::decay_t<BV>;
                                    if constexpr (std::is_same_v<BVT, ast::EmptyExpr>) {
                                        return emit_const_default_value(*current_module_path_, ty);
                                    } else if constexpr (std::is_same_v<BVT, ast::StructExpr>) {
                                        if (ty.kind != sema::TypeKind::Struct) {
                                            // e.g. untagged-union member init: needs alloca+store,
                                            // not reachable for a program that passed sema's
                                            // is_constant_expr check, but guard against crashing
                                            // codegen on an already-erroring program.
                                            report_codegen_error(diag_, {}, "unsupported global constant initializer");
                                            return llvm::UndefValue::get(llvm_type(*current_module_path_, ty));
                                        }

                                        const auto &info = sema_program_.structs.at(ty.struct_index);
                                        const auto &struct_module = info.module_path;
                                        auto *ll_ty = llvm_type(struct_module, ty);
                                        llvm::Value *agg = llvm::Constant::getNullValue(ll_ty);
                                        const auto &lowering = struct_lowering(ty.struct_index);

                                        std::unordered_map<std::string, const ast::StructExpr::Field *> provided;
                                        for (const auto &sf : bv.fields) {
                                            provided[sf.name] = &sf;
                                        }

                                        for (const auto &sf : bv.fields) {
                                            if (std::holds_alternative<ast::UndefinedExpr>(sf.expr)) continue;
                                            const auto it = std::ranges::find(info.fields, sf.name, &sema::StructField::name);
                                            if (it == info.fields.end()) continue;
                                            const auto field_pos = static_cast<size_t>(std::distance(info.fields.begin(), it));
                                            const auto ll_idx = lowering.field_indices.at(field_pos);
                                            agg = builder_.CreateInsertValue(agg, llvm::cast<llvm::Constant>(emit_const_or_runtime(sf.expr, true)), {ll_idx});
                                        }

                                        const ScopedEmitModule module_scope(*this, struct_module);
                                        use_struct_instance_exprs(ty.struct_index);

                                        for (size_t i = 0; i < info.fields.size(); ++i) {
                                            const auto &field = info.fields[i];
                                            if (provided.contains(field.name) || !field.init_expr) continue;
                                            const auto ll_idx = lowering.field_indices.at(i);
                                            agg = builder_.CreateInsertValue(agg, llvm::cast<llvm::Constant>(emit_const_or_runtime(*field.init_expr, true)), {ll_idx});
                                        }

                                        return llvm::cast<llvm::Constant>(agg);
                                    } else if constexpr (std::is_same_v<BVT, ast::BitsetExpr>) {
                                        return llvm::cast<llvm::Constant>(emit_bitset_expr_value(bv, ty));
                                    } else if (ty.kind == sema::TypeKind::Struct) { // positional struct init
                                        const auto &info = sema_program_.structs.at(ty.struct_index);
                                        const auto &struct_module = info.module_path;
                                        auto *ll_ty = llvm_type(struct_module, ty);
                                        llvm::Value *agg = llvm::Constant::getNullValue(ll_ty);
                                        const auto &lowering = struct_lowering(ty.struct_index);

                                        for (size_t i = 0; i < bv.values.size(); ++i) {
                                            if (std::holds_alternative<ast::UndefinedExpr>(bv.values[i])) continue;
                                            const auto ll_idx = lowering.field_indices.at(i);
                                            agg = builder_.CreateInsertValue(agg, llvm::cast<llvm::Constant>(emit_const_or_runtime(bv.values[i], true)), {ll_idx});
                                        }

                                        const ScopedEmitModule module_scope(*this, struct_module);
                                        use_struct_instance_exprs(ty.struct_index);

                                        for (size_t i = bv.values.size(); i < info.fields.size(); ++i) {
                                            const auto &field = info.fields[i];
                                            if (!field.init_expr) continue;
                                            const auto ll_idx = lowering.field_indices.at(i);
                                            agg = builder_.CreateInsertValue(agg, llvm::cast<llvm::Constant>(emit_const_or_runtime(*field.init_expr, true)), {ll_idx});
                                        }

                                        return llvm::cast<llvm::Constant>(agg);
                                    } else { // ast::ArrayExpr
                                        const auto &array_info = sema_program_.arrays.at(ty.array_index);
                                        auto *ll_ty = llvm_type(*current_module_path_, ty);
                                        llvm::Value *agg = llvm::Constant::getNullValue(ll_ty);
                                        llvm::Constant *fill_elem = nullptr;
                                        for (size_t i = 0; i < bv.values.size(); ++i) {
                                            if (std::holds_alternative<ast::UndefinedExpr>(bv.values[i])) continue;
                                            auto *elem = llvm::cast<llvm::Constant>(emit_const_or_runtime(bv.values[i], true));
                                            agg = builder_.CreateInsertValue(agg, elem, {static_cast<unsigned>(i)});
                                            if (bv.has_fill && i == bv.values.size() - 1) {
                                                fill_elem = elem;
                                            }
                                        }
                                        for (uint64_t i = bv.values.size(); i < array_info.count; ++i) {
                                            auto *elem = fill_elem != nullptr ? fill_elem : emit_const_default_value(*current_module_path_, array_info.element_type);
                                            agg = builder_.CreateInsertValue(agg, elem, {static_cast<unsigned>(i)});
                                        }
                                        return llvm::cast<llvm::Constant>(agg);
                                    }
                                },
                                *v);
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::MemberExpr>>) {
                            // Cross-module qualified const access, e.g. 'opts.target_os' or
                            // 'import("...").target_os' — mirrors sema's evaluate_const_value
                            // MemberExpr case (value_resolver.cpp), which is what sema used to
                            // decide this global's initializer counts as constant in the first
                            // place (is_constant_expr_impl gates global const initializers).
                            if (const auto target_module = try_namespace_chain(v->object)) {
                                const auto &target = module_for(*target_module);
                                const auto sym_it = target.symbols.find(v->member);
                                if (sym_it != target.symbols.end()) {
                                    if (const auto *g = std::get_if<sema::GlobalSymbol>(&sym_it->second); g && !g->is_mut && g->decl->init) {
                                        // 'target_path' is declared before the guard so it
                                        // outlives it — the guard stores its address.
                                        const std::string target_path = *target_module;
                                        const ScopedEmitModule module_scope(*this, target_path, target);
                                        return emit_const_or_runtime(*g->decl->init, true);
                                    }
                                }
                            }
                        }

                        report_codegen_error(diag_, {}, "unsupported global constant initializer");
                        return llvm::UndefValue::get(llvm_type(*current_module_path_, ty));
                    },
                    expr);
            }

            static auto cast_opcode(const sema::ResolvedType &from, const sema::ResolvedType &to) -> unsigned {
                if (from.is_integer() && to.is_integer()) {
                    const auto from_bits = int_bits(from);
                    const auto to_bits = int_bits(to);
                    if (from_bits == to_bits) return llvm::Instruction::BitCast;
                    if (from_bits > to_bits) return llvm::Instruction::Trunc;
                    return signedness(from) ? llvm::Instruction::SExt : llvm::Instruction::ZExt;
                }
                if (from.is_integer() && to.is_float()) return signedness(from) ? llvm::Instruction::SIToFP : llvm::Instruction::UIToFP;
                if (from.is_float() && to.is_integer()) return signedness(to) ? llvm::Instruction::FPToSI : llvm::Instruction::FPToUI;
                if (from.is_float() && to.is_float()) return int_bits(from) > int_bits(to) ? llvm::Instruction::FPTrunc : llvm::Instruction::FPExt;
                if (is_pointer_like(from) && to.is_integer()) return llvm::Instruction::PtrToInt;
                if (from.is_integer() && is_pointer_like(to)) return llvm::Instruction::IntToPtr;
                return llvm::Instruction::BitCast;
            }

            auto const_binary(const ast::BinaryExpr &expr, llvm::Constant *lhs, llvm::Constant *rhs) const -> llvm::Constant * {
                const auto lhs_type = current_exprs_->expr_types.at(sema::get_expr_key(expr.lhs));
                if (expr.op == ast::BinaryOp::In) {
                    // See the runtime emit_binary_expr 'In' case: single-member and subset
                    // membership both reduce to the same '(rhs & lhs) == lhs' formula.
                    auto *anded = llvm::ConstantFoldBinaryInstruction(llvm::Instruction::And, rhs, lhs);
                    return llvm::ConstantFoldCompareInstruction(llvm::CmpInst::ICMP_EQ, anded, lhs);
                }
                if (lhs_type.kind == sema::TypeKind::Bitset) {
                    if (expr.op == ast::BinaryOp::Add || expr.op == ast::BinaryOp::BitwiseOr) {
                        return llvm::ConstantFoldBinaryInstruction(llvm::Instruction::Or, lhs, rhs);
                    }
                    if (expr.op == ast::BinaryOp::Sub) {
                        return llvm::ConstantFoldBinaryInstruction(llvm::Instruction::And, lhs, llvm::ConstantExpr::getNot(rhs));
                    }
                    // BitwiseAnd/BitwiseXor and Equal/NotEqual fall through to the generic
                    // paths below/above, which already lower them correctly.
                }
                if (expr.op == ast::BinaryOp::Equal || expr.op == ast::BinaryOp::NotEqual ||
                    expr.op == ast::BinaryOp::Less || expr.op == ast::BinaryOp::Greater ||
                    expr.op == ast::BinaryOp::LessEqual || expr.op == ast::BinaryOp::GreaterEqual) {
                    llvm::CmpInst::Predicate pred;
                    if (lhs_type.is_float()) {
                        switch (expr.op) {
                        case ast::BinaryOp::Equal:        pred = llvm::CmpInst::FCMP_OEQ; break;
                        case ast::BinaryOp::NotEqual:     pred = llvm::CmpInst::FCMP_ONE; break;
                        case ast::BinaryOp::Less:         pred = llvm::CmpInst::FCMP_OLT; break;
                        case ast::BinaryOp::Greater:      pred = llvm::CmpInst::FCMP_OGT; break;
                        case ast::BinaryOp::LessEqual:    pred = llvm::CmpInst::FCMP_OLE; break;
                        case ast::BinaryOp::GreaterEqual: pred = llvm::CmpInst::FCMP_OGE; break;
                        default:                          pred = llvm::CmpInst::FCMP_FALSE; break;
                        }
                    } else {
                        switch (expr.op) {
                        case ast::BinaryOp::Equal:        pred = llvm::CmpInst::ICMP_EQ; break;
                        case ast::BinaryOp::NotEqual:     pred = llvm::CmpInst::ICMP_NE; break;
                        case ast::BinaryOp::Less:         pred = signedness(lhs_type) ? llvm::CmpInst::ICMP_SLT : llvm::CmpInst::ICMP_ULT; break;
                        case ast::BinaryOp::Greater:      pred = signedness(lhs_type) ? llvm::CmpInst::ICMP_SGT : llvm::CmpInst::ICMP_UGT; break;
                        case ast::BinaryOp::LessEqual:    pred = signedness(lhs_type) ? llvm::CmpInst::ICMP_SLE : llvm::CmpInst::ICMP_ULE; break;
                        case ast::BinaryOp::GreaterEqual: pred = signedness(lhs_type) ? llvm::CmpInst::ICMP_SGE : llvm::CmpInst::ICMP_UGE; break;
                        default:                          pred = llvm::CmpInst::BAD_ICMP_PREDICATE; break;
                        }
                    }
                    return llvm::ConstantFoldCompareInstruction(pred, lhs, rhs);
                }
                unsigned opcode = llvm::Instruction::Add;
                switch (expr.op) {
                case ast::BinaryOp::Add:        opcode = lhs_type.is_float() ? llvm::Instruction::FAdd : llvm::Instruction::Add; break;
                case ast::BinaryOp::Sub:        opcode = lhs_type.is_float() ? llvm::Instruction::FSub : llvm::Instruction::Sub; break;
                case ast::BinaryOp::Mul:        opcode = lhs_type.is_float() ? llvm::Instruction::FMul : llvm::Instruction::Mul; break;
                case ast::BinaryOp::Div:        opcode = lhs_type.is_float() ? llvm::Instruction::FDiv : (signedness(lhs_type) ? llvm::Instruction::SDiv : llvm::Instruction::UDiv); break;
                case ast::BinaryOp::Mod:        opcode = lhs_type.is_float() ? llvm::Instruction::FRem : (signedness(lhs_type) ? llvm::Instruction::SRem : llvm::Instruction::URem); break;
                case ast::BinaryOp::BitwiseAnd:
                case ast::BinaryOp::LogicalAnd: opcode = llvm::Instruction::And; break;
                case ast::BinaryOp::BitwiseOr:
                case ast::BinaryOp::LogicalOr:  opcode = llvm::Instruction::Or; break;
                case ast::BinaryOp::BitwiseXor: opcode = llvm::Instruction::Xor; break;
                case ast::BinaryOp::ShiftLeft:  opcode = llvm::Instruction::Shl; break;
                case ast::BinaryOp::ShiftRight: opcode = signedness(lhs_type) ? llvm::Instruction::AShr : llvm::Instruction::LShr; break;
                default:                        return llvm::UndefValue::get(lhs->getType());
                }
                return llvm::ConstantFoldBinaryInstruction(opcode, lhs, rhs);
            }

            void emit_stmt(const ast::Stmt &stmt) {
                std::visit(
                    [&]<typename T>(const T &v) {
                        using V = std::decay_t<T>;
                        if constexpr (std::is_same_v<V, std::unique_ptr<ast::BlockStmt>>) {
                            const auto saved = locals_;
                            const bool is_loop_body = next_block_is_loop_body_;
                            next_block_is_loop_body_ = false;
                            defer_scopes_.push_back(DeferScope{.is_loop_body = is_loop_body});
                            for (const auto &s : v->stmts) {
                                if (builder_.GetInsertBlock()->getTerminator()) break;
                                emit_stmt(s);
                            }
                            // Emit defers for fall-through (not already terminated by return/break/continue)
                            if (!builder_.GetInsertBlock()->getTerminator()) {
                                emit_defers_in_scope(defer_scopes_.back());
                            }
                            defer_scopes_.pop_back();
                            locals_ = saved;
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::IfStmt>>) {
                            emit_if(*v);
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::WhileStmt>>) {
                            emit_while(*v);
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::ForInStmt>>) {
                            emit_for_in(*v);
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::SwitchStmt>>) {
                            emit_switch_stmt(*v);
                        } else if constexpr (std::is_same_v<V, ast::ExprStmt>) {
                            emit_expr(v.expr);
                        } else if constexpr (std::is_same_v<V, ast::VarDeclStmt>) {
                            const auto ty = v.type
                                                ? *sema::resolve_declared_type(v.type, v.init, *current_module_path_, const_cast<sema::Program &>(sema_program_), diag_, v.location)
                                                : current_exprs_->expr_types.at(sema::get_expr_key(*v.init));
                            const auto type_module = v.type
                                                         ? type_module_for_ast_type(*v.type, *current_module_path_, ty)
                                                         : *current_module_path_;
                            auto *slot = create_entry_alloca(current_function_, llvm_type_for(ty, type_module), v.name);
                            locals_[v.name] = LocalValue{.alloca = slot, .type = ty, .type_module = type_module};
                            if (v.init) {
                                if (!std::holds_alternative<ast::UndefinedExpr>(*v.init)) {
                                    builder_.CreateStore(emit_value_as(*v.init, ty, type_module), slot);
                                }
                            } else {
                                builder_.CreateStore(emit_default_value(type_module, ty), slot);
                            }
                        } else if constexpr (std::is_same_v<V, ast::VarDeclGroupStmt>) {
                            const ast::CallExpr *call_ptr = nullptr;
                            bool is_try = false;
                            if (const auto *c = std::get_if<std::unique_ptr<ast::CallExpr>>(&v.init)) {
                                call_ptr = c->get();
                            } else if (const auto *t = std::get_if<std::unique_ptr<ast::TryExpr>>(&v.init)) {
                                is_try = true;
                                call_ptr = std::get_if<std::unique_ptr<ast::CallExpr>>(&(*t)->call)->get();
                            }
                            auto *result = emit_call(*call_ptr);
                            const auto [type_module, all_returns] = call_return_types(*call_ptr);

                            if (is_try) {
                                // Extract error (last slot), branch on it
                                auto *err_val = all_returns.size() == 1
                                    ? result
                                    : builder_.CreateExtractValue(result, {static_cast<unsigned>(all_returns.size() - 1)});
                                emit_try_propagation(err_val, all_returns.back(), {});
                                // On ok path: bind non-error names (all_returns[0..n-2])
                                for (size_t i = 0; i < v.names.size(); ++i) {
                                    if (v.names[i].empty() || v.names[i] == "_") continue;
                                    auto ty = all_returns[i];
                                    auto *slot = create_entry_alloca(current_function_, llvm_type_for(ty, type_module), v.names[i]);
                                    auto *value = builder_.CreateExtractValue(result, {static_cast<unsigned>(i)});
                                    builder_.CreateStore(value, slot);
                                    locals_[v.names[i]] = LocalValue{.alloca = slot, .type = ty, .type_module = type_module};
                                }
                            } else {
                                for (size_t i = 0; i < v.names.size(); ++i) {
                                    if (v.names[i].empty() || v.names[i] == "_") continue;
                                    auto ty = all_returns[i];
                                    auto *slot = create_entry_alloca(current_function_, llvm_type_for(ty, type_module), v.names[i]);
                                    auto *value = all_returns.size() == 1 ? result : builder_.CreateExtractValue(result, {static_cast<unsigned>(i)});
                                    builder_.CreateStore(value, slot);
                                    locals_[v.names[i]] = LocalValue{.alloca = slot, .type = ty, .type_module = type_module};
                                }
                            }
                        } else if constexpr (std::is_same_v<V, ast::ContinueStmt>) {
                            emit_defers_for_loop_exit();
                            builder_.CreateBr(continue_targets_.back());
                        } else if constexpr (std::is_same_v<V, ast::BreakStmt>) {
                            emit_defers_for_loop_exit();
                            builder_.CreateBr(break_targets_.back());
                        } else if constexpr (std::is_same_v<V, ast::ReturnStmt>) {
                            emit_return(v);
                        } else if constexpr (std::is_same_v<V, ast::ReturnErrStmt>) {
                            emit_return_err(v);
                        } else if constexpr (std::is_same_v<V, ast::ReturnOkStmt>) {
                            emit_return_ok(v);
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::DeferStmt>>) {
                            // Register defer in the current scope; emit at scope exit
                            defer_scopes_.back().defers.push_back(v.get());
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::WhenStmt>>) {
                            emit_when_stmt(*v);
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::AsmStmt>>) {
                            emit_asm_stmt(*v);
                        }
                        // ast::LinkDecl/ast::DiagnosticDecl reaching here is a sema error (rejected
                        // in check_stmt) — nothing to emit even in an already-erroring program.
                    },
                    stmt);
            }

            // Emits a 'when' statement's SELECTED branch's statements directly at the
            // current insertion point — no new basic blocks, no CreateCondBr, since sema
            // guarantees the condition is always a compile-time constant (see
            // ProgramModule::when_stmt_selected). This is the first dead-branch-elision
            // construct in this codegen: the unselected branch is never even visited here,
            // which matters concretely for e.g. an 'ext fn' declared only in a dead
            // module-scope 'when' branch — it was never registered as a symbol in the
            // first place (see sema_declare.cpp's declare_when_decl), so nothing here
            // needs a separate "is this decl dead" check for that case either.
            void emit_when_block(const ast::BlockStmt &block) {
                const auto saved = locals_;
                const bool is_loop_body = next_block_is_loop_body_;
                next_block_is_loop_body_ = false;
                defer_scopes_.push_back(DeferScope{.is_loop_body = is_loop_body});
                for (const auto &s : block.stmts) {
                    if (builder_.GetInsertBlock()->getTerminator()) break;
                    emit_stmt(s);
                }
                if (!builder_.GetInsertBlock()->getTerminator()) {
                    emit_defers_in_scope(defer_scopes_.back());
                }
                defer_scopes_.pop_back();
                locals_ = saved;
            }

            void emit_when_stmt(const ast::WhenStmt &stmt) {
                const auto it = current_exprs_->when_stmt_selected.find(&stmt);
                const bool selected = it != current_exprs_->when_stmt_selected.end() && it->second;

                if (selected) {
                    emit_when_block(stmt.then_block);
                    return;
                }
                if (stmt.else_branch) {
                    std::visit(
                        [&]<typename EV>(const EV &else_v) {
                            using EVT = std::decay_t<EV>;
                            if constexpr (std::is_same_v<EVT, ast::BlockStmt>) {
                                emit_when_block(else_v);
                            } else { // std::unique_ptr<ast::WhenStmt>
                                emit_when_stmt(*else_v);
                            }
                        },
                        *stmt.else_branch);
                }
            }

            struct AsmVarRef {
                llvm::Value *ptr = nullptr;
                llvm::Type *storage_type = nullptr;
            };

            // Resolves an asm operand's bare variable name the same way emit_lvalue's IdentExpr
            // case does (locals first, then module-level globals) — sema's check_asm_stmt
            // already validated the name resolves and is scalar/pointer, so this always
            // succeeds for a program that reached codegen.
            auto resolve_asm_variable(const std::string &name) -> AsmVarRef {
                if (const auto it = locals_.find(name); it != locals_.end()) {
                    return AsmVarRef{.ptr = it->second.alloca, .storage_type = llvm_type_for(it->second.type, it->second.type_module)};
                }
                if (const auto sym = current_module_->symbols.find(name); sym != current_module_->symbols.end()) {
                    if (const auto *g = std::get_if<sema::GlobalSymbol>(&sym->second)) {
                        const auto type_module = g->decl->type
                                                     ? type_module_for_ast_type(*g->decl->type, *current_module_path_, g->type)
                                                     : *current_module_path_;
                        return AsmVarRef{.ptr = globals_.at(global_key(*current_module_path_, name)), .storage_type = llvm_type_for(g->type, type_module)};
                    }
                }
                return AsmVarRef{};
            }

            // Lowers a resolved 'asm { ... }' block to a single LLVM InlineAsm call. Every
            // instruction is rendered into one shared asm string (joined by "\n\t") and one
            // shared constraint string, matching how sema's clobber-set computation
            // (AsmStmtInfo) already aggregates across the whole block rather than per
            // instruction.
            //
            // All of this design's outputs ('&var' operands) use LLVM's indirect memory
            // constraint ("=*m"): the variable's alloca pointer is passed as an ordinary call
            // argument (never as part of the call's return value — indirect outputs never
            // consume a return-value slot), so the InlineAsm's return type is always void, and
            // BOTH outputs and inputs are passed as call arguments, in that order — mirroring
            // the constraint string, which likewise lists every "=*m" output before every "r"
            // input. The '$N' placeholders written into the asm string use that same
            // outputs-first-then-inputs numbering.
            void emit_asm_stmt(const ast::AsmStmt &stmt) {
                const auto &info = current_exprs_->asm_stmt_info.at(&stmt);

                enum class RenderKind : uint8_t { Literal, Output, Input };
                struct RenderedOperand {
                    RenderKind kind;
                    std::string literal_text;
                    size_t index = 0; // meaningful for Output/Input, before the '$N' offset below
                };

                std::vector<llvm::Value *> output_args;
                std::vector<llvm::Type *> output_elem_types; // parallel to output_args — the
                                                              // pointee type, required by LLVM's
                                                              // 'elementtype' attribute on every
                                                              // indirect ("=*m") call argument
                                                              // now that pointers are opaque
                std::vector<llvm::Value *> input_args;
                std::vector<std::vector<RenderedOperand>> rendered(stmt.instructions.size());

                for (size_t ii = 0; ii < stmt.instructions.size(); ++ii) {
                    const auto &instr = stmt.instructions[ii];
                    auto &row = rendered[ii];
                    row.reserve(instr.operands.size());

                    for (const auto &operand : instr.operands) {
                        std::visit(
                            [&]<typename T>(const T &op) {
                                using OpT = std::decay_t<T>;
                                if constexpr (std::is_same_v<OpT, ast::AsmRegisterOperand>) {
                                    row.push_back(RenderedOperand{.kind = RenderKind::Literal, .literal_text = op.name});
                                } else if constexpr (std::is_same_v<OpT, ast::AsmImmediateOperand>) {
                                    row.push_back(RenderedOperand{.kind = RenderKind::Literal, .literal_text = std::to_string(op.value)});
                                } else { // ast::AsmVariableOperand
                                    const auto ref = resolve_asm_variable(op.name);
                                    if (ref.ptr == nullptr || ref.storage_type == nullptr) {
                                        // sema resolves and validates every asm operand name
                                        // before codegen runs, so a miss here is an internal
                                        // inconsistency. Report it: the alternative was passing a
                                        // null pointer and null element type straight into
                                        // CreateLoad / the inline-asm argument list, which either
                                        // crashes LLVM or, for a raw memory operand, silently
                                        // encodes a wrong address.
                                        report_codegen_error(diag_, op.location, std::format(
                                            "internal error: inline asm operand '{}' did not resolve to a variable", op.name));
                                        return;
                                    }
                                    if (op.is_address) {
                                        output_args.push_back(ref.ptr);
                                        output_elem_types.push_back(ref.storage_type);
                                        row.push_back(RenderedOperand{.kind = RenderKind::Output, .index = output_args.size() - 1});
                                    } else {
                                        auto *value = builder_.CreateLoad(ref.storage_type, ref.ptr);
                                        input_args.push_back(value);
                                        row.push_back(RenderedOperand{.kind = RenderKind::Input, .index = input_args.size() - 1});
                                    }
                                }
                            },
                            operand);
                    }
                }

                std::string asm_string;
                for (size_t ii = 0; ii < stmt.instructions.size(); ++ii) {
                    if (ii > 0) {
                        asm_string += "\n\t";
                    }
                    asm_string += stmt.instructions[ii].mnemonic;
                    const auto &row = rendered[ii];
                    for (size_t oi = 0; oi < row.size(); ++oi) {
                        asm_string += (oi == 0) ? " " : ", ";
                        const auto &r = row[oi];
                        if (r.kind == RenderKind::Literal) {
                            asm_string += r.literal_text;
                        } else if (r.kind == RenderKind::Output) {
                            asm_string += "$" + std::to_string(r.index);
                        } else { // Input — numbered after every output
                            asm_string += "$" + std::to_string(output_args.size() + r.index);
                        }
                    }
                }

                std::vector<std::string> constraints;
                constraints.reserve(output_args.size() + input_args.size() + info.clobbered_families.size() + 2);
                for (size_t i = 0; i < output_args.size(); ++i) constraints.emplace_back("=*m");
                for (size_t i = 0; i < input_args.size(); ++i) constraints.emplace_back("r");
                for (const auto &family : info.clobbered_families) constraints.push_back("~{" + family + "}");
                if (info.clobbers_memory) constraints.emplace_back("~{memory}");
                constraints.emplace_back("~{dirflag}");

                std::string constraint_string;
                for (size_t i = 0; i < constraints.size(); ++i) {
                    if (i > 0) constraint_string += ",";
                    constraint_string += constraints[i];
                }

                auto *ptr_type = llvm::PointerType::getUnqual(*context_);
                std::vector<llvm::Type *> param_types(output_args.size(), ptr_type);
                for (auto *value : input_args) {
                    param_types.push_back(value->getType());
                }

                auto *fn_type = llvm::FunctionType::get(llvm::Type::getVoidTy(*context_), param_types, false);
                auto *inline_asm = llvm::InlineAsm::get(fn_type, asm_string, constraint_string,
                    /*hasSideEffects=*/true, /*isAlignStack=*/false, llvm::InlineAsm::AD_Intel);

                std::vector<llvm::Value *> call_args = output_args;
                call_args.insert(call_args.end(), input_args.begin(), input_args.end());
                auto *call = builder_.CreateCall(inline_asm, call_args);

                // Every indirect ("=*m") argument needs an explicit 'elementtype' attribute —
                // opaque pointers no longer carry a pointee type, and LLVM's verifier rejects
                // an indirect inline-asm operand without one ("Operand for indirect constraint
                // must have elementtype attribute").
                for (size_t i = 0; i < output_elem_types.size(); ++i) {
                    call->addParamAttr(static_cast<unsigned>(i), llvm::Attribute::get(*context_, llvm::Attribute::ElementType, output_elem_types[i]));
                }
            }

            // Lowers a resolved 'asm -> reg [: type] { ... }' EXPRESSION to a single LLVM
            // InlineAsm call, structurally parallel to emit_asm_stmt above but NOT sharing code
            // with it: emit_asm_stmt's '$N' template numbering assumes every output starts at
            // index 0, which no longer holds once a fixed-register output constraint is
            // prepended here, so this is a deliberate, small (~60 line) duplication rather than
            // touching emit_asm_stmt (which must remain byte-for-byte behaviorally unchanged).
            //
            // The result register's output constraint ("={regname}") is a DIRECT-VALUE output —
            // unlike '&var' outputs (indirect "=*m", realized as a pointer call-argument), this
            // one contributes NO call argument at all; its value is realized purely via the
            // call's return type. LLVM numbers '$N' template placeholders by constraint-list
            // position, output constraints before input constraints, regardless of whether the
            // template text references a given operand — so prepending "={reg}" (constraint
            // index 0, no '$N' reference needed since the register is already pinned by name)
            // shifts every '&var' output's '$N' to '1 + index' and every input's to
            // '1 + output_args.size() + index'. Call-ARGUMENT indices (and thus the
            // 'elementtype' attribute loop below) are completely unaffected by this shift, since
            // the fixed-register output contributes no argument — the first '&var' output
            // pointer is still argument index 0, exactly as in emit_asm_stmt.
            auto emit_asm_expr(const ast::AsmExpr &expr, const sema::ResolvedType &result_ty) -> llvm::Value * {
                const auto &info = current_exprs_->asm_expr_info.at(&expr);

                enum class RenderKind : uint8_t { Literal, Output, Input };
                struct RenderedOperand {
                    RenderKind kind;
                    std::string literal_text;
                    size_t index = 0;
                };

                std::vector<llvm::Value *> output_args;
                std::vector<llvm::Type *> output_elem_types;
                std::vector<llvm::Value *> input_args;
                std::vector<std::vector<RenderedOperand>> rendered(expr.instructions.size());

                for (size_t ii = 0; ii < expr.instructions.size(); ++ii) {
                    const auto &instr = expr.instructions[ii];
                    auto &row = rendered[ii];
                    row.reserve(instr.operands.size());

                    for (const auto &operand : instr.operands) {
                        std::visit(
                            [&]<typename T>(const T &op) {
                                using OpT = std::decay_t<T>;
                                if constexpr (std::is_same_v<OpT, ast::AsmRegisterOperand>) {
                                    row.push_back(RenderedOperand{.kind = RenderKind::Literal, .literal_text = op.name});
                                } else if constexpr (std::is_same_v<OpT, ast::AsmImmediateOperand>) {
                                    row.push_back(RenderedOperand{.kind = RenderKind::Literal, .literal_text = std::to_string(op.value)});
                                } else { // ast::AsmVariableOperand
                                    const auto ref = resolve_asm_variable(op.name);
                                    if (ref.ptr == nullptr || ref.storage_type == nullptr) {
                                        // sema resolves and validates every asm operand name
                                        // before codegen runs, so a miss here is an internal
                                        // inconsistency. Report it: the alternative was passing a
                                        // null pointer and null element type straight into
                                        // CreateLoad / the inline-asm argument list, which either
                                        // crashes LLVM or, for a raw memory operand, silently
                                        // encodes a wrong address.
                                        report_codegen_error(diag_, op.location, std::format(
                                            "internal error: inline asm operand '{}' did not resolve to a variable", op.name));
                                        return;
                                    }
                                    if (op.is_address) {
                                        output_args.push_back(ref.ptr);
                                        output_elem_types.push_back(ref.storage_type);
                                        row.push_back(RenderedOperand{.kind = RenderKind::Output, .index = output_args.size() - 1});
                                    } else {
                                        auto *value = builder_.CreateLoad(ref.storage_type, ref.ptr);
                                        input_args.push_back(value);
                                        row.push_back(RenderedOperand{.kind = RenderKind::Input, .index = input_args.size() - 1});
                                    }
                                }
                            },
                            operand);
                    }
                }

                std::string asm_string;
                for (size_t ii = 0; ii < expr.instructions.size(); ++ii) {
                    if (ii > 0) {
                        asm_string += "\n\t";
                    }
                    asm_string += expr.instructions[ii].mnemonic;
                    const auto &row = rendered[ii];
                    for (size_t oi = 0; oi < row.size(); ++oi) {
                        asm_string += (oi == 0) ? " " : ", ";
                        const auto &r = row[oi];
                        if (r.kind == RenderKind::Literal) {
                            asm_string += r.literal_text;
                        } else if (r.kind == RenderKind::Output) {
                            asm_string += "$" + std::to_string(1 + r.index);
                        } else { // Input — numbered after the fixed-register output and every '&var' output
                            asm_string += "$" + std::to_string(1 + output_args.size() + r.index);
                        }
                    }
                }

                std::vector<std::string> constraints;
                constraints.reserve(1 + output_args.size() + input_args.size() + info.clobbered_families.size() + 2);
                constraints.push_back("={" + expr.result_register.name + "}");
                for (size_t i = 0; i < output_args.size(); ++i) constraints.emplace_back("=*m");
                for (size_t i = 0; i < input_args.size(); ++i) constraints.emplace_back("r");
                for (const auto &family : info.clobbered_families) constraints.push_back("~{" + family + "}");
                if (info.clobbers_memory) constraints.emplace_back("~{memory}");
                constraints.emplace_back("~{dirflag}");

                std::string constraint_string;
                for (size_t i = 0; i < constraints.size(); ++i) {
                    if (i > 0) constraint_string += ",";
                    constraint_string += constraints[i];
                }

                auto *ptr_type = llvm::PointerType::getUnqual(*context_);
                std::vector<llvm::Type *> param_types(output_args.size(), ptr_type);
                for (auto *value : input_args) {
                    param_types.push_back(value->getType());
                }

                auto *fn_type = llvm::FunctionType::get(llvm_type(*current_module_path_, result_ty), param_types, false);
                auto *inline_asm = llvm::InlineAsm::get(fn_type, asm_string, constraint_string,
                    /*hasSideEffects=*/true, /*isAlignStack=*/false, llvm::InlineAsm::AD_Intel);

                std::vector<llvm::Value *> call_args = output_args;
                call_args.insert(call_args.end(), input_args.begin(), input_args.end());
                auto *call = builder_.CreateCall(inline_asm, call_args);

                for (size_t i = 0; i < output_elem_types.size(); ++i) {
                    call->addParamAttr(static_cast<unsigned>(i), llvm::Attribute::get(*context_, llvm::Attribute::ElementType, output_elem_types[i]));
                }

                return call;
            }

            void emit_if(const ast::IfStmt &stmt) {
                auto *fn = builder_.GetInsertBlock()->getParent();
                auto *then_bb = llvm::BasicBlock::Create(*context_, "if.then", fn);
                auto *else_bb = stmt.else_stmt ? llvm::BasicBlock::Create(*context_, "if.else", fn) : nullptr;
                auto *end_bb = llvm::BasicBlock::Create(*context_, "if.end", fn);
                auto *cond = coerce_to_bool(emit_expr(stmt.condition), current_exprs_->expr_types.at(sema::get_expr_key(stmt.condition)));
                builder_.CreateCondBr(cond, then_bb, else_bb ? else_bb : end_bb);

                builder_.SetInsertPoint(then_bb);
                emit_stmt(stmt.then_stmt);
                if (!builder_.GetInsertBlock()->getTerminator()) builder_.CreateBr(end_bb);

                if (stmt.else_stmt) {
                    builder_.SetInsertPoint(else_bb);
                    emit_stmt(*stmt.else_stmt);
                    if (!builder_.GetInsertBlock()->getTerminator()) builder_.CreateBr(end_bb);
                }

                builder_.SetInsertPoint(end_bb);
            }

            void emit_while(const ast::WhileStmt &stmt) {
                auto *fn = builder_.GetInsertBlock()->getParent();
                auto *cond_bb = llvm::BasicBlock::Create(*context_, "while.cond", fn);
                auto *body_bb = llvm::BasicBlock::Create(*context_, "while.body", fn);
                auto *end_bb = llvm::BasicBlock::Create(*context_, "while.end", fn);
                builder_.CreateBr(cond_bb);

                builder_.SetInsertPoint(cond_bb);
                auto *cond = coerce_to_bool(emit_expr(stmt.condition), current_exprs_->expr_types.at(sema::get_expr_key(stmt.condition)));
                builder_.CreateCondBr(cond, body_bb, end_bb);

                builder_.SetInsertPoint(body_bb);
                continue_targets_.push_back(cond_bb);
                break_targets_.push_back(end_bb);
                next_block_is_loop_body_ = true;
                emit_stmt(stmt.body);
                break_targets_.pop_back();
                continue_targets_.pop_back();
                if (!builder_.GetInsertBlock()->getTerminator()) builder_.CreateBr(cond_bb);

                builder_.SetInsertPoint(end_bb);
            }

            void emit_for_in(const ast::ForInStmt &stmt) {
                auto *fn = builder_.GetInsertBlock()->getParent();
                const auto saved_locals = locals_;

                if (const auto *rp = std::get_if<std::unique_ptr<ast::RangeExpr>>(&stmt.iterable)) {
                    const auto &range = **rp;
                    const auto upper_sema_type = current_exprs_->expr_types.at(sema::get_expr_key(range.upper));
                    auto *int_ty = llvm_type_for(upper_sema_type, *current_module_path_);

                    auto *idx_slot = create_entry_alloca(current_function_, int_ty, "for.idx");
                    llvm::Value *lower_val = range.lower
                        ? emit_expr(*range.lower)
                        : llvm::ConstantInt::get(int_ty, 0);
                    builder_.CreateStore(lower_val, idx_slot);
                    llvm::Value *upper_val = emit_expr(range.upper);

                    if (stmt.index_name != "_") {
                        locals_[stmt.index_name] = LocalValue{.alloca = idx_slot, .type = sema::ResolvedType{.kind = sema::TypeKind::USize}, .type_module = *current_module_path_};
                    }
                    llvm::AllocaInst *elem_slot = nullptr;
                    if (stmt.element_name != "_") {
                        elem_slot = create_entry_alloca(current_function_, int_ty, stmt.element_name);
                        locals_[stmt.element_name] = LocalValue{.alloca = elem_slot, .type = upper_sema_type, .type_module = *current_module_path_};
                    }

                    auto *cond_bb = llvm::BasicBlock::Create(*context_, "for.cond", fn);
                    auto *body_bb = llvm::BasicBlock::Create(*context_, "for.body", fn);
                    auto *step_bb = llvm::BasicBlock::Create(*context_, "for.step", fn);
                    auto *end_bb  = llvm::BasicBlock::Create(*context_, "for.end",  fn);
                    builder_.CreateBr(cond_bb);

                    builder_.SetInsertPoint(cond_bb);
                    auto *idx = builder_.CreateLoad(int_ty, idx_slot, "for.idx");
                    auto *cmp = upper_sema_type.is_signed()
                        ? builder_.CreateICmpSLT(idx, upper_val, "for.cond")
                        : builder_.CreateICmpULT(idx, upper_val, "for.cond");
                    builder_.CreateCondBr(cmp, body_bb, end_bb);

                    builder_.SetInsertPoint(body_bb);
                    if (elem_slot) {
                        builder_.CreateStore(builder_.CreateLoad(int_ty, idx_slot), elem_slot);
                    }
                    continue_targets_.push_back(step_bb);
                    break_targets_.push_back(end_bb);
                    next_block_is_loop_body_ = true;
                    emit_stmt(stmt.body);
                    break_targets_.pop_back();
                    continue_targets_.pop_back();
                    if (!builder_.GetInsertBlock()->getTerminator()) builder_.CreateBr(step_bb);

                    builder_.SetInsertPoint(step_bb);
                    auto *idx2 = builder_.CreateLoad(int_ty, idx_slot);
                    builder_.CreateStore(builder_.CreateAdd(idx2, llvm::ConstantInt::get(int_ty, 1)), idx_slot);
                    builder_.CreateBr(cond_bb);

                    builder_.SetInsertPoint(end_bb);
                    locals_ = saved_locals;
                    return;
                }

                const auto iterable_type = current_exprs_->expr_types.at(sema::get_expr_key(stmt.iterable));
                const auto type_module = expr_type_module_hint(stmt.iterable);
                const bool is_array = (iterable_type.kind == sema::TypeKind::Array);

                sema::ResolvedType elem_type;
                llvm::Value *base = nullptr;
                llvm::Value *len_val = nullptr;

                if (is_array) {
                    const auto &array_info = sema_program_.arrays.at(iterable_type.array_index);
                    elem_type = array_info.element_type;
                    base = emit_lvalue(stmt.iterable).ptr;
                    len_val = builder_.getInt64(array_info.count);
                } else {
                    auto *slice_val = emit_expr(stmt.iterable);
                    elem_type = sema_program_.slices.at(iterable_type.slice_index).element_type;
                    base = builder_.CreateExtractValue(slice_val, {0}, "for.base");
                    len_val = builder_.CreateExtractValue(slice_val, {1}, "for.len");
                }

                auto *idx_slot = create_entry_alloca(current_function_, llvm::Type::getInt64Ty(*context_), "for.idx");
                builder_.CreateStore(builder_.getInt64(0), idx_slot);
                if (stmt.index_name != "_") {
                    locals_[stmt.index_name] = LocalValue{.alloca = idx_slot, .type = sema::ResolvedType{.kind = sema::TypeKind::USize}, .type_module = *current_module_path_};
                }

                llvm::AllocaInst *elem_slot = nullptr;
                if (stmt.element_name != "_") {
                    if (stmt.element_by_ref) {
                        auto *ptr_ll_ty = llvm::PointerType::getUnqual(*context_);
                        elem_slot = create_entry_alloca(current_function_, ptr_ll_ty, stmt.element_name);
                        const auto &pointees = sema_program_.pointer_pointees;
                        int ptr_idx = 0;
                        for (int i = 0; i < (int)pointees.size(); i++) {
                            if (pointees[i] == elem_type) { ptr_idx = i; break; }
                        }
                        auto ptr_type = sema::ResolvedType{.kind = sema::TypeKind::Pointer, .pointee_index = ptr_idx};
                        locals_[stmt.element_name] = LocalValue{.alloca = elem_slot, .type = ptr_type, .type_module = type_module};
                    } else {
                        auto *elem_ll_ty = llvm_type_for(elem_type, type_module);
                        elem_slot = create_entry_alloca(current_function_, elem_ll_ty, stmt.element_name);
                        locals_[stmt.element_name] = LocalValue{.alloca = elem_slot, .type = elem_type, .type_module = type_module};
                    }
                }

                auto *cond_bb = llvm::BasicBlock::Create(*context_, "for.cond", fn);
                auto *body_bb = llvm::BasicBlock::Create(*context_, "for.body", fn);
                auto *step_bb = llvm::BasicBlock::Create(*context_, "for.step", fn);
                auto *end_bb = llvm::BasicBlock::Create(*context_, "for.end", fn);
                builder_.CreateBr(cond_bb);

                builder_.SetInsertPoint(cond_bb);
                auto *idx = builder_.CreateLoad(llvm::Type::getInt64Ty(*context_), idx_slot, "for.idx");
                builder_.CreateCondBr(builder_.CreateICmpULT(idx, len_val, "for.cond"), body_bb, end_bb);

                builder_.SetInsertPoint(body_bb);
                auto *elem_ll_ty = llvm_type_for(elem_type, type_module);
                llvm::Value *elem_ptr;
                if (is_array) {
                    llvm::Value *indices[] = {builder_.getInt64(0), idx};
                    elem_ptr = builder_.CreateInBoundsGEP(llvm_type_for(iterable_type, type_module), base, indices, "for.elem.ptr");
                } else {
                    elem_ptr = builder_.CreateInBoundsGEP(elem_ll_ty, base, idx, "for.elem.ptr");
                }
                if (elem_slot) {
                    if (stmt.element_by_ref) {
                        builder_.CreateStore(elem_ptr, elem_slot);
                    } else {
                        builder_.CreateStore(builder_.CreateLoad(elem_ll_ty, elem_ptr), elem_slot);
                    }
                }

                continue_targets_.push_back(step_bb);
                break_targets_.push_back(end_bb);
                next_block_is_loop_body_ = true;
                emit_stmt(stmt.body);
                break_targets_.pop_back();
                continue_targets_.pop_back();
                if (!builder_.GetInsertBlock()->getTerminator()) builder_.CreateBr(step_bb);

                builder_.SetInsertPoint(step_bb);
                auto *idx2 = builder_.CreateLoad(llvm::Type::getInt64Ty(*context_), idx_slot);
                builder_.CreateStore(builder_.CreateAdd(idx2, builder_.getInt64(1)), idx_slot);
                builder_.CreateBr(cond_bb);

                builder_.SetInsertPoint(end_bb);
                locals_ = saved_locals;
            }

            void emit_return(const ast::ReturnStmt &stmt) {
                if (stmt.return_values.size() == 1 && current_returns_.size() > 1) {
                    if (const auto *call = std::get_if<std::unique_ptr<ast::CallExpr>>(&stmt.return_values[0])) {
                        auto *result = emit_call(**call);
                        emit_defers_for_return();
                        builder_.CreateRet(result);
                        return;
                    }
                }

                // Evaluate return values first, before running defers
                std::vector<llvm::Value *> ret_vals;
                ret_vals.reserve(stmt.return_values.size());
                for (size_t i = 0; i < stmt.return_values.size(); ++i) {
                    // Last slot of a function returning 'error(...)': sema resolves a bare
                    // '.Variant' operand's type to the concrete error MEMBER type, not the
                    // Ok/Failed wrapper (see resolve_return_err_member_type), so it needs the
                    // same wrapping emit_return_err uses instead of plain emit_value_as, which
                    // has no error-union-aware coercion. A value that's already the full
                    // wrapper type passes through emit_error_return_operand unchanged.
                    if (i == stmt.return_values.size() - 1) {
                        const auto *union_info = current_returns_[i].kind == sema::TypeKind::Union
                            ? sema_program_.union_at(current_returns_[i].union_index) : nullptr;
                        if (union_info && union_info->is_error_union) {
                            ret_vals.push_back(emit_error_return_operand(stmt.return_values[i], current_returns_[i]));
                            continue;
                        }
                    }
                    ret_vals.push_back(emit_value_as(stmt.return_values[i], current_returns_[i], *current_module_path_));
                }

                // Run all defers in LIFO order
                emit_defers_for_return();

                if (ret_vals.empty()) {
                    builder_.CreateRetVoid();
                    return;
                }
                if (ret_vals.size() == 1) {
                    builder_.CreateRet(ret_vals.front());
                    return;
                }
                auto *agg_ty = llvm::cast<llvm::StructType>(return_type(*current_module_path_, current_returns_));
                llvm::Value *agg = llvm::UndefValue::get(agg_ty);
                for (size_t i = 0; i < ret_vals.size(); ++i) {
                    agg = builder_.CreateInsertValue(agg, ret_vals[i], {static_cast<unsigned>(i)});
                }
                builder_.CreateRet(agg);
            }

            // For '.Variant' / '.Variant(payload)' sugar (and any other bare error-member
            // expression) the operand's sema type is the concrete error MEMBER type (e.g.
            // MemoryError) — ordinary enum/tagged-union codegen (emit_expr) applies
            // unchanged; build_error_failed_value does the Ok/Failed wrapping. But sema also
            // allows the operand to already be a full error(...) value (this function's own,
            // or a subset of it — see error_union_is_subset in sema_check.cpp); that case is
            // propagated as-is, translating tags if the two unions differ, exactly like 'try'
            // propagation. Shared by emit_return_err and emit_return's trailing error slot.
            auto emit_error_return_operand(const ast::Expr &error_value, const sema::ResolvedType &target) -> llvm::Value * {
                const auto operand_type = current_exprs_->expr_types.at(sema::get_expr_key(error_value));
                auto *operand_val = emit_expr(error_value);

                if (operand_type.kind == sema::TypeKind::Union) {
                    const auto *union_info = sema_program_.union_at(operand_type.union_index);
                    if (union_info && union_info->is_error_union) {
                        return (operand_type == target) ? operand_val : translate_error_value(operand_val, operand_type, target);
                    }
                }

                return build_error_failed_value(operand_type, operand_val, target);
            }

            void emit_return_err(const ast::ReturnErrStmt &stmt) {
                emit_error_return(emit_error_return_operand(stmt.error_value, current_returns_.back()));
            }

            void emit_return_ok(const ast::ReturnOkStmt &stmt) {
                std::vector<llvm::Value *> ret_vals;
                ret_vals.reserve(stmt.return_values.size() + 1);
                for (size_t i = 0; i < stmt.return_values.size(); ++i) {
                    ret_vals.push_back(emit_value_as(stmt.return_values[i], current_returns_[i], *current_module_path_));
                }
                // Ok: outer tag = 0, payload undefined (Ok carries no payload).
                {
                    auto *ll_ty = unions_.at(current_returns_.back().union_index);
                    auto *slot = create_entry_alloca(builder_.GetInsertBlock()->getParent(), ll_ty);
                    builder_.CreateStore(builder_.getInt32(0), slot);
                    ret_vals.push_back(builder_.CreateLoad(ll_ty, slot));
                }

                emit_defers_for_return();

                if (ret_vals.size() == 1) {
                    builder_.CreateRet(ret_vals.front());
                    return;
                }
                auto *agg_ty = llvm::cast<llvm::StructType>(return_type(*current_module_path_, current_returns_));
                llvm::Value *agg = llvm::UndefValue::get(agg_ty);
                for (size_t i = 0; i < ret_vals.size(); ++i) {
                    agg = builder_.CreateInsertValue(agg, ret_vals[i], {static_cast<unsigned>(i)});
                }
                builder_.CreateRet(agg);
            }
        };
    }

    auto generate(const ast::Program &ast_program, const sema::Program &sema_program, DiagnosticEngine &diag, const Options &options) -> std::unique_ptr<llvm::Module> {
        return Generator(ast_program, sema_program, diag, options).run();
    }
}
