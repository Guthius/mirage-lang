#pragma once

#include <string>
#include <unordered_map>

class DiagnosticEngine;

struct SourceFile {
    std::string_view filename;
    std::string_view text;
};

class SourceManager {
  public:
    auto load(const std::string &canonical_path, DiagnosticEngine &diagnostics) -> SourceFile;
    auto set_source(const std::string &canonical_path, std::string text) -> SourceFile;
    auto get_source_line(std::string_view filename, size_t line) -> std::string_view;

  private:
    std::unordered_map<std::string, std::string> sources_;
};
