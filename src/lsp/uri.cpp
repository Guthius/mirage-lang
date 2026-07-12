#include "uri.hpp"

namespace lsp {
    namespace {
        constexpr std::string_view FILE_SCHEME = "file://";

        auto hex_digit(const char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        }

        auto percent_decode(std::string_view s) -> std::string {
            std::string out;
            out.reserve(s.size());

            for (size_t i = 0; i < s.size(); ++i) {
                if (s[i] == '%' && i + 2 < s.size()) {
                    const int hi = hex_digit(s[i + 1]);
                    const int lo = hex_digit(s[i + 2]);
                    if (hi >= 0 && lo >= 0) {
                        out.push_back(static_cast<char>((hi << 4) | lo));
                        i += 2;
                        continue;
                    }
                }
                out.push_back(s[i]);
            }

            return out;
        }

        auto percent_encode(std::string_view s) -> std::string {
            std::string out;
            out.reserve(s.size());

            for (const unsigned char c : s) {
                // Keep this deliberately minimal: encode the characters that
                // are common in filesystem paths but reserved/unsafe in a URI.
                // Full RFC 3986 encoding is unnecessary for a Linux-only,
                // .mir-extension-only tool.
                switch (c) {
                case ' ':
                    out += "%20";
                    break;
                case '#':
                    out += "%23";
                    break;
                case '?':
                    out += "%3F";
                    break;
                default:
                    out.push_back(static_cast<char>(c));
                }
            }

            return out;
        }
    }

    auto uri_to_path(std::string_view uri) -> std::string {
        if (!uri.starts_with(FILE_SCHEME)) {
            return {};
        }

        return percent_decode(uri.substr(FILE_SCHEME.size()));
    }

    auto path_to_uri(std::string_view path) -> std::string {
        return std::string(FILE_SCHEME) + percent_encode(path);
    }
}
