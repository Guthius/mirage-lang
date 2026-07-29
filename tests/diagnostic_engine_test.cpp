// Regression tests for DiagnosticEngine::print_diagnostic's source-line rendering.
//
// The caret-padding loop used to index into the source line with no bound check, so any
// diagnostic reported at a column past the end of its line read out of bounds. That is
// reachable from any stage that computes a bad location -- the lexer's multi-line
// string/char handling used to underflow the column to a value near SIZE_MAX -- which
// made the diagnostic printer itself a crash vector for the whole compiler.
//
// These tests report deliberately malformed locations and assert the printer stays
// inside the line and terminates.

#include "compiler/diagnostic_engine.hpp"
#include "compiler/source_manager.hpp"

#include <cstdio>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>

namespace {
    int failures = 0;

    void check(const bool condition, const char *name) {
        if (condition) {
            return;
        }
        ++failures;
        std::fprintf(stderr, "FAIL %s\n", name);
    }

    // Reports one diagnostic and returns everything print_diagnostic wrote to stderr.
    auto render(const std::string &line, const SourceLocation &location) -> std::string {
        SourceManager source_manager;
        source_manager.set_source("<test>", line);
        DiagnosticEngine diagnostics(source_manager);

        std::ostringstream captured;
        auto *previous = std::cerr.rdbuf(captured.rdbuf());
        diagnostics.report_error(DiagnosticStage::Lexer, location, "test diagnostic");
        std::cerr.rdbuf(previous);

        return captured.str();
    }

    auto location_at(const size_t line, const size_t column, const size_t length) -> SourceLocation {
        return SourceLocation{.filename = "<test>", .line = line, .column = column, .length = length};
    }

    void test_column_past_end_of_line() {
        // The exact shape the old lexer produced for a multi-line string literal:
        // an unsigned wraparound of 'col_ - (pos_ - offset)'.
        constexpr auto underflowed = std::numeric_limits<size_t>::max() - 3;
        const auto output = render("short line", location_at(1, underflowed, 1));

        check(!output.empty(), "underflowed column still renders a diagnostic");
        check(output.find("test diagnostic") != std::string::npos,
              "underflowed column keeps the message");
        // Bounded output is the real assertion: unclamped padding would emit ~1.8e19
        // characters (if it survived the out-of-bounds reads at all).
        check(output.size() < 4096, "underflowed column produces bounded output");
    }

    void test_column_just_past_line_length() {
        const std::string line = "abc";
        const auto output = render(line, location_at(1, line.size() + 5, 1));
        check(output.find("test diagnostic") != std::string::npos,
              "column past end of a short line renders");
        check(output.size() < 4096, "column past end of a short line stays bounded");
    }

    void test_huge_length_is_clamped() {
        const auto output = render("abc", location_at(1, 1, std::numeric_limits<size_t>::max()));
        check(output.size() < 4096, "huge length produces bounded caret run");
    }

    void test_zero_column() {
        const auto output = render("abc", location_at(1, 0, 1));
        check(output.find("test diagnostic") != std::string::npos, "zero column renders");
        check(output.size() < 4096, "zero column stays bounded");
    }

    // The clamping must not change how ordinary, in-range diagnostics look.
    void test_normal_diagnostic_still_correct() {
        const auto output = render("let x = 1", location_at(1, 5, 1));
        check(output.find("let x = 1") != std::string::npos, "normal diagnostic shows source line");
        check(output.find("    \033[1;32m^") != std::string::npos,
              "normal diagnostic puts the caret under column 5");
    }

    // Tabs in the padding region are preserved so the caret lines up in a tab-using
    // editor; that behavior predates the bound check and must survive it.
    void test_tab_padding_preserved() {
        const auto output = render("\t\tx", location_at(1, 3, 1));
        check(output.find("  \t\t\033[1;32m^") != std::string::npos,
              "tabs before the caret column are preserved");
    }
}

auto main() -> int {
    test_column_past_end_of_line();
    test_column_just_past_line_length();
    test_huge_length_is_clamped();
    test_zero_column();
    test_normal_diagnostic_still_correct();
    test_tab_padding_preserved();

    if (failures > 0) {
        std::fprintf(stderr, "%d diagnostic engine test(s) failed\n", failures);
        return 1;
    }
    std::printf("all diagnostic engine tests passed\n");
    return 0;
}
