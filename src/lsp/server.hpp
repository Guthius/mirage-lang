#pragma once

#include <istream>
#include <ostream>

namespace lsp {
    class Server {
      public:
        // Runs the read-dispatch-write loop until the client disconnects or
        // sends `exit`. Returns the process exit code (0 if `shutdown` was
        // received first, 1 otherwise, per the LSP spec).
        //
        // The stdin-reading thread (this call) never touches the compiler or
        // DocumentStore directly - it only handles pure-protocol lifecycle
        // messages (initialize/initialized/shutdown/exit/$cancelRequest)
        // inline and otherwise hands work to a single background analysis
        // worker thread via a task queue, so a slow or large analysis can
        // never block it from reading the next message or answering
        // $/cancelRequest.
        auto run(std::istream &in, std::ostream &out) -> int;
    };
}
