#include "icelib/core.hpp"
#include "icelib/operations.hpp"
#include "icelib/simulation_report.hpp"

#include <nlohmann/json.hpp>

#ifdef ICE_HAVE_AGENTSDK
#include <agentsdk/mcp/mcp_server.hpp>
#include <agentsdk/mcp/mcp_tool_builder.hpp>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
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

ice::ObjectId resolveLayoutCell(const ice::Project &project, const json &arguments, const char *key = "cell") {
    return lookupByIdOrName(project.layoutCells, requiredValue(arguments, key), "layout cell");
}

ice::ObjectId resolveLayer(const ice::Project &project, const json &arguments, const char *key = "layer") {
    return lookupByIdOrName(project.layers, requiredValue(arguments, key), "layer");
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
    for (const char character : text) {
        if (character != ',') {
            current.push_back(character);
            continue;
        }
        const auto first = current.find_first_not_of(" \t");
        const auto last = current.find_last_not_of(" \t");
        if (first != std::string::npos)
            values.push_back(current.substr(first, last - first + 1));
        current.clear();
    }
    const auto first = current.find_first_not_of(" \t");
    const auto last = current.find_last_not_of(" \t");
    if (first != std::string::npos)
        values.push_back(current.substr(first, last - first + 1));
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
    IceMcpServer(std::optional<std::filesystem::path> projectPath, std::filesystem::path executablePath)
        : executablePath_(std::move(executablePath)) {
        if (projectPath) {
            auto project = ice::Project::load(*projectPath);
            session_ = std::make_unique<ice::ProjectSession>(std::move(project));
            projectPath_ = std::move(*projectPath);
        }
    }

#ifdef ICE_HAVE_AGENTSDK
    int run() {
        auto logger = spdlog::stderr_color_mt("ice-mcp");
        spdlog::set_default_logger(std::move(logger));

        agentsdk::mcp::implementation_info info;
        info.name = "ice-mcp";
        info.title = "ICE MCP Server";
        info.version = ice::version();

        agentsdk::mcp::server_capabilities capabilities;
        capabilities.tools = agentsdk::mcp::tools_capability{false};
        capabilities.resources = agentsdk::mcp::resources_capability{false, false};
        capabilities.logging = true;

        agentsdk::mcp::mcp_server server(std::move(info), std::move(capabilities));
        server.set_instructions(
            "ICE is an integrated-circuit design environment. Open or create a project first, then use the "
            "project, schematic, layout, simulation graph, and report tools. Project mutations are transactional in "
            "memory; "
            "report source files are written under reports/, but project metadata remains in memory. "
            "Use ice-cli for persisted project changes.");
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
            result["reports"].push_back({{"id", id}, {"name", report.name}, {"path", report.path}});
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

    json call(const std::string &name, const json &arguments) {
        if (name == "ice_version")
            return {{"version", ice::version()}};

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
            return summary();
        }

        if (name == "project_open") {
            const auto path = std::filesystem::path(requiredString(arguments, "path"));
            auto project = ice::Project::load(path);
            session_ = std::make_unique<ice::ProjectSession>(std::move(project));
            projectPath_ = path;
            return summary();
        }
        if (name == "project_summary")
            return summary();

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

        if (name == "report_create") {
            const auto reportName = requiredString(arguments, "name");
            const auto source = optionalString(arguments, "source");
            const auto root = projectRootPath();
            ice::ObjectId id = 0;
            session.execute("mcp report-create", [&](ice::Project &project) {
                id = ice::createProjectReport(project, root, reportName, source);
            });
            const auto &report = session.project().reports.at(id);
            return {{"id", id},
                    {"name", report.name},
                    {"path", report.path},
                    {"source", ice::readProjectReportSource(root, report)}};
        }
        if (name == "report_list") {
            json reports = json::array();
            for (const auto &[id, report] : session.project().reports)
                reports.push_back({{"id", id}, {"name", report.name}, {"path", report.path}});
            return reports;
        }
        if (name == "report_read") {
            const auto id = lookupByIdOrName(session.project().reports, requiredValue(arguments, "report"), "report");
            const auto &report = session.project().reports.at(id);
            return {{"id", id},
                    {"name", report.name},
                    {"path", report.path},
                    {"source", ice::readProjectReportSource(projectRootPath(), report)}};
        }
        if (name == "report_write") {
            const auto id = lookupByIdOrName(session.project().reports, requiredValue(arguments, "report"), "report");
            const auto &report = session.project().reports.at(id);
            const auto source = requiredStringAllowEmpty(arguments, "source");
            ice::writeProjectReportSource(projectRootPath(), report, source);
            return {{"id", id}, {"name", report.name}, {"path", report.path}, {"bytes", source.size()}};
        }
        if (name == "report_run") {
            const auto id = lookupByIdOrName(session.project().reports, requiredValue(arguments, "report"), "report");
            const auto &report = session.project().reports.at(id);
            const auto timeoutValue = optionalInteger(arguments, "timeout_ms").value_or(30000);
            const auto timeout = std::chrono::milliseconds(std::max<std::int64_t>(1, timeoutValue));
            const auto result = ice::runProjectReport(projectRootPath(), report, executablePath_, timeout);
            return {{"id", id},
                    {"name", report.name},
                    {"path", report.path},
                    {"ok", !result.timedOut && result.exitCode == 0},
                    {"timed_out", result.timedOut},
                    {"exit_code", result.exitCode},
                    {"stdout", result.stdoutText},
                    {"stderr", result.stderrText}};
        }
        if (name == "report_delete") {
            const auto id = lookupByIdOrName(session.project().reports, requiredValue(arguments, "report"), "report");
            const auto report = session.project().reports.at(id);
            session.execute("mcp report-delete", [&](ice::Project &project) { project.reports.erase(id); });
            return {{"id", id}, {"name", report.name}, {"path", report.path}, {"deleted", true}};
        }

        if (name == "schematic_create") {
            ice::ObjectId id = 0;
            const auto pdk = optionalString(arguments, "pdk", ice::genericPdkId);
            const auto cellName = requiredString(arguments, "name");
            session.execute("mcp schematic-create",
                            [&](ice::Project &project) { id = ice::createSchematicCell(project, cellName, pdk); });
            return {{"id", id}, {"name", cellName}, {"pdk", pdk}};
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
            return {{"id", id}, {"name", cellName}, {"pdk", pdk}, {"text", text}};
        }
        if (name == "schematic_add_symbol") {
            ice::ObjectId id = 0;
            const auto cell = resolveSchematic(session.project(), arguments);
            const auto symbolName = requiredString(arguments, "name");
            const auto artwork = optionalString(arguments, "artwork");
            ice::ObjectId sourceCell = 0;
            const auto source = arguments.find("source_cell");
            if (source != arguments.end() && !source->is_null())
                sourceCell = lookupByIdOrName(session.project().schematicCells, *source, "schematic");
            session.execute("mcp schematic-add-symbol", [&](ice::Project &project) {
                id = ice::addSymbol(project, cell, symbolName, artwork, sourceCell);
            });
            return {{"id", id}, {"cell", cell}, {"name", symbolName}};
        }
        if (name == "schematic_add_instance") {
            ice::ObjectId id = 0;
            const auto cell = resolveSchematic(session.project(), arguments);
            const auto symbol = static_cast<ice::ObjectId>(requiredInteger(arguments, "symbol_id"));
            const auto instanceName = requiredString(arguments, "name");
            const auto x = requiredInteger(arguments, "x");
            const auto y = requiredInteger(arguments, "y");
            session.execute("mcp schematic-add-instance", [&](ice::Project &project) {
                id = ice::addSchematicInstance(project, cell, symbol, instanceName, x, y);
            });
            return {{"id", id}, {"cell", cell}, {"name", instanceName}};
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
            session.execute("mcp layout-add-port", [&](ice::Project &project) {
                id = ice::addPort(project, cell, portName, layer, rectangle);
            });
            return {{"id", id}, {"cell", cell}, {"name", portName}};
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

        throw ToolError("Unknown tool: " + name);
    }

#ifdef ICE_HAVE_AGENTSDK
    std::vector<agentsdk::mcp::tool> listTools() const {
        using agentsdk::mcp::tool_builder;
        std::vector<agentsdk::mcp::tool> tools;
        tools.push_back(tool_builder("ice_version").with_description("Return the ICE version.").build());
        tools.push_back(tool_builder("project_create")
                            .with_description("Create and open a new ICE project.")
                            .with_string_param("path", "Project folder or project.icepro path")
                            .with_string_param("name", "Project name")
                            .build());
        tools.push_back(tool_builder("project_open")
                            .with_description("Open an existing ICE project.")
                            .with_string_param("path", "Project folder or project.icepro path")
                            .build());
        tools.push_back(
            tool_builder("project_summary").with_description("Summarize the open project and its cells.").build());
        tools.push_back(tool_builder("project_validate")
                            .with_description("Validate the open project and return diagnostics.")
                            .build());
        tools.push_back(tool_builder("project_undo").with_description("Undo the last project mutation.").build());
        tools.push_back(tool_builder("project_redo").with_description("Redo the next project mutation.").build());
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
        tools.push_back(tool_builder("report_create")
                            .with_description("Create a project-level Python report file under reports/.")
                            .with_string_param("name", "Report name")
                            .with_string_param("source", "Optional initial Python source", false)
                            .build());
        tools.push_back(tool_builder("report_list").with_description("List project-level reports.").build());
        tools.push_back(tool_builder("report_read")
                            .with_description("Read a report's metadata and Python source.")
                            .with_string_param("report", "Report name or id")
                            .build());
        tools.push_back(tool_builder("report_write")
                            .with_description("Replace a report's Python source.")
                            .with_string_param("report", "Report name or id")
                            .with_string_param("source", "Python source")
                            .build());
        tools.push_back(tool_builder("report_run")
                            .with_description("Run a report and return its exit code, stdout, and stderr.")
                            .with_string_param("report", "Report name or id")
                            .with_integer_param("timeout_ms", "Optional timeout in milliseconds", false)
                            .build());
        tools.push_back(tool_builder("report_delete")
                            .with_description("Delete report metadata; the Python file remains on disk.")
                            .with_string_param("report", "Report name or id")
                            .build());
        tools.push_back(tool_builder("schematic_create")
                            .with_description("Create a drawn schematic cell.")
                            .with_string_param("name", "Schematic name")
                            .with_string_param("pdk", "Optional PDK identifier", false)
                            .build());
        tools.push_back(tool_builder("schematic_create_text")
                            .with_description("Create a text SPICE schematic cell.")
                            .with_string_param("name", "Schematic name")
                            .with_string_param("pdk", "Optional PDK identifier", false)
                            .with_string_param("text", "Optional SPICE source", false)
                            .build());
        tools.push_back(tool_builder("schematic_add_symbol")
                            .with_description("Add a symbol definition to a schematic cell.")
                            .with_string_param("cell", "Schematic name or id")
                            .with_string_param("name", "Symbol name")
                            .with_string_param("artwork", "Optional SVG artwork", false)
                            .with_string_param("source_cell", "Optional source schematic name or id", false)
                            .build());
        tools.push_back(tool_builder("schematic_add_instance")
                            .with_description("Place a symbol instance in a schematic.")
                            .with_string_param("cell", "Schematic name or id")
                            .with_integer_param("symbol_id", "Symbol id")
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
        tools.push_back(tool_builder("layout_add_port")
                            .with_description("Add a port to a layout cell.")
                            .with_string_param("cell", "Layout cell name or id")
                            .with_string_param("name", "Port name")
                            .with_string_param("layer", "Layer name or id")
                            .with_integer_param("x", "X coordinate")
                            .with_integer_param("y", "Y coordinate")
                            .with_integer_param("width", "Width")
                            .with_integer_param("height", "Height")
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
        resources.push_back({"ice://project/reports", "Project reports", std::nullopt,
                             "Project-level report metadata and Python sources", "application/json", std::nullopt,
                             std::nullopt});
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
        else if (uri == "ice://project/reports") {
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
};

} // namespace

int main(int argc, char **argv) {
    std::optional<std::filesystem::path> projectPath;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--version" || argument == "version") {
            std::cout << ice::version() << "\n";
            return 0;
        }
        if (argument == "--help" || argument == "-h") {
            std::cout << "ice-mcp " << ice::version() << "\n"
                      << "Usage: ice-mcp [--project <path>] [--version]\n"
                      << "\n"
                      << "Runs an MCP server over stdio. Optionally opens a project at startup.\n";
            return 0;
        }
        if (argument == "--project" && index + 1 < argc) {
            projectPath = std::filesystem::path(argv[++index]);
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
        IceMcpServer server(std::move(projectPath), std::move(executablePath));
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
