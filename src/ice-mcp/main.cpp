#include "icelib/core.hpp"
#include <iostream>
int main(int argc, char** argv) {
    if (argc > 1 && (std::string(argv[1]) == "--version" || std::string(argv[1]) == "version")) {
        std::cout << ice::version() << "\n";
        return 0;
    }
    std::cout << "ice-mcp " << ice::version() << "\nUsage: ice-mcp [--help|--version]\n";
    return 0;
}
