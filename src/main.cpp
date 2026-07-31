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

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <optional>
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
        bool noinit = false;
        bool print_link_directives = false;
        bool dump_ast = false;
        bool eager_generic_check = true;
        std::string module_path;
        std::string output = "a.out";
        std::string std_path;
        std::string cc; // linker driver; see resolve_cc()
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
                     << "  --std=<path>         Override the standard library path (takes precedence over MIRAGE_PATH)\n"
                     << "  --cc=<program>       Linker driver to invoke (default: clang, or $MIRAGE_CC)\n"
                     << "  --emit-ir            Print LLVM IR to stdout instead of compiling\n"
                     << "  --freestanding       Compile without standard library\n"
                     << "  --noinit             Skip generating/calling the synthesized '@init'-runner '_init'\n"
                     << "  --opt key=value      Set a compile-time '$option' value (may be repeated)\n"
                     << "  --print-link-directives  Print collected '#link' directives and exit\n"
                     << "  --dump-ast           Print the parsed AST shape and exit\n"
                     << "  --no-eager-generic-check  Only type-check a generic's body once it is instantiated\n"
                     << "  --help               Show this help message\n";
    }

    // Returns nullopt on malformed input, having already reported why. Previously every
    // one of these cases did a bare 'return options', so a mistake was silently accepted:
    // 'mirage build p -o' (a shell-quoting slip) wrote to the default a.out, and a stray
    // positional token stopped argument parsing entirely, so any flag after it was ignored.
    auto parse_options(const int argc, char *argv[]) -> std::optional<Options> {
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
            } else if (arg == "--noinit") {
                options.noinit = true;
            } else if (arg == "--print-link-directives") {
                options.print_link_directives = true;
            } else if (arg == "--dump-ast") {
                options.dump_ast = true;
            } else if (arg == "--no-eager-generic-check") {
                options.eager_generic_check = false;
            } else if (arg == "--opt") {
                if (i + 1 >= argc) {
                    llvm::errs() << "mirage: '--opt' requires an argument of the form 'key=value'\n";
                    return std::nullopt;
                }
                const std::string kv = argv[++i];
                const auto eq = kv.find('=');
                if (eq == std::string::npos) {
                    llvm::errs() << "mirage: --opt requires 'key=value'\n";
                    return std::nullopt;
                }
                options.opt_values[kv.substr(0, eq)] = kv.substr(eq + 1);
            } else if (arg == "-o" || arg == "--output") {
                if (i + 1 >= argc) {
                    llvm::errs() << "mirage: '" << arg << "' requires an output filename\n";
                    return std::nullopt;
                }
                options.output = argv[++i];
            } else if (arg.starts_with("--std=")) {
                options.std_path = arg.substr(6);
            } else if (arg.starts_with("--cc=")) {
                options.cc = arg.substr(5);
            } else if (arg == "-l") {
                if (i + 1 >= argc) {
                    llvm::errs() << "mirage: '-l' requires a library name\n";
                    return std::nullopt;
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
                    return std::nullopt;
                }
            } else if (options.module_path.empty()) {
                options.module_path = arg;
            } else {
                llvm::errs() << "mirage: unexpected argument '" << arg << "'\n";
                return std::nullopt;
            }
        }

        return options;
    }

    // Creates a uniquely-named file in the system temp directory and returns its path,
    // or an empty path on failure.
    //
    // mkstemp/mkstemps create the file atomically with O_EXCL and mode 0600, which is
    // what makes this safe in a shared /tmp: the name cannot be guessed and pre-created
    // as a symlink pointing somewhere else. The descriptor is closed immediately -- the
    // caller (LLVM's object writer, or clang via -o) reopens the path by name. The file
    // is left in place as a placeholder so the name stays reserved until then.
    auto make_temp_file(const std::string_view suffix) -> std::filesystem::path {
        std::error_code temp_dir_error;
        const auto temp_dir = std::filesystem::temp_directory_path(temp_dir_error);
        if (temp_dir_error) {
            llvm::errs() << "mirage: cannot locate a temporary directory: "
                         << temp_dir_error.message() << "\n";
            return {};
        }

        auto tmpl = (temp_dir / "mirage-XXXXXX").string();
        tmpl += suffix;

        const int fd = suffix.empty()
            ? mkstemp(tmpl.data())
            : mkstemps(tmpl.data(), static_cast<int>(suffix.size()));
        if (fd < 0) {
            llvm::errs() << "mirage: cannot create a temporary file in " << temp_dir.string()
                         << ": " << std::strerror(errno) << "\n";
            return {};
        }
        close(fd);
        return std::filesystem::path(tmpl);
    }

    // Creates the native-target TargetMachine. Called once BEFORE codegen (so the module
    // is built under the real DataLayout — implicit alignments and constant folds during
    // IR construction depend on it) and reused for object emission.
    auto create_native_target_machine() -> std::unique_ptr<llvm::TargetMachine> {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        llvm::InitializeNativeTargetAsmParser();

        const llvm::Triple target_triple(llvm::sys::getDefaultTargetTriple());
        std::string error;
        const auto *target = llvm::TargetRegistry::lookupTarget(target_triple, error);
        if (!target) {
            llvm::errs() << "mirage: " << error << "\n";
            return nullptr;
        }

        llvm::TargetOptions target_options;
        auto target_machine = std::unique_ptr<llvm::TargetMachine>(
            target->createTargetMachine(target_triple, "generic", "", target_options, std::nullopt));
        if (!target_machine) {
            llvm::errs() << "mirage: failed to create target machine\n";
        }
        return target_machine;
    }

    auto emit_object_file(llvm::Module &module, llvm::TargetMachine &target_machine_ref, const std::filesystem::path &object_path) -> bool {
        auto *target_machine = &target_machine_ref;

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

        // CLI-12 asks for pass_manager.run()'s discarded result to be checked. It is
        // deliberately not checked: legacy::PassManager::run returns whether any pass MODIFIED
        // the module, not whether emission succeeded, so treating false as failure would reject
        // valid output. The genuine failure signal for this step is the output stream's error
        // state -- a write that failed partway (a full disk, a revoked file) would otherwise
        // leave a truncated object for the linker to choke on with no explanation from here.
        // The real emission-setup failure is already caught by addPassesToEmitFile above.
        if (out.has_error()) {
            llvm::errs() << "mirage: failed writing object file '" << object_path.string()
                         << "': " << out.error().message() << "\n";
            out.clear_error();
            return false;
        }
        return true;
    }

    // Default 'build/target_os'/'build/target_arch' $option values derived from the host
    // triple, used only when the user didn't pass an explicit '--opt' override — matching
    // OperatingSystem/Architecture's variant names in the (separately-maintained) stdlib
    // Core/Compiler/Options module, so both name-based and value-based $option coercion work.
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

    // The linker driver, in precedence order: '--cc=', then $MIRAGE_CC, then clang. Mirrors
    // how '--std=' overrides MIRAGE_PATH, which previously had no equivalent here -- the
    // driver was hardcoded, so a cross-compile or a non-default toolchain had no way in.
    auto resolve_cc(const Options &options) -> std::string {
        if (!options.cc.empty()) {
            return options.cc;
        }
        if (const char *env_value = std::getenv("MIRAGE_CC"); env_value != nullptr && *env_value != '\0') {
            return env_value;
        }
        return "clang";
    }

    auto link_executable(const std::filesystem::path &object_path, const std::filesystem::path &output_path,
                          const Options &options, const std::vector<sema::LinkDirective> &link_directives) -> bool {
        std::vector<std::string> args{resolve_cc(options)};
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

        // '#link' directives collected from the compiled program (module scope, or a live
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

        // fork + execvp with an argv array, not std::system with a quoted string. The 'run'
        // action two functions below already does exactly this; going through a shell here
        // meant every argument's correctness depended on shell_quote, for no benefit --
        // nothing in this command line needs shell expansion.
        //
        // On failure the driver's own exit status (or the signal that killed it) is reported,
        // rather than a bare "linker failed" that says nothing about what went wrong (CLI-7).
        std::vector<char *> argv;
        argv.reserve(args.size() + 1);
        for (auto &arg : args) {
            argv.push_back(arg.data());
        }
        argv.push_back(nullptr);

        const pid_t pid = fork();
        if (pid < 0) {
            llvm::errs() << "mirage: fork failed: " << std::strerror(errno) << "\n";
            return false;
        }
        if (pid == 0) {
            execvp(argv[0], argv.data());
            // Only reached if exec failed; the parent sees this as exit status 127.
            llvm::errs() << "mirage: cannot execute '" << args[0] << "': " << std::strerror(errno) << "\n";
            _exit(127);
        }

        int status = 0;
        if (waitpid(pid, &status, 0) < 0) {
            llvm::errs() << "mirage: waitpid failed: " << std::strerror(errno) << "\n";
            return false;
        }
        if (WIFSIGNALED(status)) {
            llvm::errs() << "mirage: linker '" << args[0] << "' was killed by signal "
                         << WTERMSIG(status) << " (" << strsignal(WTERMSIG(status)) << ")\n";
            return false;
        }
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            llvm::errs() << "mirage: linker '" << args[0] << "' exited with status "
                         << WEXITSTATUS(status) << "\n";
            return false;
        }
        return true;
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
        case ast::BinaryOp::In: return "in";
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
                    out << "$option(\"" << v->key << "\")";
                } else if constexpr (std::is_same_v<V, std::unique_ptr<ast::EnvExpr>>) {
                    out << "$env(\"" << v->key << "\")";
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
                    out << pad << "#link(...)\n";
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
                    out << "#link(...)\n";
                } else if constexpr (std::is_same_v<V, ast::DiagnosticDecl>) {
                    out << (v.kind == ast::DiagnosticDirectiveKind::Error ? "#error(...)\n" : "#warn(...)\n");
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
    // Parse first, then validate. The old 'argc < 3' gate ran BEFORE parse_options, so
    // 'mirage --help' (argc == 2) never reached the --help branch that exits 0: it printed
    // the same usage text but exited 1, which any tooling checking the exit status reads as
    // a failure.
    auto parsed = parse_options(argc, argv);
    if (!parsed) {
        return 1; // parse_options already reported what was wrong
    }

    auto options = *parsed;
    if (options.action == Action::None || options.module_path.empty()) {
        print_usage(argv[0]);
        return 1;
    }

    // Host-platform '$option' defaults ('build/target_os'/'build/target_arch'), used only
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
    const auto ast = ast::resolve(options.module_path, source_manager, diag, options.std_path);
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
    const auto sema = sema::check_program(ast, diag, sema::Options{
        .opt_values = options.opt_values,
        .eager_generic_check = options.eager_generic_check,
    });
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
    const auto target_machine = create_native_target_machine();
    if (!target_machine) {
        return 1;
    }
    const auto llvm_module = codegen::generate(ast, sema, diag, {
        .freestanding = options.freestanding,
        .noinit = options.noinit,
        .target_triple = target_machine->getTargetTriple().str(),
        .data_layout = target_machine->createDataLayout().getStringRepresentation(),
    });
    if (!llvm_module || diag.has_errors()) {
        return 1;
    }
    const auto codegen_elapsed = std::chrono::steady_clock::now() - codegen_start;

    const auto to_ms = [](auto elapsed) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    };
    // Progress and timing reporting goes to stderr, not stdout. stdout carries only
    // what was actually asked for -- the IR under --emit-ir, the AST under --dump-ast,
    // the directive list under --print-link-directives, and under 'run' the compiled
    // program's own output. Otherwise 'mirage build p --emit-ir > out.ll' prepends this
    // banner to the module and produces a file that is not valid LLVM IR.
    llvm::errs() << std::format(
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

    const auto object_path = make_temp_file(".o");
    if (object_path.empty()) {
        return 1;
    }
    const auto object_start = std::chrono::steady_clock::now();
    if (!emit_object_file(*llvm_module, *target_machine, object_path)) {
        // Clean up the partial/empty temp object, as the link-failure and success paths below
        // both do. mkstemp already created the file, so returning here always leaked one.
        std::error_code object_error;
        std::filesystem::remove(object_path, object_error);
        return 1;
    }
    const auto object_elapsed = std::chrono::steady_clock::now() - object_start;

    const auto exe_path = options.action == Action::Run
        ? make_temp_file("")
        : std::filesystem::path(options.output);
    if (exe_path.empty()) {
        std::error_code exe_temp_error;
        std::filesystem::remove(object_path, exe_temp_error);
        return 1;
    }

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

    llvm::errs() << std::format(
        "  object:  {}ms\n"
        "  link:    {}ms\n",
        to_ms(object_elapsed), to_ms(link_elapsed));

    const auto elapsed = std::chrono::steady_clock::now() - start_time;
    const auto secs = std::chrono::duration<double>(elapsed).count();
    if (options.action == Action::Run) {
        llvm::errs() << std::format("Compiled '{}' in {:.2f}s\n", options.module_path, secs);
    } else {
        llvm::errs() << std::format("Compiled '{}' -> '{}' in {:.2f}s\n", options.module_path, options.output, secs);
    }
    // Flush before fork() so nothing still buffered here is inherited by the child and
    // written out a second time from the child's copy of the buffer.
    llvm::outs().flush();
    llvm::errs().flush();

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
            llvm::errs() << std::format("process exited with code {}\n", code);
            return code;
        }
        if (WIFSIGNALED(status)) {
            // Previously this fell through to a bare 'return 1', so a compiled program that
            // segfaulted looked like an ordinary non-zero exit -- which is exactly how
            // CODEGEN-1's stack-exhaustion crash stayed invisible through 'mirage run'.
            const int sig = WTERMSIG(status);
            llvm::errs() << std::format("process was killed by signal {} ({})\n", sig, strsignal(sig));
            return 128 + sig;
        }
        return 1;
    }

    return 0;
}
