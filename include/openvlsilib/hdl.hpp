#pragma once

#include "openvlsilib/core.hpp"

#include <map>
#include <string>
#include <vector>

namespace openvlsi {

enum class HdlPortDirection { input, output, inout };

struct HdlPort {
    std::string name;
    HdlPortDirection direction = HdlPortDirection::input;
    // Packed width in bits. `widthKnown` is false when the declaration uses a
    // parameterized range that cannot be resolved without elaboration.
    int width = 1;
    bool widthKnown = true;
    // True for SystemVerilog real/realtime/shortreal ports.
    bool real = false;
};

struct HdlModule {
    std::string name;
    std::vector<HdlPort> ports;
};

struct HdlParseResult {
    bool ok = false;
    HdlModule module;
    std::vector<std::string> warnings;
    std::string error;
};

// One structural gate or module instance extracted from a SystemVerilog
// module body. LVS uses this instead of treating a source module as a set of
// ports only. `portNames` are gate terminals (Y/A/B) or named module ports;
// positional module connections use their decimal index.
struct HdlNetlistInstance {
    std::string name;
    std::string type;
    std::vector<std::string> portNames;
    std::vector<std::string> nets;
};

struct HdlNetlist {
    bool ok = false;
    bool structural = false;
    std::string error;
    std::vector<std::string> warnings;
    std::vector<HdlNetlistInstance> instances;
    // Simple continuous-assignment aliases (`assign out = in;`).
    std::map<std::string, std::string> netAliases;
};

// Parses a gate-level/structural module body for LVS. Behavioral constructs
// (always, initial, generate, expressions, unsupported statements) are
// rejected explicitly so LVS cannot report a false match from ports alone.
HdlNetlist parseStructuralSystemVerilog(const std::string &source, const std::string &moduleName);

struct HdlSymbolTemplate {
    std::string artwork;
    std::vector<Pin> pins;
    Coordinate labelY = 0;
};

// Returns a valid SystemVerilog identifier derived from arbitrary text.
std::string systemVerilogIdentifier(const std::string &text);

// Parses one module declaration from a SystemVerilog source string. When the
// source has several modules, `preferredModule` selects one by name; otherwise
// the parser prefers a non-testbench module that is not instantiated elsewhere.
HdlParseResult parseSystemVerilogModule(const std::string &source, const std::string &preferredModule = {});
HdlParseResult parseSystemVerilogModule(const SchematicCell &cell);

// Builds one compilation unit for a digital simulation from the selected
// SystemVerilog source cell and every project source cell it recursively
// instantiates. This lets a testbench cell reference a separate design cell
// without concatenating source by hand.
std::string buildSystemVerilogSimulationSource(const Project &project, ObjectId cellId);

// Builds a simple box symbol whose pins match the module ports. Pin ids are
// left at zero so the caller can assign project-stable ids.
HdlSymbolTemplate makeHdlSymbolTemplate(const HdlModule &module, bool flattenBuses = false);

// One HDL block instantiated in a drawn schematic. `portNodes` maps the
// original module port name to the SPICE node connected to its symbol pin.
struct HdlInstanceBinding {
    ObjectId instanceId = 0;
    ObjectId symbolId = 0;
    std::string instanceName;
    ObjectId sourceCell = 0;
    std::string moduleName;
    std::string source;
    std::vector<HdlPort> ports;
    // Scalar ports map to one node. portBits also contains one entry per bus
    // bit when the instance was created with bus_mode="flatten".
    std::map<std::string, std::string> portNodes;
    std::map<std::string, std::vector<std::string>> portBits;
};

struct MixedSignalPlan {
    std::vector<std::string> analogNetlist;
    std::vector<HdlInstanceBinding> hdlInstances;
    // Unique analog nodes that sample digital input ports.
    std::vector<std::string> analogInputNodes;
    // Unique analog nodes driven by digital output ports.
    std::vector<std::string> digitalOutputNodes;
    std::vector<std::string> warnings;
    std::string error;

    bool ok() const { return error.empty(); }
};

struct MixedSignalInterface {
    std::vector<HdlInstanceBinding> blocks;
    std::vector<std::string> analogInputNodes;
    double logicThreshold = 0.5;
    double outputHigh = 1.0;
    double outputLow = 0.0;
    int iterations = 4;
};

// True when the schematic or one of its drawn descendants instantiates a
// SystemVerilog source cell.
bool schematicContainsHdl(const Project &project, ObjectId cellId);

// Validates HDL instances anywhere in the drawn hierarchy and builds the
// flattened analog base netlist used by the mixed-signal co-simulation adapter.
// Scalar, real, and flattened bus input/output ports are supported.
MixedSignalPlan buildMixedSignalPlan(const Project &project, ObjectId cellId);

// User-facing hierarchy/port validation. Unlike Project::validate, this can
// return warnings about unsupported mixed-signal constructs without blocking
// project loading or saving.
std::vector<Diagnostic> validateHdlHierarchy(const Project &project, ObjectId cellId);

} // namespace openvlsi
