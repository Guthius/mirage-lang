#pragma once

#include <istream>
#include <mutex>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

namespace lsp::transport {
    // Blocks on `in` until one Content-Length-framed LSP message is available,
    // then returns its raw JSON body (unparsed). Returns std::nullopt on EOF,
    // a malformed header, or any other stream error - the caller should treat
    // that as "the client went away" and shut down.
    auto read_message(std::istream &in) -> std::optional<std::string>;

    // Writes `body` to `out` framed as "Content-Length: N\r\n\r\n<body>" and
    // flushes immediately. Editors block waiting for responses, so an
    // unflushed write is the most common cause of an LSP server appearing to
    // hang - every call to this function flushes unconditionally.
    void write_message(std::ostream &out, std::string_view body);

    // Synchronizes writes to `out` across threads. Once analysis runs on a
    // background worker thread (see Server), both it and the main
    // stdin-reading thread can produce outbound JSON-RPC traffic concurrently
    // (lifecycle responses from the main thread; everything else from the
    // worker), and `write_message`'s underlying ostream is not itself
    // thread-safe - every send must go through one shared OutputChannel.
    class OutputChannel {
      public:
        explicit OutputChannel(std::ostream &out) : out_(out) {
        }

        void send(std::string_view body) {
            const std::lock_guard lock(mutex_);
            write_message(out_, body);
        }

      private:
        std::ostream &out_;
        std::mutex mutex_;
    };
}
