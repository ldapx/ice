#pragma once

#include "openvlsilib/core.hpp"
#include "openvlsilib/geometry.hpp"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace openvlsi {

ObjectId createSchematicCell(Project &project, std::string name, std::string pdk = genericPdkId);
// Creates a schematic cell whose source is a raw SPICE text document. The cell
// still participates in the schematic hierarchy and project serialization.
ObjectId createTextSchematicCell(Project &project, std::string name, std::string pdk = genericPdkId,
                                 std::string spiceText = {}, std::string spiceSourcePath = {});
// Creates a schematic cell whose source is SystemVerilog. It appears in the
// schematic hierarchy and in the simulation selector.
ObjectId createSystemVerilogCell(Project &project, std::string name, std::string pdk = genericPdkId,
                                 std::string source = {});

// Replaces the source of a text SPICE or SystemVerilog cell and reconciles the
// generated hierarchical symbols placed in parent schematics. Existing pins
// are matched by name and keep their ids and positions; removed pins that are
// still connected make the edit fail instead of silently changing connectivity.
struct SchematicSourceEditResult {
    bool converted = false;
    std::size_t addedPins = 0;
    std::size_t removedPins = 0;
    std::vector<std::string> notes;
};
SchematicSourceEditResult setSchematicSourceText(Project &project, ObjectId cellId, bool systemVerilog,
                                                 std::string text, std::string sourcePath = {},
                                                 bool convertEmptyDrawn = false);
ObjectId addSymbol(Project &project, ObjectId cellId, std::string name, std::string artwork = {},
                   ObjectId sourceCell = 0);
// Replaces the icon of a user-created schematic block symbol. Every symbol in
// the given schematic that was generated from the same source cell receives
// the new artwork, so all instances of that block change together. Other
// schematics are never modified. Throws when the symbol is a primitive device
// or does not reference a drawn schematic cell.
void setSchematicSymbolArtwork(Project &project, ObjectId cellId, ObjectId symbolId, std::string artwork);
ObjectId addSchematicInstance(Project &project, ObjectId cellId, ObjectId symbolId, std::string name, Coordinate x,
                              Coordinate y);
// Sets or creates an instance parameter. Names are matched case-insensitively.
void setSchematicParameter(Project &project, ObjectId cellId, ObjectId instanceId, std::string name, std::string value);
// Sets or creates a parameter on a drawn schematic cell. Cell parameters are
// emitted as subcircuit `params:` defaults and as top-level `.param` cards, so
// component values can reference them with SPICE `{name}` expressions.
void setSchematicCellParameter(Project &project, ObjectId cellId, std::string name, std::string value);
// Removes a cell parameter by name. Returns true when one was removed.
bool removeSchematicCellParameter(Project &project, ObjectId cellId, const std::string &name);
// Creates a symbol whose pins come from the SystemVerilog module ports and
// places one instance in the parent schematic. A fresh symbol is created per
// instance so net names remain unambiguous.
ObjectId addSystemVerilogInstance(Project &project, ObjectId parentCellId, ObjectId sourceCellId,
                                  std::string instanceName, Coordinate x, Coordinate y, ObjectId *symbolId = nullptr,
                                  bool flattenBuses = false);
// Creates a symbol from the `.subckt` ports of a text SPICE cell and places one
// instance in the parent drawn schematic. Subcircuit parameter defaults are
// copied onto the cell and instance so they can be overridden per placement.
ObjectId addTextSpiceInstance(Project &project, ObjectId parentCellId, ObjectId sourceCellId, std::string instanceName,
                              Coordinate x, Coordinate y, ObjectId *symbolId = nullptr);
// SPICE node name used by the schematic netlister for a net in a drawn cell.
std::string schematicNetNodeName(const SchematicCell &cell, const Net &net);
ObjectId addNet(Project &project, ObjectId cellId, std::string name, std::vector<ObjectId> pins, Coordinate x = 0,
                Coordinate y = 0);
ObjectId addSchematicComment(Project &project, ObjectId cellId, std::string text, Coordinate x, Coordinate y);
void removeSchematicComment(Project &project, ObjectId cellId, ObjectId commentId);
// The SPICE element letter a schematic symbol maps to, or 0 when unsupported.
// Voltage and current sources both return their source letter so callers can
// derive reference designators consistently with netlist generation.
char spiceElementForSymbol(const std::string &symbolName);
// Copies a symbol's default parameters onto an instance, assigning fresh stable
// ids. Parameters whose names already exist on the instance are left untouched.
// Returns how many defaults were added.
std::size_t seedDefaultParameters(Project &project, SchematicInstance &instance,
                                  const std::vector<Parameter> &defaults);
ObjectId createLayoutCell(Project &project, std::string name, std::string pdk = genericPdkId, std::string stack = {});
// Creates a procedural layout and its generated static layout cell. The
// generated cell is an ordinary layout cell marked with generatedBy. Names are
// made unique across static and procedural layouts. Returns the procedural
// layout id, not the generated cell id.
ObjectId createProceduralLayout(Project &project, std::string name, std::string pdk = genericPdkId,
                                std::string script = {}, std::string stack = {},
                                std::vector<Parameter> parameters = {});
// Creates a layout cell using the schematic's technology and projects every
// schematic instance into a movable planned-device box. Nets are projected as
// straight connections between those boxes. The layout name defaults to the
// schematic name and is made unique when that name is already in use. Returns
// the new layout cell id.
ObjectId createLayoutFromSchematic(Project &project, ObjectId schematicId, std::string name = {},
                                   std::string stack = {});
// Links an existing layout cell to an existing schematic cell. LVS prefers
// this explicit link over planned-device inference and name matching. The link
// is persisted in the layout cell and is independent of device-level
// schematic_instance bindings.
void linkLayoutToSchematic(Project &project, ObjectId layoutCellId, ObjectId schematicCellId);
void unlinkLayoutFromSchematic(Project &project, ObjectId layoutCellId);
ObjectId addLayer(Project &project, std::string name, int number, ObjectId layout = 0, std::string pdk = {},
                  int datatype = 0, std::string stack = {}, Coordinate minimumWidth = 0);
std::string nextShapeName(const LayoutCell &cell, const std::string &type);
ObjectId addRectangle(Project &project, ObjectId cellId, ObjectId layerId, Rect rectangle,
                      std::optional<ObjectId> net = std::nullopt);
ObjectId addText(Project &project, ObjectId cellId, ObjectId layerId, Coordinate x, Coordinate y, std::string text,
                 Coordinate height = 1000, std::optional<ObjectId> net = std::nullopt);
ObjectId addPolygon(Project &project, ObjectId cellId, ObjectId layerId, const std::vector<LayoutPoint> &points,
                    std::optional<ObjectId> net = std::nullopt);
ObjectId addPort(Project &project, ObjectId cellId, std::string name, ObjectId layerId, Rect rectangle,
                 PortDirection direction = PortDirection::inout, PortSide side = PortSide::left, ObjectId net = 0,
                 std::string label = {});
bool updatePort(Project &project, ObjectId cellId, const Port &port);
void movePort(Project &project, ObjectId cellId, ObjectId portId, Coordinate dx, Coordinate dy);
void resizePort(Project &project, ObjectId cellId, ObjectId portId, Rect rectangle);
void removePort(Project &project, ObjectId cellId, ObjectId portId);
ObjectId createLayoutNet(Project &project, ObjectId cellId, std::string name);
void assignPortNet(Project &project, ObjectId cellId, ObjectId portId, ObjectId net);
ObjectId addLayoutInstance(Project &project, ObjectId parentCellId, ObjectId childCellId, std::string name,
                           Coordinate x, Coordinate y, Orientation orientation = Orientation::r0);
void moveRectangle(Project &project, ObjectId cellId, ObjectId shapeId, Coordinate dx, Coordinate dy);
void resizeRectangle(Project &project, ObjectId cellId, ObjectId shapeId, Rect rectangle);
void movePolygonPoint(Project &project, ObjectId cellId, ObjectId shapeId, std::size_t index, LayoutPoint point);
// Replaces two or more selected rectangles/polygons with the result of a
// boolean operation. All operands must share one layer. For difference, the
// lowest shape id is the subject and the remaining shapes are subtracted.
// Complex results with holes are split into independently fillable polygons.
// Returns the ids of the new shapes and throws when the result is empty.
std::vector<ObjectId> booleanLayoutShapes(Project &project, ObjectId cellId, const std::vector<ObjectId> &shapeIds,
                                          BooleanOperation operation);
// Converts a path into filled polygon shapes. Segments are grouped by their
// drawing layer and point vias become square polygons on each via layer, so the
// flattened geometry keeps every layer used by a multi-layer path. Overlapping
// segment and via outlines are unioned and split into independently fillable
// polygons. The original path is removed and an automatically routed path is
// detached from its planned connection. Returns the new shape ids in layer
// order and throws when the path has no flattenable geometry.
std::vector<ObjectId> flattenLayoutPath(Project &project, ObjectId cellId, ObjectId shapeId);

// Translation, duplication, arrays, and alignment for layout shapes. Shape ids
// must refer to entries in `cellId`. Duplicates and array copies preserve
// polygon outlines, path centerlines, per-segment layers/widths, and point
// vias; copied routed paths become standalone paths. Duplicate offsets default
// to one selection width plus one grid step to the right. Array pitches default
// to the corresponding selection extent plus one grid step. Array copies are
// limited to 100000 shapes per call.
std::vector<ObjectId> duplicateLayoutShapes(Project &project, ObjectId cellId, const std::vector<ObjectId> &shapeIds,
                                            std::optional<Coordinate> dx = std::nullopt,
                                            std::optional<Coordinate> dy = std::nullopt);
std::vector<ObjectId> arrayLayoutShapes(Project &project, ObjectId cellId, const std::vector<ObjectId> &shapeIds,
                                        int rows, int columns, Coordinate columnPitch = 0, Coordinate rowPitch = 0);

enum class LayoutArrange {
    left,
    right,
    top,
    bottom,
    centerHorizontal,
    centerVertical,
    distributeHorizontal,
    distributeVertical,
};
// Aligns or distributes two or more layout shapes. Distribute operations need
// at least three shapes. Routed paths and text-polygon members are ignored.
void arrangeLayoutShapes(Project &project, ObjectId cellId, const std::vector<ObjectId> &shapeIds,
                         LayoutArrange operation);

// Authored schematic-to-layout device links. The persisted source of truth is
// the schematic_instance field on layout instances and planned devices. These
// helpers expose it as one queryable relation, support manual binding from the
// editor and scripts, and report dangling or duplicate links without changing
// the layout file format.
enum class LayoutDeviceKind { plannedDevice, instance };

struct LayoutDeviceBinding {
    ObjectId layoutCell = 0;
    ObjectId layoutObject = 0;
    LayoutDeviceKind kind = LayoutDeviceKind::plannedDevice;
    std::string layoutName;
    // Zero when the linked schematic instance no longer exists.
    ObjectId schematicCell = 0;
    ObjectId schematicInstance = 0;
    std::string schematicName;
    bool stale = false;
    // True when more than one layout object in the same cell points at this
    // schematic instance. Duplicate links are allowed but reported.
    bool duplicate = false;
};

// Returns active, stale, and duplicate bindings in deterministic order. When
// schematicCellId is non-zero, only links into that schematic are returned.
std::vector<LayoutDeviceBinding> layoutDeviceBindings(const Project &project, ObjectId layoutCellId,
                                                      ObjectId schematicCellId = 0);

// Sets or clears the schematic link of a planned device or layout instance.
// Binding is a normal project mutation and is therefore undoable through
// ProjectSession. Duplicate links are allowed; validate them with
// layoutDeviceBindings() or Project::validate().
void bindLayoutDevice(Project &project, ObjectId layoutCellId, ObjectId layoutObject, ObjectId schematicCellId,
                      ObjectId schematicInstance);
void unbindLayoutDevice(Project &project, ObjectId layoutCellId, ObjectId layoutObject);

// Removes a project item after checking references that would become invalid.
// These throw ProjectError with a descriptive code when removal is unsafe.
void removeSchematicCell(Project &project, ObjectId cellId);
void removeLayoutItem(Project &project, ObjectId id);
void removeSimulationItem(Project &project, ObjectId simulationId);

// Simulation graph metadata is presentation-only but agent-facing: it selects
// the plotted X/Y signals and optional display limits from captured traces.
// Reject a simulation name that is already used in the project. Simulation
// names are the primary lookup key for CLI/MCP/UI actions, so duplicates make
// those actions ambiguous.
void requireUniqueSimulationName(const Project &project, const std::string &name, ObjectId except = 0);

ObjectId addSimulationGraph(Project &project, ObjectId simulationId, const SimulationGraph &graph);
bool updateSimulationGraph(Project &project, ObjectId simulationId, ObjectId graphId, const SimulationGraph &graph);
bool removeSimulationGraph(Project &project, ObjectId simulationId, ObjectId graphId);

// Converts a schematic cell into a SPICE netlist so a simulation can run the
// drawn circuit instead of hand-typed directives. Each placed symbol becomes
// one SPICE element; nets become SPICE nodes. Returns the netlist lines
// (without a trailing `.end`, which the simulator adapter appends).
//
// Symbol names are mapped to SPICE elements case-insensitively:
//   resistor -> R, capacitor -> C, inductor -> L, diode -> D,
//   voltage source -> V, current source -> I,
//   ground -> reference node (0). Anything else is emitted as a comment.
//
// Symbols backed by another schematic (Symbol::sourceCell) are hierarchical:
// each referenced drawn schematic is emitted once as a .subckt whose ports
// come from its Input and Output instances, and the parent instance becomes
// X<name> <connected nodes...> <subckt name>. Instances are wired by port
// name, so a symbol may declare its pins in any order. Subcircuits are emitted before
// the parent body and recursively for nested hierarchy.
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
enum class SchematicNetlistMode {
    // Pure SPICE: SystemVerilog blocks are an error.
    analog,
    // Mixed-signal base netlist: SystemVerilog blocks are omitted so the
    // co-simulation adapter can add their interface sources. Nested blocks in
    // drawn subcircuits require the flattened mixed-signal entry point.
    mixed,
};
std::vector<std::string> buildSchematicNetlist(const Project &project, ObjectId cellId,
                                               std::vector<std::string> *notes = nullptr,
                                               SchematicNetlistMode mode = SchematicNetlistMode::analog,
                                               double geometryScale = 1.0);

// One source block discovered while flattening a drawn hierarchy for
// mixed-signal co-simulation. `path` joins the instance names from the top
// cell; `pinNodes` maps the block symbol's pin names to flattened SPICE nodes.
struct SchematicNetlistBlock {
    ObjectId instanceId = 0;
    ObjectId symbolId = 0;
    ObjectId sourceCell = 0;
    std::string path;
    std::vector<std::string> pinNames;
    std::map<std::string, std::string> pinNodes;
};

struct SchematicNetlistBuild {
    std::vector<std::string> lines;
    std::vector<SchematicNetlistBlock> blocks;
    std::vector<std::string> notes;
    // Flattened node -> voltage-source instance name. Mixed-signal planning
    // uses this to reject a digital output shorted to an analog source.
    std::map<std::string, std::string> voltageSourceDrivers;
};

// Builds the mixed-signal analog base netlist with drawn hierarchy flattened
// into the top cell. This gives every nested node a unique name so HDL blocks
// inside drawn subcircuits can be connected to the shared co-simulation
// interface. Text SPICE blocks remain subcircuits and are emitted once.
SchematicNetlistBuild buildMixedSignalSchematicNetlist(const Project &project, ObjectId cellId,
                                                       double geometryScale = 1.0);

// Builds a simulation netlist from either a drawn schematic or a text SPICE
// schematic. Analysis-control directives that the simulation properties own
// (.tran, .dc, .ac, .op) and a trailing .end are removed from text sources.
// Device geometry is scaled to the units implied by the PDK preamble
// (`.option scale=1u`), so MOS sizes match the model bins.
std::vector<std::string> buildSimulationNetlist(const Project &project, ObjectId cellId,
                                                std::vector<std::string> *notes = nullptr);

// Symbol names may carry a numeric disambiguator ("Input 2", "Resistor 3") when
// several symbols of one kind live in the same cell. Type tests use the base
// name so those copies still netlist and verify as ports or primitives.
std::string symbolTypeName(std::string name);

// Port instances (Input/Output symbols) of a drawn cell in subcircuit port
// order: inputs top-to-bottom, then outputs. This is the order used for the
// `.subckt` port list, so hierarchical instance node lists must follow it too.
std::vector<const SchematicInstance *> schematicPortInstances(const SchematicCell &cell);

// Classifies the symbol used for a hierarchical schematic port. Input and
// Output are directional ports; Pin is a bidirectional inout port. Returns
// `none` for ordinary devices. Trailing instance disambiguators such as
// "Input 2" and "Pin 3" are ignored.
enum class SchematicPortKind { none, input, output, inout };
SchematicPortKind schematicPortKindForSymbol(const std::string &symbolName);

// Creates a symbol in `parentCellId` for the drawn or text-source sub-cell
// `sourceCellId`. Drawn cells expose their Input/Output instances; text SPICE
// cells expose the selected `.subckt` ports; SystemVerilog cells expose module
// ports. Returns the new symbol id.
ObjectId addHierarchicalSymbol(Project &project, ObjectId parentCellId, ObjectId sourceCellId, std::string name = {},
                               std::string artwork = {});

// Wraps selected instances of a drawn schematic into a new child cell and
// replaces them with one hierarchical block instance in the parent. Nets that
// stay inside the selection move to the child; nets that also connect to
// unselected instances become child port instances with matching pins on the
// parent block. The parent block is positioned at the center of the original
// selection and selected by the caller. Duplicate selected ids are ignored.
struct SchematicWrapResult {
    ObjectId cell = 0;
    ObjectId symbol = 0;
    ObjectId instance = 0;
    std::vector<ObjectId> ports;
};
SchematicWrapResult wrapSchematicInstances(Project &project, ObjectId parentCellId,
                                           const std::vector<ObjectId> &instanceIds, std::string childName = {});

// Generated block symbol for a drawn, text SPICE, or SystemVerilog subcell.
// The artwork is a titled rectangle with labeled pins; pin ids are zero and
// callers assign project-stable ids.
struct HierarchicalSymbolTemplate {
    std::string artwork;
    std::vector<Pin> pins;
    Coordinate labelY = 0;
};
HierarchicalSymbolTemplate hierarchicalSymbolTemplate(const SchematicCell &source);

// Pin layout used by addHierarchicalSymbol(). Pin ids are zero; callers assign
// them from the project. Useful for tools that build their own symbol.
std::vector<Pin> hierarchicalSymbolPins(const SchematicCell &source);

// Regenerates artwork and pin positions for hierarchical symbols that predate
// generated block artwork. Symbols with custom artwork are left untouched.
std::size_t repairHierarchicalSymbolArtwork(Project &project);

// Returns a name for a new symbol that is not used by any symbol in `cell`.
std::string uniqueSchematicSymbolName(const SchematicCell &cell, const std::string &base);

} // namespace openvlsi
