#include "icelib/core.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <fstream>
#include <nlohmann/json.hpp>
#include <set>
#include <spdlog/spdlog.h>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace ice {
using json = nlohmann::ordered_json;

namespace {
std::string trimCopy(std::string text) {
    const auto notSpace = [](unsigned char character) { return !std::isspace(character); };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), notSpace));
    text.erase(std::find_if(text.rbegin(), text.rend(), notSpace).base(), text.end());
    return text;
}

std::string joinedSignalList(const std::vector<std::string> &signalNames) {
    std::string text;
    for (const auto &signal : signalNames) {
        if (!text.empty())
            text += ", ";
        text += signal;
    }
    return text;
}
} // namespace

ProjectError::ProjectError(std::string code, std::string message)
    : std::runtime_error(message), code_(std::move(code)) {}
const std::string &ProjectError::code() const noexcept { return code_; }

double DatabaseUnits::toNanometers(Coordinate value) const {
    if (denominator == 0)
        throw ProjectError("project.units", "Database unit denominator must not be zero");
    return static_cast<double>(value) * static_cast<double>(nanometers_per_unit) / static_cast<double>(denominator);
}
bool DatabaseUnits::valid() const { return nanometers_per_unit > 0 && denominator > 0; }
bool Rect::valid() const { return width > 0 && height > 0; }

std::string orientationName(Orientation value) {
    switch (value) {
    case Orientation::r0:
        return "r0";
    case Orientation::r90:
        return "r90";
    case Orientation::r180:
        return "r180";
    case Orientation::r270:
        return "r270";
    case Orientation::mx:
        return "mx";
    case Orientation::my:
        return "my";
    case Orientation::m45:
        return "m45";
    case Orientation::m135:
        return "m135";
    }
    throw ProjectError("project.orientation", "Unknown orientation value");
}

ObjectId Project::nextId() {
    ObjectId highest = 0;
    auto update = [&highest](ObjectId id) { highest = std::max(highest, id); };
    for (const auto &[id, cell] : schematicCells) {
        update(id);
        for (const auto &parameter : cell.parameters)
            update(parameter.id);
        for (const auto &[symbolId, symbol] : cell.symbols) {
            update(symbolId);
            for (const auto &pin : symbol.pins)
                update(pin.id);
        }
        for (const auto &[instanceId, instance] : cell.instances) {
            update(instanceId);
            for (const auto &parameter : instance.parameters)
                update(parameter.id);
        }
        for (const auto &[netId, net] : cell.nets)
            update(netId);
    }
    for (const auto &[id, _] : layers)
        highest = std::max(highest, id);
    for (const auto &[id, cell] : layoutCells) {
        update(id);
        for (const auto &parameter : cell.parameters)
            update(parameter.id);
        for (const auto &[shapeId, shape] : cell.shapes)
            update(shapeId);
        for (const auto &[portId, port] : cell.ports)
            update(portId);
        for (const auto &[netId, net] : cell.nets)
            update(netId);
        for (const auto &[instanceId, instance] : cell.instances)
            update(instanceId);
        for (const auto &[deviceId, device] : cell.plannedDevices)
            update(deviceId);
        for (const auto &[connectionId, connection] : cell.plannedConnections)
            update(connectionId);
    }
    for (const auto &[id, _] : proceduralLayouts)
        update(id);
    for (const auto &[id, sim] : simulations) {
        update(id);
        for (const auto &graph : sim.graphs)
            update(graph.id);
    }
    for (const auto &[id, _] : reports)
        update(id);
    return highest + 1;
}

Coordinate Project::snap(Coordinate value) const {
    if (grid <= 0)
        throw ProjectError("project.grid", "Grid spacing must be positive");
    Coordinate quotient = value / grid;
    Coordinate remainder = value % grid;
    if (remainder < 0) {
        quotient -= 1;
        remainder += grid;
    }
    return remainder * 2 < grid ? quotient * grid : (quotient + 1) * grid;
}

std::vector<Diagnostic> Project::validate() const {
    std::vector<Diagnostic> result;
    if (identity.empty())
        result.push_back(
            {Diagnostic::Severity::error, "project.identity", "Project identity is required", std::nullopt});
    if (name.empty())
        result.push_back({Diagnostic::Severity::error, "project.name", "Project name is required", std::nullopt});
    if (!units.valid())
        result.push_back(
            {Diagnostic::Severity::error, "project.units", "Database units must be positive", std::nullopt});
    if (grid <= 0)
        result.push_back({Diagnostic::Severity::error, "project.grid", "Grid spacing must be positive", std::nullopt});
    std::unordered_set<std::string> dependencyNames;
    for (const auto &dependency : packageDependencies) {
        if (dependency.empty()) {
            result.push_back({Diagnostic::Severity::error, "project.packages.name",
                              "Package dependency names must not be empty", std::nullopt});
            continue;
        }
        if (!dependencyNames.insert(dependency).second)
            result.push_back({Diagnostic::Severity::error, "project.packages.duplicate",
                              "Package dependency is listed more than once: " + dependency, std::nullopt});
    }
    for (const auto &[cellId, cell] : schematicCells) {
        if (cell.pdk.empty() || cell.pdk.find_first_of("/\\") != std::string::npos)
            result.push_back({Diagnostic::Severity::error, "schematic.pdk", "Schematic PDK is invalid", cellId});
        for (const auto &[instanceId, instance] : cell.instances) {
            if (!cell.symbols.contains(instance.symbol))
                result.push_back({Diagnostic::Severity::error, "schematic.instance.symbol",
                                  "Instance references a missing symbol", instanceId});
        }
        for (const auto &[netId, net] : cell.nets) {
            std::unordered_set<ObjectId> pinIds;
            pinIds.reserve(cell.symbols.size() * 2);
            for (const auto &[_, symbol] : cell.symbols)
                for (const auto &pin : symbol.pins)
                    pinIds.insert(pin.id);
            for (auto pinId : net.pins) {
                if (!pinIds.contains(pinId))
                    result.push_back(
                        {Diagnostic::Severity::error, "schematic.net.pin", "Net references a missing pin", netId});
            }
        }
        (void)cellId;
    }
    const auto layoutNameKey = [](std::string value) {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
        return value;
    };
    for (const auto &[cellId, cell] : layoutCells) {
        std::set<std::string> layoutNetNames;
        for (const auto &[netId, net] : cell.nets) {
            if (net.name.empty())
                result.push_back(
                    {Diagnostic::Severity::error, "layout.net.name", "Layout net name is required", netId});
            else if (!layoutNetNames.insert(layoutNameKey(net.name)).second)
                result.push_back({Diagnostic::Severity::error, "layout.net.name.duplicate",
                                  "Layout net name is listed more than once: " + net.name, netId});
        }
        std::set<std::string> portNames;
        for (const auto &[portId, port] : cell.ports) {
            if (port.name.empty())
                result.push_back({Diagnostic::Severity::error, "layout.port.name", "Port name is required", portId});
            else if (!portNames.insert(layoutNameKey(port.name)).second)
                result.push_back({Diagnostic::Severity::error, "layout.port.name.duplicate",
                                  "Port name is listed more than once: " + port.name, portId});
            if (port.label.empty())
                result.push_back({Diagnostic::Severity::error, "layout.port.label", "Port label is required", portId});
            if (port.net != 0 && !cell.nets.contains(port.net))
                result.push_back(
                    {Diagnostic::Severity::error, "layout.port.net", "Port references a missing layout net", portId});
            if (port.sourceInstance != 0 || port.sourcePort != 0) {
                const auto instance = cell.instances.find(port.sourceInstance);
                if (instance == cell.instances.end())
                    result.push_back({Diagnostic::Severity::error, "layout.port.source_instance",
                                      "Promoted port references a missing layout instance", portId});
                else {
                    const auto child = layoutCells.find(instance->second.cell);
                    if (child == layoutCells.end() || !child->second.ports.contains(port.sourcePort))
                        result.push_back({Diagnostic::Severity::error, "layout.port.source_port",
                                          "Promoted port references a missing child port", portId});
                }
            }
        }
        for (const auto &[shapeId, shape] : cell.shapes) {
            if (shape.net && *shape.net != 0 && !cell.nets.contains(*shape.net))
                result.push_back({Diagnostic::Severity::error, "layout.shape.net",
                                  "Shape references a missing layout net", shapeId});
            if (!layers.contains(shape.layer))
                result.push_back(
                    {Diagnostic::Severity::error, "layout.shape.layer", "Shape references a missing layer", shapeId});
            if ((!shape.polygon.empty() && shape.polygon.size() < 3) ||
                (shape.polygon.empty() && !shape.rectangle.valid()))
                result.push_back({Diagnostic::Severity::error, "layout.shape.geometry",
                                  "Shape rectangle must have positive dimensions", shapeId});
            if (!shape.text.empty() && !shape.centerline.empty())
                result.push_back({Diagnostic::Severity::error, "layout.text.geometry",
                                  "Text cannot also be a routed path", shapeId});
            if (!shape.centerline.empty()) {
                if (shape.centerline.size() < 2 || shape.width <= 0)
                    result.push_back({Diagnostic::Severity::error, "layout.path.geometry",
                                      "Path must have at least two points and a positive width", shapeId});
                if (!shape.segmentLayers.empty() && shape.segmentLayers.size() != shape.centerline.size() - 1)
                    result.push_back({Diagnostic::Severity::error, "layout.path.layers",
                                      "Path segment layer count does not match its points", shapeId});
                if (!shape.pointVias.empty() && shape.pointVias.size() != shape.centerline.size())
                    result.push_back({Diagnostic::Severity::error, "layout.path.vias",
                                      "Path via count does not match its points", shapeId});
                for (std::size_t index = 0; index + 1 < shape.centerline.size(); ++index) {
                    if (!layers.contains(shape.layerForSegment(index))) {
                        result.push_back({Diagnostic::Severity::error, "layout.path.layer",
                                          "Path segment references a missing layer", shapeId});
                        break;
                    }
                }
                for (const auto &vias : shape.pointVias) {
                    bool missing = false;
                    for (const auto layer : vias) {
                        if (!layers.contains(layer)) {
                            missing = true;
                            break;
                        }
                    }
                    if (missing) {
                        result.push_back({Diagnostic::Severity::error, "layout.path.via",
                                          "Path via references a missing layer", shapeId});
                        break;
                    }
                }
            }
        }
        for (const auto &[portId, port] : cell.ports) {
            if (!layers.contains(port.layer))
                result.push_back(
                    {Diagnostic::Severity::error, "layout.port.layer", "Port references a missing layer", portId});
            if (!port.rectangle.valid())
                result.push_back({Diagnostic::Severity::error, "layout.port.geometry",
                                  "Port rectangle must have positive dimensions", portId});
        }
        for (const auto &[deviceId, device] : cell.plannedDevices) {
            if (device.width <= 0 || device.height <= 0)
                result.push_back({Diagnostic::Severity::error, "layout.planned_device.geometry",
                                  "Planned device must have positive dimensions", deviceId});
        }
        for (const auto &[connectionId, connection] : cell.plannedConnections) {
            if (!cell.plannedDevices.contains(connection.from) || !cell.plannedDevices.contains(connection.to))
                result.push_back({Diagnostic::Severity::error, "layout.planned_connection.device",
                                  "Planned connection references a missing planned device", connectionId});
            if (connection.from == connection.to)
                result.push_back({Diagnostic::Severity::error, "layout.planned_connection.self",
                                  "Planned connection must join two different devices", connectionId});
            if (connection.net != 0 && !cell.nets.contains(connection.net))
                result.push_back({Diagnostic::Severity::error, "layout.planned_connection.net",
                                  "Planned connection references a missing layout net", connectionId});
        }
        (void)cellId;
    }
    for (const auto &[layoutId, layout] : proceduralLayouts) {
        if (layout.name.empty())
            result.push_back({Diagnostic::Severity::error, "layout.procedural.name",
                              "Procedural layout name is required", layoutId});
        if (layout.pdk.empty() || layout.pdk.find_first_of("/\\") != std::string::npos)
            result.push_back(
                {Diagnostic::Severity::error, "layout.procedural.pdk", "Procedural layout PDK is invalid", layoutId});
        if (!layoutCells.contains(layout.generatedCell))
            result.push_back({Diagnostic::Severity::error, "layout.procedural.generated",
                              "Procedural layout references a missing generated layout cell", layoutId});
    }
    std::unordered_set<std::string> reportNames;
    for (const auto &[reportId, report] : reports) {
        if (report.name.empty())
            result.push_back(
                {Diagnostic::Severity::error, "report.name", "Simulation script name is required", reportId});
        else if (!reportNames.insert(report.name).second)
            result.push_back({Diagnostic::Severity::error, "report.name.duplicate",
                              "Simulation script name is listed more than once: " + report.name, reportId});
        const auto normalized = std::filesystem::path(report.path).lexically_normal();
        const auto first = normalized.begin();
        const bool inScriptsDirectory =
            first != normalized.end() && (first->string() == "scripts" || first->string() == "reports");
        if (report.path.empty() || normalized.empty() || normalized.is_absolute() || !inScriptsDirectory ||
            normalized.extension() != ".py")
            result.push_back({Diagnostic::Severity::error, "report.path",
                              "Simulation script path must be a relative Python file inside scripts/", reportId});
    }
    return result;
}

static const char *pinDirectionName(PinDirection direction);
static PinDirection parsePinDirection(const std::string &value);

static void put(json &j, const Pin &v) {
    j = {{"id", v.id}, {"name", v.name}, {"x", v.x}, {"y", v.y}, {"direction", pinDirectionName(v.direction)}};
}
static void put(json &j, const Parameter &v) { j = {{"id", v.id}, {"name", v.name}, {"value", v.value}}; }
static void put(json &j, const Rect &v) { j = {{"x", v.x}, {"y", v.y}, {"width", v.width}, {"height", v.height}}; }

std::string Project::serialize() const {
    json j;
    j["format"] = "ice-project";
    j["schema_version"] = schemaVersion;
    j["identity"] = identity;
    j["name"] = name;
    j["grid"] = grid;
    j["units"] = {{"nanometers_per_unit", units.nanometers_per_unit}, {"denominator", units.denominator}};
    j["package_dependencies"] = packageDependencies;
    j["schematic_cells"] = json::array();
    for (const auto &[id, cell] : schematicCells) {
        json c = {{"id", id},
                  {"name", cell.name},
                  {"pdk", cell.pdk},
                  {"parameters", json::array()},
                  {"symbols", json::array()},
                  {"instances", json::array()},
                  {"nets", json::array()}};
        if (cell.textSchematic)
            c["text_schematic"] = true;
        if (!cell.spiceText.empty())
            c["spice_text"] = cell.spiceText;
        for (const auto &parameter : cell.parameters) {
            json entry;
            put(entry, parameter);
            c["parameters"].push_back(std::move(entry));
        }
        for (const auto &[sid, symbol] : cell.symbols) {
            json s = {{"id", sid}, {"name", symbol.name}, {"artwork", symbol.artwork}, {"pins", json::array()}};
            if (symbol.sourceCell != 0)
                s["cell"] = symbol.sourceCell;
            if (symbol.label)
                s["label"] = {{"x", symbol.label->x}, {"y", symbol.label->y}};
            for (const auto &pin : symbol.pins) {
                json p;
                put(p, pin);
                s["pins"].push_back(p);
            }
            c["symbols"].push_back(s);
        }
        for (const auto &[iid, instance] : cell.instances) {
            json item = {{"id", iid},
                         {"symbol", instance.symbol},
                         {"name", instance.name},
                         {"x", instance.x},
                         {"y", instance.y},
                         {"orientation", orientationName(instance.orientation)},
                         {"parameters", json::array()}};
            for (const auto &parameter : instance.parameters) {
                json entry;
                put(entry, parameter);
                item["parameters"].push_back(std::move(entry));
            }
            c["instances"].push_back(std::move(item));
        }
        for (const auto &[nid, net] : cell.nets)
            c["nets"].push_back({{"id", nid}, {"name", net.name}, {"pins", net.pins}, {"x", net.x}, {"y", net.y}});
        j["schematic_cells"].push_back(c);
    }
    j["layers"] = json::array();
    for (const auto &[id, layer] : layers)
        j["layers"].push_back({{"id", id},
                               {"name", layer.name},
                               {"number", layer.number},
                               {"visible", layer.visible},
                               {"layout", layer.layout},
                               {"pdk", layer.pdk}});
    j["layout_cells"] = json::array();
    for (const auto &[id, cell] : layoutCells) {
        json c = {{"id", id},
                  {"name", cell.name},
                  {"pdk", cell.pdk},
                  {"shapes", json::array()},
                  {"ports", json::array()},
                  {"instances", json::array()},
                  {"planned_devices", json::array()},
                  {"planned_connections", json::array()}};
        if (cell.library)
            c["library"] = true;
        if (!cell.device.empty())
            c["device"] = cell.device;
        if (cell.generatedBy != 0)
            c["generated_by"] = cell.generatedBy;
        if (!cell.parameters.empty()) {
            c["parameters"] = json::array();
            for (const auto &parameter : cell.parameters) {
                json entry;
                put(entry, parameter);
                c["parameters"].push_back(std::move(entry));
            }
        }
        for (const auto &[sid, shape] : cell.shapes) {
            json s = {{"id", sid}, {"layer", shape.layer}, {"rectangle", json::object()}};
            if (shape.net)
                s["net"] = *shape.net;
            put(s["rectangle"], shape.rectangle);
            if (!shape.polygon.empty()) {
                s["polygon"] = json::array();
                for (const auto &point : shape.polygon)
                    s["polygon"].push_back({{"x", point.x}, {"y", point.y}});
            }
            if (!shape.centerline.empty()) {
                s["centerline"] = json::array();
                for (const auto &point : shape.centerline)
                    s["centerline"].push_back({{"x", point.x}, {"y", point.y}});
                if (!shape.segmentLayers.empty()) {
                    s["segment_layers"] = json::array();
                    for (const auto layer : shape.segmentLayers)
                        s["segment_layers"].push_back(layer);
                }
                if (!shape.pointVias.empty()) {
                    s["point_vias"] = json::array();
                    for (const auto &vias : shape.pointVias) {
                        json entry = json::array();
                        for (const auto layer : vias)
                            entry.push_back(layer);
                        s["point_vias"].push_back(std::move(entry));
                    }
                }
                s["path_width"] = shape.width;
            }
            if (!shape.text.empty())
                s["text"] = shape.text;
            if (!shape.name.empty())
                s["name"] = shape.name;
            c["shapes"].push_back(s);
        }
        for (const auto &[pid, port] : cell.ports) {
            json p = {{"id", pid},
                      {"name", port.name},
                      {"layer", port.layer},
                      {"rectangle", json::object()},
                      {"direction", portDirectionName(port.direction)},
                      {"side", portSideName(port.side)},
                      {"net", port.net == 0 ? json(nullptr) : json(port.net)},
                      {"label", port.label},
                      {"visible", port.visible},
                      {"locked", port.locked},
                      {"source_instance", port.sourceInstance},
                      {"source_port", port.sourcePort}};
            put(p["rectangle"], port.rectangle);
            c["ports"].push_back(p);
        }
        c["nets"] = json::array();
        for (const auto &[netId, net] : cell.nets)
            c["nets"].push_back({{"id", netId}, {"name", net.name}});
        for (const auto &[iid, instance] : cell.instances)
            c["instances"].push_back({{"id", iid},
                                      {"cell", instance.cell},
                                      {"name", instance.name},
                                      {"x", instance.x},
                                      {"y", instance.y},
                                      {"orientation", orientationName(instance.orientation)}});
        for (const auto &[did, device] : cell.plannedDevices) {
            json d = {{"id", did},     {"name", device.name},   {"x", device.x},
                      {"y", device.y}, {"width", device.width}, {"height", device.height}};
            if (device.schematicInstance != 0)
                d["schematic_instance"] = device.schematicInstance;
            c["planned_devices"].push_back(std::move(d));
        }
        for (const auto &[cid, connection] : cell.plannedConnections)
            c["planned_connections"].push_back(
                {{"id", cid}, {"from", connection.from}, {"to", connection.to}, {"net", connection.net}});
        j["layout_cells"].push_back(c);
    }
    j["procedural_layouts"] = json::array();
    for (const auto &[id, layout] : proceduralLayouts) {
        j["procedural_layouts"].push_back({{"id", id},
                                           {"name", layout.name},
                                           {"pdk", layout.pdk},
                                           {"script", layout.script},
                                           {"generated_cell", layout.generatedCell}});
    }
    j["simulations"] = json::array();
    for (const auto &[id, sim] : simulations) {
        json s = {{"id", id},
                  {"name", sim.name},
                  {"schematic", sim.schematic},
                  {"simulator", sim.simulator},
                  {"analysis", sim.analysis},
                  {"stop", sim.stop},
                  {"step", sim.step},
                  {"use_initial_conditions", sim.use_initial_conditions},
                  {"directives", sim.directives},
                  {"signals", sim.captureSignals},
                  {"graphs", json::array()}};
        for (const auto &graph : sim.graphs) {
            json graphJson = {
                {"id", graph.id}, {"name", graph.name}, {"x_axis", graph.xAxis}, {"y_signals", graph.ySignals}};
            // Limits are optional so older projects keep auto-scaling.
            if (graph.xMin)
                graphJson["x_min"] = *graph.xMin;
            if (graph.xMax)
                graphJson["x_max"] = *graph.xMax;
            if (graph.yMin)
                graphJson["y_min"] = *graph.yMin;
            if (graph.yMax)
                graphJson["y_max"] = *graph.yMax;
            s["graphs"].push_back(std::move(graphJson));
        }
        j["simulations"].push_back(s);
    }
    j["simulation_scripts"] = json::array();
    for (const auto &[id, report] : reports)
        j["simulation_scripts"].push_back({{"id", id}, {"name", report.name}, {"path", report.path}});
    return j.dump(2) + "\n";
}

static Orientation parseOrientation(const std::string &value) {
    static const std::map<std::string, Orientation> values = {
        {"r0", Orientation::r0}, {"r90", Orientation::r90}, {"r180", Orientation::r180}, {"r270", Orientation::r270},
        {"mx", Orientation::mx}, {"my", Orientation::my},   {"m45", Orientation::m45},   {"m135", Orientation::m135}};
    auto it = values.find(value);
    if (it == values.end())
        throw ProjectError("project.orientation", "Unknown orientation: " + value);
    return it->second;
}

static const char *pinDirectionName(PinDirection direction) {
    switch (direction) {
    case PinDirection::left:
        return "left";
    case PinDirection::right:
        return "right";
    case PinDirection::up:
        return "up";
    case PinDirection::down:
        return "down";
    }
    throw ProjectError("project.pin_direction", "Unknown pin direction value");
}

static PinDirection parsePinDirection(const std::string &value) {
    static const std::map<std::string, PinDirection> values = {{"left", PinDirection::left},
                                                               {"right", PinDirection::right},
                                                               {"up", PinDirection::up},
                                                               {"down", PinDirection::down}};
    auto it = values.find(value);
    return it == values.end() ? PinDirection::right : it->second;
}

const char *portDirectionName(PortDirection direction) {
    switch (direction) {
    case PortDirection::input:
        return "input";
    case PortDirection::output:
        return "output";
    case PortDirection::inout:
        return "inout";
    case PortDirection::power:
        return "power";
    case PortDirection::ground:
        return "ground";
    }
    throw ProjectError("project.port_direction", "Unknown port direction value");
}

const char *portSideName(PortSide side) {
    switch (side) {
    case PortSide::left:
        return "left";
    case PortSide::right:
        return "right";
    case PortSide::top:
        return "top";
    case PortSide::bottom:
        return "bottom";
    }
    throw ProjectError("project.port_side", "Unknown port side value");
}

std::optional<PortDirection> parsePortDirection(const std::string &value) {
    static const std::map<std::string, PortDirection> values = {
        {"input", PortDirection::input}, {"output", PortDirection::output}, {"inout", PortDirection::inout},
        {"power", PortDirection::power}, {"ground", PortDirection::ground},
    };
    const auto it = values.find(value);
    if (it == values.end())
        return std::nullopt;
    return it->second;
}

std::optional<PortSide> parsePortSide(const std::string &value) {
    static const std::map<std::string, PortSide> values = {
        {"left", PortSide::left},
        {"right", PortSide::right},
        {"top", PortSide::top},
        {"bottom", PortSide::bottom},
    };
    const auto it = values.find(value);
    if (it == values.end())
        return std::nullopt;
    return it->second;
}

Project Project::deserialize(const std::string &text) {
    try {
        auto j = json::parse(text);
        if (j.at("format") != "ice-project")
            throw ProjectError("project.format", "Unsupported project format");
        if (j.at("schema_version") != schemaVersion)
            throw ProjectError("project.schema", "Unsupported project schema version");
        Project p;
        p.identity = j.at("identity");
        p.name = j.at("name");
        p.grid = j.at("grid");
        p.units.nanometers_per_unit = j.at("units").at("nanometers_per_unit");
        p.units.denominator = j.at("units").at("denominator");
        if (j.contains("package_dependencies")) {
            for (const auto &dependency : j.at("package_dependencies")) {
                const auto name = dependency.get<std::string>();
                if (!name.empty() && std::find(p.packageDependencies.begin(), p.packageDependencies.end(), name) ==
                                         p.packageDependencies.end())
                    p.packageDependencies.push_back(name);
            }
        }
        for (const auto &c : j.at("schematic_cells")) {
            SchematicCell cell{c.at("id"), c.at("name")};
            cell.pdk = c.value("pdk", std::string{genericPdkId});
            if (cell.pdk.empty())
                cell.pdk = genericPdkId;
            cell.textSchematic = c.value("text_schematic", false);
            cell.spiceText = c.value("spice_text", std::string{});
            if (c.contains("parameters")) {
                for (const auto &parameter : c.at("parameters"))
                    cell.parameters.push_back({parameter.at("id"), parameter.at("name"), parameter.at("value")});
            }
            for (const auto &s : c.at("symbols")) {
                Symbol symbol{s.at("id"), s.at("name"), s.at("artwork")};
                symbol.sourceCell = s.value("cell", ObjectId{0});
                if (s.contains("label") && s.at("label").is_object()) {
                    const auto &label = s.at("label");
                    symbol.label = SymbolLabel{label.value("x", Coordinate{0}), label.value("y", Coordinate{0})};
                }
                for (const auto &pin : s.at("pins"))
                    symbol.pins.push_back({pin.at("id"), pin.at("name"), pin.at("x"), pin.at("y"),
                                           parsePinDirection(pin.value("direction", "right"))});
                cell.symbols.emplace(symbol.id, std::move(symbol));
            }
            for (const auto &i : c.at("instances")) {
                SchematicInstance instance{i.at("id"), i.at("symbol"), i.at("name"),
                                           i.at("x"),  i.at("y"),      parseOrientation(i.at("orientation"))};
                if (i.contains("parameters")) {
                    for (const auto &parameter : i.at("parameters"))
                        instance.parameters.push_back(
                            {parameter.at("id"), parameter.at("name"), parameter.at("value")});
                }
                cell.instances.emplace(instance.id, std::move(instance));
            }
            for (const auto &n : c.at("nets"))
                cell.nets.emplace(n.at("id"), Net{n.at("id"), n.at("name"), n.at("pins").get<std::vector<ObjectId>>(),
                                                  n.value("x", 0), n.value("y", 0)});
            p.schematicCells.emplace(cell.id, std::move(cell));
        }
        for (const auto &l : j.at("layers"))
            p.layers.emplace(l.at("id"), Layer{l.at("id"), l.at("name"), l.at("number"), l.value("visible", true),
                                               l.value("layout", ObjectId{0}), l.value("pdk", std::string{})});
        for (const auto &c : j.at("layout_cells")) {
            LayoutCell cell{c.at("id"), c.at("name")};
            cell.pdk = c.value("pdk", std::string{genericPdkId});
            if (cell.pdk.empty())
                cell.pdk = genericPdkId;
            cell.library = c.value("library", false);
            cell.device = c.value("device", std::string{});
            cell.generatedBy = c.value("generated_by", ObjectId{0});
            if (c.contains("parameters")) {
                for (const auto &parameter : c.at("parameters"))
                    cell.parameters.push_back({parameter.at("id"), parameter.at("name"), parameter.at("value")});
            }
            for (const auto &s : c.at("shapes")) {
                Shape shape{s.at("id"),
                            s.at("layer"),
                            s.contains("net") ? std::optional<ObjectId>(s.at("net")) : std::nullopt,
                            Rect{s.at("rectangle").at("x"), s.at("rectangle").at("y"), s.at("rectangle").at("width"),
                                 s.at("rectangle").at("height")},
                            {}};
                if (s.contains("polygon"))
                    for (const auto &point : s.at("polygon"))
                        shape.polygon.push_back({point.at("x"), point.at("y")});
                if (s.contains("centerline"))
                    for (const auto &point : s.at("centerline"))
                        shape.centerline.push_back({point.at("x"), point.at("y")});
                if (s.contains("segment_layers"))
                    for (const auto &layer : s.at("segment_layers"))
                        shape.segmentLayers.push_back(layer.get<ObjectId>());
                if (s.contains("point_vias"))
                    for (const auto &entry : s.at("point_vias")) {
                        std::vector<ObjectId> vias;
                        for (const auto &layer : entry)
                            vias.push_back(layer.get<ObjectId>());
                        shape.pointVias.push_back(std::move(vias));
                    }
                shape.width = s.value("path_width", Coordinate{0});
                shape.text = s.value("text", std::string{});
                shape.name = s.value("name", std::string{});
                cell.shapes.emplace(shape.id, shape);
            }
            for (const auto &net : c.at("nets"))
                cell.nets.emplace(net.at("id"), LayoutNet{net.at("id"), net.at("name")});
            for (const auto &port : c.at("ports")) {
                Port value{port.at("id"), port.at("name"), port.at("layer"),
                           Rect{port.at("rectangle").at("x"), port.at("rectangle").at("y"),
                                port.at("rectangle").at("width"), port.at("rectangle").at("height")}};
                const auto direction = parsePortDirection(port.at("direction").get<std::string>());
                const auto side = parsePortSide(port.at("side").get<std::string>());
                if (!direction || !side)
                    throw ProjectError("project.port", "Layout port has an unknown direction or side");
                value.direction = *direction;
                value.side = *side;
                value.net = port.at("net").is_null() ? ObjectId{0} : port.at("net").get<ObjectId>();
                value.label = port.at("label").get<std::string>();
                value.visible = port.at("visible").get<bool>();
                value.locked = port.at("locked").get<bool>();
                value.sourceInstance = port.at("source_instance").get<ObjectId>();
                value.sourcePort = port.at("source_port").get<ObjectId>();
                cell.ports.emplace(value.id, std::move(value));
            }
            for (const auto &i : c.at("instances"))
                cell.instances.emplace(i.at("id"), LayoutInstance{i.at("id"), i.at("cell"), i.at("name"), i.at("x"),
                                                                  i.at("y"), parseOrientation(i.at("orientation"))});
            if (c.contains("planned_devices")) {
                for (const auto &d : c.at("planned_devices")) {
                    PlannedDevice device;
                    device.id = d.at("id");
                    device.name = d.value("name", std::string{});
                    device.schematicInstance = d.value("schematic_instance", ObjectId{0});
                    device.x = d.value("x", Coordinate{0});
                    device.y = d.value("y", Coordinate{0});
                    device.width = d.value("width", Coordinate{0});
                    device.height = d.value("height", Coordinate{0});
                    const auto deviceId = device.id;
                    cell.plannedDevices.emplace(deviceId, std::move(device));
                }
            }
            if (c.contains("planned_connections")) {
                for (const auto &connection : c.at("planned_connections")) {
                    PlannedConnection planned;
                    planned.id = connection.at("id");
                    planned.from = connection.at("from").get<ObjectId>();
                    planned.to = connection.at("to").get<ObjectId>();
                    planned.net = connection.at("net").get<ObjectId>();
                    const auto connectionId = planned.id;
                    cell.plannedConnections.emplace(connectionId, std::move(planned));
                }
            }
            p.layoutCells.emplace(cell.id, std::move(cell));
        }
        if (j.contains("procedural_layouts")) {
            for (const auto &layout : j.at("procedural_layouts")) {
                ProceduralLayout entry;
                entry.id = layout.at("id");
                entry.name = layout.at("name");
                entry.pdk = layout.value("pdk", std::string{genericPdkId});
                if (entry.pdk.empty())
                    entry.pdk = genericPdkId;
                entry.script = layout.value("script", std::string{});
                entry.generatedCell = layout.value("generated_cell", ObjectId{0});
                const auto layoutId = entry.id;
                p.proceduralLayouts.emplace(layoutId, std::move(entry));
            }
        }
        if (j.contains("simulations")) {
            for (const auto &s : j.at("simulations")) {
                SimulationItem sim;
                sim.id = s.at("id");
                sim.name = s.at("name");
                sim.schematic = s.value("schematic", ObjectId{0});
                sim.simulator = s.value("simulator", std::string{"ngspice"});
                sim.analysis = s.value("analysis", std::string{"transient"});
                sim.stop = s.value("stop", 1.0);
                sim.step = s.value("step", 0.01);
                sim.use_initial_conditions = s.value("use_initial_conditions", false);
                sim.directives = s.value("directives", std::string{});
                sim.captureSignals = s.value("signals", std::string{});
                std::vector<std::string> legacySignals;
                if (s.contains("graphs")) {
                    for (const auto &g : s.at("graphs")) {
                        SimulationGraph graph;
                        graph.id = g.at("id");
                        graph.name = g.value("name", std::string{"Graph"});
                        graph.xAxis = g.value("x_axis", std::string{"time"});
                        if (g.contains("y_signals") && g.at("y_signals").is_array()) {
                            for (const auto &signal : g.at("y_signals").get<std::vector<std::string>>()) {
                                if (!signal.empty() && std::find(graph.ySignals.begin(), graph.ySignals.end(),
                                                                 signal) == graph.ySignals.end())
                                    graph.ySignals.push_back(signal);
                            }
                        } else {
                            const auto legacyYAxis = g.value("y_axis", std::string{});
                            if (!legacyYAxis.empty())
                                graph.ySignals.push_back(legacyYAxis);
                        }
                        // Older projects stored probe lists per graph. Fold
                        // them into the simulation-level captured-signal list.
                        if (g.contains("nodes")) {
                            for (const auto &node : g.at("nodes").get<std::vector<std::string>>()) {
                                const auto signal = trimCopy(node);
                                if (!signal.empty() && std::find(legacySignals.begin(), legacySignals.end(), signal) ==
                                                           legacySignals.end())
                                    legacySignals.push_back(signal);
                            }
                        }
                        if (g.contains("x_min") && !g.at("x_min").is_null())
                            graph.xMin = g.at("x_min").get<double>();
                        if (g.contains("x_max") && !g.at("x_max").is_null())
                            graph.xMax = g.at("x_max").get<double>();
                        if (g.contains("y_min") && !g.at("y_min").is_null())
                            graph.yMin = g.at("y_min").get<double>();
                        if (g.contains("y_max") && !g.at("y_max").is_null())
                            graph.yMax = g.at("y_max").get<double>();
                        sim.graphs.push_back(std::move(graph));
                    }
                }
                if (sim.captureSignals.empty() && !legacySignals.empty())
                    sim.captureSignals = joinedSignalList(legacySignals);
                p.simulations.emplace(sim.id, std::move(sim));
            }
        }
        const auto *scripts = j.contains("simulation_scripts") ? &j.at("simulation_scripts")
                                                               : (j.contains("reports") ? &j.at("reports") : nullptr);
        if (scripts != nullptr) {
            for (const auto &report : *scripts) {
                ProjectReport entry;
                entry.id = report.at("id");
                entry.name = report.at("name");
                entry.path = report.at("path");
                p.reports.emplace(entry.id, std::move(entry));
            }
        }
        auto diagnostics = p.validate();
        if (!diagnostics.empty())
            throw ProjectError(diagnostics.front().code, diagnostics.front().message);
        return p;
    } catch (const ProjectError &) {
        throw;
    } catch (const std::exception &e) {
        throw ProjectError("project.parse", e.what());
    }
}

namespace {

std::filesystem::path projectRoot(const std::filesystem::path &path) {
    if (path.empty())
        throw ProjectError("project.path", "Project path must not be empty");
    std::error_code status;
    if (std::filesystem::is_directory(path, status) && !status)
        return path;
    if (path.extension() == ".icepro" || path.filename() == "project.icepro")
        return path.parent_path();
    return path;
}

std::string normalizedRoot(const std::filesystem::path &path) {
    std::error_code status;
    const auto canonical = std::filesystem::weakly_canonical(path, status);
    return (status ? path.lexically_normal() : canonical).string();
}

std::string projectRevision(const std::filesystem::path &root) {
    std::vector<std::filesystem::path> files;
    const auto addDirectory = [&](const std::filesystem::path &directory, const std::string &extension) {
        std::error_code status;
        if (!std::filesystem::is_directory(directory, status) || status)
            return;
        for (std::filesystem::directory_iterator it(directory, status), end; !status && it != end;
             it.increment(status)) {
            if (it->is_regular_file() && it->path().extension() == extension)
                files.push_back(it->path());
        }
    };
    files.push_back(root / "project.icepro");
    addDirectory(root / "sch", ".icesch");
    addDirectory(root / "lay", ".icelay");
    addDirectory(root / "sim", ".icesim");
    std::sort(files.begin(), files.end());

    constexpr std::uint64_t offset = 1469598103934665603ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    std::uint64_t hash = offset;
    const auto mix = [&](const std::string &text) {
        for (const unsigned char character : text) {
            hash ^= character;
            hash *= prime;
        }
        hash ^= 0xff;
        hash *= prime;
    };
    std::error_code status;
    for (const auto &file : files) {
        mix(file.lexically_relative(root).generic_string());
        const auto size = std::filesystem::file_size(file, status);
        mix(status ? std::string("missing") : std::to_string(size));
        const auto modified = std::filesystem::last_write_time(file, status);
        mix(status ? std::string("missing") : std::to_string(modified.time_since_epoch().count()));
    }
    std::ostringstream text;
    text << std::hex << hash;
    return text.str();
}

class ProjectLock {
  public:
    ProjectLock(const std::filesystem::path &root, int operation) {
#ifndef _WIN32
        std::error_code status;
        std::filesystem::create_directories(root / "build", status);
        const auto path = root / "build" / ".ice-save.lock";
        descriptor_ = ::open(path.c_str(), O_CREAT | O_RDWR, 0666);
        if (descriptor_ >= 0) {
            while (::flock(descriptor_, operation) != 0 && errno == EINTR) {
            }
        }
#else
        (void)root;
        (void)operation;
#endif
    }
    ~ProjectLock() {
#ifndef _WIN32
        if (descriptor_ >= 0) {
            ::flock(descriptor_, LOCK_UN);
            ::close(descriptor_);
        }
#endif
    }
    ProjectLock(const ProjectLock &) = delete;
    ProjectLock &operator=(const ProjectLock &) = delete;

  private:
#ifndef _WIN32
    int descriptor_ = -1;
#endif
};

class ProjectSaveLock : public ProjectLock {
  public:
    explicit ProjectSaveLock(const std::filesystem::path &root) : ProjectLock(root, LOCK_EX) {}
};

class ProjectReadLock : public ProjectLock {
  public:
    explicit ProjectReadLock(const std::filesystem::path &root) : ProjectLock(root, LOCK_SH) {}
};

std::string recoveryHint() {
    return " Open with recovery=true or run 'ice-cli project-repair <project>' to quarantine the invalid file "
           "instead of deleting it.";
}

std::string safeFileStem(const std::string &name) {
    std::string stem;
    stem.reserve(name.size());
    for (const unsigned char character : name) {
        if (std::isalnum(character) || character == '-' || character == '_' || character == ' ' || character == '.')
            stem.push_back(static_cast<char>(character));
        else
            stem.push_back('_');
    }
    while (!stem.empty() && stem.front() == '.')
        stem.erase(stem.begin());
    if (stem.empty())
        stem = "cell";
    return stem;
}

json readJsonFile(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        throw ProjectError("project.load", "Unable to open file: " + path.string());
    try {
        return json::parse(stream);
    } catch (const std::exception &error) {
        throw ProjectError("project.parse", "Unable to parse " + path.string() + ": " + error.what());
    }
}

void writeJsonFile(const std::filesystem::path &path, const json &value) {
    // Write to a sibling temporary file and rename it into place. Readers then
    // see either the previous complete file or the new complete file, never a
    // truncated one while another process is saving.
    auto temporary = path;
    temporary += ".tmp";
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream)
        throw ProjectError("project.save", "Unable to open file for writing: " + path.string());
    stream << value.dump(2) << "\n";
    stream.close();
    if (!stream)
        throw ProjectError("project.save", "Unable to write file: " + path.string());
    std::error_code status;
    std::filesystem::rename(temporary, path, status);
    if (status) {
        // Windows does not replace an existing target with std::filesystem::rename.
        status.clear();
        std::filesystem::remove(path, status);
        status.clear();
        std::filesystem::rename(temporary, path, status);
    }
    if (status) {
        std::filesystem::remove(temporary, status);
        throw ProjectError("project.save", "Unable to replace file atomically: " + path.string());
    }
}

void prepareDirectory(const std::filesystem::path &path, const std::string &extension) {
    std::filesystem::create_directories(path);
    std::error_code status;
    for (std::filesystem::directory_iterator iterator(path, status), end; !status && iterator != end;
         iterator.increment(status)) {
        if (!iterator->is_regular_file())
            continue;
        if (iterator->path().extension() == extension)
            std::filesystem::remove(iterator->path(), status);
        status.clear();
    }
}

void ensureGitignore(const std::filesystem::path &root) {
    const auto path = root / ".gitignore";
    std::string text;
    {
        std::ifstream input(path);
        if (input)
            text.assign(std::istreambuf_iterator<char>(input), {});
    }
    const auto removeLine = [&](const std::string &line) {
        std::istringstream input(text);
        std::string current;
        std::string updated;
        while (std::getline(input, current)) {
            if (current == line)
                continue;
            updated += current + "\n";
        }
        text = std::move(updated);
    };
    const auto ensureLine = [&](const std::string &line) {
        if (text.find(line + "\n") == std::string::npos && text != line)
            text += (text.empty() || text.back() == '\n' ? "" : "\n") + line + "\n";
    };
    // Simulation scripts are project source files, not generated output. Older
    // projects may have used reports/ before the directory was renamed.
    removeLine("reports/");
    removeLine("reports");
    removeLine("scripts/");
    removeLine("scripts");
    ensureLine("build/");
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        throw ProjectError("project.save", "Unable to write .gitignore");
    output << text;
}

std::string generatedAgentsGuidance() {
    std::ostringstream output;
    output << "<!-- ICE generated guidance: begin (version 2) -->\n"
           << "This directory is an ICE project.\n\n"
           << "- `project.icepro` contains project metadata.\n"
           << "- `sch/*.icesch` contains schematic cells.\n"
           << "- `lay/*.icelay` contains layout cells and procedural layouts.\n"
           << "- `sim/*.icesim` contains simulations.\n"
           << "- `scripts/*.py` contains project-level simulation scripts.\n"
           << "- `build/` contains generated simulation results.\n\n"
           << "Never write or edit the split project files (`sch/*.icesch`, `lay/*.icelay`, "
              "`sim/*.icesim`) by hand. Use the ICE MCP, CLI, or GUI tools. If a project fails to "
              "load, use `project_repair` or `project_open` with recovery instead of deleting files "
              "with the shell.\n\n"
           << "At the start of a session, call `ice_version` (or `ice-cli --version`) and confirm "
              "that the features you need are present. If `project_repair`, `project_migrate`, "
              "`simulation_measure`, or text-schematic support is missing, stop and ask the user to "
              "rebuild and restart ICE instead of working around the missing tools. Restart a "
              "long-lived MCP server after installing a new build.\n\n"
           << "When the user asks you to make a schematic or circuit, use this typical workflow:\n\n"
           << "1. Clarify the expectations (waveforms, gain, bandwidth, settling, power, etc.). If "
              "important or large schematic specifications are unclear, ask the user before drawing. "
              "Examples include supply/voltage levels, topology, device types and sizes, performance "
              "targets, external loads, constraints, and required simulations.\n"
           << "2. Before drawing, open or create the project and inspect the available PDKs, model "
              "libraries, ngspice, and Python with `project_summary`, `ice_doctor`, and `pdk_info` "
              "(or `ice-cli doctor` / `ice-cli pdk-info`). Do not guess an absolute PDK `.lib` "
              "path; `pdk_info` returns the package setup. Simulation PDKs come only from ICE "
              "packages; do not use `~/.volare`, `~/.ciel`, `PDK_ROOT`, or `PDK`. If a package is "
              "missing, install it with `ice-pkg`; if its OSDI models are missing, run "
              "`ice-cli pdk-build <pdk>`. Confirm that the project's PDK reports "
              "`simulation_ready: true` and prefer a ready PDK when the project does not already "
              "select one. If no installed PDK is ready, you may still create the requested "
              "schematic and report the blocker, or ask the user whether to install/build a PDK or "
              "use an explicitly labeled behavioral model. If the user asked for transistor-level "
              "simulation, do not silently replace it with a behavioral model.\n"
           << "3. Prefer a drawn canvas schematic over a text SPICE schematic. In MCP use "
              "`schematic_create`, `schematic_add_symbol` with pins, `schematic_add_instance`, and "
              "`schematic_add_net`; inspect the result with `schematic_info`. In the CLI use the equivalent "
              "`schematic-add-*` and `schematic-info` commands. `schematic_create` creates a drawn cell; "
              "`schematic_create(kind=\"text\")`, `schematic_create_text`, and `schematic_add_text` "
              "create a text SPICE cell. Use text SPICE only when the user explicitly asks for text, "
              "provides an existing SPICE netlist, or the canvas tools are unavailable; state the "
              "reason. Canvas schematics keep connectivity, layout, LVS, and simulation workflows "
              "available.\n"
           << "4. Create the simulation and choose the signals to capture. Use `simulation_measure` "
              "(or `ice-cli simulation-measure`) for numerical checks; `simulation_result` and "
              "`simulation_signals` expose trace statistics and available signals.\n"
           << "5. Create graphs that showcase the schematic behavior: input/output waveforms, frequency "
              "response, transfer curves, settling, or other views needed to judge the expectations.\n"
           << "6. Choose the simplest simulation style that fits the task:\n"
           << "   - Use a normal simulation (`simulation-add` + `simulation-run`) for a single "
              "straightforward run when a few standard graphs are enough.\n"
           << "   - Prefer a simulation script (`script-add` / `script-write` / `script-run`) when "
              "there are useful circuit metrics to calculate (gain, bandwidth, phase margin, slew rate, "
              "settling, overshoot, power, etc.) or when you need an array of simulations such as "
              "parameter sweeps or corners. Scripts can compute metrics, compare results with the "
              "expectations, and generate GUI graphs.\n"
           << "   - Otherwise use a normal simulation for simplicity.\n"
           << "7. Run the simulation or script and inspect its diagnostics before editing blindly: "
              "`simulation_run` returns state, error, netlist context, trace details, and available "
              "signals. Use `simulation_result`/`simulation_signals` for stored runs. If ngspice or the "
              "PDK fails to initialize, return to `ice_doctor` (with the schematic or simulation name) and "
              "fix the environment first. Compare the results with the expectations and tweak the "
              "schematic if needed. Continue the loop until the expectations are met.\n"
           << "8. If you substitute a behavioral or RC approximation for a transistor-level circuit, say "
              "so explicitly and ask whether that is acceptable. A successful transient run is not proof "
              "of oscillation: use `simulation_measure` and report zero crossings, frequency or period, "
              "amplitude, and startup behavior before claiming the circuit works.\n\n"
           << "Use `project_migrate` after upgrading ICE to refresh this generated guidance. Existing "
              "user text outside the generated markers is preserved.\n\n"
           << "For scripted work, use `ice-cli` or `ice-mcp`. Successful `ice-mcp` project mutations "
              "are saved to disk automatically.\n"
           << "Run stored simulations with `ice-cli simulation-run <project> <simulation>`; "
              "its run is written under `build/` and loaded by the GUI graph tabs.\n\n"
           << "Use the ICE GUI, `ice-cli`, `icepy`, or `ice-mcp` to inspect and edit the project.\n"
           << "<!-- ICE generated guidance: end -->\n";
    return output.str();
}

bool ensureAgentsFile(const std::filesystem::path &root, const std::string &projectName) {
    const auto path = root / "AGENTS.md";
    const auto guidance = generatedAgentsGuidance();
    const std::string beginMarker = "<!-- ICE generated guidance: begin";
    const std::string endMarker = "<!-- ICE generated guidance: end -->";

    std::string text;
    std::error_code status;
    if (std::filesystem::exists(path, status) && !status) {
        std::ifstream input(path, std::ios::binary);
        if (!input)
            throw ProjectError("project.save", "Unable to read AGENTS.md");
        text.assign(std::istreambuf_iterator<char>(input), {});
    } else {
        text = "# " + projectName + "\n\n";
    }

    const auto begin = text.find(beginMarker);
    const auto end = text.find(endMarker);
    if (begin != std::string::npos && end != std::string::npos && end > begin) {
        auto replaceEnd = end + endMarker.size();
        if (replaceEnd < text.size() && text[replaceEnd] == '\n')
            ++replaceEnd;
        text.replace(begin, replaceEnd - begin, guidance);
    } else {
        if (!text.empty() && text.back() != '\n')
            text.push_back('\n');
        if (!text.empty())
            text.push_back('\n');
        text += guidance;
    }

    std::ifstream existing(path, std::ios::binary);
    const std::string previous((std::istreambuf_iterator<char>(existing)), {});
    if (existing && previous == text)
        return false;

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        throw ProjectError("project.save", "Unable to write AGENTS.md");
    output << text;
    return true;
}

std::string uniqueStem(const std::string &name, ObjectId id, std::set<std::string> &used) {
    auto stem = safeFileStem(name);
    if (used.insert(stem).second)
        return stem;
    auto candidate = stem + "-" + std::to_string(id);
    if (used.insert(candidate).second)
        return candidate;
    auto suffix = 2;
    while (!used.insert(candidate = stem + "-" + std::to_string(id) + "-" + std::to_string(suffix++)).second) {
    }
    return candidate;
}

} // namespace

void Project::save(const std::filesystem::path &path) const {
    auto diagnostics = validate();
    if (!diagnostics.empty())
        throw ProjectError(diagnostics.front().code, diagnostics.front().message);

    const auto root = projectRoot(path);
    ProjectSaveLock saveLock(root);
    if (!sourceRevision_.empty() && normalizedRoot(sourceRoot_) == normalizedRoot(root)) {
        const auto currentRevision = projectRevision(root);
        if (currentRevision != sourceRevision_)
            throw ProjectError("project.changed",
                               "Project files changed on disk since this project was loaded (another ICE process "
                               "may have saved). Reload the project before saving, or open it again with recovery "
                               "if repair is needed.");
    }
    const auto full = json::parse(serialize());
    json projectJson = full;
    const auto schematicCells = full.at("schematic_cells");
    const auto layoutCells = full.at("layout_cells");
    const auto proceduralLayouts = full.at("procedural_layouts");
    const auto simulations = full.at("simulations");
    projectJson.erase("schematic_cells");
    projectJson.erase("layout_cells");
    projectJson.erase("procedural_layouts");
    projectJson.erase("simulations");

    std::filesystem::create_directories(root);
    const auto schematicDir = root / "sch";
    const auto layoutDir = root / "lay";
    const auto simulationDir = root / "sim";
    prepareDirectory(schematicDir, ".icesch");
    prepareDirectory(layoutDir, ".icelay");
    prepareDirectory(simulationDir, ".icesim");
    std::filesystem::create_directories(root / "scripts");
    std::filesystem::create_directories(root / "build");

    writeJsonFile(root / "project.icepro", projectJson);
    ensureGitignore(root);
    ensureAgentsFile(root, name);

    std::set<std::string> usedSchematics;
    for (const auto &cell : schematicCells) {
        const auto id = cell.at("id").get<ObjectId>();
        const auto stem = uniqueStem(cell.value("name", std::string{}), id, usedSchematics);
        writeJsonFile(schematicDir / (stem + ".icesch"),
                      json{{"format", "ice-schematic"}, {"schema_version", 1}, {"cell", cell}});
    }

    std::set<std::string> usedLayouts;
    for (const auto &cell : layoutCells) {
        const auto id = cell.at("id").get<ObjectId>();
        const auto stem = uniqueStem(cell.value("name", std::string{}), id, usedLayouts);
        writeJsonFile(layoutDir / (stem + ".icelay"),
                      json{{"format", "ice-layout"}, {"schema_version", 2}, {"layout", cell}});
    }
    for (const auto &layout : proceduralLayouts) {
        const auto id = layout.at("id").get<ObjectId>();
        const auto stem = uniqueStem(layout.value("name", std::string{}), id, usedLayouts);
        writeJsonFile(layoutDir / (stem + ".icelay"),
                      json{{"format", "ice-layout"}, {"schema_version", 2}, {"procedural_layout", layout}});
    }

    std::set<std::string> usedSimulations;
    for (const auto &simulation : simulations) {
        const auto id = simulation.at("id").get<ObjectId>();
        const auto stem = uniqueStem(simulation.value("name", std::string{}), id, usedSimulations);
        writeJsonFile(simulationDir / (stem + ".icesim"),
                      json{{"format", "ice-simulation"}, {"schema_version", 1}, {"simulation", simulation}});
    }
    sourceRoot_ = root;
    sourceRevision_ = projectRevision(root);
    spdlog::info("Saved project {}", (root / "project.icepro").string());
}

Project Project::load(const std::filesystem::path &path) { return load(path, {}, nullptr); }

Project Project::load(const std::filesystem::path &path, const ProjectLoadOptions &options, ProjectLoadReport *report) {
    const auto root = projectRoot(path);
    ProjectReadLock readLock(root);
    const auto metadataPath = root / "project.icepro";
    json full;
    try {
        full = readJsonFile(metadataPath);
    } catch (const std::exception &error) {
        throw ProjectError("project.format", "Unable to load project metadata '" + metadataPath.string() +
                                                 "': " + error.what() + recoveryHint());
    }
    if (full.value("format", std::string{}) != "ice-project")
        throw ProjectError("project.format", "Invalid project metadata '" + metadataPath.string() +
                                                 "': expected format 'ice-project'" + recoveryHint());
    full["schematic_cells"] = json::array();
    full["layout_cells"] = json::array();
    full["procedural_layouts"] = json::array();
    full["simulations"] = json::array();

    const auto quarantine = [&](const std::filesystem::path &file, const std::string &message) {
        auto relative = file.lexically_relative(root);
        if (relative.empty() || relative.string().starts_with(".."))
            relative = file.filename();
        auto directory = root / "build" / "recovery" / relative.parent_path();
        std::error_code status;
        std::filesystem::create_directories(directory, status);
        auto target = directory / relative.filename();
        for (int suffix = 1; std::filesystem::exists(target, status) && !status; ++suffix) {
            target = directory / (file.stem().string() + "." + std::to_string(suffix) + file.extension().string());
        }
        status.clear();
        std::filesystem::rename(file, target, status);
        if (status) {
            status.clear();
            std::filesystem::copy_file(file, target, std::filesystem::copy_options::overwrite_existing, status);
            if (!status)
                std::filesystem::remove(file, status);
        }
        if (report) {
            report->quarantinedFiles.push_back(target.string());
            report->warnings.push_back(message + (status ? " (quarantine failed: " + status.message() + ")"
                                                         : " (quarantined to " + target.string() + ")"));
        }
    };

    const auto readDirectory = [&](const std::filesystem::path &directory, const std::string &extension,
                                   const auto &consume) {
        std::vector<std::filesystem::path> files;
        std::error_code status;
        if (std::filesystem::is_directory(directory, status) && !status) {
            for (std::filesystem::directory_iterator iterator(directory, status), end; !status && iterator != end;
                 iterator.increment(status)) {
                if (iterator->is_regular_file() && iterator->path().extension() == extension)
                    files.push_back(iterator->path());
            }
        }
        std::sort(files.begin(), files.end());
        for (const auto &file : files) {
            try {
                consume(readJsonFile(file));
            } catch (const std::exception &error) {
                const auto relative = file.lexically_relative(root).string();
                const auto message = "Invalid project file '" + relative + "': " + error.what();
                if (!options.recover)
                    throw ProjectError("project.format", message + recoveryHint());
                quarantine(file, message);
            }
        }
    };

    readDirectory(root / "sch", ".icesch", [&](const json &document) {
        if (document.value("format", std::string{}) != "ice-schematic" || !document.contains("cell"))
            throw ProjectError("project.format",
                               "Invalid schematic file: expected an 'ice-schematic' envelope with a 'cell' object");
        full["schematic_cells"].push_back(document.at("cell"));
    });
    readDirectory(root / "lay", ".icelay", [&](const json &document) {
        if (document.value("format", std::string{}) != "ice-layout")
            throw ProjectError("project.format", "Invalid layout file: expected format 'ice-layout'");
        if (document.value("schema_version", 0) != 2)
            throw ProjectError("project.schema", "Unsupported layout schema version");
        if (document.contains("layout"))
            full["layout_cells"].push_back(document.at("layout"));
        else if (document.contains("procedural_layout"))
            full["procedural_layouts"].push_back(document.at("procedural_layout"));
        else
            throw ProjectError("project.format",
                               "Invalid layout file: expected a 'layout' or 'procedural_layout' object");
    });
    readDirectory(root / "sim", ".icesim", [&](const json &document) {
        if (document.value("format", std::string{}) != "ice-simulation" || !document.contains("simulation"))
            throw ProjectError(
                "project.format",
                "Invalid simulation file: expected an 'ice-simulation' envelope with a 'simulation' object");
        full["simulations"].push_back(document.at("simulation"));
    });

    auto project = deserialize(full.dump());
    project.sourceRoot_ = root;
    project.sourceRevision_ = projectRevision(root);
    return project;
}

ProjectSession::ProjectSession(Project project) : project_(std::move(project)), historyStates_{project_} {}
Project &ProjectSession::project() noexcept { return project_; }
const Project &ProjectSession::project() const noexcept { return project_; }
bool ProjectSession::dirty() const noexcept { return dirty_; }
const std::vector<std::string> &ProjectSession::history() const noexcept { return historyLabels_; }
std::size_t ProjectSession::historyPosition() const noexcept { return historyPosition_; }
void ProjectSession::setLastHistoryLabel(std::string label) {
    if (historyPosition_ != 0 && historyPosition_ <= historyLabels_.size())
        historyLabels_[historyPosition_ - 1] = std::move(label);
}
bool ProjectSession::jumpToHistory(std::size_t position) {
    if (position > historyPosition_ || position >= historyStates_.size())
        return false;
    if (position == historyPosition_)
        return true;
    project_ = historyStates_[position];
    historyPosition_ = position;
    dirty_ = true;
    notify();
    return true;
}
void ProjectSession::replaceProject(Project project) {
    project_ = std::move(project);
    dirty_ = true;
    notify();
}
void ProjectSession::markDirty() {
    dirty_ = true;
    notify();
}
void ProjectSession::subscribe(Observer observer) { observers_.push_back(std::move(observer)); }
void ProjectSession::beginTransaction(std::string label) {
    if (transactionSnapshot_)
        throw ProjectError("transaction.nested", "Nested transactions are not supported");
    transactionSnapshot_ = project_;
    transactionLabel_ = std::move(label);
}
void ProjectSession::commitTransaction() {
    if (!transactionSnapshot_)
        throw ProjectError("transaction.missing", "No transaction is active");
    if (project_.serialize() != transactionSnapshot_->serialize()) {
        historyLabels_.resize(historyPosition_);
        historyStates_.resize(historyPosition_ + 1);
        historyLabels_.push_back(transactionLabel_);
        historyStates_.push_back(project_);
        ++historyPosition_;
        dirty_ = true;
        notify();
    }
    transactionSnapshot_.reset();
    transactionLabel_.clear();
}
void ProjectSession::rollbackTransaction() {
    if (!transactionSnapshot_)
        throw ProjectError("transaction.missing", "No transaction is active");
    project_ = *transactionSnapshot_;
    transactionSnapshot_.reset();
    transactionLabel_.clear();
    notify();
}
void ProjectSession::execute(std::string label, const std::function<void(Project &)> &mutation) {
    beginTransaction(std::move(label));
    try {
        mutation(project_);
        auto diagnostics = project_.validate();
        if (!diagnostics.empty())
            throw ProjectError(diagnostics.front().code, diagnostics.front().message);
        commitTransaction();
    } catch (...) {
        rollbackTransaction();
        throw;
    }
}
bool ProjectSession::undo() {
    if (historyPosition_ == 0)
        return false;
    --historyPosition_;
    project_ = historyStates_[historyPosition_];
    dirty_ = true;
    notify();
    return true;
}
bool ProjectSession::redo() {
    if (historyPosition_ >= historyLabels_.size())
        return false;
    ++historyPosition_;
    project_ = historyStates_[historyPosition_];
    dirty_ = true;
    notify();
    return true;
}
void ProjectSession::save(const std::filesystem::path &path) {
    project_.save(path);
    dirty_ = false;
    notify();
}
void ProjectSession::notify() {
    for (const auto &observer : observers_)
        observer();
}
void SelectionModel::select(ObjectId id, std::vector<ObjectId> related) {
    primary_ = id;
    related_ = std::move(related);
}
void SelectionModel::clear() {
    primary_.reset();
    related_.clear();
}
std::optional<ObjectId> SelectionModel::primary() const noexcept { return primary_; }
const std::vector<ObjectId> &SelectionModel::related() const noexcept { return related_; }
std::string version() { return "0.1.0"; }

BuildInfo buildInfo() {
    BuildInfo info;
    info.version = version();
#ifdef ICE_BUILD_COMMIT
    info.commit = ICE_BUILD_COMMIT;
#else
    info.commit = "unknown";
#endif
#ifdef ICE_BUILD_TIME
    info.buildTime = ICE_BUILD_TIME;
#else
    info.buildTime = "unknown";
#endif
#ifdef ICE_BUILD_TYPE
    info.buildType = ICE_BUILD_TYPE;
#else
    info.buildType = "unknown";
#endif
    info.features = {"project_repair",      "project_recovery",    "project_migrate",    "schematic_add_pin",
                     "text_schematic",      "ngspice_log_capture", "simulation_metrics", "pdk_status",
                     "pdk_build",           "save_revision_guard", "layout_pins",        "layout_pin_hierarchy",
                     "layout_connectivity", "layout_lef_def",      "layout_pin_drc"};
    return info;
}
} // namespace ice
