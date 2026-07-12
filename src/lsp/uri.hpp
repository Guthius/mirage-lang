#pragma once

#include <string>
#include <string_view>

namespace lsp {
    // Converts a "file:///abs/path/with%20space.mir" URI (as sent by the
    // client in textDocument.uri) into a plain filesystem path, stripping the
    // "file://" scheme prefix and percent-decoding. Non-file-scheme URIs
    // (untitled:, etc.) are not supported - Mirage sources are always on-disk
    // .mir files under a module directory - and yield an empty string.
    auto uri_to_path(std::string_view uri) -> std::string;

    // Inverse of uri_to_path: builds a "file://" URI from an absolute
    // filesystem path, percent-encoding the characters editors expect
    // encoded (at minimum, spaces).
    auto path_to_uri(std::string_view path) -> std::string;
}
