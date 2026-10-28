#include "icelib/core.hpp"
#include "icelib/operations.hpp"

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
    explicit IceMcpServer(std::optional<std::filesystem::path> projectPath) {
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
            "project, schematic, and layout tools. Mutations are transactional in memory; the MCP "
            "server does not write them to disk. Use ice-cli for persisted project changes.");
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
                                             {"simulator", simulation.simulator}});
        }
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
        else
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
        IceMcpServer server(std::move(projectPath));
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
