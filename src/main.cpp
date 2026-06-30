#include "compiler/codegen.hpp"
#include "compiler/module_resolver.hpp"
#include "compiler/sema.hpp"
#include "compiler/source_manager.hpp"

#include <llvm/IR/Module.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>

#include <cstdlib>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

namespace {
    struct Options {
        bool emit_ir;
        bool freestanding;
        std::string filename;
        std::string output = "start.out";
    };

    auto parse_options(const int argc, char *argv[]) -> Options {
        Options options{};

        for (int i = 1; i < argc; ++i) {
            if (const auto arg = std::string(argv[i]); arg == "--emit-ir") {
                options.emit_ir = true;
            } else if (arg == "--freestanding") {
                options.freestanding = true;
            } else if (arg == "-o" || arg == "--output") {
                if (i + 1 >= argc) {
                    options.filename.clear();
                    return options;
                }
                options.output = argv[++i];
            } else if (options.filename.empty()) {
                options.filename = arg;
            } else {
                break;
            }
        }

        return options;
    }

    auto shell_quote(const std::string &value) -> std::string {
        std::string out = "'";
        for (const char c : value) {
            if (c == '\'') {
                out += "'\\''";
            } else {
                out += c;
            }
        }
        out += "'";
        return out;
    }

    auto emit_object_file(llvm::Module &module, const std::filesystem::path &object_path) -> bool {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        llvm::InitializeNativeTargetAsmParser();

        const llvm::Triple target_triple(llvm::sys::getDefaultTargetTriple());
        module.setTargetTriple(target_triple);

        std::string error;
        const auto *target = llvm::TargetRegistry::lookupTarget(target_triple, error);
        if (!target) {
            llvm::errs() << "mirage: " << error << "\n";
            return false;
        }

        llvm::TargetOptions target_options;
        auto target_machine = std::unique_ptr<llvm::TargetMachine>(
            target->createTargetMachine(target_triple, "generic", "", target_options, std::nullopt));
        if (!target_machine) {
            llvm::errs() << "mirage: failed to create target machine\n";
            return false;
        }

        module.setDataLayout(target_machine->createDataLayout());

        std::error_code ec;
        llvm::raw_fd_ostream out(object_path.string(), ec, llvm::sys::fs::OF_None);
        if (ec) {
            llvm::errs() << "mirage: cannot open object file '" << object_path.string() << "': " << ec.message() << "\n";
            return false;
        }

        llvm::legacy::PassManager pass_manager;
        if (target_machine->addPassesToEmitFile(pass_manager, out, nullptr, llvm::CodeGenFileType::ObjectFile)) {
            llvm::errs() << "mirage: target cannot emit object files\n";
            return false;
        }

        pass_manager.run(module);
        out.flush();
        return true;
    }

    auto link_executable(const std::filesystem::path &object_path, const Options &options) -> bool {
        std::vector<std::string> args{"clang"};
        if (options.freestanding) {
            args.emplace_back("-ffreestanding");
            args.emplace_back("-nostdlib");
        } else {
            args.emplace_back("-nostartfiles");
        }

        args.push_back(object_path.string());
        args.emplace_back("-o");
        args.push_back(options.output);

        std::string command;
        for (size_t i = 0; i < args.size(); ++i) {
            if (i > 0) {
                command += " ";
            }
            command += shell_quote(args[i]);
        }

        return std::system(command.c_str()) == 0;
    }
}

auto main(const int argc, char *argv[]) -> int {
    if (argc < 2) {
        return 1;
    }

    const auto options = parse_options(argc, argv);
    if (options.filename.empty()) {
        return 1;
    }

    SourceManager source_manager;
    DiagnosticEngine diag(source_manager);

    const auto ast = ast::resolve(options.filename, source_manager, diag);
    if (!ast.ok) {
        return 1;
    }

    const auto sema = sema::check_program(ast, diag);
    if (!sema.ok) {
        return 1;
    }

    const auto llvm_module = codegen::generate(ast, sema, diag, {.freestanding = options.freestanding});
    if (!llvm_module || diag.has_errors()) {
        return 1;
    }

    if (options.emit_ir) {
        llvm_module->print(llvm::outs(), nullptr);
        return 0;
    }

    const auto object_path = std::filesystem::temp_directory_path() / std::format("mirage-{}.o", std::rand());
    if (!emit_object_file(*llvm_module, object_path)) {
        return 1;
    }

    if (!link_executable(object_path, options)) {
        std::error_code remove_error;
        std::filesystem::remove(object_path, remove_error);
        llvm::errs() << "mirage: linker failed\n";
        return 1;
    }

    std::error_code remove_error;
    std::filesystem::remove(object_path, remove_error);

    return 0;
}
