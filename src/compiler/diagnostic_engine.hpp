#pragma once

#include "source_location.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

class SourceManager;

enum class DiagnosticLevel : uint8_t {
    Error,
    Warning,
};

enum class DiagnosticStage : uint8_t {
    Lexer,
    Parser,
    Sema,
    Codegen,
};

struct Diagnostic {
    DiagnosticLevel level;
    DiagnosticStage stage;
    SourceLocation location;
    std::string message;
};

class DiagnosticEngine {
  public:
    explicit DiagnosticEngine(SourceManager &source_manager) : source_manager_(source_manager) {}

    void report(DiagnosticLevel level, DiagnosticStage stage, const SourceLocation &location, std::string message);
    void report_error(DiagnosticStage stage, const SourceLocation &location, std::string message);
    void warn(DiagnosticStage stage, const SourceLocation &location, std::string message);

    [[nodiscard]] auto has_errors() const -> bool { return error_count_ > 0; }
    [[nodiscard]] auto has_reached_max_errors() const -> bool { return error_count_ >= MAX_ERRORS; }
    [[nodiscard]] auto diagnostics() const -> const std::vector<Diagnostic> & { return diagnostics_; }

  private:
    void print_diagnostic(const Diagnostic &diagnostic) const;

    static constexpr size_t MAX_ERRORS = 20;

    SourceManager &source_manager_;
    std::vector<Diagnostic> diagnostics_;
    size_t error_count_ = 0;
};
