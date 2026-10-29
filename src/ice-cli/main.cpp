#include "icelib/core.hpp"
#include "icelib/interchange.hpp"
#include "icelib/operations.hpp"
#include "icelib/simulation.hpp"
#include "icelib/simulation_report.hpp"
#include "icelib/simulation_store.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using json = nlohmann::json;

[[noreturn]] void argumentError(const std::string &message) { throw ice::ProjectError("cli.arguments", message); }

void expectArguments(const std::vector<std::string> &args, std::size_t minimum, std::size_t maximum,
                     const std::string &usage) {
    if (args.size() < minimum || args.size() > maximum)
        argumentError("Usage: ice-cli " + usage);
}

[[noreturn]] void unknownCommand(const std::string &command) {
    throw ice::ProjectError("cli.command", "Unknown command: " + command + ". Run 'ice-cli --help'.");
}

ice::Coordinate parseInteger(const std::string &text, const char *what) {
    try {
        std::size_t used = 0;
        const auto value = std::stoll(text, &used);
        if (used != text.size())
            throw std::invalid_argument("trailing characters");
        return static_cast<ice::Coordinate>(value);
    } catch (const std::exception &) {
        argumentError(std::string("Invalid integer for ") + what + ": " + text);
    }
}

double parseDouble(const std::string &text, const char *what) {
    try {
        std::size_t used = 0;
        const auto value = std::stod(text, &used);
        auto suffix = text.substr(used);
        std::transform(suffix.begin(), suffix.end(), suffix.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        double multiplier = 1.0;
        if (suffix.empty() || suffix == "s")
            multiplier = 1.0;
        else if (suffix == "t")
            multiplier = 1e12;
        else if (suffix == "g")
            multiplier = 1e9;
        else if (suffix == "meg")
            multiplier = 1e6;
        else if (suffix == "k")
            multiplier = 1e3;
        else if (suffix == "m")
            multiplier = 1e-3;
        else if (suffix == "u")
            multiplier = 1e-6;
        else if (suffix == "n")
            multiplier = 1e-9;
        else if (suffix == "p")
            multiplier = 1e-12;
        else if (suffix == "f")
            multiplier = 1e-15;
        else
            throw std::invalid_argument("unknown suffix");
        return value * multiplier;
    } catch (const std::exception &) {
        argumentError(std::string("Invalid number for ") + what + ": " + text);
    }
}

std::optional<ice::ObjectId> parseUnsigned(const std::string &text) {
    if (text.empty() || !std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c); }))
        return std::nullopt;
    try {
        std::size_t used = 0;
        const auto value = std::stoull(text, &used);
        if (used == text.size())
            return static_cast<ice::ObjectId>(value);
    } catch (const std::exception &) {
    }
    return std::nullopt;
}

template <typename Map> ice::ObjectId resolveByIdOrName(const Map &map, const std::string &text, const char *kind) {
    if (const auto id = parseUnsigned(text); id && map.contains(*id))
        return *id;
    for (const auto &[id, entry] : map) {
        if (entry.name == text)
            return id;
    }
    argumentError(std::string("Unknown ") + kind + ": " + text);
}

ice::ObjectId resolveSchematic(const ice::Project &project, const std::string &text) {
    return resolveByIdOrName(project.schematicCells, text, "schematic");
}

ice::ObjectId resolveLayout(const ice::Project &project, const std::string &text) {
    return resolveByIdOrName(project.layoutCells, text, "layout cell");
}

ice::ObjectId resolveLayer(const ice::Project &project, const std::string &text) {
    return resolveByIdOrName(project.layers, text, "layer");
}

ice::ObjectId resolveSymbol(const ice::SchematicCell &cell, const std::string &text) {
    if (const auto id = parseUnsigned(text); id && cell.symbols.contains(*id))
        return *id;
    for (const auto &[id, symbol] : cell.symbols) {
        if (symbol.name == text)
            return id;
    }
    argumentError("Unknown symbol: " + text);
}

ice::ObjectId resolvePin(const ice::SchematicCell &cell, const std::string &text) {
    if (const auto id = parseUnsigned(text); id) {
        for (const auto &[_, symbol] : cell.symbols) {
            if (std::any_of(symbol.pins.begin(), symbol.pins.end(), [&](const auto &pin) { return pin.id == *id; }))
                return *id;
        }
    }
    const auto dot = text.find('.');
    if (dot != std::string::npos) {
        const auto symbolName = text.substr(0, dot);
        const auto pinName = text.substr(dot + 1);
        for (const auto &[_, symbol] : cell.symbols) {
            if (symbol.name != symbolName)
                continue;
            for (const auto &pin : symbol.pins)
                if (pin.name == pinName)
                    return pin.id;
        }
    }
    std::optional<ice::ObjectId> found;
    for (const auto &[_, symbol] : cell.symbols) {
        for (const auto &pin : symbol.pins) {
            if (pin.name != text)
                continue;
            if (found)
                argumentError("Pin name is ambiguous; use <symbol>.<pin>: " + text);
            found = pin.id;
        }
    }
    if (!found)
        argumentError("Unknown pin: " + text);
    return *found;
}

ice::Orientation parseOrientation(const std::string &text) {
    static const std::map<std::string, ice::Orientation> values = {
        {"r0", ice::Orientation::r0},     {"r90", ice::Orientation::r90},  {"r180", ice::Orientation::r180},
        {"r270", ice::Orientation::r270}, {"mx", ice::Orientation::mx},    {"my", ice::Orientation::my},
        {"m45", ice::Orientation::m45},   {"m135", ice::Orientation::m135}};
    const auto it = values.find(text);
    if (it == values.end())
        argumentError("Unsupported orientation: " + text);
    return it->second;
}

std::vector<ice::LayoutPoint> parsePoints(const std::vector<std::string> &args, std::size_t begin) {
    std::vector<ice::LayoutPoint> points;
    for (std::size_t index = begin; index < args.size(); ++index) {
        const auto comma = args[index].find(',');
        if (comma == std::string::npos)
            argumentError("Points must be written as x,y");
        points.push_back({parseInteger(args[index].substr(0, comma), "point x"),
                          parseInteger(args[index].substr(comma + 1), "point y")});
    }
    if (points.size() < 3)
        argumentError("A polygon requires at least three x,y points");
    return points;
}

std::vector<std::string> splitSignals(const std::string &text) {
    std::vector<std::string> signals;
    std::string current;
    for (const char character : text) {
        if (character != ',') {
            current.push_back(character);
            continue;
        }
        const auto first = current.find_first_not_of(" \t");
        const auto last = current.find_last_not_of(" \t");
        if (first != std::string::npos)
            signals.push_back(current.substr(first, last - first + 1));
        current.clear();
    }
    const auto first = current.find_first_not_of(" \t");
    const auto last = current.find_last_not_of(" \t");
    if (first != std::string::npos)
        signals.push_back(current.substr(first, last - first + 1));
    return signals;
}

std::string readTextFile(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        throw ice::ProjectError("cli.file", "Unable to read file: " + path.string());
    return {std::istreambuf_iterator<char>(stream), {}};
}

std::string readStdin() {
    std::ostringstream stream;
    stream << std::cin.rdbuf();
    return stream.str();
}

std::filesystem::path projectRootForCli(const std::filesystem::path &path) {
    std::error_code status;
    if (std::filesystem::is_directory(path, status) && !status)
        return path;
    if (path.extension() == ".icepro" || path.filename() == "project.icepro")
        return path.parent_path();
    return path;
}

std::string projectNameForPath(const std::filesystem::path &path) {
    const auto root = projectRootForCli(path);
    auto name = root.filename().string();
    if (name.empty())
        name = "project";
    return name;
}

json diagnosticsJson(const ice::Project &project) {
    json result = json::array();
    for (const auto &diagnostic : project.validate()) {
        json item = {{"severity", diagnostic.severity == ice::Diagnostic::Severity::error ? "error" : "warning"},
                     {"code", diagnostic.code},
                     {"message", diagnostic.message}};
        if (diagnostic.object)
            item["object"] = *diagnostic.object;
        result.push_back(std::move(item));
    }
    return result;
}

json projectSummary(const ice::Project &project, const std::string &path) {
    json result = {
        {"name", project.name},
        {"identity", project.identity},
        {"path", path},
        {"grid", project.grid},
        {"units",
         {{"nanometers_per_unit", project.units.nanometers_per_unit}, {"denominator", project.units.denominator}}},
        {"package_dependencies", project.packageDependencies},
        {"schematics", json::array()},
        {"layers", json::array()},
        {"layouts", json::array()},
        {"procedural_layouts", json::array()},
        {"simulations", json::array()},
        {"reports", json::array()},
    };
    for (const auto &[id, cell] : project.schematicCells) {
        result["schematics"].push_back({{"id", id},
                                        {"name", cell.name},
                                        {"pdk", cell.pdk},
                                        {"text", cell.textSchematic},
                                        {"symbols", cell.symbols.size()},
                                        {"instances", cell.instances.size()},
                                        {"nets", cell.nets.size()}});
    }
    for (const auto &[id, layer] : project.layers) {
        result["layers"].push_back(
            {{"id", id}, {"name", layer.name}, {"number", layer.number}, {"visible", layer.visible}});
    }
    for (const auto &[id, cell] : project.layoutCells) {
        result["layouts"].push_back({{"id", id},
                                     {"name", cell.name},
                                     {"pdk", cell.pdk},
                                     {"library", cell.library},
                                     {"device", cell.device},
                                     {"generated_by", cell.generatedBy},
                                     {"shapes", cell.shapes.size()},
                                     {"ports", cell.ports.size()},
                                     {"instances", cell.instances.size()},
                                     {"planned_devices", cell.plannedDevices.size()}});
    }
    for (const auto &[id, cell] : project.proceduralLayouts) {
        result["procedural_layouts"].push_back({{"id", id},
                                                {"name", cell.name},
                                                {"pdk", cell.pdk},
                                                {"generated_cell", cell.generatedCell},
                                                {"script_length", cell.script.size()}});
    }
    for (const auto &[id, simulation] : project.simulations) {
        result["simulations"].push_back({{"id", id},
                                         {"name", simulation.name},
                                         {"schematic", simulation.schematic},
                                         {"simulator", simulation.simulator},
                                         {"analysis", simulation.analysis},
                                         {"stop", simulation.stop},
                                         {"step", simulation.step},
                                         {"graphs", simulation.graphs.size()}});
    }
    for (const auto &[id, report] : project.reports)
        result["reports"].push_back({{"id", id}, {"name", report.name}, {"path", report.path}});
    return result;
}

ice::ObjectId resolveReport(const ice::Project &project, const std::string &text) {
    return resolveByIdOrName(project.reports, text, "report");
}

ice::ObjectId resolveSimulation(const ice::Project &project, const std::string &text) {
    return resolveByIdOrName(project.simulations, text, "simulation");
}

const ice::SimulationGraph &resolveSimulationGraph(const ice::SimulationItem &simulation, const std::string &text) {
    if (const auto id = parseUnsigned(text)) {
        for (const auto &graph : simulation.graphs) {
            if (graph.id == *id)
                return graph;
        }
    }
    for (const auto &graph : simulation.graphs) {
        if (graph.name == text)
            return graph;
    }
    argumentError("Unknown simulation graph: " + text);
}

json simulationGraphJson(const ice::SimulationGraph &graph) {
    json result = {{"id", graph.id},
                   {"name", graph.name},
                   {"x_axis", graph.xAxis},
                   {"y_signals", graph.ySignals},
                   {"x_min", graph.xMin ? json(*graph.xMin) : json(nullptr)},
                   {"x_max", graph.xMax ? json(*graph.xMax) : json(nullptr)},
                   {"y_min", graph.yMin ? json(*graph.yMin) : json(nullptr)},
                   {"y_max", graph.yMax ? json(*graph.yMax) : json(nullptr)}};
    return result;
}

json schematicInfo(const ice::SchematicCell &cell) {
    json result = {{"id", cell.id},
                   {"name", cell.name},
                   {"pdk", cell.pdk},
                   {"text", cell.textSchematic},
                   {"source", cell.spiceText},
                   {"symbols", json::array()},
                   {"instances", json::array()},
                   {"nets", json::array()}};
    for (const auto &[id, symbol] : cell.symbols) {
        json pins = json::array();
        for (const auto &pin : symbol.pins)
            pins.push_back({{"id", pin.id}, {"name", pin.name}, {"x", pin.x}, {"y", pin.y}});
        result["symbols"].push_back(
            {{"id", id}, {"name", symbol.name}, {"source_cell", symbol.sourceCell}, {"pins", std::move(pins)}});
    }
    for (const auto &[id, instance] : cell.instances) {
        result["instances"].push_back({{"id", id},
                                       {"name", instance.name},
                                       {"symbol", instance.symbol},
                                       {"x", instance.x},
                                       {"y", instance.y},
                                       {"orientation", ice::orientationName(instance.orientation)}});
    }
    for (const auto &[id, net] : cell.nets) {
        result["nets"].push_back({{"id", id}, {"name", net.name}, {"pins", net.pins}, {"x", net.x}, {"y", net.y}});
    }
    return result;
}

json layoutInfo(const ice::Project &project, const ice::LayoutCell &cell) {
    json result = {{"id", cell.id},
                   {"name", cell.name},
                   {"pdk", cell.pdk},
                   {"library", cell.library},
                   {"device", cell.device},
                   {"generated_by", cell.generatedBy},
                   {"shapes", json::array()},
                   {"ports", json::array()},
                   {"instances", json::array()},
                   {"planned_devices", json::array()}};
    for (const auto &[id, shape] : cell.shapes) {
        const auto layer = project.layers.find(shape.layer);
        json polygon = json::array();
        for (const auto &point : shape.polygon)
            polygon.push_back({point.x, point.y});
        result["shapes"].push_back({{"id", id},
                                    {"layer", shape.layer},
                                    {"layer_name", layer == project.layers.end() ? "" : layer->second.name},
                                    {"rectangle",
                                     {{"x", shape.rectangle.x},
                                      {"y", shape.rectangle.y},
                                      {"width", shape.rectangle.width},
                                      {"height", shape.rectangle.height}}},
                                    {"polygon", std::move(polygon)}});
    }
    for (const auto &[id, port] : cell.ports) {
        result["ports"].push_back({{"id", id},
                                   {"name", port.name},
                                   {"layer", port.layer},
                                   {"x", port.rectangle.x},
                                   {"y", port.rectangle.y},
                                   {"width", port.rectangle.width},
                                   {"height", port.rectangle.height}});
    }
    for (const auto &[id, instance] : cell.instances) {
        result["instances"].push_back({{"id", id},
                                       {"name", instance.name},
                                       {"cell", instance.cell},
                                       {"x", instance.x},
                                       {"y", instance.y},
                                       {"orientation", ice::orientationName(instance.orientation)}});
    }
    for (const auto &[id, device] : cell.plannedDevices) {
        result["planned_devices"].push_back({{"id", id},
                                             {"name", device.name},
                                             {"x", device.x},
                                             {"y", device.y},
                                             {"width", device.width},
                                             {"height", device.height}});
    }
    return result;
}

void printJson(const json &value) { std::cout << value.dump(2) << "\n"; }

void printHelp() {
    std::cout << "ice-cli " << ice::version() << "\n\n"
              << "Usage: ice-cli <command> [arguments]\n\n"
              << "Project:\n"
              << "  project-create <folder|project.icepro> [name]\n"
              << "  project-info <folder|project.icepro>\n"
              << "  project-validate <folder|project.icepro>\n\n"
              << "Schematic:\n"
              << "  schematic-add-cell <project> <name> [pdk]\n"
              << "  schematic-add-text <project> <name> [pdk] [spice-file]\n"
              << "  schematic-list <project>\n"
              << "  schematic-info <project> <cell>\n"
              << "  schematic-add-symbol <project> <cell> <name> [source-cell]\n"
              << "  schematic-add-instance <project> <cell> <symbol> <name> <x> <y>\n"
              << "  schematic-add-net <project> <cell> <name> <pin> <pin> ...\n"
              << "  schematic-netlist <project> <cell>\n\n"
              << "Layout:\n"
              << "  layout-add-cell <project> <name> [pdk]\n"
              << "  layout-add-procedural <project> <name> [pdk] [script-file]\n"
              << "  layout-create-from-schematic <project> <schematic> [layout-name]\n"
              << "  layout-list <project>\n"
              << "  layout-info <project> <cell>\n"
              << "  layout-add-layer <project> <name> <number>\n"
              << "  layout-add-rectangle <project> <cell> <layer> <x> <y> <width> <height>\n"
              << "  layout-add-polygon <project> <cell> <layer> <x,y> <x,y> <x,y> ...\n"
              << "  layout-add-port <project> <cell> <name> <layer> <x> <y> <width> <height>\n"
              << "  layout-add-instance <project> <parent> <child> <name> <x> <y> [orientation]\n"
              << "  layout-export-gds <project> <cell> <output.gds>\n\n"
              << "Simulation:\n"
              << "  simulation-add <project> <name> <schematic> [analysis] [stop] [step] [signals]\n"
              << "  simulation-list <project>\n"
              << "  simulation-graph-add <project> <simulation> <name> [x-axis] [y-signals]\n"
              << "  simulation-graph-list <project> <simulation>\n"
              << "  simulation-graph-info <project> <simulation> <graph>\n"
              << "  simulation-graph-update <project> <simulation> <graph> [name] [x-axis] [y-signals]\n"
              << "  simulation-graph-limits <project> <simulation> <graph> [x-min] [x-max] [y-min] [y-max]\n"
              << "  simulation-graph-delete <project> <simulation> <graph>\n"
              << "  simulate <project> <schematic> [stop] [step] [timeout-ms] [signals] [output.json]\n"
              << "  simulate-netlist <netlist-file> [stop] [step] [timeout-ms] [signals] [output.json]\n\n"
              << "Reports:\n"
              << "  report-add <project> <name> [source-file]\n"
              << "  report-list <project>\n"
              << "  report-info <project> <report>\n"
              << "  report-read <project> <report>\n"
              << "  report-write <project> <report> <source-file|->\n"
              << "  report-run <project> <report> [timeout-ms]\n"
              << "  report-delete <project> <report>\n\n"
              << "Reports may be referenced by name or id. Use '-' to read report source from stdin.\n";
}

} // namespace

int main(int argc, char **argv) {
    auto logger = spdlog::stderr_color_mt("ice-cli");
    spdlog::set_default_logger(std::move(logger));

    if (argc < 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h" ||
        std::string(argv[1]) == "help") {
        printHelp();
        return 0;
    }

    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc - 1));
    for (int index = 1; index < argc; ++index)
        args.emplace_back(argv[index]);
    const auto command = args.front();
    auto executablePath = ice::currentExecutablePath();
    if (executablePath.empty())
        executablePath = std::filesystem::absolute(argv[0]);

    if (command == "--version" || command == "version") {
        std::cout << ice::version() << "\n";
        return 0;
    }

    try {
        if (command == "project-create") {
            expectArguments(args, 2, 3, "project-create <folder|project.icepro> [name]");
            const std::filesystem::path path = args[1];
            const auto name = args.size() == 3 ? args[2] : projectNameForPath(path);
            const auto projectFile = projectRootForCli(path) / "project.icepro";
            std::error_code status;
            if (std::filesystem::exists(projectFile, status) && !status)
                throw ice::ProjectError("project.exists", "A project already exists at " + projectFile.string());
            ice::Project project;
            project.identity = "project-" + name;
            project.name = name;
            project.save(path);
            printJson({{"path", projectFile.string()}, {"name", name}});
            return 0;
        }

        if (command == "project-info") {
            expectArguments(args, 2, 2, "project-info <folder|project.icepro>");
            const auto project = ice::Project::load(args[1]);
            printJson(projectSummary(project, args[1]));
            return 0;
        }

        if (command == "project-validate") {
            expectArguments(args, 2, 2, "project-validate <folder|project.icepro>");
            const auto project = ice::Project::load(args[1]);
            const auto diagnostics = diagnosticsJson(project);
            printJson({{"valid", diagnostics.empty()}, {"diagnostics", diagnostics}});
            return 0;
        }

        if (command == "schematic-add-cell") {
            expectArguments(args, 3, 4, "schematic-add-cell <project> <name> [pdk]");
            ice::ProjectSession session(ice::Project::load(args[1]));
            ice::ObjectId id = 0;
            const auto pdk = args.size() == 4 ? args[3] : std::string(ice::genericPdkId);
            session.execute("schematic-add-cell",
                            [&](ice::Project &project) { id = ice::createSchematicCell(project, args[2], pdk); });
            session.save(args[1]);
            printJson({{"id", id}, {"name", args[2]}, {"pdk", pdk}});
            return 0;
        }

        if (command == "schematic-add-text") {
            expectArguments(args, 3, 5, "schematic-add-text <project> <name> [pdk] [spice-file]");
            ice::ProjectSession session(ice::Project::load(args[1]));
            const auto pdk = args.size() >= 4 ? args[3] : std::string(ice::genericPdkId);
            auto text = args.size() == 5 ? readTextFile(args[4])
                                         : "* SPICE schematic: " + args[2] + "\n.title " + args[2] + "\n.end\n";
            ice::ObjectId id = 0;
            session.execute("schematic-add-text", [&](ice::Project &project) {
                id = ice::createTextSchematicCell(project, args[2], pdk, text);
            });
            session.save(args[1]);
            printJson({{"id", id}, {"name", args[2]}, {"pdk", pdk}, {"text", text}});
            return 0;
        }

        if (command == "schematic-list") {
            expectArguments(args, 2, 2, "schematic-list <project>");
            const auto project = ice::Project::load(args[1]);
            json result = json::array();
            for (const auto &[id, cell] : project.schematicCells) {
                result.push_back({{"id", id},
                                  {"name", cell.name},
                                  {"pdk", cell.pdk},
                                  {"text", cell.textSchematic},
                                  {"symbols", cell.symbols.size()},
                                  {"instances", cell.instances.size()},
                                  {"nets", cell.nets.size()}});
            }
            printJson(result);
            return 0;
        }

        if (command == "schematic-info") {
            expectArguments(args, 3, 3, "schematic-info <project> <cell>");
            const auto project = ice::Project::load(args[1]);
            const auto id = resolveSchematic(project, args[2]);
            printJson(schematicInfo(project.schematicCells.at(id)));
            return 0;
        }

        if (command == "schematic-add-symbol") {
            expectArguments(args, 4, 5, "schematic-add-symbol <project> <cell> <name> [source-cell]");
            const auto project = ice::Project::load(args[1]);
            const auto cell = resolveSchematic(project, args[2]);
            const auto source = args.size() == 5 ? resolveSchematic(project, args[4]) : ice::ObjectId{0};
            ice::ProjectSession session(project);
            ice::ObjectId id = 0;
            session.execute("schematic-add-symbol",
                            [&](ice::Project &p) { id = ice::addSymbol(p, cell, args[3], {}, source); });
            session.save(args[1]);
            printJson({{"id", id}, {"cell", cell}, {"name", args[3]}});
            return 0;
        }

        if (command == "schematic-add-instance") {
            expectArguments(args, 7, 7, "schematic-add-instance <project> <cell> <symbol> <name> <x> <y>");
            const auto project = ice::Project::load(args[1]);
            const auto cell = resolveSchematic(project, args[2]);
            const auto symbol = resolveSymbol(project.schematicCells.at(cell), args[3]);
            const auto x = parseInteger(args[5], "x");
            const auto y = parseInteger(args[6], "y");
            ice::ProjectSession session(project);
            ice::ObjectId id = 0;
            session.execute("schematic-add-instance",
                            [&](ice::Project &p) { id = ice::addSchematicInstance(p, cell, symbol, args[4], x, y); });
            session.save(args[1]);
            printJson({{"id", id}, {"cell", cell}, {"symbol", symbol}, {"name", args[4]}});
            return 0;
        }

        if (command == "schematic-add-net") {
            expectArguments(args, 5, static_cast<std::size_t>(-1),
                            "schematic-add-net <project> <cell> <name> <pin> <pin> ...");
            const auto project = ice::Project::load(args[1]);
            const auto cell = resolveSchematic(project, args[2]);
            std::vector<ice::ObjectId> pins;
            for (std::size_t index = 4; index < args.size(); ++index)
                pins.push_back(resolvePin(project.schematicCells.at(cell), args[index]));
            ice::ProjectSession session(project);
            ice::ObjectId id = 0;
            session.execute("schematic-add-net", [&](ice::Project &p) { id = ice::addNet(p, cell, args[3], pins); });
            session.save(args[1]);
            printJson({{"id", id}, {"cell", cell}, {"name", args[3]}, {"pins", pins}});
            return 0;
        }

        if (command == "schematic-netlist") {
            expectArguments(args, 3, 3, "schematic-netlist <project> <cell>");
            const auto project = ice::Project::load(args[1]);
            const auto cell = resolveSchematic(project, args[2]);
            std::vector<std::string> notes;
            const auto lines = ice::buildSimulationNetlist(project, cell, &notes);
            printJson({{"cell", cell}, {"netlist", lines}, {"notes", notes}});
            return 0;
        }

        if (command == "layout-add-cell") {
            expectArguments(args, 3, 4, "layout-add-cell <project> <name> [pdk]");
            ice::ProjectSession session(ice::Project::load(args[1]));
            ice::ObjectId id = 0;
            const auto pdk = args.size() == 4 ? args[3] : std::string(ice::genericPdkId);
            session.execute("layout-add-cell",
                            [&](ice::Project &project) { id = ice::createLayoutCell(project, args[2], pdk); });
            session.save(args[1]);
            printJson({{"id", id}, {"name", args[2]}, {"pdk", pdk}});
            return 0;
        }

        if (command == "layout-add-procedural") {
            expectArguments(args, 3, 5, "layout-add-procedural <project> <name> [pdk] [script-file]");
            ice::ProjectSession session(ice::Project::load(args[1]));
            const auto pdk = args.size() >= 4 ? args[3] : std::string(ice::genericPdkId);
            const auto script = args.size() == 5 ? readTextFile(args[4]) : std::string{};
            ice::ObjectId id = 0;
            ice::ObjectId generated = 0;
            session.execute("layout-add-procedural", [&](ice::Project &project) {
                id = ice::createProceduralLayout(project, args[2], pdk, script);
                generated = project.proceduralLayouts.at(id).generatedCell;
            });
            session.save(args[1]);
            printJson({{"id", id}, {"name", args[2]}, {"pdk", pdk}, {"generated_cell", generated}});
            return 0;
        }

        if (command == "layout-create-from-schematic") {
            expectArguments(args, 3, 4, "layout-create-from-schematic <project> <schematic> [layout-name]");
            const auto project = ice::Project::load(args[1]);
            const auto schematic = resolveSchematic(project, args[2]);
            const auto name = args.size() == 4 ? args[3] : std::string{};
            ice::ProjectSession session(project);
            ice::ObjectId id = 0;
            session.execute("layout-create-from-schematic",
                            [&](ice::Project &p) { id = ice::createLayoutFromSchematic(p, schematic, name); });
            session.save(args[1]);
            printJson({{"id", id}, {"schematic", schematic}});
            return 0;
        }

        if (command == "layout-list") {
            expectArguments(args, 2, 2, "layout-list <project>");
            const auto project = ice::Project::load(args[1]);
            json result = json::array();
            for (const auto &[id, cell] : project.layoutCells) {
                result.push_back({{"id", id},
                                  {"name", cell.name},
                                  {"pdk", cell.pdk},
                                  {"library", cell.library},
                                  {"device", cell.device},
                                  {"generated_by", cell.generatedBy},
                                  {"shapes", cell.shapes.size()},
                                  {"ports", cell.ports.size()},
                                  {"instances", cell.instances.size()}});
            }
            printJson(result);
            return 0;
        }

        if (command == "layout-info") {
            expectArguments(args, 3, 3, "layout-info <project> <cell>");
            const auto project = ice::Project::load(args[1]);
            const auto id = resolveLayout(project, args[2]);
            printJson(layoutInfo(project, project.layoutCells.at(id)));
            return 0;
        }

        if (command == "layout-add-layer") {
            expectArguments(args, 4, 4, "layout-add-layer <project> <name> <number>");
            ice::ProjectSession session(ice::Project::load(args[1]));
            const auto number = static_cast<int>(parseInteger(args[3], "layer number"));
            ice::ObjectId id = 0;
            session.execute("layout-add-layer",
                            [&](ice::Project &project) { id = ice::addLayer(project, args[2], number); });
            session.save(args[1]);
            printJson({{"id", id}, {"name", args[2]}, {"number", number}});
            return 0;
        }

        if (command == "layout-add-rectangle") {
            expectArguments(args, 8, 8, "layout-add-rectangle <project> <cell> <layer> <x> <y> <width> <height>");
            const auto project = ice::Project::load(args[1]);
            const auto cell = resolveLayout(project, args[2]);
            const auto layer = resolveLayer(project, args[3]);
            const ice::Rect rect{parseInteger(args[4], "x"), parseInteger(args[5], "y"), parseInteger(args[6], "width"),
                                 parseInteger(args[7], "height")};
            ice::ProjectSession session(project);
            ice::ObjectId id = 0;
            session.execute("layout-add-rectangle",
                            [&](ice::Project &p) { id = ice::addRectangle(p, cell, layer, rect); });
            session.save(args[1]);
            printJson({{"id", id}, {"cell", cell}, {"layer", layer}});
            return 0;
        }

        if (command == "layout-add-polygon") {
            expectArguments(args, 6, static_cast<std::size_t>(-1),
                            "layout-add-polygon <project> <cell> <layer> <x,y> <x,y> <x,y> ...");
            const auto project = ice::Project::load(args[1]);
            const auto cell = resolveLayout(project, args[2]);
            const auto layer = resolveLayer(project, args[3]);
            const auto points = parsePoints(args, 4);
            ice::ProjectSession session(project);
            ice::ObjectId id = 0;
            session.execute("layout-add-polygon",
                            [&](ice::Project &p) { id = ice::addPolygon(p, cell, layer, points); });
            session.save(args[1]);
            printJson({{"id", id}, {"cell", cell}, {"layer", layer}, {"points", points.size()}});
            return 0;
        }

        if (command == "layout-add-port") {
            expectArguments(args, 9, 9, "layout-add-port <project> <cell> <name> <layer> <x> <y> <width> <height>");
            const auto project = ice::Project::load(args[1]);
            const auto cell = resolveLayout(project, args[2]);
            const auto layer = resolveLayer(project, args[4]);
            const ice::Rect rect{parseInteger(args[5], "x"), parseInteger(args[6], "y"), parseInteger(args[7], "width"),
                                 parseInteger(args[8], "height")};
            ice::ProjectSession session(project);
            ice::ObjectId id = 0;
            session.execute("layout-add-port",
                            [&](ice::Project &p) { id = ice::addPort(p, cell, args[3], layer, rect); });
            session.save(args[1]);
            printJson({{"id", id}, {"cell", cell}, {"name", args[3]}, {"layer", layer}});
            return 0;
        }

        if (command == "layout-add-instance") {
            expectArguments(args, 8, 9, "layout-add-instance <project> <parent> <child> <name> <x> <y> [orientation]");
            const auto project = ice::Project::load(args[1]);
            const auto parent = resolveLayout(project, args[2]);
            const auto child = resolveLayout(project, args[3]);
            const auto orientation = args.size() == 9 ? parseOrientation(args[8]) : ice::Orientation::r0;
            const auto x = parseInteger(args[5], "x");
            const auto y = parseInteger(args[6], "y");
            ice::ProjectSession session(project);
            ice::ObjectId id = 0;
            session.execute("layout-add-instance", [&](ice::Project &p) {
                id = ice::addLayoutInstance(p, parent, child, args[4], x, y, orientation);
            });
            session.save(args[1]);
            printJson({{"id", id}, {"parent", parent}, {"child", child}, {"name", args[4]}});
            return 0;
        }

        if (command == "layout-export-gds") {
            expectArguments(args, 4, 4, "layout-export-gds <project> <cell> <output.gds>");
            const auto project = ice::Project::load(args[1]);
            const auto cell = resolveLayout(project, args[2]);
            ice::LayoutInterchange::exportGds(project, cell, args[3]);
            printJson({{"cell", cell}, {"output", args[3]}});
            return 0;
        }

        if (command == "simulation-add") {
            expectArguments(args, 4, 8,
                            "simulation-add <project> <name> <schematic> [analysis] [stop] [step] [signals]");
            const auto project = ice::Project::load(args[1]);
            const auto schematic = resolveSchematic(project, args[3]);
            ice::SimulationItem simulation;
            simulation.name = args[2];
            simulation.schematic = schematic;
            if (args.size() >= 5)
                simulation.analysis = args[4];
            if (args.size() >= 6)
                simulation.stop = parseDouble(args[5], "stop time");
            if (args.size() >= 7)
                simulation.step = parseDouble(args[6], "step time");
            if (args.size() >= 8)
                simulation.captureSignals = args[7];
            ice::ProjectSession session(project);
            ice::ObjectId graphId = 0;
            session.execute("simulation-add", [&](ice::Project &p) {
                simulation.id = p.nextId();
                p.simulations.emplace(simulation.id, simulation);
                ice::SimulationGraph graph;
                graph.id = p.nextId();
                graph.name = "Graph 1";
                graph.xAxis = "time";
                graph.ySignals = splitSignals(simulation.captureSignals);
                graphId = graph.id;
                p.simulations.at(simulation.id).graphs.push_back(std::move(graph));
            });
            session.save(args[1]);
            printJson({{"id", simulation.id},
                       {"name", simulation.name},
                       {"schematic", schematic},
                       {"signals", simulation.captureSignals},
                       {"graph", graphId}});
            return 0;
        }

        if (command == "simulation-list") {
            expectArguments(args, 2, 2, "simulation-list <project>");
            const auto project = ice::Project::load(args[1]);
            printJson(projectSummary(project, args[1])["simulations"]);
            return 0;
        }

        if (command == "simulation-graph-add") {
            expectArguments(args, 4, 6, "simulation-graph-add <project> <simulation> <name> [x-axis] [y-signals]");
            const auto project = ice::Project::load(args[1]);
            const auto simulationId = resolveSimulation(project, args[2]);
            const auto &simulation = project.simulations.at(simulationId);
            ice::SimulationGraph graph;
            graph.name = args[3];
            if (args.size() >= 5 && !args[4].empty() && args[4] != "-")
                graph.xAxis = args[4];
            if (args.size() >= 6) {
                if (args[5] != "-")
                    graph.ySignals = splitSignals(args[5]);
            } else {
                graph.ySignals = splitSignals(simulation.captureSignals);
            }
            ice::ProjectSession session(project);
            ice::ObjectId graphId = 0;
            session.execute("simulation-graph-add",
                            [&](ice::Project &p) { graphId = ice::addSimulationGraph(p, simulationId, graph); });
            session.save(args[1]);
            const auto &stored = session.project().simulations.at(simulationId);
            for (const auto &entry : stored.graphs) {
                if (entry.id == graphId) {
                    printJson(simulationGraphJson(entry));
                    break;
                }
            }
            return 0;
        }

        if (command == "simulation-graph-list") {
            expectArguments(args, 3, 3, "simulation-graph-list <project> <simulation>");
            const auto project = ice::Project::load(args[1]);
            const auto simulationId = resolveSimulation(project, args[2]);
            json graphs = json::array();
            for (const auto &graph : project.simulations.at(simulationId).graphs)
                graphs.push_back(simulationGraphJson(graph));
            printJson(graphs);
            return 0;
        }

        if (command == "simulation-graph-info") {
            expectArguments(args, 4, 4, "simulation-graph-info <project> <simulation> <graph>");
            const auto project = ice::Project::load(args[1]);
            const auto simulationId = resolveSimulation(project, args[2]);
            const auto &graph = resolveSimulationGraph(project.simulations.at(simulationId), args[3]);
            printJson(simulationGraphJson(graph));
            return 0;
        }

        if (command == "simulation-graph-update") {
            expectArguments(args, 4, 7,
                            "simulation-graph-update <project> <simulation> <graph> [name] [x-axis] [y-signals]");
            const auto project = ice::Project::load(args[1]);
            const auto simulationId = resolveSimulation(project, args[2]);
            const auto &current = resolveSimulationGraph(project.simulations.at(simulationId), args[3]);
            auto updated = current;
            if (args.size() >= 5 && !args[4].empty() && args[4] != "-")
                updated.name = args[4];
            if (args.size() >= 6 && !args[5].empty() && args[5] != "-")
                updated.xAxis = args[5];
            if (args.size() >= 7 && args[6] != "-")
                updated.ySignals = splitSignals(args[6]);
            ice::ProjectSession session(project);
            const auto graphId = updated.id;
            session.execute("simulation-graph-update",
                            [&](ice::Project &p) { ice::updateSimulationGraph(p, simulationId, graphId, updated); });
            session.save(args[1]);
            const auto &stored = session.project().simulations.at(simulationId);
            for (const auto &entry : stored.graphs) {
                if (entry.id == graphId) {
                    printJson(simulationGraphJson(entry));
                    break;
                }
            }
            return 0;
        }

        if (command == "simulation-graph-limits") {
            expectArguments(args, 4, 8,
                            "simulation-graph-limits <project> <simulation> <graph> [x-min] [x-max] [y-min] [y-max]");
            const auto project = ice::Project::load(args[1]);
            const auto simulationId = resolveSimulation(project, args[2]);
            const auto &current = resolveSimulationGraph(project.simulations.at(simulationId), args[3]);
            auto updated = current;
            const auto applyLimit = [](const std::string &text, std::optional<double> &value, const char *label) {
                if (text == "-")
                    value.reset();
                else
                    value = parseDouble(text, label);
            };
            if (args.size() >= 5 && !args[4].empty())
                applyLimit(args[4], updated.xMin, "x-min");
            if (args.size() >= 6 && !args[5].empty())
                applyLimit(args[5], updated.xMax, "x-max");
            if (args.size() >= 7 && !args[6].empty())
                applyLimit(args[6], updated.yMin, "y-min");
            if (args.size() >= 8 && !args[7].empty())
                applyLimit(args[7], updated.yMax, "y-max");
            ice::ProjectSession session(project);
            const auto graphId = updated.id;
            session.execute("simulation-graph-limits",
                            [&](ice::Project &p) { ice::updateSimulationGraph(p, simulationId, graphId, updated); });
            session.save(args[1]);
            const auto &stored = session.project().simulations.at(simulationId);
            for (const auto &entry : stored.graphs) {
                if (entry.id == graphId) {
                    printJson(simulationGraphJson(entry));
                    break;
                }
            }
            return 0;
        }

        if (command == "simulation-graph-delete") {
            expectArguments(args, 4, 4, "simulation-graph-delete <project> <simulation> <graph>");
            const auto project = ice::Project::load(args[1]);
            const auto simulationId = resolveSimulation(project, args[2]);
            const auto &simulation = project.simulations.at(simulationId);
            if (simulation.graphs.size() <= 1)
                throw ice::ProjectError("simulation.graph.last", "A simulation must have at least one graph");
            const auto &graph = resolveSimulationGraph(simulation, args[3]);
            const auto graphId = graph.id;
            const auto graphName = graph.name;
            ice::ProjectSession session(project);
            session.execute("simulation-graph-delete",
                            [&](ice::Project &p) { ice::removeSimulationGraph(p, simulationId, graphId); });
            session.save(args[1]);
            printJson({{"id", graphId}, {"name", graphName}, {"simulation", simulationId}, {"deleted", true}});
            return 0;
        }

        if (command == "report-add") {
            expectArguments(args, 3, 4, "report-add <project> <name> [source-file]");
            auto project = ice::Project::load(args[1]);
            std::string source;
            if (args.size() > 3)
                source = args[3] == "-" ? readStdin() : readTextFile(args[3]);
            const auto id = ice::createProjectReport(project, projectRootForCli(args[1]), args[2], source);
            project.save(args[1]);
            const auto &report = project.reports.at(id);
            printJson({{"id", id}, {"name", report.name}, {"path", report.path}});
            return 0;
        }

        if (command == "report-list") {
            expectArguments(args, 2, 2, "report-list <project>");
            const auto project = ice::Project::load(args[1]);
            printJson(projectSummary(project, args[1])["reports"]);
            return 0;
        }

        if (command == "report-info") {
            expectArguments(args, 3, 3, "report-info <project> <report>");
            const auto project = ice::Project::load(args[1]);
            const auto id = resolveReport(project, args[2]);
            const auto &report = project.reports.at(id);
            printJson({{"id", id}, {"name", report.name}, {"path", report.path}});
            return 0;
        }

        if (command == "report-read") {
            expectArguments(args, 3, 3, "report-read <project> <report>");
            const auto project = ice::Project::load(args[1]);
            const auto id = resolveReport(project, args[2]);
            const auto &report = project.reports.at(id);
            printJson({{"id", id},
                       {"name", report.name},
                       {"path", report.path},
                       {"source", ice::readProjectReportSource(projectRootForCli(args[1]), report)}});
            return 0;
        }

        if (command == "report-write") {
            expectArguments(args, 4, 4, "report-write <project> <report> <source-file|->");
            const auto project = ice::Project::load(args[1]);
            const auto id = resolveReport(project, args[2]);
            const auto &report = project.reports.at(id);
            const auto source = args[3] == "-" ? readStdin() : readTextFile(args[3]);
            ice::writeProjectReportSource(projectRootForCli(args[1]), report, source);
            printJson({{"id", id}, {"name", report.name}, {"path", report.path}, {"bytes", source.size()}});
            return 0;
        }

        if (command == "report-run") {
            expectArguments(args, 3, 4, "report-run <project> <report> [timeout-ms]");
            const auto project = ice::Project::load(args[1]);
            const auto id = resolveReport(project, args[2]);
            const auto &report = project.reports.at(id);
            const auto timeoutMs =
                args.size() > 3 ? parseInteger(args[3], "timeout") : static_cast<ice::Coordinate>(30000);
            const auto timeout = std::chrono::milliseconds(std::max<ice::Coordinate>(1, timeoutMs));
            const auto result = ice::runProjectReport(projectRootForCli(args[1]), report, executablePath, timeout);
            const bool ok = !result.timedOut && result.exitCode == 0;
            printJson({{"id", id},
                       {"name", report.name},
                       {"path", report.path},
                       {"ok", ok},
                       {"timed_out", result.timedOut},
                       {"exit_code", result.exitCode},
                       {"stdout", result.stdoutText},
                       {"stderr", result.stderrText}});
            if (result.timedOut)
                throw ice::ProjectError("report.timeout", "Report timed out");
            if (result.exitCode != 0)
                throw ice::ProjectError("report.failed", "Report exited with code " + std::to_string(result.exitCode));
            return 0;
        }

        if (command == "report-delete") {
            expectArguments(args, 3, 3, "report-delete <project> <report>");
            auto project = ice::Project::load(args[1]);
            const auto id = resolveReport(project, args[2]);
            const auto report = project.reports.at(id);
            project.reports.erase(id);
            project.save(args[1]);
            printJson({{"id", id}, {"name", report.name}, {"path", report.path}, {"deleted", true}});
            return 0;
        }

        if (command == "simulate" || command == "simulate-netlist") {
            const bool fromSchematic = command == "simulate";
            const std::size_t base = fromSchematic ? 3 : 2;
            expectArguments(args, base, base + 5,
                            fromSchematic
                                ? "simulate <project> <schematic> [stop] [step] [timeout-ms] [signals] [output.json]"
                                : "simulate-netlist <netlist-file> [stop] [step] [timeout-ms] [signals] [output.json]");

            std::size_t end = args.size();
            std::string outputPath;
            if (end > base && std::filesystem::path(args.back()).extension() == ".json") {
                outputPath = args.back();
                --end;
            }

            ice::SimulationRequest request;
            ice::SimulationRunContext context;
            if (fromSchematic) {
                const auto project = ice::Project::load(args[1]);
                const auto schematicId = resolveSchematic(project, args[2]);
                const auto &schematic = project.schematicCells.at(schematicId);
                request.netlist = ice::buildSimulationNetlist(project, schematicId);
                context.projectRoot = args[1];
                context.projectIdentity = project.identity;
                context.projectName = project.name;
                context.simulationId = 0;
                context.simulationName = "cli-" + schematic.name;
                context.schematicId = schematicId;
                context.schematicName = schematic.name;
            } else {
                std::ifstream netlist(args[1]);
                if (!netlist)
                    throw ice::ProjectError("cli.file", "Unable to read netlist: " + args[1]);
                std::string line;
                while (std::getline(netlist, line))
                    request.netlist.push_back(line);
                context.projectRoot = std::filesystem::current_path();
                context.projectIdentity = "netlist";
                context.projectName = "netlist";
                context.simulationId = 0;
                context.simulationName = std::filesystem::path(args[1]).stem().string();
                context.schematicId = 0;
                context.schematicName = context.simulationName;
            }

            std::size_t option = base;
            if (end > option)
                request.stop = parseDouble(args[option++], "stop time");
            if (end > option)
                request.step = parseDouble(args[option++], "step time");
            const auto timeout = end > option ? std::chrono::milliseconds(parseInteger(args[option++], "timeout"))
                                              : std::chrono::milliseconds(10000);
            if (end > option)
                request.probeSignals = splitSignals(args[option++]);
            if (end > option)
                throw ice::ProjectError("cli.arguments", "Too many simulation arguments");
            context.request = request;

            ice::SimulationRunner runner;
            const auto result = runner.run(request, timeout, [](const ice::SimulationProgress &progress) {
                std::clog << "progress: " << progress.fraction << " " << progress.message << "\n";
            });

            std::filesystem::path storedPath;
            std::string runId;
            if (outputPath.empty()) {
                const auto artifact = ice::writeSimulationRun(context, result);
                storedPath = artifact.path;
                runId = artifact.runId;
            } else {
                std::ofstream output(outputPath);
                if (!output)
                    throw ice::ProjectError("simulation.save", "Unable to open simulation result path");
                output << ice::serializeSimulationRun(context, result);
                storedPath = outputPath;
            }

            json summary = {{"state", ice::jobStateName(result.state)},
                            {"analysis", result.analysis},
                            {"time_points", result.time.size()},
                            {"traces", result.traces.size()},
                            {"stored", storedPath.string()},
                            {"error", result.error}};
            if (!runId.empty())
                summary["run_id"] = runId;
            printJson(summary);
            if (result.state != ice::JobState::succeeded)
                throw ice::ProjectError("simulation." + ice::jobStateName(result.state), result.error);
            return 0;
        }

        unknownCommand(command);
    } catch (const ice::ProjectError &error) {
        std::cerr << "error[" << error.code() << "]: " << error.what() << "\n";
        return 2;
    } catch (const std::exception &error) {
        std::cerr << "error[internal]: " << error.what() << "\n";
        return 3;
    }
}
