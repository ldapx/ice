#include "icelib/core.hpp"
#include "icelib/packages.hpp"
#include <iostream>
#include <filesystem>
int main(int argc, char** argv) {
    if (argc > 1 && (std::string(argv[1]) == "--version" || std::string(argv[1]) == "version")) {
        std::cout << ice::version() << "\n";
        return 0;
    }
    if (argc < 2 || std::string(argv[1]) == "--help") {
        std::cout << "ice-pkg " << ice::version()
                  << "\nUsage: ice-pkg <manifest|hash|install|resolve|verify> ...\n";
        return 0;
    }
    try {
        std::string command = argv[1];
        if (command == "manifest" && argc == 3) {
            auto package = ice::PackageManager::readManifest(argv[2]);
            std::cout << package.name << " " << package.version << "\n";
            for (const auto& capability : ice::PackageManager::capabilities(package))
                std::cout << "capability: " << capability << "\n";
            return 0;
        }
        if (command == "hash" && argc == 3) {
            std::cout << ice::PackageManager::contentHash(argv[2]) << "\n";
            return 0;
        }
        if (command == "install" && (argc == 6 || argc == 7)) {
            const std::string revision = argc == 7 ? argv[6] : std::string{};
            ice::PackageManager::installGit(argv[2], argv[3], argv[4], argv[5], revision);
            std::cout << "installed " << argv[3] << " in " << argv[2] << "\n";
            return 0;
        }
        if (command == "resolve" && argc >= 5) {
            std::vector<std::filesystem::path> repositories{argv[2]};
            std::map<std::string, std::string> requested;
            std::filesystem::path outputPath = "ice.lock";
            for (int index = 4; index < argc; ++index) {
                if (std::string(argv[index]) == "--output") {
                    if (++index >= argc)
                        throw ice::ProjectError("package.arguments", "--output requires a path");
                    outputPath = argv[index];
                    continue;
                }
                auto separator = std::string(argv[index]).find('=');
                if (separator == std::string::npos)
                    throw ice::ProjectError("package.arguments", "Dependencies must use name=constraint");
                requested.emplace(std::string(argv[index]).substr(0, separator),
                                  std::string(argv[index]).substr(separator + 1));
            }
            auto lock = ice::PackageManager::resolve(repositories, requested, argv[3]);
            ice::PackageManager::writeLockfile(outputPath, lock);
            std::cout << "wrote " << outputPath.string() << "\n";
            return 0;
        }
        if (command == "verify" && argc == 4) {
            ice::PackageManager::verifyLockfile(ice::PackageManager::readLockfile(argv[3]), {argv[2]});
            std::cout << "lockfile verified\n";
            return 0;
        }
        throw ice::ProjectError("package.arguments", "Invalid package-manager command or arguments");
    } catch (const ice::ProjectError& error) {
        std::cerr << "error[" << error.code() << "]: " << error.what() << "\n";
        return 2;
    }
}
