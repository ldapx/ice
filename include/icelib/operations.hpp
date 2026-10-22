#pragma once

#include "icelib/core.hpp"
#include "icelib/geometry.hpp"

namespace ice {

ObjectId createSchematicCell(Project &project, std::string name);
ObjectId addSymbol(Project &project, ObjectId cellId, std::string name, std::string artwork = {});
ObjectId addSchematicInstance(Project &project, ObjectId cellId, ObjectId symbolId, std::string name, Coordinate x,
                              Coordinate y);
ObjectId createLayoutCell(Project &project, std::string name);
ObjectId addLayer(Project &project, std::string name, int number);
ObjectId addRectangle(Project &project, ObjectId cellId, ObjectId layerId, Rect rectangle,
                      std::optional<ObjectId> net = std::nullopt);
void moveRectangle(Project &project, ObjectId cellId, ObjectId shapeId, Coordinate dx, Coordinate dy);

} // namespace ice
