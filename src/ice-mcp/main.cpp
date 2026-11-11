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

std::string lowerCopy(std::string text) {
    for (auto &character : text)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return text;
}

std::vector<std::filesystem::path> discoverProjectCandidates() {
    std::vector<std::filesystem::path> candidates;
    if (const auto *configured = std::getenv("ICE_PROJECT"); configured && *configured) {
        candidates.emplace_back(configured);
        return candidates;
    }
    std::error_code status;
    auto directory = std::filesystem::current_path(status);
    if (status)
        return candidates;
    for (int depth = 0; depth < 4 && !directory.empty(); ++depth) {
        const auto candidate = directory / "project.icepro";
        if (std::filesystem::is_regular_file(candidate, status) && !status)
            candidates.push_back(candidate);
        const auto parent = directory.parent_path();
        if (parent == directory)
            break;
        directory = parent;
    }
    return candidates;
}

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

const char *hdlPortDirectionName(ice::HdlPortDirection direction) {
    switch (direction) {
    case ice::HdlPortDirection::input:
        return "input";
    case ice::HdlPortDirection::output:
        return "output";
    case ice::HdlPortDirection::inout:
        return "inout";
    }
    return "input";
}

json hdlPortsJson(const ice::HdlModule &module) {
    json ports = json::array();
    for (const auto &port : module.ports) {
        ports.push_back({{"name", port.name},
                         {"direction", hdlPortDirectionName(port.direction)},
                         {"width", port.width},
                         {"width_known", port.widthKnown},
                         {"real", port.real}});
    }
    return ports;
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
                        {"direction", pinDirectionName(pin.direction)},
                        {"width", pin.width},
                        {"width_known", pin.widthKnown}});
    }
    return {{"id", symbol.id},
            {"name", symbol.name},
            {"source_cell", symbol.sourceCell},
            {"artwork", !symbol.artwork.empty()},
            {"pins", std::move(pins)}};
}

std::vector<ice::Parameter> parseParameters(const json *value, const char *name) {
    std::vector<ice::Parameter> parameters;
    if (!value || value->is_null())
        return parameters;
    if (value->is_object()) {
        for (auto it = value->begin(); it != value->end(); ++it) {
            if (it.value().is_string()) {
                parameters.push_back({0, it.key(), it.value().get<std::string>()});
            } else if (it.value().is_number() || it.value().is_boolean()) {
                parameters.push_back({0, it.key(), it.value().dump()});
            } else {
                throw ToolError(std::string("Parameter values must be strings, numbers, or booleans: ") + name + "." +
                                it.key());
            }
        }
        return parameters;
    }
    if (value->is_array()) {
        for (const auto &entry : *value) {
            if (!entry.is_object())
                throw ToolError(std::string("Parameter entries must be objects: ") + name);
            parameters.push_back({0, requiredString(entry, "name"), requiredStringAllowEmpty(entry, "value")});
        }
        return parameters;
    }
    throw ToolError(std::string("Parameters must be an object or array: ") + name);
}

json diagnosticJson(const ice::Diagnostic &diagnostic) {
    json item = {{"severity", diagnostic.severity == ice::Diagnostic::Severity::error ? "error" : "warning"},
                 {"code", diagnostic.code},
                 {"message", diagnostic.message}};
    if (diagnostic.object)
        item["object"] = *diagnostic.object;
    return item;
}

json componentPlacementJson(const ice::ComponentPlacement &placement) {
    json pins = json::array();
    for (const auto &pin : placement.pins) {
        pins.push_back({{"id", pin.id},
                        {"name", pin.name},
                        {"direction", pinDirectionName(pin.direction)},
                        {"width", pin.width},
                        {"width_known", pin.widthKnown}});
    }
    return {{"instance", placement.instance},
            {"symbol", placement.symbol},
            {"symbol_name", placement.symbolName},
            {"name", placement.instanceName},
            {"pins", std::move(pins)}};
}

json fileSymbolJson(const ice::FileSymbol &symbol) {
    json pins = json::array();
    for (const auto &pin : symbol.pins) {
        pins.push_back({{"name", pin.name},
                        {"direction", pinDirectionName(pin.direction)},
                        {"x", pin.x},
                        {"y", pin.y},
                        {"width", pin.width},
                        {"width_known", pin.widthKnown}});
    }
    json defaults = json::array();
    for (const auto &parameter : symbol.defaultParameters)
        defaults.push_back({{"name", parameter.name}, {"value", parameter.value}});
    return {{"name", symbol.name},
            {"category", symbol.category},
            {"pins", std::move(pins)},
            {"defaults", std::move(defaults)}};
}

json schematicJson(const ice::SchematicCell &cell) {
    json result = {{"id", cell.id},
                   {"name", cell.name},
                   {"pdk", cell.pdk},
                   {"text", cell.textSchematic},
                   {"systemverilog", cell.systemVerilogSchematic},
                   {"source", cell.sourceText()},
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

json helpJson(const std::string &requestedTopic) {
    const auto topic = lowerCopy(requestedTopic.empty() ? "overview" : requestedTopic);
    json topics = {
        {"overview",
         {{"summary", "Open or create a project, build a schematic, validate it, create a simulation, run it, "
                      "and inspect the stored traces."},
          {"steps",
           json::array({"project_open or project_create",
                        "schematic_create and schematic_add_component/schematic_add_hdl_instance",
                        "schematic_add_net or schematic_add_circuit", "schematic_validate", "simulation_create",
                        "simulation_run", "simulation_result/simulation_trace/simulation_measure"})}}},
        {"project",
         {{"tools", json::array({"project_create", "project_open", "project_summary", "project_validate",
                                 "project_recover", "project_repair", "project_undo", "project_redo"})},
          {"example", {{"tool", "project_open"}, {"arguments", json::object()}}}}},
        {"schematic",
         {{"tools", json::array({"schematic_create", "schematic_add_component", "schematic_list_symbols",
                                 "schematic_add_circuit", "schematic_add_net", "schematic_set_parameter",
                                 "schematic_validate", "schematic_info", "schematic_netlist"})},
          {"example",
           {{"tool", "schematic_add_component"},
            {"arguments",
             {{"cell", "top"}, {"type", "resistor"}, {"name", "R1"}, {"parameters", {{"resistance", "10k"}}}}}}}}},
        {"mixed-signal",
         {{"summary", "Place a SystemVerilog block with a generated symbol, wire its pins like any other device, "
                      "then let simulation_create auto-select the mixed simulator."},
          {"tools", json::array({"schematic_create_systemverilog", "schematic_hdl_ports", "schematic_add_hdl_instance",
                                 "schematic_set_parameter", "schematic_add_net", "schematic_validate_hierarchy",
                                 "simulation_create", "simulation_run"})},
          {"notes",
           json::array({"Mixed mode currently supports top-level scalar logic input/output ports.",
                        "For a PWM DAC, drive the analog RC filter from a logic PWM output; real ports are "
                        "rejected explicitly.",
                        "A requested signal such as v(U1.out) is invalid for a digital port; use U1.out instead."})}}},
        {"simulation",
         {{"tools", json::array({"simulation_create", "simulation_update", "simulation_run", "simulation_result",
                                 "simulation_signals", "simulation_trace", "simulation_measure",
                                 "simulation_graph_create", "simulation_graph_list", "simulation_graph_update"})},
          {"example",
           {{"tool", "simulation_create"},
            {"arguments", {{"name", "tran1"}, {"schematic", "top"}, {"template", "tran"}}}}}}},
        {"pdk",
         {{"tools", json::array({"pdk_info", "pdk_build", "ice_doctor"})},
          {"notes", json::array({"ICE applies the package PDK setup automatically based on the schematic PDK.",
                                 "Do not copy another PDK's .include/.lib lines into a text schematic.",
                                 "Use pdk_info recommended_setup for reference and pdk_build for OSDI models."})}}},
    };
    if (topic == "all" || topic == "topics") {
        json names = json::array();
        for (const auto &[name, _] : topics.items())
            names.push_back(name);
        return {{"topics", std::move(names)}, {"help", topics}};
    }
    const auto found = topics.find(topic);
    if (found == topics.end()) {
        json names = json::array();
        for (const auto &[name, _] : topics.items())
            names.push_back(name);
        return {{"error", "Unknown help topic: " + requestedTopic}, {"topics", std::move(names)}};
    }
    return {{"topic", topic}, {"help", *found}};
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
            {"ok", pythonAvailable && !runner.empty() && (ngspiceAvailable || ngspiceLibrary)}};
}

ice::ObjectId resolveLayoutCell(const ice::Project &project, const json &arguments, const char *key = "cell") {
    return lookupByIdOrName(project.layoutCells, requiredValue(arguments, key), "layout cell");
}

// Fills in the package default when a layout tool omits the stack. Invalid
// stack requests fail before the project is modified.
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
            throw ToolError("No project is open. Call project_open (path optional; cwd/parent search) or "
                            "project_create. Use ice_help(topic='project') for the workflow.");
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
        if (!session_) {
            json candidates = json::array();
            json suggested = json::array();
            for (const auto &candidate : discoverProjectCandidates()) {
                candidates.push_back(candidate.string());
                suggested.push_back({{"tool", "project_open"}, {"arguments", {{"path", candidate.string()}}}});
            }
            return {{"open", false},
                    {"cwd", std::filesystem::current_path().string()},
                    {"candidates", std::move(candidates)},
                    {"suggested_calls", std::move(suggested)}};
        }
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
                                            {"systemverilog", cell.systemVerilogSchematic},
                                            {"symbols", cell.symbols.size()},
                                            {"instances", cell.instances.size()},
                                            {"nets", cell.nets.size()}});
        }
        for (const auto &[id, layer] : project.layers)
            result["layers"].push_back({{"id", id},
                                        {"name", layer.name},
                                        {"number", layer.number},
                                        {"datatype", layer.datatype},
                                        {"color", layer.color},
                                        {"pattern", layer.pattern},
                                        {"visible", layer.visible}});
        for (const auto &[id, cell] : project.layoutCells) {
            result["layouts"].push_back({{"id", id},
                                         {"name", cell.name},
                                         {"pdk", cell.pdk},
                                         {"stack", cell.stack},
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
                                                    {"stack", cell.stack},
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
        if (name == "ice_version") {
            auto result = buildJson(executablePath_);
            result["features"].push_back("mcp");
            result["features"].push_back("schematic_create_text");
            result["features"].push_back("simulation_measure");
            result["features"].push_back("pdk_info");
            if (!startupWarning_.empty())
                result["startup_warning"] = startupWarning_;
            return result;
        }

        if (name == "ice_help") {
            auto result = helpJson(optionalString(arguments, "topic", "overview"));
            result["project_open"] = session_ != nullptr;
            if (session_)
                result["project_path"] = projectPath_.string();
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
            std::filesystem::path path;
            if (const auto value = arguments.find("path"); value != arguments.end() && !value->is_null()) {
                path = std::filesystem::path(requiredString(arguments, "path"));
            } else {
                const auto candidates = discoverProjectCandidates();
                if (candidates.empty())
                    throw ToolError("No project.icepro was found in the current directory or its parents. "
                                    "Pass path=... or call project_create.");
                if (candidates.size() > 1) {
                    std::string listed;
                    for (const auto &candidate : candidates) {
                        if (!listed.empty())
                            listed += ", ";
                        listed += candidate.string();
                    }
                    throw ToolError("Several project files were found; pass an explicit path: " + listed);
                }
                path = candidates.front();
            }
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
            result["guidance_version"] = 3;
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
            const auto &schematicCell = session.project().schematicCells.at(schematic);
            const bool mixedSignal =
                !schematicCell.systemVerilogSchematic && ice::schematicContainsHdl(session.project(), schematic);
            ice::SimulationItem simulation;
            simulation.name = requiredString(arguments, "name");
            simulation.schematic = schematic;
            const auto templateName = lowerCopy(optionalString(arguments, "template"));
            if (!templateName.empty()) {
                if (templateName == "tran" || templateName == "transient") {
                    simulation.analysis = "transient";
                    simulation.stop = 1.0e-3;
                    simulation.step = 1.0e-6;
                } else if (templateName == "mixed" || templateName == "pwm_dac" || templateName == "pwm-dac") {
                    if (!mixedSignal)
                        throw ToolError("Mixed/PWM template requires a drawn schematic with SystemVerilog blocks");
                    simulation.simulator = "mixed";
                    simulation.analysis = "transient";
                    simulation.stop = 1.0e-6;
                    simulation.step = 1.0e-9;
                } else if (templateName == "digital") {
                    if (!schematicCell.systemVerilogSchematic)
                        throw ToolError("Digital template requires a SystemVerilog source cell");
                    simulation.simulator = "iverilog";
                    simulation.analysis = "digital";
                } else {
                    throw ToolError("Unknown simulation template: " + templateName);
                }
            }
            if (schematicCell.systemVerilogSchematic) {
                simulation.simulator = "iverilog";
                simulation.analysis = "digital";
            } else if (mixedSignal) {
                simulation.simulator = "mixed";
                simulation.analysis = "transient";
                simulation.stop = 1.0e-6;
                simulation.step = 1.0e-9;
            }
            simulation.analysis = optionalString(arguments, "analysis", simulation.analysis);
            if (const auto simulator = optionalString(arguments, "simulator"); !simulator.empty())
                simulation.simulator = simulator;
            if (simulation.simulator == "mixed" && !mixedSignal)
                throw ToolError("simulator='mixed' requires a drawn schematic with SystemVerilog blocks");
            if (simulation.simulator == "iverilog" && !schematicCell.systemVerilogSchematic)
                throw ToolError("simulator='iverilog' requires a SystemVerilog source cell");
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
                    {"simulator", simulation.simulator},
                    {"analysis", simulation.analysis},
                    {"signals", simulation.captureSignals},
                    {"graph", graphId}};
        }
        if (name == "simulation_update") {
            const auto id = resolveSimulation(session.project(), requiredValue(arguments, "simulation"));
            auto updated = session.project().simulations.at(id);
            if (arguments.contains("analysis"))
                updated.analysis = requiredString(arguments, "analysis");
            if (arguments.contains("simulator"))
                updated.simulator = requiredString(arguments, "simulator");
            const auto &updatedSchematic = session.project().schematicCells.at(updated.schematic);
            const bool updatedMixed = !updatedSchematic.systemVerilogSchematic &&
                                      ice::schematicContainsHdl(session.project(), updated.schematic);
            if (updated.simulator == "mixed" && !updatedMixed)
                throw ToolError("simulator='mixed' requires a drawn schematic with SystemVerilog blocks");
            if (updated.simulator == "iverilog" && !updatedSchematic.systemVerilogSchematic)
                throw ToolError("simulator='iverilog' requires a SystemVerilog source cell");
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
                    {"simulator", updated.simulator},
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
            request.workingDirectory = projectRootPath();
            const auto appendDirectives = [&] {
                std::istringstream directives(simulation.directives);
                std::string directive;
                while (std::getline(directives, directive)) {
                    const auto trimmed = directive.find_first_not_of(" \t\r\n") == std::string::npos
                                             ? std::string{}
                                             : directive.substr(directive.find_first_not_of(" \t\r\n"));
                    if (!trimmed.empty())
                        request.netlist.push_back(trimmed);
                }
            };
            const bool mixedSignal = !schematic->second.systemVerilogSchematic &&
                                     ice::schematicContainsHdl(session.project(), simulation.schematic);
            if (mixedSignal) {
                std::string error;
                if (!ice::prepareMixedSignalRequest(request, session.project(), simulation.schematic, &error))
                    throw ToolError("Mixed-signal plan failed: " + error);
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
                request.netlist = ice::buildSimulationNetlist(session.project(), simulation.schematic);
                ice::applyPdkSimulationSetup(schematic->second.pdk, request);
            }
            request.probeSignals = splitCommaList(simulation.captureSignals);
            const auto timeout = std::chrono::milliseconds(
                std::max<std::int64_t>(1, optionalInteger(arguments, "timeout_ms").value_or(60000)));
            ice::SimulationRunner runner;
            const auto result = runner.run(request, timeout);
            if (result.state == ice::JobState::succeeded && !result.traces.empty()) {
                auto updated = simulation;
                if (ice::populateSimulationSignalsFromTraces(updated, result.traces)) {
                    session.execute("mcp populate simulation signals", [&](ice::Project &project) {
                        auto it = project.simulations.find(id);
                        if (it != project.simulations.end())
                            ice::populateSimulationSignalsFromTraces(it->second, result.traces);
                    });
                }
            }
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
            json pdkSetup = {
                {"pdk", schematic->second.pdk}, {"lines", json::array()}, {"init_commands", json::array()}};
            try {
                const auto setup = ice::pdkSimulationSetupFor(schematic->second.pdk);
                pdkSetup["lines"] = setup.netlistLines;
                pdkSetup["init_commands"] = setup.initCommands;
            } catch (const ice::ProjectError &error) {
                pdkSetup["warning"] = error.what();
            }
            summary["pdk_setup"] = std::move(pdkSetup);

            json analogSignals = json::array();
            json digitalSignals = json::array();
            for (const auto &trace : result.traces) {
                const auto name = lowerCopy(trace.name);
                if (name.find('.') != std::string::npos && !name.starts_with("v("))
                    digitalSignals.push_back(trace.name);
                else
                    analogSignals.push_back(trace.name);
            }
            summary["analog_signals"] = std::move(analogSignals);
            summary["digital_signals"] = std::move(digitalSignals);
            json graphs = json::array();
            if (const auto updated = session.project().simulations.find(id);
                updated != session.project().simulations.end()) {
                for (const auto &graph : updated->second.graphs)
                    graphs.push_back(graphJson(graph));
            }
            summary["graphs"] = std::move(graphs);

            json hints = json::array();
            json suggested = json::array();
            if (result.state != ice::JobState::succeeded) {
                summary["hint"] =
                    "Inspect netlist, trace_details, and available_signals; run ice_doctor to check ngspice/PDK paths.";
                suggested.push_back(
                    {{"tool", "schematic_validate"}, {"arguments", {{"schematic", schematic->second.name}}}});
                suggested.push_back({{"tool", "ice_doctor"}, {"arguments", {{"schematic", schematic->second.name}}}});
                suggested.push_back({{"tool", "simulation_signals"}, {"arguments", {{"simulation", simulation.name}}}});
            } else {
                suggested.push_back({{"tool", "simulation_trace"}, {"arguments", {{"simulation", simulation.name}}}});
                suggested.push_back(
                    {{"tool", "simulation_graph_list"}, {"arguments", {{"simulation", simulation.name}}}});
            }
            if (result.analysis == "mixed") {
                const auto digitalNames = summary["digital_signals"];
                for (const auto &probe : request.probeSignals) {
                    const auto lower = lowerCopy(probe);
                    if (!lower.starts_with("v(") || !lower.ends_with(")") || lower.find('.') == std::string::npos)
                        continue;
                    const auto inner = probe.substr(2, probe.size() - 3);
                    for (const auto &digital : digitalNames) {
                        if (lowerCopy(digital.get<std::string>()) == lowerCopy(inner)) {
                            hints.push_back("'" + probe + "' is an analog probe syntax for a digital trace. Use '" +
                                            inner + "' without v(...) in graphs or simulation_trace.");
                            break;
                        }
                    }
                }
            }
            summary["hints"] = std::move(hints);
            summary["suggested_calls"] = std::move(suggested);
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
        if (name == "simulation_trace") {
            const auto id = resolveSimulation(session.project(), requiredValue(arguments, "simulation"));
            const auto stored = ice::latestSimulationRun(projectRootPath(), id);
            if (!stored)
                throw ToolError("No stored simulation run was found for this simulation.");
            if (stored->result.time.empty() || stored->result.traces.empty())
                throw ToolError("The stored simulation has no traces to sample.");
            std::vector<std::string> wanted;
            if (const auto value = arguments.find("signals"); value != arguments.end() && !value->is_null())
                wanted = parseSignalListValue(*value, "signals");
            const auto maxPoints =
                std::clamp<std::int64_t>(optionalInteger(arguments, "max_points").value_or(2000), 2, 100000);
            std::vector<const ice::SimulationTrace *> selected;
            for (const auto &trace : stored->result.traces) {
                if (wanted.empty())
                    continue;
                const auto traceName = lowerCopy(trace.name);
                for (const auto &name : wanted) {
                    const auto candidate = lowerCopy(name);
                    if (traceName == candidate || traceName.ends_with("." + candidate)) {
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
                throw ToolError("No stored trace matched the requested signals. Available signals: " +
                                resultJson(stored->result)["available_signals"].dump());
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
            json signals = json::array();
            for (const auto *trace : selected) {
                json values = json::array();
                for (const auto index : indices) {
                    if (index < trace->values.size())
                        values.push_back(trace->values[index]);
                    else
                        values.push_back(nullptr);
                }
                const auto name = lowerCopy(trace->name);
                const bool digital = name.find('.') != std::string::npos && !name.starts_with("v(");
                signals.push_back({{"name", trace->name},
                                   {"signal_type", digital ? "digital" : "analog"},
                                   {"values", std::move(values)}});
            }
            return {{"simulation", stored->simulationName},
                    {"schematic", stored->schematicName},
                    {"run_id", stored->runId},
                    {"time_points", totalPoints},
                    {"returned_points", indices.size()},
                    {"decimated", decimated},
                    {"time", std::move(time)},
                    {"signals", std::move(signals)}};
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

        if (name == "simulation_script_create") {
            const auto scriptName = requiredString(arguments, "name");
            const auto source = optionalString(arguments, "source");
            const auto root = projectRootPath();
            ice::ObjectId id = 0;
            session.execute("mcp simulation-script-create", [&](ice::Project &project) {
                id = ice::createProjectReport(project, root, scriptName, source);
            });
            const auto &script = session.project().reports.at(id);
            return {{"id", id},
                    {"name", script.name},
                    {"path", script.path},
                    {"source", ice::readProjectReportSource(root, script)}};
        }
        if (name == "simulation_script_list") {
            json scripts = json::array();
            for (const auto &[id, script] : session.project().reports)
                scripts.push_back({{"id", id}, {"name", script.name}, {"path", script.path}});
            return scripts;
        }
        if (name == "simulation_script_read") {
            const auto id =
                lookupByIdOrName(session.project().reports, requiredValue(arguments, "script"), "simulation script");
            const auto &script = session.project().reports.at(id);
            return {{"id", id},
                    {"name", script.name},
                    {"path", script.path},
                    {"source", ice::readProjectReportSource(projectRootPath(), script)}};
        }
        if (name == "simulation_script_write") {
            const auto id =
                lookupByIdOrName(session.project().reports, requiredValue(arguments, "script"), "simulation script");
            const auto &script = session.project().reports.at(id);
            const auto source = requiredStringAllowEmpty(arguments, "source");
            ice::writeProjectReportSource(projectRootPath(), script, source);
            return {{"id", id}, {"name", script.name}, {"path", script.path}, {"bytes", source.size()}};
        }
        if (name == "simulation_script_run") {
            const auto id =
                lookupByIdOrName(session.project().reports, requiredValue(arguments, "script"), "simulation script");
            const auto &script = session.project().reports.at(id);
            const auto timeoutValue = optionalInteger(arguments, "timeout_ms").value_or(30000);
            const auto timeout = std::chrono::milliseconds(std::max<std::int64_t>(1, timeoutValue));
            const auto result = ice::runProjectReport(projectRootPath(), script, executablePath_, timeout);
            json graphs = json::array();
            if (!result.graphsText.empty()) {
                const auto document = json::parse(result.graphsText, nullptr, false);
                if (!document.is_discarded() && document.contains("graphs"))
                    graphs = document.at("graphs");
            }
            return {{"id", id},
                    {"name", script.name},
                    {"path", script.path},
                    {"ok", !result.timedOut && result.exitCode == 0},
                    {"timed_out", result.timedOut},
                    {"exit_code", result.exitCode},
                    {"stdout", result.stdoutText},
                    {"stderr", result.stderrText},
                    {"graphs", std::move(graphs)}};
        }
        if (name == "simulation_script_delete") {
            const auto id =
                lookupByIdOrName(session.project().reports, requiredValue(arguments, "script"), "simulation script");
            const auto script = session.project().reports.at(id);
            const bool purge = optionalBoolean(arguments, "purge", false);
            session.execute("mcp simulation-script-delete", [&](ice::Project &project) { project.reports.erase(id); });
            if (purge) {
                std::error_code status;
                std::filesystem::remove(projectReportPath(projectRootPath(), script), status);
                if (status)
                    throw ToolError("Unable to purge script file: " + status.message());
            }
            return {{"id", id}, {"name", script.name}, {"path", script.path}, {"deleted", true}, {"purged", purge}};
        }

        if (name == "schematic_delete") {
            const auto id = resolveSchematic(session.project(), arguments, "schematic");
            const auto plan = ice::planSchematicDeletion(session.project(), id);
            const bool dryRun = optionalBoolean(arguments, "dry_run", false);
            const bool cascade = optionalBoolean(arguments, "cascade", false);
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
            json suggested = json::array();
            for (const auto &simulation : plan.simulations)
                suggested.push_back({{"tool", "simulation_delete"}, {"arguments", {{"simulation", simulation.name}}}});
            if (dryRun)
                return {{"id", id},
                        {"name", plan.name},
                        {"deleted", false},
                        {"dry_run", true},
                        {"blocked", plan.blocked()},
                        {"simulations", std::move(simulations)},
                        {"symbols", std::move(symbols)},
                        {"suggested_calls", std::move(suggested)}};
            if (plan.blocked() && !cascade)
                return {{"id", id},
                        {"name", plan.name},
                        {"deleted", false},
                        {"blocked", true},
                        {"simulations", std::move(simulations)},
                        {"symbols", std::move(symbols)},
                        {"suggested_calls", std::move(suggested)}};
            session.execute("mcp schematic-delete", [&](ice::Project &project) {
                if (cascade)
                    ice::removeSchematicCascade(project, id);
                else
                    ice::removeSchematicCell(project, id);
            });
            return {{"id", id}, {"name", plan.name}, {"deleted", true}, {"cascaded", cascade}};
        }

        if (name == "schematic_create") {
            ice::ObjectId id = 0;
            const auto pdk = optionalString(arguments, "pdk", ice::genericPdkId);
            const auto cellName = requiredString(arguments, "name");
            const auto kind = optionalString(arguments, "kind", "drawn");
            auto text = optionalString(arguments, "text");
            if (kind != "drawn" && kind != "text" && kind != "systemverilog" && kind != "sv")
                throw ToolError("schematic_create kind must be 'drawn', 'text', or 'systemverilog'");
            if (kind == "systemverilog" || kind == "sv") {
                if (text.empty())
                    text = "// SystemVerilog block: " + cellName + "\nmodule " + verilogIdentifier(cellName) +
                           " (\n    input  logic clk,\n    input  logic rst,\n    output logic out\n);\n"
                           "    assign out = 1'b0;\nendmodule\n";
                session.execute("mcp schematic-create-systemverilog", [&](ice::Project &project) {
                    id = ice::createSystemVerilogCell(project, cellName, pdk, text);
                });
                return {{"id", id}, {"name", cellName}, {"pdk", pdk}, {"kind", "systemverilog"}, {"text", text}};
            }
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
        if (name == "schematic_create_systemverilog" || name == "schematic_add_systemverilog") {
            ice::ObjectId id = 0;
            const auto pdk = optionalString(arguments, "pdk", ice::genericPdkId);
            const auto cellName = requiredString(arguments, "name");
            auto text = optionalString(arguments, "text");
            if (text.empty())
                text = "// SystemVerilog block: " + cellName + "\nmodule " + verilogIdentifier(cellName) +
                       " (\n    input  logic clk,\n    input  logic rst,\n    output logic out\n);\n"
                       "    assign out = 1'b0;\nendmodule\n";
            session.execute("mcp schematic-create-systemverilog", [&](ice::Project &project) {
                id = ice::createSystemVerilogCell(project, cellName, pdk, text);
            });
            return {{"id", id}, {"name", cellName}, {"pdk", pdk}, {"kind", "systemverilog"}, {"text", text}};
        }
        if (name == "schematic_create_text") {
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
            const auto id = resolveSchematic(session.project(), arguments, "schematic");
            const auto &cell = session.project().schematicCells.at(id);
            const bool systemVerilogCell = cell.systemVerilogSchematic;
            const auto kind = optionalString(arguments, "kind", systemVerilogCell ? "systemverilog" : "text");
            const bool systemVerilog = kind == "systemverilog" || kind == "sv";
            if (kind != "text" && kind != "spice" && !systemVerilog)
                throw ToolError("schematic_set_text kind must be 'text' or 'systemverilog'");
            if (cell.isTextSource() && systemVerilogCell != systemVerilog) {
                throw ToolError("Schematic '" + cell.name + "' is a " +
                                (systemVerilogCell ? "SystemVerilog" : "text SPICE") +
                                " cell; use the matching kind instead of converting between source languages");
            }
            const bool wasSource = cell.isTextSource();
            const bool convert = optionalBoolean(arguments, "convert", false);
            if (!cell.isTextSource()) {
                const bool empty =
                    cell.symbols.empty() && cell.instances.empty() && cell.nets.empty() && cell.parameters.empty();
                if (!convert || !empty) {
                    throw ToolError("Schematic '" + cell.name +
                                    "' is a drawn schematic, not a source cell. Use schematic_create with "
                                    "kind='text' or kind='systemverilog' for a new source cell. To convert an empty "
                                    "drawn cell, pass convert=true. Do not edit sch/*.icesch by hand.");
                }
            }
            const auto text = requiredStringAllowEmpty(arguments, "text");
            session.execute("mcp schematic-set-text", [&](ice::Project &project) {
                auto &target = project.schematicCells.at(id);
                target.textSchematic = !systemVerilog;
                target.systemVerilogSchematic = systemVerilog;
                if (systemVerilog) {
                    target.systemVerilogText = text;
                    target.spiceText.clear();
                } else {
                    target.spiceText = text;
                    target.systemVerilogText.clear();
                }
            });
            return {{"id", id},
                    {"name", cell.name},
                    {"kind", systemVerilog ? "systemverilog" : "text"},
                    {"text", text},
                    {"converted", !wasSource}};
        }
        if (name == "schematic_list") {
            json result = json::array();
            for (const auto &[id, cell] : session.project().schematicCells) {
                result.push_back({{"id", id},
                                  {"name", cell.name},
                                  {"pdk", cell.pdk},
                                  {"text", cell.textSchematic},
                                  {"systemverilog", cell.systemVerilogSchematic},
                                  {"symbols", cell.symbols.size()},
                                  {"instances", cell.instances.size()},
                                  {"nets", cell.nets.size()}});
            }
            return result;
        }
        if (name == "schematic_info") {
            const auto id = resolveSchematicArgument(session.project(), arguments, {"schematic", "cell"});
            const auto &cell = session.project().schematicCells.at(id);
            auto result = schematicJson(cell);
            try {
                const auto setup = ice::pdkSimulationSetupFor(cell.pdk);
                result["pdk_setup"] = {
                    {"pdk", cell.pdk}, {"lines", setup.netlistLines}, {"init_commands", setup.initCommands}};
            } catch (const ice::ProjectError &error) {
                result["pdk_setup"] = {{"pdk", cell.pdk}, {"warning", error.what()}};
            }
            return result;
        }
        if (name == "schematic_hdl_ports") {
            const auto id = resolveSchematicArgument(session.project(), arguments, {"schematic", "cell"});
            const auto &cell = session.project().schematicCells.at(id);
            if (!cell.systemVerilogSchematic)
                throw ToolError("Schematic '" + cell.name + "' is not a SystemVerilog source cell");
            const auto parsed = ice::parseSystemVerilogModule(cell);
            if (!parsed.ok)
                throw ToolError("SystemVerilog parse failed: " + parsed.error);
            return {{"id", id},
                    {"schematic", cell.name},
                    {"module", parsed.module.name},
                    {"ports", hdlPortsJson(parsed.module)},
                    {"warnings", parsed.warnings}};
        }
        if (name == "schematic_add_component") {
            const auto cellId = resolveSchematicArgument(session.project(), arguments, {"cell", "schematic"});
            ice::ComponentRequest request;
            request.type = optionalString(arguments, "type");
            request.symbol = optionalString(arguments, "symbol");
            request.name = optionalString(arguments, "name");
            request.x = optionalInteger(arguments, "x").value_or(0);
            request.y = optionalInteger(arguments, "y").value_or(0);
            const auto paramsIt = arguments.find("parameters");
            request.parameters = parseParameters(paramsIt == arguments.end() ? nullptr : &*paramsIt, "parameters");
            ice::ComponentPlacement placement;
            session.execute("mcp schematic-add-component", [&](ice::Project &project) {
                placement = ice::addSchematicComponent(project, cellId, request);
            });
            auto result = componentPlacementJson(placement);
            result["cell"] = cellId;
            return result;
        }
        if (name == "schematic_list_symbols") {
            const auto pdk = optionalString(arguments, "pdk", ice::genericPdkId);
            const auto query = lowerCopy(optionalString(arguments, "query"));
            try {
                auto symbols = ice::availableSymbols(pdk);
                json result = json::array();
                for (const auto &symbol : symbols) {
                    if (!query.empty() && lowerCopy(symbol.name).find(query) == std::string::npos &&
                        lowerCopy(symbol.category).find(query) == std::string::npos)
                        continue;
                    result.push_back(fileSymbolJson(symbol));
                }
                return {{"pdk", pdk}, {"symbols", std::move(result)}};
            } catch (const ice::ProjectError &error) {
                throw ToolError(error.what());
            }
        }
        if (name == "schematic_add_circuit") {
            const auto cellId = resolveSchematicArgument(session.project(), arguments, {"cell", "schematic"});
            const auto &components = requiredValue(arguments, "components");
            if (!components.is_array() || components.empty())
                throw ToolError("Argument 'components' must be a non-empty array.");
            ice::CircuitRequest request;
            for (const auto &entry : components) {
                if (!entry.is_object())
                    throw ToolError("Each component must be an object.");
                ice::ComponentRequest component;
                component.type = optionalString(entry, "type");
                component.symbol = optionalString(entry, "symbol");
                component.name = optionalString(entry, "name");
                component.x = optionalInteger(entry, "x").value_or(0);
                component.y = optionalInteger(entry, "y").value_or(0);
                const auto parametersIt = entry.find("parameters");
                component.parameters =
                    parseParameters(parametersIt == entry.end() ? nullptr : &*parametersIt, "component.parameters");
                request.components.push_back(std::move(component));
            }
            if (const auto nets = arguments.find("nets"); nets != arguments.end() && !nets->is_null()) {
                if (!nets->is_array())
                    throw ToolError("Argument 'nets' must be an array.");
                for (const auto &entry : *nets) {
                    if (!entry.is_object())
                        throw ToolError("Each net must be an object.");
                    ice::CircuitNetRequest net;
                    net.name = optionalString(entry, "name");
                    const auto &pins = requiredValue(entry, "pins");
                    if (!pins.is_array() || pins.size() < 2)
                        throw ToolError("Each net needs at least two pin references.");
                    for (const auto &pin : pins) {
                        if (pin.is_string()) {
                            net.pins.push_back(pin.get<std::string>());
                        } else if (pin.is_number_integer()) {
                            net.pins.push_back(std::to_string(pin.get<std::int64_t>()));
                        } else if (pin.is_number_unsigned()) {
                            net.pins.push_back(std::to_string(pin.get<std::uint64_t>()));
                        } else {
                            throw ToolError("Net pins must be strings or integer pin ids.");
                        }
                    }
                    net.x = optionalInteger(entry, "x").value_or(0);
                    net.y = optionalInteger(entry, "y").value_or(0);
                    request.nets.push_back(std::move(net));
                }
            }
            ice::CircuitPlacement placement;
            session.execute("mcp schematic-add-circuit", [&](ice::Project &project) {
                placement = ice::addSchematicCircuit(project, cellId, request);
            });
            json componentsJson = json::array();
            for (const auto &component : placement.components)
                componentsJson.push_back(componentPlacementJson(component));
            return {{"cell", cellId}, {"components", std::move(componentsJson)}, {"nets", placement.nets}};
        }
        if (name == "schematic_validate") {
            const auto cellId = resolveSchematicArgument(session.project(), arguments, {"cell", "schematic"});
            const auto diagnostics = ice::validateSchematic(session.project(), cellId);
            json items = json::array();
            bool ok = true;
            for (const auto &diagnostic : diagnostics) {
                auto item = diagnosticJson(diagnostic);
                if (diagnostic.severity == ice::Diagnostic::Severity::error)
                    ok = false;
                json suggestions = json::array();
                if (diagnostic.code.starts_with("schematic.pdk")) {
                    suggestions.push_back({{"tool", "pdk_info"},
                                           {"arguments", {{"pdk", session.project().schematicCells.at(cellId).pdk}}}});
                } else if (diagnostic.code.starts_with("schematic.symbol")) {
                    suggestions.push_back(
                        {{"tool", "schematic_info"},
                         {"arguments", {{"schematic", session.project().schematicCells.at(cellId).name}}}});
                } else if (diagnostic.code.starts_with("hdl.")) {
                    suggestions.push_back(
                        {{"tool", "schematic_validate_hierarchy"},
                         {"arguments", {{"schematic", session.project().schematicCells.at(cellId).name}}}});
                } else {
                    suggestions.push_back(
                        {{"tool", "schematic_info"},
                         {"arguments", {{"schematic", session.project().schematicCells.at(cellId).name}}}});
                }
                item["suggested_calls"] = std::move(suggestions);
                items.push_back(std::move(item));
            }
            return {{"schematic", session.project().schematicCells.at(cellId).name},
                    {"ok", ok},
                    {"diagnostics", std::move(items)}};
        }
        if (name == "schematic_import_spice") {
            const auto schematicName = requiredString(arguments, "name");
            const auto pdk = optionalString(arguments, "pdk", ice::genericPdkId);
            std::string source = optionalString(arguments, "source");
            if (const auto file = optionalString(arguments, "file"); !file.empty()) {
                std::filesystem::path path(file);
                std::error_code status;
                if (path.is_relative()) {
                    const auto candidate = projectRootPath() / path;
                    if (std::filesystem::is_regular_file(candidate, status) && !status)
                        path = candidate;
                }
                std::ifstream input(path, std::ios::binary);
                if (!input)
                    throw ToolError("Unable to read SPICE file: " + path.string());
                std::ostringstream contents;
                contents << input.rdbuf();
                source = contents.str();
            }
            if (source.empty())
                throw ToolError("Argument 'source' or a readable 'file' is required.");
            const auto mode = lowerCopy(optionalString(arguments, "mode", "text"));
            if (mode != "text" && mode != "drawn" && mode != "auto")
                throw ToolError("mode must be 'text', 'drawn', or 'auto'");
            bool drawn = mode == "drawn";
            if (mode == "auto")
                drawn = ice::spiceNetlistSupportsDrawn(source);
            ice::SpiceImportResult imported;
            session.execute("mcp schematic-import-spice", [&](ice::Project &project) {
                imported = ice::createSchematicFromSpice(project, schematicName, pdk, source, drawn);
            });
            return {{"id", imported.cell},
                    {"name", schematicName},
                    {"pdk", pdk},
                    {"mode", drawn ? "drawn" : "text"},
                    {"warnings", imported.warnings}};
        }
        if (name == "schematic_add_symbol") {
            ice::ObjectId id = 0;
            const auto cell = resolveSchematicArgument(session.project(), arguments, {"cell", "schematic"});
            const auto symbolName = requiredString(arguments, "name");
            auto artwork = optionalString(arguments, "artwork");
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
                int width = 1;
                bool widthKnown = true;
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
                    input.width = optionalInteger(pin, "width").value_or(1);
                    input.widthKnown = optionalBoolean(pin, "width_known", true);
                    pinInputs.push_back(std::move(input));
                }
            }
            if (sourceCell != 0 && pinInputs.empty()) {
                const auto &sourceCellRef = session.project().schematicCells.at(sourceCell);
                if (sourceCellRef.systemVerilogSchematic) {
                    const auto parsed = ice::parseSystemVerilogModule(sourceCellRef);
                    if (!parsed.ok)
                        throw ToolError("SystemVerilog parse failed: " + parsed.error);
                    const auto templ = ice::makeHdlSymbolTemplate(parsed.module);
                    if (artwork.empty())
                        artwork = templ.artwork;
                    for (const auto &pin : templ.pins)
                        pinInputs.push_back({pin.name, pin.x, pin.y, pin.direction, pin.width, pin.widthKnown});
                }
            }
            if (sourceCell == 0) {
                try {
                    const auto definition = ice::resolveSchematicSymbol(cellRef.pdk, symbolName);
                    if (artwork.empty())
                        artwork = definition.svg;
                    if (pinInputs.empty()) {
                        for (const auto &pin : definition.pins)
                            pinInputs.push_back({pin.name, pin.x, pin.y, pin.direction, pin.width, pin.widthKnown});
                    }
                } catch (const ice::ProjectError &error) {
                    if (pinInputs.empty()) {
                        throw ToolError(std::string(error.what()) +
                                        ". Use schematic_add_component, or provide explicit pins and artwork. "
                                        "Use schematic_list_symbols to inspect available symbols.");
                    }
                }
            }
            if (pinInputs.empty())
                throw ToolError("Symbol '" + symbolName +
                                "' would have no pins. Use schematic_add_component, schematic_add_hdl_instance, "
                                "or provide a non-empty pins array.");
            session.execute("mcp schematic-add-symbol", [&](ice::Project &project) {
                id = ice::addSymbol(project, cell, symbolName, artwork, sourceCell);
                auto &symbol = project.schematicCells.at(cell).symbols.at(id);
                for (auto &input : pinInputs) {
                    const auto pinId = project.nextId();
                    symbol.pins.push_back(
                        {pinId, input.name, input.x, input.y, input.direction, input.width, input.widthKnown});
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
        if (name == "schematic_validate_hierarchy") {
            const auto cellId = resolveSchematicArgument(session.project(), arguments, {"cell", "schematic"});
            const auto plan = ice::buildMixedSignalPlan(session.project(), cellId);
            const auto diagnostics = ice::validateHdlHierarchy(session.project(), cellId);
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
                                     {"direction", hdlPortDirectionName(port.direction)},
                                     {"width", port.width},
                                     {"node", node == binding.portNodes.end() ? json(nullptr) : json(node->second)}});
                }
                blocks.push_back({{"instance", binding.instanceName},
                                  {"module", binding.moduleName},
                                  {"source_cell", binding.sourceCell},
                                  {"ports", std::move(ports)}});
            }
            json result = {{"id", cellId},
                           {"schematic", session.project().schematicCells.at(cellId).name},
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
            return result;
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
            const auto &symbol = cell.symbols.at(symbolId);
            if (symbol.pins.empty()) {
                throw ToolError("Symbol '" + symbol.name +
                                "' has no pins and cannot be wired. Recreate it with schematic_add_component "
                                "or schematic_add_hdl_instance, or add pins explicitly with schematic_add_pin.");
            }
            session.execute("mcp schematic-add-instance", [&](ice::Project &project) {
                id = ice::addSchematicInstance(project, cellId, symbolId, instanceName, x, y);
            });
            return {{"id", id},
                    {"cell", cellId},
                    {"name", instanceName},
                    {"symbol", symbolId},
                    {"symbol_name", cell.symbols.at(symbolId).name}};
        }
        if (name == "schematic_set_parameter") {
            const auto cellId = resolveSchematicArgument(session.project(), arguments, {"cell", "schematic"});
            const auto &cell = session.project().schematicCells.at(cellId);
            const auto instanceId = lookupByIdOrName(cell.instances, requiredValue(arguments, "instance"), "instance");
            const auto parameterName = requiredString(arguments, "name");
            const auto value = requiredStringAllowEmpty(arguments, "value");
            session.execute("mcp schematic-set-parameter", [&](ice::Project &project) {
                ice::setSchematicParameter(project, cellId, instanceId, parameterName, value);
            });
            return {{"cell", cellId}, {"instance", instanceId}, {"name", parameterName}, {"value", value}};
        }
        if (name == "schematic_add_hdl_instance") {
            const auto parentCell = resolveSchematicArgument(session.project(), arguments, {"cell", "schematic"});
            const auto sourceText = requiredValue(arguments, "source_cell");
            const auto sourceCell = lookupByIdOrName(session.project().schematicCells, sourceText, "schematic");
            const auto instanceName = requiredString(arguments, "name");
            const auto x = requiredInteger(arguments, "x");
            const auto y = requiredInteger(arguments, "y");
            const auto busMode = optionalString(arguments, "bus_mode", "scalar");
            if (busMode != "scalar" && busMode != "flatten")
                throw ToolError("bus_mode must be 'scalar' or 'flatten'");
            const bool flattenBuses = busMode == "flatten" || optionalBoolean(arguments, "flatten_buses", false);
            ice::ObjectId instanceId = 0;
            ice::ObjectId symbolId = 0;
            session.execute("mcp schematic-add-hdl-instance", [&](ice::Project &project) {
                instanceId = ice::addSystemVerilogInstance(project, parentCell, sourceCell, instanceName, x, y,
                                                           &symbolId, flattenBuses);
            });
            json pins = json::array();
            const auto &symbol = session.project().schematicCells.at(parentCell).symbols.at(symbolId);
            for (const auto &pin : symbol.pins) {
                pins.push_back({{"id", pin.id},
                                {"name", pin.name},
                                {"direction", pinDirectionName(pin.direction)},
                                {"width", pin.width},
                                {"width_known", pin.widthKnown}});
            }
            return {{"id", instanceId},
                    {"cell", parentCell},
                    {"source_cell", sourceCell},
                    {"symbol", symbolId},
                    {"symbol_name", symbol.name},
                    {"name", instanceName},
                    {"x", x},
                    {"y", y},
                    {"pins", std::move(pins)}};
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
            const auto stack = resolveLayoutStack(pdk, optionalString(arguments, "stack"));
            const auto cellName = requiredString(arguments, "name");
            session.execute("mcp layout-create",
                            [&](ice::Project &project) { id = ice::createLayoutCell(project, cellName, pdk, stack); });
            return {{"id", id}, {"name", cellName}, {"pdk", pdk}, {"stack", stack}};
        }
        if (name == "layout_create_from_schematic") {
            ice::ObjectId id = 0;
            const auto schematic = resolveSchematic(session.project(), arguments, "schematic");
            const auto stack = resolveLayoutStack(session.project().schematicCells.at(schematic).pdk,
                                                  optionalString(arguments, "stack"));
            const auto layoutName = optionalString(arguments, "name");
            session.execute("mcp layout-create-from-schematic", [&](ice::Project &project) {
                id = ice::createLayoutFromSchematic(project, schematic, layoutName, stack);
            });
            return {{"id", id}, {"schematic", schematic}, {"stack", stack}};
        }
        if (name == "layout_add_layer") {
            ice::ObjectId id = 0;
            const auto layerName = requiredString(arguments, "name");
            const auto number = static_cast<int>(requiredInteger(arguments, "number"));
            const auto datatype = static_cast<int>(optionalInteger(arguments, "datatype").value_or(0));
            session.execute("mcp layout-add-layer", [&](ice::Project &project) {
                id = ice::addLayer(project, layerName, number, 0, {}, datatype);
            });
            return {{"id", id}, {"name", layerName}, {"number", number}, {"datatype", datatype}};
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
        if (name == "layout_info") {
            const auto cellId = resolveLayoutCell(session.project(), arguments);
            const auto &cell = session.project().layoutCells.at(cellId);
            json ports = json::array();
            for (const auto &[id, port] : cell.ports) {
                ports.push_back({{"id", id},
                                 {"name", port.name},
                                 {"label", port.label},
                                 {"layer", port.layer},
                                 {"rectangle",
                                  {{"x", port.rectangle.x},
                                   {"y", port.rectangle.y},
                                   {"width", port.rectangle.width},
                                   {"height", port.rectangle.height}}},
                                 {"direction", ice::portDirectionName(port.direction)},
                                 {"side", ice::portSideName(port.side)},
                                 {"net", port.net == 0 ? json(nullptr) : json(port.net)},
                                 {"visible", port.visible},
                                 {"locked", port.locked},
                                 {"source_instance", port.sourceInstance},
                                 {"source_port", port.sourcePort}});
            }
            json nets = json::array();
            for (const auto &[id, net] : cell.nets)
                nets.push_back({{"id", id}, {"name", net.name}});
            json instances = json::array();
            for (const auto &[id, instance] : cell.instances) {
                instances.push_back({{"id", id},
                                     {"cell", instance.cell},
                                     {"name", instance.name},
                                     {"x", instance.x},
                                     {"y", instance.y},
                                     {"orientation", ice::orientationName(instance.orientation)}});
            }
            return {{"id", cellId},
                    {"name", cell.name},
                    {"pdk", cell.pdk},
                    {"stack", cell.stack},
                    {"device", cell.device},
                    {"library", cell.library},
                    {"shapes", cell.shapes.size()},
                    {"ports", std::move(ports)},
                    {"nets", std::move(nets)},
                    {"instances", std::move(instances)}};
        }
        if (name == "layout_connectivity") {
            const auto cell = resolveLayoutCell(session.project(), arguments);
            json nets = json::array();
            for (const auto &group : ice::extractLayoutNets(session.project(), cell)) {
                nets.push_back({{"net", group.net == 0 ? json(nullptr) : json(group.net)},
                                {"name", group.name},
                                {"shapes", group.shapes},
                                {"ports", group.ports},
                                {"connections", group.connections}});
            }
            return {{"cell", cell}, {"nets", std::move(nets)}};
        }
        if (name == "layout_export_gds") {
            const auto cell = resolveLayoutCell(session.project(), arguments);
            const auto path = requiredString(arguments, "path");
            ice::LayoutInterchange::exportGds(session.project(), cell, path);
            return {{"cell", cell}, {"path", path}};
        }
        if (name == "layout_export_lef") {
            const auto cell = resolveLayoutCell(session.project(), arguments);
            const auto path = requiredString(arguments, "path");
            ice::LayoutInterchange::exportLef(session.project(), cell, path);
            return {{"cell", cell}, {"path", path}};
        }
        if (name == "layout_export_def") {
            const auto cell = resolveLayoutCell(session.project(), arguments);
            const auto path = requiredString(arguments, "path");
            ice::LayoutInterchange::exportDef(session.project(), cell, path);
            return {{"cell", cell}, {"path", path}};
        }

        if (name == "pex_run") {
            const auto layout = resolveLayoutCell(session.project(), arguments, "layout");
            ice::PexOptions options;
            const auto optionalNumber = [&](const char *key, double &target) {
                const auto it = arguments.find(key);
                if (it == arguments.end() || it->is_null())
                    return;
                const auto value = numberValue(*it, key);
                if (value < 0.0)
                    throw ToolError(std::string("Argument must be non-negative: ") + key);
                target = value;
            };
            optionalNumber("metal_sheet_resistance", options.metalSheetResistance);
            optionalNumber("poly_sheet_resistance", options.polySheetResistance);
            optionalNumber("diffusion_sheet_resistance", options.diffusionSheetResistance);
            optionalNumber("via_resistance", options.viaResistance);
            optionalNumber("area_capacitance", options.areaCapacitance);
            optionalNumber("perimeter_capacitance", options.perimeterCapacitance);
            options.extractPlacedDevices = optionalBoolean(arguments, "extract_placed_devices", true);
            options.extractPolygonDevices = optionalBoolean(arguments, "extract_polygon_devices", true);
            options.includeLinkedSchematic = optionalBoolean(arguments, "include_linked_schematic", true);
            const auto extractionRulesPath = optionalString(arguments, "extraction_rules_path");
            if (!extractionRulesPath.empty())
                options.extractionRulesPath = extractionRulesPath;
            options.schematicName = optionalString(arguments, "name");
            ice::PexReport report;
            session.execute("mcp pex-run",
                            [&](ice::Project &project) { report = ice::runPex(project, layout, options); });
            return json::parse(ice::serializePexReport(report));
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
        const auto customTool = [](std::string name, std::string description, std::string schema) {
            agentsdk::mcp::tool result;
            result.name = std::move(name);
            result.description = std::move(description);
            result.input_schema = std::move(schema);
            return result;
        };
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
                            .with_description("Open an ICE project; without path, searches cwd and parents for "
                                              "exactly one project.icepro.")
                            .with_string_param("path", "Optional project folder or project.icepro path", false)
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
            tool_builder("project_summary")
                .with_description("Summarize the open project and its cells; when closed, return candidate paths.")
                .build());
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
            tool_builder("ice_help")
                .with_description("Return workflow help, common tools, and example payloads.")
                .with_string_param("topic", "overview, project, schematic, mixed-signal, simulation, pdk", false)
                .build());
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
                            .with_string_param("template", "tran, mixed, pwm_dac, or digital", false)
                            .with_string_param("analysis", "Analysis type", false)
                            .with_string_param("simulator", "ngspice, iverilog, or mixed", false)
                            .with_number_param("stop", "Stop time", false)
                            .with_number_param("step", "Time step", false)
                            .with_string_param("signals", "Signals to capture", false)
                            .with_string_param("graph_name", "Initial graph name", false)
                            .build());
        tools.push_back(tool_builder("simulation_update")
                            .with_description("Update simulation settings and capture signals.")
                            .with_string_param("simulation", "Simulation name or id")
                            .with_string_param("analysis", "Analysis type", false)
                            .with_string_param("simulator", "ngspice, iverilog, or mixed", false)
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
        tools.push_back(tool_builder("simulation_trace")
                            .with_description("Return decimated stored waveform samples for signals.")
                            .with_string_param("simulation", "Simulation name or id")
                            .with_string_param("signals", "Optional comma-separated or array of signal names", false)
                            .with_integer_param("max_points", "Maximum samples per trace (default 2000)", false)
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
        tools.push_back(tool_builder("schematic_hdl_ports")
                            .with_description("Parse a SystemVerilog cell and return its module name and ports.")
                            .with_string_param("schematic", "SystemVerilog schematic name or id")
                            .build());
        tools.push_back(customTool(
            "schematic_add_component",
            "Create a common component (resistor, capacitor, source, ground, device) with symbol and pins.",
            R"json({"type":"object","properties":{"cell":{"type":["string","integer"]},"type":{"type":"string","description":"resistor, capacitor, voltage source, ground, nmos, ..."},"symbol":{"type":"string","description":"Exact symbol name; overrides type"},"name":{"type":"string"},"x":{"type":"integer"},"y":{"type":"integer"},"parameters":{"type":["object","array"],"description":"Parameter map or array of {name,value}"}},"required":["cell"]})json"));
        tools.push_back(customTool(
            "schematic_add_circuit",
            "Add multiple components and nets atomically and return all instance, symbol, pin, and net ids.",
            R"json({"type":"object","properties":{"cell":{"type":["string","integer"]},"components":{"type":"array","minItems":1,"items":{"type":"object","properties":{"type":{"type":"string"},"symbol":{"type":"string"},"name":{"type":"string"},"x":{"type":"integer"},"y":{"type":"integer"},"parameters":{"type":["object","array"]}}}},"nets":{"type":"array","items":{"type":"object","properties":{"name":{"type":"string"},"pins":{"type":"array","minItems":2,"items":{"type":["string","integer"]}},"x":{"type":"integer"},"y":{"type":"integer"}},"required":["pins"]}}},"required":["cell","components"]})json"));
        tools.push_back(tool_builder("schematic_list_symbols")
                            .with_description("List available primitive and PDK symbols with pins and defaults.")
                            .with_string_param("pdk", "Optional PDK id", false)
                            .with_string_param("query", "Optional name/category filter", false)
                            .build());
        tools.push_back(tool_builder("schematic_validate")
                            .with_description("Validate schematic connectivity, component values, PDK setup, and HDL.")
                            .with_string_param("schematic", "Schematic name or id")
                            .build());
        tools.push_back(customTool(
            "schematic_import_spice", "Create a text SPICE cell or a drawn R/C/L/V/I/D schematic from a SPICE source.",
            R"json({"type":"object","properties":{"name":{"type":"string"},"pdk":{"type":"string"},"source":{"type":"string"},"file":{"type":"string"},"mode":{"type":"string","enum":["text","drawn","auto"]}},"required":["name"]})json"));
        tools.push_back(tool_builder("schematic_create")
                            .with_description("Create a drawn, text SPICE, or SystemVerilog schematic cell.")
                            .with_string_param("name", "Schematic name")
                            .with_string_param("pdk", "Optional PDK identifier", false)
                            .with_string_param("kind", "drawn, text, or systemverilog (default drawn)", false)
                            .with_string_param("text", "Optional source for text/SystemVerilog cells", false)
                            .build());
        tools.push_back(tool_builder("schematic_create_text")
                            .with_description("Create a text SPICE schematic cell.")
                            .with_string_param("name", "Schematic name")
                            .with_string_param("pdk", "Optional PDK identifier", false)
                            .with_string_param("text", "Optional SPICE source", false)
                            .build());
        tools.push_back(tool_builder("schematic_create_systemverilog")
                            .with_description("Create a SystemVerilog source cell.")
                            .with_string_param("name", "Schematic name")
                            .with_string_param("pdk", "Optional PDK identifier", false)
                            .with_string_param("text", "Optional initial SystemVerilog source", false)
                            .build());
        agentsdk::mcp::tool setTextTool;
        setTextTool.name = "schematic_set_text";
        setTextTool.description =
            "Replace the source of an existing text SPICE or SystemVerilog cell, or convert an empty drawn cell.";
        setTextTool.input_schema =
            R"({"type":"object","properties":{"schematic":{"type":["string","integer"],)"
            R"("description":"Schematic name or id"},"text":{"type":"string","description":"Source text"},)"
            R"("kind":{"type":"string","enum":["text","spice","systemverilog","sv"],)"
            R"("description":"Source language of the cell; defaults to its existing language"},)"
            R"("convert":{"type":"boolean","description":"Convert an empty drawn cell to a source cell"}},)"
            R"("required":["schematic","text"]})";
        tools.push_back(std::move(setTextTool));
        tools.push_back(tool_builder("schematic_delete")
                            .with_description("Inspect or delete a schematic, optionally cascading dependencies.")
                            .with_string_param("schematic", "Schematic name or id")
                            .with_boolean_param("dry_run", "Only return dependencies and suggested deletions", false)
                            .with_boolean_param("cascade", "Delete dependent simulations and HDL symbols too", false)
                            .build());
        agentsdk::mcp::tool symbolTool;
        symbolTool.name = "schematic_add_symbol";
        symbolTool.description =
            "Add a symbol definition; known library names auto-load artwork and pins. Custom symbols may "
            "provide explicit pins and artwork.";
        symbolTool.input_schema =
            R"({"type":"object","properties":{"cell":{"type":"string","description":"Schematic name or id"},)"
            R"("name":{"type":"string","description":"Symbol name"},"artwork":{"type":"string"},)"
            R"("source_cell":{"type":"string"},"pins":{"type":"array","items":{"type":"object","properties":{)"
            R"("name":{"type":"string"},"x":{"type":"integer"},"y":{"type":"integer"},)"
            R"("width":{"type":"integer"},"width_known":{"type":"boolean"},)"
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
        tools.push_back(
            tool_builder("schematic_validate_hierarchy")
                .with_description("Validate SystemVerilog instances, ports, nets, and mixed-signal support.")
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
        tools.push_back(tool_builder("schematic_add_hdl_instance")
                            .with_description("Create a symbol from a SystemVerilog module's ports and place it.")
                            .with_string_param("cell", "Parent schematic name or id")
                            .with_string_param("source_cell", "SystemVerilog source cell name or id")
                            .with_string_param("name", "Instance name")
                            .with_integer_param("x", "X coordinate")
                            .with_integer_param("y", "Y coordinate")
                            .with_string_param("bus_mode", "scalar or flatten (default scalar)", false)
                            .with_boolean_param("flatten_buses", "Alias for bus_mode=flatten", false)
                            .build());
        tools.push_back(tool_builder("schematic_set_parameter")
                            .with_description("Set or create an instance parameter such as resistance or value.")
                            .with_string_param("cell", "Schematic name or id")
                            .with_string_param("instance", "Instance name or id")
                            .with_string_param("name", "Parameter name")
                            .with_string_param("value", "Parameter value")
                            .build());
        tools.push_back(tool_builder("layout_create")
                            .with_description("Create a static layout cell.")
                            .with_string_param("name", "Layout name")
                            .with_string_param("pdk", "Optional PDK identifier", false)
                            .with_string_param("stack", "Optional PDK stack identifier", false)
                            .build());
        tools.push_back(tool_builder("layout_create_from_schematic")
                            .with_description("Create a layout from a schematic.")
                            .with_string_param("schematic", "Schematic name or id")
                            .with_string_param("name", "Optional layout name", false)
                            .with_string_param("stack", "Optional PDK stack identifier", false)
                            .build());
        tools.push_back(tool_builder("layout_delete")
                            .with_description("Delete a static or procedural layout; blocked when instances use it.")
                            .with_string_param("layout", "Layout or procedural layout name or id")
                            .build());
        tools.push_back(tool_builder("layout_add_layer")
                            .with_description("Add a layout layer.")
                            .with_string_param("name", "Layer name")
                            .with_integer_param("number", "Layer number")
                            .with_integer_param("datatype", "GDS datatype/purpose", false)
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
        tools.push_back(tool_builder("layout_info")
                            .with_description("Read a layout cell's pins, nets, instances, and shape count.")
                            .with_string_param("cell", "Layout cell name or id")
                            .build());
        tools.push_back(tool_builder("layout_connectivity")
                            .with_description("Extract nets from explicit layout nets and same-layer geometry.")
                            .with_string_param("cell", "Layout cell name or id")
                            .build());
        tools.push_back(tool_builder("layout_export_gds")
                            .with_description("Export a layout cell to GDSII, including pins and labels.")
                            .with_string_param("cell", "Layout cell name or id")
                            .with_string_param("path", "Output GDS path")
                            .build());
        tools.push_back(tool_builder("layout_export_lef")
                            .with_description("Export layout pin metadata as a minimal LEF macro.")
                            .with_string_param("cell", "Layout cell name or id")
                            .with_string_param("path", "Output LEF path")
                            .build());
        tools.push_back(tool_builder("layout_export_def")
                            .with_description("Export layout pins as a minimal DEF design.")
                            .with_string_param("cell", "Layout cell name or id")
                            .with_string_param("path", "Output DEF path")
                            .build());
        tools.push_back(
            tool_builder("lvs_run")
                .with_description("Compare a layout against its schematic and report mismatches.")
                .with_string_param("layout", "Layout cell name or id")
                .with_string_param("schematic", "Optional schematic name or id; inferred when omitted", false)
                .build());
        tools.push_back(
            tool_builder("pex_run")
                .with_description("Extract placed and PDK-rule polygon devices plus first-order layout parasitics into "
                                  "a text SPICE schematic.")
                .with_string_param("layout", "Layout cell name or id")
                .with_string_param("name", "Optional text schematic name", false)
                .with_number_param("metal_sheet_resistance", "Optional metal sheet resistance (ohm/square)", false)
                .with_number_param("poly_sheet_resistance", "Optional poly sheet resistance (ohm/square)", false)
                .with_number_param("diffusion_sheet_resistance", "Optional diffusion sheet resistance (ohm/square)",
                                   false)
                .with_number_param("via_resistance", "Optional resistance per via shape (ohm)", false)
                .with_number_param("area_capacitance", "Optional area capacitance (fF/um^2)", false)
                .with_number_param("perimeter_capacitance", "Optional perimeter capacitance (fF/um)", false)
                .with_boolean_param("extract_placed_devices",
                                    "Extract devices from placed layout instances (default true)", false)
                .with_boolean_param("extract_polygon_devices", "Extract polygon devices using PDK rules (default true)",
                                    false)
                .with_string_param("extraction_rules_path", "Optional override for layout/extraction.json", false)
                .with_boolean_param("include_linked_schematic",
                                    "Fallback to the inferred source schematic netlist (default true)", false)
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
            for (const auto *feature : {"mcp", "schematic_create_text", "simulation_measure", "pdk_info"})
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
