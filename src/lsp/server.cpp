#include "server.hpp"

#include "compiler/module_resolver.hpp"
#include "handlers/definition.hpp"
#include "handlers/diagnostics.hpp"
#include "handlers/hover.hpp"
#include "transport.hpp"
#include "uri.hpp"

#include <filesystem>

#include <nlohmann/json.hpp>

#include <iostream>

namespace lsp {
    namespace {
        using json = nlohmann::json;

        enum class LifecycleState : uint8_t {
            Uninitialized,
            Running,
        };

        constexpr int METHOD_NOT_FOUND = -32601;
        constexpr int SERVER_NOT_INITIALIZED = -32002;

        void send(std::ostream &out, json message) {
            message["jsonrpc"] = "2.0";
            transport::write_message(out, message.dump());
        }

        void send_response(std::ostream &out, const json &id, json result) {
            send(out, {{"id", id}, {"result", std::move(result)}});
        }

        void send_error(std::ostream &out, const json &id, const int code, std::string message_text) {
            send(out, {{"id", id}, {"error", {{"code", code}, {"message", std::move(message_text)}}}});
        }

        void send_notification(std::ostream &out, std::string method, json params) {
            send(out, {{"method", std::move(method)}, {"params", std::move(params)}});
        }

        // Derives the canonical filesystem path this analysis pipeline keys
        // everything by, from a client-supplied "file://..." URI.
        auto canonical_path_of(const std::string &uri) -> std::string {
            return ast::canonicalize(uri_to_path(uri));
        }
    }

    auto Server::run(std::istream &in, std::ostream &out) -> int {
        auto state = LifecycleState::Uninitialized;
        bool shutdown_received = false;

        // After analysing `path`, publishes diagnostics for every file the
        // analysis touched, plus always for `path` itself (even if empty) so
        // fixing the last error in the currently-edited file clears its
        // squiggles. Other files that had errors in a previous run but were
        // not part of *this* run's diagnostics keep their last-published
        // squiggles - a known v1 simplification (see analysis.hpp).
        auto publish_diagnostics = [&](const std::string &path) {
            const auto &result = documents_.ensure_analysed(path);
            auto grouped = handlers::group_diagnostics_by_file(result.diagnostics);
            if (!grouped.contains(path)) {
                grouped[path] = {};
            }
            for (auto &[filename, diags] : grouped) {
                send_notification(out, "textDocument/publishDiagnostics", {
                    {"uri", path_to_uri(filename)},
                    {"diagnostics", std::move(diags)},
                });
            }
        };

        while (true) {
            const auto raw = transport::read_message(in);
            if (!raw) {
                std::cerr << "mirage-lsp: input stream closed, exiting\n";
                return shutdown_received ? 0 : 1;
            }

            json message;
            try {
                message = json::parse(*raw);
            } catch (const std::exception &e) {
                std::cerr << "mirage-lsp: failed to parse message as JSON: " << e.what() << "\n";
                continue;
            }

            const auto method = message.value("method", std::string{});
            const bool has_id = message.contains("id") && !message["id"].is_null();

            std::cerr << "mirage-lsp: <- " << (method.empty() ? "(response)" : method) << "\n";

            if (method.empty()) {
                // A message with no "method" is a response to a request we
                // sent (we don't send any requests yet), not something to dispatch.
                continue;
            }

            if (state == LifecycleState::Uninitialized && method != "initialize" && method != "exit") {
                if (has_id) {
                    send_error(out, message["id"], SERVER_NOT_INITIALIZED, "server not initialized");
                }
                continue;
            }

            if (method == "initialize") {
                state = LifecycleState::Running;
                const json capabilities = {
                    {"textDocumentSync", {{"openClose", true}, {"change", 1}}},
                    {"hoverProvider", true},
                    {"definitionProvider", true},
                };
                send_response(out, message["id"], {{"capabilities", capabilities}});
            } else if (method == "initialized") {
                std::cerr << "mirage-lsp: client finished initializing\n";
            } else if (method == "shutdown") {
                shutdown_received = true;
                send_response(out, message["id"], nullptr);
            } else if (method == "exit") {
                std::cerr << "mirage-lsp: exit received (shutdown_received=" << shutdown_received << ")\n";
                return shutdown_received ? 0 : 1;
            } else if (method == "$/cancelRequest") {
                // Single-threaded server: every request is answered synchronously
                // before the next message is read, so by the time a cancellation
                // could arrive the response has already been sent. Nothing to do.
            } else if (method == "textDocument/didOpen") {
                const auto &doc = message["params"]["textDocument"];
                const auto uri = doc["uri"].get<std::string>();
                const auto path = canonical_path_of(uri);
                documents_.open(path, doc["text"].get<std::string>());
                publish_diagnostics(path);
            } else if (method == "textDocument/didChange") {
                const auto uri = message["params"]["textDocument"]["uri"].get<std::string>();
                const auto path = canonical_path_of(uri);
                // Full sync (textDocumentSync=1): exactly one change, containing
                // the entire new buffer contents.
                const auto &changes = message["params"]["contentChanges"];
                documents_.update(path, changes.back()["text"].get<std::string>());
                publish_diagnostics(path);
            } else if (method == "textDocument/didClose") {
                const auto uri = message["params"]["textDocument"]["uri"].get<std::string>();
                const auto path = canonical_path_of(uri);
                documents_.close(path);
                send_notification(out, "textDocument/publishDiagnostics", {
                    {"uri", uri},
                    {"diagnostics", json::array()},
                });
            } else if (method == "textDocument/hover") {
                const auto uri = message["params"]["textDocument"]["uri"].get<std::string>();
                const auto path = canonical_path_of(uri);
                const auto module_path = ast::canonicalize(std::filesystem::path(path).parent_path().string());
                auto &result = documents_.ensure_analysed(path);
                const size_t line = message["params"]["position"]["line"].get<size_t>() + 1;
                const size_t character = message["params"]["position"]["character"].get<size_t>() + 1;
                send_response(out, message["id"], handlers::handle_hover(result, module_path, path, line, character));
            } else if (method == "textDocument/definition") {
                const auto uri = message["params"]["textDocument"]["uri"].get<std::string>();
                const auto path = canonical_path_of(uri);
                const auto module_path = ast::canonicalize(std::filesystem::path(path).parent_path().string());
                auto &result = documents_.ensure_analysed(path);
                const size_t line = message["params"]["position"]["line"].get<size_t>() + 1;
                const size_t character = message["params"]["position"]["character"].get<size_t>() + 1;
                send_response(out, message["id"], handlers::handle_definition(result, module_path, path, line, character));
            } else {
                std::cerr << "mirage-lsp: unhandled method '" << method << "'\n";
                if (has_id) {
                    send_error(out, message["id"], METHOD_NOT_FOUND, "method not found: " + method);
                }
            }
        }
    }
}
