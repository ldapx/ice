#include "icelib/core.hpp"
#include "icelib/implementation.hpp"
#include "icelib/operations.hpp"
#include "icelib/packages.hpp"
#include "icelib/pdk.hpp"
#include "icelib/pex.hpp"
#include "icelib/verification.hpp"

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
using namespace ice;

namespace {

Orientation parseOrientation(const std::string &text) {
    static const std::map<std::string, Orientation> values = {
        {"r0", Orientation::r0}, {"r90", Orientation::r90}, {"r180", Orientation::r180}, {"r270", Orientation::r270},
        {"mx", Orientation::mx}, {"my", Orientation::my},   {"m45", Orientation::m45},   {"m135", Orientation::m135}};
    const auto it = values.find(text);
    if (it == values.end())
        throw ProjectError("icepy.orientation", "Unknown orientation: " + text);
    return it->second;
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
            throw ProjectError("icepy.layer", "Layer name must not be empty");
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
        ice::addLayer(*project_, name, layerNumber, 0, target.first, datatype, target.second);
        return name;
    }

    ObjectId rectangle(const std::string &layer, Coordinate x, Coordinate y, Coordinate width, Coordinate height) {
        return ice::addRectangle(*project_, cellId_, layerId(layer), Rect{x, y, width, height});
    }

    ObjectId polygon(const std::string &layer, const std::vector<LayoutPoint> &points) {
        return ice::addPolygon(*project_, cellId_, layerId(layer), points);
    }

    ObjectId layoutNet(const std::string &name) {
        if (name.empty())
            return 0;
        auto &cell = project_->layoutCells.at(cellId_);
        for (const auto &[id, net] : cell.nets) {
            if (net.name == name)
                return id;
        }
        return ice::createLayoutNet(*project_, cellId_, name);
    }

    ObjectId port(const std::string &name, const std::string &layer, Coordinate x, Coordinate y, Coordinate width,
                  Coordinate height, const std::string &direction = "inout", const std::string &side = "left",
                  const std::string &net = {}, const std::string &label = {}) {
        const auto parsedDirection = ice::parsePortDirection(direction);
        const auto parsedSide = ice::parsePortSide(side);
        if (!parsedDirection || !parsedSide)
            throw ProjectError("icepy.port", "Unknown port direction or side");
        return ice::addPort(*project_, cellId_, name, layerId(layer), Rect{x, y, width, height}, *parsedDirection,
                            *parsedSide, layoutNet(net), label);
    }

    bool updatePort(const Port &port) { return ice::updatePort(*project_, cellId_, port); }

    void removePort(ObjectId portId) { ice::removePort(*project_, cellId_, portId); }

    void assignPortNet(ObjectId portId, const std::string &net) {
        ice::assignPortNet(*project_, cellId_, portId, layoutNet(net));
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
            throw ProjectError("icepy.instance", "Unknown layout cell: " + cellName);
        if (childId == cellId_)
            throw ProjectError("icepy.instance", "A layout cell cannot instance itself");
        auto &cell = project_->layoutCells.at(cellId_);
        const auto &childCell = project_->layoutCells.at(childId);
        if (!cell.pdk.empty() && !childCell.pdk.empty() && cell.pdk != childCell.pdk)
            throw ProjectError("icepy.instance", "Parent and child layouts must use the same PDK");
        if (cell.stack != childCell.stack)
            throw ProjectError("icepy.instance", "Parent and child layouts must use the same PDK stack");
        const auto id = project_->nextId();
        std::string name = instanceName;
        if (name.empty())
            name = "instance_" + std::to_string(id);
        cell.instances.emplace(id, LayoutInstance{id, childId, name, x, y, parseOrientation(orientation)});
        return id;
    }

    void parameter(const std::string &name, const std::string &value) {
        if (name.empty())
            throw ProjectError("icepy.parameter", "Parameter name must not be empty");
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

    LayoutCell cell() const {
        const auto it = project_->layoutCells.find(cellId_);
        if (it == project_->layoutCells.end())
            throw ProjectError("icepy.cell", "Layout cell no longer exists");
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
        throw ProjectError("icepy.layer", "Layer does not exist: " + name);
    }

    Project *project_ = nullptr;
    ObjectId cellId_ = 0;
};

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
        throw ProjectError("icepy.implementation", std::string("Invalid implementation stage: ") + label);
    return *parsed;
}

ImplementationExecutionResult executeImplementationForPython(Project &project, ObjectId runId,
                                                             const std::string &projectRoot,
                                                             const std::string &fromText, const std::string &toText) {
    auto *run = findImplementationRun(project, runId);
    if (!run)
        throw ProjectError("icepy.implementation", "Implementation run does not exist");
    const auto package = implementationPackageForPdk(run->config.pdk);
    if (!package || !package->pdk)
        throw ProjectError("icepy.implementation", "No installed PDK package provides '" + run->config.pdk + "'");
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

NB_MODULE(icepy, module) {
    module.doc() = "Python access to the ICE canonical project model and layout database.";

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
        .def_rw("source_instance", &Port::sourceInstance)
        .def_rw("source_port", &Port::sourcePort);

    nb::class_<LayoutInstance>(module, "LayoutInstance")
        .def(nb::init<>())
        .def_rw("id", &LayoutInstance::id)
        .def_rw("cell", &LayoutInstance::cell)
        .def_rw("name", &LayoutInstance::name)
        .def_rw("x", &LayoutInstance::x)
        .def_rw("y", &LayoutInstance::y)
        .def_rw("orientation", &LayoutInstance::orientation);

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
        .def_prop_ro("generated_by", [](const LayoutCell &cell) { return cell.generatedBy; })
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
        .def("set_library", &LayoutBuilder::set_library, nb::arg("library"))
        .def("set_device", &LayoutBuilder::set_device, nb::arg("device"))
        .def("cell", &LayoutBuilder::cell);

    nb::class_<Project>(module, "Project")
        .def(nb::init<>())
        .def_rw("name", &Project::name)
        .def_rw("identity", &Project::identity)
        .def_rw("grid", &Project::grid)
        .def_rw("units", &Project::units)
        .def_prop_ro("schema_version", [](const Project &) { return Project::schemaVersion; })
        .def_prop_ro("package_dependencies", [](const Project &project) { return project.packageDependencies; })
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
        .def(
            "add_layer",
            [](Project &project, const std::string &name, int number, int datatype) {
                ice::addLayer(project, name, number, 0, {}, datatype);
                for (const auto &[_, layer] : project.layers) {
                    if (layer.name == name && layer.datatype == datatype)
                        return layer;
                }
                throw ProjectError("icepy.layer", "Layer creation failed");
            },
            nb::arg("name"), nb::arg("number"), nb::arg("datatype") = 0)
        .def(
            "create_layout_cell",
            [](Project &project, const std::string &name, const std::string &pdk, const std::string &stack) {
                const auto id = ice::createLayoutCell(project, name, pdk, resolveLayoutStack(pdk, stack));
                return LayoutBuilder(project, id);
            },
            nb::arg("name"), nb::arg("pdk") = genericPdkId, nb::arg("stack") = "", nb::keep_alive<0, 1>())
        .def(
            "edit_layout_cell",
            [](Project &project, ObjectId id) {
                if (!project.layoutCells.contains(id))
                    throw ProjectError("icepy.cell", "Layout cell does not exist");
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
                    throw ProjectError("icepy.implementation", "Implementation run does not exist: " + name);
                return *run;
            },
            nb::arg("name"))
        .def(
            "implementation_run_by_id",
            [](const Project &project, ObjectId id) {
                const auto *run = findImplementationRun(project, id);
                if (!run)
                    throw ProjectError("icepy.implementation", "Implementation run does not exist");
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
                    throw ProjectError("icepy.implementation", "Implementation run does not exist");
                const auto package = implementationPackageForPdk(run->config.pdk);
                if (!package || !package->pdk)
                    throw ProjectError("icepy.implementation",
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

    nb::class_<LvsMismatch>(module, "LvsMismatch")
        .def(nb::init<>())
        .def_rw("schema_version", &LvsMismatch::schemaVersion)
        .def_rw("category", &LvsMismatch::category)
        .def_rw("message", &LvsMismatch::message)
        .def_rw("severity", &LvsMismatch::severity)
        .def_rw("schematic_object", &LvsMismatch::schematicObject)
        .def_rw("layout_object", &LvsMismatch::layoutObject);

    nb::class_<LvsOptions>(module, "LvsOptions")
        .def(nb::init<>())
        .def_rw("compare_parameters", &LvsOptions::compareParameters);

    nb::class_<LvsReport>(module, "LvsReport")
        .def(nb::init<>())
        .def_rw("schema_version", &LvsReport::schemaVersion)
        .def_rw("schematic", &LvsReport::schematic)
        .def_rw("layout", &LvsReport::layout)
        .def_rw("mismatches", &LvsReport::mismatches)
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
                throw ProjectError("icepy.layout", "Unknown layout: " + layoutName);
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
                throw ProjectError("icepy.schematic", "Unknown schematic: " + schematicName);
            if (layout == 0)
                throw ProjectError("icepy.layout", "Unknown layout: " + layoutName);
            return runLvs(project, schematic, layout);
        },
        nb::arg("project"), nb::arg("schematic"), nb::arg("layout"));

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

    // The PCell helper layer lives in python/icepy_pcells.py. Import it when it
    // is available so package code can use icepy.pcells without a second import.
    try {
        module.attr("pcells") = nb::module_::import_("icepy_pcells");
    } catch (const nb::python_error &) {
        PyErr_Clear();
    }

    // The simulation metrics loader is pure Python and can also be imported
    // directly as icepy_sim. Attach it as icepy.sim when available.
    try {
        module.attr("sim") = nb::module_::import_("icepy_sim");
    } catch (const nb::python_error &) {
        PyErr_Clear();
    }
}
