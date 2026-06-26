#pragma once

#include <Compiler/SourceLocation.hpp>

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
    DiagnosticLevel Level;
    DiagnosticStage Stage;
    SourceLocation Location;
    std::string Message;
};

class DiagnosticEngine {
  public:
    void SetSource(std::string_view filename, std::string_view source);

    void Report(DiagnosticLevel level, DiagnosticStage stage, const SourceLocation &location, std::string message);
    void ReportError(DiagnosticStage stage, const SourceLocation &location, std::string message);
    void Warn(DiagnosticStage stage, const SourceLocation &location, std::string message);

    [[nodiscard]] auto HasErrors() const -> bool { return error_count_ > 0; }
    [[nodiscard]] auto HasReachedMaxErrors() const -> bool { return error_count_ >= MAX_ERRORS; }

  private:
    void PrintDiagnostic(const Diagnostic &diagnostic) const;

    [[nodiscard]]
    auto GetSourceLine(uint32_t line) const -> std::string_view;

    static constexpr size_t MAX_ERRORS = 20;

    std::string_view filename_;
    std::string_view source_;
    std::vector<Diagnostic> diagnostics_;
    size_t error_count_ = 0;
};
