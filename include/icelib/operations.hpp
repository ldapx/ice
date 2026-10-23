#pragma once

#include "icelib/core.hpp"
#include "icelib/geometry.hpp"

#include <string>
#include <vector>

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

// Converts a schematic cell into a SPICE netlist so a simulation can run the
// drawn circuit instead of hand-typed directives. Each placed symbol becomes
// one SPICE element; nets become SPICE nodes. Returns the netlist lines
// (without a trailing `.end`, which the simulator adapter appends).
//
// Symbol names are mapped to SPICE elements case-insensitively:
//   resistor -> R, capacitor -> C, inductor -> L, diode -> D, voltage source -> V,
//   ground   -> reference node (0). Anything else is emitted as a comment.
// Element values are read from the instance parameter named "value" (falling
// back to any single parameter); when absent a placeholder is emitted.
//
// Limitation: pins are identified at symbol level, so a symbol instantiated
// more than once in the same cell cannot be resolved to a unique instance for
// netlisting. Such ambiguous instances are skipped and reported via notes.
std::vector<std::string> buildSchematicNetlist(const Project &project, ObjectId cellId,
                                               std::vector<std::string> *notes = nullptr);

} // namespace ice
