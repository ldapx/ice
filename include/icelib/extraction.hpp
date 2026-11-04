#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ice {

// One PDK-provided polygon device extraction rule. Rules are intentionally
// declarative: the geometry engine does the layer intersections/differences
// and the rule supplies the device model and terminal-layer information.
struct DeviceExtractionRule {
    std::string name;
    // "mos", "resistor", or "capacitor".
    std::string type;
    std::string model;
    // SPICE element letter: M, X, Q, R, C, D, ...
    std::string element;

    // MOS geometry.
    std::string active;
    std::string gate;
    // Layer whose overlapping region supplies the body/substrate net.
    std::string bodyLayer;
    // Layers that must / must not overlap the channel for this rule to match.
    std::vector<std::string> requireLayers;
    std::vector<std::string> excludeLayers;
    // Layers used to find the source/drain terminal nets.
    std::vector<std::string> terminalLayers;
    std::string substrate = "0";
    // "auto", "x", or "y". Auto uses the gate aspect ratio.
    std::string lengthAxis = "auto";

    // Two-terminal passives.
    // Resistor body/marker layer.
    std::string body;
    // Capacitor plate layers.
    std::vector<std::string> plates;
    // Resistor sheet resistance in ohm/square.
    double sheetResistance = 0.0;
    // Capacitor density in fF/um^2.
    double capacitancePerArea = 0.0;
    // Optional fixed value for a two-terminal device.
    std::string value;
};

struct DeviceExtractionRules {
    int schemaVersion = 1;
    std::string format;
    std::string pdk;
    std::vector<DeviceExtractionRule> rules;
};

// Loads a package extraction rule file. Returns nullopt when the file does not
// exist or when `error` is used for a malformed rule. A non-empty `error`
// indicates a malformed rule file.
std::optional<DeviceExtractionRules> loadDeviceExtractionRules(const std::filesystem::path &path,
                                                               std::string *error = nullptr);

// Searches installed PDK packages for `layout/extraction.json`.
std::optional<DeviceExtractionRules> loadDeviceExtractionRulesForPdk(const std::string &pdkId,
                                                                     std::string *error = nullptr);

} // namespace ice
