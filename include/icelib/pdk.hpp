#pragma once

#include "icelib/core.hpp"
#include "icelib/packages.hpp"

#include <chrono>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace ice {

// Technology-level layer semantics declared by a PDK package in
// `technology/connectivity.json`. Names are matched case-insensitively after
// removing a trailing `.purpose` suffix, so package data can name `met1` while
// layout geometry uses `met1.drawing` or `met1.pin`.
struct PdkLayerRole {
    std::string name;
    // One of: conductor, active, gate, via, marker, implant, well, boundary,
    // label, text, ignored. Unknown roles leave the layer to ICE's generic
    // name fallback.
    std::string role;
    // Optional sheet resistance in ohm/square. Zero means the extractor keeps
    // its generic per-kind default.
    double sheetResistance = 0.0;
};

struct PdkVia {
    std::string name;
    // Electrical layer bases connected by this cut layer.
    std::vector<std::string> lower;
    std::vector<std::string> upper;
    // Minimum cut size and enclosure in nanometres. Zero means unspecified.
    double size = 0.0;
    double enclosure = 0.0;
};

struct PdkConnectivity {
    // Layers in vertical routing order, from lowest to highest.
    std::vector<std::string> routingStack;
    std::vector<PdkLayerRole> layers;
    std::vector<PdkVia> vias;
    bool declared = false;

    // Loads and validates a connectivity document. Throws ProjectError on
    // malformed data.
    static PdkConnectivity load(const std::filesystem::path &path);

    // Role of a layer name, matched by exact lower-case name first and then by
    // base name (the part before the last dot). Empty when undeclared.
    std::string roleFor(const std::string &layerName) const;
    // Via declaration matching a cut layer name.
    const PdkVia *viaFor(const std::string &layerName) const;
    // Via declaration connecting two conductor names in either order.
    const PdkVia *viaBetween(const std::string &lower, const std::string &upper) const;
};

// Reads `technology/connectivity.json` from the installed package that provides
// the PDK id. Returns nullopt for the generic PDK, an unknown PDK, or a package
// without the file. Results are cached per canonical PDK id.
std::optional<PdkConnectivity> pdkConnectivityFor(const std::string &pdkId);

// One SPICE model file shipped by a PDK. `sections` contains the names defined
// by .LIB/.lib section headers in that file.
struct PdkModelFile {
    std::filesystem::path path;
    std::vector<std::string> sections;
};

// Filesystem-level readiness information for one installed PDK version.
//
// This is intentionally a best-effort inspection rather than a full simulator
// run. Callers that need proof should use probePdk(), which loads a tiny device
// netlist with ngspice.
struct PdkInfo {
    std::string id;
    std::string packageName;
    std::string packageVersion;
    std::string source = "ice-package";
    std::filesystem::path packageRoot;
    // PDK manufacturing grid in database units (nanometres). Zero when the
    // package does not declare one.
    Coordinate manufacturingGrid = 0;
    std::filesystem::path root;
    std::filesystem::path versionRoot;
    std::string version;

    std::filesystem::path ngspiceModelDir;
    std::filesystem::path modelFile;
    std::string modelSection;
    std::vector<PdkModelFile> modelFiles;
    std::vector<std::string> sections;

    std::filesystem::path osdiDir;
    std::vector<std::filesystem::path> osdiFiles;
    std::filesystem::path verilogA;
    std::vector<PackagePdkOsdiSource> osdiSources;
    bool needsOsdi = false;

    bool hasSpiceInit = false;
    std::filesystem::path spiceInitPath;
    bool hasOpenVaf = false;

    bool simulationReady = false;
    bool implementationReady = false;
    std::string implementationEngine;
    std::string implementationStack;
    std::vector<std::string> implementationMissing;
    std::vector<std::string> implementationNotes;
    std::vector<std::string> missing;
    std::vector<std::string> notes;
    // A ready-to-use top-level .lib directive, or empty when the PDK does not
    // expose a sectioned ngspice model library.
    std::string recommendedLibLine;
    // Ordered netlist preamble lines for this PDK (includes/options/lib call).
    std::vector<std::string> recommendedSetup;
    // OSDI loading commands to issue before the circuit is loaded.
    std::vector<std::string> initCommands;
    std::map<std::string, std::string> exampleDevices;
    std::optional<PackagePdkSimulation> packageSimulation;
};

struct PdkProbeResult {
    bool attempted = false;
    bool ok = false;
    std::string summary;
    std::string log;
};

struct PdkBuildResult {
    bool ok = false;
    std::string summary;
    std::string log;
    std::string compiler;
    std::filesystem::path osdiDir;
};

// ICE package roots. This is the only supported simulation PDK source.
// Reads ICE_PACKAGE_ROOT/ICE_PACKAGE_ROOTS, then ~/.ice/packages and the
// XDG data directory.
std::vector<std::filesystem::path> defaultPdkRoots();

// Process-wide override used by the GUI when its package install directory is
// configured through QSettings instead of ICE_PACKAGE_ROOT.
void setPdkPackageRoots(std::vector<std::filesystem::path> roots);

// Finds PDKs provided by ICE packages below the supplied package roots.
std::vector<PdkInfo> discoverPdks(const std::vector<std::filesystem::path> &roots = {});

// Finds a PDK by canonical id, alias, package name, or package path.
std::vector<PdkInfo> findPdks(const std::string &idOrPath, const std::vector<std::filesystem::path> &roots = {});

// Returns the declared manufacturing grid for a PDK, or zero when it is not
// specified or the PDK is not installed.
Coordinate pdkManufacturingGrid(const std::string &idOrPath);

// Applies a PDK manufacturing grid as the default layout grid. An explicitly
// changed project grid is preserved; the old 1 nm default is updated.
void applyPdkDefaultGrid(Project &project, const std::string &pdkId);

// Builds the package's Verilog-A sources into the local OSDI cache with
// openvaf-r/openvaf. This is always an explicit user action.
PdkBuildResult buildPdkOsdi(const PdkInfo &info, std::chrono::milliseconds timeout = std::chrono::minutes(5));

// Runs a minimal NMOS probe through the system ngspice using the package setup.
PdkProbeResult probePdk(const PdkInfo &info, std::chrono::milliseconds timeout = std::chrono::seconds(20));

nlohmann::json pdkInfoJson(const PdkInfo &info, bool includeFiles = true);

struct SimulationRequest;

// Package-provided lines and simulator commands for a schematic PDK.
struct PdkSimulationSetup {
    std::vector<std::string> netlistLines;
    std::vector<std::string> initCommands;
    // Meters per netlist length unit implied by a `.option scale=<value>` line.
    // PDKs that netlist device geometry in microns (for example SKY130, where
    // the model library sets `.option scale=1u`) report 1e-6 here, so the
    // schematic netlist can emit geometry that matches the model bins.
    double geometryScale = 1.0;
};

// Resolves a package PDK and returns its simulation preamble. Throws when a
// non-generic PDK is requested but no ICE package provides it.
PdkSimulationSetup pdkSimulationSetupFor(const std::string &pdkId);

// Returns the geometry scale declared by the package simulation options
// (`.option scale=1u` -> 1e-6) or 1.0 when the package declares none or cannot
// be resolved. Never throws, so netlist construction keeps working offline.
double pdkNetlistGeometryScale(const std::string &pdkId);

// Converts a length parameter to the units the netlist expects when a PDK
// declares `.option scale=...`. Values with an SI suffix ("1u") are converted;
// a bare number is already written in the netlist's own units (SKY130 writes
// "60" meaning 60 um) and is left untouched. Returns nullopt when the text is
// not a plain length so the caller can emit it verbatim.
std::optional<double> netlistLengthNumber(const std::string &text, double geometryScale);

// Same conversion, returning the text to emit. Unparseable values are returned
// unchanged. This is what netlist writers should use so device geometry is not
// scaled twice by a PDK preamble such as `.option scale=1u`.
std::string netlistLengthText(const std::string &text, double geometryScale);

// Returns the SPICE node names of a package subcircuit (for example the
// terminal order of a bipolar model), read from the package model files. Used
// to instantiate three and four terminal devices with the right node count.
// Returns nullopt when the model cannot be found.
std::optional<std::vector<std::string>> pdkSubcircuitNodes(const std::string &pdkId, const std::string &model);

// Inserts package setup before the schematic netlist and records OSDI commands.
void applyPdkSimulationSetup(const std::string &pdkId, SimulationRequest &request);

} // namespace ice
