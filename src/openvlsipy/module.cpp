#include "openvlsilib/agent_api.hpp"
#include "openvlsilib/core.hpp"
#include "openvlsilib/drc.hpp"
#include "openvlsilib/implementation.hpp"
#include "openvlsilib/operations.hpp"
#include "openvlsilib/packages.hpp"
#include "openvlsilib/pdk.hpp"
#include "openvlsilib/pex.hpp"
#include "openvlsilib/procedural_layout.hpp"
#include "openvlsilib/simulation.hpp"
#include "openvlsilib/verification.hpp"

#include <nanobind/nanobind.h>
#include <nanobind/stl/filesystem.h>
#include <nanobind/stl/map.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace nb = nanobind;
using namespace openvlsi;

namespace {

Orientation parseOrientation(const std::string &text) {
    static const std::map<std::string, Orientation> values = {
        {"r0", Orientation::r0}, {"r90", Orientation::r90}, {"r180", Orientation::r180}, {"r270", Orientation::r270},
        {"mx", Orientation::mx}, {"my", Orientation::my},   {"m45", Orientation::m45},   {"m135", Orientation::m135}};
    const auto it = values.find(text);
    if (it == values.end())
        throw ProjectError("openvlsipy.orientation", "Unknown orientation: " + text);
    return it->second;
}

LayoutArrange parseLayoutArrange(const std::string &text) {
    if (text == "left")
        return LayoutArrange::left;
    if (text == "right")
        return LayoutArrange::right;
    if (text == "top")
        return LayoutArrange::top;
    if (text == "bottom")
        return LayoutArrange::bottom;
    if (text == "center-horizontal" || text == "center_horizontal")
        return LayoutArrange::centerHorizontal;
    if (text == "center-vertical" || text == "center_vertical")
        return LayoutArrange::centerVertical;
    if (text == "distribute-horizontal" || text == "distribute_horizontal")
        return LayoutArrange::distributeHorizontal;
    if (text == "distribute-vertical" || text == "distribute_vertical")
        return LayoutArrange::distributeVertical;
    throw ProjectError("openvlsipy.arrange", "Unknown layout arrangement operation: " + text);
}

std::optional<Rect> cellBounds(const LayoutCell &cell) {
    Coordinate minX = std::numeric_limits<Coordinate>::max();
    Coordinate minY = std::numeric_limits<Coordinate>::max();
    Coordinate maxX = std::numeric_limits<Coordinate>::lowest();
    Coordinate maxY = std::numeric_limits<Coordinate>::lowest();
    bool any = false;
    const auto addPoint = [&](Coordinate x, Coordinate y) {
        minX = std::min(minX, x);
        minY = std::min(minY, y);
        maxX = std::max(maxX, x);
        maxY = std::max(maxY, y);
        any = true;
    };
    for (const auto &[_, shape] : cell.shapes) {
        if (!shape.polygon.empty()) {
            for (const auto &point : shape.polygon)
                addPoint(point.x, point.y);
            continue;
        }
        addPoint(shape.rectangle.x, shape.rectangle.y);
        addPoint(shape.rectangle.x + shape.rectangle.width, shape.rectangle.y + shape.rectangle.height);
    }
    for (const auto &[_, port] : cell.ports) {
        addPoint(port.rectangle.x, port.rectangle.y);
        addPoint(port.rectangle.x + port.rectangle.width, port.rectangle.y + port.rectangle.height);
    }
    if (!any)
        return std::nullopt;
    return Rect{minX, minY, maxX - minX, maxY - minY};
}

class LayoutBuilder {
  public:
    LayoutBuilder(Project &project, ObjectId cellId) : project_(&project), cellId_(cellId) {}

    ObjectId id() const { return cellId_; }

    std::string name() const { return cell().name; }

    std::string layer(const std::string &name, int number = -1, int datatype = 0) {
        if (name.empty())
            throw ProjectError("openvlsipy.layer", "Layer name must not be empty");
        const auto target = layerScope();
        for (const auto &[_, existing] : project_->layers) {
            if (existing.name == name && existing.datatype == datatype && existing.pdk == target.first &&
                existing.stack == target.second)
                return name;
        }
        int layerNumber = number;
        if (layerNumber < 0) {
            layerNumber = 0;
            for (const auto &[_, existing] : project_->layers)
                layerNumber = std::max(layerNumber, existing.number);
            ++layerNumber;
        }
        openvlsi::addLayer(*project_, name, layerNumber, 0, target.first, datatype, target.second);
        return name;
    }

    ObjectId rectangle(const std::string &layer, Coordinate x, Coordinate y, Coordinate width, Coordinate height) {
        return openvlsi::addRectangle(*project_, cellId_, layerId(layer), Rect{x, y, width, height});
    }

    ObjectId polygon(const std::string &layer, const std::vector<LayoutPoint> &points) {
        return openvlsi::addPolygon(*project_, cellId_, layerId(layer), points);
    }

    ObjectId layoutNet(const std::string &name) {
        if (name.empty())
            return 0;
        auto &cell = project_->layoutCells.at(cellId_);
        for (const auto &[id, net] : cell.nets) {
            if (net.name == name)
                return id;
        }
        return openvlsi::createLayoutNet(*project_, cellId_, name);
    }

    ObjectId port(const std::string &name, const std::string &layer, Coordinate x, Coordinate y, Coordinate width,
                  Coordinate height, const std::string &direction = "inout", const std::string &side = "left",
                  const std::string &net = {}, const std::string &label = {}) {
        const auto parsedDirection = openvlsi::parsePortDirection(direction);
        const auto parsedSide = openvlsi::parsePortSide(side);
        if (!parsedDirection || !parsedSide)
            throw ProjectError("openvlsipy.port", "Unknown port direction or side");
        return openvlsi::addPort(*project_, cellId_, name, layerId(layer), Rect{x, y, width, height}, *parsedDirection,
                            *parsedSide, layoutNet(net), label);
    }

    bool updatePort(const Port &port) { return openvlsi::updatePort(*project_, cellId_, port); }

    void removePort(ObjectId portId) { openvlsi::removePort(*project_, cellId_, portId); }

    void assignPortNet(ObjectId portId, const std::string &net) {
        openvlsi::assignPortNet(*project_, cellId_, portId, layoutNet(net));
    }

    ObjectId instance(const std::string &cellName, Coordinate x, Coordinate y, const std::string &instanceName = {},
                      const std::string &orientation = "r0") {
        ObjectId childId = 0;
        for (const auto &[id, cell] : project_->layoutCells) {
            if (cell.name == cellName) {
                childId = id;
                break;
            }
        }
        if (childId == 0)
            throw ProjectError("openvlsipy.instance", "Unknown layout cell: " + cellName);
        if (childId == cellId_)
            throw ProjectError("openvlsipy.instance", "A layout cell cannot instance itself");
        auto &cell = project_->layoutCells.at(cellId_);
        const auto &childCell = project_->layoutCells.at(childId);
        if (!cell.pdk.empty() && !childCell.pdk.empty() && cell.pdk != childCell.pdk)
            throw ProjectError("openvlsipy.instance", "Parent and child layouts must use the same PDK");
        if (cell.stack != childCell.stack)
            throw ProjectError("openvlsipy.instance", "Parent and child layouts must use the same PDK stack");
        const auto id = project_->nextId();
        std::string name = instanceName;
        if (name.empty())
            name = "instance_" + std::to_string(id);
        cell.instances.emplace(id, LayoutInstance{id, childId, name, x, y, parseOrientation(orientation)});
        return id;
    }

    void parameter(const std::string &name, const std::string &value) {
        if (name.empty())
            throw ProjectError("openvlsipy.parameter", "Parameter name must not be empty");
        auto &parameters = project_->layoutCells.at(cellId_).parameters;
        for (auto &parameter : parameters) {
            if (parameter.name == name) {
                parameter.value = value;
                return;
            }
        }
        parameters.push_back({project_->nextId(), name, value});
    }

    void set_library(bool library) { project_->layoutCells.at(cellId_).library = library; }

    void set_device(const std::string &device) { project_->layoutCells.at(cellId_).device = device; }

    std::vector<ObjectId> duplicate(const std::vector<ObjectId> &shapes, std::optional<Coordinate> dx = std::nullopt,
                                    std::optional<Coordinate> dy = std::nullopt) {
        return openvlsi::duplicateLayoutShapes(*project_, cellId_, shapes, dx, dy);
    }

    std::vector<ObjectId> array(const std::vector<ObjectId> &shapes, int rows, int columns, Coordinate xPitch = 0,
                                Coordinate yPitch = 0) {
        return openvlsi::arrayLayoutShapes(*project_, cellId_, shapes, rows, columns, xPitch, yPitch);
    }

    void arrange(const std::vector<ObjectId> &shapes, const std::string &operation) {
        openvlsi::arrangeLayoutShapes(*project_, cellId_, shapes, parseLayoutArrange(operation));
    }

    std::vector<LayoutDeviceBinding> device_links(ObjectId schematicCell = 0) const {
        return openvlsi::layoutDeviceBindings(*project_, cellId_, schematicCell);
    }

    void link_schematic(ObjectId schematicCell) { openvlsi::linkLayoutToSchematic(*project_, cellId_, schematicCell); }

    void unlink_schematic() { openvlsi::unlinkLayoutFromSchematic(*project_, cellId_); }

    void bind_device(ObjectId layoutDevice, ObjectId schematicCell, ObjectId schematicInstance) {
        openvlsi::bindLayoutDevice(*project_, cellId_, layoutDevice, schematicCell, schematicInstance);
    }

    void unbind_device(ObjectId layoutDevice) { openvlsi::unbindLayoutDevice(*project_, cellId_, layoutDevice); }

    LayoutCell cell() const {
        const auto it = project_->layoutCells.find(cellId_);
        if (it == project_->layoutCells.end())
            throw ProjectError("openvlsipy.cell", "Layout cell no longer exists");
        return it->second;
    }

  private:
    std::pair<std::string, std::string> layerScope() const {
        const auto &cell = project_->layoutCells.at(cellId_);
        return {cell.pdk == genericPdkId ? std::string{} : cell.pdk, cell.stack};
    }

    ObjectId layerId(const std::string &name) {
        const auto target = layerScope();
        for (const auto &[id, layer] : project_->layers) {
            if (layer.name == name && layer.pdk == target.first && layer.stack == target.second)
                return id;
        }
        layer(name);
        for (const auto &[id, layer] : project_->layers) {
            if (layer.name == name && layer.pdk == target.first && layer.stack == target.second)
                return id;
        }
        throw ProjectError("openvlsipy.layer", "Layer does not exist: " + name);
    }

    Project *project_ = nullptr;
    ObjectId cellId_ = 0;
};

PinDirection parsePinDirection(const std::string &text) {
    if (text == "left")
        return PinDirection::left;
    if (text == "right")
        return PinDirection::right;
    if (text == "up")
        return PinDirection::up;
    if (text == "down")
        return PinDirection::down;
    throw ProjectError("openvlsipy.pin", "Unknown pin direction: " + text);
}

class SchematicBuilder {
  public:
    SchematicBuilder(Project &project, ObjectId cellId) : project_(&project), cellId_(cellId) {}

    ObjectId id() const { return cellId_; }
    std::string name() const { return cell().name; }

    ObjectId symbol(const std::string &name, const std::string &artwork = {}, ObjectId sourceCell = 0) {
        return openvlsi::addSymbol(*project_, cellId_, name, artwork, sourceCell);
    }

    ObjectId pin(ObjectId symbolId, const std::string &name, const std::string &direction = "right", Coordinate x = 0,
                 Coordinate y = 0, int width = 1, bool widthKnown = true) {
        auto &cell = project_->schematicCells.at(cellId_);
        const auto symbol = cell.symbols.find(symbolId);
        if (symbol == cell.symbols.end())
            throw ProjectError("openvlsipy.pin", "Symbol does not exist");
        const auto id = project_->nextId();
        symbol->second.pins.push_back({id, name, x, y, parsePinDirection(direction), width, widthKnown});
        return id;
    }

    ObjectId instance(ObjectId symbolId, const std::string &name, Coordinate x = 0, Coordinate y = 0) {
        return openvlsi::addSchematicInstance(*project_, cellId_, symbolId, name, x, y);
    }

    ObjectId net(const std::string &name, const std::vector<ObjectId> &pins, Coordinate x = 0, Coordinate y = 0) {
        return openvlsi::addNet(*project_, cellId_, name, pins, x, y);
    }

    ObjectId component(const std::string &type, const std::string &name, Coordinate x = 0, Coordinate y = 0,
                       const std::map<std::string, std::string> &parameters = {}) {
        ComponentRequest request;
        request.type = type;
        request.name = name;
        request.x = x;
        request.y = y;
        for (const auto &[parameterName, value] : parameters)
            request.parameters.push_back({0, parameterName, value});
        return openvlsi::addSchematicComponent(*project_, cellId_, request).instance;
    }

    void set_parameter(ObjectId instanceId, const std::string &name, const std::string &value) {
        openvlsi::setSchematicParameter(*project_, cellId_, instanceId, name, value);
    }

    void set_cell_parameter(const std::string &name, const std::string &value) {
        openvlsi::setSchematicCellParameter(*project_, cellId_, name, value);
    }

    bool remove_cell_parameter(const std::string &name) {
        return openvlsi::removeSchematicCellParameter(*project_, cellId_, name);
    }

    std::vector<std::string> netlist(bool mixed = false) const {
        const auto &cell = project_->schematicCells.at(cellId_);
        if (cell.textSchematic)
            return openvlsi::buildSimulationNetlist(*project_, cellId_);
        return openvlsi::buildSchematicNetlist(*project_, cellId_, nullptr,
                                          mixed ? SchematicNetlistMode::mixed : SchematicNetlistMode::analog,
                                          openvlsi::pdkNetlistGeometryScale(cell.pdk));
    }

    SchematicWrapResult wrap(const std::vector<ObjectId> &instances, const std::string &name = {}) {
        return openvlsi::wrapSchematicInstances(*project_, cellId_, instances, name);
    }

    SchematicCell cell() const {
        const auto it = project_->schematicCells.find(cellId_);
        if (it == project_->schematicCells.end())
            throw ProjectError("openvlsipy.cell", "Schematic cell no longer exists");
        return it->second;
    }

  private:
    Project *project_ = nullptr;
    ObjectId cellId_ = 0;
};

std::optional<SchematicCell> findSchematicCell(const Project &project, const std::string &name) {
    for (const auto &[_, cell] : project.schematicCells) {
        if (cell.name == name)
            return cell;
    }
    return std::nullopt;
}

std::optional<LayoutCell> findLayoutCell(const Project &project, const std::string &name);

ObjectId resolveSchematicId(const Project &project, const std::string &text) {
    if (const auto cell = findSchematicCell(project, text))
        return cell->id;
    try {
        std::size_t consumed = 0;
        const auto id = static_cast<ObjectId>(std::stoull(text, &consumed));
        if (consumed == text.size() && project.schematicCells.contains(id))
            return id;
    } catch (const std::exception &) {
    }
    throw ProjectError("openvlsipy.schematic", "Schematic does not exist: " + text);
}

ObjectId resolveSimulationId(const Project &project, const std::string &text) {
    for (const auto &[id, simulation] : project.simulations) {
        if (simulation.name == text)
            return id;
    }
    try {
        std::size_t consumed = 0;
        const auto id = static_cast<ObjectId>(std::stoull(text, &consumed));
        if (consumed == text.size() && project.simulations.contains(id))
            return id;
    } catch (const std::exception &) {
    }
    throw ProjectError("openvlsipy.simulation", "Simulation does not exist: " + text);
}

ObjectId resolveLayoutId(const Project &project, const std::string &text) {
    if (const auto cell = findLayoutCell(project, text))
        return cell->id;
    try {
        std::size_t consumed = 0;
        const auto id = static_cast<ObjectId>(std::stoull(text, &consumed));
        if (consumed == text.size() && project.layoutCells.contains(id))
            return id;
    } catch (const std::exception &) {
    }
    throw ProjectError("openvlsipy.layout", "Layout does not exist: " + text);
}

std::optional<LayoutCell> findLayoutCell(const Project &project, const std::string &name) {
    for (const auto &[_, cell] : project.layoutCells) {
        if (cell.name == name)
            return cell;
    }
    return std::nullopt;
}

std::optional<LayoutCell> findLayoutCell(const Project &project, ObjectId id) {
    const auto it = project.layoutCells.find(id);
    if (it == project.layoutCells.end())
        return std::nullopt;
    return it->second;
}

std::string resolveLayoutStack(const std::string &pdk, const std::string &requested) {
    if (pdk.empty() || pdk == genericPdkId)
        return requested;
    const auto pdks = findPdks(pdk);
    if (pdks.empty())
        return requested;
    const auto package = PackageManager::readManifest(pdks.front().packageRoot);
    if (!package.pdk)
        return requested;
    return resolvePdkStack(*package.pdk, requested);
}

std::optional<PackageSpec> implementationPackageForPdk(const std::string &pdk) {
    if (pdk.empty() || pdk == genericPdkId)
        return std::nullopt;
    const auto pdks = findPdks(pdk);
    if (pdks.empty())
        return std::nullopt;
    return PackageManager::readManifest(pdks.front().packageRoot);
}

ImplementationStage parseImplementationStageArgument(const std::string &text, const char *label) {
    const auto parsed = parseImplementationStage(text);
    if (!parsed || *parsed == ImplementationStage::created)
        throw ProjectError("openvlsipy.implementation", std::string("Invalid implementation stage: ") + label);
    return *parsed;
}

ImplementationExecutionResult executeImplementationForPython(Project &project, ObjectId runId,
                                                             const std::string &projectRoot,
                                                             const std::string &fromText, const std::string &toText) {
    auto *run = findImplementationRun(project, runId);
    if (!run)
        throw ProjectError("openvlsipy.implementation", "Implementation run does not exist");
    const auto package = implementationPackageForPdk(run->config.pdk);
    if (!package || !package->pdk)
        throw ProjectError("openvlsipy.implementation", "No installed PDK package provides '" + run->config.pdk + "'");
    std::vector<Diagnostic> diagnostics;
    const auto platform = resolveImplementationPlatform(*package, run->config.stack, diagnostics);
    for (const auto &diagnostic : diagnostics) {
        if (diagnostic.severity == Diagnostic::Severity::error)
            throw ProjectError(diagnostic.code, diagnostic.message);
    }
    ImplementationExecutionOptions options;
    options.from = parseImplementationStageArgument(fromText, "from");
    options.to = parseImplementationStageArgument(toText, "to");
    return executeImplementationRun(project, *run, platform, projectRoot, options);
}

} // namespace

NB_MODULE(openvlsipy, module) {
    module.doc() = "Python access to the OpenVLSI canonical project model and layout database.";

    nb::enum_<Diagnostic::Severity>(module, "Severity")
        .value("error", Diagnostic::Severity::error)
        .value("warning", Diagnostic::Severity::warning);

    nb::enum_<Orientation>(module, "Orientation")
        .value("r0", Orientation::r0)
        .value("r90", Orientation::r90)
        .value("r180", Orientation::r180)
        .value("r270", Orientation::r270)
        .value("mx", Orientation::mx)
        .value("my", Orientation::my)
        .value("m45", Orientation::m45)
        .value("m135", Orientation::m135);

    nb::class_<DatabaseUnits>(module, "DatabaseUnits")
        .def(nb::init<>())
        .def_rw("nanometers_per_unit", &DatabaseUnits::nanometers_per_unit)
        .def_rw("denominator", &DatabaseUnits::denominator)
        .def("valid", &DatabaseUnits::valid);

    nb::class_<Rect>(module, "Rect")
        .def(nb::init<>())
        .def(nb::init<Coordinate, Coordinate, Coordinate, Coordinate>(), nb::arg("x"), nb::arg("y"), nb::arg("width"),
             nb::arg("height"))
        .def_rw("x", &Rect::x)
        .def_rw("y", &Rect::y)
        .def_rw("width", &Rect::width)
        .def_rw("height", &Rect::height)
        .def("valid", &Rect::valid);

    nb::class_<LayoutPoint>(module, "LayoutPoint")
        .def(nb::init<>())
        .def(nb::init<Coordinate, Coordinate>(), nb::arg("x"), nb::arg("y"))
        .def_rw("x", &LayoutPoint::x)
        .def_rw("y", &LayoutPoint::y)
        .def("__repr__", [](const LayoutPoint &point) {
            return "LayoutPoint(" + std::to_string(point.x) + ", " + std::to_string(point.y) + ")";
        });

    nb::class_<Layer>(module, "Layer")
        .def(nb::init<>())
        .def_rw("id", &Layer::id)
        .def_rw("name", &Layer::name)
        .def_rw("number", &Layer::number)
        .def_rw("datatype", &Layer::datatype)
        .def_rw("minimum_width", &Layer::minimumWidth)
        .def_rw("stack", &Layer::stack)
        .def_rw("color", &Layer::color)
        .def_rw("pattern", &Layer::pattern)
        .def_rw("visible", &Layer::visible);

    nb::class_<Parameter>(module, "Parameter")
        .def(nb::init<>())
        .def_rw("id", &Parameter::id)
        .def_rw("name", &Parameter::name)
        .def_rw("value", &Parameter::value);

    nb::class_<Shape>(module, "Shape")
        .def(nb::init<>())
        .def_rw("id", &Shape::id)
        .def_rw("layer", &Shape::layer)
        .def_rw("net", &Shape::net)
        .def_rw("rectangle", &Shape::rectangle)
        .def_rw("polygon", &Shape::polygon)
        .def_rw("centerline", &Shape::centerline)
        .def_rw("segment_layers", &Shape::segmentLayers)
        .def_rw("point_vias", &Shape::pointVias)
        .def_rw("width", &Shape::width)
        .def_rw("segment_widths", &Shape::segmentWidths)
        .def_rw("text", &Shape::text)
        .def_rw("name", &Shape::name)
        .def_prop_ro("is_path", &Shape::isPath);

    nb::enum_<PinDirection>(module, "PinDirection")
        .value("left", PinDirection::left)
        .value("right", PinDirection::right)
        .value("up", PinDirection::up)
        .value("down", PinDirection::down);

    nb::enum_<PortDirection>(module, "PortDirection")
        .value("input", PortDirection::input)
        .value("output", PortDirection::output)
        .value("inout", PortDirection::inout)
        .value("power", PortDirection::power)
        .value("ground", PortDirection::ground);

    nb::enum_<PortSide>(module, "PortSide")
        .value("left", PortSide::left)
        .value("right", PortSide::right)
        .value("top", PortSide::top)
        .value("bottom", PortSide::bottom);

    nb::class_<LayoutNet>(module, "LayoutNet")
        .def(nb::init<>())
        .def_rw("id", &LayoutNet::id)
        .def_rw("name", &LayoutNet::name);

    nb::class_<Port>(module, "Port")
        .def(nb::init<>())
        .def_rw("id", &Port::id)
        .def_rw("name", &Port::name)
        .def_rw("layer", &Port::layer)
        .def_rw("rectangle", &Port::rectangle)
        .def_rw("direction", &Port::direction)
        .def_rw("side", &Port::side)
        .def_rw("net", &Port::net)
        .def_rw("label", &Port::label)
        .def_rw("visible", &Port::visible)
        .def_rw("locked", &Port::locked)
        .def_rw("internal", &Port::internal)
        .def_rw("source_instance", &Port::sourceInstance)
        .def_rw("source_port", &Port::sourcePort);

    nb::class_<LayoutInstance>(module, "LayoutInstance")
        .def(nb::init<>())
        .def_rw("id", &LayoutInstance::id)
        .def_rw("cell", &LayoutInstance::cell)
        .def_rw("name", &LayoutInstance::name)
        .def_rw("x", &LayoutInstance::x)
        .def_rw("y", &LayoutInstance::y)
        .def_rw("orientation", &LayoutInstance::orientation)
        .def_rw("schematic_instance", &LayoutInstance::schematicInstance);

    nb::enum_<LayoutDeviceKind>(module, "LayoutDeviceKind")
        .value("planned_device", LayoutDeviceKind::plannedDevice)
        .value("instance", LayoutDeviceKind::instance);

    nb::class_<LayoutDeviceBinding>(module, "LayoutDeviceBinding")
        .def(nb::init<>())
        .def_ro("layout_cell", &LayoutDeviceBinding::layoutCell)
        .def_ro("layout_object", &LayoutDeviceBinding::layoutObject)
        .def_ro("kind", &LayoutDeviceBinding::kind)
        .def_ro("layout_name", &LayoutDeviceBinding::layoutName)
        .def_ro("schematic_cell", &LayoutDeviceBinding::schematicCell)
        .def_ro("schematic_instance", &LayoutDeviceBinding::schematicInstance)
        .def_ro("schematic_name", &LayoutDeviceBinding::schematicName)
        .def_ro("stale", &LayoutDeviceBinding::stale)
        .def_ro("duplicate", &LayoutDeviceBinding::duplicate);

    nb::enum_<ImplementationStage>(module, "ImplementationStage")
        .value("created", ImplementationStage::created)
        .value("elaboration", ImplementationStage::elaboration)
        .value("synthesis", ImplementationStage::synthesis)
        .value("floorplan", ImplementationStage::floorplan)
        .value("placement", ImplementationStage::placement)
        .value("cts", ImplementationStage::cts)
        .value("routing", ImplementationStage::routing)
        .value("final", ImplementationStage::final);

    nb::enum_<ImplementationStatus>(module, "ImplementationStatus")
        .value("pending", ImplementationStatus::pending)
        .value("running", ImplementationStatus::running)
        .value("succeeded", ImplementationStatus::succeeded)
        .value("failed", ImplementationStatus::failed)
        .value("cancelled", ImplementationStatus::cancelled);

    nb::enum_<ImplementationStageStatus>(module, "ImplementationStageStatus")
        .value("pending", ImplementationStageStatus::pending)
        .value("running", ImplementationStageStatus::running)
        .value("succeeded", ImplementationStageStatus::succeeded)
        .value("failed", ImplementationStageStatus::failed)
        .value("skipped", ImplementationStageStatus::skipped)
        .value("cancelled", ImplementationStageStatus::cancelled);

    nb::class_<ImplementationMacroPlacement>(module, "ImplementationMacroPlacement")
        .def(nb::init<>())
        .def_rw("cell", &ImplementationMacroPlacement::cell)
        .def_rw("x", &ImplementationMacroPlacement::x)
        .def_rw("y", &ImplementationMacroPlacement::y)
        .def_rw("orientation", &ImplementationMacroPlacement::orientation);

    nb::class_<ImplementationConfig>(module, "ImplementationConfig")
        .def(nb::init<>())
        .def_rw("name", &ImplementationConfig::name)
        .def_rw("top", &ImplementationConfig::top)
        .def_rw("pdk", &ImplementationConfig::pdk)
        .def_rw("stack", &ImplementationConfig::stack)
        .def_rw("standard_cell_library", &ImplementationConfig::standardCellLibrary)
        .def_rw("corner", &ImplementationConfig::corner)
        .def_rw("sources", &ImplementationConfig::sources)
        .def_rw("source_cells", &ImplementationConfig::sourceCells)
        .def_rw("sdc", &ImplementationConfig::sdc)
        .def_rw("die_area", &ImplementationConfig::dieArea)
        .def_rw("core_area", &ImplementationConfig::coreArea)
        .def_rw("utilization", &ImplementationConfig::utilization)
        .def_rw("aspect_ratio", &ImplementationConfig::aspectRatio)
        .def_rw("site", &ImplementationConfig::site)
        .def_rw("io_horizontal_layers", &ImplementationConfig::ioHorizontalLayers)
        .def_rw("io_vertical_layers", &ImplementationConfig::ioVerticalLayers)
        .def_rw("macros", &ImplementationConfig::macros)
        .def_rw("placement_density", &ImplementationConfig::placementDensity)
        .def_rw("timing_driven", &ImplementationConfig::timingDriven)
        .def_rw("routability_driven", &ImplementationConfig::routabilityDriven)
        .def_rw("cts_enabled", &ImplementationConfig::ctsEnabled)
        .def_rw("cts_root_buffer", &ImplementationConfig::ctsRootBuffer)
        .def_rw("cts_buffers", &ImplementationConfig::ctsBuffers)
        .def_rw("routing_min_layer", &ImplementationConfig::routingMinLayer)
        .def_rw("routing_max_layer", &ImplementationConfig::routingMaxLayer)
        .def_rw("antenna_repair", &ImplementationConfig::antennaRepair)
        .def_rw("backend", &ImplementationConfig::backend)
        .def_rw("threads", &ImplementationConfig::threads)
        .def_rw("seed", &ImplementationConfig::seed)
        .def_rw("timeout_seconds", &ImplementationConfig::timeoutSeconds);

    nb::class_<ImplementationStageState>(module, "ImplementationStageState")
        .def(nb::init<>())
        .def_rw("stage", &ImplementationStageState::stage)
        .def_rw("status", &ImplementationStageState::status)
        .def_rw("started_at", &ImplementationStageState::startedAt)
        .def_rw("finished_at", &ImplementationStageState::finishedAt)
        .def_rw("error", &ImplementationStageState::error)
        .def_rw("artifacts", &ImplementationStageState::artifacts);

    nb::class_<ImplementationRun>(module, "ImplementationRun")
        .def(nb::init<>())
        .def_ro("id", &ImplementationRun::id)
        .def_rw("name", &ImplementationRun::name)
        .def_rw("config", &ImplementationRun::config)
        .def_rw("current_stage", &ImplementationRun::currentStage)
        .def_rw("status", &ImplementationRun::status)
        .def_prop_ro("stages",
                     [](const ImplementationRun &run) {
                         std::vector<ImplementationStageState> states;
                         for (const auto stage : implementationStages()) {
                             const auto found = run.stages.find(stage);
                             if (found != run.stages.end())
                                 states.push_back(found->second);
                             else {
                                 ImplementationStageState state;
                                 state.stage = stage;
                                 states.push_back(std::move(state));
                             }
                         }
                         return states;
                     })
        .def_rw("directory", &ImplementationRun::directory)
        .def_rw("config_hash", &ImplementationRun::configHash)
        .def_rw("tool_versions", &ImplementationRun::toolVersions)
        .def_rw("metrics", &ImplementationRun::metrics)
        .def_rw("artifacts", &ImplementationRun::artifacts)
        .def_rw("result_layout", &ImplementationRun::resultLayout)
        .def_rw("last_error", &ImplementationRun::lastError)
        .def_rw("created_at", &ImplementationRun::createdAt)
        .def_rw("updated_at", &ImplementationRun::updatedAt);

    nb::class_<ImplementationExecutionResult>(module, "ImplementationExecutionResult")
        .def_ro("success", &ImplementationExecutionResult::success)
        .def_ro("cancelled", &ImplementationExecutionResult::cancelled)
        .def_prop_ro("completed_stage",
                     [](const ImplementationExecutionResult &result) { return result.completedStage; })
        .def_prop_ro("diagnostics", [](const ImplementationExecutionResult &result) { return result.diagnostics; });

    nb::class_<LayoutCell>(module, "LayoutCell")
        .def(nb::init<>())
        .def_prop_ro("id", [](const LayoutCell &cell) { return cell.id; })
        .def_prop_rw(
            "name", [](const LayoutCell &cell) { return cell.name; },
            [](LayoutCell &cell, const std::string &name) { cell.name = name; })
        .def_prop_rw(
            "pdk", [](const LayoutCell &cell) { return cell.pdk; },
            [](LayoutCell &cell, const std::string &pdk) { cell.pdk = pdk; })
        .def_prop_rw(
            "stack", [](const LayoutCell &cell) { return cell.stack; },
            [](LayoutCell &cell, const std::string &stack) { cell.stack = stack; })
        .def_prop_rw(
            "library", [](const LayoutCell &cell) { return cell.library; },
            [](LayoutCell &cell, bool library) { cell.library = library; })
        .def_prop_rw(
            "device", [](const LayoutCell &cell) { return cell.device; },
            [](LayoutCell &cell, const std::string &device) { cell.device = device; })
        .def_prop_rw(
            "read_only", [](const LayoutCell &cell) { return cell.readOnly; },
            [](LayoutCell &cell, bool readOnly) { cell.readOnly = readOnly; })
        .def_prop_ro("generated_by", [](const LayoutCell &cell) { return cell.generatedBy; })
        .def_prop_ro("schematic_cell", [](const LayoutCell &cell) { return cell.schematicCell; })
        .def_prop_ro("parameters", [](const LayoutCell &cell) { return cell.parameters; })
        .def_prop_ro("shapes",
                     [](const LayoutCell &cell) {
                         std::vector<Shape> shapes;
                         shapes.reserve(cell.shapes.size());
                         for (const auto &[_, shape] : cell.shapes)
                             shapes.push_back(shape);
                         return shapes;
                     })
        .def_prop_ro("ports",
                     [](const LayoutCell &cell) {
                         std::vector<Port> ports;
                         ports.reserve(cell.ports.size());
                         for (const auto &[_, port] : cell.ports)
                             ports.push_back(port);
                         return ports;
                     })
        .def_prop_ro("nets",
                     [](const LayoutCell &cell) {
                         std::vector<LayoutNet> nets;
                         nets.reserve(cell.nets.size());
                         for (const auto &[_, net] : cell.nets)
                             nets.push_back(net);
                         return nets;
                     })
        .def_prop_ro("instances",
                     [](const LayoutCell &cell) {
                         std::vector<LayoutInstance> instances;
                         instances.reserve(cell.instances.size());
                         for (const auto &[_, instance] : cell.instances)
                             instances.push_back(instance);
                         return instances;
                     })
        .def_prop_ro("bounds", [](const LayoutCell &cell) { return cellBounds(cell); });

    nb::class_<Diagnostic>(module, "Diagnostic")
        .def_ro("severity", &Diagnostic::severity)
        .def_ro("code", &Diagnostic::code)
        .def_ro("message", &Diagnostic::message)
        .def_ro("object", &Diagnostic::object);

    nb::class_<ProceduralLayout>(module, "ProceduralLayout")
        .def_prop_ro("id", [](const ProceduralLayout &layout) { return layout.id; })
        .def_prop_rw(
            "name", [](const ProceduralLayout &layout) { return layout.name; },
            [](ProceduralLayout &layout, const std::string &name) { layout.name = name; })
        .def_prop_rw(
            "pdk", [](const ProceduralLayout &layout) { return layout.pdk; },
            [](ProceduralLayout &layout, const std::string &pdk) { layout.pdk = pdk; })
        .def_prop_rw(
            "stack", [](const ProceduralLayout &layout) { return layout.stack; },
            [](ProceduralLayout &layout, const std::string &stack) { layout.stack = stack; })
        .def_prop_rw(
            "script", [](const ProceduralLayout &layout) { return layout.script; },
            [](ProceduralLayout &layout, const std::string &script) { layout.script = script; })
        .def_prop_ro("generated_cell", [](const ProceduralLayout &layout) { return layout.generatedCell; })
        .def_prop_ro("generated_script_hash", [](const ProceduralLayout &layout) { return layout.generatedScriptHash; })
        .def_prop_ro("generated_pdk_version", [](const ProceduralLayout &layout) { return layout.generatedPdkVersion; })
        .def_prop_ro("generated_output_hash", [](const ProceduralLayout &layout) { return layout.generatedOutputHash; })
        .def_prop_ro("generated_parameter_hash",
                     [](const ProceduralLayout &layout) { return layout.generatedParameterHash; })
        .def_prop_ro("parameters", [](const ProceduralLayout &layout) { return layout.parameters; })
        .def_prop_ro("needs_regeneration",
                     [](const ProceduralLayout &layout) { return proceduralLayoutNeedsRegeneration(layout); });

    nb::class_<LayoutBuilder>(module, "LayoutBuilder")
        .def_prop_ro("id", &LayoutBuilder::id)
        .def_prop_ro("name", &LayoutBuilder::name)
        .def("layer", &LayoutBuilder::layer, nb::arg("name"), nb::arg("number") = -1, nb::arg("datatype") = 0)
        .def("rectangle", &LayoutBuilder::rectangle, nb::arg("layer"), nb::arg("x"), nb::arg("y"), nb::arg("width"),
             nb::arg("height"))
        .def("polygon", &LayoutBuilder::polygon, nb::arg("layer"), nb::arg("points"))
        .def("port", &LayoutBuilder::port, nb::arg("name"), nb::arg("layer"), nb::arg("x"), nb::arg("y"),
             nb::arg("width"), nb::arg("height"), nb::arg("direction") = "inout", nb::arg("side") = "left",
             nb::arg("net") = "", nb::arg("label") = "")
        .def("layout_net", &LayoutBuilder::layoutNet, nb::arg("name"))
        .def("update_port", &LayoutBuilder::updatePort, nb::arg("port"))
        .def("remove_port", &LayoutBuilder::removePort, nb::arg("port"))
        .def("assign_port_net", &LayoutBuilder::assignPortNet, nb::arg("port"), nb::arg("net"))
        .def("instance", &LayoutBuilder::instance, nb::arg("cell"), nb::arg("x"), nb::arg("y"), nb::arg("name") = "",
             nb::arg("orientation") = "r0")
        .def("parameter", &LayoutBuilder::parameter, nb::arg("name"), nb::arg("value"))
        .def("duplicate", &LayoutBuilder::duplicate, nb::arg("shapes"), nb::arg("dx") = nb::none(),
             nb::arg("dy") = nb::none())
        .def("array", &LayoutBuilder::array, nb::arg("shapes"), nb::arg("rows"), nb::arg("columns"),
             nb::arg("x_pitch") = 0, nb::arg("y_pitch") = 0)
        .def("arrange", &LayoutBuilder::arrange, nb::arg("shapes"), nb::arg("operation"))
        .def("device_links", &LayoutBuilder::device_links, nb::arg("schematic_cell") = 0)
        .def("link_schematic", &LayoutBuilder::link_schematic, nb::arg("schematic_cell"))
        .def("unlink_schematic", &LayoutBuilder::unlink_schematic)
        .def("bind_device", &LayoutBuilder::bind_device, nb::arg("layout_device"), nb::arg("schematic_cell"),
             nb::arg("schematic_instance"))
        .def("unbind_device", &LayoutBuilder::unbind_device, nb::arg("layout_device"))
        .def("set_library", &LayoutBuilder::set_library, nb::arg("library"))
        .def("set_device", &LayoutBuilder::set_device, nb::arg("device"))
        .def("cell", &LayoutBuilder::cell);

    nb::class_<Pin>(module, "Pin")
        .def(nb::init<>())
        .def_rw("id", &Pin::id)
        .def_rw("name", &Pin::name)
        .def_rw("x", &Pin::x)
        .def_rw("y", &Pin::y)
        .def_rw("direction", &Pin::direction)
        .def_rw("width", &Pin::width)
        .def_rw("width_known", &Pin::widthKnown);

    nb::class_<Symbol>(module, "Symbol")
        .def(nb::init<>())
        .def_rw("id", &Symbol::id)
        .def_rw("name", &Symbol::name)
        .def_rw("artwork", &Symbol::artwork)
        .def_rw("source_cell", &Symbol::sourceCell)
        .def_rw("pins", &Symbol::pins);

    nb::class_<SchematicInstance>(module, "SchematicInstance")
        .def(nb::init<>())
        .def_rw("id", &SchematicInstance::id)
        .def_rw("symbol", &SchematicInstance::symbol)
        .def_rw("name", &SchematicInstance::name)
        .def_rw("x", &SchematicInstance::x)
        .def_rw("y", &SchematicInstance::y)
        .def_rw("orientation", &SchematicInstance::orientation)
        .def_rw("parameters", &SchematicInstance::parameters);

    nb::class_<Net>(module, "Net")
        .def(nb::init<>())
        .def_rw("id", &Net::id)
        .def_rw("name", &Net::name)
        .def_rw("pins", &Net::pins)
        .def_rw("x", &Net::x)
        .def_rw("y", &Net::y);

    nb::class_<SchematicWrapResult>(module, "SchematicWrapResult")
        .def_ro("cell", &SchematicWrapResult::cell)
        .def_ro("symbol", &SchematicWrapResult::symbol)
        .def_ro("instance", &SchematicWrapResult::instance)
        .def_ro("ports", &SchematicWrapResult::ports);

    nb::class_<SchematicCell>(module, "SchematicCell")
        .def(nb::init<>())
        .def_prop_ro("id", [](const SchematicCell &cell) { return cell.id; })
        .def_prop_rw(
            "name", [](const SchematicCell &cell) { return cell.name; },
            [](SchematicCell &cell, const std::string &name) { cell.name = name; })
        .def_prop_rw(
            "pdk", [](const SchematicCell &cell) { return cell.pdk; },
            [](SchematicCell &cell, const std::string &pdk) { cell.pdk = pdk; })
        .def_rw("parameters", &SchematicCell::parameters)
        .def_prop_ro("symbols",
                     [](const SchematicCell &cell) {
                         std::vector<Symbol> symbols;
                         symbols.reserve(cell.symbols.size());
                         for (const auto &[_, symbol] : cell.symbols)
                             symbols.push_back(symbol);
                         return symbols;
                     })
        .def_prop_ro("instances",
                     [](const SchematicCell &cell) {
                         std::vector<SchematicInstance> instances;
                         instances.reserve(cell.instances.size());
                         for (const auto &[_, instance] : cell.instances)
                             instances.push_back(instance);
                         return instances;
                     })
        .def_prop_ro("nets",
                     [](const SchematicCell &cell) {
                         std::vector<Net> nets;
                         nets.reserve(cell.nets.size());
                         for (const auto &[_, net] : cell.nets)
                             nets.push_back(net);
                         return nets;
                     })
        .def_prop_ro("text_schematic", [](const SchematicCell &cell) { return cell.textSchematic; })
        .def_prop_ro("systemverilog_schematic", [](const SchematicCell &cell) { return cell.systemVerilogSchematic; })
        .def_prop_ro("is_text_source", [](const SchematicCell &cell) { return cell.isTextSource(); })
        .def_prop_ro("spice_source_path", [](const SchematicCell &cell) { return cell.spiceSourcePath; })
        .def_prop_ro("source", [](const SchematicCell &cell) { return cell.sourceText(); });

    nb::class_<SchematicBuilder>(module, "SchematicBuilder")
        .def_prop_ro("id", &SchematicBuilder::id)
        .def_prop_ro("name", &SchematicBuilder::name)
        .def("symbol", &SchematicBuilder::symbol, nb::arg("name"), nb::arg("artwork") = "", nb::arg("source_cell") = 0)
        .def("pin", &SchematicBuilder::pin, nb::arg("symbol"), nb::arg("name"), nb::arg("direction") = "right",
             nb::arg("x") = 0, nb::arg("y") = 0, nb::arg("width") = 1, nb::arg("width_known") = true)
        .def("instance", &SchematicBuilder::instance, nb::arg("symbol"), nb::arg("name"), nb::arg("x") = 0,
             nb::arg("y") = 0)
        .def("net", &SchematicBuilder::net, nb::arg("name"), nb::arg("pins"), nb::arg("x") = 0, nb::arg("y") = 0)
        .def("component", &SchematicBuilder::component, nb::arg("type"), nb::arg("name") = "", nb::arg("x") = 0,
             nb::arg("y") = 0, nb::arg("parameters") = std::map<std::string, std::string>{})
        .def("set_parameter", &SchematicBuilder::set_parameter, nb::arg("instance"), nb::arg("name"), nb::arg("value"))
        .def("set_cell_parameter", &SchematicBuilder::set_cell_parameter, nb::arg("name"), nb::arg("value"))
        .def("remove_cell_parameter", &SchematicBuilder::remove_cell_parameter, nb::arg("name"))
        .def("netlist", &SchematicBuilder::netlist, nb::arg("mixed") = false)
        .def("wrap", &SchematicBuilder::wrap, nb::arg("instances"), nb::arg("name") = "")
        .def("cell", &SchematicBuilder::cell);

    nb::class_<SimulationDerivedTrace>(module, "SimulationDerivedTrace")
        .def(nb::init<>())
        .def_rw("name", &SimulationDerivedTrace::name)
        .def_rw("expression", &SimulationDerivedTrace::expression);

    nb::class_<SimulationOverlayRun>(module, "SimulationOverlayRun")
        .def(nb::init<>())
        .def_rw("run_id", &SimulationOverlayRun::runId)
        .def_rw("label", &SimulationOverlayRun::label);

    nb::class_<SimulationGraphMarker>(module, "SimulationGraphMarker")
        .def(nb::init<>())
        .def_rw("label", &SimulationGraphMarker::label)
        .def_rw("signal", &SimulationGraphMarker::signal)
        .def_rw("x", &SimulationGraphMarker::x)
        .def_rw("y", &SimulationGraphMarker::y)
        .def_rw("has_y", &SimulationGraphMarker::hasY);

    nb::class_<SimulationGraph>(module, "SimulationGraph")
        .def(nb::init<>())
        .def_rw("id", &SimulationGraph::id)
        .def_rw("name", &SimulationGraph::name)
        .def_rw("x_axis", &SimulationGraph::xAxis)
        .def_rw("y_signals", &SimulationGraph::ySignals)
        .def_rw("signal_labels", &SimulationGraph::signalLabels)
        .def_rw("y_axis_label", &SimulationGraph::yAxisLabel)
        .def_rw("x_min", &SimulationGraph::xMin)
        .def_rw("x_max", &SimulationGraph::xMax)
        .def_rw("y_min", &SimulationGraph::yMin)
        .def_rw("y_max", &SimulationGraph::yMax)
        .def_rw("view", &SimulationGraph::view)
        .def_rw("fft", &SimulationGraph::fft)
        .def_rw("fft_window", &SimulationGraph::fftWindow)
        .def_rw("fft_points", &SimulationGraph::fftPoints)
        .def_rw("show_measurements", &SimulationGraph::showMeasurements)
        .def_rw("derived_traces", &SimulationGraph::derivedTraces)
        .def_rw("overlays", &SimulationGraph::overlays)
        .def_rw("markers", &SimulationGraph::markers);

    nb::class_<SimulationCorner>(module, "SimulationCorner")
        .def(nb::init<>())
        .def_rw("name", &SimulationCorner::name)
        .def_rw("model_section", &SimulationCorner::modelSection)
        .def_rw("temperature", &SimulationCorner::temperature)
        .def_rw("source_overrides", &SimulationCorner::sourceOverrides)
        .def_rw("parameters", &SimulationCorner::parameters);

    nb::class_<SimulationSweep>(module, "SimulationSweep")
        .def(nb::init<>())
        .def_rw("parameter", &SimulationSweep::parameter)
        .def_rw("values", &SimulationSweep::values);

    nb::class_<SimulationMonteCarloOptions>(module, "SimulationMonteCarloOptions")
        .def(nb::init<>())
        .def_rw("enabled", &SimulationMonteCarloOptions::enabled)
        .def_rw("samples", &SimulationMonteCarloOptions::samples)
        .def_rw("temperature_sigma", &SimulationMonteCarloOptions::temperatureSigma)
        .def_rw("supply_sigma_percent", &SimulationMonteCarloOptions::supplySigmaPercent)
        .def_rw("supply_source", &SimulationMonteCarloOptions::supplySource)
        .def_rw("supply_nominal", &SimulationMonteCarloOptions::supplyNominal)
        .def_rw("parameter_sigma_percent", &SimulationMonteCarloOptions::parameterSigmaPercent)
        .def_rw("seed", &SimulationMonteCarloOptions::seed);

    nb::class_<SimulationMeasurementSpec>(module, "SimulationMeasurementSpec")
        .def(nb::init<>())
        .def_rw("name", &SimulationMeasurementSpec::name)
        .def_rw("metric", &SimulationMeasurementSpec::metric)
        .def_rw("signal", &SimulationMeasurementSpec::signal)
        .def_rw("input", &SimulationMeasurementSpec::input)
        .def_rw("voltage", &SimulationMeasurementSpec::voltage)
        .def_rw("threshold", &SimulationMeasurementSpec::threshold)
        .def_rw("tolerance", &SimulationMeasurementSpec::tolerance);

    nb::class_<SimulationItem>(module, "SimulationItem")
        .def(nb::init<>())
        .def_rw("id", &SimulationItem::id)
        .def_rw("name", &SimulationItem::name)
        .def_rw("schematic", &SimulationItem::schematic)
        .def_rw("simulator", &SimulationItem::simulator)
        .def_rw("analysis", &SimulationItem::analysis)
        .def_rw("start", &SimulationItem::start)
        .def_rw("stop", &SimulationItem::stop)
        .def_rw("step", &SimulationItem::step)
        .def_rw("use_initial_conditions", &SimulationItem::use_initial_conditions)
        .def_rw("directives", &SimulationItem::directives)
        .def_rw("temperature", &SimulationItem::temperature)
        .def_rw("model_section", &SimulationItem::modelSection)
        .def_rw("parameters", &SimulationItem::parameters)
        .def_rw("source_overrides", &SimulationItem::sourceOverrides)
        .def_rw("dc_source", &SimulationItem::dcSource)
        .def_rw("dc_start", &SimulationItem::dcStart)
        .def_rw("dc_stop", &SimulationItem::dcStop)
        .def_rw("dc_step", &SimulationItem::dcStep)
        .def_rw("ac_variation", &SimulationItem::acVariation)
        .def_rw("ac_points", &SimulationItem::acPoints)
        .def_rw("ac_start", &SimulationItem::acStart)
        .def_rw("ac_stop", &SimulationItem::acStop)
        .def_rw("noise_output", &SimulationItem::noiseOutput)
        .def_rw("noise_input", &SimulationItem::noiseInput)
        .def_rw("noise_variation", &SimulationItem::noiseVariation)
        .def_rw("noise_points", &SimulationItem::noisePoints)
        .def_rw("noise_start", &SimulationItem::noiseStart)
        .def_rw("noise_stop", &SimulationItem::noiseStop)
        .def_rw("tf_output", &SimulationItem::tfOutput)
        .def_rw("tf_source", &SimulationItem::tfSource)
        .def_rw("capture_signals", &SimulationItem::captureSignals)
        .def_rw("corners", &SimulationItem::corners)
        .def_rw("sweeps", &SimulationItem::sweeps)
        .def_rw("monte_carlo", &SimulationItem::monteCarlo)
        .def_rw("measurements", &SimulationItem::measurements)
        .def_rw("graphs", &SimulationItem::graphs);

    nb::enum_<JobState>(module, "JobState")
        .value("queued", JobState::queued)
        .value("running", JobState::running)
        .value("succeeded", JobState::succeeded)
        .value("failed", JobState::failed)
        .value("cancelled", JobState::cancelled)
        .value("timed_out", JobState::timed_out);

    nb::class_<SimulationTrace>(module, "SimulationTrace")
        .def(nb::init<>())
        .def_rw("name", &SimulationTrace::name)
        .def_rw("values", &SimulationTrace::values);

    nb::class_<SimulationMeasurementResult>(module, "SimulationMeasurementResult")
        .def(nb::init<>())
        .def_rw("name", &SimulationMeasurementResult::name)
        .def_rw("metric", &SimulationMeasurementResult::metric)
        .def_rw("value", &SimulationMeasurementResult::value)
        .def_rw("unit", &SimulationMeasurementResult::unit)
        .def_rw("valid", &SimulationMeasurementResult::valid)
        .def_rw("error", &SimulationMeasurementResult::error);

    nb::class_<SimulationResult>(module, "SimulationResult")
        .def(nb::init<>())
        .def_rw("state", &SimulationResult::state)
        .def_rw("analysis", &SimulationResult::analysis)
        .def_rw("x_axis", &SimulationResult::xAxis)
        .def_rw("x_unit", &SimulationResult::xUnit)
        .def_rw("time", &SimulationResult::time)
        .def_rw("traces", &SimulationResult::traces)
        .def_rw("scalars", &SimulationResult::scalars)
        .def_rw("measurements", &SimulationResult::measurements)
        .def_rw("simulator", &SimulationResult::simulator)
        .def_rw("simulator_version", &SimulationResult::simulator_version)
        .def_rw("error", &SimulationResult::error)
        .def_rw("warnings", &SimulationResult::warnings);

    nb::class_<DrcViolation>(module, "DrcViolation")
        .def(nb::init<>())
        .def_rw("schema_version", &DrcViolation::schemaVersion)
        .def_rw("severity", &DrcViolation::severity)
        .def_rw("rule", &DrcViolation::rule)
        .def_rw("message", &DrcViolation::message)
        .def_rw("object", &DrcViolation::object)
        .def_rw("location", &DrcViolation::location)
        .def_rw("cell", &DrcViolation::cell);

    nb::class_<ParasiticElement>(module, "ParasiticElement")
        .def(nb::init<>())
        .def_rw("schema_version", &ParasiticElement::schemaVersion)
        .def_rw("net", &ParasiticElement::net)
        .def_rw("resistance", &ParasiticElement::resistance)
        .def_rw("capacitance", &ParasiticElement::capacitance);

    nb::class_<VerificationReport>(module, "VerificationReport")
        .def(nb::init<>())
        .def_rw("schema_version", &VerificationReport::schemaVersion)
        .def_rw("drc", &VerificationReport::drc)
        .def_rw("lvs", &VerificationReport::lvs)
        .def_rw("pex", &VerificationReport::pex);

    nb::class_<LocalVerificationAdapter>(module, "LocalVerificationAdapter")
        .def(nb::init<>())
        .def(
            "run",
            [](LocalVerificationAdapter &adapter, const Project &project, std::optional<ObjectId> cell) {
                return adapter.run(project, cell);
            },
            nb::arg("project"), nb::arg("cell") = nb::none());

    nb::class_<LayoutDrcOptions>(module, "LayoutDrcOptions")
        .def(nb::init<>())
        .def_rw("work_directory", &LayoutDrcOptions::workDirectory)
        .def_rw("flatten", &LayoutDrcOptions::flatten)
        .def_rw("top_cell", &LayoutDrcOptions::topCell)
        .def_rw("suite", &LayoutDrcOptions::suite)
        .def_rw("timeout_seconds", &LayoutDrcOptions::timeoutSeconds)
        .def_rw("engine", &LayoutDrcOptions::engine)
        .def_rw("deck", &LayoutDrcOptions::deck);

    nb::class_<LayoutDrcResult>(module, "LayoutDrcResult")
        .def(nb::init<>())
        .def_rw("ok", &LayoutDrcResult::ok)
        .def_rw("layout", &LayoutDrcResult::layout)
        .def_rw("layout_name", &LayoutDrcResult::layoutName)
        .def_rw("engine", &LayoutDrcResult::engine)
        .def_rw("deck_path", &LayoutDrcResult::deckPath)
        .def_rw("gds_path", &LayoutDrcResult::gdsPath)
        .def_rw("report_path", &LayoutDrcResult::reportPath)
        .def_rw("log_path", &LayoutDrcResult::logPath)
        .def_rw("report", &LayoutDrcResult::report)
        .def_rw("error", &LayoutDrcResult::error);

    nb::class_<PackageSpec>(module, "PackageSpec")
        .def(nb::init<>())
        .def_rw("schema_version", &PackageSpec::schemaVersion)
        .def_rw("name", &PackageSpec::name)
        .def_rw("version", &PackageSpec::version)
        .def_rw("description", &PackageSpec::description)
        .def_rw("license", &PackageSpec::license)
        .def_rw("dependencies", &PackageSpec::dependencies)
        .def_rw("capabilities", &PackageSpec::capabilities)
        .def_rw("trusted", &PackageSpec::trusted)
        .def_rw("source_type", &PackageSpec::sourceType)
        .def_rw("source_url", &PackageSpec::sourceUrl)
        .def_rw("source_revision", &PackageSpec::sourceRevision)
        .def_rw("root", &PackageSpec::root)
        .def_prop_ro("pdk_id", [](const PackageSpec &spec) { return spec.pdk ? spec.pdk->id : std::string{}; });

    nb::class_<LockedPackage>(module, "LockedPackage")
        .def(nb::init<>())
        .def_rw("name", &LockedPackage::name)
        .def_rw("version", &LockedPackage::version)
        .def_rw("source", &LockedPackage::source)
        .def_rw("content_hash", &LockedPackage::contentHash)
        .def_rw("trusted", &LockedPackage::trusted)
        .def_rw("capabilities", &LockedPackage::capabilities)
        .def_rw("pdk_id", &LockedPackage::pdkId);

    nb::class_<Lockfile>(module, "Lockfile")
        .def(nb::init<>())
        .def_rw("schema_version", &Lockfile::schemaVersion)
        .def_rw("openvlsi_version", &Lockfile::openvlsiVersion)
        .def_rw("packages", &Lockfile::packages);

    nb::class_<PackageIndexEntry>(module, "PackageIndexEntry")
        .def(nb::init<>())
        .def_rw("spec", &PackageIndexEntry::spec)
        .def_rw("root", &PackageIndexEntry::root)
        .def_rw("logo", &PackageIndexEntry::logo)
        .def_rw("repository_url", &PackageIndexEntry::repositoryUrl)
        .def_rw("repository_revision", &PackageIndexEntry::repositoryRevision);

    nb::class_<ProvenanceDiagnostic>(module, "ProvenanceDiagnostic")
        .def(nb::init<>())
        .def_rw("code", &ProvenanceDiagnostic::code)
        .def_rw("message", &ProvenanceDiagnostic::message)
        .def_rw("package", &ProvenanceDiagnostic::package);

    nb::class_<PackageManager>(module, "PackageManager")
        .def_static("read_manifest", &PackageManager::readManifest, nb::arg("package_root"))
        .def_static("content_hash", &PackageManager::contentHash, nb::arg("package_root"))
        .def_static("install_git", &PackageManager::installGit, nb::arg("repository"), nb::arg("name"),
                    nb::arg("version"), nb::arg("url"), nb::arg("revision") = "")
        .def_static("install_git_package", &PackageManager::installGitPackage, nb::arg("repository"), nb::arg("url"),
                    nb::arg("revision") = "")
        .def_static("uninstall", &PackageManager::uninstallPackage, nb::arg("repository"), nb::arg("name"))
        .def_static("scan_index", &PackageManager::scanPackageIndex, nb::arg("index_root"))
        .def_static("resolve", &PackageManager::resolve, nb::arg("repositories"), nb::arg("requested"),
                    nb::arg("openvlsi_version"))
        .def_static("write_lockfile", &PackageManager::writeLockfile, nb::arg("path"), nb::arg("lockfile"))
        .def_static("read_lockfile", &PackageManager::readLockfile, nb::arg("path"))
        .def_static("verify_lockfile", &PackageManager::verifyLockfile, nb::arg("lockfile"), nb::arg("repositories"))
        .def_static("capabilities", &PackageManager::capabilities, nb::arg("package"))
        .def_static("provenance", &PackageManager::provenance, nb::arg("lockfile"), nb::arg("repositories"));

    nb::class_<Project>(module, "Project")
        .def(nb::init<>())
        .def_rw("name", &Project::name)
        .def_rw("identity", &Project::identity)
        .def_rw("grid", &Project::grid)
        .def_rw("units", &Project::units)
        .def_prop_ro("schema_version", [](const Project &) { return Project::schemaVersion; })
        .def_prop_ro("package_dependencies", [](const Project &project) { return project.packageDependencies; })
        .def_prop_ro("trusted_packages", [](const Project &project) { return project.trustedPackages; })
        .def_prop_ro("layers",
                     [](const Project &project) {
                         std::vector<Layer> layers;
                         layers.reserve(project.layers.size());
                         for (const auto &[_, layer] : project.layers)
                             layers.push_back(layer);
                         return layers;
                     })
        .def_prop_ro("layout_cells",
                     [](const Project &project) {
                         std::vector<LayoutCell> cells;
                         cells.reserve(project.layoutCells.size());
                         for (const auto &[_, cell] : project.layoutCells)
                             cells.push_back(cell);
                         return cells;
                     })
        .def("layout_cell",
             [](const Project &project, const std::string &name) { return findLayoutCell(project, name); })
        .def("layout_cell_by_id", [](const Project &project, ObjectId id) { return findLayoutCell(project, id); })
        .def_prop_ro("schematic_cells",
                     [](const Project &project) {
                         std::vector<SchematicCell> cells;
                         cells.reserve(project.schematicCells.size());
                         for (const auto &[_, cell] : project.schematicCells)
                             cells.push_back(cell);
                         return cells;
                     })
        .def("schematic_cell",
             [](const Project &project, const std::string &name) {
                 const auto cell = findSchematicCell(project, name);
                 if (!cell)
                     throw ProjectError("openvlsipy.schematic", "Schematic does not exist: " + name);
                 return *cell;
             })
        .def("schematic_cell_by_id",
             [](const Project &project, ObjectId id) {
                 const auto cell = project.schematicCells.find(id);
                 if (cell == project.schematicCells.end())
                     throw ProjectError("openvlsipy.schematic", "Schematic does not exist");
                 return cell->second;
             })
        .def(
            "create_schematic_cell",
            [](Project &project, const std::string &name, const std::string &pdk) {
                const auto id = openvlsi::createSchematicCell(project, name, pdk);
                return SchematicBuilder(project, id);
            },
            nb::arg("name"), nb::arg("pdk") = genericPdkId, nb::keep_alive<0, 1>())
        .def(
            "create_text_schematic_cell",
            [](Project &project, const std::string &name, const std::string &text, const std::string &pdk,
               const std::string &sourcePath) {
                const auto id = openvlsi::createTextSchematicCell(project, name, pdk, text, sourcePath);
                return SchematicBuilder(project, id);
            },
            nb::arg("name"), nb::arg("text") = "", nb::arg("pdk") = genericPdkId, nb::arg("source_path") = "",
            nb::keep_alive<0, 1>())
        .def(
            "create_systemverilog_cell",
            [](Project &project, const std::string &name, const std::string &source, const std::string &pdk) {
                const auto id = openvlsi::createSystemVerilogCell(project, name, pdk, source);
                return SchematicBuilder(project, id);
            },
            nb::arg("name"), nb::arg("source") = "", nb::arg("pdk") = genericPdkId, nb::keep_alive<0, 1>())
        .def(
            "edit_schematic_cell",
            [](Project &project, ObjectId id) {
                if (!project.schematicCells.contains(id))
                    throw ProjectError("openvlsipy.cell", "Schematic cell does not exist");
                return SchematicBuilder(project, id);
            },
            nb::arg("id"), nb::keep_alive<0, 1>())
        .def("remove_schematic_cell",
             [](Project &project, ObjectId id) {
                 openvlsi::removeSchematicCell(project, id);
                 return true;
             })
        .def(
            "schematic_netlist",
            [](const Project &project, ObjectId id, bool mixed) {
                const auto cell = project.schematicCells.find(id);
                if (cell == project.schematicCells.end())
                    throw ProjectError("openvlsipy.schematic", "Schematic does not exist");
                if (cell->second.textSchematic)
                    return openvlsi::buildSimulationNetlist(project, id);
                return openvlsi::buildSchematicNetlist(project, id, nullptr,
                                                  mixed ? SchematicNetlistMode::mixed : SchematicNetlistMode::analog,
                                                  openvlsi::pdkNetlistGeometryScale(cell->second.pdk));
            },
            nb::arg("schematic"), nb::arg("mixed") = false)
        .def_prop_ro("simulations",
                     [](const Project &project) {
                         std::vector<SimulationItem> simulations;
                         simulations.reserve(project.simulations.size());
                         for (const auto &[_, simulation] : project.simulations)
                             simulations.push_back(simulation);
                         return simulations;
                     })
        .def("simulation",
             [](const Project &project, const std::string &name) {
                 return project.simulations.at(resolveSimulationId(project, name));
             })
        .def(
            "create_simulation",
            [](Project &project, SimulationItem simulation) {
                if (simulation.name.empty())
                    throw ProjectError("openvlsipy.simulation", "Simulation name is required");
                openvlsi::requireUniqueSimulationName(project, simulation.name);
                simulation.id = project.nextId();
                const auto id = simulation.id;
                project.simulations.emplace(id, std::move(simulation));
                return project.simulations.at(id);
            },
            nb::arg("simulation"))
        .def(
            "update_simulation",
            [](Project &project, SimulationItem simulation) {
                if (!project.simulations.contains(simulation.id))
                    throw ProjectError("openvlsipy.simulation", "Simulation does not exist");
                openvlsi::requireUniqueSimulationName(project, simulation.name, simulation.id);
                const auto id = simulation.id;
                project.simulations.at(id) = std::move(simulation);
                return project.simulations.at(id);
            },
            nb::arg("simulation"))
        .def("remove_simulation",
             [](Project &project, const std::string &simulation) {
                 const auto id = resolveSimulationId(project, simulation);
                 openvlsi::removeSimulationItem(project, id);
                 return true;
             })
        .def(
            "run_simulation",
            [](const Project &project, const std::string &simulation, const std::string &projectRoot, int timeoutMs) {
                const auto id = resolveSimulationId(project, simulation);
                std::vector<std::string> notes;
                const auto request = openvlsi::simulationRequestForItem(
                    project, project.simulations.at(id),
                    projectRoot.empty() ? std::filesystem::path{} : std::filesystem::path(projectRoot), &notes);
                openvlsi::SimulationRunner runner;
                return runner.run(request, std::chrono::milliseconds(std::max(1, timeoutMs)));
            },
            nb::arg("simulation"), nb::arg("project_root") = "", nb::arg("timeout_ms") = 60000)
        .def_prop_ro("procedural_layouts",
                     [](const Project &project) {
                         std::vector<ProceduralLayout> layouts;
                         layouts.reserve(project.proceduralLayouts.size());
                         for (const auto &[_, layout] : project.proceduralLayouts)
                             layouts.push_back(layout);
                         return layouts;
                     })
        .def(
            "procedural_layout",
            [](const Project &project, const std::string &name) -> ProceduralLayout {
                for (const auto &[_, layout] : project.proceduralLayouts) {
                    if (layout.name == name)
                        return layout;
                }
                throw ProjectError("openvlsipy.procedural", "Procedural layout does not exist: " + name);
            },
            nb::arg("name"))
        .def(
            "create_procedural_layout",
            [](Project &project, const std::string &name, const std::string &pdk, const std::string &stack,
               const std::string &script, const std::map<std::string, std::string> &parameters) -> ProceduralLayout {
                std::vector<Parameter> entries;
                entries.reserve(parameters.size());
                for (const auto &[parameterName, value] : parameters)
                    entries.push_back({0, parameterName, value});
                const auto id = openvlsi::createProceduralLayout(project, name, pdk, script, stack, std::move(entries));
                return project.proceduralLayouts.at(id);
            },
            nb::arg("name"), nb::arg("pdk") = genericPdkId, nb::arg("stack") = "", nb::arg("script") = "",
            nb::arg("parameters") = std::map<std::string, std::string>{})
        .def(
            "regenerate_procedural_layout",
            [](Project &project, const std::string &name) -> ProceduralLayout {
                auto id = ObjectId{0};
                for (const auto &[candidate, layout] : project.proceduralLayouts) {
                    if (layout.name == name) {
                        id = candidate;
                        break;
                    }
                }
                if (id == 0)
                    throw ProjectError("openvlsipy.procedural", "Procedural layout does not exist: " + name);
                const auto result = openvlsi::regenerateProceduralLayout(project, id);
                if (!result.ok)
                    throw ProjectError("openvlsipy.procedural.generate", result.error);
                return project.proceduralLayouts.at(id);
            },
            nb::arg("name"))
        .def(
            "set_procedural_parameter",
            [](Project &project, const std::string &name, const std::string &parameterName,
               const std::string &value) -> ProceduralLayout {
                for (auto &[_, layout] : project.proceduralLayouts) {
                    if (layout.name != name)
                        continue;
                    const auto parameter = std::find_if(layout.parameters.begin(), layout.parameters.end(),
                                                        [&](const auto &entry) { return entry.name == parameterName; });
                    if (parameter == layout.parameters.end())
                        layout.parameters.push_back({project.nextId(), parameterName, value});
                    else
                        parameter->value = value;
                    return layout;
                }
                throw ProjectError("openvlsipy.procedural", "Procedural layout does not exist: " + name);
            },
            nb::arg("name"), nb::arg("parameter"), nb::arg("value"))
        .def(
            "add_layer",
            [](Project &project, const std::string &name, int number, int datatype) {
                openvlsi::addLayer(project, name, number, 0, {}, datatype);
                for (const auto &[_, layer] : project.layers) {
                    if (layer.name == name && layer.datatype == datatype)
                        return layer;
                }
                throw ProjectError("openvlsipy.layer", "Layer creation failed");
            },
            nb::arg("name"), nb::arg("number"), nb::arg("datatype") = 0)
        .def(
            "create_layout_cell",
            [](Project &project, const std::string &name, const std::string &pdk, const std::string &stack) {
                const auto id = openvlsi::createLayoutCell(project, name, pdk, resolveLayoutStack(pdk, stack));
                return LayoutBuilder(project, id);
            },
            nb::arg("name"), nb::arg("pdk") = genericPdkId, nb::arg("stack") = "", nb::keep_alive<0, 1>())
        .def(
            "create_layout_from_schematic",
            [](Project &project, ObjectId schematic, const std::string &name, const std::string &stack) {
                const auto cell = project.schematicCells.find(schematic);
                if (cell == project.schematicCells.end())
                    throw ProjectError("openvlsipy.schematic", "Schematic cell does not exist");
                const auto id = openvlsi::createLayoutFromSchematic(project, schematic, name,
                                                               resolveLayoutStack(cell->second.pdk, stack));
                return LayoutBuilder(project, id);
            },
            nb::arg("schematic"), nb::arg("name") = "", nb::arg("stack") = "", nb::keep_alive<0, 1>())
        .def(
            "edit_layout_cell",
            [](Project &project, ObjectId id) {
                if (!project.layoutCells.contains(id))
                    throw ProjectError("openvlsipy.cell", "Layout cell does not exist");
                return LayoutBuilder(project, id);
            },
            nb::arg("id"), nb::keep_alive<0, 1>())
        .def("remove_layout_cell", [](Project &project, ObjectId id) { return project.layoutCells.erase(id) != 0; })
        .def_prop_ro("implementation_runs",
                     [](const Project &project) {
                         std::vector<ImplementationRun> runs;
                         runs.reserve(project.implementationRuns.size());
                         for (const auto &[_, run] : project.implementationRuns)
                             runs.push_back(run);
                         return runs;
                     })
        .def(
            "implementation_run",
            [](const Project &project, const std::string &name) {
                const auto *run = findImplementationRun(project, name);
                if (!run)
                    throw ProjectError("openvlsipy.implementation", "Implementation run does not exist: " + name);
                return *run;
            },
            nb::arg("name"))
        .def(
            "implementation_run_by_id",
            [](const Project &project, ObjectId id) {
                const auto *run = findImplementationRun(project, id);
                if (!run)
                    throw ProjectError("openvlsipy.implementation", "Implementation run does not exist");
                return *run;
            },
            nb::arg("id"))
        .def(
            "create_implementation",
            [](Project &project, const ImplementationConfig &config) {
                const auto id = createImplementationRun(project, config);
                return project.implementationRuns.at(id);
            },
            nb::arg("config"))
        .def(
            "run_implementation",
            [](Project &project, ObjectId runId, const std::string &projectRoot, const std::string &fromStage,
               const std::string &toStage) {
                const auto root = projectRoot.empty() ? std::filesystem::current_path().string() : projectRoot;
                return executeImplementationForPython(project, runId, root, fromStage, toStage);
            },
            nb::arg("run_id"), nb::arg("project_root") = "", nb::arg("from_stage") = "synthesis",
            nb::arg("to_stage") = "final")
        .def(
            "import_implementation_layout",
            [](Project &project, ObjectId runId, const std::string &projectRoot, const std::string &layoutName,
               bool replace) {
                auto *run = findImplementationRun(project, runId);
                if (!run)
                    throw ProjectError("openvlsipy.implementation", "Implementation run does not exist");
                const auto package = implementationPackageForPdk(run->config.pdk);
                if (!package || !package->pdk)
                    throw ProjectError("openvlsipy.implementation",
                                       "No installed PDK package provides '" + run->config.pdk + "'");
                auto imported = importImplementationLayout(project, *run, *package, projectRoot, layoutName, replace);
                for (const auto &diagnostic : imported.diagnostics) {
                    if (diagnostic.severity == Diagnostic::Severity::error)
                        throw ProjectError(diagnostic.code, diagnostic.message);
                }
                return imported.layout;
            },
            nb::arg("run_id"), nb::arg("project_root"), nb::arg("layout_name") = "", nb::arg("replace") = false)
        .def("next_id", &Project::nextId)
        .def("validate", [](const Project &project) { return project.validate().size(); })
        .def("diagnostics", [](const Project &project) { return project.validate(); })
        .def("serialize", &Project::serialize)
        .def("save", &Project::save)
        .def_static("deserialize", &Project::deserialize)
        .def_static("load", [](const std::string &path) { return Project::load(path); });

    nb::enum_<VerificationSeverity>(module, "VerificationSeverity")
        .value("info", VerificationSeverity::info)
        .value("warning", VerificationSeverity::warning)
        .value("error", VerificationSeverity::error);

    nb::enum_<LvsObjectKind>(module, "LvsObjectKind")
        .value("device", LvsObjectKind::device)
        .value("net", LvsObjectKind::net)
        .value("port", LvsObjectKind::port)
        .value("planned_device", LvsObjectKind::plannedDevice)
        .value("instance", LvsObjectKind::instance)
        .value("shape", LvsObjectKind::shape)
        .value("connection", LvsObjectKind::connection)
        .value("geometry", LvsObjectKind::geometry);

    nb::enum_<LvsMatchMethod>(module, "LvsMatchMethod")
        .value("none", LvsMatchMethod::none)
        .value("linked", LvsMatchMethod::linked)
        .value("name", LvsMatchMethod::name)
        .value("topology", LvsMatchMethod::topology);

    nb::class_<LvsObjectRef>(module, "LvsObjectRef")
        .def(nb::init<>())
        .def_rw("kind", &LvsObjectRef::kind)
        .def_rw("cell", &LvsObjectRef::cell)
        .def_rw("id", &LvsObjectRef::id)
        .def_rw("name", &LvsObjectRef::name)
        .def_rw("bounds", &LvsObjectRef::bounds);

    nb::class_<LvsPinMatch>(module, "LvsPinMatch")
        .def(nb::init<>())
        .def_rw("schematic_pin", &LvsPinMatch::schematicPin)
        .def_rw("layout_pin", &LvsPinMatch::layoutPin)
        .def_rw("schematic_net", &LvsPinMatch::schematicNet)
        .def_rw("layout_net", &LvsPinMatch::layoutNet)
        .def_rw("matched", &LvsPinMatch::matched)
        .def_rw("swapped", &LvsPinMatch::swapped);

    nb::class_<LvsParameterMatch>(module, "LvsParameterMatch")
        .def(nb::init<>())
        .def_rw("name", &LvsParameterMatch::name)
        .def_rw("schematic_value", &LvsParameterMatch::schematicValue)
        .def_rw("layout_value", &LvsParameterMatch::layoutValue)
        .def_rw("matched", &LvsParameterMatch::matched);

    nb::class_<LvsCandidate>(module, "LvsCandidate")
        .def(nb::init<>())
        .def_rw("object", &LvsCandidate::object)
        .def_rw("score", &LvsCandidate::score)
        .def_rw("reason", &LvsCandidate::reason);

    nb::class_<LvsDeviceMatch>(module, "LvsDeviceMatch")
        .def(nb::init<>())
        .def_rw("schematic", &LvsDeviceMatch::schematic)
        .def_rw("layout", &LvsDeviceMatch::layout)
        .def_rw("schematic_type", &LvsDeviceMatch::schematicType)
        .def_rw("layout_type", &LvsDeviceMatch::layoutType)
        .def_rw("method", &LvsDeviceMatch::method)
        .def_rw("matched", &LvsDeviceMatch::matched)
        .def_rw("parameters_compared", &LvsDeviceMatch::parametersCompared)
        .def_rw("pins", &LvsDeviceMatch::pins)
        .def_rw("parameters", &LvsDeviceMatch::parameters);

    nb::class_<LvsNetMatch>(module, "LvsNetMatch")
        .def(nb::init<>())
        .def_rw("schematic", &LvsNetMatch::schematic)
        .def_rw("layout", &LvsNetMatch::layout)
        .def_rw("schematic_connections", &LvsNetMatch::schematicConnections)
        .def_rw("layout_connections", &LvsNetMatch::layoutConnections)
        .def_rw("matched", &LvsNetMatch::matched)
        .def_rw("ground", &LvsNetMatch::ground);

    nb::class_<LvsPortMatch>(module, "LvsPortMatch")
        .def(nb::init<>())
        .def_rw("schematic", &LvsPortMatch::schematic)
        .def_rw("layout", &LvsPortMatch::layout)
        .def_rw("schematic_direction", &LvsPortMatch::schematicDirection)
        .def_rw("layout_direction", &LvsPortMatch::layoutDirection)
        .def_rw("schematic_net", &LvsPortMatch::schematicNet)
        .def_rw("layout_net", &LvsPortMatch::layoutNet)
        .def_rw("matched", &LvsPortMatch::matched)
        .def_rw("direction_matched", &LvsPortMatch::directionMatched)
        .def_rw("net_matched", &LvsPortMatch::netMatched);

    nb::class_<LvsSummary>(module, "LvsSummary")
        .def(nb::init<>())
        .def_rw("devices_matched", &LvsSummary::devicesMatched)
        .def_rw("devices_mismatched", &LvsSummary::devicesMismatched)
        .def_rw("devices_schematic_only", &LvsSummary::devicesSchematicOnly)
        .def_rw("devices_layout_only", &LvsSummary::devicesLayoutOnly)
        .def_rw("nets_matched", &LvsSummary::netsMatched)
        .def_rw("nets_schematic_only", &LvsSummary::netsSchematicOnly)
        .def_rw("nets_layout_only", &LvsSummary::netsLayoutOnly)
        .def_rw("ports_matched", &LvsSummary::portsMatched);

    nb::class_<LvsMismatch>(module, "LvsMismatch")
        .def(nb::init<>())
        .def_rw("schema_version", &LvsMismatch::schemaVersion)
        .def_rw("category", &LvsMismatch::category)
        .def_rw("message", &LvsMismatch::message)
        .def_rw("severity", &LvsMismatch::severity)
        .def_rw("schematic_object", &LvsMismatch::schematicObject)
        .def_rw("layout_object", &LvsMismatch::layoutObject)
        .def_rw("schematic_ref", &LvsMismatch::schematicRef)
        .def_rw("layout_ref", &LvsMismatch::layoutRef)
        .def_rw("hint", &LvsMismatch::hint)
        .def_rw("candidates", &LvsMismatch::candidates);

    nb::class_<LvsOptions>(module, "LvsOptions")
        .def(nb::init<>())
        .def_rw("compare_parameters", &LvsOptions::compareParameters);

    nb::class_<LvsReport>(module, "LvsReport")
        .def(nb::init<>())
        .def_rw("schema_version", &LvsReport::schemaVersion)
        .def_rw("schematic", &LvsReport::schematic)
        .def_rw("layout", &LvsReport::layout)
        .def_rw("ports_matched", &LvsReport::portsMatched)
        .def_rw("summary", &LvsReport::summary)
        .def_rw("mismatches", &LvsReport::mismatches)
        .def_rw("devices", &LvsReport::devices)
        .def_rw("nets", &LvsReport::nets)
        .def_rw("ports", &LvsReport::ports)
        .def_prop_ro("matched", &LvsReport::matched);

    nb::class_<PexOptions>(module, "PexOptions")
        .def(nb::init<>())
        .def_rw("metal_sheet_resistance", &PexOptions::metalSheetResistance)
        .def_rw("poly_sheet_resistance", &PexOptions::polySheetResistance)
        .def_rw("diffusion_sheet_resistance", &PexOptions::diffusionSheetResistance)
        .def_rw("via_resistance", &PexOptions::viaResistance)
        .def_rw("area_capacitance", &PexOptions::areaCapacitance)
        .def_rw("perimeter_capacitance", &PexOptions::perimeterCapacitance)
        .def_rw("extract_placed_devices", &PexOptions::extractPlacedDevices)
        .def_rw("extract_planned_devices", &PexOptions::extractPlannedDevices)
        .def_rw("extract_polygon_devices", &PexOptions::extractPolygonDevices)
        .def_rw("extraction_rules_path", &PexOptions::extractionRulesPath)
        .def_rw("include_linked_schematic", &PexOptions::includeLinkedSchematic)
        .def_rw("schematic_name", &PexOptions::schematicName);

    nb::class_<PexNet>(module, "PexNet")
        .def(nb::init<>())
        .def_rw("schema_version", &PexNet::schemaVersion)
        .def_rw("net", &PexNet::net)
        .def_rw("name", &PexNet::name)
        .def_rw("node", &PexNet::node)
        .def_rw("ports", &PexNet::ports)
        .def_rw("area", &PexNet::area)
        .def_rw("perimeter", &PexNet::perimeter)
        .def_rw("resistance", &PexNet::resistance)
        .def_rw("capacitance", &PexNet::capacitance)
        .def_rw("via_count", &PexNet::viaCount);

    nb::class_<PexDevice>(module, "PexDevice")
        .def(nb::init<>())
        .def_rw("schema_version", &PexDevice::schemaVersion)
        .def_rw("instance", &PexDevice::instance)
        .def_rw("name", &PexDevice::name)
        .def_rw("type", &PexDevice::type)
        .def_rw("model", &PexDevice::model)
        .def_rw("pins", &PexDevice::pins)
        .def_rw("nodes", &PexDevice::nodes)
        .def_rw("spice_line", &PexDevice::spiceLine);

    nb::class_<PexReport>(module, "PexReport")
        .def(nb::init<>())
        .def_rw("schema_version", &PexReport::schemaVersion)
        .def_rw("layout_cell", &PexReport::layoutCell)
        .def_rw("layout", &PexReport::layout)
        .def_rw("pdk", &PexReport::pdk)
        .def_rw("schematic_cell", &PexReport::schematicCell)
        .def_rw("schematic", &PexReport::schematic)
        .def_rw("spice_text", &PexReport::spiceText)
        .def_rw("nets", &PexReport::nets)
        .def_rw("devices", &PexReport::devices)
        .def_rw("notes", &PexReport::notes)
        .def_rw("devices_from_layout", &PexReport::devicesFromLayout)
        .def_rw("devices_from_schematic", &PexReport::devicesFromSchematic)
        .def_rw("total_resistance", &PexReport::totalResistance)
        .def_rw("total_capacitance", &PexReport::totalCapacitance);

    module.def("run_pex", &runPex, nb::arg("project"), nb::arg("layout"), nb::arg("options") = PexOptions{});
    module.def(
        "run_pex_named",
        [](Project &project, const std::string &layoutName, const PexOptions &options) {
            ObjectId layout = 0;
            for (const auto &[id, cell] : project.layoutCells) {
                if (cell.name == layoutName) {
                    layout = id;
                    break;
                }
            }
            if (layout == 0)
                throw ProjectError("openvlsipy.layout", "Unknown layout: " + layoutName);
            return runPex(project, layout, options);
        },
        nb::arg("project"), nb::arg("layout"), nb::arg("options") = PexOptions{});
    module.def("pex_report_json", &serializePexReport, nb::arg("report"));
    module.def("schematic_for_layout", &schematicForLayout, nb::arg("project"), nb::arg("layout"));
    module.def("run_lvs", &runLvs, nb::arg("project"), nb::arg("schematic"), nb::arg("layout"),
               nb::arg("options") = LvsOptions{});
    module.def("lvs_report_json", &serializeLvsReport, nb::arg("report"));
    module.def(
        "run_lvs_named",
        [](const Project &project, const std::string &schematicName, const std::string &layoutName) {
            ObjectId schematic = 0;
            ObjectId layout = 0;
            for (const auto &[id, cell] : project.schematicCells) {
                if (cell.name == schematicName) {
                    schematic = id;
                    break;
                }
            }
            for (const auto &[id, cell] : project.layoutCells) {
                if (cell.name == layoutName) {
                    layout = id;
                    break;
                }
            }
            if (schematic == 0)
                throw ProjectError("openvlsipy.schematic", "Unknown schematic: " + schematicName);
            if (layout == 0)
                throw ProjectError("openvlsipy.layout", "Unknown layout: " + layoutName);
            return runLvs(project, schematic, layout);
        },
        nb::arg("project"), nb::arg("schematic"), nb::arg("layout"));

    module.def(
        "run_layout_drc",
        [](const Project &project, ObjectId layout, const LayoutDrcOptions &options) {
            return runLayoutDrc(project, layout, options);
        },
        nb::arg("project"), nb::arg("layout"), nb::arg("options") = LayoutDrcOptions{});
    module.def(
        "run_layout_drc_named",
        [](const Project &project, const std::string &layout, const LayoutDrcOptions &options) {
            return runLayoutDrc(project, resolveLayoutId(project, layout), options);
        },
        nb::arg("project"), nb::arg("layout"), nb::arg("options") = LayoutDrcOptions{});
    module.def("parse_lyrdb_text", &parseGdsCheckLyrdbText, nb::arg("text"));
    module.def("parse_lyrdb", &parseGdsCheckLyrdb, nb::arg("path"));
    module.def("verification_report_json", &serializeVerificationReport, nb::arg("report"));
    module.def(
        "run_local_verification",
        [](const Project &project, std::optional<ObjectId> cell) {
            LocalVerificationAdapter adapter;
            return adapter.run(project, cell);
        },
        nb::arg("project"), nb::arg("cell") = nb::none());
    module.def("package_matches_pdk", &packageMatchesPdk, nb::arg("package"), nb::arg("pdk"));
    module.def(
        "resolve_pdk_stack",
        [](const std::string &pdk, const std::string &requested) {
            if (pdk.empty() || pdk == genericPdkId)
                return requested;
            const auto pdks = findPdks(pdk);
            if (pdks.empty())
                return requested;
            const auto manifest = PackageManager::readManifest(pdks.front().packageRoot);
            if (!manifest.pdk)
                return requested;
            return resolvePdkStack(*manifest.pdk, requested);
        },
        nb::arg("pdk"), nb::arg("requested") = "");

    module.def("load_project", [](const std::string &path) { return Project::load(path); });
    module.def("new_project", [](const std::string &name) {
        Project project;
        project.identity = "project-" + name;
        project.name = name;
        return project;
    });
    module.def("create_project", [](const std::string &path, const std::string &name) {
        Project project;
        project.identity = "project-" + name;
        project.name = name;
        project.save(path);
        return project;
    });

    // The PCell helper layer lives in python/openvlsipy_pcells.py. Import it when it
    // is available so package code can use openvlsipy.pcells without a second import.
    try {
        module.attr("pcells") = nb::module_::import_("openvlsipy_pcells");
    } catch (const nb::python_error &) {
        PyErr_Clear();
    }

    // The simulation metrics loader is pure Python and can also be imported
    // directly as openvlsipy_sim. Attach it as openvlsipy.sim when available.
    try {
        module.attr("sim") = nb::module_::import_("openvlsipy_sim");
    } catch (const nb::python_error &) {
        PyErr_Clear();
    }
}
