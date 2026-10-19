#include "icelib/core.hpp"

#include <fstream>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <stdexcept>

namespace ice {
using json = nlohmann::ordered_json;

ProjectError::ProjectError(std::string code, std::string message)
    : std::runtime_error(std::move(message)), code_(std::move(code)) {}
const std::string& ProjectError::code() const noexcept { return code_; }

double DatabaseUnits::toNanometers(Coordinate value) const {
    return static_cast<double>(value) * static_cast<double>(nanometers_per_unit) /
           static_cast<double>(denominator);
}
bool DatabaseUnits::valid() const { return nanometers_per_unit > 0 && denominator > 0; }
bool Rect::valid() const { return width > 0 && height > 0; }

std::string orientationName(Orientation value) {
    static const char* names[] = {"r0", "r90", "r180", "r270", "mx", "my"};
    return names[static_cast<int>(value)];
}

ObjectId Project::nextId() {
    ObjectId highest = 0;
    auto update = [&highest](ObjectId id) { highest = std::max(highest, id); };
    for (const auto& [id, cell] : schematicCells) {
        update(id);
        for (const auto& [symbolId, symbol] : cell.symbols) {
            update(symbolId);
            for (const auto& pin : symbol.pins) update(pin.id);
        }
        for (const auto& [instanceId, instance] : cell.instances) {
            update(instanceId);
            for (const auto& parameter : instance.parameters) update(parameter.id);
        }
        for (const auto& [netId, net] : cell.nets) update(netId);
    }
    for (const auto& [id, _] : layers) highest = std::max(highest, id);
    for (const auto& [id, cell] : layoutCells) {
        update(id);
        for (const auto& [shapeId, shape] : cell.shapes) update(shapeId);
        for (const auto& [portId, port] : cell.ports) update(portId);
        for (const auto& [instanceId, instance] : cell.instances) update(instanceId);
    }
    return highest + 1;
}

Coordinate Project::snap(Coordinate value) const {
    if (grid <= 0) throw ProjectError("project.grid", "Grid spacing must be positive");
    const Coordinate remainder = value % grid;
    const Coordinate lower = value - remainder;
    const Coordinate upper = lower + grid;
    return remainder * 2 < grid ? lower : upper;
}

std::vector<Diagnostic> Project::validate() const {
    std::vector<Diagnostic> result;
    if (identity.empty()) result.push_back({Diagnostic::Severity::error, "project.identity", "Project identity is required", std::nullopt});
    if (name.empty()) result.push_back({Diagnostic::Severity::error, "project.name", "Project name is required", std::nullopt});
    if (!units.valid()) result.push_back({Diagnostic::Severity::error, "project.units", "Database units must be positive", std::nullopt});
    if (grid <= 0) result.push_back({Diagnostic::Severity::error, "project.grid", "Grid spacing must be positive", std::nullopt});
    auto onGrid = [this](Coordinate value) { return grid > 0 && value % grid == 0; };
    for (const auto& [cellId, cell] : schematicCells) {
        for (const auto& [instanceId, instance] : cell.instances) {
            if (!cell.symbols.contains(instance.symbol))
                result.push_back({Diagnostic::Severity::error, "schematic.instance.symbol", "Instance references a missing symbol", instanceId});
            if (!onGrid(instance.x) || !onGrid(instance.y))
                result.push_back({Diagnostic::Severity::error, "schematic.instance.grid", "Instance position is off-grid", instanceId});
        }
        for (const auto& [netId, net] : cell.nets) {
            for (auto pinId : net.pins) {
                bool found = false;
                for (const auto& [_, symbol] : cell.symbols)
                    for (const auto& pin : symbol.pins) found = found || pin.id == pinId;
                if (!found)
                    result.push_back({Diagnostic::Severity::error, "schematic.net.pin", "Net references a missing pin", netId});
            }
        }
        (void)cellId;
    }
    for (const auto& [cellId, cell] : layoutCells) {
        for (const auto& [shapeId, shape] : cell.shapes) {
            if (!layers.contains(shape.layer))
                result.push_back({Diagnostic::Severity::error, "layout.shape.layer", "Shape references a missing layer", shapeId});
            if ((!shape.polygon.empty() && shape.polygon.size() < 3) ||
                (shape.polygon.empty() && !shape.rectangle.valid()))
                result.push_back({Diagnostic::Severity::error, "layout.shape.geometry", "Shape rectangle must have positive dimensions", shapeId});
            if (shape.polygon.empty() &&
                (!onGrid(shape.rectangle.x) || !onGrid(shape.rectangle.y) ||
                 !onGrid(shape.rectangle.width) || !onGrid(shape.rectangle.height)))
                result.push_back({Diagnostic::Severity::error, "layout.shape.grid", "Shape geometry is off-grid", shapeId});
        }
        for (const auto& [portId, port] : cell.ports) {
            if (!layers.contains(port.layer))
                result.push_back({Diagnostic::Severity::error, "layout.port.layer", "Port references a missing layer", portId});
            if (!port.rectangle.valid())
                result.push_back({Diagnostic::Severity::error, "layout.port.geometry", "Port rectangle must have positive dimensions", portId});
            if (!onGrid(port.rectangle.x) || !onGrid(port.rectangle.y) ||
                !onGrid(port.rectangle.width) || !onGrid(port.rectangle.height))
                result.push_back({Diagnostic::Severity::error, "layout.port.grid", "Port geometry is off-grid", portId});
        }
        (void)cellId;
    }
    return result;
}

static const char* pinDirectionName(PinDirection direction);
static PinDirection parsePinDirection(const std::string& value);

static void put(json& j, const Pin& v) {
    j = {{"id", v.id}, {"name", v.name}, {"x", v.x}, {"y", v.y},
         {"direction", pinDirectionName(v.direction)}};
}
static void put(json& j, const Parameter& v) { j = {{"id", v.id}, {"name", v.name}, {"value", v.value}}; }
static void put(json& j, const Rect& v) { j = {{"x", v.x}, {"y", v.y}, {"width", v.width}, {"height", v.height}}; }

std::string Project::serialize() const {
    json j;
    j["format"] = "ice-project";
    j["schema_version"] = schemaVersion;
    j["identity"] = identity;
    j["name"] = name;
    j["grid"] = grid;
    j["units"] = {{"nanometers_per_unit", units.nanometers_per_unit}, {"denominator", units.denominator}};
    j["schematic_cells"] = json::array();
    for (const auto& [id, cell] : schematicCells) {
        json c = {{"id", id}, {"name", cell.name}, {"symbols", json::array()}, {"instances", json::array()}, {"nets", json::array()}};
        for (const auto& [sid, symbol] : cell.symbols) {
            json s = {{"id", sid}, {"name", symbol.name}, {"artwork", symbol.artwork}, {"pins", json::array()}};
            for (const auto& pin : symbol.pins) { json p; put(p, pin); s["pins"].push_back(p); }
            c["symbols"].push_back(s);
        }
        for (const auto& [iid, instance] : cell.instances)
            c["instances"].push_back({{"id", iid}, {"symbol", instance.symbol}, {"name", instance.name}, {"x", instance.x}, {"y", instance.y}, {"orientation", orientationName(instance.orientation)}});
        for (const auto& [nid, net] : cell.nets)
            c["nets"].push_back({{"id", nid}, {"name", net.name}, {"pins", net.pins}, {"x", net.x}, {"y", net.y}});
        j["schematic_cells"].push_back(c);
    }
    j["layers"] = json::array();
    for (const auto& [id, layer] : layers)
        j["layers"].push_back({{"id", id}, {"name", layer.name}, {"number", layer.number}, {"visible", layer.visible}});
    j["layout_cells"] = json::array();
    for (const auto& [id, cell] : layoutCells) {
        json c = {{"id", id}, {"name", cell.name}, {"shapes", json::array()}, {"ports", json::array()}, {"instances", json::array()}};
        for (const auto& [sid, shape] : cell.shapes) {
            json s = {{"id", sid}, {"layer", shape.layer}, {"rectangle", json::object()}};
            if (shape.net) s["net"] = *shape.net;
            put(s["rectangle"], shape.rectangle);
            if (!shape.polygon.empty()) {
                s["polygon"] = json::array();
                for (const auto& point : shape.polygon)
                    s["polygon"].push_back({{"x", point.x}, {"y", point.y}});
            }
            c["shapes"].push_back(s);
        }
        for (const auto& [pid, port] : cell.ports) {
            json p = {{"id", pid}, {"name", port.name}, {"layer", port.layer}, {"rectangle", json::object()}};
            put(p["rectangle"], port.rectangle);
            c["ports"].push_back(p);
        }
        for (const auto& [iid, instance] : cell.instances)
            c["instances"].push_back({{"id", iid}, {"cell", instance.cell}, {"name", instance.name}, {"x", instance.x}, {"y", instance.y}, {"orientation", orientationName(instance.orientation)}});
        j["layout_cells"].push_back(c);
    }
    return j.dump(2) + "\n";
}

static Orientation parseOrientation(const std::string& value) {
    static const std::map<std::string, Orientation> values = {{"r0", Orientation::r0}, {"r90", Orientation::r90}, {"r180", Orientation::r180}, {"r270", Orientation::r270}, {"mx", Orientation::mx}, {"my", Orientation::my}};
    auto it = values.find(value);
    if (it == values.end()) throw ProjectError("project.orientation", "Unknown orientation: " + value);
    return it->second;
}

static const char* pinDirectionName(PinDirection direction) {
    static const char* names[] = {"left", "right", "up", "down"};
    return names[static_cast<int>(direction)];
}

static PinDirection parsePinDirection(const std::string& value) {
    static const std::map<std::string, PinDirection> values = {
        {"left", PinDirection::left}, {"right", PinDirection::right},
        {"up", PinDirection::up}, {"down", PinDirection::down}};
    auto it = values.find(value);
    return it == values.end() ? PinDirection::right : it->second;
}

Project Project::deserialize(const std::string& text) {
    try {
        auto j = json::parse(text);
        if (j.at("format") != "ice-project") throw ProjectError("project.format", "Unsupported project format");
        if (j.at("schema_version") != schemaVersion) throw ProjectError("project.schema", "Unsupported project schema version");
        Project p;
        p.identity = j.at("identity");
        p.name = j.at("name");
        p.grid = j.at("grid");
        p.units.nanometers_per_unit = j.at("units").at("nanometers_per_unit");
        p.units.denominator = j.at("units").at("denominator");
        for (const auto& c : j.at("schematic_cells")) {
            SchematicCell cell{c.at("id"), c.at("name")};
            for (const auto& s : c.at("symbols")) {
                Symbol symbol{s.at("id"), s.at("name"), s.at("artwork")};
                for (const auto& pin : s.at("pins"))
                    symbol.pins.push_back({pin.at("id"), pin.at("name"), pin.at("x"), pin.at("y"),
                                           parsePinDirection(pin.value("direction", "right"))});
                cell.symbols.emplace(symbol.id, std::move(symbol));
            }
            for (const auto& i : c.at("instances")) cell.instances.emplace(i.at("id"), SchematicInstance{i.at("id"), i.at("symbol"), i.at("name"), i.at("x"), i.at("y"), parseOrientation(i.at("orientation"))});
            for (const auto& n : c.at("nets"))
                cell.nets.emplace(n.at("id"), Net{n.at("id"), n.at("name"), n.at("pins").get<std::vector<ObjectId>>(),
                                                  n.value("x", 0), n.value("y", 0)});
            p.schematicCells.emplace(cell.id, std::move(cell));
        }
        for (const auto& l : j.at("layers"))
            p.layers.emplace(l.at("id"), Layer{l.at("id"), l.at("name"), l.at("number"), l.value("visible", true)});
        for (const auto& c : j.at("layout_cells")) {
            LayoutCell cell{c.at("id"), c.at("name")};
            for (const auto& s : c.at("shapes")) {
                Shape shape{s.at("id"), s.at("layer"), s.contains("net") ? std::optional<ObjectId>(s.at("net")) : std::nullopt, Rect{s.at("rectangle").at("x"), s.at("rectangle").at("y"), s.at("rectangle").at("width"), s.at("rectangle").at("height")}, {}};
                if (s.contains("polygon"))
                    for (const auto& point : s.at("polygon"))
                        shape.polygon.push_back({point.at("x"), point.at("y")});
                cell.shapes.emplace(shape.id, shape);
            }
            for (const auto& port : c.at("ports")) cell.ports.emplace(port.at("id"), Port{port.at("id"), port.at("name"), port.at("layer"), Rect{port.at("rectangle").at("x"), port.at("rectangle").at("y"), port.at("rectangle").at("width"), port.at("rectangle").at("height")}});
            for (const auto& i : c.at("instances")) cell.instances.emplace(i.at("id"), LayoutInstance{i.at("id"), i.at("cell"), i.at("name"), i.at("x"), i.at("y"), parseOrientation(i.at("orientation"))});
            p.layoutCells.emplace(cell.id, std::move(cell));
        }
        auto diagnostics = p.validate();
        if (!diagnostics.empty()) throw ProjectError(diagnostics.front().code, diagnostics.front().message);
        return p;
    } catch (const ProjectError&) { throw; }
    catch (const std::exception& e) { throw ProjectError("project.parse", e.what()); }
}

void Project::save(const std::filesystem::path& path) const {
    auto diagnostics = validate();
    if (!diagnostics.empty()) throw ProjectError(diagnostics.front().code, diagnostics.front().message);
    std::ofstream stream(path);
    if (!stream) throw ProjectError("project.save", "Unable to open project file for writing");
    stream << serialize();
    if (!stream) throw ProjectError("project.save", "Unable to write project file");
    spdlog::info("Saved project {}", path.string());
}

Project Project::load(const std::filesystem::path& path) {
    std::ifstream stream(path);
    if (!stream) throw ProjectError("project.load", "Unable to open project file");
    return deserialize({std::istreambuf_iterator<char>(stream), {}});
}

ProjectSession::ProjectSession(Project project) : project_(std::move(project)) {}
Project& ProjectSession::project() noexcept { return project_; }
const Project& ProjectSession::project() const noexcept { return project_; }
bool ProjectSession::dirty() const noexcept { return dirty_; }
void ProjectSession::markDirty() { dirty_ = true; notify(); }
void ProjectSession::subscribe(Observer observer) { observers_.push_back(std::move(observer)); }
void ProjectSession::beginTransaction(std::string label) {
    if (transactionSnapshot_) throw ProjectError("transaction.nested", "Nested transactions are not supported");
    transactionSnapshot_ = project_;
    transactionLabel_ = std::move(label);
}
void ProjectSession::commitTransaction() {
    if (!transactionSnapshot_) throw ProjectError("transaction.missing", "No transaction is active");
    if (project_.serialize() != transactionSnapshot_->serialize()) {
        undo_.push_back(*transactionSnapshot_);
        redo_.clear();
        dirty_ = true;
        notify();
    }
    transactionSnapshot_.reset();
    transactionLabel_.clear();
}
void ProjectSession::rollbackTransaction() {
    if (!transactionSnapshot_) throw ProjectError("transaction.missing", "No transaction is active");
    project_ = *transactionSnapshot_;
    transactionSnapshot_.reset();
    transactionLabel_.clear();
    notify();
}
void ProjectSession::execute(std::string label, const std::function<void(Project&)>& mutation) {
    beginTransaction(std::move(label));
    try {
        mutation(project_);
        auto diagnostics = project_.validate();
        if (!diagnostics.empty()) throw ProjectError(diagnostics.front().code, diagnostics.front().message);
        commitTransaction();
    } catch (...) {
        rollbackTransaction();
        throw;
    }
}
bool ProjectSession::undo() {
    if (undo_.empty()) return false;
    redo_.push_back(project_);
    project_ = undo_.back();
    undo_.pop_back();
    dirty_ = true;
    notify();
    return true;
}
bool ProjectSession::redo() {
    if (redo_.empty()) return false;
    undo_.push_back(project_);
    project_ = redo_.back();
    redo_.pop_back();
    dirty_ = true;
    notify();
    return true;
}
void ProjectSession::save(const std::filesystem::path& path) { project_.save(path); dirty_ = false; notify(); }
void ProjectSession::notify() { for (const auto& observer : observers_) observer(); }
void SelectionModel::select(ObjectId id, std::vector<ObjectId> related) {
    primary_ = id;
    related_ = std::move(related);
}
void SelectionModel::clear() {
    primary_.reset();
    related_.clear();
}
std::optional<ObjectId> SelectionModel::primary() const noexcept { return primary_; }
const std::vector<ObjectId>& SelectionModel::related() const noexcept { return related_; }
std::string version() { return "0.1.0"; }
} // namespace ice
