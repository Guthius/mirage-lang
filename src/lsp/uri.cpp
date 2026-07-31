#include "uri.hpp"

namespace lsp {
    namespace {
        constexpr std::string_view FILE_SCHEME = "file://";

        // Scheme and host are case-insensitive per RFC 3986; 'FILE://' and
        // 'file://LOCALHOST/...' are valid spellings clients may produce.
        auto iequals_ascii(const std::string_view a, const std::string_view b) -> bool {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i) {
                const auto lower = [](const char c) {
                    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
                };
                if (lower(a[i]) != lower(b[i])) return false;
            }
            return true;
        }

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

            const auto escape = [&out](const unsigned char c) {
                constexpr char HEX[] = "0123456789ABCDEF";
                out.push_back('%');
                out.push_back(HEX[c >> 4]);
                out.push_back(HEX[c & 0xF]);
            };

            for (const unsigned char c : s) {
                // Keep this deliberately minimal: encode the characters that
                // are common in filesystem paths but reserved/unsafe in a URI,
                // plus every non-ASCII byte — VS Code percent-encodes those, and
                // a server-minted URI must string-compare equal to the client's
                // spelling of the same file. Full RFC 3986 encoding is
                // unnecessary for a Linux-only, .mir-extension-only tool.
                switch (c) {
                case '%':
                    // Must come first in spirit if not in order: without it a path containing
                    // a literal '%41' encoded to itself and decoded back to 'A', so the URI
                    // did not round-trip. '%' is the escape character, so it has to be escaped
                    // for any of the others to be unambiguous.
                case ' ':
                case '#':
                case '?':
                    escape(c);
                    break;
                default:
                    if (c >= 0x80) {
                        escape(c);
                    } else {
                        out.push_back(static_cast<char>(c));
                    }
                }
            }

            return out;
        }
    }

    // Returns "" for anything that is not a 'file://' URI with an empty or 'localhost'
    // authority (RFC 8089's two spellings of the local form). Callers treat "" as "not a
    // document this server can handle" (see canonical_path_of). Scheme and host match
    // case-insensitively, as RFC 3986 requires.
    //
    // Any other non-empty authority ('file://host/path') is rejected rather than silently
    // treated as the relative path 'host/path', which is what stripping the scheme alone
    // would do. This is a Linux-only tool and a remote host is not something it can serve.
    auto uri_to_path(std::string_view uri) -> std::string {
        if (uri.size() < FILE_SCHEME.size() || !iequals_ascii(uri.substr(0, FILE_SCHEME.size()), FILE_SCHEME)) {
            return {};
        }

        auto rest = uri.substr(FILE_SCHEME.size());
        constexpr std::string_view LOCALHOST = "localhost";
        if (rest.size() > LOCALHOST.size() && rest[LOCALHOST.size()] == '/' &&
            iequals_ascii(rest.substr(0, LOCALHOST.size()), LOCALHOST)) {
            rest = rest.substr(LOCALHOST.size());
        }
        if (!rest.starts_with('/')) {
            return {};
        }

        return percent_decode(rest);
    }

    auto path_to_uri(std::string_view path) -> std::string {
        return std::string(FILE_SCHEME) + percent_encode(path);
    }
}
