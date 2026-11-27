#pragma once

#include "openvlsilib/core.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace openvlsi {

// One SPICE parameter assignment. Values are kept as source text so `{expr}`
// and model expressions survive import and can be resolved by ngspice.
struct SpiceParameter {
    std::string name;
    std::string value;
};

// One parsed element/device line. `kind` is the lower-case SPICE element
// letter. Element-specific positional fields are normalized by the parser so
// callers do not have to reimplement token slicing.
struct SpiceElement {
    char kind = 0;
    std::string name;
    std::vector<std::string> nodes;
    std::string model;
    std::string value;
    std::map<std::string, std::string> parameters;
    std::string raw;
    int line = 0;
};

struct SpiceModel {
    std::string name;
    char type = 0;
    std::map<std::string, std::string> parameters;
};

struct SpiceSubcircuit {
    std::string name;
    std::vector<std::string> ports;
    std::vector<SpiceParameter> parameters;
    std::vector<SpiceElement> elements;
};

// A parsed SPICE source file. Includes and libraries are retained in the AST
// for diagnostics. parseSpiceDocument() leaves directives untouched;
// parseSpiceDocumentResolved() follows them relative to a known source path.
struct SpiceDocument {
    std::string title;
    std::vector<std::string> includes;
    std::vector<std::string> libraries;
    // `.global` node names, already unquoted.
    std::vector<std::string> globals;
    std::vector<SpiceParameter> parameters;
    std::vector<SpiceModel> models;
    std::vector<SpiceSubcircuit> subcircuits;
    std::vector<SpiceElement> elements;
    std::vector<std::string> warnings;
};

SpiceDocument parseSpiceDocument(const std::string &source);

// Parses a SPICE source and recursively inlines `.include`, `.inc`, and `.lib`
// files relative to `sourcePath`. Missing files, include cycles, and missing
// library sections are reported in `warnings` and the original directive is
// preserved so diagnostics still point at the source line.
SpiceDocument parseSpiceDocumentResolved(const std::string &source, const std::filesystem::path &sourcePath,
                                         std::vector<std::string> *warnings = nullptr);

// Rewrites relative include/library paths to absolute paths so an emitted
// netlist is independent of the process working directory. It does not inline
// file contents, which keeps model files shared by several blocks from being
// duplicated. Missing files are left unchanged and reported in `warnings`.
std::string rewriteSpiceIncludePaths(const std::string &source, const std::filesystem::path &sourcePath,
                                     std::vector<std::string> *warnings = nullptr);

// Finds a subcircuit by case-insensitive name. Returns nullptr when absent.
const SpiceSubcircuit *findSpiceSubcircuit(const SpiceDocument &document, const std::string &name);

// Chooses the subcircuit a text schematic cell represents. An exact cell-name
// match wins, then a single subcircuit in the source is accepted. Multiple
// unrelated subcircuits require the cell name to match one of them.
const SpiceSubcircuit *preferredSpiceSubcircuit(const SpiceDocument &document, const std::string &preferredName,
                                                std::string *error = nullptr);

// Expands `data[7:0]` into `data[0]` ... `data[7]` (ascending, matching HDL bus
// flattening). Scalar and single-bit names are returned unchanged.
std::vector<std::string> expandSpiceBus(const std::string &name);

// Model/type name used by LVS. Passives map to their symbol type; D/M/Q/X use
// the configured model or subcircuit name.
std::string spiceElementType(const SpiceElement &element);

// Parameters relevant to LVS for one element. R/C/L expose both their quantity
// name and a generic `value`; transistors keep W/L/M/AREA-style attributes.
std::map<std::string, std::string> spiceElementParameters(const SpiceElement &element);

struct SpiceInterface {
    bool ok = false;
    std::string error;
    std::string subcircuitName;
    // Declared port names, expanded one entry per scalar/bus bit.
    std::vector<std::string> ports;
    std::vector<std::string> declaredPorts;
    std::vector<SpiceParameter> parameters;
    std::vector<std::string> warnings;
};

// Parses the interface of a text SPICE cell. `preferredName` should be the
// schematic cell name.
SpiceInterface spiceInterfaceForText(const std::string &source, const std::string &preferredName,
                                     const std::filesystem::path &sourcePath = {});
SpiceInterface spiceInterfaceForCell(const SchematicCell &cell);

// Box-symbol pins for a parsed SPICE subcircuit. Pin IDs are zero.
std::vector<Pin> spiceSubcircuitPins(const SpiceInterface &interface);

// Returns the source lines that belong in a parent netlist when this text cell
// is instantiated as a hierarchical block. Analysis-control directives and a
// trailing .end are removed; subcircuits, models, includes, parameters, and
// comments are preserved. Relative include/library paths are rewritten to
// absolute paths when sourcePath is provided.
std::vector<std::string> spiceSourceLinesForHierarchy(const std::string &source,
                                                      const std::filesystem::path &sourcePath = {},
                                                      std::vector<std::string> *warnings = nullptr);

} // namespace openvlsi
