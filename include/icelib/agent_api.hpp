#pragma once

#include "icelib/core.hpp"
#include "icelib/symbols.hpp"

#include <string>
#include <vector>

namespace ice {

struct ComponentRequest {
    // Friendly type alias ("resistor", "capacitor", ...), an exact symbol name,
    // or a PDK device symbol name.
    std::string type;
    std::string symbol;
    std::string name;
    Coordinate x = 0;
    Coordinate y = 0;
    std::vector<Parameter> parameters;
};

struct ComponentPlacement {
    ObjectId instance = 0;
    ObjectId symbol = 0;
    std::string symbolName;
    std::string instanceName;
    std::vector<Pin> pins;
};

struct CircuitNetRequest {
    std::string name;
    std::vector<std::string> pins;
    Coordinate x = 0;
    Coordinate y = 0;
};

struct CircuitRequest {
    std::vector<ComponentRequest> components;
    std::vector<CircuitNetRequest> nets;
};

struct CircuitPlacement {
    std::vector<ComponentPlacement> components;
    std::vector<ObjectId> nets;
};

// Resolves a pin id, "<instance>.<pin>", "<symbol>.<pin>", or a unique bare pin
// name in a drawn schematic.
ObjectId resolveSchematicPin(const SchematicCell &cell, const std::string &text);

// Creates the symbol and instance for a common component and returns stable pin
// handles for immediate wiring.
ComponentPlacement addSchematicComponent(Project &project, ObjectId cellId, const ComponentRequest &request);

// Adds components and nets in one transaction-friendly operation. The caller
// should wrap this in ProjectSession::execute so a failure rolls back.
CircuitPlacement addSchematicCircuit(Project &project, ObjectId cellId, const CircuitRequest &request);

// General pre-simulation validation for drawn, text SPICE, and mixed-signal
// schematics. Codes are stable strings suitable for agent hints.
std::vector<Diagnostic> validateSchematic(const Project &project, ObjectId cellId);

struct SchematicSymbolDependency {
    ObjectId parentCell = 0;
    std::string parentName;
    ObjectId symbol = 0;
    std::string symbolName;
    std::vector<ObjectId> instances;
};

struct SchematicDependency {
    ObjectId id = 0;
    std::string name;
};

struct SchematicDeletePlan {
    ObjectId cell = 0;
    std::string name;
    std::vector<SchematicDependency> simulations;
    std::vector<SchematicSymbolDependency> symbols;

    bool blocked() const { return !simulations.empty() || !symbols.empty(); }
};

SchematicDeletePlan planSchematicDeletion(const Project &project, ObjectId cellId);

// Removes the cell together with simulations and hierarchical symbols that
// reference it. Instance pins are removed from any nets left behind.
void removeSchematicCascade(Project &project, ObjectId cellId);

struct SpiceImportResult {
    ObjectId cell = 0;
    std::vector<std::string> warnings;
};

// Creates a text SPICE cell, or a drawn schematic for a flat R/C/L/V/I/D
// netlist. Unsupported drawn constructs are rejected explicitly.
SpiceImportResult createSchematicFromSpice(Project &project, std::string name, std::string pdk,
                                           const std::string &source, bool drawn);

// True when a flat SPICE source only contains constructs supported by the
// drawn importer (R/C/L/V/I/D and simple directives).
bool spiceNetlistSupportsDrawn(const std::string &source);

} // namespace ice
