#include "server.hpp"

#include <iostream>

auto main() -> int {
    lsp::Server server;
    return server.run(std::cin, std::cout);
}
