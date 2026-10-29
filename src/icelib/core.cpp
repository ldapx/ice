#include "icelib/core.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <nlohmann/json.hpp>
#include <set>
#include <spdlog/spdlog.h>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

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
    auto onGrid = [this](Coordinate value) { return grid > 0 && value % grid == 0; };
    for (const auto &[cellId, cell] : schematicCells) {
        if (cell.pdk.empty() || cell.pdk.find_first_of("/\\") != std::string::npos)
            result.push_back({Diagnostic::Severity::error, "schematic.pdk", "Schematic PDK is invalid", cellId});
        for (const auto &[instanceId, instance] : cell.instances) {
            if (!cell.symbols.contains(instance.symbol))
                result.push_back({Diagnostic::Severity::error, "schematic.instance.symbol",
                                  "Instance references a missing symbol", instanceId});
            if (!onGrid(instance.x) || !onGrid(instance.y))
                result.push_back({Diagnostic::Severity::error, "schematic.instance.grid",
                                  "Instance position is off-grid", instanceId});
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
    for (const auto &[cellId, cell] : layoutCells) {
        for (const auto &[shapeId, shape] : cell.shapes) {
            if (!layers.contains(shape.layer))
                result.push_back(
                    {Diagnostic::Severity::error, "layout.shape.layer", "Shape references a missing layer", shapeId});
            if ((!shape.polygon.empty() && shape.polygon.size() < 3) ||
                (shape.polygon.empty() && !shape.rectangle.valid()))
                result.push_back({Diagnostic::Severity::error, "layout.shape.geometry",
                                  "Shape rectangle must have positive dimensions", shapeId});
            if (shape.polygon.empty() && (!onGrid(shape.rectangle.x) || !onGrid(shape.rectangle.y) ||
                                          !onGrid(shape.rectangle.width) || !onGrid(shape.rectangle.height)))
                result.push_back(
                    {Diagnostic::Severity::error, "layout.shape.grid", "Shape geometry is off-grid", shapeId});
        }
        for (const auto &[portId, port] : cell.ports) {
            if (!layers.contains(port.layer))
                result.push_back(
                    {Diagnostic::Severity::error, "layout.port.layer", "Port references a missing layer", portId});
            if (!port.rectangle.valid())
                result.push_back({Diagnostic::Severity::error, "layout.port.geometry",
                                  "Port rectangle must have positive dimensions", portId});
            if (!onGrid(port.rectangle.x) || !onGrid(port.rectangle.y) || !onGrid(port.rectangle.width) ||
                !onGrid(port.rectangle.height))
                result.push_back(
                    {Diagnostic::Severity::error, "layout.port.grid", "Port geometry is off-grid", portId});
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
            result.push_back({Diagnostic::Severity::error, "report.name", "Report name is required", reportId});
        else if (!reportNames.insert(report.name).second)
            result.push_back({Diagnostic::Severity::error, "report.name.duplicate",
                              "Report name is listed more than once: " + report.name, reportId});
        const auto normalized = std::filesystem::path(report.path).lexically_normal();
        const auto first = normalized.begin();
        if (report.path.empty() || normalized.empty() || normalized.is_absolute() || first == normalized.end() ||
            first->string() != "reports" || normalized.extension() != ".py")
            result.push_back({Diagnostic::Severity::error, "report.path",
                              "Report path must be a relative Python file inside reports/", reportId});
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
        j["layers"].push_back({{"id", id}, {"name", layer.name}, {"number", layer.number}, {"visible", layer.visible}});
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
            c["shapes"].push_back(s);
        }
        for (const auto &[pid, port] : cell.ports) {
            json p = {{"id", pid}, {"name", port.name}, {"layer", port.layer}, {"rectangle", json::object()}};
            put(p["rectangle"], port.rectangle);
            c["ports"].push_back(p);
        }
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
                {{"id", cid}, {"from", connection.from}, {"to", connection.to}, {"name", connection.name}});
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
    j["reports"] = json::array();
    for (const auto &[id, report] : reports)
        j["reports"].push_back({{"id", id}, {"name", report.name}, {"path", report.path}});
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
            p.layers.emplace(l.at("id"), Layer{l.at("id"), l.at("name"), l.at("number"), l.value("visible", true)});
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
                cell.shapes.emplace(shape.id, shape);
            }
            for (const auto &port : c.at("ports"))
                cell.ports.emplace(port.at("id"),
                                   Port{port.at("id"), port.at("name"), port.at("layer"),
                                        Rect{port.at("rectangle").at("x"), port.at("rectangle").at("y"),
                                             port.at("rectangle").at("width"), port.at("rectangle").at("height")}});
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
                    planned.from = connection.value("from", ObjectId{0});
                    planned.to = connection.value("to", ObjectId{0});
                    planned.name = connection.value("name", std::string{});
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
        if (j.contains("reports")) {
            for (const auto &report : j.at("reports")) {
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
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream)
        throw ProjectError("project.save", "Unable to open file for writing: " + path.string());
    stream << value.dump(2) << "\n";
    if (!stream)
        throw ProjectError("project.save", "Unable to write file: " + path.string());
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
    // Reports are project source files, not generated output. Older projects
    // may have been created when reports/ was ignored.
    removeLine("reports/");
    removeLine("reports");
    ensureLine("build/");
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        throw ProjectError("project.save", "Unable to write .gitignore");
    output << text;
}

void ensureAgentsFile(const std::filesystem::path &root, const std::string &projectName) {
    const auto path = root / "AGENTS.md";
    std::error_code status;
    if (std::filesystem::exists(path, status) && !status)
        return;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        throw ProjectError("project.save", "Unable to write AGENTS.md");
    output << "# " << projectName << "\n\n"
           << "This directory is an ICE project.\n\n"
           << "- `project.icepro` contains project metadata.\n"
           << "- `sch/*.icesch` contains schematic cells.\n"
           << "- `lay/*.icelay` contains layout cells and procedural layouts.\n"
           << "- `sim/*.icesim` contains simulations.\n"
           << "- `reports/*.py` contains project-level simulation reports.\n"
           << "- `build/` contains generated simulation results.\n\n"
           << "Use the ICE GUI, `ice-cli`, `icepy`, or `ice-mcp` to inspect and edit the project.\n";
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
    std::filesystem::create_directories(root / "reports");
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
                      json{{"format", "ice-layout"}, {"schema_version", 1}, {"layout", cell}});
    }
    for (const auto &layout : proceduralLayouts) {
        const auto id = layout.at("id").get<ObjectId>();
        const auto stem = uniqueStem(layout.value("name", std::string{}), id, usedLayouts);
        writeJsonFile(layoutDir / (stem + ".icelay"),
                      json{{"format", "ice-layout"}, {"schema_version", 1}, {"procedural_layout", layout}});
    }

    std::set<std::string> usedSimulations;
    for (const auto &simulation : simulations) {
        const auto id = simulation.at("id").get<ObjectId>();
        const auto stem = uniqueStem(simulation.value("name", std::string{}), id, usedSimulations);
        writeJsonFile(simulationDir / (stem + ".icesim"),
                      json{{"format", "ice-simulation"}, {"schema_version", 1}, {"simulation", simulation}});
    }
    spdlog::info("Saved project {}", (root / "project.icepro").string());
}

Project Project::load(const std::filesystem::path &path) {
    const auto root = projectRoot(path);
    auto full = readJsonFile(root / "project.icepro");
    if (full.value("format", std::string{}) != "ice-project")
        throw ProjectError("project.format", "Unsupported project format");
    full["schematic_cells"] = json::array();
    full["layout_cells"] = json::array();
    full["procedural_layouts"] = json::array();
    full["simulations"] = json::array();

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
        for (const auto &file : files)
            consume(readJsonFile(file));
    };

    readDirectory(root / "sch", ".icesch", [&](const json &document) {
        if (document.value("format", std::string{}) != "ice-schematic" || !document.contains("cell"))
            throw ProjectError("project.format", "Invalid schematic file");
        full["schematic_cells"].push_back(document.at("cell"));
    });
    readDirectory(root / "lay", ".icelay", [&](const json &document) {
        if (document.value("format", std::string{}) != "ice-layout")
            throw ProjectError("project.format", "Invalid layout file");
        if (document.contains("layout"))
            full["layout_cells"].push_back(document.at("layout"));
        else if (document.contains("procedural_layout"))
            full["procedural_layouts"].push_back(document.at("procedural_layout"));
        else
            throw ProjectError("project.format", "Invalid layout file");
    });
    readDirectory(root / "sim", ".icesim", [&](const json &document) {
        if (document.value("format", std::string{}) != "ice-simulation" || !document.contains("simulation"))
            throw ProjectError("project.format", "Invalid simulation file");
        full["simulations"].push_back(document.at("simulation"));
    });

    return deserialize(full.dump());
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
} // namespace ice
