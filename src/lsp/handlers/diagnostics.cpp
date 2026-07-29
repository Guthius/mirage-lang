#include "diagnostics.hpp"

#include <algorithm>

namespace lsp::handlers {
    namespace {
        auto to_zero_based(const size_t one_based) -> size_t {
            return one_based == 0 ? 0 : one_based - 1;
        }
    }

    namespace {
        auto stage_name(const DiagnosticStage stage) -> const char * {
            switch (stage) {
            case DiagnosticStage::Lexer:   return "lexer";
            case DiagnosticStage::Parser:  return "parser";
            case DiagnosticStage::Sema:    return "sema";
            case DiagnosticStage::Codegen: return "codegen";
            }
            return "mirage";
        }
    }

    auto to_lsp_diagnostic(const Diagnostic &diagnostic) -> nlohmann::json {
        const auto line = to_zero_based(diagnostic.location.line);
        const auto character = to_zero_based(diagnostic.location.column);
        const auto length = std::max<size_t>(diagnostic.location.length, 1);

        return {
            {"range", {
                {"start", {{"line", line}, {"character", character}}},
                {"end", {{"line", line}, {"character", character + length}}},
            }},
            {"severity", diagnostic.level == DiagnosticLevel::Error ? 1 : 2},
            {"source", "mirage"},
            // The producing stage, surfaced as the LSP 'code'. 'source' stays "mirage" (the
            // tool) so existing client filters keep working; 'code' carries the category,
            // which is what it is for. This is the only reader of Diagnostic::stage, which
            // was otherwise set at every call site and never consulted anywhere.
            {"code", stage_name(diagnostic.stage)},
            {"message", diagnostic.message},
        };
    }

    auto group_diagnostics_by_file(const std::vector<Diagnostic> &diagnostics)
        -> std::unordered_map<std::string, std::vector<nlohmann::json>> {
        std::unordered_map<std::string, std::vector<nlohmann::json>> grouped;

        for (const auto &diagnostic : diagnostics) {
            if (diagnostic.location.filename.empty()) {
                continue;
            }
            grouped[std::string(diagnostic.location.filename)].push_back(to_lsp_diagnostic(diagnostic));
        }

        return grouped;
    }
}
