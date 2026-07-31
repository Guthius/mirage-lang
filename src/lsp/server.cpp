#include "server.hpp"

#include "analysis.hpp"
#include "compiler/module_resolver.hpp"
#include "handlers/completion.hpp"
#include "handlers/definition.hpp"
#include "handlers/diagnostics.hpp"
#include "handlers/hover.hpp"
#include "handlers/inlay_hint.hpp"
#include "handlers/references.hpp"
#include "task_queue.hpp"
#include "transport.hpp"
#include "uri.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <mutex>
#include <ranges>
#include <set>
#include <stop_token>
#include <thread>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include <iostream>

namespace lsp {
    namespace {
        using json = nlohmann::json;
        using namespace std::chrono_literals;

        enum class LifecycleState : uint8_t {
            Uninitialized,
            Running,
        };

        constexpr int INVALID_REQUEST = -32600;
        constexpr int METHOD_NOT_FOUND = -32601;
        constexpr int INTERNAL_ERROR = -32603;
        constexpr int SERVER_NOT_INITIALIZED = -32002;
        constexpr int REQUEST_CANCELLED = -32800;

        // How long a burst of didChange notifications for the same file is allowed to
        // coalesce before diagnostics are republished for it. Only diagnostics are
        // debounced this way - hover/definition/etc. run as soon as the worker reaches
        // them in the FIFO queue, never delayed.
        constexpr auto DIAGNOSTICS_DEBOUNCE = 200ms;

        // How long the worker blocks waiting for a task before it wakes up anyway to
        // sweep for due diagnostics-republish deadlines.
        constexpr auto WORKER_POLL_INTERVAL = 50ms;

        void send(transport::OutputChannel &out, json message) {
            message["jsonrpc"] = "2.0";
            out.send(message.dump());
        }

        void send_response(transport::OutputChannel &out, const json &id, json result) {
            send(out, {{"id", id}, {"result", std::move(result)}});
        }

        void send_error(transport::OutputChannel &out, const json &id, const int code, std::string message_text) {
            send(out, {{"id", id}, {"error", {{"code", code}, {"message", std::move(message_text)}}}});
        }

        void send_notification(transport::OutputChannel &out, std::string method, json params) {
            send(out, {{"method", std::move(method)}, {"params", std::move(params)}});
        }

        // A request FROM the server. The client's response carries no "method", and the
        // dispatch loop already skips those, so the reply is harmlessly ignored -- which is
        // fine for the one request this server makes (registering a file watcher): there is
        // nothing to do on success, and nothing useful to do on failure either.
        void send_request(transport::OutputChannel &out, const json &id, std::string method, json params) {
            send(out, {{"id", id}, {"method", std::move(method)}, {"params", std::move(params)}});
        }

        // Derives the canonical filesystem path this analysis pipeline keys
        // everything by, from a client-supplied "file://..." URI.
        // Returns "" for a URI this server cannot handle, rather than feeding an empty path
        // into ast::canonicalize and the std::filesystem calls downstream. Editors do send
        // non-file schemes -- 'untitled:' for an unsaved buffer is the common one.
        auto canonical_path_of(const std::string &uri) -> std::string {
            auto path = uri_to_path(uri);
            if (path.empty()) {
                std::cerr << "mirage-lsp: ignoring unsupported document URI '" << uri << "'\n";
                return {};
            }
            return ast::canonicalize(path);
        }

        // JSON-RPC ids are either a number or a string; normalize to a string so they
        // can key an ordinary unordered_map.
        auto id_key_of(const json &id) -> std::string {
            return id.dump();
        }

        // Thread-safe handoff of "this request id's task has finished running" from the
        // worker thread back to the main thread, which is the only thread allowed to
        // touch Server::run's in_flight_ map. Without this, in_flight_ would grow by one
        // entry per request for the life of the connection - small per-entry, but exactly
        // the kind of unbounded-over-a-long-session growth this workstream exists to
        // eliminate elsewhere.
        class CompletedRequests {
          public:
            void mark_done(std::string id_key) {
                const std::lock_guard lock(mutex_);
                done_.push_back(std::move(id_key));
            }

            auto drain() -> std::vector<std::string> {
                const std::lock_guard lock(mutex_);
                return std::exchange(done_, {});
            }

          private:
            std::mutex mutex_;
            std::vector<std::string> done_;
        };

        // Owns the only DocumentStore instance and runs exclusively on the analysis
        // worker thread - every Task pushed onto the queue that touches `documents` or
        // diagnostics bookkeeping is executed by Worker::run, never by the stdin thread.
        // This is what lets DocumentStore itself stay lock-free: there is never more than
        // one thread that could be touching it.
        class Worker {
          public:
            explicit Worker(transport::OutputChannel &out) : out_(out) {
            }

            analysis::DocumentStore documents;

            // The client's workspace root, from initialize's rootUri/workspaceFolders. Empty
            // when the client sent neither, in which case Find All References stays scoped to
            // the analysed module's import closure — the pre-workspace behaviour.
            std::string workspace_root;

            // Coalesces rapid didOpen/didChange bursts for the same file into one
            // publishDiagnostics round, DIAGNOSTICS_DEBOUNCE after the last edit.
            void schedule_diagnostics(const std::string &path) {
                pending_diag_deadlines_[path] = std::chrono::steady_clock::now() + DIAGNOSTICS_DEBOUNCE;
            }

            // Publishes diagnostics for `path`'s module immediately (used for didOpen's
            // very first analysis, so a freshly opened file's squiggles don't wait out a
            // full debounce window with nothing having changed yet).
            void publish_diagnostics_now(const std::string &path) {
                pending_diag_deadlines_.erase(path);
                do_publish_diagnostics(path);
            }

            // Called by the worker loop on every wakeup (task arrival or poll timeout) to
            // move any past-deadline pending paths into an actual publish.
            void sweep_due_diagnostics() {
                const auto now = std::chrono::steady_clock::now();
                for (auto it = pending_diag_deadlines_.begin(); it != pending_diag_deadlines_.end();) {
                    if (it->second <= now) {
                        const auto path = it->first;
                        it = pending_diag_deadlines_.erase(it);
                        do_publish_diagnostics(path);
                    } else {
                        ++it;
                    }
                }
            }

            // Diagnostics for `path` as still seen from the documents that remain open.
            //
            // Called after a didClose, to decide whether the closed file's squiggles should
            // survive. Empty in the ordinary case -- no remaining open document's import
            // closure includes it -- which reproduces the old unconditional clear exactly.
            //
            // The closed file is read from DISK by this analysis, its buffer having just been
            // dropped, which is the point: what survives is precisely the on-disk errors, not
            // anything derived from the editor's copy.
            auto closure_diagnostics_for(const std::string &path) -> json {
                for (const auto &open_path : documents.open_paths()) {
                    auto &result = documents.ensure_analysed(open_path);
                    auto grouped = handlers::group_diagnostics_by_file(result.diagnostics);
                    if (const auto it = grouped.find(path); it != grouped.end() && !it->second.empty()) {
                        return std::move(it->second);
                    }
                }
                return json::array();
            }

          private:
            // After analysing `path`, publishes diagnostics for every file the analysis
            // touched, plus always for `path` itself (even if empty, so fixing the last
            // error in the currently-edited file clears its squiggles), plus an explicit
            // empty entry for any file that had non-empty diagnostics in a previous round
            // but dropped out of this one (see DocumentStore::files_that_became_clean).
            void do_publish_diagnostics(const std::string &path) {
                auto &result = documents.ensure_analysed(path);
                auto grouped = handlers::group_diagnostics_by_file(result.diagnostics);
                if (!grouped.contains(path)) {
                    grouped[path] = {};
                }

                std::set<std::string> current_nonempty;
                for (const auto &[file, diags] : grouped) {
                    if (!diags.empty()) current_nonempty.insert(file);
                }
                std::set<std::string> closure_dirs;
                for (const auto &dir : result.ast_program.modules | std::views::keys) {
                    closure_dirs.insert(dir);
                }
                for (const auto &stale_file : documents.files_that_became_clean(closure_dirs, current_nonempty)) {
                    grouped[stale_file];
                }

                for (auto &[filename, diags] : grouped) {
                    send_notification(out_, "textDocument/publishDiagnostics", {
                        {"uri", path_to_uri(filename)},
                        {"diagnostics", std::move(diags)},
                    });
                }
            }

            transport::OutputChannel &out_;
            std::unordered_map<std::string, std::chrono::steady_clock::time_point> pending_diag_deadlines_;
        };

        // Recovers the JSON-RPC id from a Task's id_key (which is id.dump() — see
        // id_key_of) and answers the client with an error. Without this, a request whose
        // task threw was logged and marked done but never answered, leaving the editor's
        // hover/definition/references promise pending forever.
        void answer_failed_task(transport::OutputChannel &out, const std::string &id_key, const int code, const std::string &message_text) {
            const auto id = json::parse(id_key, nullptr, /*allow_exceptions=*/false);
            if (id.is_discarded()) {
                std::cerr << "mirage-lsp: cannot answer failed task; unparsable id " << id_key << "\n";
                return;
            }
            send_error(out, id, code, message_text);
        }

        void run_worker_loop(std::stop_token stop, TaskQueue &queue, Worker &worker, CompletedRequests &completed, transport::OutputChannel &out) {
            while (!stop.stop_requested()) {
                worker.sweep_due_diagnostics();

                auto task = queue.pop(WORKER_POLL_INTERVAL);
                if (stop.stop_requested()) {
                    return;
                }
                if (!task) {
                    continue; // poll timeout - loop back around to sweep again
                }
                if (task->cancelled && task->cancelled->load()) {
                    // LSPCORE-4: mark_done even though the task never ran. in_flight_ entries
                    // are only erased via CompletedRequests::drain, which is fed exclusively
                    // from run_cancellable_request -- and that never runs for a task cancelled
                    // before the worker reached it. Fast cursor movement during hover leaked one
                    // entry per superseded request, forever, which is exactly the unbounded
                    // growth CompletedRequests exists to prevent.
                    //
                    // Per the spec a cancelled request is still ANSWERED, with
                    // ErrorCodes.RequestCancelled — a silent drop left stricter clients'
                    // promises pending forever.
                    if (!task->id_key.empty()) {
                        answer_failed_task(out, task->id_key, REQUEST_CANCELLED, "request cancelled");
                        completed.mark_done(task->id_key);
                    }
                    continue; // caller already moved on; skip without doing any work
                }
                // This runs on a std::jthread. An exception escaping here does not merely fail
                // the request -- it calls std::terminate directly, killing the server. The task
                // bodies call into analyse()/sema::check_program with no try/catch anywhere
                // above them, so any .at() miss or bad std::get in the compiler front end
                // reaches this point.
                try {
                    task->run();
                } catch (const std::exception &e) {
                    std::cerr << "mirage-lsp: worker task failed: " << e.what() << "\n";
                    if (!task->id_key.empty()) {
                        answer_failed_task(out, task->id_key, INTERNAL_ERROR, std::string("internal error: ") + e.what());
                        completed.mark_done(task->id_key);
                    }
                } catch (...) {
                    std::cerr << "mirage-lsp: worker task failed with a non-standard exception\n";
                    if (!task->id_key.empty()) {
                        answer_failed_task(out, task->id_key, INTERNAL_ERROR, "internal error handling request");
                        completed.mark_done(task->id_key);
                    }
                }
            }
        }

        // Runs `compute` unless already cancelled, sends its result as the response for
        // `id` unless cancellation happened while it ran, and always reports completion
        // via `completed` so the main thread can stop tracking this request. This is
        // cooperative-coarse cancellation, not preemption: `compute` (which ends up
        // calling into sema::check_program) has no cancellation checkpoints of its own,
        // so a request already mid-analysis when cancelled still runs to completion here
        // with its result simply discarded - the worst case is wasted background CPU on
        // a superseded computation, never a hang or a main-thread stall.
        void run_cancellable_request(const std::shared_ptr<std::atomic<bool>> &cancelled, CompletedRequests &completed,
                                      const std::string &id_key, transport::OutputChannel &out, const json &id,
                                      const std::function<json()> &compute) {
            if (cancelled->load()) {
                send_error(out, id, REQUEST_CANCELLED, "request cancelled");
                completed.mark_done(id_key);
                return;
            }

            auto result = compute();

            if (cancelled->load()) {
                send_error(out, id, REQUEST_CANCELLED, "request cancelled");
                completed.mark_done(id_key);
                return;
            }

            send_response(out, id, std::move(result));
            completed.mark_done(id_key);
        }
    }

    auto Server::run(std::istream &in, std::ostream &out) -> int {
        transport::OutputChannel channel(out);
        TaskQueue queue;
        Worker worker(channel);
        CompletedRequests completed;

        std::jthread worker_thread([&](const std::stop_token stop) { run_worker_loop(stop, queue, worker, completed, channel); });

        auto state = LifecycleState::Uninitialized;
        bool shutdown_received = false;
        // Set from the client's initialize capabilities; consulted on 'initialized'.
        bool supports_watch_registration = false;
        std::unordered_map<std::string, std::shared_ptr<std::atomic<bool>>> in_flight_;

        while (true) {
            for (const auto &id_key : completed.drain()) {
                in_flight_.erase(id_key);
            }

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

            // Extracted defensively: this runs OUTSIDE the try below, and
            // message.value("method", ...) throws when the field exists with a non-string
            // type ('{"method": 42}') — one malformed message used to std::terminate the
            // whole server.
            std::string method;
            if (const auto it = message.find("method"); it != message.end()) {
                if (!it->is_string()) {
                    std::cerr << "mirage-lsp: message with non-string 'method', rejecting\n";
                    if (message.contains("id") && !message["id"].is_null()) {
                        send_error(channel, message["id"], INVALID_REQUEST, "'method' must be a string");
                    }
                    continue;
                }
                method = it->get<std::string>();
            }
            const bool has_id = message.contains("id") && !message["id"].is_null();

            std::cerr << "mirage-lsp: <- " << (method.empty() ? "(response)" : method) << "\n";

            if (method.empty()) {
                // A message with no "method" is a response to a request WE sent, not
                // something to dispatch. The only such request is the file-watcher
                // registration on 'initialized', and there is nothing to do with its reply
                // either way, so dropping it is the whole handling.
                continue;
            }

            if (state == LifecycleState::Uninitialized && method != "initialize" && method != "exit") {
                if (has_id) {
                    send_error(channel, message["id"], SERVER_NOT_INITIALIZED, "server not initialized");
                }
                continue;
            }

            // Per the LSP spec, once 'shutdown' has been received the only further message
            // that may be honoured is 'exit'; anything else must be answered with
            // InvalidRequest. shutdown_received was tracked but never consulted by dispatch,
            // so a client that kept sending after shutdown was served as if nothing had
            // happened -- including requests that queue real analysis onto a worker the server
            // is about to stop.
            if (shutdown_received && method != "exit") {
                std::cerr << "mirage-lsp: '" << method << "' received after shutdown, rejecting\n";
                if (has_id) {
                    send_error(channel, message["id"], INVALID_REQUEST, "server is shutting down");
                }
                continue;
            }

            // Everything below reads request fields with unchecked chained access --
            // message["params"]["position"]["line"].get<size_t>() and friends. nlohmann's
            // non-const operator[] auto-vivifies a missing key to null, and .get<> on null
            // throws; nothing between here and main() caught it, so one malformed request --
            // a hover with no "position", say -- terminated the process and took down every
            // open file, not just that request.
            //
            // A failed request becomes a JSON-RPC error response when it has an id, so the
            // client learns the call failed instead of waiting forever; a malformed
            // notification is logged and dropped, since there is nobody to answer.
            try {

            if (method == "initialize") {
                state = LifecycleState::Running;
                // Whether the client will accept a watcher registered at runtime. Read
                // defensively: every level is optional, and nlohmann's operator[] would
                // auto-vivify a missing key rather than report it.
                if (const auto &params = message["params"]; params.is_object()) {
                    const auto &client_caps = params.contains("capabilities") ? params["capabilities"] : json::object();
                    const auto &workspace = client_caps.is_object() && client_caps.contains("workspace")
                        ? client_caps["workspace"] : json::object();
                    const auto &watched = workspace.is_object() && workspace.contains("didChangeWatchedFiles")
                        ? workspace["didChangeWatchedFiles"] : json::object();
                    supports_watch_registration = watched.is_object() && watched.value("dynamicRegistration", false);

                    // The root to search for references. 'workspaceFolders' supersedes the
                    // deprecated 'rootUri', which supersedes the older 'rootPath'; take the
                    // first folder, since a reference search needs one tree to walk.
                    std::string root;
                    if (params.contains("workspaceFolders") && params["workspaceFolders"].is_array() &&
                        !params["workspaceFolders"].empty()) {
                        const auto &first = params["workspaceFolders"][0];
                        if (first.is_object() && first.contains("uri") && first["uri"].is_string()) {
                            root = canonical_path_of(first["uri"].get<std::string>());
                        }
                    }
                    if (root.empty() && params.contains("rootUri") && params["rootUri"].is_string()) {
                        root = canonical_path_of(params["rootUri"].get<std::string>());
                    }
                    if (root.empty() && params.contains("rootPath") && params["rootPath"].is_string()) {
                        root = params["rootPath"].get<std::string>();
                    }
                    worker.workspace_root = root;
                }
                const json capabilities = {
                    {"textDocumentSync", {{"openClose", true}, {"change", 1}}},
                    {"hoverProvider", true},
                    {"definitionProvider", true},
                    {"completionProvider", {{"triggerCharacters", {"."}}}},
                    {"inlayHintProvider", true},
                    // Workspace-wide when the client supplied a root (see Worker::
                    // workspace_root); scoped to the analysed module's import closure when it
                    // did not, since there is then no tree to search.
                    {"referencesProvider", true},
                };
                send_response(channel, message["id"], {{"capabilities", capabilities}});
            } else if (method == "initialized") {
                std::cerr << "mirage-lsp: client finished initializing\n";
                // Ask to be told about '.mir' files changed outside the editor. Registering
                // here rather than in the initialize response is what the spec requires:
                // the client is only ready to receive server-initiated requests once it has
                // sent 'initialized'.
                //
                // A client without dynamicRegistration simply never sends
                // didChangeWatchedFiles, and analysis stays editor-driven as before. That is
                // a degradation, not a failure, so it is not worth an error.
                if (supports_watch_registration) {
                    send_request(channel, "mirage-watch-registration", "client/registerCapability", {
                        {"registrations", json::array({
                            {
                                {"id", "mirage-watched-files"},
                                {"method", "workspace/didChangeWatchedFiles"},
                                {"registerOptions", {{"watchers", json::array({{{"globPattern", "**/*.mir"}}})}}},
                            },
                        })},
                    });
                } else {
                    std::cerr << "mirage-lsp: client does not support dynamic file-watcher "
                                 "registration; external changes will not invalidate analysis\n";
                }
            } else if (method == "shutdown") {
                shutdown_received = true;
                send_response(channel, message["id"], nullptr);
            } else if (method == "exit") {
                std::cerr << "mirage-lsp: exit received (shutdown_received=" << shutdown_received << ")\n";
                worker_thread.request_stop();
                return shutdown_received ? 0 : 1;
            } else if (method == "$/cancelRequest") {
                const auto id_key = id_key_of(message["params"]["id"]);
                if (const auto it = in_flight_.find(id_key); it != in_flight_.end()) {
                    it->second->store(true);
                }
            } else if (method == "textDocument/didOpen") {
                const auto &doc = message["params"]["textDocument"];
                auto path = canonical_path_of(doc["uri"].get<std::string>());
                // canonical_path_of returns "" for URIs this server cannot handle
                // ('untitled:' buffers); a phantom "" document must not be opened,
                // analysed, or given diagnostics for the nonsense URI 'file://'.
                if (path.empty()) continue;
                auto text = doc["text"].get<std::string>();
                queue.push(Task{nullptr, [&worker, path, text = std::move(text)]() mutable {
                    worker.documents.open(path, std::move(text));
                    worker.publish_diagnostics_now(path);
                }});
            } else if (method == "textDocument/didChange") {
                auto path = canonical_path_of(message["params"]["textDocument"]["uri"].get<std::string>());
                if (path.empty()) continue;
                // Full sync (textDocumentSync=1): exactly one change, containing
                // the entire new buffer contents.
                //
                // The emptiness check is not defensive padding. nlohmann's back() decrements
                // end() with no bounds check and does not throw, so 'contentChanges: []'
                // dereferenced past the start of the array and took the whole server down with
                // SIGSEGV -- a crash that no amount of try/catch around message dispatch could
                // have caught, which is why this is fixed before LSPCORE-1 rather than after.
                const auto &changes = message["params"]["contentChanges"];
                if (!changes.is_array() || changes.empty()) {
                    std::cerr << "mirage-lsp: didChange with no content changes, ignoring\n";
                    continue;
                }
                auto text = changes.back()["text"].get<std::string>();
                queue.push(Task{nullptr, [&worker, path, text = std::move(text)]() mutable {
                    worker.documents.update(path, std::move(text));
                    worker.schedule_diagnostics(path);
                }});
            } else if (method == "workspace/didChangeWatchedFiles") {
                // Files changed outside the editor: a git pull, a checkout, a generated file.
                // Without this the cached analysis is only ever invalidated by editor
                // notifications, so an edited dependency left stale diagnostics and hover
                // indefinitely -- bounded only by the unrelated MAX_CACHED_MODULES LRU.
                const auto &changes = message["params"]["changes"];
                if (!changes.is_array() || changes.empty()) {
                    continue;
                }

                std::vector<std::string> changed;
                for (const auto &change : changes) {
                    if (!change.contains("uri")) continue;
                    auto changed_path = canonical_path_of(change["uri"].get<std::string>());
                    if (!changed_path.empty()) changed.push_back(std::move(changed_path));
                }
                if (changed.empty()) {
                    continue;
                }

                queue.push(Task{nullptr, [&worker, changed = std::move(changed)] {
                    for (const auto &path : changed) {
                        worker.documents.invalidate_external(path);
                    }
                    // Re-publish for every open document. Only the bundles covering a changed
                    // file were dropped, so an unaffected document just re-reads its cached
                    // analysis; and an open file's own diagnostics can move because of a
                    // change to a file it imports, which is the whole point. Debounced rather
                    // than immediate, since a branch switch arrives as a burst.
                    for (const auto &path : worker.documents.open_paths()) {
                        worker.schedule_diagnostics(path);
                    }
                }});
            } else if (method == "textDocument/didClose") {
                // The spec's guidance is that a server clears diagnostics it computed from an
                // in-memory buffer once that buffer closes, and this used to do exactly that
                // -- unconditionally.
                //
                // That is right for BUFFER-derived diagnostics and wrong for the rest
                // (LSPCORE-7). A closed file that still has on-disk errors and stays inside an
                // open module's import closure is still being analysed on every round; only
                // its squiggles vanished, until some unrelated edit re-touched the closure.
                //
                // Rather than track the provenance of each published diagnostic, the two cases
                // are separated by re-deriving from what is still open: whatever the remaining
                // open documents' closures say about this file is what it keeps. Nothing open
                // covers it -> empty, which is the old behaviour and the common case.
                const auto uri = message["params"]["textDocument"]["uri"].get<std::string>();
                auto path = canonical_path_of(uri);
                if (path.empty()) continue;
                queue.push(Task{nullptr, [&worker, &channel, path, uri]() {
                    worker.documents.close(path);
                    send_notification(channel, "textDocument/publishDiagnostics", {
                        {"uri", uri},
                        {"diagnostics", worker.closure_diagnostics_for(path)},
                    });
                }});
            } else if (method == "textDocument/hover" || method == "textDocument/definition") {
                const auto id = message["id"];
                const auto id_key = id_key_of(id);
                auto path = canonical_path_of(message["params"]["textDocument"]["uri"].get<std::string>());
                // An unsupported URI ('untitled:' buffer) has nothing to analyse; a null
                // result is the spec's "no answer here", and skipping without one would
                // leave the client's promise pending.
                if (path.empty()) {
                    send_response(channel, id, nullptr);
                    continue;
                }
                const auto line = message["params"]["position"]["line"].get<size_t>() + 1;
                const auto character = message["params"]["position"]["character"].get<size_t>() + 1;

                auto cancelled = std::make_shared<std::atomic<bool>>(false);
                // Reusing an id while the previous request with it is still in flight is a
                // protocol violation, but assigning over the entry silently orphaned the older
                // request's cancellation flag, so a later $/cancelRequest for that id would
                // cancel the wrong one. Report rather than misattribute quietly.
                if (in_flight_.contains(id_key)) {
                    std::cerr << "mirage-lsp: request id " << id_key
                              << " reused while still in flight; cancellation may be misattributed\n";
                }
                in_flight_[id_key] = cancelled;

                // A full queue means the single worker has fallen far behind. Answer now
                // rather than leave the client waiting on a request that was never accepted.
                if (queue.full()) {
                    std::cerr << "mirage-lsp: task queue full, rejecting '" << method << "'\n";
                    in_flight_.erase(id_key);
                    send_error(channel, id, INTERNAL_ERROR, "server busy: task queue full");
                    continue;
                }

                const bool is_hover = method == "textDocument/hover";
                queue.push(Task{cancelled, [&worker, &channel, &completed, cancelled, id, id_key, path, line, character, is_hover] {
                    run_cancellable_request(cancelled, completed, id_key, channel, id, [&]() -> json {
                        const auto module_path = ast::canonicalize(std::filesystem::path(path).parent_path().string());
                        auto &result = worker.documents.ensure_analysed(path);
                        return is_hover ? handlers::handle_hover(result, module_path, path, line, character)
                                        : handlers::handle_definition(result, module_path, path, line, character);
                    });
                }, id_key});
            } else if (method == "textDocument/completion") {
                const auto id = message["id"];
                const auto id_key = id_key_of(id);
                auto path = canonical_path_of(message["params"]["textDocument"]["uri"].get<std::string>());
                if (path.empty()) {
                    send_response(channel, id, nullptr);
                    continue;
                }
                const auto line = message["params"]["position"]["line"].get<size_t>() + 1;
                const auto character = message["params"]["position"]["character"].get<size_t>() + 1;

                auto cancelled = std::make_shared<std::atomic<bool>>(false);
                // Reusing an id while the previous request with it is still in flight is a
                // protocol violation, but assigning over the entry silently orphaned the older
                // request's cancellation flag, so a later $/cancelRequest for that id would
                // cancel the wrong one. Report rather than misattribute quietly.
                if (in_flight_.contains(id_key)) {
                    std::cerr << "mirage-lsp: request id " << id_key
                              << " reused while still in flight; cancellation may be misattributed\n";
                }
                in_flight_[id_key] = cancelled;

                // A full queue means the single worker has fallen far behind. Answer now
                // rather than leave the client waiting on a request that was never accepted.
                if (queue.full()) {
                    std::cerr << "mirage-lsp: task queue full, rejecting '" << method << "'\n";
                    in_flight_.erase(id_key);
                    send_error(channel, id, INTERNAL_ERROR, "server busy: task queue full");
                    continue;
                }

                queue.push(Task{cancelled, [&worker, &channel, &completed, cancelled, id, id_key, path, line, character] {
                    run_cancellable_request(cancelled, completed, id_key, channel, id, [&]() -> json {
                        const auto module_path = ast::canonicalize(std::filesystem::path(path).parent_path().string());
                        auto &result = worker.documents.ensure_analysed(path);
                        return handlers::handle_completion(result, module_path, path, line, character);
                    });
                }, id_key});
            } else if (method == "textDocument/references") {
                const auto id = message["id"];
                const auto id_key = id_key_of(id);
                auto path = canonical_path_of(message["params"]["textDocument"]["uri"].get<std::string>());
                if (path.empty()) {
                    send_response(channel, id, nullptr);
                    continue;
                }
                const auto line = message["params"]["position"]["line"].get<size_t>() + 1;
                const auto character = message["params"]["position"]["character"].get<size_t>() + 1;
                // 'context' is required by the spec, but tolerate its absence rather than
                // throwing on a client that omits it; including the declaration is the more
                // useful default.
                const auto include_declaration = message["params"].contains("context")
                    ? message["params"]["context"].value("includeDeclaration", true)
                    : true;

                auto cancelled = std::make_shared<std::atomic<bool>>(false);
                if (in_flight_.contains(id_key)) {
                    std::cerr << "mirage-lsp: request id " << id_key
                              << " reused while still in flight; cancellation may be misattributed\n";
                }
                in_flight_[id_key] = cancelled;

                if (queue.full()) {
                    std::cerr << "mirage-lsp: task queue full, rejecting '" << method << "'\n";
                    in_flight_.erase(id_key);
                    send_error(channel, id, INTERNAL_ERROR, "server busy: task queue full");
                    continue;
                }

                queue.push(Task{cancelled, [&worker, &channel, &completed, cancelled, id, id_key, path, line, character, include_declaration] {
                    run_cancellable_request(cancelled, completed, id_key, channel, id, [&]() -> json {
                        const auto module_path = ast::canonicalize(std::filesystem::path(path).parent_path().string());
                        auto &result = worker.documents.ensure_analysed(path);

                        // Discovered per request rather than cached: files appear and vanish
                        // between searches, and a stale module list silently misses references.
                        // The walk is filesystem-bound and dwarfed by analysing what it finds.
                        handlers::WorkspaceSearch workspace{
                            .module_dirs = analysis::discover_module_dirs(worker.workspace_root),
                            .analyse = [&worker](const std::string &dir) { return worker.documents.analyse_uncached(dir); },
                        };
                        const auto *search = workspace.module_dirs.empty() ? nullptr : &workspace;
                        return handlers::handle_references(result, module_path, path, line, character, include_declaration, search);
                    });
                }, id_key});
            } else if (method == "textDocument/inlayHint") {
                const auto id = message["id"];
                const auto id_key = id_key_of(id);
                auto path = canonical_path_of(message["params"]["textDocument"]["uri"].get<std::string>());
                if (path.empty()) {
                    send_response(channel, id, nullptr);
                    continue;
                }
                const auto start_line = message["params"]["range"]["start"]["line"].get<size_t>() + 1;
                const auto end_line = message["params"]["range"]["end"]["line"].get<size_t>() + 1;

                auto cancelled = std::make_shared<std::atomic<bool>>(false);
                // Reusing an id while the previous request with it is still in flight is a
                // protocol violation, but assigning over the entry silently orphaned the older
                // request's cancellation flag, so a later $/cancelRequest for that id would
                // cancel the wrong one. Report rather than misattribute quietly.
                if (in_flight_.contains(id_key)) {
                    std::cerr << "mirage-lsp: request id " << id_key
                              << " reused while still in flight; cancellation may be misattributed\n";
                }
                in_flight_[id_key] = cancelled;

                // A full queue means the single worker has fallen far behind. Answer now
                // rather than leave the client waiting on a request that was never accepted.
                if (queue.full()) {
                    std::cerr << "mirage-lsp: task queue full, rejecting '" << method << "'\n";
                    in_flight_.erase(id_key);
                    send_error(channel, id, INTERNAL_ERROR, "server busy: task queue full");
                    continue;
                }

                queue.push(Task{cancelled, [&worker, &channel, &completed, cancelled, id, id_key, path, start_line, end_line] {
                    run_cancellable_request(cancelled, completed, id_key, channel, id, [&]() -> json {
                        const auto module_path = ast::canonicalize(std::filesystem::path(path).parent_path().string());
                        auto &result = worker.documents.ensure_analysed(path);
                        return handlers::handle_inlay_hint(result, module_path, path, start_line, end_line);
                    });
                }, id_key});
            } else {
                std::cerr << "mirage-lsp: unhandled method '" << method << "'\n";
                if (has_id) {
                    send_error(channel, message["id"], METHOD_NOT_FOUND, "method not found: " + method);
                }
            }
            } catch (const std::exception &e) {
                std::cerr << "mirage-lsp: error handling '" << method << "': " << e.what() << "\n";
                if (has_id) {
                    send_error(channel, message["id"], INTERNAL_ERROR,
                               std::string("error handling ") + method + ": " + e.what());
                }
            }
        }
    }
}
