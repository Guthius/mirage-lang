#include "DiagnosticEngine.hpp"

#include <iostream>

void DiagnosticEngine::SetSource(const std::string_view filename, const std::string_view source) {
    filename_ = filename;
    source_ = source;
}

void DiagnosticEngine::Report(const DiagnosticLevel level, const DiagnosticStage stage, const SourceLocation &location, std::string message) {
    if (error_count_ >= MAX_ERRORS) {
        return;
    }

    diagnostics_.push_back({
        .Level = level,
        .Stage = stage,
        .Location = location,
        .Message = std::move(message),
    });

    const auto &diagnostic = diagnostics_.back();
    if (level == DiagnosticLevel::Error) {
        ++error_count_;

        if (error_count_ == MAX_ERRORS) {
            PrintDiagnostic(diagnostic);
            std::cerr << "mirage: too many errors emitted, stopping.\n";
            return;
        }
    }

    PrintDiagnostic(diagnostic);
}

void DiagnosticEngine::ReportError(const DiagnosticStage stage, const SourceLocation &location, std::string message) {
    Report(DiagnosticLevel::Error, stage, location, std::move(message));
}

void DiagnosticEngine::Warn(const DiagnosticStage stage, const SourceLocation &location, std::string message) {
    Report(DiagnosticLevel::Warning, stage, location, std::move(message));
}

void DiagnosticEngine::PrintDiagnostic(const Diagnostic &diagnostic) const {
    auto &out = std::cerr;

    out << diagnostic.Location.Filename << ":"
        << diagnostic.Location.Line << ":"
        << diagnostic.Location.Column << ": ";

    switch (diagnostic.Level) {
    case DiagnosticLevel::Error:   out << "\033[1;31merror\033[0m: "; break;
    case DiagnosticLevel::Warning: out << "\033[1;33mwarning\033[0m: "; break;
    }

    out << diagnostic.Message << "\n";

    if (const auto source_line = GetSourceLine(diagnostic.Location.Line); !source_line.empty()) {
        out << "  " << source_line << "\n";
        out << "  ";

        for (uint32_t i = 1; i < diagnostic.Location.Column; ++i) {
            out << (source_line[i - 1] == '\t' ? '\t' : ' ');
        }

        out << "\033[1;32m^\033[0m\n";
    }
}

auto DiagnosticEngine::GetSourceLine(const uint32_t line) const -> std::string_view {
    if (source_.empty() || line == 0) {
        return {};
    }

    uint32_t current_line = 1;
    size_t line_start = 0;

    for (size_t i = 0; i < source_.size(); ++i) {
        if (current_line == line) {
            line_start = i;
            while (i < source_.size() && source_[i] != '\n') {
                ++i;
            }

            return source_.substr(line_start, i - line_start);
        }

        if (source_[i] == '\n') {
            ++current_line;
        }
    }

    return {};
}
