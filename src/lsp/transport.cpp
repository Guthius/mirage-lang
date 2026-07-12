#include "transport.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>

namespace lsp::transport {
    namespace {
        auto to_lower(std::string s) -> std::string {
            std::ranges::transform(s, s.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        }

        auto trim(std::string_view s) -> std::string_view {
            const auto start = s.find_first_not_of(" \t");
            if (start == std::string_view::npos) {
                return {};
            }
            const auto end = s.find_last_not_of(" \t");
            return s.substr(start, end - start + 1);
        }

        // Header lines are terminated by "\r\n"; std::getline splits on '\n'
        // so we just need to strip a trailing '\r'.
        auto read_line(std::istream &in) -> std::optional<std::string> {
            std::string line;
            if (!std::getline(in, line)) {
                return std::nullopt;
            }
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            return line;
        }
    }

    auto read_message(std::istream &in) -> std::optional<std::string> {
        size_t content_length = 0;
        bool have_length = false;

        while (true) {
            const auto line = read_line(in);
            if (!line) {
                return std::nullopt;
            }
            if (line->empty()) {
                break;
            }

            const auto colon = line->find(':');
            if (colon == std::string::npos) {
                std::cerr << "mirage-lsp: malformed header line, skipping: '" << *line << "'\n";
                continue;
            }

            const auto name = to_lower(line->substr(0, colon));
            const auto value = trim(std::string_view(*line).substr(colon + 1));

            if (name == "content-length") {
                try {
                    content_length = std::stoul(std::string(value));
                    have_length = true;
                } catch (const std::exception &) {
                    std::cerr << "mirage-lsp: malformed Content-Length header value: '" << value << "'\n";
                    return std::nullopt;
                }
            }
            // Other headers (e.g. Content-Type) are recognized-but-ignored.
        }

        if (!have_length) {
            std::cerr << "mirage-lsp: message received with no Content-Length header\n";
            return std::nullopt;
        }

        std::string body(content_length, '\0');
        size_t total_read = 0;
        while (total_read < content_length) {
            in.read(body.data() + total_read, static_cast<std::streamsize>(content_length - total_read));
            const auto n = in.gcount();
            if (n <= 0) {
                std::cerr << "mirage-lsp: stream closed before full message body was received\n";
                return std::nullopt;
            }
            total_read += static_cast<size_t>(n);
        }

        return body;
    }

    void write_message(std::ostream &out, std::string_view body) {
        out << "Content-Length: " << body.size() << "\r\n\r\n" << body;
        out.flush();
    }
}
