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

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {
    enum class Action { None, Build, Run };

    struct Options {
        Action action = Action::None;
        bool emit_ir = false;
        bool freestanding = false;
        std::string module_path;
        std::string output = "a.out";
        std::vector<std::string> libs;
    };

    auto print_usage(const char *argv0) -> void {
        llvm::errs() << "Usage: " << argv0 << " <action> <module> [options]\n"
                     << "\n"
                     << "Actions:\n"
                     << "  build   Compile a module to an executable\n"
                     << "  run     Compile and run a module\n"
                     << "\n"
                     << "Options:\n"
                     << "  -o, --output <file>  Output file name (default: a.out)\n"
                     << "  -l <lib>             Link with additional library (may be repeated)\n"
                     << "  --emit-ir            Print LLVM IR to stdout instead of compiling\n"
                     << "  --freestanding       Compile without standard library\n"
                     << "  --help               Show this help message\n";
    }

    auto parse_options(const int argc, char *argv[]) -> Options {
        Options options{};

        for (int i = 1; i < argc; ++i) {
            const auto arg = std::string(argv[i]);
            if (arg == "--help" || arg == "-h") {
                print_usage(argv[0]);
                std::exit(0);
            } else if (arg == "--emit-ir") {
                options.emit_ir = true;
            } else if (arg == "--freestanding") {
                options.freestanding = true;
            } else if (arg == "-o" || arg == "--output") {
                if (i + 1 >= argc) {
                    return options;
                }
                options.output = argv[++i];
            } else if (arg == "-l") {
                if (i + 1 >= argc) {
                    return options;
                }
                options.libs.push_back(argv[++i]);
            } else if (arg.starts_with("-l") && arg.size() > 2) {
                options.libs.push_back(arg.substr(2));
            } else if (options.action == Action::None) {
                if (arg == "build") {
                    options.action = Action::Build;
                } else if (arg == "run") {
                    options.action = Action::Run;
                } else {
                    llvm::errs() << "mirage: unknown action '" << arg << "'; expected 'build' or 'run'\n";
                    return options;
                }
            } else if (options.module_path.empty()) {
                options.module_path = arg;
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

    auto link_executable(const std::filesystem::path &object_path, const std::filesystem::path &output_path,
                          const Options &options) -> bool {
        std::vector<std::string> args{"clang"};
        if (options.freestanding) {
            args.emplace_back("-ffreestanding");
            args.emplace_back("-nostdlib");
        } else {
            args.emplace_back("-nostartfiles");
        }
        args.emplace_back("-no-pie");

        args.push_back(object_path.string());
        for (const auto &lib : options.libs) {
            args.push_back("-l" + lib);
        }
        args.emplace_back("-o");
        args.push_back(output_path.string());

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
    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }

    const auto options = parse_options(argc, argv);
    if (options.action == Action::None || options.module_path.empty()) {
        print_usage(argv[0]);
        return 1;
    }

    const auto start_time = std::chrono::steady_clock::now();

    SourceManager source_manager;
    DiagnosticEngine diag(source_manager);

    const auto parse_start = std::chrono::steady_clock::now();
    const auto ast = ast::resolve(options.module_path, source_manager, diag);
    if (!ast.ok) {
        return 1;
    }
    const auto parse_elapsed = std::chrono::steady_clock::now() - parse_start;

    const auto sema_start = std::chrono::steady_clock::now();
    const auto sema = sema::check_program(ast, diag);
    if (!sema.ok) {
        return 1;
    }
    const auto sema_elapsed = std::chrono::steady_clock::now() - sema_start;

    const auto codegen_start = std::chrono::steady_clock::now();
    const auto llvm_module = codegen::generate(ast, sema, diag, {.freestanding = options.freestanding});
    if (!llvm_module || diag.has_errors()) {
        return 1;
    }
    const auto codegen_elapsed = std::chrono::steady_clock::now() - codegen_start;

    const auto to_ms = [](auto elapsed) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    };
    llvm::outs() << std::format(
        "Processed {} file(s), {} token(s)\n"
        "  parsing: {}ms\n"
        "  sema:    {}ms\n"
        "  codegen: {}ms\n",
        ast.file_count, ast.token_count,
        to_ms(parse_elapsed), to_ms(sema_elapsed), to_ms(codegen_elapsed));

    if (options.emit_ir) {
        llvm_module->print(llvm::outs(), nullptr);
        return 0;
    }

    const auto object_path = std::filesystem::temp_directory_path() / std::format("mirage-{}.o", std::rand());
    if (!emit_object_file(*llvm_module, object_path)) {
        return 1;
    }

    const auto exe_path = options.action == Action::Run
        ? std::filesystem::temp_directory_path() / std::format("mirage-{}", std::rand())
        : std::filesystem::path(options.output);

    if (!link_executable(object_path, exe_path, options)) {
        std::error_code remove_error;
        std::filesystem::remove(object_path, remove_error);
        llvm::errs() << "mirage: linker failed\n";
        return 1;
    }

    std::error_code remove_error;
    std::filesystem::remove(object_path, remove_error);

    const auto elapsed = std::chrono::steady_clock::now() - start_time;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    if (options.action == Action::Run) {
        llvm::outs() << std::format("Compiled '{}' in {}ms\n", options.module_path, ms);
    } else {
        llvm::outs() << std::format("Compiled '{}' -> '{}' in {}ms\n", options.module_path, options.output, ms);
    }
    llvm::outs().flush();

    if (options.action == Action::Run) {
        const pid_t pid = fork();
        if (pid < 0) {
            llvm::errs() << "mirage: fork failed\n";
            std::filesystem::remove(exe_path, remove_error);
            return 1;
        }
        if (pid == 0) {
            const char *args[] = {exe_path.c_str(), nullptr};
            execv(exe_path.c_str(), const_cast<char *const *>(args));
            _exit(127);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        std::filesystem::remove(exe_path, remove_error);
        if (WIFEXITED(status)) {
            const int code = WEXITSTATUS(status);
            llvm::outs() << std::format("process exited with code {}\n", code);
            return code;
        }
        return 1;
    }

    return 0;
}
