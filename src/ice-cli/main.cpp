#include "icelib/core.hpp"
#include "icelib/operations.hpp"
#include "icelib/simulation.hpp"
#include <chrono>
#include <fstream>
#include <iostream>

int main(int argc, char **argv) {
    if (argc < 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") {
        std::cout << "ice-cli " << ice::version()
                  << "\nUsage: ice-cli "
                     "<version|project-create|project-validate|schematic-add-cell|layout-add-cell|layout-add-layer|"
                     "layout-add-rectangle|simulate> ...\n";
        return 0;
    }
    std::string command = argv[1];
    if (command == "--version" || command == "version") {
        std::cout << ice::version() << "\n";
        return 0;
    }
    const auto parseInteger = [](const char *text, const char *what) {
        try {
            std::size_t used = 0;
            long long value = std::stoll(text, &used);
            if (text[used] != '\0')
                throw ice::ProjectError("cli.arguments", std::string("Invalid integer for ") + what);
            return static_cast<ice::Coordinate>(value);
        } catch (const ice::ProjectError &) {
            throw;
        } catch (const std::exception &) {
            throw ice::ProjectError("cli.arguments", std::string("Invalid integer for ") + what);
        }
    };
    const auto parseUnsigned = [](const char *text, const char *what) {
        try {
            std::size_t used = 0;
            unsigned long long value = std::stoull(text, &used);
            if (text[used] != '\0')
                throw ice::ProjectError("cli.arguments", std::string("Invalid id for ") + what);
            return static_cast<ice::ObjectId>(value);
        } catch (const ice::ProjectError &) {
            throw;
        } catch (const std::exception &) {
            throw ice::ProjectError("cli.arguments", std::string("Invalid id for ") + what);
        }
    };
    const auto parseDouble = [](const char *text, const char *what) {
        try {
            std::size_t used = 0;
            double value = std::stod(text, &used);
            if (text[used] != '\0')
                throw ice::ProjectError("cli.arguments", std::string("Invalid number for ") + what);
            return value;
        } catch (const ice::ProjectError &) {
            throw;
        } catch (const std::exception &) {
            throw ice::ProjectError("cli.arguments", std::string("Invalid number for ") + what);
        }
    };
    try {
        if (command == "project-create") {
            if (argc != 3)
                throw ice::ProjectError("cli.arguments", "project-create requires a path");
            ice::Project project;
            project.identity = "project-" + std::filesystem::path(argv[2]).stem().string();
            project.name = std::filesystem::path(argv[2]).stem().string();
            project.save(argv[2]);
            return 0;
        }
        if (command == "project-validate") {
            if (argc != 3)
                throw ice::ProjectError("cli.arguments", "project-validate requires a path");
            auto project = ice::Project::load(argv[2]);
            std::cout << "valid: " << project.name << "\n";
            return 0;
        }
        if (command == "schematic-add-cell") {
            if (argc != 4)
                throw ice::ProjectError("cli.arguments", "schematic-add-cell requires a project and cell name");
            ice::ProjectSession session(ice::Project::load(argv[2]));
            session.execute("schematic-add-cell",
                            [&](ice::Project &project) { ice::createSchematicCell(project, argv[3]); });
            session.save(argv[2]);
            return 0;
        }
        if (command == "layout-add-cell") {
            if (argc != 4)
                throw ice::ProjectError("cli.arguments", "layout-add-cell requires a project and cell name");
            ice::ProjectSession session(ice::Project::load(argv[2]));
            session.execute("layout-add-cell", [&](ice::Project &project) { ice::createLayoutCell(project, argv[3]); });
            session.save(argv[2]);
            return 0;
        }
        if (command == "layout-add-layer") {
            if (argc != 5)
                throw ice::ProjectError("cli.arguments", "layout-add-layer requires a project, name, and number");
            ice::ProjectSession session(ice::Project::load(argv[2]));
            const int number = static_cast<int>(parseInteger(argv[4], "layer number"));
            session.execute("layout-add-layer",
                            [&](ice::Project &project) { ice::addLayer(project, argv[3], number); });
            session.save(argv[2]);
            return 0;
        }
        if (command == "layout-add-rectangle") {
            if (argc != 9)
                throw ice::ProjectError("cli.arguments",
                                        "layout-add-rectangle requires project, cell, layer, x, y, width, height");
            ice::ProjectSession session(ice::Project::load(argv[2]));
            const auto cell = parseUnsigned(argv[3], "cell");
            const auto layer = parseUnsigned(argv[4], "layer");
            const ice::Rect rect{parseInteger(argv[5], "x"), parseInteger(argv[6], "y"), parseInteger(argv[7], "width"),
                                 parseInteger(argv[8], "height")};
            session.execute("layout-add-rectangle",
                            [&](ice::Project &project) { ice::addRectangle(project, cell, layer, rect); });
            session.save(argv[2]);
            return 0;
        }
        if (command == "simulate") {
            if (argc < 3 || argc > 6)
                throw ice::ProjectError("cli.arguments",
                                        "simulate requires an output path and optional stop, step, and timeout");
            ice::SimulationRequest request;
            if (argc >= 4)
                request.stop = parseDouble(argv[3], "stop time");
            if (argc >= 5)
                request.step = parseDouble(argv[4], "step time");
            const auto timeout = argc == 6 ? std::chrono::milliseconds(parseInteger(argv[5], "timeout"))
                                           : std::chrono::milliseconds(5000);
            ice::SimulationRunner runner;
            auto result = runner.run(request, timeout, [](const ice::SimulationProgress &progress) {
                std::clog << "progress: " << progress.fraction << " " << progress.message << "\n";
            });
            std::ofstream output(argv[2]);
            if (!output)
                throw ice::ProjectError("simulation.save", "Unable to open simulation result path");
            output << ice::serializeSimulationResult(result);
            if (result.state != ice::JobState::succeeded)
                throw ice::ProjectError("simulation." + ice::jobStateName(result.state), result.error);
            return 0;
        }
        throw ice::ProjectError("cli.command", "Unknown command: " + command);
    } catch (const ice::ProjectError &error) {
        std::cerr << "error[" << error.code() << "]: " << error.what() << "\n";
        return 2;
    } catch (const std::exception &error) {
        std::cerr << "error[internal]: " << error.what() << "\n";
        return 3;
    }
}
