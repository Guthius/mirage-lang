#include "compiler/backend_wasm.hpp"
#include "compiler/backend_x86.hpp"
#include "compiler/elf_writer.hpp"
#include "compiler/mirgen.hpp"
#include "compiler/module_resolver.hpp"
#include "compiler/sema.hpp"
#include "compiler/source_manager.hpp"


#include <cerrno>
#include <fstream>
#include <iostream>
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
    enum class Action { None, Build, Run, Test };

    // 'core/testing' is a reserved module path: under 'mirage test' the compiler assumes it
    // resolves and exposes the fixed contract in spec.md's Testing section. Same
    // "known contract, not built into the compiler" posture as 'runtime/type_info' — the
    // module is ordinary Mirage source, the compiler just knows the names.
    constexpr const char *TESTING_MODULE_PATH = "core/testing";

    struct Options {
        Action action = Action::None;
        // '--emit-mir': lower to Mirage IR and print it. The native backend's primary
        // '--mir-opt': additionally run the stage-3 passes (promote_slots) before
        // printing — the debugging view of what the later stages actually consume.
        bool mir_opt = false;
        // '--regalloc=linear|trivial' (native backend). Trivial is the standing
        // triage tool (docs/backend.md stage 6): if a bug reproduces under it,
        // the bug is not in the linear-scan allocator.
        std::string regalloc = "linear";
        // debugging surface until it can produce objects (docs/backend.md, stage 2).
        bool emit_mir = false;
        bool freestanding = false;
        bool noinit = false;
        bool nortti = false;
        bool print_link_directives = false;
        bool print_module_search = false;
        bool dump_ast = false;
        bool eager_generic_check = true;
        std::string module_path;
        std::string output = "a.out";
        // Whether '-o' was given explicitly, as opposed to defaulted. Only 'test' cares:
        // it has no output to name, and silently ignoring the flag looked like it worked.
        bool output_explicit = false;
        std::string std_path;
        std::string cc; // linker driver; see resolve_cc()
        std::string target; // '--target=' triple; empty means the host triple
        std::vector<std::string> libs;
        // '--load <path>', repeatable: modules to compile even though nothing imports
        // them. Under 'test', 'core/testing' is appended to this list automatically.
        std::vector<std::string> forced_modules;
        std::unordered_map<std::string, std::string> opt_values;
    };

    auto print_usage(const char *argv0) -> void {
        std::cerr << "Usage: " << argv0 << " <action> <module> [options]\n"
                     << "\n"
                     << "Actions:\n"
                     << "  build   Compile a module to an executable\n"
                     << "  run     Compile and run a module\n"
                     << "  test    Compile and run the module's '@test' functions\n"
                     << "\n"
                     << "Options:\n"
                     << "  -o, --output <file>  Output file name (default: a.out)\n"
                     << "  -l <lib>             Link with additional library (may be repeated)\n"
                     << "  --std=<path>         Override the module root (takes precedence over MIRAGE_MODULES_ROOT)\n"
                     << "  --cc=<program>       Linker driver to invoke (default: clang, or $MIRAGE_CC)\n"
                     << "  --target=<triple>    Cross-compile for <triple> (default: the host triple)\n"
                     << "  --emit-mir           Print Mirage IR to stdout instead of compiling\n"
                     << "  --mir-opt            With --emit-mir: run the MIR passes before printing\n"
                     << "  --regalloc=<name>    Register allocator: 'linear' (default) or 'trivial' (triage)\n"
                     << "  --freestanding       Compile without standard library\n"
                     << "  --noinit             Skip generating/calling the synthesized '@init'-runner '_init'\n"
                     << "  --nortti             Disable runtime type information ('type_info_of'); sets '$rtti_enabled' to false\n"
                     << "  --load <path>        Compile a module nothing imports (may be repeated)\n"
                     << "  --opt key=value      Set a compile-time '$option' value (may be repeated)\n"
                     << "  --print-link-directives  Print collected '#link' directives and exit\n"
                     << "  --print-module-search    Print how each import was resolved and exit\n"
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
                std::cerr << "error: '--emit-ir' was removed with the LLVM backend; "
                              "'--emit-mir' prints the native IR\n";
                return std::nullopt;
            } else if (arg == "--emit-mir") {
                options.emit_mir = true;
            } else if (arg == "--mir-opt") {
                options.mir_opt = true;
            } else if (arg.starts_with("--backend=")) {
                // Tolerated as a no-op for 'native' so scripts from the soak
                // period keep working; 'llvm' is gone.
                if (arg != "--backend=native") {
                    std::cerr << "error: the LLVM backend was removed; the native "
                                  "backend is the only code generator\n";
                    return std::nullopt;
                }
            } else if (arg.starts_with("--regalloc=")) {
                options.regalloc = arg.substr(std::string("--regalloc=").size());
                if (options.regalloc != "linear" && options.regalloc != "trivial") {
                    std::cerr << "error: unknown register allocator '" << options.regalloc
                                 << "' (expected 'linear' or 'trivial')\n";
                    return std::nullopt;
                }
            } else if (arg == "--freestanding") {
                options.freestanding = true;
            } else if (arg == "--noinit") {
                options.noinit = true;
            } else if (arg == "--nortti") {
                options.nortti = true;
            } else if (arg == "--print-link-directives") {
                options.print_link_directives = true;
            } else if (arg == "--print-module-search") {
                options.print_module_search = true;
            } else if (arg == "--dump-ast") {
                options.dump_ast = true;
            } else if (arg == "--no-eager-generic-check") {
                options.eager_generic_check = false;
            } else if (arg == "--opt") {
                if (i + 1 >= argc) {
                    std::cerr << "mirage: '--opt' requires an argument of the form 'key=value'\n";
                    return std::nullopt;
                }
                const std::string kv = argv[++i];
                const auto eq = kv.find('=');
                if (eq == std::string::npos) {
                    std::cerr << "mirage: --opt requires 'key=value'\n";
                    return std::nullopt;
                }
                options.opt_values[kv.substr(0, eq)] = kv.substr(eq + 1);
            } else if (arg == "--load") {
                if (i + 1 >= argc) {
                    std::cerr << "mirage: '--load' requires a module path\n";
                    return std::nullopt;
                }
                options.forced_modules.emplace_back(argv[++i]);
            } else if (arg == "-o" || arg == "--output") {
                if (i + 1 >= argc) {
                    std::cerr << "mirage: '" << arg << "' requires an output filename\n";
                    return std::nullopt;
                }
                options.output = argv[++i];
                options.output_explicit = true;
            } else if (arg.starts_with("--std=")) {
                options.std_path = arg.substr(6);
            } else if (arg.starts_with("--cc=")) {
                options.cc = arg.substr(5);
            } else if (arg.starts_with("--target=")) {
                options.target = arg.substr(9);
            } else if (arg == "-l") {
                if (i + 1 >= argc) {
                    std::cerr << "mirage: '-l' requires a library name\n";
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
                } else if (arg == "test") {
                    options.action = Action::Test;
                } else {
                    std::cerr << "mirage: unknown action '" << arg << "'; expected 'build', 'run' or 'test'\n";
                    return std::nullopt;
                }
            } else if (options.module_path.empty()) {
                options.module_path = arg;
            } else {
                std::cerr << "mirage: unexpected argument '" << arg << "'\n";
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
            std::cerr << "mirage: cannot locate a temporary directory: "
                         << temp_dir_error.message() << "\n";
            return {};
        }

        auto tmpl = (temp_dir / "mirage-XXXXXX").string();
        tmpl += suffix;

        const int fd = suffix.empty()
            ? mkstemp(tmpl.data())
            : mkstemps(tmpl.data(), static_cast<int>(suffix.size()));
        if (fd < 0) {
            std::cerr << "mirage: cannot create a temporary file in " << temp_dir.string()
                         << ": " << std::strerror(errno) << "\n";
            return {};
        }
        close(fd);
        return std::filesystem::path(tmpl);
    }

    // The compiler's own target model (stage 10: target selection no longer
    // goes through llvm::Triple — LLVM is gone). Exactly the targets the native
    // backend owns; anything else is refused by the parser below with the full
    // list, where the LLVM build would have attempted a cross-compile.
    enum class TargetKind : uint8_t { X86_64_Linux, Wasm32_Unknown, Wasm32_Wasi, Wasm32_Emscripten };
    struct Target {
        TargetKind kind = TargetKind::X86_64_Linux;
        [[nodiscard]] auto is_wasm() const -> bool { return kind != TargetKind::X86_64_Linux; }
        [[nodiscard]] auto is_emscripten() const -> bool { return kind == TargetKind::Wasm32_Emscripten; }
        [[nodiscard]] auto pointer_bits() const -> uint32_t { return is_wasm() ? 32u : 64u; }
        // The canonical spelling, for diagnostics.
        [[nodiscard]] auto name() const -> const char * {
            switch (kind) {
            case TargetKind::X86_64_Linux:      return "x86_64-unknown-linux-gnu";
            case TargetKind::Wasm32_Unknown:    return "wasm32-unknown-unknown";
            case TargetKind::Wasm32_Wasi:       return "wasm32-wasi";
            case TargetKind::Wasm32_Emscripten: return "wasm32-unknown-emscripten";
            }
            return "?";
        }
    };

    // Accepts the '--target=' spellings that worked before the LLVM removal
    // (llvm::Triple::normalize was forgiving about vendor/OS fields), plus the
    // bare shorthands people actually type. Empty means the host, which this
    // compiler only exists on as x86_64-linux today.
    auto parse_target(const std::string &spelling) -> std::optional<Target> {
        if (spelling.empty()) return Target{TargetKind::X86_64_Linux};
        const auto is = [&](const char *candidate) { return spelling == candidate; };
        if (is("x86_64-linux") || is("x86_64-unknown-linux") || is("x86_64-unknown-linux-gnu") ||
            is("x86_64-pc-linux-gnu") || is("x86_64-linux-gnu")) {
            return Target{TargetKind::X86_64_Linux};
        }
        if (is("wasm32") || is("wasm32-unknown-unknown")) {
            return Target{TargetKind::Wasm32_Unknown};
        }
        if (is("wasm32-wasi") || is("wasm32-unknown-wasi")) {
            return Target{TargetKind::Wasm32_Wasi};
        }
        if (is("wasm32-emscripten") || is("wasm32-unknown-emscripten")) {
            return Target{TargetKind::Wasm32_Emscripten};
        }
        return std::nullopt;
    }

    // Default 'build/target_os'/'build/target_arch' $option values, used only when
    // the user didn't pass an explicit '--opt' override — matching
    // OperatingSystem/Architecture's variant names in the (separately-maintained)
    // stdlib Core/Compiler/Options module, so both name-based and value-based
    // $option coercion work. These strings are BYTE-IDENTICAL to what the
    // llvm::Triple-based code produced; tests/cli_test.py pins them, because every
    // '#compile_only_if' in the standard library switches on them.
    auto default_target_os(const Target &target) -> std::string {
        return target.is_wasm() ? "Wasm32" : "Linux";
    }

    auto default_target_arch(const Target &target) -> std::string {
        return target.is_wasm() ? "Wasm32" : "X86_64";
    }

    // The linker driver, in precedence order: '--cc=', then $MIRAGE_CC, then a default read
    // off the target — 'emcc' for wasm (it is what knows how to turn a wasm object plus the
    // emscripten runtime into a loadable .js/.html), 'clang' otherwise. Mirrors how '--std='
    // overrides MIRAGE_PATH, which previously had no equivalent here -- the driver was
    // hardcoded, so a cross-compile or a non-default toolchain had no way in.
    auto resolve_cc(const Options &options, const Target &target) -> std::string {
        if (!options.cc.empty()) {
            return options.cc;
        }
        if (const char *env_value = std::getenv("MIRAGE_CC"); env_value != nullptr && *env_value != '\0') {
            return env_value;
        }
        return target.is_wasm() ? "emcc" : "clang";
    }

    auto link_executable(const std::filesystem::path &object_path, const std::filesystem::path &output_path,
                          const Options &options, const Target &target,
                          const std::vector<sema::LinkDirective> &link_directives) -> bool {
        std::vector<std::string> args{resolve_cc(options, target)};
        // wasm links through emscripten's own startup (it owns 'main' and brings up the
        // runtime before calling it), and neither of the two position-independence flags
        // means anything to a wasm module — emcc rejects '-no-pie' outright.
        if (target.is_wasm()) {
            // Nothing to add: every wasm-specific link input arrives as a '#link' directive.
        } else if (options.freestanding) {
            args.emplace_back("-ffreestanding");
            args.emplace_back("-nostdlib");
            args.emplace_back("-no-pie");
        } else {
            args.emplace_back("-nostartfiles");
            args.emplace_back("-no-pie");
        }

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
            std::cerr << "mirage: fork failed: " << std::strerror(errno) << "\n";
            return false;
        }
        if (pid == 0) {
            execvp(argv[0], argv.data());
            // Only reached if exec failed; the parent sees this as exit status 127.
            std::cerr << "mirage: cannot execute '" << args[0] << "': " << std::strerror(errno) << "\n";
            _exit(127);
        }

        int status = 0;
        if (waitpid(pid, &status, 0) < 0) {
            std::cerr << "mirage: waitpid failed: " << std::strerror(errno) << "\n";
            return false;
        }
        if (WIFSIGNALED(status)) {
            std::cerr << "mirage: linker '" << args[0] << "' was killed by signal "
                         << WTERMSIG(status) << " (" << strsignal(WTERMSIG(status)) << ")\n";
            return false;
        }
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            std::cerr << "mirage: linker '" << args[0] << "' exited with status "
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

    void dump_expr(const ast::Expr &expr, std::ostream &out) {
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
                } else if constexpr (std::is_same_v<V, ast::RttiEnabledExpr>) {
                    out << "$rtti_enabled";
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

    void dump_stmt(const ast::Stmt &stmt, std::ostream &out, int indent) {
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

    void dump_decl(const ast::Decl &decl, std::ostream &out) {
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
                } else if constexpr (std::is_same_v<V, ast::CompileOnlyIfDecl>) {
                    out << "#compile_only_if("; dump_expr(v.condition, out); out << ")\n";
                } else {
                    out << "<decl>\n";
                }
            },
            decl);
    }
}

// Links 'object_path' into the requested output and, for 'run'/'test', executes
// it -- shared verbatim by the LLVM and native backends, which differ only in how
// the object file's bytes came to exist.
auto link_and_finish(const std::filesystem::path &object_path, const Options &options,
                      const Target &target,
                      const std::vector<sema::LinkDirective> &link_directives,
                      const std::chrono::steady_clock::time_point start_time,
                      const std::chrono::steady_clock::duration object_elapsed) -> int {
    const auto to_ms = [](auto elapsed) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    };
    // 'test' builds to a temp path and executes it, exactly as 'run' does — the compiled
    // output is the test binary, and there is nothing to leave behind.
    const bool executes_output = options.action == Action::Run || options.action == Action::Test;
    const auto exe_path = executes_output
        ? make_temp_file("")
        : std::filesystem::path(options.output);
    if (exe_path.empty()) {
        std::error_code exe_temp_error;
        std::filesystem::remove(object_path, exe_temp_error);
        return 1;
    }

    const auto link_start = std::chrono::steady_clock::now();
    if (!link_executable(object_path, exe_path, options, target, link_directives)) {
        // link_executable already printed a detailed diagnostic; no second banner. For
        // 'run', the executable path is an mkstemp placeholder that would otherwise be
        // orphaned in $TMPDIR on every failed link.
        std::error_code remove_error;
        std::filesystem::remove(object_path, remove_error);
        if (executes_output) {
            std::filesystem::remove(exe_path, remove_error);
        }
        return 1;
    }
    const auto link_elapsed = std::chrono::steady_clock::now() - link_start;

    std::error_code remove_error;
    std::filesystem::remove(object_path, remove_error);

    std::cerr << std::format(
        "  object:  {}ms\n"
        "  link:    {}ms\n",
        to_ms(object_elapsed), to_ms(link_elapsed));

    const auto elapsed = std::chrono::steady_clock::now() - start_time;
    const auto secs = std::chrono::duration<double>(elapsed).count();
    if (executes_output) {
        std::cerr << std::format("Compiled '{}' in {:.2f}s\n", options.module_path, secs);
    } else {
        std::cerr << std::format("Compiled '{}' -> '{}' in {:.2f}s\n", options.module_path, options.output, secs);
    }
    // Flush before fork() so nothing still buffered here is inherited by the child and
    // written out a second time from the child's copy of the buffer.
    std::cout.flush();
    std::cerr.flush();

    if (executes_output) {
        const pid_t pid = fork();
        if (pid < 0) {
            std::cerr << "mirage: fork failed\n";
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
            std::cerr << std::format("process exited with code {}\n", code);
            return code;
        }
        if (WIFSIGNALED(status)) {
            // Previously this fell through to a bare 'return 1', so a compiled program that
            // segfaulted looked like an ordinary non-zero exit -- which is exactly how
            // CODEGEN-1's stack-exhaustion crash stayed invisible through 'mirage run'.
            const int sig = WTERMSIG(status);
            std::cerr << std::format("process was killed by signal {} ({})\n", sig, strsignal(sig));
            return 128 + sig;
        }
        return 1;
    }

    return 0;
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

    // Target-platform '$option' defaults ('build/target_os'/'build/target_arch'), used only
    // where the user didn't already pass an explicit '--opt' override. Derived from the
    // SELECTED triple, so '--target=wasm32-unknown-emscripten' alone flips every
    // '#compile_only_if(target_os == .Wasm32)' file in the standard library without the
    // caller having to restate it as a '--opt'.
    const auto parsed_target = parse_target(options.target);
    if (!parsed_target) {
        std::cerr << "error: unsupported target '" << options.target << "'\n"
                  << "       supported: x86_64-linux (the host), wasm32-unknown-unknown,\n"
                  << "       wasm32-wasi, wasm32-unknown-emscripten\n";
        return 1;
    }
    const auto target = *parsed_target;
    options.opt_values.try_emplace("build/target_os", default_target_os(target));
    options.opt_values.try_emplace("build/target_arch", default_target_arch(target));

    // 'mirage test' validation, up front rather than after a full compile. The harness in
    // 'core/testing' forks a child per test case for crash isolation, so it needs POSIX
    // process primitives that neither a freestanding target nor wasm can provide.
    if (options.action == Action::Test) {
        if (options.freestanding) {
            std::cerr << "mirage: 'mirage test' is not supported with '--freestanding' (test isolation\n"
                            "       requires POSIX process primitives not available in freestanding builds)\n";
            return 1;
        }
        if (target.is_wasm()) {
            std::cerr << "mirage: 'mirage test' is not supported for target '" << target.name()
                         << "' (test isolation requires POSIX process primitives)\n";
            return 1;
        }
        // The test binary is a temporary that is run and then deleted, so there is nothing
        // for '-o' to name. Rejected rather than ignored: silently accepting it looked like
        // it had worked and left the user looking for a file that was never written.
        if (options.output_explicit) {
            std::cerr << "mirage: '-o' is not supported with 'mirage test'; the test binary is temporary\n"
                            "       (use '--emit-ir' to inspect what was generated)\n";
            return 1;
        }
        // The reserved module supplying the harness. Appended AFTER any '--load' the user
        // gave, so an explicit '--load core/testing' still resolves to the same module and
        // is a no-op rather than a double load.
        options.forced_modules.emplace_back(TESTING_MODULE_PATH);
    }

    // 'run' forks and execs the linked output. A wasm build's output is a page (plus a .wasm
    // beside it) that only a browser can start, so say that up front rather than after a
    // full compile — and before the link step, where emcc would reject the extensionless
    // temp file 'run' hands it with a message about suffixes.
    if (options.action == Action::Run && target.is_wasm()) {
        std::cerr << "mirage: 'run' is not supported for target '" << target.name()
                     << "'; use 'build -o <file>.html' and open the result in a browser\n";
        return 1;
    }

    const auto start_time = std::chrono::steady_clock::now();

    SourceManager source_manager;
    DiagnosticEngine diag(source_manager);

    const auto parse_start = std::chrono::steady_clock::now();
    const auto ast = ast::resolve(options.module_path, source_manager, diag, ast::ResolveOptions{
        .std_path_override = options.std_path,
        // Search root 4. Computed here rather than inside the resolver so that module
        // resolution stays free of process introspection and the LSP (which shares the
        // resolver) can supply its own answer or none at all.
        .compiler_dir = ast::executable_directory(argv[0]),
        .forced_modules = options.forced_modules,
    });
    if (!ast.ok) {
        // The search trace is still worth printing on failure -- it shows which imports DID
        // resolve and where, which is usually what narrows down the one that didn't.
        if (options.print_module_search) {
            for (const auto &record : ast.module_search_trace) {
                std::cout << std::format("{} -> '{}' -> {}  [{}]\n",
                    record.importer, record.import_path, record.resolved_path, record.root_label);
            }
        }
        return 1;
    }
    const auto parse_elapsed = std::chrono::steady_clock::now() - parse_start;

    if (options.print_module_search) {
        for (const auto &record : ast.module_search_trace) {
            std::cout << std::format("{} -> '{}' -> {}  [{}]\n",
                record.importer, record.import_path, record.resolved_path, record.root_label);
        }
        return 0;
    }

    if (options.dump_ast) {
        if (const auto root_it = ast.modules.find(ast.root_module_path); root_it != ast.modules.end()) {
            for (const auto &decl : ast::all_decls(root_it->second)) {
                dump_decl(decl, std::cout);
            }
        }
        return 0;
    }

    const auto sema_start = std::chrono::steady_clock::now();
    const auto sema = sema::check_program(ast, diag, sema::Options{
        .opt_values = options.opt_values,
        .eager_generic_check = options.eager_generic_check,
        .pointer_size = target.pointer_bits() / 8,
        .rtti_enabled = !options.nortti,
        .test_mode = options.action == Action::Test,
    });
    if (!sema.ok) {
        return 1;
    }
    const auto sema_elapsed = std::chrono::steady_clock::now() - sema_start;

    if (options.print_link_directives) {
        for (const auto &link : sema.link_directives) {
            const char *category = link.category == sema::LinkCategory::Lib ? "lib"
                                  : link.category == sema::LinkCategory::System ? "system" : "flag";
            std::cout << category << " " << link.data << "  (from " << link.source_module << ")\n";
        }
        return 0;
    }

    // 'core/testing' is a reserved path: under 'test' the compiler assumes it resolved and
    // exposes the fixed contract. Checked HERE — right after sema, before codegen — so a
    // missing or reshaped module is a legible driver error rather than a confusing type
    // mismatch deep inside Test_Info synthesis.
    std::string testing_module_path;
    if (options.action == Action::Test) {
        for (const auto &record : ast.module_search_trace) {
            if (record.import_path == TESTING_MODULE_PATH) {
                testing_module_path = record.resolved_path;
                break;
            }
        }
        const auto missing_contract = [&] {
            const auto mod = sema.modules.find(testing_module_path);
            if (testing_module_path.empty() || mod == sema.modules.end()) {
                return true;
            }
            // Exactly the names codegen reaches for, and their shapes: two structs and a
            // function. Checking the shape (not just the name) is what makes the error
            // legible when the module exists but has drifted.
            for (const char *type_name : {"Test_Function", "Test_Case", "Test_Info"}) {
                const auto it = mod->second.symbols.find(type_name);
                if (it == mod->second.symbols.end() || !std::holds_alternative<sema::TypeSymbol>(it->second)) {
                    return true;
                }
            }
            const auto runner = mod->second.symbols.find("_run_tests");
            return runner == mod->second.symbols.end() ||
                   !std::holds_alternative<sema::FunctionSymbol>(runner->second);
        };
        if (missing_contract()) {
            std::cerr << "mirage: 'core/testing' could not be resolved or does not expose the expected\n"
                            "        testing contract (Test_Function, Test_Case, Test_Info, _run_tests) —\n"
                            "        required for 'mirage test'\n";
            return 1;
        }
    }

    // '--emit-mir' stops here: the native backend cannot produce an object yet, so there is
    // nothing downstream to hand the module to. Printing is the whole point — reading MIR is
    // how stage 2 is validated (docs/backend.md).
    if (options.emit_mir) {
        auto lowered = mirgen::generate(ast, sema, diag, mirgen::Options{
            .noinit = options.noinit,
            .freestanding = options.freestanding,
            // Under 'test', print the module the test binary would be built
            // from — the synthesized wrappers, Test_Info and runner entry
            // included. Omitting this made '--emit-mir' show a module that no
            // build ever produces, which is exactly what one inspects it for.
            .testing_module_path = testing_module_path,
            .pointer_bits = target.pointer_bits(),
        });
        if (options.mir_opt && lowered.ok) {
            mir::promote_slots(lowered.module);
            mir::peephole(lowered.module);
            // The pass must leave a well-formed module behind; a failure here is a
            // pass bug, reported as such rather than passed downstream silently.
            for (const auto &error : mir::verify(lowered.module)) {
                std::cerr << "error: internal error: promote_slots produced malformed MIR: "
                             << error.message << "\n";
                lowered.ok = false;
            }
        }
        std::cout << mir::print(lowered.module);
        if (!lowered.unsupported.empty()) {
            // A coverage report, not a failure list: mirgen is grown construct by construct,
            // and this says exactly how far it has got on THIS program.
            std::cerr << "\nnot yet lowered by the native backend:\n";
            for (const auto &what : lowered.unsupported) {
                std::cerr << "  - " << what << "\n";
            }
        }
        return lowered.ok ? 0 : 1;
    }

    // The front-end banner. stderr, as ever: stdout carries only what was
    // asked for.
    const auto to_ms = [](auto elapsed) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    };
    std::cerr << std::format(
        "Processed {} file(s), {} token(s)\n"
        "  parsing: {}ms\n"
        "  sema:    {}ms\n",
        ast.file_count, ast.token_count,
        to_ms(parse_elapsed), to_ms(sema_elapsed));

    // The native pipeline — the only one, since stage 10's deletion half: lower,
    // run the MIR passes, verify, then per target either x86-64 machine code and
    // the link/run tail, or a wasm module/object.
    const bool wasm_target = target.is_wasm();
    {
        const auto codegen_start = std::chrono::steady_clock::now();
        auto lowered = mirgen::generate(ast, sema, diag, mirgen::Options{
            .noinit = options.noinit,
            .validate_entry = !options.freestanding,
            .freestanding = options.freestanding,
            .testing_module_path = testing_module_path, // resolved above; empty unless 'test'
            .pointer_bits = target.pointer_bits(),
        });
        if (!lowered.ok) {
            if (!lowered.unsupported.empty()) {
                std::cerr << "\nnot yet lowered by the native backend:\n";
                for (const auto &what : lowered.unsupported) {
                    std::cerr << "  - " << what << "\n";
                }
            }
            return 1;
        }
        mir::promote_slots(lowered.module);
        mir::peephole(lowered.module);
        for (const auto &error : mir::verify(lowered.module)) {
            std::cerr << "error: internal error: MIR passes produced malformed MIR: "
                         << error.message << "\n";
            return 1;
        }
        std::cerr << std::format("  codegen: {}ms\n",
                                     to_ms(std::chrono::steady_clock::now() - codegen_start));

        if (wasm_target) {
            const auto object_start = std::chrono::steady_clock::now();
            // Emscripten (stage 8): a RELOCATABLE object handed to the same emcc
            // link tail the LLVM path uses — libc, the web runtime and raylib's
            // ports all come from there (decision D5). Anything else: the final
            // standalone .wasm module, written straight to the output.
            if (target.is_emscripten()) {
                const auto generated = backend_wasm::generate_object(
                    lowered.module, lowered.test_info_global, lowered.test_runner_function);
                if (!generated.ok) {
                    for (const auto &error : generated.errors) {
                        std::cerr << "error: native backend: " << error << "\n";
                    }
                    return 1;
                }
                const auto object_path = make_temp_file(".o");
                if (object_path.empty()) {
                    return 1;
                }
                std::ofstream out(object_path, std::ios::binary);
                if (!out) {
                    std::cerr << "error: cannot write '" << object_path.string() << "'\n";
                    return 1;
                }
                out.write(reinterpret_cast<const char *>(generated.bytes.data()),
                           static_cast<std::streamsize>(generated.bytes.size()));
                out.close();
                const auto object_elapsed = std::chrono::steady_clock::now() - object_start;
                return link_and_finish(object_path, options, target,
                                        sema.link_directives, start_time, object_elapsed);
            }
            const auto generated = backend_wasm::generate(lowered.module,
                                                           lowered.test_info_global,
                                                           lowered.test_runner_function);
            if (!generated.ok) {
                for (const auto &error : generated.errors) {
                    std::cerr << "error: native backend: " << error << "\n";
                }
                return 1;
            }
            std::ofstream out(options.output, std::ios::binary);
            if (!out) {
                std::cerr << "error: cannot write '" << options.output << "'\n";
                return 1;
            }
            out.write(reinterpret_cast<const char *>(generated.bytes.data()),
                       static_cast<std::streamsize>(generated.bytes.size()));
            out.close();
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - object_start);
            std::cerr << "  wasm:    " << elapsed.count() << "ms\n";
            const auto total = std::chrono::duration_cast<std::chrono::duration<double>>(
                std::chrono::steady_clock::now() - start_time);
            std::cerr << std::format("Compiled '{}' -> '{}' in {:.2f}s\n",
                                         options.module_path, options.output, total.count());
            return 0;
        }

        const auto object_start = std::chrono::steady_clock::now();
        const auto generated = backend_x86::generate(lowered.module, lowered.test_info_global,
                                                      lowered.test_runner_function,
                                                      options.regalloc == "trivial"
                                                          ? backend_x86::RegAlloc::Trivial
                                                          : backend_x86::RegAlloc::Linear);
        if (!generated.ok) {
            for (const auto &error : generated.errors) {
                std::cerr << "error: native backend: " << error << "\n";
            }
            return 1;
        }
        const auto bytes = elf::write(generated.object);
        const auto object_path = make_temp_file(".o");
        if (object_path.empty()) {
            return 1;
        }
        {
            std::ofstream out(object_path, std::ios::binary);
            if (!out) {
                std::cerr << "error: cannot write '" << object_path.string() << "'\n";
                return 1;
            }
            out.write(reinterpret_cast<const char *>(bytes.data()),
                       static_cast<std::streamsize>(bytes.size()));
            if (!out) {
                std::cerr << "error: failed writing '" << object_path.string() << "'\n";
                return 1;
            }
        }
        const auto object_elapsed = std::chrono::steady_clock::now() - object_start;
        return link_and_finish(object_path, options, target, sema.link_directives,
                                start_time, object_elapsed);
    }

}

