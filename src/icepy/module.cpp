#include "icelib/core.hpp"
#include "icelib/operations.hpp"

#include <nanobind/nanobind.h>
#include <nanobind/stl/filesystem.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <string>
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

    std::string layer(const std::string &name, int number = -1) {
        if (name.empty())
            throw ProjectError("icepy.layer", "Layer name must not be empty");
        for (const auto &[_, existing] : project_->layers) {
            if (existing.name == name)
                return name;
        }
        int layerNumber = number;
        if (layerNumber < 0) {
            layerNumber = 0;
            for (const auto &[_, existing] : project_->layers)
                layerNumber = std::max(layerNumber, existing.number);
            ++layerNumber;
        }
        ice::addLayer(*project_, name, layerNumber);
        return name;
    }

    ObjectId rectangle(const std::string &layer, Coordinate x, Coordinate y, Coordinate width, Coordinate height) {
        return ice::addRectangle(*project_, cellId_, layerId(layer), Rect{x, y, width, height});
    }

    ObjectId polygon(const std::string &layer, const std::vector<LayoutPoint> &points) {
        return ice::addPolygon(*project_, cellId_, layerId(layer), points);
    }

    ObjectId port(const std::string &name, const std::string &layer, Coordinate x, Coordinate y, Coordinate width,
                  Coordinate height) {
        return ice::addPort(*project_, cellId_, name, layerId(layer), Rect{x, y, width, height});
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
    ObjectId layerId(const std::string &name) {
        layer(name);
        for (const auto &[id, layer] : project_->layers) {
            if (layer.name == name)
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
        .def_rw("polygon", &Shape::polygon);

    nb::class_<Port>(module, "Port")
        .def(nb::init<>())
        .def_rw("id", &Port::id)
        .def_rw("name", &Port::name)
        .def_rw("layer", &Port::layer)
        .def_rw("rectangle", &Port::rectangle);

    nb::class_<LayoutInstance>(module, "LayoutInstance")
        .def(nb::init<>())
        .def_rw("id", &LayoutInstance::id)
        .def_rw("cell", &LayoutInstance::cell)
        .def_rw("name", &LayoutInstance::name)
        .def_rw("x", &LayoutInstance::x)
        .def_rw("y", &LayoutInstance::y)
        .def_rw("orientation", &LayoutInstance::orientation);

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
        .def("layer", &LayoutBuilder::layer, nb::arg("name"), nb::arg("number") = -1)
        .def("rectangle", &LayoutBuilder::rectangle, nb::arg("layer"), nb::arg("x"), nb::arg("y"), nb::arg("width"),
             nb::arg("height"))
        .def("polygon", &LayoutBuilder::polygon, nb::arg("layer"), nb::arg("points"))
        .def("port", &LayoutBuilder::port, nb::arg("name"), nb::arg("layer"), nb::arg("x"), nb::arg("y"),
             nb::arg("width"), nb::arg("height"))
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
        .def("add_layer",
             [](Project &project, const std::string &name, int number) {
                 ice::addLayer(project, name, number);
                 for (const auto &[_, layer] : project.layers) {
                     if (layer.name == name)
                         return layer;
                 }
                 throw ProjectError("icepy.layer", "Layer creation failed");
             })
        .def(
            "create_layout_cell",
            [](Project &project, const std::string &name, const std::string &pdk) {
                const auto id = ice::createLayoutCell(project, name, pdk);
                return LayoutBuilder(project, id);
            },
            nb::arg("name"), nb::arg("pdk") = genericPdkId, nb::keep_alive<0, 1>())
        .def(
            "edit_layout_cell",
            [](Project &project, ObjectId id) {
                if (!project.layoutCells.contains(id))
                    throw ProjectError("icepy.cell", "Layout cell does not exist");
                return LayoutBuilder(project, id);
            },
            nb::arg("id"), nb::keep_alive<0, 1>())
        .def("remove_layout_cell", [](Project &project, ObjectId id) { return project.layoutCells.erase(id) != 0; })
        .def("next_id", &Project::nextId)
        .def("validate", [](const Project &project) { return project.validate().size(); })
        .def("diagnostics", [](const Project &project) { return project.validate(); })
        .def("serialize", &Project::serialize)
        .def("save", &Project::save)
        .def_static("deserialize", &Project::deserialize)
        .def_static("load", &Project::load);

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
}
