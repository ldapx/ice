#pragma once

#include "icelib/core.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace ice {

// First-order parasitic extraction options. The layout database stores
// coordinates in database units; the extractor converts them to micrometers
// before applying these coefficients.
//
// These are intentionally simple lumped estimates: every net gets one total
// resistance and one total capacitance to ground. They are useful for early
// post-layout exploration, not for sign-off.
struct PexOptions {
    // Sheet resistance for metal layers, in ohm/square.
    double metalSheetResistance = 0.05;
    // Sheet resistance for polysilicon/gate layers, in ohm/square.
    double polySheetResistance = 5.0;
    // Sheet resistance for diffusion/active layers, in ohm/square.
    double diffusionSheetResistance = 10.0;
    // Resistance contributed by each via/contact shape, in ohm.
    double viaResistance = 0.5;
    // Area capacitance, in fF/um^2.
    double areaCapacitance = 0.02;
    // Perimeter (fringe) capacitance, in fF/um.
    double perimeterCapacitance = 0.01;
    // Extract devices from placed layout instances whose child cells carry
    // device metadata (device-library cells with model parameters and pins).
    bool extractPlacedDevices = true;
    // Extract devices drawn directly as polygons using the PDK extraction
    // rules. The rules are loaded from the installed package's
    // layout/extraction.json, or from extractionRulesPath when non-empty.
    bool extractPolygonDevices = true;
    std::filesystem::path extractionRulesPath;
    // Fallback when no placed devices are extracted: include the inferred
    // source schematic's device netlist before the parasitics. This makes the
    // generated text cell a ready-to-simulate post-layout deck. Set false to
    // emit only the layout RC network.
    bool includeLinkedSchematic = true;
    // Optional explicit name for the generated text schematic. When empty the
    // layout name plus " PEX" is used.
    std::string schematicName;
};

struct PexNet {
    int schemaVersion = 1;
    ObjectId net = 0;
    std::string name;
    std::string node;
    std::vector<std::string> ports;
    double area = 0;
    double perimeter = 0;
    double resistance = 0;
    double capacitance = 0;
    int viaCount = 0;
};

struct PexDevice {
    int schemaVersion = 1;
    ObjectId instance = 0;
    std::string name;
    std::string type;
    std::string model;
    std::vector<std::string> pins;
    std::vector<std::string> nodes;
    std::string spiceLine;
};

struct PexReport {
    int schemaVersion = 1;
    ObjectId layoutCell = 0;
    std::string layout;
    std::string pdk;
    ObjectId schematicCell = 0;
    std::string schematic;
    std::string spiceText;
    std::vector<PexNet> nets;
    std::vector<PexDevice> devices;
    std::vector<std::string> notes;
    bool devicesFromLayout = false;
    bool devicesFromSchematic = false;
    double totalResistance = 0;
    double totalCapacitance = 0;
};

// Extracts placed device-library instances and first-order parasitics from a
// layout cell, then creates or updates a text SPICE schematic containing the
// extracted device netlist and net parasitics. The generated cell is an
// ordinary text schematic, so it appears in the project hierarchy and can be
// opened, edited, or simulated like any other SPICE text schematic.
//
// Placed devices are recognized from their device-library child cells: the
// model parameters and child pin geometry provide the device type and
// terminals, and each terminal is matched to the overlapping parent net. If no
// placed devices are extracted, the linked source schematic netlist can be
// copied as a fallback.
//
// Conducting layers are recognized by name (metal, poly/gate, diffusion) and
// via/contact layers by name. Shapes and pins are grouped using the same
// geometric connectivity used by the layout connectivity view. Text and
// non-drawing purpose layers are ignored.
PexReport runPex(Project &project, ObjectId layoutCellId, const PexOptions &options = {});

std::string serializePexReport(const PexReport &report);

} // namespace ice
