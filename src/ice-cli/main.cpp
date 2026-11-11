#include "icelib/agent_api.hpp"
#include "icelib/connectivity.hpp"
#include "icelib/core.hpp"
#include "icelib/interchange.hpp"
#include "icelib/measure.hpp"
#include "icelib/operations.hpp"
#include "icelib/pdk.hpp"
#include "icelib/pex.hpp"
#include "icelib/simulation.hpp"
#include "icelib/simulation_report.hpp"
#include "icelib/simulation_store.hpp"
#include "icelib/symbols.hpp"
#include "icelib/verification.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
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

ice::ObjectId resolveLayoutItem(const ice::Project &project, const std::string &text) {
    if (const auto id = parseUnsigned(text)) {
        if (project.layoutCells.contains(*id) || project.proceduralLayouts.contains(*id))
            return *id;
    }
    for (const auto &[id, procedural] : project.proceduralLayouts) {
        if (procedural.name == text)
            return id;
    }
    for (const auto &[id, cell] : project.layoutCells) {
        if (cell.name == text)
            return id;
    }
    argumentError("Unknown layout or procedural layout: " + text);
}

// Fills in the package default when a layout command omits the stack. A
// request that names an undeclared stack is rejected before the project is
// modified.
std::string resolveLayoutStack(const std::string &pdk, const std::string &requested) {
    if (pdk.empty() || pdk == ice::genericPdkId)
        return requested;
    const auto pdks = ice::findPdks(pdk);
    if (pdks.empty())
        return requested;
    const auto package = ice::PackageManager::readManifest(pdks.front().packageRoot);
    if (!package.pdk)
        return requested;
    return ice::resolvePdkStack(*package.pdk, requested);
}

ice::ObjectId resolveLayer(const ice::Project &project, const std::string &text) {
    return resolveByIdOrName(project.layers, text, "layer");
}

ice::ObjectId resolveLayoutNet(const ice::LayoutCell &cell, const std::string &text) {
    return resolveByIdOrName(cell.nets, text, "layout net");
}

ice::ObjectId resolveLayoutPort(const ice::LayoutCell &cell, const std::string &text) {
    if (const auto id = parseUnsigned(text); id && cell.ports.contains(*id))
        return *id;
    for (const auto &[id, port] : cell.ports) {
        if (port.name == text)
            return id;
    }
    argumentError("Unknown layout port: " + text);
}

ice::PortDirection parsePortDirectionArgument(const std::string &text) {
    if (const auto direction = ice::parsePortDirection(text))
        return *direction;
    argumentError("Unknown port direction: " + text);
}

ice::PortSide parsePortSideArgument(const std::string &text) {
    if (const auto side = ice::parsePortSide(text))
        return *side;
    argumentError("Unknown port side: " + text);
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
        const auto ownerName = text.substr(0, dot);
        const auto pinName = text.substr(dot + 1);
        for (const auto &[_, instance] : cell.instances) {
            if (instance.name != ownerName)
                continue;
            const auto symbol = cell.symbols.find(instance.symbol);
            if (symbol == cell.symbols.end())
                continue;
            for (const auto &pin : symbol->second.pins)
                if (pin.name == pinName)
                    return pin.id;
        }
        for (const auto &[_, symbol] : cell.symbols) {
            if (symbol.name != ownerName)
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

ice::PinDirection parsePinDirection(const std::string &text) {
    static const std::map<std::string, ice::PinDirection> values = {
        {"left", ice::PinDirection::left},
        {"right", ice::PinDirection::right},
        {"up", ice::PinDirection::up},
        {"down", ice::PinDirection::down},
    };
    const auto it = values.find(text);
    if (it == values.end())
        argumentError("Unsupported pin direction: " + text);
    return it->second;
}

const char *pinDirectionName(ice::PinDirection direction) {
    switch (direction) {
    case ice::PinDirection::left:
        return "left";
    case ice::PinDirection::right:
        return "right";
    case ice::PinDirection::up:
        return "up";
    case ice::PinDirection::down:
        return "down";
    }
    return "right";
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
    const auto flush = [&] {
        const auto first = current.find_first_not_of(" \t\r\n");
        const auto last = current.find_last_not_of(" \t\r\n");
        if (first != std::string::npos)
            signals.push_back(current.substr(first, last - first + 1));
        current.clear();
    };
    for (const char character : text) {
        if (character == ',' || std::isspace(static_cast<unsigned char>(character))) {
            flush();
            continue;
        }
        current.push_back(character);
    }
    flush();
    return signals;
}

std::string trimText(const std::string &text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::string verilogIdentifier(const std::string &text) {
    std::string result;
    result.reserve(text.size());
    for (const auto character : text) {
        const auto byte = static_cast<unsigned char>(character);
        if (std::isalnum(byte) || character == '_')
            result.push_back(character);
        else if (!result.empty() && result.back() != '_')
            result.push_back('_');
    }
    while (!result.empty() && result.back() == '_')
        result.pop_back();
    if (result.empty())
        result = "digital_block";
    if (std::isdigit(static_cast<unsigned char>(result.front())))
        result.insert(result.begin(), '_');
    return result;
}

std::string normalizeAnalysisName(const std::string &text) {
    std::string normalized;
    for (const unsigned char character : text) {
        if (!std::isspace(character))
            normalized.push_back(static_cast<char>(std::tolower(character)));
    }
    if (normalized.empty() || normalized == "tran" || normalized == "transient")
        return "transient";
    if (normalized == "dc" || normalized == "dcsweep")
        return "dc";
    if (normalized == "ac" || normalized == "acsweep")
        return "ac";
    if (normalized == "op" || normalized == "operatingpoint")
        return "op";
    if (normalized == "digital" || normalized == "vcd" || normalized == "systemverilog")
        return "digital";
    argumentError("Unsupported analysis: " + text);
}

bool isAnalysisDirective(const std::string &line) {
    std::string normalized;
    for (const unsigned char character : line) {
        if (!std::isspace(character))
            normalized.push_back(static_cast<char>(std::tolower(character)));
    }
    const auto starts = [&](const char *name) {
        const std::string prefix(name);
        return normalized == prefix || (normalized.starts_with(prefix) && normalized.size() > prefix.size() &&
                                        std::isspace(static_cast<unsigned char>(normalized[prefix.size()])));
    };
    return starts(".tran") || starts(".dc") || starts(".ac") || starts(".op") || normalized == ".end";
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

std::filesystem::path defaultProjectPath() {
    if (const auto *configured = std::getenv("ICE_PROJECT"); configured && *configured)
        return configured;
    std::error_code status;
    auto directory = std::filesystem::current_path(status);
    if (status)
        return {};
    for (int depth = 0; depth < 4 && !directory.empty(); ++depth) {
        const auto candidate = directory / "project.icepro";
        if (std::filesystem::is_regular_file(candidate, status) && !status)
            return candidate;
        const auto parent = directory.parent_path();
        if (parent == directory)
            break;
        directory = parent;
    }
    return {};
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
        {"simulation_scripts", json::array()},
    };
    for (const auto &[id, cell] : project.schematicCells) {
        result["schematics"].push_back({{"id", id},
                                        {"name", cell.name},
                                        {"pdk", cell.pdk},
                                        {"text", cell.textSchematic},
                                        {"systemverilog", cell.systemVerilogSchematic},
                                        {"symbols", cell.symbols.size()},
                                        {"instances", cell.instances.size()},
                                        {"nets", cell.nets.size()}});
    }
    for (const auto &[id, layer] : project.layers) {
        result["layers"].push_back({{"id", id},
                                    {"name", layer.name},
                                    {"number", layer.number},
                                    {"datatype", layer.datatype},
                                    {"color", layer.color},
                                    {"pattern", layer.pattern},
                                    {"visible", layer.visible}});
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
        result["simulation_scripts"].push_back({{"id", id}, {"name", report.name}, {"path", report.path}});
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
                   {"systemverilog", cell.systemVerilogSchematic},
                   {"source", cell.sourceText()},
                   {"symbols", json::array()},
                   {"instances", json::array()},
                   {"nets", json::array()}};
    for (const auto &[id, symbol] : cell.symbols) {
        json pins = json::array();
        for (const auto &pin : symbol.pins)
            pins.push_back({{"id", pin.id},
                            {"name", pin.name},
                            {"x", pin.x},
                            {"y", pin.y},
                            {"direction", pinDirectionName(pin.direction)},
                            {"width", pin.width},
                            {"width_known", pin.widthKnown}});
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

json diagnosticJson(const ice::Diagnostic &diagnostic) {
    json item = {{"severity", diagnostic.severity == ice::Diagnostic::Severity::error ? "error" : "warning"},
                 {"code", diagnostic.code},
                 {"message", diagnostic.message}};
    if (diagnostic.object)
        item["object"] = *diagnostic.object;
    return item;
}

json fileSymbolJson(const ice::FileSymbol &symbol) {
    json pins = json::array();
    for (const auto &pin : symbol.pins)
        pins.push_back({{"name", pin.name},
                        {"direction", pinDirectionName(pin.direction)},
                        {"x", pin.x},
                        {"y", pin.y},
                        {"width", pin.width},
                        {"width_known", pin.widthKnown}});
    json defaults = json::array();
    for (const auto &parameter : symbol.defaultParameters)
        defaults.push_back({{"name", parameter.name}, {"value", parameter.value}});
    return {{"name", symbol.name},
            {"category", symbol.category},
            {"pins", std::move(pins)},
            {"defaults", std::move(defaults)}};
}

json componentPlacementJson(const ice::ComponentPlacement &placement) {
    json pins = json::array();
    for (const auto &pin : placement.pins)
        pins.push_back({{"id", pin.id},
                        {"name", pin.name},
                        {"direction", pinDirectionName(pin.direction)},
                        {"width", pin.width},
                        {"width_known", pin.widthKnown}});
    return {{"instance", placement.instance},
            {"symbol", placement.symbol},
            {"symbol_name", placement.symbolName},
            {"name", placement.instanceName},
            {"pins", std::move(pins)}};
}

json layoutInfo(const ice::Project &project, const ice::LayoutCell &cell) {
    json result = {{"id", cell.id},
                   {"name", cell.name},
                   {"pdk", cell.pdk},
                   {"stack", cell.stack},
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
        json centerline = json::array();
        for (const auto &point : shape.centerline)
            centerline.push_back({point.x, point.y});
        json segmentLayers = json::array();
        for (const auto layer : shape.segmentLayers)
            segmentLayers.push_back(layer);
        json pointVias = json::array();
        for (const auto &vias : shape.pointVias) {
            json entry = json::array();
            for (const auto layer : vias)
                entry.push_back(layer);
            pointVias.push_back(std::move(entry));
        }
        result["shapes"].push_back({{"id", id},
                                    {"layer", shape.layer},
                                    {"layer_name", layer == project.layers.end() ? "" : layer->second.name},
                                    {"is_path", shape.isPath()},
                                    {"is_text", shape.isText()},
                                    {"name", shape.name},
                                    {"text", shape.text},
                                    {"path_width", shape.width},
                                    {"centerline", std::move(centerline)},
                                    {"segment_layers", std::move(segmentLayers)},
                                    {"point_vias", std::move(pointVias)},
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
                                   {"height", port.rectangle.height},
                                   {"direction", ice::portDirectionName(port.direction)},
                                   {"side", ice::portSideName(port.side)},
                                   {"net", port.net == 0 ? json(nullptr) : json(port.net)},
                                   {"label", port.label},
                                   {"visible", port.visible},
                                   {"locked", port.locked}});
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

json buildJson(const std::filesystem::path &executablePath) {
    const auto info = ice::buildInfo();
    return {{"version", info.version},
            {"commit", info.commit},
            {"build_time", info.buildTime},
            {"build_type", info.buildType},
            {"executable", executablePath.string()},
            {"features", info.features}};
}

json measurementJson(const ice::SignalMeasurement &measurement) {
    json result = {{"name", measurement.name},
                   {"points", measurement.statistics.points},
                   {"min", measurement.statistics.min},
                   {"max", measurement.statistics.max},
                   {"amplitude", measurement.statistics.amplitude()},
                   {"mean", measurement.statistics.mean},
                   {"rms", measurement.statistics.rms},
                   {"first", measurement.statistics.first},
                   {"last", measurement.statistics.last},
                   {"threshold", measurement.threshold},
                   {"zero_crossings", measurement.zeroCrossings},
                   {"rising_crossings", measurement.risingCrossings}};
    if (measurement.period)
        result["period"] = *measurement.period;
    if (measurement.frequency)
        result["frequency"] = *measurement.frequency;
    if (measurement.settlingTime)
        result["settling_time"] = *measurement.settlingTime;
    return result;
}

json doctorJson(const std::filesystem::path &executablePath, const std::optional<std::filesystem::path> &projectPath,
                bool probe = false) {
    const auto runner = ice::reportRunnerScriptPath(executablePath);
    const auto python = ice::reportPythonExecutable();
    const bool pythonAvailable = !python.empty() && std::system((python + " --version >/dev/null 2>&1").c_str()) == 0;
    bool ngspiceLibrary = false;
    for (const auto *candidate :
         {"/usr/lib/libngspice.so.0", "/usr/lib64/libngspice.so.0", "/usr/lib/x86_64-linux-gnu/libngspice.so.0"}) {
        std::error_code status;
        if (std::filesystem::exists(candidate, status) && !status) {
            ngspiceLibrary = true;
            break;
        }
    }
    const bool ngspiceExecutable = std::system("ngspice --version >/dev/null 2>&1") == 0;

    const auto roots = ice::defaultPdkRoots();
    const auto pdks = ice::discoverPdks(roots);
    json pdkArray = json::array();
    json simulationLibraries = json::array();
    json readyPdks = json::array();
    bool pdksReady = false;
    bool allPdksReady = !pdks.empty();
    for (const auto &pdk : pdks) {
        pdkArray.push_back(ice::pdkInfoJson(pdk, false));
        pdksReady = pdksReady || pdk.simulationReady;
        allPdksReady = allPdksReady && pdk.simulationReady;
        if (pdk.simulationReady)
            readyPdks.push_back(pdk.id);
        for (const auto &file : pdk.modelFiles) {
            simulationLibraries.push_back(
                {{"pdk", pdk.id}, {"version", pdk.version}, {"path", file.path.string()}, {"sections", file.sections}});
        }
    }

    std::vector<std::string> timingLibraries;
    for (const auto &root : roots) {
        std::error_code status;
        if (!std::filesystem::is_directory(root, status) || status)
            continue;
        for (std::filesystem::recursive_directory_iterator it(root, status), end; !status && it != end;
             it.increment(status)) {
            if (it.depth() > 8) {
                it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file() || it->path().extension() != ".lib")
                continue;
            if (it->path().string().find("/libs.ref/") == std::string::npos)
                continue;
            timingLibraries.push_back(it->path().string());
            if (timingLibraries.size() >= 50)
                break;
        }
        if (timingLibraries.size() >= 50)
            break;
    }

    json result = {{"build", buildJson(executablePath)},
                   {"report_runner", runner.empty() ? json(nullptr) : json(runner.string())},
                   {"python", python},
                   {"python_available", pythonAvailable},
                   {"ngspice_executable_available", ngspiceExecutable},
                   {"ngspice_library_available", ngspiceLibrary},
                   {"pdk_roots", json::array()},
                   {"pdks", std::move(pdkArray)},
                   {"pdks_ready", pdksReady},
                   {"all_pdks_ready", allPdksReady},
                   {"ready_pdks", std::move(readyPdks)},
                   {"simulation_model_libraries", std::move(simulationLibraries)},
                   {"timing_libraries", std::move(timingLibraries)}};
    for (const auto &root : roots)
        result["pdk_roots"].push_back(root.string());

    bool ok = !runner.empty() && pythonAvailable && (ngspiceExecutable || ngspiceLibrary);
    if (projectPath) {
        const auto project = ice::Project::load(*projectPath);
        std::set<std::string> ids;
        for (const auto &[_, cell] : project.schematicCells)
            if (!cell.pdk.empty() && cell.pdk != ice::genericPdkId)
                ids.insert(cell.pdk);
        for (const auto &[_, cell] : project.layoutCells)
            if (!cell.pdk.empty() && cell.pdk != ice::genericPdkId)
                ids.insert(cell.pdk);
        for (const auto &[_, cell] : project.proceduralLayouts)
            if (!cell.pdk.empty() && cell.pdk != ice::genericPdkId)
                ids.insert(cell.pdk);
        json checks = json::array();
        bool projectReady = true;
        std::string missingSummary;
        for (const auto &id : ids) {
            const auto matches = ice::findPdks(id);
            if (matches.empty()) {
                projectReady = false;
                checks.push_back({{"id", id}, {"simulation_ready", false}, {"missing", {"PDK package not installed"}}});
                if (!missingSummary.empty())
                    missingSummary += "; ";
                missingSummary += id + ": PDK package not installed";
                continue;
            }
            for (const auto &pdk : matches) {
                auto item = ice::pdkInfoJson(pdk, false);
                projectReady = projectReady && pdk.simulationReady;
                if (probe) {
                    const auto probeResult = ice::probePdk(pdk);
                    item["probe"] = {{"attempted", probeResult.attempted},
                                     {"ok", probeResult.ok},
                                     {"summary", probeResult.summary},
                                     {"log", probeResult.log}};
                    projectReady = projectReady && probeResult.ok;
                    if (!probeResult.ok && !probeResult.summary.empty()) {
                        if (!missingSummary.empty())
                            missingSummary += "; ";
                        missingSummary += id + ": " + probeResult.summary;
                    }
                }
                if (!pdk.simulationReady) {
                    for (const auto &missing : pdk.missing) {
                        if (!missingSummary.empty())
                            missingSummary += "; ";
                        missingSummary += id + ": " + missing;
                    }
                }
                checks.push_back(std::move(item));
            }
        }
        result["project"] = projectPath->string();
        result["pdk_checks"] = std::move(checks);
        result["pdk_ready"] = projectReady;
        if (!projectReady) {
            ok = false;
            result["pdk_hint"] = "The project PDK is not simulation-ready: " + missingSummary;
        }
    }
    result["ok"] = ok;
    return result;
}

void printJson(const json &value) { std::cout << value.dump(2) << "\n"; }

void printHelp() {
    std::cout << "ice-cli " << ice::version() << "\n\n"
              << "Usage: ice-cli <command> [arguments]\n\n"
              << "Project:\n"
              << "  project-create <folder|project.icepro> [name]\n"
              << "  project-info [folder|project.icepro]  (ICE_PROJECT or cwd search when omitted)\n"
              << "  project-validate <folder|project.icepro>\n"
              << "  project-repair <folder|project.icepro>\n"
              << "  project-migrate <folder|project.icepro>\n"
              << "  doctor [project] [--probe]\n"
              << "  pdk-info [pdk] [--probe]\n"
              << "  pdk-build <pdk-or-package>\n\n"
              << "Schematic:\n"
              << "  schematic-add-cell <project> <name> [pdk]\n"
              << "  schematic-add-text <project> <name> [pdk] [spice-file]\n"
              << "      or: schematic-add-text <project> <name> --pdk <pdk> --file <spice-file>\n"
              << "  schematic-list <project>\n"
              << "  schematic-info <project> <cell>\n"
              << "  schematic-add-symbol <project> <cell> <name> [source-cell] [--blank]\n"
              << "  schematic-add-pin <project> <cell> <symbol> <name> [x] [y] [direction]\n"
              << "  schematic-add-instance <project> <cell> <symbol> <name> <x> <y>\n"
              << "  schematic-add-component <project> <cell> <type> [name] [x] [y] [param=value ...]\n"
              << "  schematic-list-symbols [pdk] [query]\n"
              << "  schematic-add-circuit <project> <cell> <json-file|->\n"
              << "  schematic-set-parameter <project> <cell> <instance> <name> <value>\n"
              << "  schematic-add-net <project> <cell> <name> <pin> <pin> ...\n"
              << "  schematic-validate <project> <cell>\n"
              << "  schematic-import-spice <project> <name> [--pdk <pdk>] [--mode text|drawn|auto] <file|->\n"
              << "  schematic-remove <project> <cell> [--dry-run] [--cascade]\n"
              << "  schematic-hdl-ports <project> <cell>\n"
              << "  schematic-add-hdl-instance <project> <cell> <source-cell> <name> <x> <y> "
                 "[--flatten-buses]\n"
              << "  schematic-validate-hierarchy <project> <cell>\n"
              << "  schematic-netlist <project> <cell>\n\n"
              << "Layout:\n"
              << "  layout-add-cell <project> <name> [pdk] [stack]\n"
              << "  layout-add-procedural <project> <name> [pdk] [script-file] [stack]\n"
              << "  layout-create-from-schematic <project> <schematic> [layout-name]\n"
              << "  layout-list <project>\n"
              << "  layout-info <project> <cell|procedural>\n"
              << "  layout-remove <project> <cell|procedural>\n"
              << "  layout-add-layer <project> <name> <number> [datatype]\n"
              << "  layout-add-rectangle <project> <cell> <layer> <x> <y> <width> <height>\n"
              << "  layout-add-text <project> <cell> <layer> <x> <y> <text> [height]\n"
              << "  layout-add-polygon <project> <cell> <layer> <x,y> <x,y> <x,y> ...\n"
              << "  layout-add-port <project> <cell> <name> <layer> <x> <y> <width> <height> [pin options]\n"
              << "  layout-list-ports <project> <cell>\n"
              << "  layout-update-port <project> <cell> <port> [pin options]\n"
              << "  layout-remove-port <project> <cell> <port>\n"
              << "  layout-add-net <project> <cell> <name>\n"
              << "  layout-assign-port-net <project> <cell> <port> <net|none>\n"
              << "  layout-add-instance <project> <parent> <child> <name> <x> <y> [orientation]\n"
              << "  layout-connectivity <project> <cell>\n"
              << "  layout-export-gds <project> <cell> <output.gds>\n"
              << "  layout-export-lef <project> <cell> <output.lef>\n"
              << "  layout-export-def <project> <cell> <output.def>\n"
              << "  layout-import-gds <input.gds> <project> [pin-label-layer]\n"
              << "  layout-import-lef <input.lef> <project>\n"
              << "  layout-import-def <input.def> <project>\n\n"
              << "Verification:\n"
              << "  lvs <project> <schematic> <layout>\n"
              << "  lvs-auto <project> <layout>\n"
              << "  pex <project> <layout> [schematic-name]\n\n"
              << "Simulation:\n"
              << "  simulation-add <project> <name> <schematic> [analysis] [stop] [step] "
                 "[signals...]\n"
              << "  simulation-list <project>\n"
              << "  simulation-remove <project> <simulation>\n"
              << "  simulation-run <project> <simulation> [timeout-ms] [output.json]\n"
              << "  simulation-trace <project> <simulation> [signals] [max-points]\n"
              << "  simulation-measure <project> <simulation> [signal] [threshold]\n"
              << "  simulation-graph-add <project> <simulation> <name> [x-axis] [y-signals]\n"
              << "  simulation-graph-list <project> <simulation>\n"
              << "  simulation-graph-info <project> <simulation> <graph>\n"
              << "  simulation-graph-update <project> <simulation> <graph> [name] [x-axis] [y-signals]\n"
              << "  simulation-graph-limits <project> <simulation> <graph> [x-min] [x-max] [y-min] [y-max]\n"
              << "  simulation-graph-delete <project> <simulation> <graph>\n"
              << "  simulate <project> <schematic> [stop] [step] [timeout-ms] [signals] [output.json]\n"
              << "  simulate-netlist <netlist-file> [stop] [step] [timeout-ms] [signals] [output.json]\n"
              << "  (drawn schematics with SystemVerilog blocks use the mixed simulator automatically)\n\n"
              << "Simulation scripts:\n"
              << "  script-add <project> <name> [source-file]\n"
              << "  script-list <project>\n"
              << "  script-info <project> <script>\n"
              << "  script-read <project> <script>\n"
              << "  script-write <project> <script> <source-file|->\n"
              << "  script-run <project> <script> [timeout-ms] [--json]\n"
              << "  script-delete <project> <script> [--purge]\n\n"
              << "Simulation scripts may be referenced by name or id. Use '-' to read script source from stdin.\n";
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
    auto command = args.front();
    auto executablePath = ice::currentExecutablePath();
    if (executablePath.empty())
        executablePath = std::filesystem::absolute(argv[0]);

    if (command == "--version" || command == "version") {
        auto version = buildJson(executablePath);
        for (const auto *feature : {"simulation_measure", "pdk_info"})
            version["features"].push_back(feature);
        printJson(version);
        return 0;
    }

    try {
        if (command == "doctor") {
            expectArguments(args, 1, 3, "doctor [project] [--probe]");
            std::optional<std::filesystem::path> projectPath;
            bool probe = false;
            for (std::size_t index = 1; index < args.size(); ++index) {
                if (args[index] == "--probe")
                    probe = true;
                else if (!projectPath)
                    projectPath = args[index];
                else
                    argumentError("Usage: ice-cli doctor [project] [--probe]");
            }
            const auto report = doctorJson(executablePath, projectPath, probe);
            printJson(report);
            return report.value("ok", false) ? 0 : 1;
        }
        if (command == "pdk-info") {
            expectArguments(args, 1, 3, "pdk-info [pdk] [--probe]");
            std::string requested;
            bool probe = false;
            for (std::size_t index = 1; index < args.size(); ++index) {
                if (args[index] == "--probe")
                    probe = true;
                else if (requested.empty())
                    requested = args[index];
                else
                    argumentError("Usage: ice-cli pdk-info [pdk] [--probe]");
            }
            auto pdks = requested.empty() ? ice::discoverPdks() : ice::findPdks(requested);
            if (requested.empty() && pdks.empty()) {
                printJson({{"pdks", json::array()}});
                return 0;
            }
            if (pdks.empty()) {
                std::string available;
                for (const auto &candidate : ice::discoverPdks()) {
                    if (!available.empty())
                        available += ", ";
                    available += candidate.id;
                }
                argumentError("Unknown PDK '" + requested +
                              "'. Available PDKs: " + (available.empty() ? "none" : available));
            }
            json result = {{"pdks", json::array()}};
            for (const auto &pdk : pdks) {
                auto item = ice::pdkInfoJson(pdk, true);
                if (probe) {
                    const auto probeResult = ice::probePdk(pdk);
                    item["probe"] = {{"attempted", probeResult.attempted},
                                     {"ok", probeResult.ok},
                                     {"summary", probeResult.summary},
                                     {"log", probeResult.log}};
                }
                result["pdks"].push_back(std::move(item));
            }
            printJson(result);
            return 0;
        }
        if (command == "pdk-build") {
            expectArguments(args, 2, 2, "pdk-build <pdk-or-package>");
            const auto pdks = ice::findPdks(args[1]);
            if (pdks.empty())
                argumentError("Unknown ICE PDK package: " + args[1]);
            const auto result = ice::buildPdkOsdi(pdks.front());
            printJson({{"ok", result.ok},
                       {"summary", result.summary},
                       {"compiler", result.compiler},
                       {"osdi_dir", result.osdiDir.string()},
                       {"log", result.log}});
            return result.ok ? 0 : 1;
        }
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
            expectArguments(args, 1, 2, "project-info [folder|project.icepro]");
            const auto path = args.size() == 2 ? std::filesystem::path(args[1]) : defaultProjectPath();
            if (path.empty())
                argumentError("No project path was given and no project.icepro was found; pass a path or set "
                              "ICE_PROJECT");
            const auto project = ice::Project::load(path);
            printJson(projectSummary(project, path.string()));
            return 0;
        }

        if (command == "project-validate") {
            expectArguments(args, 2, 2, "project-validate <folder|project.icepro>");
            const auto project = ice::Project::load(args[1]);
            const auto diagnostics = diagnosticsJson(project);
            printJson({{"valid", diagnostics.empty()}, {"diagnostics", diagnostics}});
            return 0;
        }

        if (command == "lvs") {
            expectArguments(args, 4, 4, "lvs <project> <schematic> <layout>");
            const auto project = ice::Project::load(args[1]);
            const auto schematic = resolveSchematic(project, args[2]);
            const auto layout = resolveLayout(project, args[3]);
            const auto report = ice::runLvs(project, schematic, layout);
            std::cout << ice::serializeLvsReport(report);
            return report.matched() ? 0 : 1;
        }

        if (command == "lvs-auto") {
            expectArguments(args, 3, 3, "lvs-auto <project> <layout>");
            const auto project = ice::Project::load(args[1]);
            const auto layout = resolveLayout(project, args[2]);
            const auto schematic = ice::schematicForLayout(project, layout);
            if (schematic == 0)
                throw ice::ProjectError(
                    "lvs.schematic", "Unable to infer a schematic for this layout; use lvs with an explicit schematic");
            const auto report = ice::runLvs(project, schematic, layout);
            std::cout << ice::serializeLvsReport(report);
            return report.matched() ? 0 : 1;
        }

        if (command == "pex") {
            expectArguments(args, 3, 4, "pex <project> <layout> [schematic-name]");
            ice::ProjectSession session(ice::Project::load(args[1]));
            const auto layout = resolveLayout(session.project(), args[2]);
            ice::PexOptions options;
            if (args.size() == 4)
                options.schematicName = args[3];
            const auto report = ice::runPex(session.project(), layout, options);
            session.save(args[1]);
            std::cout << ice::serializePexReport(report);
            return 0;
        }

        if (command == "project-repair") {
            expectArguments(args, 2, 2, "project-repair <folder|project.icepro>");
            ice::ProjectLoadOptions options;
            options.recover = true;
            ice::ProjectLoadReport loadReport;
            auto project = ice::Project::load(args[1], options, &loadReport);
            ice::ProjectSession session(std::move(project));
            session.save(args[1]);
            printJson({{"repaired", true},
                       {"warnings", loadReport.warnings},
                       {"quarantined_files", loadReport.quarantinedFiles}});
            return 0;
        }

        if (command == "project-migrate") {
            expectArguments(args, 2, 2, "project-migrate <folder|project.icepro>");
            const auto root = projectRootForCli(args[1]);
            const auto agentsPath = root / "AGENTS.md";
            const auto readAgents = [&] {
                std::ifstream input(agentsPath, std::ios::binary);
                return std::string((std::istreambuf_iterator<char>(input)), {});
            };
            const auto before = readAgents();
            auto project = ice::Project::load(args[1]);
            const bool pdkIdsUpdated = ice::migrateProjectPdkIds(project);
            ice::ProjectSession session(std::move(project));
            session.save(args[1]);
            const auto after = readAgents();
            printJson({{"migrated", true},
                       {"agents_updated", before != after},
                       {"pdk_ids_updated", pdkIdsUpdated},
                       {"guidance_version", 3}});
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
            expectArguments(args, 3, static_cast<std::size_t>(-1),
                            "schematic-add-text <project> <name> [pdk] [spice-file] "
                            "or --pdk <pdk> --file <spice-file>");
            auto pdk = std::string(ice::genericPdkId);
            std::string spiceFile;
            bool pdkSpecified = false;
            if (args.size() > 3 && std::string_view(args[3]).starts_with("--")) {
                for (std::size_t index = 3; index < args.size(); ++index) {
                    if (args[index] == "--pdk" && index + 1 < args.size()) {
                        pdk = args[++index];
                        pdkSpecified = true;
                    } else if (args[index] == "--file" && index + 1 < args.size()) {
                        spiceFile = args[++index];
                    } else {
                        argumentError("Expected --pdk <pdk> or --file <spice-file>");
                    }
                }
            } else if (args.size() == 4) {
                std::error_code status;
                if (std::filesystem::is_regular_file(args[3], status) && !status) {
                    spiceFile = args[3];
                } else if (args[3].find('/') != std::string::npos ||
                           std::filesystem::path(args[3]).extension() == ".sp") {
                    argumentError("'" + args[3] +
                                  "' was interpreted as a PDK because no --pdk was given. "
                                  "Use --pdk <pdk> --file <spice-file> or pass the PDK before the file.");
                } else {
                    pdk = args[3];
                    pdkSpecified = true;
                }
            } else if (args.size() == 5) {
                pdk = args[3];
                spiceFile = args[4];
                pdkSpecified = true;
            } else if (args.size() > 5) {
                argumentError("Too many positional arguments; use --pdk <pdk> --file <spice-file>");
            }
            ice::ProjectSession session(ice::Project::load(args[1]));
            auto text = spiceFile.empty() ? "* SPICE schematic: " + args[2] + "\n.title " + args[2] + "\n.end\n"
                                          : readTextFile(spiceFile);
            ice::ObjectId id = 0;
            session.execute("schematic-add-text", [&](ice::Project &project) {
                id = ice::createTextSchematicCell(project, args[2], pdk, text);
            });
            session.save(args[1]);
            json result = {{"id", id}, {"name", args[2]}, {"pdk", pdk}, {"text", text}};
            if (!pdkSpecified)
                result["warning"] = "No PDK was specified; the text schematic was stored with the generic PDK. "
                                    "Pass --pdk <pdk> if this cell belongs to a PDK-specific design.";
            printJson(result);
            return 0;
        }

        if (command == "schematic-add-sv") {
            expectArguments(args, 3, 5, "schematic-add-sv <project> <name> [pdk] [source-file]");
            auto pdk = std::string(ice::genericPdkId);
            std::string sourceFile;
            if (args.size() == 4) {
                std::error_code status;
                if (std::filesystem::is_regular_file(args[3], status) && !status)
                    sourceFile = args[3];
                else
                    pdk = args[3];
            } else if (args.size() == 5) {
                pdk = args[3];
                sourceFile = args[4];
            }
            ice::ProjectSession session(ice::Project::load(args[1]));
            const auto moduleName = verilogIdentifier(args[2]);
            const auto source = sourceFile.empty()
                                    ? "// SystemVerilog block: " + args[2] + "\nmodule " + moduleName +
                                          " (\n    input  logic clk,\n    input  logic rst,\n    output logic out\n);\n"
                                          "    assign out = 1'b0;\nendmodule\n"
                                    : readTextFile(sourceFile);
            ice::ObjectId id = 0;
            session.execute("schematic-add-systemverilog", [&](ice::Project &project) {
                id = ice::createSystemVerilogCell(project, args[2], pdk, source);
            });
            session.save(args[1]);
            printJson({{"id", id}, {"name", args[2]}, {"pdk", pdk}, {"source", source}});
            return 0;
        }

        if (command == "schematic-add-component") {
            expectArguments(args, 4, static_cast<std::size_t>(-1),
                            "schematic-add-component <project> <cell> <type> [name] [x] [y] "
                            "[--symbol <name>] [param=value ...]");
            auto project = ice::Project::load(args[1]);
            const auto cell = resolveSchematic(project, args[2]);
            ice::ComponentRequest request;
            request.type = args[3];
            std::vector<std::string> positional;
            for (std::size_t index = 4; index < args.size(); ++index) {
                if (args[index] == "--symbol" && index + 1 < args.size()) {
                    request.symbol = args[++index];
                    continue;
                }
                const auto equals = args[index].find('=');
                if (equals != std::string::npos) {
                    request.parameters.push_back({0, args[index].substr(0, equals), args[index].substr(equals + 1)});
                } else {
                    positional.push_back(args[index]);
                }
            }
            if (!positional.empty())
                request.name = positional[0];
            if (positional.size() > 1)
                request.x = parseInteger(positional[1], "x");
            if (positional.size() > 2)
                request.y = parseInteger(positional[2], "y");
            if (positional.size() > 3)
                argumentError("Too many positional arguments for schematic-add-component");
            ice::ProjectSession session(project);
            ice::ComponentPlacement placement;
            session.execute("schematic-add-component",
                            [&](ice::Project &p) { placement = ice::addSchematicComponent(p, cell, request); });
            session.save(args[1]);
            auto result = componentPlacementJson(placement);
            result["cell"] = cell;
            printJson(result);
            return 0;
        }

        if (command == "schematic-list-symbols") {
            expectArguments(args, 1, 3, "schematic-list-symbols [pdk] [query]");
            const auto pdk = args.size() >= 2 ? args[1] : std::string(ice::genericPdkId);
            const auto query = args.size() >= 3 ? args[2] : std::string{};
            const auto symbols = ice::availableSymbols(pdk);
            const auto loweredQuery = [&] {
                auto value = query;
                std::transform(value.begin(), value.end(), value.begin(),
                               [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
                return value;
            }();
            json result = json::array();
            for (const auto &symbol : symbols) {
                const auto lowerName = [&] {
                    auto value = symbol.name;
                    std::transform(value.begin(), value.end(), value.begin(),
                                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
                    return value;
                }();
                const auto lowerCategory = [&] {
                    auto value = symbol.category;
                    std::transform(value.begin(), value.end(), value.begin(),
                                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
                    return value;
                }();
                if (!loweredQuery.empty() && lowerName.find(loweredQuery) == std::string::npos &&
                    lowerCategory.find(loweredQuery) == std::string::npos)
                    continue;
                result.push_back(fileSymbolJson(symbol));
            }
            printJson({{"pdk", pdk}, {"symbols", std::move(result)}});
            return 0;
        }

        if (command == "schematic-add-circuit") {
            expectArguments(args, 4, 4, "schematic-add-circuit <project> <cell> <json-file|->");
            auto project = ice::Project::load(args[1]);
            const auto cell = resolveSchematic(project, args[2]);
            const auto text = args[3] == "-" ? readStdin() : readTextFile(args[3]);
            const auto document = json::parse(text, nullptr, false);
            if (document.is_discarded() || !document.is_object())
                argumentError("Circuit JSON must be an object with components and optional nets");
            ice::CircuitRequest request;
            const auto &components = document.at("components");
            if (!components.is_array())
                argumentError("Circuit JSON 'components' must be an array");
            for (const auto &entry : components) {
                ice::ComponentRequest component;
                component.type = entry.value("type", std::string{});
                component.symbol = entry.value("symbol", std::string{});
                component.name = entry.value("name", std::string{});
                component.x = entry.value("x", ice::Coordinate{0});
                component.y = entry.value("y", ice::Coordinate{0});
                const auto addParameters = [&](const json &parameters) {
                    for (const auto &[name, value] : parameters.items()) {
                        if (value.is_string())
                            component.parameters.push_back({0, name, value.get<std::string>()});
                        else if (value.is_number() || value.is_boolean())
                            component.parameters.push_back({0, name, value.dump()});
                        else
                            argumentError("Component parameter values must be strings, numbers, or booleans");
                    }
                };
                if (entry.contains("parameters"))
                    addParameters(entry.at("parameters"));
                else if (entry.contains("params"))
                    addParameters(entry.at("params"));
                request.components.push_back(std::move(component));
            }
            if (document.contains("nets")) {
                for (const auto &entry : document.at("nets")) {
                    ice::CircuitNetRequest net;
                    net.name = entry.value("name", std::string{});
                    net.x = entry.value("x", ice::Coordinate{0});
                    net.y = entry.value("y", ice::Coordinate{0});
                    for (const auto &pin : entry.at("pins")) {
                        if (pin.is_string())
                            net.pins.push_back(pin.get<std::string>());
                        else if (pin.is_number_integer())
                            net.pins.push_back(std::to_string(pin.get<std::int64_t>()));
                        else if (pin.is_number_unsigned())
                            net.pins.push_back(std::to_string(pin.get<std::uint64_t>()));
                        else
                            argumentError("Circuit net pins must be strings or integer pin ids");
                    }
                    request.nets.push_back(std::move(net));
                }
            }
            ice::ProjectSession session(project);
            ice::CircuitPlacement placement;
            session.execute("schematic-add-circuit",
                            [&](ice::Project &p) { placement = ice::addSchematicCircuit(p, cell, request); });
            session.save(args[1]);
            json componentsJson = json::array();
            for (const auto &component : placement.components)
                componentsJson.push_back(componentPlacementJson(component));
            printJson({{"cell", cell}, {"components", std::move(componentsJson)}, {"nets", placement.nets}});
            return 0;
        }

        if (command == "schematic-validate") {
            expectArguments(args, 3, 3, "schematic-validate <project> <cell>");
            const auto project = ice::Project::load(args[1]);
            const auto cell = resolveSchematic(project, args[2]);
            const auto diagnostics = ice::validateSchematic(project, cell);
            json items = json::array();
            bool ok = true;
            for (const auto &diagnostic : diagnostics) {
                if (diagnostic.severity == ice::Diagnostic::Severity::error)
                    ok = false;
                auto item = diagnosticJson(diagnostic);
                json suggestions = json::array();
                if (diagnostic.code.starts_with("schematic.pdk")) {
                    suggestions.push_back(
                        {{"command", "pdk-info"}, {"arguments", json::array({project.schematicCells.at(cell).pdk})}});
                } else if (diagnostic.code.starts_with("schematic.symbol")) {
                    suggestions.push_back(
                        {{"command", "schematic-info"}, {"arguments", json::array({args[1], args[2]})}});
                } else if (diagnostic.code.starts_with("hdl.")) {
                    suggestions.push_back(
                        {{"command", "schematic-validate-hierarchy"}, {"arguments", json::array({args[1], args[2]})}});
                } else {
                    suggestions.push_back(
                        {{"command", "schematic-info"}, {"arguments", json::array({args[1], args[2]})}});
                }
                item["suggested_calls"] = std::move(suggestions);
                items.push_back(std::move(item));
            }
            printJson(
                {{"schematic", project.schematicCells.at(cell).name}, {"ok", ok}, {"diagnostics", std::move(items)}});
            return 0;
        }

        if (command == "schematic-import-spice") {
            expectArguments(args, 4, static_cast<std::size_t>(-1),
                            "schematic-import-spice <project> <name> [--pdk <pdk>] "
                            "[--mode text|drawn|auto] <source-file|->");
            auto pdk = std::string(ice::genericPdkId);
            auto mode = std::string{"text"};
            std::string file;
            for (std::size_t index = 3; index < args.size(); ++index) {
                if (args[index] == "--pdk" && index + 1 < args.size())
                    pdk = args[++index];
                else if (args[index] == "--mode" && index + 1 < args.size())
                    mode = args[++index];
                else if (file.empty())
                    file = args[index];
                else
                    argumentError("Too many schematic-import-spice arguments");
            }
            if (file.empty())
                argumentError("A SPICE source file or '-' is required");
            if (mode != "text" && mode != "drawn" && mode != "auto")
                argumentError("--mode must be text, drawn, or auto");
            const auto source = file == "-" ? readStdin() : readTextFile(file);
            const bool drawn = mode == "drawn" || (mode == "auto" && ice::spiceNetlistSupportsDrawn(source));
            ice::ProjectSession session(ice::Project::load(args[1]));
            ice::SpiceImportResult imported;
            session.execute("schematic-import-spice", [&](ice::Project &project) {
                imported = ice::createSchematicFromSpice(project, args[2], pdk, source, drawn);
            });
            session.save(args[1]);
            printJson({{"id", imported.cell},
                       {"name", args[2]},
                       {"pdk", pdk},
                       {"mode", drawn ? "drawn" : "text"},
                       {"warnings", imported.warnings}});
            return 0;
        }

        if (command == "schematic-hdl-ports") {
            expectArguments(args, 3, 3, "schematic-hdl-ports <project> <cell>");
            const auto project = ice::Project::load(args[1]);
            const auto id = resolveSchematic(project, args[2]);
            const auto &cell = project.schematicCells.at(id);
            if (!cell.systemVerilogSchematic)
                throw ice::ProjectError("schematic.hdl", "Schematic cell is not a SystemVerilog source cell");
            const auto parsed = ice::parseSystemVerilogModule(cell);
            if (!parsed.ok)
                throw ice::ProjectError("schematic.hdl.parse", parsed.error);
            const auto directionName = [](ice::HdlPortDirection direction) {
                switch (direction) {
                case ice::HdlPortDirection::input:
                    return "input";
                case ice::HdlPortDirection::output:
                    return "output";
                case ice::HdlPortDirection::inout:
                    return "inout";
                }
                return "input";
            };
            json ports = json::array();
            for (const auto &port : parsed.module.ports)
                ports.push_back({{"name", port.name},
                                 {"direction", directionName(port.direction)},
                                 {"width", port.width},
                                 {"width_known", port.widthKnown},
                                 {"real", port.real}});
            printJson({{"id", id},
                       {"schematic", cell.name},
                       {"module", parsed.module.name},
                       {"ports", std::move(ports)},
                       {"warnings", parsed.warnings}});
            return 0;
        }

        if (command == "schematic-add-hdl-instance") {
            expectArguments(args, 7, 9,
                            "schematic-add-hdl-instance <project> <cell> <source-cell> <name> <x> <y> "
                            "[--flatten-buses]");
            auto project = ice::Project::load(args[1]);
            const auto parent = resolveSchematic(project, args[2]);
            const auto source = resolveSchematic(project, args[3]);
            const auto x = parseInteger(args[5], "x");
            const auto y = parseInteger(args[6], "y");
            bool flattenBuses = false;
            for (std::size_t index = 7; index < args.size(); ++index) {
                if (args[index] == "--flatten-buses")
                    flattenBuses = true;
                else if (args[index] == "--bus-mode" && index + 1 < args.size()) {
                    const auto mode = args[++index];
                    if (mode != "scalar" && mode != "flatten")
                        argumentError("--bus-mode must be scalar or flatten");
                    flattenBuses = mode == "flatten";
                } else if (args[index] != "scalar")
                    argumentError("Expected --flatten-buses or --bus-mode <scalar|flatten>");
            }
            ice::ProjectSession session(project);
            ice::ObjectId instance = 0;
            ice::ObjectId symbol = 0;
            session.execute("schematic-add-hdl-instance", [&](ice::Project &p) {
                instance = ice::addSystemVerilogInstance(p, parent, source, args[4], x, y, &symbol, flattenBuses);
            });
            session.save(args[1]);
            json pins = json::array();
            const auto &symbolRef = session.project().schematicCells.at(parent).symbols.at(symbol);
            for (const auto &pin : symbolRef.pins) {
                pins.push_back({{"id", pin.id},
                                {"name", pin.name},
                                {"direction", pinDirectionName(pin.direction)},
                                {"width", pin.width},
                                {"width_known", pin.widthKnown}});
            }
            printJson({{"id", instance},
                       {"cell", parent},
                       {"source_cell", source},
                       {"symbol", symbol},
                       {"symbol_name", symbolRef.name},
                       {"name", args[4]},
                       {"x", x},
                       {"y", y},
                       {"pins", std::move(pins)}});
            return 0;
        }

        if (command == "schematic-validate-hierarchy") {
            expectArguments(args, 3, 3, "schematic-validate-hierarchy <project> <cell>");
            const auto project = ice::Project::load(args[1]);
            const auto id = resolveSchematic(project, args[2]);
            const auto plan = ice::buildMixedSignalPlan(project, id);
            const auto diagnostics = ice::validateHdlHierarchy(project, id);
            json checked = json::array();
            for (const auto &diagnostic : diagnostics) {
                json item = {
                    {"severity", diagnostic.severity == ice::Diagnostic::Severity::error ? "error" : "warning"},
                    {"code", diagnostic.code},
                    {"message", diagnostic.message}};
                if (diagnostic.object)
                    item["object"] = *diagnostic.object;
                checked.push_back(std::move(item));
            }
            json blocks = json::array();
            for (const auto &binding : plan.hdlInstances) {
                json ports = json::array();
                for (const auto &port : binding.ports) {
                    const auto node = binding.portNodes.find(port.name);
                    ports.push_back({{"name", port.name},
                                     {"width", port.width},
                                     {"node", node == binding.portNodes.end() ? json(nullptr) : json(node->second)}});
                }
                blocks.push_back({{"instance", binding.instanceName},
                                  {"module", binding.moduleName},
                                  {"source_cell", binding.sourceCell},
                                  {"ports", std::move(ports)}});
            }
            json result = {{"schematic", project.schematicCells.at(id).name},
                           {"ok", plan.ok()},
                           {"diagnostics", std::move(checked)},
                           {"hdl_instances", std::move(blocks)},
                           {"analog_inputs", plan.analogInputNodes},
                           {"digital_outputs", plan.digitalOutputNodes},
                           {"warnings", plan.warnings}};
            if (plan.ok())
                result["analog_netlist"] = plan.analogNetlist;
            else
                result["error"] = plan.error;
            printJson(result);
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
                                  {"systemverilog", cell.systemVerilogSchematic},
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
            const auto &cell = project.schematicCells.at(id);
            auto result = schematicInfo(cell);
            try {
                const auto setup = ice::pdkSimulationSetupFor(cell.pdk);
                result["pdk_setup"] = {
                    {"pdk", cell.pdk}, {"lines", setup.netlistLines}, {"init_commands", setup.initCommands}};
            } catch (const ice::ProjectError &error) {
                result["pdk_setup"] = {{"pdk", cell.pdk}, {"warning", error.what()}};
            }
            printJson(result);
            return 0;
        }

        if (command == "schematic-remove") {
            expectArguments(args, 3, 5, "schematic-remove <project> <cell> [--dry-run] [--cascade]");
            bool dryRun = false;
            bool cascade = false;
            for (std::size_t index = 3; index < args.size(); ++index) {
                if (args[index] == "--dry-run")
                    dryRun = true;
                else if (args[index] == "--cascade")
                    cascade = true;
                else
                    argumentError("Expected --dry-run or --cascade");
            }
            auto project = ice::Project::load(args[1]);
            const auto id = resolveSchematic(project, args[2]);
            const auto plan = ice::planSchematicDeletion(project, id);
            json simulations = json::array();
            for (const auto &simulation : plan.simulations)
                simulations.push_back({{"id", simulation.id}, {"name", simulation.name}});
            json symbols = json::array();
            for (const auto &dependency : plan.symbols)
                symbols.push_back({{"parent_cell", dependency.parentCell},
                                   {"parent_name", dependency.parentName},
                                   {"symbol", dependency.symbol},
                                   {"symbol_name", dependency.symbolName},
                                   {"instances", dependency.instances}});
            if (dryRun) {
                printJson({{"id", id},
                           {"name", plan.name},
                           {"deleted", false},
                           {"dry_run", true},
                           {"blocked", plan.blocked()},
                           {"simulations", std::move(simulations)},
                           {"symbols", std::move(symbols)}});
                return 0;
            }
            if (plan.blocked() && !cascade) {
                printJson({{"id", id},
                           {"name", plan.name},
                           {"deleted", false},
                           {"blocked", true},
                           {"simulations", std::move(simulations)},
                           {"symbols", std::move(symbols)}});
                return 0;
            }
            ice::ProjectSession session(project);
            session.execute("remove schematic", [&](ice::Project &p) {
                if (cascade)
                    ice::removeSchematicCascade(p, id);
                else
                    ice::removeSchematicCell(p, id);
            });
            session.save(args[1]);
            printJson({{"id", id}, {"name", plan.name}, {"deleted", true}, {"cascaded", cascade}});
            return 0;
        }

        if (command == "schematic-add-symbol") {
            expectArguments(args, 4, 6, "schematic-add-symbol <project> <cell> <name> [source-cell] [--blank]");
            const auto project = ice::Project::load(args[1]);
            const auto cell = resolveSchematic(project, args[2]);
            bool blank = false;
            ice::ObjectId source = 0;
            for (std::size_t index = 4; index < args.size(); ++index) {
                if (args[index] == "--blank")
                    blank = true;
                else if (source == 0)
                    source = resolveSchematic(project, args[index]);
                else
                    argumentError("Unexpected schematic-add-symbol argument: " + args[index]);
            }
            std::string artwork;
            std::vector<ice::Pin> templatePins;
            if (source != 0) {
                const auto &sourceCell = project.schematicCells.at(source);
                if (sourceCell.systemVerilogSchematic) {
                    const auto parsed = ice::parseSystemVerilogModule(sourceCell);
                    if (!parsed.ok)
                        throw ice::ProjectError("schematic.hdl.parse", parsed.error);
                    const auto templ = ice::makeHdlSymbolTemplate(parsed.module);
                    artwork = templ.artwork;
                    templatePins = templ.pins;
                }
            } else if (!blank) {
                try {
                    const auto definition = ice::resolveSchematicSymbol(project.schematicCells.at(cell).pdk, args[3]);
                    artwork = definition.svg;
                    templatePins = definition.pins;
                } catch (const ice::ProjectError &error) {
                    throw ice::ProjectError("schematic.symbol",
                                            std::string(error.what()) +
                                                ". Use schematic-add-component, or pass --blank and add pins "
                                                "explicitly with schematic-add-pin.");
                }
            }
            ice::ProjectSession session(project);
            ice::ObjectId id = 0;
            session.execute("schematic-add-symbol", [&](ice::Project &p) {
                id = ice::addSymbol(p, cell, args[3], artwork, source);
                auto &symbol = p.schematicCells.at(cell).symbols.at(id);
                for (auto pin : templatePins) {
                    pin.id = p.nextId();
                    symbol.pins.push_back(std::move(pin));
                }
            });
            session.save(args[1]);
            const auto &symbol = session.project().schematicCells.at(cell).symbols.at(id);
            json pins = json::array();
            for (const auto &pin : symbol.pins)
                pins.push_back({{"id", pin.id},
                                {"name", pin.name},
                                {"direction", pinDirectionName(pin.direction)},
                                {"width", pin.width},
                                {"width_known", pin.widthKnown}});
            printJson({{"id", id}, {"cell", cell}, {"name", args[3]}, {"pins", std::move(pins)}});
            return 0;
        }

        if (command == "schematic-add-pin") {
            expectArguments(args, 5, 8, "schematic-add-pin <project> <cell> <symbol> <name> [x] [y] [direction]");
            auto project = ice::Project::load(args[1]);
            const auto cellId = resolveSchematic(project, args[2]);
            auto &cell = project.schematicCells.at(cellId);
            const auto symbolId = resolveSymbol(cell, args[3]);
            auto &symbol = cell.symbols.at(symbolId);
            for (const auto &pin : symbol.pins) {
                if (pin.name == args[4])
                    throw ice::ProjectError("schematic.pin", "Pin already exists: " + args[4]);
            }
            const auto x = args.size() > 5 ? parseInteger(args[5], "x") : ice::Coordinate{0};
            const auto y = args.size() > 6 ? parseInteger(args[6], "y") : ice::Coordinate{0};
            const auto direction = args.size() > 7 ? parsePinDirection(args[7]) : ice::PinDirection::right;
            ice::ProjectSession session(project);
            ice::ObjectId pinId = 0;
            session.execute("schematic-add-pin", [&](ice::Project &p) {
                pinId = p.nextId();
                p.schematicCells.at(cellId).symbols.at(symbolId).pins.push_back({pinId, args[4], x, y, direction});
            });
            session.save(args[1]);
            printJson({{"id", pinId}, {"cell", cellId}, {"symbol", symbolId}, {"name", args[4]}, {"x", x}, {"y", y}});
            return 0;
        }

        if (command == "schematic-add-instance") {
            expectArguments(args, 7, 7, "schematic-add-instance <project> <cell> <symbol> <name> <x> <y>");
            const auto project = ice::Project::load(args[1]);
            const auto cell = resolveSchematic(project, args[2]);
            const auto symbol = resolveSymbol(project.schematicCells.at(cell), args[3]);
            const auto &symbolRef = project.schematicCells.at(cell).symbols.at(symbol);
            if (symbolRef.pins.empty())
                throw ice::ProjectError("schematic.symbol.pins",
                                        "Symbol '" + symbolRef.name +
                                            "' has no pins; use schematic-add-component or add pins explicitly");
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

        if (command == "schematic-set-parameter") {
            expectArguments(args, 6, 6, "schematic-set-parameter <project> <cell> <instance> <name> <value>");
            auto project = ice::Project::load(args[1]);
            const auto cell = resolveSchematic(project, args[2]);
            const auto &cellRef = project.schematicCells.at(cell);
            ice::ObjectId instanceId = 0;
            if (const auto id = parseUnsigned(args[3]); id && cellRef.instances.contains(*id)) {
                instanceId = *id;
            } else {
                for (const auto &[id, instance] : cellRef.instances) {
                    if (instance.name == args[3]) {
                        instanceId = id;
                        break;
                    }
                }
            }
            if (instanceId == 0)
                throw ice::ProjectError("schematic.instance", "Unknown instance: " + args[3]);
            ice::ProjectSession session(project);
            session.execute("schematic-set-parameter", [&](ice::Project &p) {
                ice::setSchematicParameter(p, cell, instanceId, args[4], args[5]);
            });
            session.save(args[1]);
            printJson({{"cell", cell}, {"instance", instanceId}, {"name", args[4]}, {"value", args[5]}});
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
            expectArguments(args, 3, 5, "layout-add-cell <project> <name> [pdk] [stack]");
            ice::ProjectSession session(ice::Project::load(args[1]));
            ice::ObjectId id = 0;
            const auto pdk = args.size() >= 4 ? args[3] : std::string(ice::genericPdkId);
            const auto stack = resolveLayoutStack(pdk, args.size() == 5 ? args[4] : std::string{});
            session.execute("layout-add-cell",
                            [&](ice::Project &project) { id = ice::createLayoutCell(project, args[2], pdk, stack); });
            session.save(args[1]);
            printJson({{"id", id}, {"name", args[2]}, {"pdk", pdk}, {"stack", stack}});
            return 0;
        }

        if (command == "layout-add-procedural") {
            expectArguments(args, 3, 6, "layout-add-procedural <project> <name> [pdk] [script-file] [stack]");
            ice::ProjectSession session(ice::Project::load(args[1]));
            const auto pdk = args.size() >= 4 ? args[3] : std::string(ice::genericPdkId);
            const auto script = args.size() >= 5 ? readTextFile(args[4]) : std::string{};
            const auto stack = resolveLayoutStack(pdk, args.size() == 6 ? args[5] : std::string{});
            ice::ObjectId id = 0;
            ice::ObjectId generated = 0;
            session.execute("layout-add-procedural", [&](ice::Project &project) {
                id = ice::createProceduralLayout(project, args[2], pdk, script, stack);
                generated = project.proceduralLayouts.at(id).generatedCell;
            });
            session.save(args[1]);
            printJson({{"id", id}, {"name", args[2]}, {"pdk", pdk}, {"stack", stack}, {"generated_cell", generated}});
            return 0;
        }

        if (command == "layout-create-from-schematic") {
            expectArguments(args, 3, 4, "layout-create-from-schematic <project> <schematic> [layout-name]");
            const auto project = ice::Project::load(args[1]);
            const auto schematic = resolveSchematic(project, args[2]);
            const auto name = args.size() == 4 ? args[3] : std::string{};
            const auto stack = resolveLayoutStack(project.schematicCells.at(schematic).pdk, {});
            ice::ProjectSession session(project);
            ice::ObjectId id = 0;
            session.execute("layout-create-from-schematic",
                            [&](ice::Project &p) { id = ice::createLayoutFromSchematic(p, schematic, name, stack); });
            session.save(args[1]);
            printJson({{"id", id}, {"schematic", schematic}, {"stack", stack}});
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
                                  {"stack", cell.stack},
                                  {"procedural", false},
                                  {"library", cell.library},
                                  {"device", cell.device},
                                  {"generated_by", cell.generatedBy},
                                  {"shapes", cell.shapes.size()},
                                  {"ports", cell.ports.size()},
                                  {"instances", cell.instances.size()}});
            }
            for (const auto &[id, procedural] : project.proceduralLayouts) {
                result.push_back({{"id", id},
                                  {"name", procedural.name},
                                  {"pdk", procedural.pdk},
                                  {"stack", procedural.stack},
                                  {"procedural", true},
                                  {"generated_cell", procedural.generatedCell},
                                  {"script_length", procedural.script.size()}});
            }
            printJson(result);
            return 0;
        }

        if (command == "layout-info") {
            expectArguments(args, 3, 3, "layout-info <project> <cell|procedural>");
            const auto project = ice::Project::load(args[1]);
            const auto id = resolveLayoutItem(project, args[2]);
            if (const auto procedural = project.proceduralLayouts.find(id);
                procedural != project.proceduralLayouts.end()) {
                printJson({{"id", id},
                           {"name", procedural->second.name},
                           {"pdk", procedural->second.pdk},
                           {"stack", procedural->second.stack},
                           {"procedural", true},
                           {"generated_cell", procedural->second.generatedCell},
                           {"script", procedural->second.script}});
            } else {
                printJson(layoutInfo(project, project.layoutCells.at(id)));
            }
            return 0;
        }

        if (command == "layout-remove") {
            expectArguments(args, 3, 3, "layout-remove <project> <cell|procedural>");
            auto project = ice::Project::load(args[1]);
            const auto id = resolveLayoutItem(project, args[2]);
            std::string name;
            if (const auto procedural = project.proceduralLayouts.find(id);
                procedural != project.proceduralLayouts.end())
                name = procedural->second.name;
            else
                name = project.layoutCells.at(id).name;
            ice::ProjectSession session(project);
            session.execute("remove layout", [&](ice::Project &p) { ice::removeLayoutItem(p, id); });
            session.save(args[1]);
            printJson({{"id", id}, {"name", name}, {"deleted", true}});
            return 0;
        }

        if (command == "layout-add-layer") {
            expectArguments(args, 4, 5, "layout-add-layer <project> <name> <number> [datatype]");
            ice::ProjectSession session(ice::Project::load(args[1]));
            const auto number = static_cast<int>(parseInteger(args[3], "layer number"));
            const auto datatype = args.size() >= 5 ? static_cast<int>(parseInteger(args[4], "layer datatype")) : 0;
            ice::ObjectId id = 0;
            session.execute("layout-add-layer", [&](ice::Project &project) {
                id = ice::addLayer(project, args[2], number, 0, {}, datatype);
            });
            session.save(args[1]);
            printJson({{"id", id}, {"name", args[2]}, {"number", number}, {"datatype", datatype}});
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

        if (command == "layout-add-text") {
            expectArguments(args, 7, 8, "layout-add-text <project> <cell> <layer> <x> <y> <text> [height]");
            const auto project = ice::Project::load(args[1]);
            const auto cell = resolveLayout(project, args[2]);
            const auto layer = resolveLayer(project, args[3]);
            const auto x = parseInteger(args[4], "x");
            const auto y = parseInteger(args[5], "y");
            const auto height = args.size() > 7 ? parseInteger(args[7], "height") : ice::Coordinate{1000};
            ice::ProjectSession session(project);
            ice::ObjectId id = 0;
            session.execute("layout-add-text",
                            [&](ice::Project &p) { id = ice::addText(p, cell, layer, x, y, args[6], height); });
            session.save(args[1]);
            printJson({{"id", id}, {"cell", cell}, {"layer", layer}, {"text", args[6]}});
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
            expectArguments(args, 9, 1000,
                            "layout-add-port <project> <cell> <name> <layer> <x> <y> <width> <height> [options]");
            const auto project = ice::Project::load(args[1]);
            const auto cell = resolveLayout(project, args[2]);
            const auto layer = resolveLayer(project, args[4]);
            const ice::Rect rect{parseInteger(args[5], "x"), parseInteger(args[6], "y"), parseInteger(args[7], "width"),
                                 parseInteger(args[8], "height")};
            auto direction = ice::PortDirection::inout;
            auto side = ice::PortSide::left;
            ice::ObjectId net = 0;
            std::string label = args[3];
            bool visible = true;
            bool locked = false;
            for (std::size_t index = 9; index < args.size(); ++index) {
                const auto &option = args[index];
                if (option == "--direction" && index + 1 < args.size())
                    direction = parsePortDirectionArgument(args[++index]);
                else if (option == "--side" && index + 1 < args.size())
                    side = parsePortSideArgument(args[++index]);
                else if (option == "--net" && index + 1 < args.size())
                    net = resolveLayoutNet(project.layoutCells.at(cell), args[++index]);
                else if (option == "--label" && index + 1 < args.size())
                    label = args[++index];
                else if (option == "--hidden")
                    visible = false;
                else if (option == "--locked")
                    locked = true;
                else
                    argumentError("Unknown layout-add-port option: " + option);
            }
            ice::ProjectSession session(project);
            ice::ObjectId id = 0;
            session.execute("layout-add-port", [&](ice::Project &p) {
                id = ice::addPort(p, cell, args[3], layer, rect, direction, side, net, label);
                auto &port = p.layoutCells.at(cell).ports.at(id);
                port.visible = visible;
                port.locked = locked;
            });
            session.save(args[1]);
            printJson({{"id", id},
                       {"cell", cell},
                       {"name", args[3]},
                       {"layer", layer},
                       {"direction", ice::portDirectionName(direction)},
                       {"side", ice::portSideName(side)},
                       {"net", net == 0 ? json(nullptr) : json(net)}});
            return 0;
        }

        if (command == "layout-list-ports") {
            expectArguments(args, 3, 3, "layout-list-ports <project> <cell>");
            const auto project = ice::Project::load(args[1]);
            const auto cell = resolveLayout(project, args[2]);
            json ports = json::array();
            for (const auto &[id, port] : project.layoutCells.at(cell).ports) {
                ports.push_back({{"id", id},
                                 {"name", port.name},
                                 {"layer", port.layer},
                                 {"x", port.rectangle.x},
                                 {"y", port.rectangle.y},
                                 {"width", port.rectangle.width},
                                 {"height", port.rectangle.height},
                                 {"direction", ice::portDirectionName(port.direction)},
                                 {"side", ice::portSideName(port.side)},
                                 {"net", port.net == 0 ? json(nullptr) : json(port.net)},
                                 {"label", port.label},
                                 {"visible", port.visible},
                                 {"locked", port.locked},
                                 {"source_instance", port.sourceInstance},
                                 {"source_port", port.sourcePort}});
            }
            printJson({{"cell", cell}, {"ports", std::move(ports)}});
            return 0;
        }

        if (command == "layout-update-port") {
            expectArguments(args, 4, 1000, "layout-update-port <project> <cell> <port> [options]");
            const auto project = ice::Project::load(args[1]);
            const auto cell = resolveLayout(project, args[2]);
            auto &cellRef = project.layoutCells.at(cell);
            const auto portId = resolveLayoutPort(cellRef, args[3]);
            auto port = cellRef.ports.at(portId);
            for (std::size_t index = 4; index < args.size(); ++index) {
                const auto &option = args[index];
                if (option == "--name" && index + 1 < args.size())
                    port.name = args[++index];
                else if (option == "--label" && index + 1 < args.size())
                    port.label = args[++index];
                else if (option == "--direction" && index + 1 < args.size())
                    port.direction = parsePortDirectionArgument(args[++index]);
                else if (option == "--side" && index + 1 < args.size())
                    port.side = parsePortSideArgument(args[++index]);
                else if (option == "--net" && index + 1 < args.size()) {
                    const auto &value = args[++index];
                    port.net = value == "none" ? ice::ObjectId{0} : resolveLayoutNet(cellRef, value);
                } else if (option == "--x" && index + 1 < args.size())
                    port.rectangle.x = parseInteger(args[++index], "x");
                else if (option == "--y" && index + 1 < args.size())
                    port.rectangle.y = parseInteger(args[++index], "y");
                else if (option == "--width" && index + 1 < args.size())
                    port.rectangle.width = parseInteger(args[++index], "width");
                else if (option == "--height" && index + 1 < args.size())
                    port.rectangle.height = parseInteger(args[++index], "height");
                else if (option == "--visible")
                    port.visible = true;
                else if (option == "--hidden")
                    port.visible = false;
                else if (option == "--locked")
                    port.locked = true;
                else if (option == "--unlocked")
                    port.locked = false;
                else
                    argumentError("Unknown layout-update-port option: " + option);
            }
            ice::ProjectSession session(project);
            session.execute("layout-update-port", [&](ice::Project &p) { ice::updatePort(p, cell, port); });
            session.save(args[1]);
            printJson({{"id", portId}, {"cell", cell}});
            return 0;
        }

        if (command == "layout-remove-port") {
            expectArguments(args, 4, 4, "layout-remove-port <project> <cell> <port>");
            const auto project = ice::Project::load(args[1]);
            const auto cell = resolveLayout(project, args[2]);
            const auto portId = resolveLayoutPort(project.layoutCells.at(cell), args[3]);
            ice::ProjectSession session(project);
            session.execute("layout-remove-port", [&](ice::Project &p) { ice::removePort(p, cell, portId); });
            session.save(args[1]);
            printJson({{"id", portId}, {"cell", cell}});
            return 0;
        }

        if (command == "layout-add-net") {
            expectArguments(args, 4, 4, "layout-add-net <project> <cell> <name>");
            const auto project = ice::Project::load(args[1]);
            const auto cell = resolveLayout(project, args[2]);
            ice::ProjectSession session(project);
            ice::ObjectId id = 0;
            session.execute("layout-add-net", [&](ice::Project &p) { id = ice::createLayoutNet(p, cell, args[3]); });
            session.save(args[1]);
            printJson({{"id", id}, {"cell", cell}, {"name", args[3]}});
            return 0;
        }

        if (command == "layout-assign-port-net") {
            expectArguments(args, 5, 5, "layout-assign-port-net <project> <cell> <port> <net|none>");
            const auto project = ice::Project::load(args[1]);
            const auto cell = resolveLayout(project, args[2]);
            const auto portId = resolveLayoutPort(project.layoutCells.at(cell), args[3]);
            const auto netId =
                args[4] == "none" ? ice::ObjectId{0} : resolveLayoutNet(project.layoutCells.at(cell), args[4]);
            ice::ProjectSession session(project);
            session.execute("layout-assign-port-net",
                            [&](ice::Project &p) { ice::assignPortNet(p, cell, portId, netId); });
            session.save(args[1]);
            printJson({{"port", portId}, {"net", netId == 0 ? json(nullptr) : json(netId)}});
            return 0;
        }

        if (command == "layout-add-instance") {
            expectArguments(args, 7, 8, "layout-add-instance <project> <parent> <child> <name> <x> <y> [orientation]");
            const auto project = ice::Project::load(args[1]);
            const auto parent = resolveLayout(project, args[2]);
            const auto child = resolveLayout(project, args[3]);
            const auto orientation = args.size() == 8 ? parseOrientation(args[7]) : ice::Orientation::r0;
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

        if (command == "layout-export-lef") {
            expectArguments(args, 4, 4, "layout-export-lef <project> <cell> <output.lef>");
            const auto project = ice::Project::load(args[1]);
            const auto cell = resolveLayout(project, args[2]);
            ice::LayoutInterchange::exportLef(project, cell, args[3]);
            printJson({{"cell", cell}, {"output", args[3]}});
            return 0;
        }

        if (command == "layout-export-def") {
            expectArguments(args, 4, 4, "layout-export-def <project> <cell> <output.def>");
            const auto project = ice::Project::load(args[1]);
            const auto cell = resolveLayout(project, args[2]);
            ice::LayoutInterchange::exportDef(project, cell, args[3]);
            printJson({{"cell", cell}, {"output", args[3]}});
            return 0;
        }

        if (command == "layout-import-gds") {
            expectArguments(args, 3, 4, "layout-import-gds <input.gds> <project> [pin-label-layer]");
            ice::LayoutImportOptions options;
            if (args.size() == 4)
                options.pinLabelLayer = std::stoi(args[3]);
            auto project = ice::LayoutInterchange::importGds(args[1], options);
            auto name = projectNameForPath(args[2]);
            if (project.name.empty())
                project.name = name;
            project.identity = "project-" + project.name;
            project.save(args[2]);
            printJson({{"path", (projectRootForCli(args[2]) / "project.icepro").string()},
                       {"name", project.name},
                       {"cells", project.layoutCells.size()},
                       {"pins", project.layoutCells.empty() ? 0 : project.layoutCells.begin()->second.ports.size()}});
            return 0;
        }

        if (command == "layout-import-lef") {
            expectArguments(args, 3, 3, "layout-import-lef <input.lef> <project>");
            auto project = ice::LayoutInterchange::importLef(args[1]);
            project.identity = "project-" + project.name;
            project.save(args[2]);
            printJson({{"path", (projectRootForCli(args[2]) / "project.icepro").string()},
                       {"name", project.name},
                       {"cells", project.layoutCells.size()},
                       {"pins", project.layoutCells.empty() ? 0 : project.layoutCells.begin()->second.ports.size()}});
            return 0;
        }

        if (command == "layout-import-def") {
            expectArguments(args, 3, 3, "layout-import-def <input.def> <project>");
            auto project = ice::LayoutInterchange::importDef(args[1]);
            project.identity = "project-" + project.name;
            project.save(args[2]);
            printJson({{"path", (projectRootForCli(args[2]) / "project.icepro").string()},
                       {"name", project.name},
                       {"cells", project.layoutCells.size()},
                       {"pins", project.layoutCells.empty() ? 0 : project.layoutCells.begin()->second.ports.size()}});
            return 0;
        }

        if (command == "layout-connectivity") {
            expectArguments(args, 3, 3, "layout-connectivity <project> <cell>");
            const auto project = ice::Project::load(args[1]);
            const auto cell = resolveLayout(project, args[2]);
            json groups = json::array();
            for (const auto &group : ice::extractLayoutNets(project, cell)) {
                groups.push_back({{"net", group.net == 0 ? json(nullptr) : json(group.net)},
                                  {"name", group.name},
                                  {"shapes", group.shapes},
                                  {"ports", group.ports},
                                  {"connections", group.connections}});
            }
            printJson({{"cell", cell}, {"nets", std::move(groups)}});
            return 0;
        }

        if (command == "simulation-add") {
            expectArguments(args, 4, static_cast<std::size_t>(-1),
                            "simulation-add <project> <name> <schematic> [analysis] [stop] [step] "
                            "[signals (comma-separated or multiple arguments)]");
            const auto project = ice::Project::load(args[1]);
            const auto schematic = resolveSchematic(project, args[3]);
            const auto &schematicCell = project.schematicCells.at(schematic);
            const bool mixedSignal =
                !schematicCell.systemVerilogSchematic && ice::schematicContainsHdl(project, schematic);
            ice::SimulationItem simulation;
            simulation.name = args[2];
            simulation.schematic = schematic;
            if (schematicCell.systemVerilogSchematic) {
                simulation.simulator = "iverilog";
                simulation.analysis = "digital";
            } else if (mixedSignal) {
                simulation.simulator = "mixed";
                simulation.analysis = "transient";
                simulation.stop = 1.0e-6;
                simulation.step = 1.0e-9;
            }
            if (args.size() >= 5 && !mixedSignal)
                simulation.analysis = normalizeAnalysisName(args[4]);
            if (args.size() >= 6)
                simulation.stop = parseDouble(args[5], "stop time");
            if (args.size() >= 7)
                simulation.step = parseDouble(args[6], "step time");
            if (args.size() >= 8) {
                std::string signals;
                for (std::size_t index = 7; index < args.size(); ++index) {
                    if (!signals.empty())
                        signals += ",";
                    signals += args[index];
                }
                simulation.captureSignals = std::move(signals);
            }
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
                       {"simulator", simulation.simulator},
                       {"analysis", simulation.analysis},
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

        if (command == "simulation-remove") {
            expectArguments(args, 3, 3, "simulation-remove <project> <simulation>");
            auto project = ice::Project::load(args[1]);
            const auto id = resolveSimulation(project, args[2]);
            const auto name = project.simulations.at(id).name;
            ice::ProjectSession session(project);
            session.execute("remove simulation", [&](ice::Project &p) { ice::removeSimulationItem(p, id); });
            session.save(args[1]);
            printJson({{"id", id}, {"name", name}, {"deleted", true}});
            return 0;
        }

        if (command == "simulation-run") {
            expectArguments(args, 3, 5, "simulation-run <project> <simulation> [timeout-ms] [output.json]");
            std::size_t end = args.size();
            std::string outputPath;
            if (end > 3 && std::filesystem::path(args.back()).extension() == ".json") {
                outputPath = args.back();
                --end;
            }
            if (end > 4)
                argumentError("Usage: ice-cli simulation-run <project> <simulation> [timeout-ms] [output.json]");
            const auto timeout = std::chrono::milliseconds(
                end > 3 ? std::max<ice::Coordinate>(1, parseInteger(args[3], "timeout")) : 60000);

            auto project = ice::Project::load(args[1]);
            const auto simulationId = resolveSimulation(project, args[2]);
            const auto &simulation = project.simulations.at(simulationId);
            const auto schematic = project.schematicCells.find(simulation.schematic);
            if (schematic == project.schematicCells.end())
                throw ice::ProjectError("simulation.schematic", "Simulation references a missing schematic");

            ice::SimulationRequest request;
            request.simulator = simulation.simulator;
            request.stop = simulation.stop;
            request.step = simulation.step;
            request.use_initial_conditions = simulation.use_initial_conditions;
            request.workingDirectory = projectRootForCli(args[1]);
            std::vector<std::string> notes;
            const bool mixedSignal =
                !schematic->second.systemVerilogSchematic && ice::schematicContainsHdl(project, simulation.schematic);
            const auto appendDirectives = [&] {
                std::istringstream directives(simulation.directives);
                std::string directive;
                while (std::getline(directives, directive)) {
                    const auto trimmed = trimText(directive);
                    if (!trimmed.empty() && !isAnalysisDirective(trimmed))
                        request.netlist.push_back(trimmed);
                }
            };
            if (mixedSignal) {
                std::string error;
                if (!ice::prepareMixedSignalRequest(request, project, simulation.schematic, &error))
                    throw ice::ProjectError("simulation.mixed", error);
                ice::applyPdkSimulationSetup(schematic->second.pdk, request);
                appendDirectives();
            } else if (schematic->second.systemVerilogSchematic) {
                request.analysis = "digital";
                request.simulator = "iverilog";
                std::istringstream source(schematic->second.systemVerilogText);
                std::string line;
                while (std::getline(source, line))
                    request.netlist.push_back(line);
            } else {
                request.simulator = "ngspice";
                request.analysis = normalizeAnalysisName(simulation.analysis);
                request.netlist = ice::buildSimulationNetlist(project, simulation.schematic, &notes);
                ice::applyPdkSimulationSetup(schematic->second.pdk, request);
                appendDirectives();
            }
            request.probeSignals = splitSignals(simulation.captureSignals);

            ice::SimulationRunner runner;
            const auto result = runner.run(request, timeout, [](const ice::SimulationProgress &progress) {
                std::clog << "progress: " << progress.fraction << " " << progress.message << "\n";
            });
            if (result.state == ice::JobState::succeeded && !result.traces.empty()) {
                auto updated = project.simulations.at(simulationId);
                if (ice::populateSimulationSignalsFromTraces(updated, result.traces)) {
                    project.simulations[simulationId] = std::move(updated);
                    project.save(args[1]);
                }
            }

            ice::SimulationRunContext context;
            context.projectRoot = projectRootForCli(args[1]);
            context.projectIdentity = project.identity;
            context.projectName = project.name;
            context.simulationId = simulationId;
            context.simulationName = simulation.name;
            context.schematicId = simulation.schematic;
            context.schematicName = schematic->second.name;
            context.request = request;
            context.directives = simulation.directives;
            context.netlistNotes = std::move(notes);
            context.graphs = simulation.graphs;

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
            summary["warnings"] = result.warnings;
            summary["capture_signals"] = simulation.captureSignals;
            if (!runId.empty())
                summary["run_id"] = runId;
            printJson(summary);
            if (result.state != ice::JobState::succeeded)
                throw ice::ProjectError("simulation." + ice::jobStateName(result.state), result.error);
            return 0;
        }

        if (command == "simulation-trace") {
            expectArguments(args, 3, 5, "simulation-trace <project> <simulation> [signals] [max-points]");
            const auto project = ice::Project::load(args[1]);
            const auto simulationId = resolveSimulation(project, args[2]);
            const auto stored = ice::latestSimulationRun(projectRootForCli(args[1]), simulationId);
            if (!stored)
                throw ice::ProjectError("simulation.result", "No stored run was found for this simulation");
            if (stored->result.time.empty() || stored->result.traces.empty())
                throw ice::ProjectError("simulation.result", "The stored run has no time vector or traces");
            const auto wanted = args.size() >= 4 ? splitSignals(args[3]) : std::vector<std::string>{};
            const auto maxPoints = args.size() >= 5 ? parseInteger(args[4], "max-points") : 2000;
            std::vector<const ice::SimulationTrace *> selected;
            for (const auto &trace : stored->result.traces) {
                if (wanted.empty())
                    continue;
                for (const auto &name : wanted) {
                    if (trace.name == name || trace.name.ends_with("." + name)) {
                        selected.push_back(&trace);
                        break;
                    }
                }
            }
            if (selected.empty() && wanted.empty()) {
                for (const auto &trace : stored->result.traces) {
                    selected.push_back(&trace);
                    if (selected.size() >= 8)
                        break;
                }
            }
            if (selected.empty())
                throw ice::ProjectError("simulation.result", "No stored trace matched the requested signals");
            const auto totalPoints = std::min(stored->result.time.size(), selected.front()->values.size());
            const auto limit = static_cast<std::size_t>(std::max<std::int64_t>(2, maxPoints));
            std::vector<std::size_t> indices;
            if (totalPoints <= limit) {
                for (std::size_t index = 0; index < totalPoints; ++index)
                    indices.push_back(index);
            } else {
                for (std::size_t index = 0; index + 1 < limit; ++index)
                    indices.push_back(index * (totalPoints - 1) / (limit - 1));
                indices.push_back(totalPoints - 1);
            }
            const bool decimated = indices.size() < totalPoints;
            json time = json::array();
            for (const auto index : indices)
                time.push_back(stored->result.time[index]);
            json traces = json::array();
            for (const auto *trace : selected) {
                json values = json::array();
                for (const auto index : indices)
                    values.push_back(index < trace->values.size() ? json(trace->values[index]) : json(nullptr));
                traces.push_back({{"name", trace->name}, {"values", std::move(values)}});
            }
            printJson({{"simulation", stored->simulationName},
                       {"run_id", stored->runId},
                       {"time_points", totalPoints},
                       {"returned_points", indices.size()},
                       {"decimated", decimated},
                       {"time", std::move(time)},
                       {"signals", std::move(traces)}});
            return 0;
        }

        if (command == "simulation-measure") {
            expectArguments(args, 3, 5, "simulation-measure <project> <simulation> [signal] [threshold]");
            const auto project = ice::Project::load(args[1]);
            const auto simulationId = resolveSimulation(project, args[2]);
            const auto stored = ice::latestSimulationRun(projectRootForCli(args[1]), simulationId);
            if (!stored)
                throw ice::ProjectError("simulation.result", "No stored run was found for this simulation");
            if (stored->result.time.empty() || stored->result.traces.empty())
                throw ice::ProjectError("simulation.result", "The stored run has no time vector or traces");
            double threshold = std::numeric_limits<double>::quiet_NaN();
            if (args.size() >= 5)
                threshold = parseDouble(args[4], "threshold");
            const std::string selected = args.size() >= 4 ? args[3] : std::string{};
            json measurements = json::array();
            for (const auto &trace : stored->result.traces) {
                if (!selected.empty() && trace.name != selected)
                    continue;
                measurements.push_back(measurementJson(ice::measureSignal(trace, stored->result.time, threshold)));
            }
            if (measurements.empty())
                argumentError("No captured signal matched '" + selected + "'");
            printJson({{"simulation", stored->simulationName},
                       {"schematic", stored->schematicName},
                       {"run_id", stored->runId},
                       {"time_points", stored->result.time.size()},
                       {"measurements", measurements}});
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

        if (command == "script-add") {
            expectArguments(args, 3, 4, "script-add <project> <name> [source-file]");
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

        if (command == "script-list") {
            expectArguments(args, 2, 2, "script-list <project>");
            const auto project = ice::Project::load(args[1]);
            printJson(projectSummary(project, args[1])["simulation_scripts"]);
            return 0;
        }

        if (command == "script-info") {
            expectArguments(args, 3, 3, "script-info <project> <script>");
            const auto project = ice::Project::load(args[1]);
            const auto id = resolveReport(project, args[2]);
            const auto &report = project.reports.at(id);
            printJson({{"id", id}, {"name", report.name}, {"path", report.path}});
            return 0;
        }

        if (command == "script-read") {
            expectArguments(args, 3, 3, "script-read <project> <script>");
            const auto project = ice::Project::load(args[1]);
            const auto id = resolveReport(project, args[2]);
            const auto &report = project.reports.at(id);
            printJson({{"id", id},
                       {"name", report.name},
                       {"path", report.path},
                       {"source", ice::readProjectReportSource(projectRootForCli(args[1]), report)}});
            return 0;
        }

        if (command == "script-write") {
            expectArguments(args, 4, 4, "script-write <project> <script> <source-file|->");
            const auto project = ice::Project::load(args[1]);
            const auto id = resolveReport(project, args[2]);
            const auto &report = project.reports.at(id);
            const auto source = args[3] == "-" ? readStdin() : readTextFile(args[3]);
            ice::writeProjectReportSource(projectRootForCli(args[1]), report, source);
            printJson({{"id", id}, {"name", report.name}, {"path", report.path}, {"bytes", source.size()}});
            return 0;
        }

        if (command == "script-run") {
            expectArguments(args, 3, 5, "script-run <project> <script> [timeout-ms] [--json]");
            const auto project = ice::Project::load(args[1]);
            const auto id = resolveReport(project, args[2]);
            const auto &report = project.reports.at(id);
            bool fullJson = false;
            std::optional<ice::Coordinate> timeoutArgument;
            for (std::size_t index = 3; index < args.size(); ++index) {
                if (args[index] == "--json")
                    fullJson = true;
                else if (!timeoutArgument)
                    timeoutArgument = parseInteger(args[index], "timeout");
                else
                    argumentError("Usage: ice-cli script-run <project> <script> [timeout-ms] [--json]");
            }
            const auto timeoutMs = timeoutArgument.value_or(30000);
            const auto timeout = std::chrono::milliseconds(std::max<ice::Coordinate>(1, timeoutMs));
            const auto result = ice::runProjectReport(projectRootForCli(args[1]), report, executablePath, timeout);
            const bool ok = !result.timedOut && result.exitCode == 0;
            json graphs = json::array();
            if (!result.graphsText.empty()) {
                const auto document = json::parse(result.graphsText, nullptr, false);
                if (!document.is_discarded() && document.contains("graphs"))
                    graphs = document.at("graphs");
            }
            if (fullJson) {
                printJson({{"id", id},
                           {"name", report.name},
                           {"path", report.path},
                           {"ok", ok},
                           {"timed_out", result.timedOut},
                           {"exit_code", result.exitCode},
                           {"stdout", result.stdoutText},
                           {"stderr", result.stderrText},
                           {"graphs", std::move(graphs)}});
            } else {
                json graphSummary = json::array();
                for (const auto &graph : graphs)
                    graphSummary.push_back({{"name", graph.value("name", "")},
                                            {"points", graph.value("x", json::array()).size()},
                                            {"series", graph.value("series", json::array()).size()}});
                printJson({{"id", id},
                           {"name", report.name},
                           {"path", report.path},
                           {"ok", ok},
                           {"timed_out", result.timedOut},
                           {"exit_code", result.exitCode},
                           {"stdout", result.stdoutText},
                           {"stderr", result.stderrText},
                           {"graphs", graphSummary}});
            }
            if (result.timedOut)
                throw ice::ProjectError("script.timeout", "Simulation script timed out");
            if (result.exitCode != 0)
                throw ice::ProjectError("script.failed",
                                        "Simulation script exited with code " + std::to_string(result.exitCode));
            return 0;
        }

        if (command == "script-delete") {
            expectArguments(args, 3, 4, "script-delete <project> <script> [--purge]");
            auto project = ice::Project::load(args[1]);
            const auto id = resolveReport(project, args[2]);
            const auto report = project.reports.at(id);
            const bool purge = args.size() == 4 && args[3] == "--purge";
            if (args.size() == 4 && !purge)
                argumentError("Usage: ice-cli script-delete <project> <script> [--purge]");
            project.reports.erase(id);
            project.save(args[1]);
            if (purge) {
                std::error_code status;
                std::filesystem::remove(projectRootForCli(args[1]) / report.path, status);
                if (status)
                    throw ice::ProjectError("script.delete", "Unable to purge script file: " + status.message());
            }
            printJson({{"id", id}, {"name", report.name}, {"path", report.path}, {"deleted", true}, {"purged", purge}});
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
                request.workingDirectory = projectRootForCli(args[1]);
                if (schematic.systemVerilogSchematic) {
                    request.simulator = "iverilog";
                    request.analysis = "digital";
                    std::istringstream source(schematic.systemVerilogText);
                    std::string line;
                    while (std::getline(source, line))
                        request.netlist.push_back(line);
                } else if (ice::schematicContainsHdl(project, schematicId)) {
                    std::string error;
                    if (!ice::prepareMixedSignalRequest(request, project, schematicId, &error))
                        throw ice::ProjectError("simulation.mixed", error);
                    ice::applyPdkSimulationSetup(schematic.pdk, request);
                } else {
                    request.netlist = ice::buildSimulationNetlist(project, schematicId);
                    ice::applyPdkSimulationSetup(schematic.pdk, request);
                }
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
                if (std::filesystem::path(args[1]).extension() == ".sv") {
                    request.simulator = "iverilog";
                    request.analysis = "digital";
                    request.workingDirectory = std::filesystem::current_path();
                }
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
