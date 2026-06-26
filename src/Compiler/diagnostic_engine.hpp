#pragma once

#include "source_location.hpp"

#include <cstdint>
#include <string>
#include <vector>

enum class DiagnosticLevel : uint8_t {
    Error,
    Warning,
};

enum class DiagnosticStage : uint8_t {
    Lexer,
    Parser,
    Sema,
};

struct Diagnostic {
    DiagnosticLevel level;
    DiagnosticStage stage;
    SourceLocation location;
    std::string message;
};

class DiagnosticEngine {
  public:
    void set_source(std::string_view filename, std::string_view source);

    void report(DiagnosticLevel level, DiagnosticStage stage, const SourceLocation &location, std::string message);
    void report_error(DiagnosticStage stage, const SourceLocation &location, std::string message);
    void warn(DiagnosticStage stage, const SourceLocation &location, std::string message);

    [[nodiscard]] auto has_errors() const -> bool { return error_count_ > 0; }
    [[nodiscard]] auto has_reached_max_errors() const -> bool { return error_count_ >= MAX_ERRORS; }

  private:
    void print_diagnostic(const Diagnostic &diagnostic) const;

    [[nodiscard]]
    auto get_source_line(uint32_t line) const -> std::string_view;

    static constexpr size_t MAX_ERRORS = 20;

    std::string_view filename_;
    std::string_view source_;
    std::vector<Diagnostic> diagnostics_;
    size_t error_count_ = 0;
};
