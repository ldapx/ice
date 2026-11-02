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
};

// Resolves a package PDK and returns its simulation preamble. Throws when a
// non-generic PDK is requested but no ICE package provides it.
PdkSimulationSetup pdkSimulationSetupFor(const std::string &pdkId);

// Inserts package setup before the schematic netlist and records OSDI commands.
void applyPdkSimulationSetup(const std::string &pdkId, SimulationRequest &request);

// Rewrites project PDK references to the package canonical IDs. Returns true
// when anything changed.
bool migrateProjectPdkIds(Project &project);

} // namespace ice
