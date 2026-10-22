#include "icelib/core.hpp"
#include <iostream>
int main(int argc, char **argv) {
    if (argc > 1 && (std::string(argv[1]) == "--version" || std::string(argv[1]) == "version")) {
        std::cout << ice::version() << "\n";
        return 0;
    }
    // TODO: implement the MCP JSON-RPC server (see TODO.md Phase 7). For now fail
    // loudly instead of pretending to serve requests.
    std::cout << "ice-mcp " << ice::version() << "\nUsage: ice-mcp [--help|--version]\n";
    std::cerr << "ice-mcp server is not implemented yet\n";
    return 1;
}
