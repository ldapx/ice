#pragma once

#include "icelib/core.hpp"
#include "icelib/geometry.hpp"

#include <string>
#include <vector>

namespace ice {

ObjectId createSchematicCell(Project &project, std::string name, std::string pdk = genericPdkId);
ObjectId addSymbol(Project &project, ObjectId cellId, std::string name, std::string artwork = {},
                   ObjectId sourceCell = 0);
ObjectId addSchematicInstance(Project &project, ObjectId cellId, ObjectId symbolId, std::string name, Coordinate x,
                              Coordinate y);
// The SPICE element letter a schematic symbol maps to, or 0 when unsupported.
// Voltage and current sources both return their source letter so callers can
// derive reference designators consistently with netlist generation.
char spiceElementForSymbol(const std::string &symbolName);
// Copies a symbol's default parameters onto an instance, assigning fresh stable
// ids. Parameters whose names already exist on the instance are left untouched.
// Returns how many defaults were added.
std::size_t seedDefaultParameters(Project &project, SchematicInstance &instance,
                                  const std::vector<Parameter> &defaults);
ObjectId createLayoutCell(Project &project, std::string name, std::string pdk = genericPdkId);
// Creates a layout cell using the schematic's technology and projects every
// schematic instance into a movable planned-device box. Nets are projected as
// straight connections between those boxes. The layout name defaults to the
// schematic name and is made unique when that name is already in use. Returns
// the new layout cell id.
ObjectId createLayoutFromSchematic(Project &project, ObjectId schematicId, std::string name = {});
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
//   resistor -> R, capacitor -> C, inductor -> L, diode -> D,
//   voltage source -> V, current source -> I,
//   ground -> reference node (0). Anything else is emitted as a comment.
// A raw "value" parameter is emitted verbatim. Otherwise voltage/current
// sources support structured waveform parameters: pulse sources produce
// PULSE(v1 v2 td tr tf pw per ncycles), where ncycles 0 means continuous,
// and sine sources produce SIN(vo va freq td theta phi). Other elements read
// their quantity parameter
// (resistance, capacitance, ...), falling back to any single parameter; when
// absent a placeholder is emitted.
//
// Limitation: pins are identified at symbol level, so a symbol instantiated
// more than once in the same cell cannot be resolved to a unique instance for
// netlisting. Such ambiguous instances are skipped and reported via notes.
std::vector<std::string> buildSchematicNetlist(const Project &project, ObjectId cellId,
                                               std::vector<std::string> *notes = nullptr);

} // namespace ice
