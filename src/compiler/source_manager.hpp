#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

class DiagnosticEngine;

struct SourceFile {
    std::string_view filename;
    std::string_view text;
};

// Owns the text of every source file loaded during one compilation.
//
// Lifetime contract: every SourceFile / string_view handed out by this class points into
// storage owned here, and stays valid only until the entry for that same path is replaced.
// Calling set_source() again for a path invalidates every view previously returned for it
// -- insert_or_assign move-assigns over the stored std::string, freeing its old buffer.
// Views for *other* paths are unaffected (node-based container, so rehashing does not move
// the mapped strings).
//
// Today nothing relies on this: the one set_source() caller, lsp/analysis.cpp, builds a
// fresh SourceManager per analyse() call. Any future incremental or long-lived use must
// either re-fetch views after re-setting a file, or stop reusing the instance.
class SourceManager {
  public:
    auto load(const std::string &canonical_path, DiagnosticEngine &diagnostics) -> SourceFile;

    // Replaces the text for this path. See the lifetime note above: this invalidates every
    // view previously returned for the same path.
    auto set_source(const std::string &canonical_path, std::string text) -> SourceFile;

    // The line's text without its terminator, and without the '\r' of a CRLF pair.
    auto get_source_line(std::string_view filename, size_t line) -> std::string_view;

  private:
    // Transparent hashing so get_source_line can look up a string_view key directly.
    // Without it, every diagnostic printed built a throwaway std::string just to probe
    // the map.
    struct StringHash {
        using is_transparent = void;
        [[nodiscard]] auto operator()(const std::string_view value) const -> size_t {
            return std::hash<std::string_view>{}(value);
        }
    };

    std::unordered_map<std::string, std::string, StringHash, std::equal_to<>> sources_;
};
