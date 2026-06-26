#include <Compiler/ast.hpp>
#include <Compiler/lexer.hpp>
#include <Compiler/sema.hpp>

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {
    auto ReadFile(const std::string &path) -> std::string {
        std::ifstream ifs(path);
        if (!ifs) {
            std::cerr << "cannot open file '" << path << "'\n";
            std::exit(1);
        }
        std::ostringstream ss;
        ss << ifs.rdbuf();
        return ss.str();
    }
}

auto main(const int argc, char *argv[]) -> int {
    if (argc != 2) {
        return 1;
    }

    const auto filename = std::string(argv[1]);
    const auto source = ReadFile(filename);

    DiagnosticEngine diagnostics;
    diagnostics.set_source(filename, source);

    auto tokens = lexer::Tokenize(source, filename, diagnostics);
    if (diagnostics.has_errors()) {
        return 1;
    }

    const auto decls = ast::parse(tokens, diagnostics);
    if (diagnostics.has_errors()) {
        return 1;
    }

    auto res = sema::check(decls, diagnostics);
    if (diagnostics.has_errors()) {
        return 1;
    }

    return 0;
}

#include <unordered_map>

using module = std::vector<ast::Decl>;

struct program {
    std::unordered_map<std::string, module> modules;
    std::vector<std::string> resolution_order;
    std::vector<std::pair<std::string, std::string>> cycles;
};

class module_resolver {
  public:
    auto resolve(const module &m) -> program;
};
