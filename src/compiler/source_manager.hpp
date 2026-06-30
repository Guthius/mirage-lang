#pragma once

#include <string>
#include <unordered_map>

class DiagnosticEngine;

class SourceManager {
  public:
    auto load(const std::string &canonical_path, DiagnosticEngine &diagnostics) -> std::string_view;
    auto get_source_line(std::string_view filename, size_t line) -> std::string_view;

  private:
    std::unordered_map<std::string, std::string> sources_;
};
