#include "icelib/core.hpp"
#include "icelib/measure.hpp"
#include "icelib/operations.hpp"
#include "icelib/pdk.hpp"
#include "icelib/simulation.hpp"
#include "icelib/simulation_report.hpp"
#include "icelib/simulation_store.hpp"
#include "icelib/verification.hpp"

#include <nlohmann/json.hpp>

#ifdef ICE_HAVE_AGENTSDK
#include <agentsdk/mcp/mcp_server.hpp>
#include <agentsdk/mcp/mcp_tool_builder.hpp>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using json = nlohmann::json;

struct ToolError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

json parseArguments(const std::string &text) {
    if (text.empty())
        return json::object();
    auto value = json::parse(text, nullptr, false);
    if (value.is_discarded() || !value.is_object())
        throw ToolError("Tool arguments must be a JSON object.");
    return value;
}

const json &requiredValue(const json &arguments, const char *name) {
    const auto it = arguments.find(name);
    if (it == arguments.end())
        throw ToolError(std::string("Missing required argument: ") + name);
    return *it;
}

std::string requiredString(const json &arguments, const char *name) {
    const auto &value = requiredValue(arguments, name);
    if (!value.is_string() || value.get<std::string>().empty())
        throw ToolError(std::string("Argument must be a non-empty string: ") + name);
    return value.get<std::string>();
}

std::string optionalString(const json &arguments, const char *name, const std::string &fallback = {}) {
    const auto it = arguments.find(name);
    if (it == arguments.end() || it->is_null())
        return fallback;
    if (!it->is_string())
        throw ToolError(std::string("Argument must be a string: ") + name);
    return it->get<std::string>();
}

std::string requiredStringAllowEmpty(const json &arguments, const char *name) {
    const auto &value = requiredValue(arguments, name);
    if (!value.is_string())
        throw ToolError(std::string("Argument must be a string: ") + name);
    return value.get<std::string>();
}

ice::Coordinate coordinateValue(const json &value, const char *name) {
    if (value.is_number_integer())
        return value.get<ice::Coordinate>();
    if (value.is_number_unsigned())
        return static_cast<ice::Coordinate>(value.get<std::uint64_t>());
    if (value.is_number_float())
        return static_cast<ice::Coordinate>(std::llround(value.get<double>()));
    throw ToolError(std::string("Argument must be an integer: ") + name);
}

ice::Coordinate requiredInteger(const json &arguments, const char *name) {
    return coordinateValue(requiredValue(arguments, name), name);
}

std::optional<std::int64_t> optionalInteger(const json &arguments, const char *name) {
    const auto it = arguments.find(name);
    if (it == arguments.end() || it->is_null())
        return std::nullopt;
    return static_cast<std::int64_t>(coordinateValue(*it, name));
}

bool optionalBoolean(const json &arguments, const char *name, bool fallback = false) {
    const auto it = arguments.find(name);
    if (it == arguments.end() || it->is_null())
        return fallback;
    if (!it->is_boolean())
        throw ToolError(std::string("Argument must be a boolean: ") + name);
    return it->get<bool>();
}

double numberValue(const json &value, const char *name) {
    if (value.is_number_integer())
        return static_cast<double>(value.get<std::int64_t>());
    if (value.is_number_unsigned())
        return static_cast<double>(value.get<std::uint64_t>());
    if (value.is_number_float())
        return value.get<double>();
    throw ToolError(std::string("Argument must be a number: ") + name);
}

std::vector<ice::LayoutPoint> parsePoints(const json &arguments) {
    const auto &value = requiredValue(arguments, "points");
    if (!value.is_array() || value.size() < 3)
        throw ToolError("Argument 'points' must be an array with at least three points.");
    std::vector<ice::LayoutPoint> points;
    points.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        const auto &point = value[index];
        if (point.is_array() && point.size() == 2) {
            points.push_back({coordinateValue(point[0], "point x"), coordinateValue(point[1], "point y")});
            continue;
        }
        if (point.is_object()) {
            points.push_back({coordinateValue(requiredValue(point, "x"), "point x"),
                              coordinateValue(requiredValue(point, "y"), "point y")});
            continue;
        }
        throw ToolError("Each point must be [x, y] or {\"x\": ..., \"y\": ...}.");
    }
    return points;
}

ice::Orientation parseOrientation(const std::string &text) {
    static const std::map<std::string, ice::Orientation> values = {
        {"r0", ice::Orientation::r0},     {"r90", ice::Orientation::r90},  {"r180", ice::Orientation::r180},
        {"r270", ice::Orientation::r270}, {"mx", ice::Orientation::mx},    {"my", ice::Orientation::my},
        {"m45", ice::Orientation::m45},   {"m135", ice::Orientation::m135}};
    const auto it = values.find(text);
    if (it == values.end())
        throw ToolError("Unsupported orientation: " + text);
    return it->second;
}

template <typename Map> ice::ObjectId lookupByIdOrName(const Map &map, const json &value, const char *kind) {
    if (value.is_number_integer()) {
        const auto id = value.get<ice::ObjectId>();
        if (map.contains(id))
            return id;
        throw ToolError(std::string("Unknown ") + kind + " id: " + std::to_string(id));
    }
    if (value.is_number_unsigned()) {
        const auto id = static_cast<ice::ObjectId>(value.get<std::uint64_t>());
        if (map.contains(id))
            return id;
        throw ToolError(std::string("Unknown ") + kind + " id: " + std::to_string(id));
    }
    if (value.is_string()) {
        const auto name = value.get<std::string>();
        if (!name.empty() &&
            std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
            try {
                const auto id = static_cast<ice::ObjectId>(std::stoull(name));
                if (map.contains(id))
                    return id;
            } catch (const std::exception &) {
            }
        }
        for (const auto &[id, entry] : map) {
            if (entry.name == name)
                return id;
        }
        throw ToolError(std::string("Unknown ") + kind + ": " + name);
    }
    throw ToolError(std::string("Expected a ") + kind + " name or id.");
}

ice::ObjectId resolveSchematic(const ice::Project &project, const json &arguments, const char *key = "cell") {
    return lookupByIdOrName(project.schematicCells, requiredValue(arguments, key), "schematic");
}

ice::ObjectId resolveSchematicArgument(const ice::Project &project, const json &arguments,
                                       std::initializer_list<const char *> keys) {
    for (const auto *key : keys) {
        const auto value = arguments.find(key);
        if (value != arguments.end() && !value->is_null())
            return lookupByIdOrName(project.schematicCells, *value, "schematic");
    }
    throw ToolError("Missing required argument: schematic (or cell)");
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

ice::PinDirection parsePinDirection(const std::string &text) {
    static const std::map<std::string, ice::PinDirection> values = {
        {"left", ice::PinDirection::left},
        {"right", ice::PinDirection::right},
        {"up", ice::PinDirection::up},
        {"down", ice::PinDirection::down},
    };
    const auto it = values.find(text);
    return it == values.end() ? ice::PinDirection::right : it->second;
}

ice::ObjectId resolveSymbol(const ice::SchematicCell &cell, const json &value) {
    return lookupByIdOrName(cell.symbols, value, "symbol");
}

const ice::Pin *findPinInSymbol(const ice::Symbol &symbol, const std::string &name) {
    for (const auto &pin : symbol.pins) {
        if (pin.name == name)
            return &pin;
    }
    return nullptr;
}

ice::ObjectId resolvePin(const ice::SchematicCell &cell, const json &value) {
    if (value.is_number_integer()) {
        const auto id = static_cast<ice::ObjectId>(value.get<std::int64_t>());
        for (const auto &[_, symbol] : cell.symbols)
            for (const auto &pin : symbol.pins)
                if (pin.id == id)
                    return id;
        throw ToolError("Unknown pin id: " + std::to_string(id));
    }
    if (value.is_number_unsigned()) {
        const auto id = static_cast<ice::ObjectId>(value.get<std::uint64_t>());
        for (const auto &[_, symbol] : cell.symbols)
            for (const auto &pin : symbol.pins)
                if (pin.id == id)
                    return id;
        throw ToolError("Unknown pin id: " + std::to_string(id));
    }
    if (!value.is_string())
        throw ToolError("Each pin must be a pin id or a name.");

    const auto text = value.get<std::string>();
    if (!text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
        try {
            const auto id = static_cast<ice::ObjectId>(std::stoull(text));
            for (const auto &[_, symbol] : cell.symbols)
                for (const auto &pin : symbol.pins)
                    if (pin.id == id)
                        return id;
        } catch (const std::exception &) {
        }
    }

    const auto dot = text.find('.');
    if (dot != std::string::npos) {
        const auto left = text.substr(0, dot);
        const auto right = text.substr(dot + 1);
        // Prefer instance.pin, then symbol.pin.
        for (const auto &[_, instance] : cell.instances) {
            if (instance.name != left)
                continue;
            const auto symbol = cell.symbols.find(instance.symbol);
            if (symbol != cell.symbols.end()) {
                if (const auto *pin = findPinInSymbol(symbol->second, right); pin)
                    return pin->id;
            }
        }
        for (const auto &[_, symbol] : cell.symbols) {
            if (symbol.name != left)
                continue;
            if (const auto *pin = findPinInSymbol(symbol, right); pin)
                return pin->id;
        }
    }

    std::vector<ice::ObjectId> matches;
    for (const auto &[_, symbol] : cell.symbols) {
        if (const auto *pin = findPinInSymbol(symbol, text); pin)
            matches.push_back(pin->id);
    }
    if (matches.size() == 1)
        return matches.front();
    if (matches.size() > 1)
        throw ToolError("Pin name is ambiguous; use <instance>.<pin> or <symbol>.<pin>: " + text);
    throw ToolError("Unknown pin: " + text);
}

json symbolJson(const ice::Symbol &symbol) {
    json pins = json::array();
    for (const auto &pin : symbol.pins) {
        pins.push_back({{"id", pin.id},
                        {"name", pin.name},
                        {"x", pin.x},
                        {"y", pin.y},
                        {"direction", pinDirectionName(pin.direction)}});
    }
    return {{"id", symbol.id}, {"name", symbol.name}, {"source_cell", symbol.sourceCell}, {"pins", std::move(pins)}};
}

json schematicJson(const ice::SchematicCell &cell) {
    json result = {{"id", cell.id},
                   {"name", cell.name},
                   {"pdk", cell.pdk},
                   {"text", cell.textSchematic},
                   {"source", cell.spiceText},
                   {"symbols", json::array()},
                   {"instances", json::array()},
                   {"nets", json::array()}};
    for (const auto &[_, symbol] : cell.symbols)
        result["symbols"].push_back(symbolJson(symbol));
    for (const auto &[id, instance] : cell.instances) {
        json parameters = json::array();
        for (const auto &parameter : instance.parameters)
            parameters.push_back({{"id", parameter.id}, {"name", parameter.name}, {"value", parameter.value}});
        result["instances"].push_back({{"id", id},
                                       {"name", instance.name},
                                       {"symbol", instance.symbol},
                                       {"x", instance.x},
                                       {"y", instance.y},
                                       {"orientation", ice::orientationName(instance.orientation)},
                                       {"parameters", std::move(parameters)}});
    }
    for (const auto &[id, net] : cell.nets)
        result["nets"].push_back({{"id", id}, {"name", net.name}, {"pins", net.pins}, {"x", net.x}, {"y", net.y}});
    return result;
}

json resultJson(const ice::SimulationResult &result) {
    json traces = json::array();
    for (const auto &trace : result.traces) {
        const auto stats = ice::traceStatistics(trace);
        traces.push_back({{"name", trace.name},
                          {"points", stats.points},
                          {"min", stats.min},
                          {"max", stats.max},
                          {"amplitude", stats.amplitude()},
                          {"mean", stats.mean},
                          {"rms", stats.rms},
                          {"first", stats.first},
                          {"last", stats.last}});
    }
    json available = json::array();
    for (const auto &trace : result.traces)
        available.push_back(trace.name);
    return {{"state", ice::jobStateName(result.state)},
            {"analysis", result.analysis},
            {"simulator", result.simulator},
            {"simulator_version", result.simulator_version},
            {"error", result.error},
            {"warnings", result.warnings},
            {"time_points", result.time.size()},
            {"trace_count", result.traces.size()},
            {"trace_details", std::move(traces)},
            {"available_signals", std::move(available)}};
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

json doctorJson(const std::filesystem::path &executablePath) {
    const auto python = ice::reportPythonExecutable();
    const auto runner = ice::reportRunnerScriptPath(executablePath);
    const auto pythonAvailable = !python.empty() && std::system((python + " --version >/dev/null 2>&1").c_str()) == 0;
    const auto ngspiceAvailable = std::system("ngspice --version >/dev/null 2>&1") == 0;
    bool ngspiceLibrary = false;
    for (const auto *candidate :
         {"/usr/lib/libngspice.so.0", "/usr/lib64/libngspice.so.0", "/usr/lib/x86_64-linux-gnu/libngspice.so.0"}) {
        std::error_code status;
        if (std::filesystem::exists(candidate, status) && !status) {
            ngspiceLibrary = true;
            break;
        }
    }

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

    // Keep the old top-level list for compatibility, but make it a list of
    // simulation models. Liberty timing libraries live in their own field.
    std::vector<std::string> simulationPaths;
    for (const auto &entry : simulationLibraries)
        simulationPaths.push_back(entry.at("path").get<std::string>());

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

    std::vector<std::string> rootStrings;
    for (const auto &root : roots)
        rootStrings.push_back(root.string());

    return {{"build", buildJson(executablePath)},
            {"python", python},
            {"python_available", pythonAvailable},
            {"report_runner", runner.string()},
            {"report_runner_available", !runner.empty()},
            {"ngspice_executable_available", ngspiceAvailable},
            {"ngspice_library_available", ngspiceLibrary},
            {"pdk_roots", std::move(rootStrings)},
            {"pdks", std::move(pdkArray)},
            {"pdks_ready", pdksReady},
            {"all_pdks_ready", allPdksReady},
            {"ready_pdks", std::move(readyPdks)},
            {"simulation_model_libraries", std::move(simulationLibraries)},
            {"timing_libraries", std::move(timingLibraries)},
            {"model_libraries", std::move(simulationPaths)},
            {"ok", pythonAvailable && !runner.empty() && (ngspiceAvailable || ngspiceLibrary)}};
}

ice::ObjectId resolveLayoutCell(const ice::Project &project, const json &arguments, const char *key = "cell") {
    return lookupByIdOrName(project.layoutCells, requiredValue(arguments, key), "layout cell");
}

ice::ObjectId resolveLayoutItem(const ice::Project &project, const json &value) {
    if (value.is_number_integer() || value.is_number_unsigned()) {
        const auto id = static_cast<ice::ObjectId>(value.get<std::uint64_t>());
        if (project.layoutCells.contains(id) || project.proceduralLayouts.contains(id))
            return id;
        throw ToolError("Unknown layout or procedural layout id: " + std::to_string(id));
    }
    if (value.is_string()) {
        const auto name = value.get<std::string>();
        for (const auto &[id, procedural] : project.proceduralLayouts) {
            if (procedural.name == name)
                return id;
        }
        for (const auto &[id, cell] : project.layoutCells) {
            if (cell.name == name)
                return id;
        }
        throw ToolError("Unknown layout or procedural layout: " + name);
    }
    throw ToolError("Expected a layout or procedural layout name or id.");
}

ice::ObjectId resolveLayer(const ice::Project &project, const json &arguments, const char *key = "layer") {
    return lookupByIdOrName(project.layers, requiredValue(arguments, key), "layer");
}

ice::ObjectId resolveLayoutNet(const ice::LayoutCell &cell, const json &value) {
    return lookupByIdOrName(cell.nets, value, "layout net");
}

ice::ObjectId resolveLayoutPort(const ice::LayoutCell &cell, const json &value) {
    return lookupByIdOrName(cell.ports, value, "layout port");
}

ice::PortDirection parsePortDirectionArgument(const std::string &text) {
    if (const auto direction = ice::parsePortDirection(text))
        return *direction;
    throw ToolError("Unknown port direction: " + text);
}

ice::PortSide parsePortSideArgument(const std::string &text) {
    if (const auto side = ice::parsePortSide(text))
        return *side;
    throw ToolError("Unknown port side: " + text);
}

ice::ObjectId resolveSimulation(const ice::Project &project, const json &value) {
    return lookupByIdOrName(project.simulations, value, "simulation");
}

std::size_t resolveGraphIndex(const ice::SimulationItem &simulation, const json &value) {
    if (value.is_number_integer()) {
        const auto id = static_cast<ice::ObjectId>(value.get<std::int64_t>());
        for (std::size_t index = 0; index < simulation.graphs.size(); ++index)
            if (simulation.graphs[index].id == id)
                return index;
        throw ToolError("Unknown graph id: " + std::to_string(id));
    }
    if (value.is_number_unsigned()) {
        const auto id = static_cast<ice::ObjectId>(value.get<std::uint64_t>());
        for (std::size_t index = 0; index < simulation.graphs.size(); ++index)
            if (simulation.graphs[index].id == id)
                return index;
        throw ToolError("Unknown graph id: " + std::to_string(id));
    }
    if (value.is_string()) {
        const auto name = value.get<std::string>();
        for (std::size_t index = 0; index < simulation.graphs.size(); ++index)
            if (simulation.graphs[index].name == name)
                return index;
        throw ToolError("Unknown graph: " + name);
    }
    throw ToolError("Expected a graph name or id.");
}

std::vector<std::string> splitCommaList(const std::string &text) {
    std::vector<std::string> values;
    std::string current;
    const auto flush = [&] {
        const auto first = current.find_first_not_of(" \t\r\n");
        const auto last = current.find_last_not_of(" \t\r\n");
        if (first != std::string::npos)
            values.push_back(current.substr(first, last - first + 1));
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
    return values;
}

std::vector<std::string> parseSignalListValue(const json &value, const char *name) {
    if (value.is_string())
        return splitCommaList(value.get<std::string>());
    if (value.is_array()) {
        std::vector<std::string> signals;
        for (const auto &entry : value) {
            if (!entry.is_string())
                throw ToolError(std::string("Argument must contain only strings: ") + name);
            const auto signal = entry.get<std::string>();
            if (!signal.empty())
                signals.push_back(signal);
        }
        return signals;
    }
    throw ToolError(std::string("Argument must be a string or string array: ") + name);
}

json graphJson(const ice::SimulationGraph &graph) {
    return {{"id", graph.id},
            {"name", graph.name},
            {"x_axis", graph.xAxis},
            {"y_signals", graph.ySignals},
            {"x_min", graph.xMin ? json(*graph.xMin) : json(nullptr)},
            {"x_max", graph.xMax ? json(*graph.xMax) : json(nullptr)},
            {"y_min", graph.yMin ? json(*graph.yMin) : json(nullptr)},
            {"y_max", graph.yMax ? json(*graph.yMax) : json(nullptr)}};
}

json graphWithSimulation(const ice::SimulationItem &simulation, const ice::SimulationGraph &graph) {
    auto result = graphJson(graph);
    result["simulation"] = simulation.id;
    result["simulation_name"] = simulation.name;
    return result;
}

std::filesystem::file_time_type projectStateStamp(const std::filesystem::path &path) {
    auto root = path;
    std::error_code status;
    if (!std::filesystem::is_directory(root, status))
        root = root.parent_path();
    auto stamp = std::filesystem::file_time_type::min();
    const auto visit = [&](const std::filesystem::path &candidate) {
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error) && !error) {
            const auto value = std::filesystem::last_write_time(candidate, error);
            if (!error)
                stamp = std::max(stamp, value);
        }
    };
    visit(root / "project.icepro");
    for (const auto &directory : {"sch", "lay", "sim"}) {
        std::error_code error;
        if (!std::filesystem::is_directory(root / directory, error) || error)
            continue;
        for (std::filesystem::recursive_directory_iterator it(root / directory, error), end; !error && it != end;
             it.increment(error))
            visit(it->path());
    }
    return stamp;
}

#ifdef ICE_HAVE_AGENTSDK

agentsdk::mcp::tool_result textResult(const std::string &text, bool isError = false) {
    agentsdk::mcp::tool_result result;
    agentsdk::mcp::text_content content;
    content.text = text;
    result.content.push_back(std::move(content));
    result.is_error = isError;
    return result;
}

agentsdk::mcp::tool_result jsonResult(const json &value, bool isError = false) {
    return textResult(value.dump(2), isError);
}

#endif

class IceMcpServer {
  public:
    IceMcpServer(std::optional<std::filesystem::path> projectPath, std::filesystem::path executablePath,
                 bool recover = false)
        : executablePath_(std::move(executablePath)) {
        if (!projectPath)
            return;
        projectPath_ = std::move(*projectPath);
        ice::ProjectLoadOptions options;
        options.recover = recover;
        try {
            ice::ProjectLoadReport report;
            auto project = ice::Project::load(projectPath_, options, &report);
            session_ = std::make_unique<ice::ProjectSession>(std::move(project));
            projectStamp_ = projectStateStamp(projectPath_);
            if (!report.warnings.empty()) {
                for (const auto &warning : report.warnings) {
                    if (!startupWarning_.empty())
                        startupWarning_ += "\n";
                    startupWarning_ += warning;
                }
            }
        } catch (const ice::ProjectError &error) {
            // Keep the server alive in a degraded state so the client can call
            // project_open(recover=true) or project_repair. Exiting here would
            // make the recovery tools unreachable when a project is corrupt.
            startupWarning_ = std::string(error.code()) + ": " + error.what();
        }
    }

#ifdef ICE_HAVE_AGENTSDK
    int run() {
        auto logger = spdlog::stderr_color_mt("ice-mcp");
        spdlog::set_default_logger(std::move(logger));

        const auto build = ice::buildInfo();
        agentsdk::mcp::implementation_info info;
        info.name = "ice-mcp";
        info.title = "ICE MCP Server";
        info.version = build.version + "+" + build.commit;

        agentsdk::mcp::server_capabilities capabilities;
        capabilities.tools = agentsdk::mcp::tools_capability{false};
        capabilities.resources = agentsdk::mcp::resources_capability{false, false};
        capabilities.logging = true;

        agentsdk::mcp::mcp_server server(std::move(info), std::move(capabilities));
        std::string instructions =
            "ICE is an integrated-circuit design environment. Open or create a project first, then use the "
            "project, schematic, layout, simulation graph, and simulation script tools. Successful project "
            "mutations are saved to disk automatically so the GUI and ice-cli can see them immediately. "
            "Simulation script source files are written under scripts/. Build " +
            build.version + " (" + build.commit + ", " + build.buildTime +
            "). Call ice_version and confirm required features before relying on them; restart the MCP client "
            "after installing a new build.";
        if (!startupWarning_.empty())
            instructions += "\nStartup project warning: " + startupWarning_;
        server.set_instructions(std::move(instructions));
        server.set_list_tools_handler([this](const std::optional<std::string> &) {
            return agentsdk::mcp::tools_list_result{listTools(), std::nullopt};
        });
        server.set_call_tool_handler(
            [this](const std::string &name, const std::string &arguments) { return callTool(name, arguments); });
        server.set_list_resources_handler([this](const std::optional<std::string> &) {
            return agentsdk::mcp::resources_list_result{listResources(), std::nullopt};
        });
        server.set_list_templates_handler(
            [](const std::optional<std::string> &) { return agentsdk::mcp::resource_templates_result{}; });
        server.set_read_resource_handler([this](const std::string &uri) { return readResource(uri); });
        server.set_list_prompts_handler(
            [](const std::optional<std::string> &) { return agentsdk::mcp::prompts_list_result{}; });
        server.run();
        return 0;
    }
#endif

  private:
    ice::ProjectSession &requireSession() {
        if (!session_)
            throw ToolError("No project is open. Call project_open or project_create first.");
        return *session_;
    }

    std::filesystem::path projectRootPath() const {
        if (projectPath_.empty())
            throw ToolError("Project path is not available.");
        std::error_code status;
        if (std::filesystem::is_directory(projectPath_, status) && !status)
            return projectPath_;
        if (projectPath_.extension() == ".icepro" || projectPath_.filename() == "project.icepro")
            return projectPath_.parent_path();
        return projectPath_;
    }

    json summary() const {
        if (!session_)
            return json{{"open", false}};
        const auto &project = session_->project();
        json result = {
            {"open", true},
            {"name", project.name},
            {"identity", project.identity},
            {"path", projectPath_.string()},
            {"dirty", session_->dirty()},
            {"history_position", session_->historyPosition()},
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
                                            {"symbols", cell.symbols.size()},
                                            {"instances", cell.instances.size()},
                                            {"nets", cell.nets.size()}});
        }
        for (const auto &[id, layer] : project.layers)
            result["layers"].push_back(
                {{"id", id}, {"name", layer.name}, {"number", layer.number}, {"visible", layer.visible}});
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
                                             {"analysis", simulation.analysis},
                                             {"simulator", simulation.simulator},
                                             {"graphs", simulation.graphs.size()}});
        }
        for (const auto &[id, report] : project.reports)
            result["simulation_scripts"].push_back({{"id", id}, {"name", report.name}, {"path", report.path}});
        return result;
    }

    json diagnostics() const {
        if (!session_)
            throw ToolError("No project is open.");
        json result = json::array();
        for (const auto &diagnostic : session_->project().validate()) {
            json item = {{"severity", diagnostic.severity == ice::Diagnostic::Severity::error ? "error" : "warning"},
                         {"code", diagnostic.code},
                         {"message", diagnostic.message}};
            if (diagnostic.object)
                item["object"] = *diagnostic.object;
            result.push_back(std::move(item));
        }
        return result;
    }

    json doctorWithChecks(const json &arguments) const {
        auto result = doctorJson(executablePath_);
        if (!startupWarning_.empty())
            result["startup_warning"] = startupWarning_;
        if (!session_)
            return result;

        const auto &project = session_->project();
        ice::ObjectId schematicId = 0;
        if (const auto value = arguments.find("schematic"); value != arguments.end() && !value->is_null()) {
            schematicId = lookupByIdOrName(project.schematicCells, *value, "schematic");
        } else if (const auto value = arguments.find("simulation"); value != arguments.end() && !value->is_null()) {
            const auto simulation = resolveSimulation(project, *value);
            schematicId = project.simulations.at(simulation).schematic;
        }

        std::set<std::string> pdkIds;
        const auto collectPdk = [&](const std::string &pdk) {
            if (!pdk.empty() && pdk != ice::genericPdkId && pdk != "generic")
                pdkIds.insert(pdk);
        };
        for (const auto &[_, cell] : project.schematicCells)
            collectPdk(cell.pdk);
        for (const auto &[_, cell] : project.layoutCells)
            collectPdk(cell.pdk);
        for (const auto &[_, procedural] : project.proceduralLayouts)
            collectPdk(procedural.pdk);

        if (schematicId != 0) {
            const auto &cell = project.schematicCells.at(schematicId);
            collectPdk(cell.pdk);
            std::vector<std::string> notes;
            const auto netlist = ice::buildSimulationNetlist(project, schematicId, &notes);
            json checks = json::array();
            bool allPresent = true;
            for (const auto &line : netlist) {
                auto trimmed = line;
                const auto first = trimmed.find_first_not_of(" \t\r\n");
                if (first == std::string::npos)
                    continue;
                trimmed = trimmed.substr(first);
                const auto lower = [&] {
                    std::string value = trimmed;
                    for (auto &character : value)
                        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
                    return value;
                }();
                if (!(lower.starts_with(".include ") || lower.starts_with(".inc ") || lower.starts_with(".lib ")))
                    continue;
                const auto quote = trimmed.find_first_of("\"'");
                if (quote == std::string::npos)
                    continue;
                const auto endQuote = trimmed.find(trimmed[quote], quote + 1);
                if (endQuote == std::string::npos)
                    continue;
                const auto pathText = trimmed.substr(quote + 1, endQuote - quote - 1);
                std::filesystem::path path(pathText);
                std::error_code status;
                if (path.is_relative()) {
                    const auto candidate = projectRootPath() / path;
                    if (std::filesystem::exists(candidate, status) && !status)
                        path = candidate;
                }
                const bool exists = std::filesystem::exists(path, status) && !status;
                allPresent = allPresent && exists;
                checks.push_back({{"line", trimmed}, {"path", path.string()}, {"exists", exists}});
            }
            result["schematic"] = cell.name;
            result["model_checks"] = std::move(checks);
            result["model_paths_ok"] = allPresent;
            result["netlist_notes"] = notes;
            if (!allPresent)
                result["ok"] = false;
        }

        if (!pdkIds.empty()) {
            const bool probe = optionalBoolean(arguments, "probe", false);
            json checks = json::array();
            bool allReady = true;
            std::string missingSummary;
            for (const auto &id : pdkIds) {
                const auto matches = ice::findPdks(id);
                if (matches.empty()) {
                    allReady = false;
                    if (!missingSummary.empty())
                        missingSummary += "; ";
                    missingSummary += id + ": ICE PDK package is not installed";
                    checks.push_back(
                        {{"id", id}, {"simulation_ready", false}, {"missing", {"PDK package not installed"}}});
                    continue;
                }
                for (const auto &pdk : matches) {
                    auto item = ice::pdkInfoJson(pdk, false);
                    allReady = allReady && pdk.simulationReady;
                    if (probe) {
                        const auto probeResult = ice::probePdk(pdk);
                        item["probe"] = {{"attempted", probeResult.attempted},
                                         {"ok", probeResult.ok},
                                         {"summary", probeResult.summary},
                                         {"log", probeResult.log}};
                        allReady = allReady && probeResult.ok;
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
            result["pdk_checks"] = std::move(checks);
            result["pdk_ready"] = allReady;
            if (!allReady) {
                result["ok"] = false;
                result["pdk_hint"] = "The project PDK is not simulation-ready: " + missingSummary +
                                     ". Run pdk_info with probe=true after installing the missing package.";
            }
        }
        return result;

        return result;
    }

    json call(const std::string &name, const json &arguments) {
        if (session_ && !projectPath_.empty() && name != "project_open" && name != "project_create" &&
            name != "project_repair" && name != "project_reload" && name != "ice_doctor") {
            const auto currentStamp = projectStateStamp(projectPath_);
            if (currentStamp != projectStamp_) {
                if (session_->dirty())
                    throw ToolError("Project changed outside ice-mcp while local changes are pending; save or reopen "
                                    "the project before mutating it.");
                session_->replaceProject(ice::Project::load(projectPath_));
                projectStamp_ = projectStateStamp(projectPath_);
            }
        }
        auto result = callInternal(name, arguments);
        if (session_ && session_->dirty() && !projectPath_.empty()) {
            try {
                session_->save(projectPath_);
                projectStamp_ = projectStateStamp(projectPath_);
                result["saved"] = true;
            } catch (const ice::ProjectError &error) {
                throw ToolError("Mutation was applied in memory but could not be saved: " + std::string(error.what()));
            }
        }
        return result;
    }

    json callInternal(const std::string &name, const json &arguments) {
        std::string toolName = name;
        json toolArguments = arguments;
        if (toolName.starts_with("simulation_script_")) {
            toolName = "report_" + toolName.substr(std::string("simulation_script_").size());
            if (toolArguments.contains("script")) {
                toolArguments["report"] = toolArguments.at("script");
                toolArguments.erase("script");
            }
        }
        if (name == "ice_version") {
            auto result = buildJson(executablePath_);
            result["features"].push_back("mcp");
            result["features"].push_back("schematic_create_text");
            result["features"].push_back("schematic_add_text");
            result["features"].push_back("simulation_measure");
            result["features"].push_back("pdk_info");
            if (!startupWarning_.empty())
                result["startup_warning"] = startupWarning_;
            return result;
        }

        if (name == "pdk_info") {
            const auto requested = optionalString(arguments, "pdk");
            auto pdks = requested.empty() ? ice::discoverPdks() : ice::findPdks(requested);
            if (requested.empty() && pdks.empty())
                return {{"pdks", json::array()}};
            if (pdks.empty()) {
                std::string available;
                for (const auto &candidate : ice::discoverPdks()) {
                    if (!available.empty())
                        available += ", ";
                    available += candidate.id;
                }
                throw ToolError("Unknown PDK '" + requested +
                                "'. Available PDKs: " + (available.empty() ? "none" : available));
            }
            json result = {{"pdks", json::array()}};
            const bool probe = optionalBoolean(arguments, "probe", false);
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
            return result;
        }

        if (name == "pdk_build") {
            const auto requested = requiredString(arguments, "pdk");
            const auto pdks = ice::findPdks(requested);
            if (pdks.empty())
                throw ToolError("Unknown ICE PDK package: " + requested);
            const auto result = ice::buildPdkOsdi(pdks.front());
            return {{"ok", result.ok},
                    {"summary", result.summary},
                    {"compiler", result.compiler},
                    {"osdi_dir", result.osdiDir.string()},
                    {"log", result.log}};
        }

        if (name == "project_create") {
            const auto path = std::filesystem::path(requiredString(arguments, "path"));
            const auto projectName = requiredString(arguments, "name");
            if (path.has_parent_path())
                std::filesystem::create_directories(path.parent_path());
            ice::Project project;
            project.identity = "project-" + projectName;
            project.name = projectName;
            project.save(path);
            session_ = std::make_unique<ice::ProjectSession>(std::move(project));
            projectPath_ = path;
            projectStamp_ = projectStateStamp(projectPath_);
            startupWarning_.clear();
            return summary();
        }

        if (name == "project_open") {
            const auto path = std::filesystem::path(requiredString(arguments, "path"));
            ice::ProjectLoadOptions options;
            options.recover = optionalBoolean(arguments, "recover", false);
            ice::ProjectLoadReport report;
            auto project = ice::Project::load(path, options, &report);
            session_ = std::make_unique<ice::ProjectSession>(std::move(project));
            projectPath_ = path;
            projectStamp_ = projectStateStamp(projectPath_);
            startupWarning_.clear();
            auto result = summary();
            if (!report.warnings.empty())
                result["warnings"] = report.warnings;
            if (!report.quarantinedFiles.empty())
                result["quarantined_files"] = report.quarantinedFiles;
            return result;
        }
        if (name == "project_repair") {
            auto path = projectPath_;
            if (const auto value = arguments.find("path"); value != arguments.end() && !value->is_null())
                path = std::filesystem::path(requiredString(arguments, "path"));
            if (path.empty())
                throw ToolError("Missing required argument: path (project is not open).");
            ice::ProjectLoadOptions options;
            options.recover = true;
            ice::ProjectLoadReport report;
            auto project = ice::Project::load(path, options, &report);
            session_ = std::make_unique<ice::ProjectSession>(std::move(project));
            projectPath_ = path;
            projectStamp_ = projectStateStamp(projectPath_);
            session_->save(projectPath_);
            startupWarning_.clear();
            auto result = summary();
            result["repaired"] = true;
            result["warnings"] = report.warnings;
            result["quarantined_files"] = report.quarantinedFiles;
            return result;
        }
        if (name == "project_migrate") {
            auto path = projectPath_;
            if (const auto value = arguments.find("path"); value != arguments.end() && !value->is_null())
                path = std::filesystem::path(requiredString(arguments, "path"));
            if (path.empty())
                throw ToolError("Missing required argument: path (project is not open).");
            auto root = path;
            std::error_code status;
            if (!std::filesystem::is_directory(root, status) || status) {
                if (root.extension() == ".icepro" || root.filename() == "project.icepro")
                    root = root.parent_path();
            }
            const auto agentsPath = root / "AGENTS.md";
            const auto readAgents = [&] {
                std::ifstream input(agentsPath, std::ios::binary);
                return std::string((std::istreambuf_iterator<char>(input)), {});
            };
            const auto before = readAgents();
            auto project = ice::Project::load(path);
            const bool pdkIdsUpdated = ice::migrateProjectPdkIds(project);
            ice::ProjectSession migration(std::move(project));
            migration.save(path);
            const auto after = readAgents();
            session_ = std::make_unique<ice::ProjectSession>(ice::Project::load(path));
            projectPath_ = path;
            projectStamp_ = projectStateStamp(projectPath_);
            startupWarning_.clear();
            auto result = summary();
            result["migrated"] = true;
            result["agents_updated"] = before != after;
            result["pdk_ids_updated"] = pdkIdsUpdated;
            result["guidance_version"] = 2;
            return result;
        }
        if (name == "project_summary")
            return summary();
        if (name == "ice_doctor")
            return doctorWithChecks(arguments);

        if (name == "project_reload") {
            requireSession();
            if (session_->dirty())
                throw ToolError("Cannot reload a project with unsaved local changes.");
            const bool recover = optionalBoolean(arguments, "recover", false);
            ice::ProjectLoadOptions options;
            options.recover = recover;
            ice::ProjectLoadReport report;
            session_->replaceProject(ice::Project::load(projectPath_, options, &report));
            projectStamp_ = projectStateStamp(projectPath_);
            if (recover) {
                session_->save(projectPath_);
                startupWarning_.clear();
            }
            auto result = summary();
            if (!report.warnings.empty())
                result["warnings"] = report.warnings;
            if (!report.quarantinedFiles.empty())
                result["quarantined_files"] = report.quarantinedFiles;
            return result;
        }

        auto &session = requireSession();

        if (name == "project_validate")
            return {{"valid", session.project().validate().empty()}, {"diagnostics", diagnostics()}};
        if (name == "project_undo") {
            const bool changed = session.undo();
            return {{"changed", changed}};
        }
        if (name == "project_redo") {
            const bool changed = session.redo();
            return {{"changed", changed}};
        }
        if (name == "simulation_create") {
            const auto schematic = resolveSchematic(session.project(), arguments, "schematic");
            ice::SimulationItem simulation;
            simulation.name = requiredString(arguments, "name");
            simulation.schematic = schematic;
            simulation.analysis = optionalString(arguments, "analysis", "transient");
            if (const auto value = arguments.find("stop"); value != arguments.end())
                simulation.stop = numberValue(*value, "stop");
            if (const auto value = arguments.find("step"); value != arguments.end())
                simulation.step = numberValue(*value, "step");
            if (arguments.contains("signals")) {
                const auto signals = parseSignalListValue(arguments.at("signals"), "signals");
                for (std::size_t index = 0; index < signals.size(); ++index) {
                    if (index)
                        simulation.captureSignals += ",";
                    simulation.captureSignals += signals[index];
                }
            }
            const auto graphName = optionalString(arguments, "graph_name", "Graph 1");
            ice::ObjectId simulationId = 0;
            ice::ObjectId graphId = 0;
            session.execute("mcp simulation-create", [&](ice::Project &project) {
                simulation.id = project.nextId();
                simulationId = simulation.id;
                project.simulations.emplace(simulation.id, simulation);
                ice::SimulationGraph graph;
                graph.id = project.nextId();
                graphId = graph.id;
                graph.name = graphName;
                graph.xAxis = "time";
                graph.ySignals = splitCommaList(simulation.captureSignals);
                project.simulations.at(simulation.id).graphs.push_back(std::move(graph));
            });
            return {{"id", simulationId},
                    {"name", simulation.name},
                    {"schematic", schematic},
                    {"signals", simulation.captureSignals},
                    {"graph", graphId}};
        }
        if (name == "simulation_update") {
            const auto id = resolveSimulation(session.project(), requiredValue(arguments, "simulation"));
            auto updated = session.project().simulations.at(id);
            if (arguments.contains("analysis"))
                updated.analysis = requiredString(arguments, "analysis");
            if (arguments.contains("stop"))
                updated.stop = numberValue(arguments.at("stop"), "stop");
            if (arguments.contains("step"))
                updated.step = numberValue(arguments.at("step"), "step");
            if (arguments.contains("signals")) {
                const auto signals = parseSignalListValue(arguments.at("signals"), "signals");
                updated.captureSignals.clear();
                for (std::size_t index = 0; index < signals.size(); ++index) {
                    if (index)
                        updated.captureSignals += ",";
                    updated.captureSignals += signals[index];
                }
            }
            session.execute("mcp simulation-update",
                            [&](ice::Project &project) { project.simulations.at(id) = updated; });
            return {{"id", id},
                    {"name", updated.name},
                    {"analysis", updated.analysis},
                    {"stop", updated.stop},
                    {"step", updated.step},
                    {"signals", updated.captureSignals}};
        }
        if (name == "simulation_run") {
            const auto id = resolveSimulation(session.project(), requiredValue(arguments, "simulation"));
            const auto &simulation = session.project().simulations.at(id);
            const auto schematic = session.project().schematicCells.find(simulation.schematic);
            if (schematic == session.project().schematicCells.end())
                throw ToolError("Simulation references a missing schematic.");
            ice::SimulationRequest request;
            request.analysis = simulation.analysis;
            request.stop = simulation.stop;
            request.step = simulation.step;
            request.simulator = simulation.simulator;
            request.use_initial_conditions = simulation.use_initial_conditions;
            request.netlist = ice::buildSimulationNetlist(session.project(), simulation.schematic);
            ice::applyPdkSimulationSetup(schematic->second.pdk, request);
            request.probeSignals = splitCommaList(simulation.captureSignals);
            const auto timeout = std::chrono::milliseconds(
                std::max<std::int64_t>(1, optionalInteger(arguments, "timeout_ms").value_or(60000)));
            ice::SimulationRunner runner;
            const auto result = runner.run(request, timeout);
            ice::SimulationRunContext context;
            context.projectRoot = projectRootPath();
            context.projectIdentity = session.project().identity;
            context.projectName = session.project().name;
            context.simulationId = id;
            context.simulationName = simulation.name;
            context.schematicId = simulation.schematic;
            context.schematicName = schematic->second.name;
            context.request = request;
            context.graphs = simulation.graphs;
            const auto artifact = ice::writeSimulationRun(context, result);
            auto summary = resultJson(result);
            summary["run_id"] = artifact.runId;
            summary["stored"] = artifact.path.string();
            summary["simulation"] = simulation.name;
            summary["schematic"] = schematic->second.name;
            summary["traces"] = result.traces.size();
            summary["requested_signals"] = request.probeSignals;
            summary["netlist"] = request.netlist;
            summary["netlist_lines"] = request.netlist.size();
            if (!request.netlist.empty())
                summary["netlist_title"] = request.netlist.front();
            if (result.state != ice::JobState::succeeded)
                summary["hint"] =
                    "Inspect netlist, trace_details, and available_signals; run ice_doctor to check ngspice/PDK paths.";
            return summary;
        }
        if (name == "simulation_result" || name == "simulation_signals") {
            const auto id = resolveSimulation(session.project(), requiredValue(arguments, "simulation"));
            const auto stored = ice::latestSimulationRun(projectRootPath(), id);
            if (!stored)
                throw ToolError("No stored simulation run was found for this simulation.");
            auto summary = resultJson(stored->result);
            summary["simulation"] = stored->simulationName;
            summary["schematic"] = stored->schematicName;
            summary["run_id"] = stored->runId;
            summary["stored"] = stored->path.string();
            summary["created_at"] = stored->createdAt;
            if (name == "simulation_signals") {
                return {{"simulation", stored->simulationName},
                        {"schematic", stored->schematicName},
                        {"time_points", summary["time_points"]},
                        {"available_signals", summary["available_signals"]},
                        {"trace_details", summary["trace_details"]}};
            }
            return summary;
        }
        if (name == "simulation_measure") {
            const auto id = resolveSimulation(session.project(), requiredValue(arguments, "simulation"));
            const auto stored = ice::latestSimulationRun(projectRootPath(), id);
            if (!stored)
                throw ToolError("No stored simulation run was found for this simulation.");
            if (stored->result.time.empty() || stored->result.traces.empty())
                throw ToolError("The stored simulation has no time vector or traces to measure.");
            double threshold = std::numeric_limits<double>::quiet_NaN();
            if (const auto value = arguments.find("threshold"); value != arguments.end() && !value->is_null())
                threshold = numberValue(*value, "threshold");
            double start = 0.0;
            if (const auto value = arguments.find("start"); value != arguments.end() && !value->is_null())
                start = numberValue(*value, "start");
            double settleTolerance = 0.02;
            if (const auto value = arguments.find("settle_tolerance"); value != arguments.end() && !value->is_null())
                settleTolerance = numberValue(*value, "settle_tolerance");
            const auto selected = optionalString(arguments, "signal");
            json measurements = json::array();
            for (const auto &trace : stored->result.traces) {
                if (!selected.empty() && trace.name != selected)
                    continue;
                measurements.push_back(
                    measurementJson(ice::measureSignal(trace, stored->result.time, threshold, start, settleTolerance)));
            }
            if (measurements.empty())
                throw ToolError("No captured signal matched '" + selected + "'.");
            return {{"simulation", stored->simulationName},
                    {"schematic", stored->schematicName},
                    {"run_id", stored->runId},
                    {"time_points", stored->result.time.size()},
                    {"measurements", std::move(measurements)}};
        }
        if (name == "simulation_graph_create") {
            const auto simulationId = resolveSimulation(session.project(), requiredValue(arguments, "simulation"));
            const auto &simulation = session.project().simulations.at(simulationId);
            ice::SimulationGraph graph;
            graph.name = requiredString(arguments, "name");
            graph.xAxis = optionalString(arguments, "x_axis", "time");
            const auto ySignals = arguments.find("y_signals");
            if (ySignals != arguments.end() && !ySignals->is_null()) {
                graph.ySignals = parseSignalListValue(*ySignals, "y_signals");
            } else {
                graph.ySignals = splitCommaList(simulation.captureSignals);
            }
            const auto applyLimit = [&](const char *key, std::optional<double> &value) {
                const auto it = arguments.find(key);
                if (it == arguments.end())
                    return;
                if (it->is_null())
                    value.reset();
                else
                    value = numberValue(*it, key);
            };
            applyLimit("x_min", graph.xMin);
            applyLimit("x_max", graph.xMax);
            applyLimit("y_min", graph.yMin);
            applyLimit("y_max", graph.yMax);
            ice::ObjectId graphId = 0;
            session.execute("mcp simulation-graph-create", [&](ice::Project &project) {
                graphId = ice::addSimulationGraph(project, simulationId, graph);
            });
            const auto &storedSimulation = session.project().simulations.at(simulationId);
            for (const auto &entry : storedSimulation.graphs) {
                if (entry.id == graphId)
                    return graphWithSimulation(storedSimulation, entry);
            }
            throw ToolError("Created graph could not be found.");
        }

        if (name == "simulation_graph_list") {
            json result = json::array();
            const auto simulationValue = arguments.find("simulation");
            const std::optional<ice::ObjectId> simulationId =
                simulationValue != arguments.end() && !simulationValue->is_null()
                    ? std::optional<ice::ObjectId>(resolveSimulation(session.project(), *simulationValue))
                    : std::nullopt;
            for (const auto &[id, simulation] : session.project().simulations) {
                if (simulationId && id != *simulationId)
                    continue;
                for (const auto &graph : simulation.graphs)
                    result.push_back(graphWithSimulation(simulation, graph));
            }
            return result;
        }

        if (name == "simulation_graph_info") {
            const auto simulationId = resolveSimulation(session.project(), requiredValue(arguments, "simulation"));
            const auto &simulation = session.project().simulations.at(simulationId);
            const auto index = resolveGraphIndex(simulation, requiredValue(arguments, "graph"));
            return graphWithSimulation(simulation, simulation.graphs[index]);
        }

        if (name == "simulation_graph_update") {
            const auto simulationId = resolveSimulation(session.project(), requiredValue(arguments, "simulation"));
            const auto &simulation = session.project().simulations.at(simulationId);
            const auto index = resolveGraphIndex(simulation, requiredValue(arguments, "graph"));
            auto updated = simulation.graphs[index];
            const auto nameValue = arguments.find("name");
            if (nameValue != arguments.end() && !nameValue->is_null()) {
                if (!nameValue->is_string() || nameValue->get<std::string>().empty())
                    throw ToolError("Argument must be a non-empty string: name");
                updated.name = nameValue->get<std::string>();
            }
            const auto xAxisValue = arguments.find("x_axis");
            if (xAxisValue != arguments.end() && !xAxisValue->is_null()) {
                if (!xAxisValue->is_string())
                    throw ToolError("Argument must be a string: x_axis");
                updated.xAxis = xAxisValue->get<std::string>();
            }
            const auto ySignalsValue = arguments.find("y_signals");
            if (ySignalsValue != arguments.end() && !ySignalsValue->is_null())
                updated.ySignals = parseSignalListValue(*ySignalsValue, "y_signals");
            if (optionalBoolean(arguments, "clear_limits", false)) {
                updated.xMin.reset();
                updated.xMax.reset();
                updated.yMin.reset();
                updated.yMax.reset();
            }
            const auto applyLimit = [&](const char *key, std::optional<double> &value) {
                const auto it = arguments.find(key);
                if (it == arguments.end())
                    return;
                if (it->is_null())
                    value.reset();
                else
                    value = numberValue(*it, key);
            };
            applyLimit("x_min", updated.xMin);
            applyLimit("x_max", updated.xMax);
            applyLimit("y_min", updated.yMin);
            applyLimit("y_max", updated.yMax);
            session.execute("mcp simulation-graph-update", [&](ice::Project &project) {
                ice::updateSimulationGraph(project, simulationId, updated.id, updated);
            });
            const auto &storedSimulation = session.project().simulations.at(simulationId);
            for (const auto &entry : storedSimulation.graphs)
                if (entry.id == updated.id)
                    return graphWithSimulation(storedSimulation, entry);
            throw ToolError("Updated graph could not be found.");
        }

        if (name == "simulation_graph_delete") {
            const auto simulationId = resolveSimulation(session.project(), requiredValue(arguments, "simulation"));
            const auto &simulation = session.project().simulations.at(simulationId);
            if (simulation.graphs.size() <= 1)
                throw ToolError("A simulation must have at least one graph.");
            const auto index = resolveGraphIndex(simulation, requiredValue(arguments, "graph"));
            const auto graph = simulation.graphs[index];
            session.execute("mcp simulation-graph-delete", [&](ice::Project &project) {
                ice::removeSimulationGraph(project, simulationId, graph.id);
            });
            return {{"id", graph.id}, {"name", graph.name}, {"simulation", simulationId}, {"deleted", true}};
        }

        if (name == "simulation_delete") {
            const auto id = resolveSimulation(session.project(), requiredValue(arguments, "simulation"));
            const auto simulationName = session.project().simulations.at(id).name;
            session.execute("mcp simulation-delete",
                            [&](ice::Project &project) { ice::removeSimulationItem(project, id); });
            return {{"id", id}, {"name", simulationName}, {"deleted", true}};
        }

        if (toolName == "report_create") {
            const auto reportName = requiredString(toolArguments, "name");
            const auto source = optionalString(toolArguments, "source");
            const auto root = projectRootPath();
            ice::ObjectId id = 0;
            session.execute("mcp simulation-script-create", [&](ice::Project &project) {
                id = ice::createProjectReport(project, root, reportName, source);
            });
            const auto &report = session.project().reports.at(id);
            return {{"id", id},
                    {"name", report.name},
                    {"path", report.path},
                    {"source", ice::readProjectReportSource(root, report)}};
        }
        if (toolName == "report_list") {
            json reports = json::array();
            for (const auto &[id, report] : session.project().reports)
                reports.push_back({{"id", id}, {"name", report.name}, {"path", report.path}});
            return reports;
        }
        if (toolName == "report_read") {
            const auto id = lookupByIdOrName(session.project().reports, requiredValue(toolArguments, "report"),
                                             "simulation script");
            const auto &report = session.project().reports.at(id);
            return {{"id", id},
                    {"name", report.name},
                    {"path", report.path},
                    {"source", ice::readProjectReportSource(projectRootPath(), report)}};
        }
        if (toolName == "report_write") {
            const auto id = lookupByIdOrName(session.project().reports, requiredValue(toolArguments, "report"),
                                             "simulation script");
            const auto &report = session.project().reports.at(id);
            const auto source = requiredStringAllowEmpty(toolArguments, "source");
            ice::writeProjectReportSource(projectRootPath(), report, source);
            return {{"id", id}, {"name", report.name}, {"path", report.path}, {"bytes", source.size()}};
        }
        if (toolName == "report_run") {
            const auto id = lookupByIdOrName(session.project().reports, requiredValue(toolArguments, "report"),
                                             "simulation script");
            const auto &report = session.project().reports.at(id);
            const auto timeoutValue = optionalInteger(toolArguments, "timeout_ms").value_or(30000);
            const auto timeout = std::chrono::milliseconds(std::max<std::int64_t>(1, timeoutValue));
            const auto result = ice::runProjectReport(projectRootPath(), report, executablePath_, timeout);
            json graphs = json::array();
            if (!result.graphsText.empty()) {
                const auto document = json::parse(result.graphsText, nullptr, false);
                if (!document.is_discarded() && document.contains("graphs"))
                    graphs = document.at("graphs");
            }
            return {{"id", id},
                    {"name", report.name},
                    {"path", report.path},
                    {"ok", !result.timedOut && result.exitCode == 0},
                    {"timed_out", result.timedOut},
                    {"exit_code", result.exitCode},
                    {"stdout", result.stdoutText},
                    {"stderr", result.stderrText},
                    {"graphs", std::move(graphs)}};
        }
        if (toolName == "report_delete") {
            const auto id = lookupByIdOrName(session.project().reports, requiredValue(toolArguments, "report"),
                                             "simulation script");
            const auto report = session.project().reports.at(id);
            const bool purge = optionalBoolean(toolArguments, "purge", false);
            session.execute("mcp simulation-script-delete", [&](ice::Project &project) { project.reports.erase(id); });
            if (purge) {
                std::error_code status;
                std::filesystem::remove(projectReportPath(projectRootPath(), report), status);
                if (status)
                    throw ToolError("Unable to purge script file: " + status.message());
            }
            return {{"id", id}, {"name", report.name}, {"path", report.path}, {"deleted", true}, {"purged", purge}};
        }

        if (name == "schematic_delete") {
            const auto id = resolveSchematic(session.project(), arguments, "schematic");
            const auto schematicName = session.project().schematicCells.at(id).name;
            session.execute("mcp schematic-delete",
                            [&](ice::Project &project) { ice::removeSchematicCell(project, id); });
            return {{"id", id}, {"name", schematicName}, {"deleted", true}};
        }

        if (name == "schematic_create") {
            ice::ObjectId id = 0;
            const auto pdk = optionalString(arguments, "pdk", ice::genericPdkId);
            const auto cellName = requiredString(arguments, "name");
            const auto kind = optionalString(arguments, "kind", "drawn");
            auto text = optionalString(arguments, "text");
            if (kind != "drawn" && kind != "text")
                throw ToolError("schematic_create kind must be 'drawn' or 'text'");
            if (kind == "text" || !text.empty()) {
                if (text.empty())
                    text = "* SPICE schematic: " + cellName + "\n.title " + cellName + "\n.end\n";
                session.execute("mcp schematic-create-text", [&](ice::Project &project) {
                    id = ice::createTextSchematicCell(project, cellName, pdk, text);
                });
                return {{"id", id}, {"name", cellName}, {"pdk", pdk}, {"kind", "text"}, {"text", text}};
            }
            session.execute("mcp schematic-create",
                            [&](ice::Project &project) { id = ice::createSchematicCell(project, cellName, pdk); });
            return {{"id", id}, {"name", cellName}, {"pdk", pdk}, {"kind", "drawn"}};
        }
        if (name == "schematic_create_text" || name == "schematic_add_text") {
            ice::ObjectId id = 0;
            const auto pdk = optionalString(arguments, "pdk", ice::genericPdkId);
            const auto cellName = requiredString(arguments, "name");
            auto text = optionalString(arguments, "text");
            if (text.empty())
                text = "* SPICE schematic: " + cellName + "\n.title " + cellName + "\n.end\n";
            session.execute("mcp schematic-create-text", [&](ice::Project &project) {
                id = ice::createTextSchematicCell(project, cellName, pdk, text);
            });
            return {{"id", id}, {"name", cellName}, {"pdk", pdk}, {"kind", "text"}, {"text", text}};
        }
        if (name == "schematic_set_text") {
            const auto id = resolveSchematicArgument(session.project(), arguments, {"schematic", "cell"});
            auto &cell = session.project().schematicCells.at(id);
            const bool wasText = cell.textSchematic;
            const bool convert = optionalBoolean(arguments, "convert", false);
            if (!cell.textSchematic) {
                const bool empty =
                    cell.symbols.empty() && cell.instances.empty() && cell.nets.empty() && cell.parameters.empty();
                if (!convert || !empty) {
                    throw ToolError("Schematic '" + cell.name +
                                    "' is a drawn schematic, not a text SPICE cell. Use schematic_create with "
                                    "kind='text' or schematic_create_text for a text cell. To convert an empty "
                                    "drawn cell, pass convert=true. Do not edit sch/*.icesch by hand.");
                }
            }
            const auto text = requiredStringAllowEmpty(arguments, "text");
            session.execute("mcp schematic-set-text", [&](ice::Project &project) {
                auto &target = project.schematicCells.at(id);
                target.textSchematic = true;
                target.spiceText = text;
            });
            return {{"id", id}, {"name", cell.name}, {"text", text}, {"converted", !wasText}};
        }
        if (name == "schematic_list") {
            json result = json::array();
            for (const auto &[id, cell] : session.project().schematicCells) {
                result.push_back({{"id", id},
                                  {"name", cell.name},
                                  {"pdk", cell.pdk},
                                  {"text", cell.textSchematic},
                                  {"symbols", cell.symbols.size()},
                                  {"instances", cell.instances.size()},
                                  {"nets", cell.nets.size()}});
            }
            return result;
        }
        if (name == "schematic_info") {
            const auto id = resolveSchematicArgument(session.project(), arguments, {"schematic", "cell"});
            return schematicJson(session.project().schematicCells.at(id));
        }
        if (name == "schematic_add_symbol") {
            ice::ObjectId id = 0;
            const auto cell = resolveSchematicArgument(session.project(), arguments, {"cell", "schematic"});
            const auto symbolName = requiredString(arguments, "name");
            const auto artwork = optionalString(arguments, "artwork");
            ice::ObjectId sourceCell = 0;
            const auto source = arguments.find("source_cell");
            if (source != arguments.end() && !source->is_null())
                sourceCell = lookupByIdOrName(session.project().schematicCells, *source, "schematic");
            const auto &cellRef = session.project().schematicCells.at(cell);
            for (const auto &[existingId, existing] : cellRef.symbols) {
                if (existing.name == symbolName) {
                    throw ToolError("Symbol '" + symbolName + "' already exists with id " + std::to_string(existingId) +
                                    "; reuse it instead of adding a duplicate");
                }
            }

            struct PinInput {
                std::string name;
                ice::Coordinate x = 0;
                ice::Coordinate y = 0;
                ice::PinDirection direction = ice::PinDirection::right;
            };
            std::vector<PinInput> pinInputs;
            const auto pinsValue = arguments.find("pins");
            if (pinsValue != arguments.end() && !pinsValue->is_null()) {
                if (!pinsValue->is_array())
                    throw ToolError("Argument 'pins' must be an array.");
                std::set<std::string> pinNames;
                for (const auto &pin : *pinsValue) {
                    if (!pin.is_object())
                        throw ToolError("Each pin must be an object.");
                    PinInput input;
                    input.name = requiredString(pin, "name");
                    if (!pinNames.insert(input.name).second)
                        throw ToolError("Duplicate pin name: " + input.name);
                    if (pin.contains("x"))
                        input.x = coordinateValue(pin.at("x"), "pin x");
                    if (pin.contains("y"))
                        input.y = coordinateValue(pin.at("y"), "pin y");
                    input.direction = parsePinDirection(optionalString(pin, "direction", "right"));
                    pinInputs.push_back(std::move(input));
                }
            }
            session.execute("mcp schematic-add-symbol", [&](ice::Project &project) {
                id = ice::addSymbol(project, cell, symbolName, artwork, sourceCell);
                auto &symbol = project.schematicCells.at(cell).symbols.at(id);
                for (auto &input : pinInputs) {
                    const auto pinId = project.nextId();
                    symbol.pins.push_back({pinId, input.name, input.x, input.y, input.direction});
                }
            });
            auto result = symbolJson(session.project().schematicCells.at(cell).symbols.at(id));
            result["cell"] = cell;
            return result;
        }
        if (name == "schematic_add_pin") {
            const auto cellId = resolveSchematicArgument(session.project(), arguments, {"cell", "schematic"});
            const auto &cell = session.project().schematicCells.at(cellId);
            ice::ObjectId symbolId = 0;
            if (const auto symbolValue = arguments.find("symbol_id");
                symbolValue != arguments.end() && !symbolValue->is_null()) {
                symbolId = resolveSymbol(cell, *symbolValue);
            } else if (const auto symbolValue = arguments.find("symbol");
                       symbolValue != arguments.end() && !symbolValue->is_null()) {
                symbolId = resolveSymbol(cell, *symbolValue);
            } else {
                throw ToolError("Missing required argument: symbol or symbol_id");
            }
            const auto pinName = requiredString(arguments, "name");
            const auto x = optionalInteger(arguments, "x").value_or(0);
            const auto y = optionalInteger(arguments, "y").value_or(0);
            const auto direction = parsePinDirection(optionalString(arguments, "direction", "right"));
            const auto &symbol = cell.symbols.at(symbolId);
            if (findPinInSymbol(symbol, pinName))
                throw ToolError("Pin '" + pinName + "' already exists on symbol " + symbol.name);
            ice::ObjectId pinId = 0;
            session.execute("mcp schematic-add-pin", [&](ice::Project &project) {
                pinId = project.nextId();
                project.schematicCells.at(cellId).symbols.at(symbolId).pins.push_back(
                    {pinId, pinName, x, y, direction});
            });
            return {{"id", pinId},
                    {"cell", cellId},
                    {"symbol", symbolId},
                    {"symbol_name", symbol.name},
                    {"name", pinName},
                    {"x", x},
                    {"y", y},
                    {"direction", pinDirectionName(direction)}};
        }
        if (name == "schematic_add_net") {
            const auto cellId = resolveSchematicArgument(session.project(), arguments, {"cell", "schematic"});
            const auto &cell = session.project().schematicCells.at(cellId);
            const auto &pinsValue = requiredValue(arguments, "pins");
            if (!pinsValue.is_array() || pinsValue.size() < 2)
                throw ToolError("Argument 'pins' must be an array with at least two pin ids or names.");
            std::vector<ice::ObjectId> pins;
            pins.reserve(pinsValue.size());
            for (const auto &pin : pinsValue)
                pins.push_back(resolvePin(cell, pin));
            const auto netName = optionalString(arguments, "name");
            const auto x = optionalInteger(arguments, "x").value_or(0);
            const auto y = optionalInteger(arguments, "y").value_or(0);
            ice::ObjectId netId = 0;
            session.execute("mcp schematic-add-net",
                            [&](ice::Project &project) { netId = ice::addNet(project, cellId, netName, pins, x, y); });
            const auto &net = session.project().schematicCells.at(cellId).nets.at(netId);
            return {{"id", net.id},     {"cell", cellId}, {"name", net.name},
                    {"pins", net.pins}, {"x", net.x},     {"y", net.y}};
        }
        if (name == "schematic_netlist") {
            const auto cellId = resolveSchematicArgument(session.project(), arguments, {"cell", "schematic"});
            std::vector<std::string> notes;
            const auto netlist = ice::buildSimulationNetlist(session.project(), cellId, &notes);
            return {{"id", cellId},
                    {"name", session.project().schematicCells.at(cellId).name},
                    {"netlist", netlist},
                    {"notes", notes}};
        }
        if (name == "schematic_add_instance") {
            ice::ObjectId id = 0;
            const auto cellId = resolveSchematicArgument(session.project(), arguments, {"cell", "schematic"});
            const auto &cell = session.project().schematicCells.at(cellId);
            ice::ObjectId symbolId = 0;
            if (const auto symbolValue = arguments.find("symbol_id");
                symbolValue != arguments.end() && !symbolValue->is_null()) {
                symbolId = resolveSymbol(cell, *symbolValue);
            } else if (const auto symbolValue = arguments.find("symbol");
                       symbolValue != arguments.end() && !symbolValue->is_null()) {
                symbolId = resolveSymbol(cell, *symbolValue);
            } else {
                throw ToolError("Missing required argument: symbol or symbol_id");
            }
            const auto instanceName = requiredString(arguments, "name");
            const auto x = requiredInteger(arguments, "x");
            const auto y = requiredInteger(arguments, "y");
            session.execute("mcp schematic-add-instance", [&](ice::Project &project) {
                id = ice::addSchematicInstance(project, cellId, symbolId, instanceName, x, y);
            });
            return {{"id", id},
                    {"cell", cellId},
                    {"name", instanceName},
                    {"symbol", symbolId},
                    {"symbol_name", cell.symbols.at(symbolId).name}};
        }
        if (name == "layout_delete") {
            const auto id = resolveLayoutItem(session.project(), requiredValue(arguments, "layout"));
            std::string layoutName;
            if (const auto procedural = session.project().proceduralLayouts.find(id);
                procedural != session.project().proceduralLayouts.end())
                layoutName = procedural->second.name;
            else
                layoutName = session.project().layoutCells.at(id).name;
            session.execute("mcp layout-delete", [&](ice::Project &project) { ice::removeLayoutItem(project, id); });
            return {{"id", id}, {"name", layoutName}, {"deleted", true}};
        }

        if (name == "layout_create") {
            ice::ObjectId id = 0;
            const auto pdk = optionalString(arguments, "pdk", ice::genericPdkId);
            const auto cellName = requiredString(arguments, "name");
            session.execute("mcp layout-create",
                            [&](ice::Project &project) { id = ice::createLayoutCell(project, cellName, pdk); });
            return {{"id", id}, {"name", cellName}, {"pdk", pdk}};
        }
        if (name == "layout_create_from_schematic") {
            ice::ObjectId id = 0;
            const auto schematic = resolveSchematic(session.project(), arguments, "schematic");
            const auto layoutName = optionalString(arguments, "name");
            session.execute("mcp layout-create-from-schematic", [&](ice::Project &project) {
                id = ice::createLayoutFromSchematic(project, schematic, layoutName);
            });
            return {{"id", id}, {"schematic", schematic}};
        }
        if (name == "layout_add_layer") {
            ice::ObjectId id = 0;
            const auto layerName = requiredString(arguments, "name");
            const auto number = static_cast<int>(requiredInteger(arguments, "number"));
            session.execute("mcp layout-add-layer",
                            [&](ice::Project &project) { id = ice::addLayer(project, layerName, number); });
            return {{"id", id}, {"name", layerName}, {"number", number}};
        }
        if (name == "layout_add_rectangle") {
            ice::ObjectId id = 0;
            const auto cell = resolveLayoutCell(session.project(), arguments);
            const auto layer = resolveLayer(session.project(), arguments);
            const ice::Rect rectangle{requiredInteger(arguments, "x"), requiredInteger(arguments, "y"),
                                      requiredInteger(arguments, "width"), requiredInteger(arguments, "height")};
            session.execute("mcp layout-add-rectangle",
                            [&](ice::Project &project) { id = ice::addRectangle(project, cell, layer, rectangle); });
            return {{"id", id}, {"cell", cell}, {"layer", layer}};
        }
        if (name == "layout_add_text") {
            ice::ObjectId id = 0;
            const auto cell = resolveLayoutCell(session.project(), arguments);
            const auto layer = resolveLayer(session.project(), arguments);
            const auto x = requiredInteger(arguments, "x");
            const auto y = requiredInteger(arguments, "y");
            const auto text = requiredString(arguments, "text");
            const auto height = optionalInteger(arguments, "height").value_or(1000);
            session.execute("mcp layout-add-text", [&](ice::Project &project) {
                id = ice::addText(project, cell, layer, x, y, text, height);
            });
            return {{"id", id}, {"cell", cell}, {"layer", layer}, {"text", text}};
        }
        if (name == "layout_add_polygon") {
            ice::ObjectId id = 0;
            const auto cell = resolveLayoutCell(session.project(), arguments);
            const auto layer = resolveLayer(session.project(), arguments);
            const auto points = parsePoints(arguments);
            session.execute("mcp layout-add-polygon",
                            [&](ice::Project &project) { id = ice::addPolygon(project, cell, layer, points); });
            return {{"id", id}, {"cell", cell}, {"layer", layer}, {"points", points.size()}};
        }
        if (name == "layout_add_port") {
            ice::ObjectId id = 0;
            const auto cell = resolveLayoutCell(session.project(), arguments);
            const auto layer = resolveLayer(session.project(), arguments);
            const auto portName = requiredString(arguments, "name");
            const ice::Rect rectangle{requiredInteger(arguments, "x"), requiredInteger(arguments, "y"),
                                      requiredInteger(arguments, "width"), requiredInteger(arguments, "height")};
            const auto direction = parsePortDirectionArgument(optionalString(arguments, "direction", "inout"));
            const auto side = parsePortSideArgument(optionalString(arguments, "side", "left"));
            const auto netValue = optionalString(arguments, "net");
            const auto net = netValue.empty() ? ice::ObjectId{0}
                                              : resolveLayoutNet(session.project().layoutCells.at(cell), netValue);
            const auto label = optionalString(arguments, "label", portName);
            const bool visible = optionalBoolean(arguments, "visible", true);
            const bool locked = optionalBoolean(arguments, "locked", false);
            session.execute("mcp layout-add-port", [&](ice::Project &project) {
                id = ice::addPort(project, cell, portName, layer, rectangle, direction, side, net, label);
                auto &port = project.layoutCells.at(cell).ports.at(id);
                port.visible = visible;
                port.locked = locked;
            });
            return {{"id", id},
                    {"cell", cell},
                    {"name", portName},
                    {"direction", ice::portDirectionName(direction)},
                    {"side", ice::portSideName(side)},
                    {"net", net == 0 ? json(nullptr) : json(net)}};
        }
        if (name == "layout_list_ports") {
            const auto cell = resolveLayoutCell(session.project(), arguments);
            const auto &layout = session.project().layoutCells.at(cell);
            json ports = json::array();
            for (const auto &[id, port] : layout.ports) {
                ports.push_back({{"id", id},
                                 {"name", port.name},
                                 {"layer", port.layer},
                                 {"rectangle",
                                  {{"x", port.rectangle.x},
                                   {"y", port.rectangle.y},
                                   {"width", port.rectangle.width},
                                   {"height", port.rectangle.height}}},
                                 {"direction", ice::portDirectionName(port.direction)},
                                 {"side", ice::portSideName(port.side)},
                                 {"net", port.net == 0 ? json(nullptr) : json(port.net)},
                                 {"label", port.label},
                                 {"visible", port.visible},
                                 {"locked", port.locked},
                                 {"source_instance", port.sourceInstance},
                                 {"source_port", port.sourcePort}});
            }
            return {{"cell", cell}, {"ports", std::move(ports)}};
        }
        if (name == "layout_update_port") {
            const auto cell = resolveLayoutCell(session.project(), arguments);
            const auto &layout = session.project().layoutCells.at(cell);
            const auto portId = resolveLayoutPort(layout, requiredValue(arguments, "port"));
            auto port = layout.ports.at(portId);
            if (arguments.contains("name"))
                port.name = requiredString(arguments, "name");
            if (arguments.contains("label"))
                port.label = requiredString(arguments, "label");
            if (arguments.contains("direction"))
                port.direction = parsePortDirectionArgument(requiredString(arguments, "direction"));
            if (arguments.contains("side"))
                port.side = parsePortSideArgument(requiredString(arguments, "side"));
            if (arguments.contains("net")) {
                if (arguments.at("net").is_null())
                    port.net = 0;
                else
                    port.net = resolveLayoutNet(layout, arguments.at("net"));
            }
            if (arguments.contains("x"))
                port.rectangle.x = requiredInteger(arguments, "x");
            if (arguments.contains("y"))
                port.rectangle.y = requiredInteger(arguments, "y");
            if (arguments.contains("width"))
                port.rectangle.width = requiredInteger(arguments, "width");
            if (arguments.contains("height"))
                port.rectangle.height = requiredInteger(arguments, "height");
            if (arguments.contains("visible"))
                port.visible = optionalBoolean(arguments, "visible");
            if (arguments.contains("locked"))
                port.locked = optionalBoolean(arguments, "locked");
            session.execute("mcp layout-update-port", [&](ice::Project &project) {
                if (!ice::updatePort(project, cell, port))
                    throw ToolError("Unknown layout port");
            });
            return {{"id", portId}, {"cell", cell}};
        }
        if (name == "layout_remove_port") {
            const auto cell = resolveLayoutCell(session.project(), arguments);
            const auto portId =
                resolveLayoutPort(session.project().layoutCells.at(cell), requiredValue(arguments, "port"));
            session.execute("mcp layout-remove-port",
                            [&](ice::Project &project) { ice::removePort(project, cell, portId); });
            return {{"id", portId}, {"cell", cell}};
        }
        if (name == "layout_add_net") {
            const auto cell = resolveLayoutCell(session.project(), arguments);
            const auto netName = requiredString(arguments, "name");
            ice::ObjectId id = 0;
            session.execute("mcp layout-add-net",
                            [&](ice::Project &project) { id = ice::createLayoutNet(project, cell, netName); });
            return {{"id", id}, {"cell", cell}, {"name", netName}};
        }
        if (name == "layout_assign_port_net") {
            const auto cell = resolveLayoutCell(session.project(), arguments);
            const auto &layout = session.project().layoutCells.at(cell);
            const auto portId = resolveLayoutPort(layout, requiredValue(arguments, "port"));
            ice::ObjectId netId = 0;
            if (arguments.contains("net") && !arguments.at("net").is_null())
                netId = resolveLayoutNet(layout, arguments.at("net"));
            session.execute("mcp layout-assign-port-net",
                            [&](ice::Project &project) { ice::assignPortNet(project, cell, portId, netId); });
            return {{"port", portId}, {"net", netId == 0 ? json(nullptr) : json(netId)}};
        }
        if (name == "layout_add_instance") {
            ice::ObjectId id = 0;
            const auto parent = resolveLayoutCell(session.project(), arguments, "parent");
            const auto child = resolveLayoutCell(session.project(), arguments, "child");
            const auto instanceName = requiredString(arguments, "name");
            const auto x = requiredInteger(arguments, "x");
            const auto y = requiredInteger(arguments, "y");
            const auto orientation = parseOrientation(optionalString(arguments, "orientation", "r0"));
            session.execute("mcp layout-add-instance", [&](ice::Project &project) {
                id = ice::addLayoutInstance(project, parent, child, instanceName, x, y, orientation);
            });
            return {{"id", id}, {"parent", parent}, {"child", child}, {"name", instanceName}};
        }

        if (name == "lvs_run") {
            const auto layout = resolveLayoutCell(session.project(), arguments, "layout");
            ice::ObjectId schematic = 0;
            const auto schematicValue = arguments.find("schematic");
            if (schematicValue != arguments.end() && !schematicValue->is_null())
                schematic = lookupByIdOrName(session.project().schematicCells, *schematicValue, "schematic");
            else
                schematic = ice::schematicForLayout(session.project(), layout);
            if (schematic == 0)
                throw ToolError("Unable to infer a schematic for the layout; provide 'schematic'.");
            const auto report = ice::runLvs(session.project(), schematic, layout);
            return json::parse(ice::serializeLvsReport(report));
        }

        throw ToolError("Unknown tool: " + name);
    }

#ifdef ICE_HAVE_AGENTSDK
    std::vector<agentsdk::mcp::tool> listTools() const {
        using agentsdk::mcp::tool_builder;
        std::vector<agentsdk::mcp::tool> tools;
        tools.push_back(tool_builder("ice_version")
                            .with_description("Return the ICE build fingerprint and feature flags.")
                            .build());
        tools.push_back(tool_builder("pdk_info")
                            .with_description("Inspect ICE PDK packages, ngspice models, OSDI setup, and sections.")
                            .with_string_param("pdk", "Optional PDK id or path; omit to list all", false)
                            .with_boolean_param("probe", "Run a minimal transistor probe", false)
                            .build());
        tools.push_back(tool_builder("pdk_build")
                            .with_description("Build an ICE PDK package's Verilog-A sources into the OSDI cache.")
                            .with_string_param("pdk", "PDK id, package name, or package path")
                            .build());
        tools.push_back(tool_builder("project_create")
                            .with_description("Create and open a new ICE project.")
                            .with_string_param("path", "Project folder or project.icepro path")
                            .with_string_param("name", "Project name")
                            .build());
        tools.push_back(tool_builder("project_open")
                            .with_description("Open an existing ICE project.")
                            .with_string_param("path", "Project folder or project.icepro path")
                            .with_boolean_param("recover", "Quarantine invalid sidecar files and open the rest", false)
                            .build());
        tools.push_back(tool_builder("project_repair")
                            .with_description("Recover a project by quarantining invalid sidecar files and saving it.")
                            .with_string_param("path", "Optional project folder or project.icepro path", false)
                            .build());
        tools.push_back(tool_builder("project_migrate")
                            .with_description("Refresh the generated AGENTS.md guidance while preserving user text.")
                            .with_string_param("path", "Optional project folder or project.icepro path", false)
                            .build());
        tools.push_back(
            tool_builder("project_summary").with_description("Summarize the open project and its cells.").build());
        tools.push_back(tool_builder("project_validate")
                            .with_description("Validate the open project and return diagnostics.")
                            .build());
        tools.push_back(
            tool_builder("project_reload")
                .with_description("Reload the open project from disk.")
                .with_boolean_param("recover", "Quarantine invalid sidecar files and reload the rest", false)
                .build());
        tools.push_back(tool_builder("project_undo").with_description("Undo the last project mutation.").build());
        tools.push_back(tool_builder("project_redo").with_description("Redo the next project mutation.").build());
        tools.push_back(
            tool_builder("ice_doctor")
                .with_description("Inspect ngspice, Python, report-runner, PDK readiness/model libraries, "
                                  "and optional schematic/simulation includes.")
                .with_string_param("schematic", "Optional schematic to validate includes", false)
                .with_string_param("simulation", "Optional simulation whose schematic includes to validate", false)
                .with_boolean_param("probe", "Run a minimal transistor load check for the project PDK", false)
                .build());
        tools.push_back(tool_builder("simulation_create")
                            .with_description("Create a simulation and its first graph.")
                            .with_string_param("name", "Simulation name")
                            .with_string_param("schematic", "Schematic name or id")
                            .with_string_param("analysis", "Analysis type", false)
                            .with_number_param("stop", "Stop time", false)
                            .with_number_param("step", "Time step", false)
                            .with_string_param("signals", "Signals to capture", false)
                            .with_string_param("graph_name", "Initial graph name", false)
                            .build());
        tools.push_back(tool_builder("simulation_update")
                            .with_description("Update simulation settings and capture signals.")
                            .with_string_param("simulation", "Simulation name or id")
                            .with_string_param("analysis", "Analysis type", false)
                            .with_number_param("stop", "Stop time", false)
                            .with_number_param("step", "Time step", false)
                            .with_string_param("signals", "Signals to capture", false)
                            .build());
        tools.push_back(tool_builder("simulation_run")
                            .with_description("Run a simulation and store a compact run artifact.")
                            .with_string_param("simulation", "Simulation name or id")
                            .with_integer_param("timeout_ms", "Timeout in milliseconds", false)
                            .build());
        tools.push_back(tool_builder("simulation_result")
                            .with_description("Read the latest stored run for a simulation with trace statistics.")
                            .with_string_param("simulation", "Simulation name or id")
                            .build());
        tools.push_back(tool_builder("simulation_measure")
                            .with_description("Measure zero crossings, frequency, amplitude, and settling time.")
                            .with_string_param("simulation", "Simulation name or id")
                            .with_string_param("signal", "Optional captured signal name", false)
                            .with_number_param("threshold", "Optional crossing threshold", false)
                            .with_number_param("start", "Optional start time for frequency measurements", false)
                            .with_number_param("settle_tolerance", "Relative settling tolerance (default 0.02)", false)
                            .build());
        tools.push_back(tool_builder("simulation_signals")
                            .with_description("List available signals and trace sizes from the latest stored run.")
                            .with_string_param("simulation", "Simulation name or id")
                            .build());
        tools.push_back(tool_builder("simulation_graph_create")
                            .with_description("Add a graph definition to a simulation.")
                            .with_string_param("simulation", "Simulation name or id")
                            .with_string_param("name", "Graph name")
                            .with_string_param("x_axis", "Optional X-axis signal or time", false)
                            .with_string_param("y_signals", "Optional comma-separated Y signals", false)
                            .with_number_param("x_min", "Optional X minimum", false)
                            .with_number_param("x_max", "Optional X maximum", false)
                            .with_number_param("y_min", "Optional Y minimum", false)
                            .with_number_param("y_max", "Optional Y maximum", false)
                            .build());
        tools.push_back(tool_builder("simulation_graph_list")
                            .with_description("List simulation graph definitions.")
                            .with_string_param("simulation", "Optional simulation name or id", false)
                            .build());
        tools.push_back(tool_builder("simulation_graph_info")
                            .with_description("Return one simulation graph definition.")
                            .with_string_param("simulation", "Simulation name or id")
                            .with_string_param("graph", "Graph name or id")
                            .build());
        tools.push_back(tool_builder("simulation_graph_update")
                            .with_description("Update a simulation graph definition.")
                            .with_string_param("simulation", "Simulation name or id")
                            .with_string_param("graph", "Graph name or id")
                            .with_string_param("name", "Optional new graph name", false)
                            .with_string_param("x_axis", "Optional new X-axis signal or time", false)
                            .with_string_param("y_signals", "Optional new comma-separated Y signals", false)
                            .with_number_param("x_min", "Optional X minimum", false)
                            .with_number_param("x_max", "Optional X maximum", false)
                            .with_number_param("y_min", "Optional Y minimum", false)
                            .with_number_param("y_max", "Optional Y maximum", false)
                            .with_boolean_param("clear_limits", "Clear all axis limits before applying new ones", false)
                            .build());
        tools.push_back(tool_builder("simulation_graph_delete")
                            .with_description("Delete a simulation graph; one graph must remain.")
                            .with_string_param("simulation", "Simulation name or id")
                            .with_string_param("graph", "Graph name or id")
                            .build());
        tools.push_back(tool_builder("simulation_delete")
                            .with_description("Delete a simulation and all its graph definitions.")
                            .with_string_param("simulation", "Simulation name or id")
                            .build());
        tools.push_back(tool_builder("simulation_script_create")
                            .with_description("Create a project-level Python simulation script under scripts/.")
                            .with_string_param("name", "Simulation script name")
                            .with_string_param("source", "Optional initial Python source", false)
                            .build());
        tools.push_back(
            tool_builder("simulation_script_list").with_description("List project-level simulation scripts.").build());
        tools.push_back(tool_builder("simulation_script_read")
                            .with_description("Read a simulation script's metadata and Python source.")
                            .with_string_param("script", "Simulation script name or id")
                            .build());
        tools.push_back(tool_builder("simulation_script_write")
                            .with_description("Replace a simulation script's Python source.")
                            .with_string_param("script", "Simulation script name or id")
                            .with_string_param("source", "Python source")
                            .build());
        tools.push_back(tool_builder("simulation_script_run")
                            .with_description("Run a simulation script and return its output and generated graphs.")
                            .with_string_param("script", "Simulation script name or id")
                            .with_integer_param("timeout_ms", "Optional timeout in milliseconds", false)
                            .build());
        tools.push_back(tool_builder("simulation_script_delete")
                            .with_description("Delete simulation script metadata; optionally purge its Python file.")
                            .with_string_param("script", "Simulation script name or id")
                            .with_boolean_param("purge", "Also remove the Python source file", false)
                            .build());
        tools.push_back(tool_builder("schematic_list")
                            .with_description("List schematic cells with symbol, instance, and net counts.")
                            .build());
        tools.push_back(tool_builder("schematic_info")
                            .with_description("Return symbols, pins, instances, and nets for a schematic.")
                            .with_string_param("schematic", "Schematic name or id")
                            .build());
        tools.push_back(tool_builder("schematic_create")
                            .with_description("Create a schematic cell; kind='text' creates a SPICE text cell.")
                            .with_string_param("name", "Schematic name")
                            .with_string_param("pdk", "Optional PDK identifier", false)
                            .with_string_param("kind", "drawn or text (default drawn)", false)
                            .with_string_param("text", "Optional SPICE source for kind=text", false)
                            .build());
        tools.push_back(tool_builder("schematic_create_text")
                            .with_description("Create a text SPICE schematic cell.")
                            .with_string_param("name", "Schematic name")
                            .with_string_param("pdk", "Optional PDK identifier", false)
                            .with_string_param("text", "Optional SPICE source", false)
                            .build());
        tools.push_back(tool_builder("schematic_add_text")
                            .with_description("Alias for schematic_create_text; creates a text SPICE cell.")
                            .with_string_param("name", "Schematic name")
                            .with_string_param("pdk", "Optional PDK identifier", false)
                            .with_string_param("text", "Optional SPICE source", false)
                            .build());
        agentsdk::mcp::tool setTextTool;
        setTextTool.name = "schematic_set_text";
        setTextTool.description =
            "Replace the source of an existing text SPICE schematic, or convert an empty drawn cell.";
        setTextTool.input_schema =
            R"({"type":"object","properties":{"schematic":{"type":["string","integer"],)"
            R"("description":"Schematic name or id"},"cell":{"type":["string","integer"],)"
            R"("description":"Legacy alias for schematic"},"text":{"type":"string","description":"SPICE source"},)"
            R"("convert":{"type":"boolean","description":"Convert an empty drawn cell to a text cell"}},)"
            R"("anyOf":[{"required":["schematic"]},{"required":["cell"]}],"required":["text"]})";
        tools.push_back(std::move(setTextTool));
        tools.push_back(
            tool_builder("schematic_delete")
                .with_description("Delete a schematic; blocked when simulations or hierarchical symbols use it.")
                .with_string_param("schematic", "Schematic name or id")
                .build());
        agentsdk::mcp::tool symbolTool;
        symbolTool.name = "schematic_add_symbol";
        symbolTool.description = "Add a symbol definition, optionally with its pins, to a schematic cell.";
        symbolTool.input_schema =
            R"({"type":"object","properties":{"cell":{"type":"string","description":"Schematic name or id"},)"
            R"("name":{"type":"string","description":"Symbol name"},"artwork":{"type":"string"},)"
            R"("source_cell":{"type":"string"},"pins":{"type":"array","items":{"type":"object","properties":{)"
            R"("name":{"type":"string"},"x":{"type":"integer"},"y":{"type":"integer"},)"
            R"("direction":{"type":"string","enum":["left","right","up","down"]}},"required":["name"]}}},)"
            R"("required":["cell","name"]})";
        tools.push_back(std::move(symbolTool));
        tools.push_back(tool_builder("schematic_add_pin")
                            .with_description("Add one pin to an existing schematic symbol.")
                            .with_string_param("cell", "Schematic name or id")
                            .with_string_param("symbol", "Symbol id or name", false)
                            .with_integer_param("symbol_id", "Symbol id", false)
                            .with_string_param("name", "Pin name")
                            .with_integer_param("x", "X coordinate", false)
                            .with_integer_param("y", "Y coordinate", false)
                            .with_string_param("direction", "Optional left/right/up/down", false)
                            .build());
        agentsdk::mcp::tool netTool;
        netTool.name = "schematic_add_net";
        netTool.description = "Connect two or more pins into a named schematic net.";
        netTool.input_schema =
            R"({"type":"object","properties":{"cell":{"type":"string","description":"Schematic name or id"},)"
            R"("name":{"type":"string"},"pins":{"type":"array","minItems":2,"items":{"oneOf":)"
            R"([{"type":"string","description":"Pin id or instance.pin/symbol.pin name"},{"type":"integer"}]}},)"
            R"("x":{"type":"integer"},"y":{"type":"integer"}},"required":["cell","pins"]})";
        tools.push_back(std::move(netTool));
        tools.push_back(tool_builder("schematic_netlist")
                            .with_description("Return the SPICE netlist generated from a schematic cell.")
                            .with_string_param("schematic", "Schematic name or id")
                            .build());
        tools.push_back(tool_builder("schematic_add_instance")
                            .with_description("Place a symbol instance in a schematic.")
                            .with_string_param("cell", "Schematic name or id")
                            .with_integer_param("symbol_id", "Symbol id", false)
                            .with_string_param("symbol", "Symbol id or name", false)
                            .with_string_param("name", "Instance name")
                            .with_integer_param("x", "X coordinate")
                            .with_integer_param("y", "Y coordinate")
                            .build());
        tools.push_back(tool_builder("layout_create")
                            .with_description("Create a static layout cell.")
                            .with_string_param("name", "Layout name")
                            .with_string_param("pdk", "Optional PDK identifier", false)
                            .build());
        tools.push_back(tool_builder("layout_create_from_schematic")
                            .with_description("Create a layout from a schematic.")
                            .with_string_param("schematic", "Schematic name or id")
                            .with_string_param("name", "Optional layout name", false)
                            .build());
        tools.push_back(tool_builder("layout_delete")
                            .with_description("Delete a static or procedural layout; blocked when instances use it.")
                            .with_string_param("layout", "Layout or procedural layout name or id")
                            .build());
        tools.push_back(tool_builder("layout_add_layer")
                            .with_description("Add a layout layer.")
                            .with_string_param("name", "Layer name")
                            .with_integer_param("number", "Layer number")
                            .build());
        tools.push_back(tool_builder("layout_add_rectangle")
                            .with_description("Add a rectangle to a layout cell.")
                            .with_string_param("cell", "Layout cell name or id")
                            .with_string_param("layer", "Layer name or id")
                            .with_integer_param("x", "X coordinate")
                            .with_integer_param("y", "Y coordinate")
                            .with_integer_param("width", "Width")
                            .with_integer_param("height", "Height")
                            .build());
        tools.push_back(tool_builder("layout_add_text")
                            .with_description("Add text to a layout cell on a layer.")
                            .with_string_param("cell", "Layout cell name or id")
                            .with_string_param("layer", "Layer name or id")
                            .with_integer_param("x", "X coordinate")
                            .with_integer_param("y", "Y coordinate")
                            .with_string_param("text", "Text to place")
                            .with_integer_param("height", "Optional text height in database units", false)
                            .build());
        tools.push_back(tool_builder("layout_add_port")
                            .with_description("Add a pin/port to a layout cell.")
                            .with_string_param("cell", "Layout cell name or id")
                            .with_string_param("name", "Port name")
                            .with_string_param("layer", "Layer name or id")
                            .with_integer_param("x", "X coordinate")
                            .with_integer_param("y", "Y coordinate")
                            .with_integer_param("width", "Width")
                            .with_integer_param("height", "Height")
                            .with_string_param("direction", "input, output, inout, power, or ground", false)
                            .with_string_param("side", "left, right, top, or bottom", false)
                            .with_string_param("net", "Optional layout net name or id", false)
                            .with_string_param("label", "Optional display label", false)
                            .with_boolean_param("visible", "Show the pin", false)
                            .with_boolean_param("locked", "Prevent editing the pin", false)
                            .build());
        tools.push_back(tool_builder("layout_list_ports")
                            .with_description("List the pins/ports of a layout cell.")
                            .with_string_param("cell", "Layout cell name or id")
                            .build());
        tools.push_back(tool_builder("layout_update_port")
                            .with_description("Update a layout pin/port.")
                            .with_string_param("cell", "Layout cell name or id")
                            .with_string_param("port", "Port name or id")
                            .with_string_param("name", "New port name", false)
                            .with_string_param("label", "New display label", false)
                            .with_string_param("direction", "input, output, inout, power, or ground", false)
                            .with_string_param("side", "left, right, top, or bottom", false)
                            .with_string_param("net", "Layout net name or id, or null", false)
                            .with_integer_param("x", "X coordinate", false)
                            .with_integer_param("y", "Y coordinate", false)
                            .with_integer_param("width", "Width", false)
                            .with_integer_param("height", "Height", false)
                            .with_boolean_param("visible", "Show the pin", false)
                            .with_boolean_param("locked", "Prevent editing the pin", false)
                            .build());
        tools.push_back(tool_builder("layout_remove_port")
                            .with_description("Remove a layout pin/port.")
                            .with_string_param("cell", "Layout cell name or id")
                            .with_string_param("port", "Port name or id")
                            .build());
        tools.push_back(tool_builder("layout_add_net")
                            .with_description("Create a layout net.")
                            .with_string_param("cell", "Layout cell name or id")
                            .with_string_param("name", "Net name")
                            .build());
        tools.push_back(tool_builder("layout_assign_port_net")
                            .with_description("Assign or clear the net of a layout pin/port.")
                            .with_string_param("cell", "Layout cell name or id")
                            .with_string_param("port", "Port name or id")
                            .with_string_param("net", "Layout net name or id, or null", false)
                            .build());
        tools.push_back(tool_builder("layout_add_instance")
                            .with_description("Place a layout cell instance.")
                            .with_string_param("parent", "Parent layout name or id")
                            .with_string_param("child", "Child layout name or id")
                            .with_string_param("name", "Instance name")
                            .with_integer_param("x", "X coordinate")
                            .with_integer_param("y", "Y coordinate")
                            .with_string_param("orientation", "Optional orientation (r0, r90, ...)", false)
                            .build());
        tools.push_back(
            tool_builder("lvs_run")
                .with_description("Compare a layout against its schematic and report mismatches.")
                .with_string_param("layout", "Layout cell name or id")
                .with_string_param("schematic", "Optional schematic name or id; inferred when omitted", false)
                .build());

        agentsdk::mcp::tool polygon;
        polygon.name = "layout_add_polygon";
        polygon.description = "Add a polygon to a layout cell.";
        polygon.input_schema =
            R"({"type":"object","properties":{"cell":{"type":"string"},"layer":{"type":"string"},)"
            R"("points":{"type":"array","items":{"type":"array","items":{"type":"integer"},"minItems":2,"maxItems":2},)"
            R"("minItems":3}},"required":["cell","layer","points"]})";
        tools.push_back(std::move(polygon));
        return tools;
    }

    agentsdk::mcp::tool_result callTool(const std::string &name, const std::string &argumentsText) {
        try {
            return jsonResult(call(name, parseArguments(argumentsText)));
        } catch (const ToolError &error) {
            return jsonResult({{"error", error.what()}}, true);
        } catch (const ice::ProjectError &error) {
            return jsonResult({{"error", error.what()}, {"code", error.code()}}, true);
        } catch (const std::exception &error) {
            return jsonResult({{"error", error.what()}}, true);
        }
    }

    std::vector<agentsdk::mcp::resource> listResources() const {
        std::vector<agentsdk::mcp::resource> resources;
        resources.push_back({"ice://project/summary", "Project summary", std::nullopt,
                             "JSON summary of the open project", "application/json", std::nullopt, std::nullopt});
        resources.push_back({"ice://project/json", "Project JSON", std::nullopt, "Canonical serialized project file",
                             "application/json", std::nullopt, std::nullopt});
        resources.push_back({"ice://project/validation", "Project validation", std::nullopt,
                             "Validation diagnostics for the open project", "application/json", std::nullopt,
                             std::nullopt});
        resources.push_back({"ice://project/simulation-scripts", "Simulation scripts", std::nullopt,
                             "Project-level simulation script metadata and Python sources", "application/json",
                             std::nullopt, std::nullopt});
        return resources;
    }

    agentsdk::result<agentsdk::mcp::resource_read_result, agentsdk::mcp::mcp_error>
    readResource(const std::string &uri) {
        using namespace agentsdk::mcp;
        if (!session_)
            return mcp_error{RESOURCE_NOT_FOUND, "No project is open", std::nullopt};
        std::string text;
        if (uri == "ice://project/summary")
            text = summary().dump(2);
        else if (uri == "ice://project/json")
            text = session_->project().serialize();
        else if (uri == "ice://project/validation")
            text = diagnostics().dump(2);
        else if (uri == "ice://project/simulation-scripts" || uri == "ice://project/reports") {
            json reports = json::array();
            for (const auto &[id, report] : session_->project().reports) {
                json entry = {{"id", id}, {"name", report.name}, {"path", report.path}};
                try {
                    entry["source"] = ice::readProjectReportSource(projectRootPath(), report);
                } catch (const ice::ProjectError &error) {
                    entry["source_error"] = error.what();
                }
                reports.push_back(std::move(entry));
            }
            text = reports.dump(2);
        } else
            return mcp_error{RESOURCE_NOT_FOUND, "Resource not found: " + uri, std::nullopt};
        resource_read_result result;
        resource_contents contents;
        contents.uri = uri;
        contents.mime_type = "application/json";
        contents.text = std::move(text);
        result.contents.push_back(std::move(contents));
        return result;
    }
#endif

    std::unique_ptr<ice::ProjectSession> session_;
    std::filesystem::path projectPath_;
    std::filesystem::path executablePath_;
    std::string startupWarning_;
    std::filesystem::file_time_type projectStamp_ = std::filesystem::file_time_type::min();
};

} // namespace

int main(int argc, char **argv) {
    std::optional<std::filesystem::path> projectPath;
    bool recover = false;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--version" || argument == "version") {
            auto executablePath = ice::currentExecutablePath();
            if (executablePath.empty())
                executablePath = std::filesystem::absolute(argv[0]);
            auto version = buildJson(executablePath);
            for (const auto *feature :
                 {"mcp", "schematic_create_text", "schematic_add_text", "simulation_measure", "pdk_info"})
                version["features"].push_back(feature);
            std::cout << version.dump(2) << "\n";
            return 0;
        }
        if (argument == "--help" || argument == "-h") {
            std::cout << "ice-mcp " << ice::version() << "\n"
                      << "Usage: ice-mcp [--project <path>] [--recover] [--version]\n"
                      << "\n"
                      << "Runs an MCP server over stdio. Optionally opens a project at startup.\n"
                      << "Use --recover to quarantine malformed sidecars while opening the project.\n";
            return 0;
        }
        if (argument == "--project" && index + 1 < argc) {
            projectPath = std::filesystem::path(argv[++index]);
            continue;
        }
        if (argument == "--recover") {
            recover = true;
            continue;
        }
        std::cerr << "Unknown argument: " << argument << "\n";
        return 2;
    }

#ifdef ICE_HAVE_AGENTSDK
    try {
        auto executablePath = ice::currentExecutablePath();
        if (executablePath.empty())
            executablePath = std::filesystem::absolute(argv[0]);
        IceMcpServer server(std::move(projectPath), std::move(executablePath), recover);
        return server.run();
    } catch (const ice::ProjectError &error) {
        std::cerr << "error[" << error.code() << "]: " << error.what() << "\n";
        return 2;
    } catch (const std::exception &error) {
        std::cerr << "error[internal]: " << error.what() << "\n";
        return 3;
    }
#else
    std::cerr << "ice-mcp was built without agentsdk-cpp. Set AGENTSDK_CPP_DIR and rebuild.\n";
    return 1;
#endif
}
