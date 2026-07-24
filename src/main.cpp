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
#include <unordered_map>
#include <vector>

namespace {
    enum class Action { None, Build, Run };

    struct Options {
        Action action = Action::None;
        bool emit_ir = false;
        bool freestanding = false;
        bool print_link_directives = false;
        bool dump_ast = false;
        std::string module_path;
        std::string output = "a.out";
        std::vector<std::string> libs;
        std::unordered_map<std::string, std::string> opt_values;
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
                     << "  --opt key=value      Set a compile-time '@option' value (may be repeated)\n"
                     << "  --print-link-directives  Print collected '@link' directives and exit\n"
                     << "  --dump-ast           Print the parsed AST shape and exit\n"
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
            } else if (arg == "--print-link-directives") {
                options.print_link_directives = true;
            } else if (arg == "--dump-ast") {
                options.dump_ast = true;
            } else if (arg == "--opt") {
                if (i + 1 >= argc) {
                    return options;
                }
                const std::string kv = argv[++i];
                const auto eq = kv.find('=');
                if (eq == std::string::npos) {
                    llvm::errs() << "mirage: --opt requires 'key=value'\n";
                    return options;
                }
                options.opt_values[kv.substr(0, eq)] = kv.substr(eq + 1);
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

    // Default 'build/target_os'/'build/target_arch' @option values derived from the host
    // triple, used only when the user didn't pass an explicit '--opt' override — matching
    // OperatingSystem/Architecture's variant names in the (separately-maintained) stdlib
    // Core/Compiler/Options module, so both name-based and value-based @option coercion work.
    auto default_target_os(const llvm::Triple &triple) -> std::string {
        if (triple.isOSLinux()) return "Linux";
        if (triple.isOSWindows()) return "Windows";
        if (triple.isMacOSX()) return "MacOS";
        if (triple.isWasm()) return "Wasm32";
        return "Other";
    }

    auto default_target_arch(const llvm::Triple &triple) -> std::string {
        switch (triple.getArch()) {
        case llvm::Triple::x86:      return "X86";
        case llvm::Triple::x86_64:   return "X86_64";
        case llvm::Triple::aarch64:  return "Arm64";
        case llvm::Triple::wasm32:   return "Wasm32";
        case llvm::Triple::wasm64:   return "Wasm64p32";
        default:                     return "Other";
        }
    }

    auto link_executable(const std::filesystem::path &object_path, const std::filesystem::path &output_path,
                          const Options &options, const std::vector<sema::LinkDirective> &link_directives) -> bool {
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

        // '@link' directives collected from the compiled program (module scope, or a live
        // 'when' branch). 'lib' paths are resolved relative to the directory of the module
        // file that declared them (source_module is a canonicalized module *directory*
        // path, matching import resolution) — not the current working directory.
        for (const auto &link : link_directives) {
            switch (link.category) {
            case sema::LinkCategory::Lib:
                args.push_back((std::filesystem::path(link.source_module) / link.data).string());
                break;
            case sema::LinkCategory::System:
                args.push_back("-l" + link.data);
                break;
            case sema::LinkCategory::Flag:
                args.push_back(link.data);
                break;
            }
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

    // Minimal recursive AST dumper for '--dump-ast', a debug-only affordance added so the
    // 'when' expression's precedence composition (e.g. 'a + b when c else d' parsing as
    // '(a + b) when (c) else (d)') can actually be inspected, rather than only inferred
    // from behavior. Not a general pretty-printer — covers the expression/statement/
    // declaration shapes relevant to this feature set plus the common existing ones;
    // anything else prints its bare kind name via the fallback branch.
    auto binary_op_name(const ast::BinaryOp op) -> const char * {
        switch (op) {
        case ast::BinaryOp::Add: return "+"; case ast::BinaryOp::Sub: return "-";
        case ast::BinaryOp::Mul: return "*"; case ast::BinaryOp::Div: return "/";
        case ast::BinaryOp::Mod: return "%"; case ast::BinaryOp::BitwiseAnd: return "&";
        case ast::BinaryOp::BitwiseOr: return "|"; case ast::BinaryOp::BitwiseXor: return "^";
        case ast::BinaryOp::ShiftLeft: return "<<"; case ast::BinaryOp::ShiftRight: return ">>";
        case ast::BinaryOp::Equal: return "=="; case ast::BinaryOp::NotEqual: return "!=";
        case ast::BinaryOp::Less: return "<"; case ast::BinaryOp::Greater: return ">";
        case ast::BinaryOp::LessEqual: return "<="; case ast::BinaryOp::GreaterEqual: return ">=";
        case ast::BinaryOp::LogicalAnd: return "&&"; case ast::BinaryOp::LogicalOr: return "||";
        }
        return "?";
    }

    void dump_expr(const ast::Expr &expr, llvm::raw_ostream &out) {
        std::visit(
            [&]<typename T>(const T &v) {
                using V = std::decay_t<T>;
                if constexpr (std::is_same_v<V, ast::LiteralIntegerExpr>) {
                    out << v.value;
                } else if constexpr (std::is_same_v<V, ast::LiteralFloatExpr>) {
                    out << v.value;
                } else if constexpr (std::is_same_v<V, ast::LiteralStringExpr>) {
                    out << "\"" << v.value << "\"";
                } else if constexpr (std::is_same_v<V, ast::LiteralBoolExpr>) {
                    out << (v.value ? "true" : "false");
                } else if constexpr (std::is_same_v<V, ast::LiteralNilExpr>) {
                    out << "nil";
                } else if constexpr (std::is_same_v<V, ast::IdentExpr>) {
                    out << v.name;
                } else if constexpr (std::is_same_v<V, ast::DotIdentExpr>) {
                    out << "." << v.name;
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::UnaryExpr>>) {
                    out << "(unary "; dump_expr(v->operand, out); out << ")";
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::BinaryExpr>>) {
                    out << "("; dump_expr(v->lhs, out); out << " " << binary_op_name(v->op) << " "; dump_expr(v->rhs, out); out << ")";
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::TernaryExpr>>) {
                    out << "("; dump_expr(v->condition, out); out << " ? "; dump_expr(v->then_expr, out); out << " : "; dump_expr(v->else_expr, out); out << ")";
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::WhenExpr>>) {
                    out << "("; dump_expr(v->then_expr, out); out << " when ("; dump_expr(v->condition, out); out << ") else "; dump_expr(v->else_expr, out); out << ")";
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::AssignExpr>>) {
                    out << "("; dump_expr(v->target, out); out << " = "; dump_expr(v->value, out); out << ")";
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::CallExpr>>) {
                    dump_expr(v->callee, out); out << "(";
                    for (size_t i = 0; i < v->args.size(); ++i) { if (i) out << ", "; dump_expr(v->args[i], out); }
                    out << ")";
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::MemberExpr>>) {
                    dump_expr(v->object, out); out << "." << v->member;
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::OptionExpr>>) {
                    out << "@option(\"" << v->key << "\")";
                } else if constexpr (std::is_same_v<V, ast::ImportExpr>) {
                    out << "import(\"" << v.module_name << "\")";
                } else {
                    out << "<expr>";
                }
            },
            expr);
    }

    void dump_stmt(const ast::Stmt &stmt, llvm::raw_ostream &out, int indent) {
        const std::string pad(static_cast<size_t>(indent) * 2, ' ');
        std::visit(
            [&]<typename T>(const T &v) {
                using V = std::decay_t<T>;
                if constexpr (std::is_same_v<V, std::unique_ptr<ast::BlockStmt>>) {
                    out << pad << "{\n";
                    for (auto &s : v->stmts) dump_stmt(s, out, indent + 1);
                    out << pad << "}\n";
                } else if constexpr (std::is_same_v<V, ast::ExprStmt>) {
                    out << pad; dump_expr(v.expr, out); out << "\n";
                } else if constexpr (std::is_same_v<V, ast::VarDeclStmt>) {
                    out << pad << (v.is_mut ? "mut " : "const ") << v.name;
                    if (v.init) { out << " := "; dump_expr(*v.init, out); }
                    out << "\n";
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::WhenStmt>>) {
                    out << pad << "when "; dump_expr(v->condition, out); out << " {\n";
                    for (auto &s : v->then_block.stmts) dump_stmt(s, out, indent + 1);
                    out << pad << "}\n";
                } else if constexpr (std::is_same_v<V, ast::LinkDecl>) {
                    out << pad << "@link(...)\n";
                } else {
                    out << pad << "<stmt>\n";
                }
            },
            stmt);
    }

    void dump_decl(const ast::Decl &decl, llvm::raw_ostream &out) {
        std::visit(
            [&]<typename T>(const T &v) {
                using V = std::decay_t<T>;
                if constexpr (std::is_same_v<V, ast::FunctionDecl>) {
                    out << "fn " << v.name << "(...) {\n";
                    dump_stmt(v.body, out, 1);
                    out << "}\n";
                } else if constexpr (std::is_same_v<V, ast::VarDecl>) {
                    out << (v.is_mut ? "mut " : "const ") << v.name;
                    if (v.init) { out << " := "; dump_expr(*v.init, out); }
                    out << "\n";
                } else if constexpr (std::is_same_v<V, ast::LinkDecl>) {
                    out << "@link(...)\n";
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::WhenDecl>>) {
                    out << "when "; dump_expr(v->condition, out); out << " {\n";
                    for (auto &d : v->then_decls) dump_decl(d, out);
                    out << "}\n";
                } else {
                    out << "<decl>\n";
                }
            },
            decl);
    }
}

auto main(const int argc, char *argv[]) -> int {
    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }

    auto options = parse_options(argc, argv);
    if (options.action == Action::None || options.module_path.empty()) {
        print_usage(argv[0]);
        return 1;
    }

    // Host-platform '@option' defaults ('build/target_os'/'build/target_arch'), used only
    // where the user didn't already pass an explicit '--opt' override.
    {
        const llvm::Triple host_triple(llvm::sys::getDefaultTargetTriple());
        options.opt_values.try_emplace("build/target_os", default_target_os(host_triple));
        options.opt_values.try_emplace("build/target_arch", default_target_arch(host_triple));
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

    if (options.dump_ast) {
        if (const auto root_it = ast.modules.find(ast.root_module_path); root_it != ast.modules.end()) {
            for (const auto &decl : root_it->second) {
                dump_decl(decl, llvm::outs());
            }
        }
        return 0;
    }

    const auto sema_start = std::chrono::steady_clock::now();
    const auto sema = sema::check_program(ast, diag, sema::Options{.opt_values = options.opt_values});
    if (!sema.ok) {
        return 1;
    }
    const auto sema_elapsed = std::chrono::steady_clock::now() - sema_start;

    if (options.print_link_directives) {
        for (const auto &link : sema.link_directives) {
            const char *category = link.category == sema::LinkCategory::Lib ? "lib"
                                  : link.category == sema::LinkCategory::System ? "system" : "flag";
            llvm::outs() << category << " " << link.data << "  (from " << link.source_module << ")\n";
        }
        return 0;
    }

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
    const auto object_start = std::chrono::steady_clock::now();
    if (!emit_object_file(*llvm_module, object_path)) {
        return 1;
    }
    const auto object_elapsed = std::chrono::steady_clock::now() - object_start;

    const auto exe_path = options.action == Action::Run
        ? std::filesystem::temp_directory_path() / std::format("mirage-{}", std::rand())
        : std::filesystem::path(options.output);

    const auto link_start = std::chrono::steady_clock::now();
    if (!link_executable(object_path, exe_path, options, sema.link_directives)) {
        std::error_code remove_error;
        std::filesystem::remove(object_path, remove_error);
        llvm::errs() << "mirage: linker failed\n";
        return 1;
    }
    const auto link_elapsed = std::chrono::steady_clock::now() - link_start;

    std::error_code remove_error;
    std::filesystem::remove(object_path, remove_error);

    llvm::outs() << std::format(
        "  object:  {}ms\n"
        "  link:    {}ms\n",
        to_ms(object_elapsed), to_ms(link_elapsed));

    const auto elapsed = std::chrono::steady_clock::now() - start_time;
    const auto secs = std::chrono::duration<double>(elapsed).count();
    if (options.action == Action::Run) {
        llvm::outs() << std::format("Compiled '{}' in {:.2f}s\n", options.module_path, secs);
    } else {
        llvm::outs() << std::format("Compiled '{}' -> '{}' in {:.2f}s\n", options.module_path, options.output, secs);
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
