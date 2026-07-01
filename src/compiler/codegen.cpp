#include "codegen.hpp"

#include <llvm/ADT/APFloat.h>
#include <llvm/ADT/APInt.h>
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
#include <memory>
#include <ranges>
#include <string_view>
#include <unordered_map>
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
            case sema::TypeKind::Error:
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
            case sema::TypeKind::Error:
            case sema::TypeKind::Pointer:
            case sema::TypeKind::Anyptr:
                return 8;
            case sema::TypeKind::Slice:
                return 16;
            default:
                return 0;
            }
        }

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

            auto method_fn_key(const std::string &type_name, const std::string &method_name) const -> std::string {
                return type_name + "::" + method_name;
            }

            auto run() -> std::unique_ptr<llvm::Module> {
                declare_structs();
                declare_globals_and_functions();
                declare_methods();
                const sema::FunctionSymbol *entry_main = nullptr;
                if (!options_.freestanding) {
                    entry_main = validate_hosted_main();
                }
                emit_global_initializers();
                emit_functions();
                emit_methods();
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

            const std::string *current_module_path_ = nullptr;
            const sema::ProgramModule *current_module_ = nullptr;
            llvm::Function *current_function_ = nullptr;
            std::vector<llvm::BasicBlock *> continue_targets_;
            std::vector<llvm::BasicBlock *> break_targets_;
            std::vector<sema::ResolvedType> current_returns_;
            std::unordered_map<std::string, LocalValue> locals_;
            std::unordered_map<std::string, MacroArg> macro_args_;

            std::unordered_map<std::string, llvm::GlobalVariable *> globals_;
            std::unordered_map<FunctionKey, llvm::Function *, FunctionKeyHash> functions_;
            std::unordered_map<std::string, llvm::Function *> ext_functions_;
            std::unordered_map<int, StructLowering> structs_;
            size_t string_counter_ = 0;

            auto module_for(const std::string &path) const -> const sema::ProgramModule & {
                return sema_program_.modules.at(path);
            }

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
                    resolved.kind != sema::TypeKind::Array && resolved.kind != sema::TypeKind::Slice) {
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
                            type.kind == sema::TypeKind::Slice || type.kind == sema::TypeKind::Enum
                        ? type_module
                        : *current_module_path_,
                    type);
            }

            auto expr_type(const ast::Expr &expr) const -> sema::ResolvedType {
                const auto it = current_module_->expr_types.find(sema::get_expr_key(expr));
                if (it == current_module_->expr_types.end()) {
                    return sema::ResolvedType{.kind = sema::TypeKind::Invalid};
                }
                return it->second;
            }

            auto size_of(const std::string &module_path, const sema::ResolvedType &type) const -> uint32_t {
                if (type.kind == sema::TypeKind::Struct) {
                    return sema_program_.structs.at(type.struct_index).size;
                }
                if (type.kind == sema::TypeKind::Array) {
                    return module_for(module_path).arrays.at(type.array_index).size;
                }
                if (type.kind == sema::TypeKind::Slice) {
                    return 16;
                }
                if (type.kind == sema::TypeKind::Enum) {
                    return primitive_size(sema_program_.enums.at(type.enum_index).underlying_type.kind);
                }
                return primitive_size(type.kind);
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
                case sema::TypeKind::USize:
                case sema::TypeKind::Error:   return llvm::Type::getInt64Ty(*context_);
                case sema::TypeKind::F32:     return llvm::Type::getFloatTy(*context_);
                case sema::TypeKind::F64:     return llvm::Type::getDoubleTy(*context_);
                case sema::TypeKind::Pointer:
                case sema::TypeKind::Anyptr:  return llvm::PointerType::getUnqual(*context_);
                case sema::TypeKind::Struct:  return struct_lowering(type.struct_index).type;
                case sema::TypeKind::Array: {
                    const auto &array = module_for(module_path).arrays.at(type.array_index);
                    return llvm::ArrayType::get(llvm_type(module_path, array.element_type), array.count);
                }
                case sema::TypeKind::Slice:
                    return llvm::StructType::get(*context_, {llvm::PointerType::getUnqual(*context_), llvm::Type::getInt64Ty(*context_)});
                case sema::TypeKind::Enum: {
                    const auto &enum_info = sema_program_.enums.at(type.enum_index);
                    return llvm_type(module_path, enum_info.underlying_type);
                }
                default:                      return llvm::Type::getVoidTy(*context_);
                }
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

            auto function_type(const std::string &module_path, const std::vector<sema::ResolvedType> &params, const std::vector<sema::ResolvedType> &returns) -> llvm::FunctionType * {
                std::vector<llvm::Type *> param_types;
                for (const auto &param : params) {
                    param_types.push_back(llvm_type(module_path, param));
                }
                return llvm::FunctionType::get(return_type(module_path, returns), param_types, false);
            }

            void declare_globals_and_functions() {
                for (const auto &[path, mod] : sema_program_.modules) {
                    for (const auto &[name, sym] : mod.symbols) {
                        if (const auto *global = std::get_if<sema::GlobalSymbol>(&sym)) {
                            const auto gname = symbol_name(path, name);
                            globals_[path + "\n" + name] = new llvm::GlobalVariable(
                                *module_, llvm_type(path, global->type), !global->is_mut,
                                global->is_pub ? llvm::GlobalValue::ExternalLinkage : llvm::GlobalValue::InternalLinkage,
                                nullptr, gname);
                        } else if (const auto *fn = std::get_if<sema::FunctionSymbol>(&sym)) {
                            const bool entry_symbol = path == ast_program_.root_module_path && (name == "main" || (options_.freestanding && name == "_start"));
                            const auto fname = symbol_name(path, name, entry_symbol);
                            auto *llvm_fn = llvm::Function::Create(
                                function_type(path, fn->params, fn->return_types),
                                fn->is_pub || entry_symbol ? llvm::GlobalValue::ExternalLinkage : llvm::GlobalValue::InternalLinkage,
                                fname, *module_);
                            functions_[FunctionKey{path, name}] = llvm_fn;
                        } else if (const auto *ef = std::get_if<sema::ExtFunctionSymbol>(&sym)) {
                            std::vector<sema::ResolvedType> returns;
                            if (ef->return_type) {
                                returns.push_back(*ef->return_type);
                            }
                            ext_functions_[path + "\n" + name] = llvm::Function::Create(
                                function_type(path, ef->params, returns),
                                llvm::GlobalValue::ExternalLinkage,
                                name, *module_);
                        }
                    }
                }
            }

            auto global_key(const std::string &module_path, const std::string &name) const -> std::string {
                return module_path + "\n" + name;
            }

            auto zero_value(const std::string &module_path, const sema::ResolvedType &type) -> llvm::Constant * {
                return llvm::Constant::getNullValue(llvm_type(module_path, type));
            }

            auto validate_hosted_main() -> const sema::FunctionSymbol * {
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
                if (!returns_void && !returns_i32) {
                    report_codegen_error(diag_, decl.location, "hosted entry point must return either no value or i32");
                    return nullptr;
                }

                return main_fn;
            }

            void emit_global_initializers() {
                for (const auto &[path, mod] : sema_program_.modules) {
                    current_module_path_ = &path;
                    current_module_ = &mod;

                    for (const auto &[name, sym] : mod.symbols) {
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
                    current_module_path_ = &path;
                    current_module_ = &mod;

                    for (const auto &[name, sym] : mod.symbols) {
                        const auto *fn = std::get_if<sema::FunctionSymbol>(&sym);
                        if (!fn) {
                            continue;
                        }
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
                            std::vector<sema::ResolvedType> params_with_self;
                            // Placeholder for pointer — we use llvm::PointerType directly below
                            std::vector<llvm::Type *> param_types;
                            param_types.push_back(llvm::PointerType::getUnqual(*context_));
                            for (const auto &p : info.param_types) {
                                param_types.push_back(llvm_type(path, p));
                            }

                            llvm::Type *ret = info.return_types.empty()
                                                 ? llvm::Type::getVoidTy(*context_)
                                                 : llvm_type(path, info.return_types.front());
                            if (info.return_types.size() > 1) {
                                // Multi-return: return struct (simplified - use first for now)
                                // TODO: multi-return methods
                            }

                            auto *fn_type = llvm::FunctionType::get(ret, param_types, false);
                            const auto fname = symbol_name(path, method_fn_key(type_name, method_name));
                            auto *llvm_fn = llvm::Function::Create(
                                fn_type, llvm::GlobalValue::InternalLinkage, fname, *module_);
                            functions_[FunctionKey{path, method_fn_key(type_name, method_name)}] = llvm_fn;
                        }
                    }
                }
            }

            // Find the pointer ResolvedType for self_type in the current module's pointees.
            auto find_self_ptr_type(const sema::ResolvedType &self_type) -> sema::ResolvedType {
                for (size_t i = 0; i < current_module_->pointer_pointees.size(); ++i) {
                    if (current_module_->pointer_pointees[i] == self_type) {
                        return sema::ResolvedType{.kind = sema::TypeKind::Pointer, .pointee_index = static_cast<int>(i)};
                    }
                }
                return sema::ResolvedType{.kind = sema::TypeKind::Pointer, .pointee_index = 0};
            }

            void emit_method(const std::string &module_path, const std::string &type_name, const sema::MethodInfo &info) {
                const auto key = method_fn_key(type_name, info.decl->name);
                current_function_ = functions_.at(FunctionKey{module_path, key});
                current_returns_ = info.return_types;
                locals_.clear();
                macro_args_.clear();
                continue_targets_.clear();
                break_targets_.clear();

                auto *entry = llvm::BasicBlock::Create(*context_, "entry", current_function_);
                builder_.SetInsertPoint(entry);

                // First arg is self (a pointer to the struct/enum)
                auto arg_it = current_function_->arg_begin();
                arg_it->setName("self");
                auto *self_slot = create_entry_alloca(current_function_, llvm::PointerType::getUnqual(*context_), "self");
                builder_.CreateStore(&*arg_it, self_slot);
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
                    builder_.CreateStore(&*arg_it, slot);
                    locals_[param.name] = LocalValue{
                        .alloca = slot,
                        .type = info.param_types[index],
                        .type_module = type_module_for_ast_type(param.type, module_path, info.param_types[index]),
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
                    current_module_path_ = &path;
                    current_module_ = &mod;
                    for (const auto &[type_name, method_map] : mod.methods) {
                        for (const auto &[method_name, info] : method_map) {
                            if (!info.is_resolved) continue;
                            emit_method(path, type_name, info);
                        }
                    }
                }
            }

            void emit_start(const sema::FunctionSymbol &main_fn) {
                auto *start_ty = llvm::FunctionType::get(llvm::Type::getVoidTy(*context_), {}, false);
                auto *start = llvm::Function::Create(start_ty, llvm::GlobalValue::ExternalLinkage, "_start", *module_);
                start->setDoesNotReturn();

                auto *entry = llvm::BasicBlock::Create(*context_, "entry", start);
                builder_.SetInsertPoint(entry);

                auto *main = functions_.at(FunctionKey{ast_program_.root_module_path, "main"});
                llvm::Value *exit_code = nullptr;
                if (main_fn.return_types.empty()) {
                    builder_.CreateCall(main);
                    exit_code = builder_.getInt64(0);
                } else {
                    auto *main_result = builder_.CreateCall(main);
                    exit_code = builder_.CreateSExt(main_result, llvm::Type::getInt64Ty(*context_));
                }

                auto *syscall_ty = llvm::FunctionType::get(
                    llvm::Type::getVoidTy(*context_),
                    {llvm::Type::getInt64Ty(*context_), llvm::Type::getInt64Ty(*context_)},
                    false);
                auto *syscall = llvm::InlineAsm::get(
                    syscall_ty,
                    "syscall",
                    "{rax},{rdi},~{rcx},~{r11},~{memory}",
                    true);

                builder_.CreateCall(syscall, {builder_.getInt64(231), exit_code});
                builder_.CreateUnreachable();
            }

            auto create_entry_alloca(llvm::Function *fn, llvm::Type *type, llvm::StringRef name) -> llvm::AllocaInst * {
                llvm::IRBuilder<> tmp(&fn->getEntryBlock(), fn->getEntryBlock().begin());
                return tmp.CreateAlloca(type, nullptr, name);
            }

            void emit_function(const std::string &module_path, const std::string &name, const sema::FunctionSymbol &fn) {
                current_function_ = functions_.at(FunctionKey{module_path, name});
                current_returns_ = fn.return_types;
                locals_.clear();
                macro_args_.clear();
                continue_targets_.clear();
                break_targets_.clear();

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
                        .type_module = type_module_for_ast_type(param.type, module_path, fn.params[index]),
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
                if (type.is_float()) {
                    return builder_.CreateFCmpONE(value, llvm::ConstantFP::get(llvm_type(*current_module_path_, type), 0.0));
                }
                if (is_pointer_like(type)) {
                    return builder_.CreateICmpNE(builder_.CreatePtrToInt(value, llvm::Type::getInt64Ty(*context_)), builder_.getInt64(0));
                }
                return builder_.CreateICmpNE(value, llvm::ConstantInt::get(llvm_type(*current_module_path_, type), 0));
            }

            auto signedness(const sema::ResolvedType &type) const -> bool {
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

                if (is_pointer_like(type)) {
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
                const auto &pointee = current_module_->pointer_pointees.at(type.pointee_index);
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

                const auto lhs_type = current_module_->expr_types.at(sema::get_expr_key(expr.lhs));
                const auto rhs_type = current_module_->expr_types.at(sema::get_expr_key(expr.rhs));
                auto *lhs = emit_expr(expr.lhs);
                auto *rhs = emit_expr(expr.rhs);

                if (is_pointer_like(lhs_type) && rhs_type.is_integer() && (expr.op == ast::BinaryOp::Add || expr.op == ast::BinaryOp::Sub)) {
                    return emit_pointer_offset(lhs, rhs, lhs_type, expr.op == ast::BinaryOp::Sub);
                }
                if (is_pointer_like(rhs_type) && lhs_type.is_integer() && expr.op == ast::BinaryOp::Add) {
                    return emit_pointer_offset(rhs, lhs, rhs_type, false);
                }

                if (expr.op == ast::BinaryOp::Equal || expr.op == ast::BinaryOp::NotEqual ||
                    expr.op == ast::BinaryOp::Less || expr.op == ast::BinaryOp::Greater ||
                    expr.op == ast::BinaryOp::LessEqual || expr.op == ast::BinaryOp::GreaterEqual) {
                    if (is_pointer_like(lhs_type) && rhs_type == sema::ResolvedType{.kind = sema::TypeKind::Anyptr}) {
                        rhs = emit_cast(rhs, rhs_type, lhs_type);
                    } else if (lhs_type == sema::ResolvedType{.kind = sema::TypeKind::Anyptr} && is_pointer_like(rhs_type)) {
                        lhs = emit_cast(lhs, lhs_type, rhs_type);
                    }
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

                auto *lhs = coerce_to_bool(emit_expr(expr.lhs), current_module_->expr_types.at(sema::get_expr_key(expr.lhs)));
                auto *lhs_done = builder_.GetInsertBlock();
                if (expr.op == ast::BinaryOp::LogicalAnd) {
                    builder_.CreateCondBr(lhs, rhs_bb, merge_bb);
                } else {
                    builder_.CreateCondBr(lhs, merge_bb, rhs_bb);
                }

                builder_.SetInsertPoint(rhs_bb);
                auto *rhs = coerce_to_bool(emit_expr(expr.rhs), current_module_->expr_types.at(sema::get_expr_key(expr.rhs)));
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
                const auto &array = module_for(array_module).arrays.at(array_type.array_index);
                auto *ptr = builder_.CreateInBoundsGEP(
                    llvm_type(array_module, array_type),
                    lv.ptr,
                    {builder_.getInt32(0), builder_.getInt64(0)});
                llvm::Value *slice = llvm::UndefValue::get(llvm_type(array_module, slice_type));
                slice = builder_.CreateInsertValue(slice, ptr, {0});
                slice = builder_.CreateInsertValue(slice, builder_.getInt64(array.count), {1});
                return slice;
            }

            auto emit_value_as(const ast::Expr &expr, const sema::ResolvedType &target) -> llvm::Value * {
                const auto from = current_module_->expr_types.at(sema::get_expr_key(expr));
                if (from.kind == sema::TypeKind::Array && target.kind == sema::TypeKind::Slice) {
                    return emit_array_to_slice(expr, from, target, expr_type_module_hint(expr));
                }
                return emit_expr(expr);
            }

            auto emit_macro_arg(const MacroArg &arg) -> llvm::Value * {
                const auto *saved_path = current_module_path_;
                const auto *saved_module = current_module_;
                current_module_path_ = arg.module_path;
                current_module_ = arg.module;
                auto *value = emit_expr(*arg.expr);
                current_module_path_ = saved_path;
                current_module_ = saved_module;
                return value;
            }

            auto emit_const_macro_arg(const MacroArg &arg) -> llvm::Value * {
                const auto *saved_path = current_module_path_;
                const auto *saved_module = current_module_;
                current_module_path_ = arg.module_path;
                current_module_ = arg.module;
                auto *value = emit_const_or_runtime(*arg.expr, true);
                current_module_path_ = saved_path;
                current_module_ = saved_module;
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
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::IndexExpr>>) {
                            const auto operand_ty = current_module_->expr_types.at(sema::get_expr_key(v->operand));
                            if (operand_ty.kind == sema::TypeKind::Pointer || operand_ty.kind == sema::TypeKind::Array || operand_ty.kind == sema::TypeKind::Slice) {
                                return expr_type_module_hint(v->operand);
                            }
                        }
                        return *current_module_path_;
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
                                const auto ptr_type = current_module_->expr_types.at(sema::get_expr_key(v->operand));
                                const auto pointee = current_module_->pointer_pointees.at(ptr_type.pointee_index);
                                const auto pointee_module = expr_type_module_hint(v->operand);
                                return LValue{.ptr = emit_expr(v->operand), .type = pointee, .type_module = pointee_module, .storage_type = llvm_type_for(pointee, pointee_module)};
                            }
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::MemberExpr>>) {
                            return emit_member_lvalue(*v);

                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::IndexExpr>>) {
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

                const auto object_type = current_module_->expr_types.at(sema::get_expr_key(member.object));
                sema::ResolvedType struct_type;
                std::string struct_module = *current_module_path_;
                llvm::Value *base = nullptr;
                if (object_type.kind == sema::TypeKind::Pointer) {
                    struct_type = current_module_->pointer_pointees.at(object_type.pointee_index);
                    struct_module = expr_type_module_hint(member.object);
                    base = emit_expr(member.object);
                } else {
                    auto lv = emit_lvalue(member.object);
                    struct_type = lv.type;
                    struct_module = lv.type_module;
                    base = lv.ptr;
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

            auto emit_index_lvalue(const ast::IndexExpr &index) -> LValue {
                const auto operand_type = current_module_->expr_types.at(sema::get_expr_key(index.operand));
                const auto type_module = expr_type_module_hint(index.operand);
                auto *idx = emit_expr(index.index);
                if (!idx->getType()->isIntegerTy(64)) {
                    idx = integer_cast(idx, llvm::Type::getInt64Ty(*context_), current_module_->expr_types.at(sema::get_expr_key(index.index)));
                }

                if (operand_type.kind == sema::TypeKind::Pointer) {
                    const auto element = current_module_->pointer_pointees.at(operand_type.pointee_index);
                    auto *ptr = builder_.CreateInBoundsGEP(llvm_type_for(element, type_module), emit_expr(index.operand), idx);
                    return LValue{.ptr = ptr, .type = element, .type_module = type_module, .storage_type = llvm_type_for(element, type_module)};
                }
                if (operand_type.kind == sema::TypeKind::Array) {
                    auto base = emit_lvalue(index.operand);
                    const auto element = module_for(type_module).arrays.at(operand_type.array_index).element_type;
                    auto *ptr = builder_.CreateInBoundsGEP(llvm_type(type_module, operand_type), base.ptr, {builder_.getInt32(0), idx});
                    return LValue{.ptr = ptr, .type = element, .type_module = type_module, .storage_type = llvm_type_for(element, type_module)};
                }
                if (operand_type.kind == sema::TypeKind::Slice) {
                    auto *slice = emit_expr(index.operand);
                    auto *base = builder_.CreateExtractValue(slice, {0});
                    const auto element = module_for(type_module).slices.at(operand_type.slice_index).element_type;
                    auto *ptr = builder_.CreateInBoundsGEP(llvm_type_for(element, type_module), base, idx);
                    return LValue{.ptr = ptr, .type = element, .type_module = type_module, .storage_type = llvm_type_for(element, type_module)};
                }
                report_codegen_error(diag_, index.location, "unsupported index operand in codegen");
                return {};
            }

            auto try_namespace_chain(const ast::Expr &expr) const -> std::optional<std::string> {
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

            auto emit_call(const ast::CallExpr &call) -> llvm::Value * {
                std::string target_module = *current_module_path_;
                std::string name;

                if (const auto *ident = std::get_if<ast::IdentExpr>(&call.callee)) {
                    name = ident->name;
                } else if (const auto *member = std::get_if<std::unique_ptr<ast::MemberExpr>>(&call.callee)) {
                    if (auto ns = try_namespace_chain((*member)->object)) {
                        target_module = *ns;
                        name = (*member)->member;
                    } else {
                        // Method call on a value or pointer
                        const auto obj_type = current_module_->expr_types.at(sema::get_expr_key((*member)->object));
                        sema::ResolvedType receiver_type = obj_type;
                        if (obj_type.kind == sema::TypeKind::Pointer) {
                            receiver_type = current_module_->pointer_pointees.at(obj_type.pointee_index);
                        }
                        const auto *method = sema::find_method(receiver_type, (*member)->member, sema_program_);
                        if (!method) {
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
                        for (size_t i = 0; i < call.args.size(); ++i) {
                            args.push_back(emit_value_as(call.args[i], method->param_types[i]));
                        }
                        return builder_.CreateCall(
                            functions_.at(FunctionKey{method->impl_module, method_fn_key(method->type_name, method->decl->name)}),
                            args);
                    }
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
                    for (size_t i = 0; i < macro->decl->params.size(); ++i) {
                        macro_args_[macro->decl->params[i].name] = MacroArg{.expr = &call.args[i], .module_path = current_module_path_, .module = current_module_};
                    }
                    const auto *saved_path = current_module_path_;
                    const auto *saved_module = current_module_;
                    current_module_path_ = &target_module;
                    current_module_ = &target;
                    auto *value = emit_expr(macro->decl->expr_template);
                    current_module_path_ = saved_path;
                    current_module_ = saved_module;
                    macro_args_ = std::move(saved);
                    return value;
                }

                std::vector<llvm::Value *> args;
                if (const auto *fn = std::get_if<sema::FunctionSymbol>(&sym_it->second)) {
                    for (size_t i = 0; i < call.args.size(); ++i) {
                        args.push_back(emit_value_as(call.args[i], fn->params[i]));
                    }
                    return builder_.CreateCall(functions_.at(FunctionKey{target_module, name}), args);
                }
                if (const auto *ef = std::get_if<sema::ExtFunctionSymbol>(&sym_it->second)) {
                    for (size_t i = 0; i < call.args.size(); ++i) {
                        args.push_back(emit_value_as(call.args[i], ef->params[i]));
                    }
                    return builder_.CreateCall(ext_functions_.at(global_key(target_module, name)), args);
                }
                return nullptr;
            }

            auto call_return_types(const ast::CallExpr &call) -> std::pair<std::string, std::vector<sema::ResolvedType>> {
                std::string target_module = *current_module_path_;
                std::string name;

                if (const auto *ident = std::get_if<ast::IdentExpr>(&call.callee)) {
                    name = ident->name;
                } else if (const auto *member = std::get_if<std::unique_ptr<ast::MemberExpr>>(&call.callee)) {
                    if (auto ns = try_namespace_chain((*member)->object)) {
                        target_module = *ns;
                        name = (*member)->member;
                    }
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

            auto emit_expr(const ast::Expr &expr) -> llvm::Value * {
                return std::visit(
                    [&]<typename T>(const T &v) -> llvm::Value * {
                        using V = std::decay_t<T>;
                        const auto ty = expr_type(expr);

                        if constexpr (std::is_same_v<V, ast::LiteralIntegerExpr>) {
                            return llvm::ConstantInt::get(llvm_type(*current_module_path_, ty), v.value, ty.is_signed());
                        } else if constexpr (std::is_same_v<V, ast::LiteralFloatExpr>) {
                            return llvm::ConstantFP::get(llvm_type(*current_module_path_, ty), v.value);
                        } else if constexpr (std::is_same_v<V, ast::LiteralStringExpr>) {
                            return emit_string_literal(v.value);
                        } else if constexpr (std::is_same_v<V, ast::LiteralBoolExpr>) {
                            return builder_.getInt1(v.value);
                        } else if constexpr (std::is_same_v<V, ast::LiteralNilExpr>) {
                            return llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context_));
                        } else if constexpr (std::is_same_v<V, ast::IdentExpr>) {
                            if (const auto macro = macro_args_.find(v.name); macro != macro_args_.end()) {
                                return emit_macro_arg(macro->second);
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
                            const auto operand_ty = current_module_->expr_types.at(sema::get_expr_key(v->operand));
                            switch (v->op) {
                            case ast::UnaryOp::Negate:     return operand_ty.is_float() ? builder_.CreateFNeg(operand) : builder_.CreateNeg(operand);
                            case ast::UnaryOp::LogicalNot: return builder_.CreateNot(coerce_to_bool(operand, operand_ty));
                            case ast::UnaryOp::BitwiseNot: return builder_.CreateNot(operand);
                            default:                       return operand;
                            }
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::BinaryExpr>>) {
                            return emit_binary_expr(*v);
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::TernaryExpr>>) {
                            return emit_ternary(*v);
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::AssignExpr>>) {
                            return emit_assign(*v);
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::CallExpr>>) {
                            return emit_call(*v);
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::IncrDecrExpr>>) {
                            return emit_incr_decr(*v);
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::SizeOfExpr>>) {
                            return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), sizeof_operand(*v));
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::LenExpr>>) {
                            const auto operand_type = current_module_->expr_types.at(sema::get_expr_key(v->operand));
                            if (operand_type.kind == sema::TypeKind::Array) {
                                return builder_.getInt64(module_for(expr_type_module_hint(v->operand)).arrays.at(operand_type.array_index).count);
                            }
                            auto *slice = emit_expr(v->operand);
                            return builder_.CreateExtractValue(slice, {1});
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::CastExpr>>) {
                            const auto from = current_module_->expr_types.at(sema::get_expr_key(v->value));
                            if (ty.kind == sema::TypeKind::Slice) {
                                return emit_slice_cast(*v, from, ty);
                            }
                            return emit_cast(emit_expr(v->value), from, ty);
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::IndexExpr>>) {
                            const auto lv = emit_lvalue(expr);
                            return builder_.CreateLoad(lv.storage_type, lv.ptr);
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::SliceExpr>>) {
                            return emit_slice_expr(*v, ty);
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::MemberExpr>>) {
                            const auto lv = emit_lvalue(expr);
                            return builder_.CreateLoad(lv.storage_type, lv.ptr);
                        } else if constexpr (std::is_same_v<V, ast::DotIdentExpr>) {
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
                return llvm::ConstantExpr::getInBoundsGetElementPtr(
                    array_ty, global, llvm::ArrayRef<llvm::Constant *>(indices));
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
                const auto operand_type = current_module_->expr_types.at(sema::get_expr_key(expr.operand));
                const auto type_module = expr_type_module_hint(expr.operand);
                auto *start = emit_expr(expr.start);
                auto *end = emit_expr(expr.end);
                if (!start->getType()->isIntegerTy(64)) start = integer_cast(start, llvm::Type::getInt64Ty(*context_), current_module_->expr_types.at(sema::get_expr_key(expr.start)));
                if (!end->getType()->isIntegerTy(64)) end = integer_cast(end, llvm::Type::getInt64Ty(*context_), current_module_->expr_types.at(sema::get_expr_key(expr.end)));
                auto *count = builder_.CreateSub(end, start);

                if (operand_type.kind == sema::TypeKind::Array) {
                    auto base = emit_lvalue(expr.operand);
                    auto *ptr = builder_.CreateInBoundsGEP(llvm_type(type_module, operand_type), base.ptr, {builder_.getInt32(0), start});
                    return build_slice_value(ptr, count, result_type, type_module);
                }

                auto *slice = emit_expr(expr.operand);
                auto *base = builder_.CreateExtractValue(slice, {0});
                const auto element = module_for(type_module).slices.at(operand_type.slice_index).element_type;
                auto *ptr = builder_.CreateInBoundsGEP(llvm_type_for(element, type_module), base, start);
                return build_slice_value(ptr, count, result_type, type_module);
            }

            auto emit_match(const ast::MatchExpr &expr, const sema::ResolvedType &result_type) -> llvm::Value * {
                auto *fn = builder_.GetInsertBlock()->getParent();
                auto *unreachable_bb = llvm::BasicBlock::Create(*context_, "match.unreachable", fn);
                auto *merge_bb = llvm::BasicBlock::Create(*context_, "match.end", fn);

                const auto operand_type = current_module_->expr_types.at(sema::get_expr_key(expr.operand));
                auto *operand = emit_expr(expr.operand);

                // Underlying integer type for the enum
                const auto &enum_info = sema_program_.enums.at(operand_type.enum_index);
                auto *underlying_llvm_ty = llvm_type(*current_module_path_, enum_info.underlying_type);

                auto *sw = builder_.CreateSwitch(operand, unreachable_bb, static_cast<unsigned>(expr.arms.size()));

                std::vector<std::pair<llvm::BasicBlock *, llvm::Value *>> arm_results;

                for (const auto &arm : expr.arms) {
                    // Find the enum field value
                    int64_t field_val = 0;
                    for (const auto &field : enum_info.fields) {
                        if (field.name == arm.field) {
                            field_val = field.value;
                            break;
                        }
                    }

                    auto *arm_bb = llvm::BasicBlock::Create(*context_, std::format("match.arm.{}", arm.field), fn);
                    sw->addCase(llvm::cast<llvm::ConstantInt>(llvm::ConstantInt::get(underlying_llvm_ty, static_cast<uint64_t>(field_val), enum_info.underlying_type.is_signed())), arm_bb);

                    builder_.SetInsertPoint(arm_bb);
                    auto *arm_val = emit_expr(arm.value);
                    auto *arm_done = builder_.GetInsertBlock();
                    builder_.CreateBr(merge_bb);
                    arm_results.emplace_back(arm_done, arm_val);
                }

                // Unreachable block (match is exhaustive)
                builder_.SetInsertPoint(unreachable_bb);
                builder_.CreateUnreachable();

                builder_.SetInsertPoint(merge_bb);
                if (result_type.kind == sema::TypeKind::Void || arm_results.empty()) {
                    return llvm::UndefValue::get(llvm::Type::getInt32Ty(*context_));
                }

                auto *phi = builder_.CreatePHI(llvm_type(*current_module_path_, result_type), static_cast<unsigned>(arm_results.size()));
                for (auto &[bb, val] : arm_results) {
                    phi->addIncoming(val, bb);
                }
                return phi;
            }

            auto emit_ternary(const ast::TernaryExpr &expr) -> llvm::Value * {
                auto *fn = builder_.GetInsertBlock()->getParent();
                auto *then_bb = llvm::BasicBlock::Create(*context_, "ternary.then", fn);
                auto *else_bb = llvm::BasicBlock::Create(*context_, "ternary.else", fn);
                auto *merge_bb = llvm::BasicBlock::Create(*context_, "ternary.end", fn);

                auto *cond = coerce_to_bool(emit_expr(expr.condition), current_module_->expr_types.at(sema::get_expr_key(expr.condition)));
                builder_.CreateCondBr(cond, then_bb, else_bb);

                builder_.SetInsertPoint(then_bb);
                auto *then_value = emit_expr(expr.then_expr);
                auto *then_done = builder_.GetInsertBlock();
                builder_.CreateBr(merge_bb);

                builder_.SetInsertPoint(else_bb);
                auto *else_value = emit_expr(expr.else_expr);
                auto *else_done = builder_.GetInsertBlock();
                builder_.CreateBr(merge_bb);

                builder_.SetInsertPoint(merge_bb);
                auto result_ty = current_module_->expr_types.at(sema::get_expr_key(expr.then_expr));
                auto *phi = builder_.CreatePHI(llvm_type(*current_module_path_, result_ty), 2);
                phi->addIncoming(then_value, then_done);
                phi->addIncoming(else_value, else_done);
                return phi;
            }

            auto compound_op(ast::AssignOp op) -> ast::BinaryOp {
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
                case ast::AssignOp::Assign:    return ast::BinaryOp::Add;
                }
                return ast::BinaryOp::Add;
            }

            auto emit_assign(const ast::AssignExpr &expr) -> llvm::Value * {
                auto lv = emit_lvalue(expr.target);
                auto *value = emit_value_as(expr.value, lv.type);

                if (expr.op != ast::AssignOp::Assign) {
                    auto *old = builder_.CreateLoad(lv.storage_type, lv.ptr);
                    const auto op = compound_op(expr.op);
                    if (is_pointer_like(lv.type) && (op == ast::BinaryOp::Add || op == ast::BinaryOp::Sub)) {
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

            auto sizeof_operand(const ast::SizeOfExpr &expr) -> uint64_t {
                if (const auto *ident = std::get_if<ast::IdentExpr>(&expr.operand)) {
                    if (const auto it = current_module_->symbols.find(ident->name); it != current_module_->symbols.end()) {
                        if (const auto *ts = std::get_if<sema::TypeSymbol>(&it->second); ts && ts->resolved) {
                            return size_of(*current_module_path_, *ts->resolved);
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
                return size_of(*current_module_path_, current_module_->expr_types.at(sema::get_expr_key(expr.operand)));
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
                            return llvm::ConstantInt::get(llvm_type(*current_module_path_, ty), v.value, ty.is_signed());
                        } else if constexpr (std::is_same_v<V, ast::LiteralFloatExpr>) {
                            return llvm::ConstantFP::get(llvm_type(*current_module_path_, ty), v.value);
                        } else if constexpr (std::is_same_v<V, ast::LiteralStringExpr>) {
                            return emit_string_literal(v.value);
                        } else if constexpr (std::is_same_v<V, ast::LiteralBoolExpr>) {
                            return builder_.getInt1(v.value);
                        } else if constexpr (std::is_same_v<V, ast::LiteralNilExpr>) {
                            return llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context_));
                        } else if constexpr (std::is_same_v<V, ast::IdentExpr>) {
                            if (const auto macro = macro_args_.find(v.name); macro != macro_args_.end()) {
                                return emit_const_macro_arg(macro->second);
                            }
                            const auto sym = current_module_->symbols.find(v.name);
                            if (sym != current_module_->symbols.end()) {
                                if (const auto *g = std::get_if<sema::GlobalSymbol>(&sym->second); g && !g->is_mut && g->decl->init) {
                                    return emit_const_or_runtime(*g->decl->init, true);
                                }
                            }
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::SizeOfExpr>>) {
                            return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), sizeof_operand(*v));
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::CastExpr>>) {
                            auto *value = llvm::cast<llvm::Constant>(emit_const_or_runtime(v->value, true));
                            const auto from = current_module_->expr_types.at(sema::get_expr_key(v->value));
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
                                        for (size_t i = 0; i < macro->decl->params.size(); ++i) {
                                            macro_args_[macro->decl->params[i].name] = MacroArg{.expr = &v->args[i], .module_path = current_module_path_, .module = current_module_};
                                        }
                                        const auto *saved_path = current_module_path_;
                                        const auto *saved_module = current_module_;
                                        current_module_path_ = &target_module;
                                        current_module_ = &target;
                                        auto *value = emit_const_or_runtime(macro->decl->expr_template, true);
                                        current_module_path_ = saved_path;
                                        current_module_ = saved_module;
                                        macro_args_ = std::move(saved_args);
                                        return value;
                                    }
                                }
                            }
                        }

                        report_codegen_error(diag_, {}, "unsupported global constant initializer");
                        return llvm::UndefValue::get(llvm_type(*current_module_path_, ty));
                    },
                    expr);
            }

            auto cast_opcode(const sema::ResolvedType &from, const sema::ResolvedType &to) const -> unsigned {
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

            auto const_binary(const ast::BinaryExpr &expr, llvm::Constant *lhs, llvm::Constant *rhs) -> llvm::Constant * {
                const auto lhs_type = current_module_->expr_types.at(sema::get_expr_key(expr.lhs));
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
                case ast::BinaryOp::BitwiseAnd: opcode = llvm::Instruction::And; break;
                case ast::BinaryOp::BitwiseOr:  opcode = llvm::Instruction::Or; break;
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
                            for (const auto &s : v->stmts) {
                                if (builder_.GetInsertBlock()->getTerminator()) break;
                                emit_stmt(s);
                            }
                            locals_ = saved;
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::IfStmt>>) {
                            emit_if(*v);
                        } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::WhileStmt>>) {
                            emit_while(*v);
                        } else if constexpr (std::is_same_v<V, ast::ExprStmt>) {
                            emit_expr(v.expr);
                        } else if constexpr (std::is_same_v<V, ast::VarDeclStmt>) {
                            const auto ty = v.type
                                                ? sema::resolve_type(*v.type, *current_module_path_, const_cast<sema::Program &>(sema_program_), diag_)
                                                : current_module_->expr_types.at(sema::get_expr_key(*v.init));
                            const auto type_module = v.type
                                                         ? type_module_for_ast_type(*v.type, *current_module_path_, ty)
                                                         : *current_module_path_;
                            auto *slot = create_entry_alloca(current_function_, llvm_type_for(ty, type_module), v.name);
                            locals_[v.name] = LocalValue{.alloca = slot, .type = ty, .type_module = type_module};
                            if (v.init) {
                                builder_.CreateStore(emit_value_as(*v.init, ty), slot);
                            } else {
                                builder_.CreateStore(zero_value(type_module, ty), slot);
                            }
                        } else if constexpr (std::is_same_v<V, ast::VarDeclGroupStmt>) {
                            auto *call = std::get_if<std::unique_ptr<ast::CallExpr>>(&v.init);
                            auto *result = emit_call(**call);
                            const auto [type_module, returns] = call_return_types(**call);
                            for (size_t i = 0; i < v.names.size(); ++i) {
                                if (v.names[i].empty()) {
                                    continue;
                                }
                                auto ty = returns[i];
                                auto *slot = create_entry_alloca(current_function_, llvm_type_for(ty, type_module), v.names[i]);
                                auto *value = returns.size() == 1 ? result : builder_.CreateExtractValue(result, {static_cast<unsigned>(i)});
                                builder_.CreateStore(value, slot);
                                locals_[v.names[i]] = LocalValue{.alloca = slot, .type = ty, .type_module = type_module};
                            }
                        } else if constexpr (std::is_same_v<V, ast::ContinueStmt>) {
                            builder_.CreateBr(continue_targets_.back());
                        } else if constexpr (std::is_same_v<V, ast::BreakStmt>) {
                            builder_.CreateBr(break_targets_.back());
                        } else if constexpr (std::is_same_v<V, ast::ReturnStmt>) {
                            emit_return(v);
                        }
                    },
                    stmt);
            }

            void emit_if(const ast::IfStmt &stmt) {
                auto *fn = builder_.GetInsertBlock()->getParent();
                auto *then_bb = llvm::BasicBlock::Create(*context_, "if.then", fn);
                auto *else_bb = stmt.else_stmt ? llvm::BasicBlock::Create(*context_, "if.else", fn) : nullptr;
                auto *end_bb = llvm::BasicBlock::Create(*context_, "if.end", fn);
                auto *cond = coerce_to_bool(emit_expr(stmt.condition), current_module_->expr_types.at(sema::get_expr_key(stmt.condition)));
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
                auto *cond = coerce_to_bool(emit_expr(stmt.condition), current_module_->expr_types.at(sema::get_expr_key(stmt.condition)));
                builder_.CreateCondBr(cond, body_bb, end_bb);

                builder_.SetInsertPoint(body_bb);
                continue_targets_.push_back(cond_bb);
                break_targets_.push_back(end_bb);
                emit_stmt(stmt.body);
                break_targets_.pop_back();
                continue_targets_.pop_back();
                if (!builder_.GetInsertBlock()->getTerminator()) builder_.CreateBr(cond_bb);

                builder_.SetInsertPoint(end_bb);
            }

            void emit_return(const ast::ReturnStmt &stmt) {
                if (stmt.return_values.empty()) {
                    builder_.CreateRetVoid();
                    return;
                }
                if (stmt.return_values.size() == 1) {
                    builder_.CreateRet(emit_value_as(stmt.return_values.front(), current_returns_.front()));
                    return;
                }

                auto *agg_ty = llvm::cast<llvm::StructType>(return_type(*current_module_path_, current_returns_));
                llvm::Value *agg = llvm::UndefValue::get(agg_ty);
                for (size_t i = 0; i < stmt.return_values.size(); ++i) {
                    agg = builder_.CreateInsertValue(agg, emit_value_as(stmt.return_values[i], current_returns_[i]), {static_cast<unsigned>(i)});
                }
                builder_.CreateRet(agg);
            }
        };
    }

    auto generate(const ast::Program &ast_program, const sema::Program &sema_program, DiagnosticEngine &diag, const Options &options) -> std::unique_ptr<llvm::Module> {
        return Generator(ast_program, sema_program, diag, options).run();
    }
}
