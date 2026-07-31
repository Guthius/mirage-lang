#include "diagnostics.hpp"

#include "locations.hpp"

namespace lsp::handlers {
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
        return {
            {"range", range_json(diagnostic.location.line, diagnostic.location.column, diagnostic.location.length)},
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
