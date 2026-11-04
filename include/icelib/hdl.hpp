#pragma once

#include "icelib/core.hpp"

#include <map>
#include <string>
#include <vector>

namespace ice {

enum class HdlPortDirection { input, output, inout };

struct HdlPort {
    std::string name;
    HdlPortDirection direction = HdlPortDirection::input;
    // Packed width in bits. `widthKnown` is false when the declaration uses a
    // parameterized range that cannot be resolved without elaboration.
    int width = 1;
    bool widthKnown = true;
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

// Builds a simple box symbol whose pins match the module ports. Pin ids are
// left at zero so the caller can assign project-stable ids.
HdlSymbolTemplate makeHdlSymbolTemplate(const HdlModule &module);

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
    std::map<std::string, std::string> portNodes;
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

// Validates the HDL instances in a schematic and builds the analog base netlist
// used by the mixed-signal co-simulation adapter. Only top-level HDL instances
// with scalar input/output ports are supported in this version.
MixedSignalPlan buildMixedSignalPlan(const Project &project, ObjectId cellId);

// User-facing hierarchy/port validation. Unlike Project::validate, this can
// return warnings about unsupported mixed-signal constructs without blocking
// project loading or saving.
std::vector<Diagnostic> validateHdlHierarchy(const Project &project, ObjectId cellId);

} // namespace ice
