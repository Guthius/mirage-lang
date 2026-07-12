#pragma once

#include "analysis.hpp"

#include <istream>
#include <ostream>

namespace lsp {
    class Server {
      public:
        // Runs the read-dispatch-write loop until the client disconnects or
        // sends `exit`. Returns the process exit code (0 if `shutdown` was
        // received first, 1 otherwise, per the LSP spec).
        auto run(std::istream &in, std::ostream &out) -> int;

      private:
        analysis::DocumentStore documents_;
    };
}
