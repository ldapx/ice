#include "icelib/operations.hpp"

namespace ice {
namespace {
template <typename Map> auto &require(Map &map, ObjectId id, const char *code, const char *message) {
    auto it = map.find(id);
    if (it == map.end())
        throw ProjectError(code, message);
    return it->second;
}
} // namespace

ObjectId createSchematicCell(Project &project, std::string name) {
    if (name.empty())
        throw ProjectError("schematic.cell", "Schematic cell name must not be empty");
    ObjectId id = project.nextId();
    project.schematicCells.emplace(id, SchematicCell{id, std::move(name)});
    return id;
}
ObjectId addSymbol(Project &project, ObjectId cellId, std::string name, std::string artwork) {
    if (name.empty())
        throw ProjectError("schematic.symbol", "Symbol name must not be empty");
    auto &cell = require(project.schematicCells, cellId, "schematic.cell", "Schematic cell does not exist");
    ObjectId id = project.nextId();
    cell.symbols.emplace(id, Symbol{id, std::move(name), std::move(artwork)});
    return id;
}
ObjectId addSchematicInstance(Project &project, ObjectId cellId, ObjectId symbolId, std::string name, Coordinate x,
                              Coordinate y) {
    if (name.empty())
        throw ProjectError("schematic.instance", "Instance name must not be empty");
    auto &cell = require(project.schematicCells, cellId, "schematic.cell", "Schematic cell does not exist");
    if (!cell.symbols.contains(symbolId))
        throw ProjectError("schematic.symbol", "Symbol does not exist");
    ObjectId id = project.nextId();
    cell.instances.emplace(id, SchematicInstance{id, symbolId, std::move(name), x, y});
    return id;
}
ObjectId createLayoutCell(Project &project, std::string name) {
    if (name.empty())
        throw ProjectError("layout.cell", "Layout cell name must not be empty");
    ObjectId id = project.nextId();
    project.layoutCells.emplace(id, LayoutCell{id, std::move(name)});
    return id;
}
ObjectId addLayer(Project &project, std::string name, int number) {
    if (name.empty())
        throw ProjectError("layout.layer", "Layer name must not be empty");
    if (number < 0)
        throw ProjectError("layout.layer", "Layer number must not be negative");
    ObjectId id = project.nextId();
    project.layers.emplace(id, Layer{id, std::move(name), number});
    return id;
}
ObjectId addRectangle(Project &project, ObjectId cellId, ObjectId layerId, Rect rectangle,
                      std::optional<ObjectId> net) {
    if (!rectangle.valid())
        throw ProjectError("layout.shape", "Rectangle must have positive dimensions");
    auto &cell = require(project.layoutCells, cellId, "layout.cell", "Layout cell does not exist");
    if (!project.layers.contains(layerId))
        throw ProjectError("layout.layer", "Layer does not exist");
    ObjectId id = project.nextId();
    cell.shapes.emplace(id, Shape{id, layerId, net, rectangle});
    return id;
}
void moveRectangle(Project &project, ObjectId cellId, ObjectId shapeId, Coordinate dx, Coordinate dy) {
    auto &cell = require(project.layoutCells, cellId, "layout.cell", "Layout cell does not exist");
    auto &shape = require(cell.shapes, shapeId, "layout.shape", "Shape does not exist");
    shape.rectangle.x += dx;
    shape.rectangle.y += dy;
    for (auto &point : shape.polygon) {
        point.x += dx;
        point.y += dy;
    }
}
} // namespace ice
