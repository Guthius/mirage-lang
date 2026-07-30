#include "diagnostic_engine.hpp"

#include "source_manager.hpp"

#include <algorithm>
#include <format>
#include <iostream>

void DiagnosticEngine::report(const DiagnosticLevel level, const DiagnosticStage stage, const SourceLocation &location, std::string message) {
    // Deliberately checked before the level is examined, so once the error cap is reached
    // warnings stop too. Past that point the compilation has already been abandoned and
    // later stages are working from a broken program, so their warnings are as likely to be
    // noise as signal -- and the "too many errors emitted, stopping." line below would
    // otherwise be followed by more output, which reads as if it had not stopped.
    if (error_count_ >= MAX_ERRORS) {
        return;
    }

    // Exact-duplicate suppression, keyed on (stage, location, text). One source line can be
    // checked many times over: a generic body is checked once per instantiation AND once at
    // its declaration by the eager pass, so a genuine mistake in it would otherwise be
    // printed once per check. Nothing is lost — a repeat carries no information the first
    // copy didn't, and distinct problems at one location differ in their message.
    const auto key = std::format("{}|{}|{}:{}:{}|{}", static_cast<int>(level), static_cast<int>(stage),
                                  location.filename, location.line, location.column, message);
    if (!reported_.insert(key).second) {
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
