#include "diagnostic_engine.hpp"

#include "source_manager.hpp"

#include <algorithm>
#include <iostream>

void DiagnosticEngine::report(const DiagnosticLevel level, const DiagnosticStage stage, const SourceLocation &location, std::string message) {
    if (error_count_ >= MAX_ERRORS) {
        return;
    }

    diagnostics_.push_back({
        .level = level,
        .stage = stage,
        .location = location,
        .message = std::move(message),
    });

    const auto &diagnostic = diagnostics_.back();
    if (level == DiagnosticLevel::Error) {
        ++error_count_;

        if (error_count_ == MAX_ERRORS) {
            print_diagnostic(diagnostic);
            std::cerr << "mirage: too many errors emitted, stopping.\n";
            return;
        }
    }

    print_diagnostic(diagnostic);
}

void DiagnosticEngine::report_error(const DiagnosticStage stage, const SourceLocation &location, std::string message) {
    report(DiagnosticLevel::Error, stage, location, std::move(message));
}

void DiagnosticEngine::warn(const DiagnosticStage stage, const SourceLocation &location, std::string message) {
    report(DiagnosticLevel::Warning, stage, location, std::move(message));
}

void DiagnosticEngine::print_diagnostic(const Diagnostic &diagnostic) const {
    auto &out = std::cerr;

    out << diagnostic.location.filename << ":"
        << diagnostic.location.line << ":"
        << diagnostic.location.column << ": ";

    switch (diagnostic.level) {
    case DiagnosticLevel::Error:   out << "\033[1;31merror\033[0m: "; break;
    case DiagnosticLevel::Warning: out << "\033[1;33mwarning\033[0m: "; break;
    }

    out << diagnostic.message << "\n";

    if (const auto source_line = source_manager_.get_source_line(diagnostic.location.filename, diagnostic.location.line); !source_line.empty()) {
        out << "  " << source_line << "\n";
        out << "  ";

        // A reported column is not guaranteed to fall inside its line: a stage can point
        // just past end-of-line, and a miscomputed one can be arbitrarily large (an
        // unsigned underflow lands near SIZE_MAX). Clamp before indexing -- this loop
        // used to read source_line[i - 1] unbounded, which is an out-of-bounds read and,
        // for an underflowed column, also an effectively unbounded loop.
        const auto column = diagnostic.location.column;
        const auto padding = column > 0 ? std::min<size_t>(column - 1, source_line.size()) : 0;
        for (size_t i = 0; i < padding; ++i) {
            out << (source_line[i] == '\t' ? '\t' : ' ');
        }

        const auto max_length = column > 0 && column <= source_line.size()
            ? source_line.size() - column + 1
            : 1;
        const auto caret_count = std::max<size_t>(std::min<size_t>(diagnostic.location.length, max_length), 1);

        out << "\033[1;32m";
        for (size_t i = 0; i < caret_count; ++i) {
            out << '^';
        }
        out << "\033[0m\n";
    }
}
