#pragma once

#include "icelib/core.hpp"
#include "icelib/packages.hpp"
#include "icelib/process.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace ice {

const char *implementationStageName(ImplementationStage stage);
std::optional<ImplementationStage> parseImplementationStage(const std::string &text);
const char *implementationStatusName(ImplementationStatus status);
std::optional<ImplementationStatus> parseImplementationStatus(const std::string &text);
const char *implementationStageStatusName(ImplementationStageStatus status);
std::optional<ImplementationStageStatus> parseImplementationStageStatus(const std::string &text);
std::vector<ImplementationStage> implementationStages();

nlohmann::ordered_json implementationRunJson(const ImplementationRun &run);
ImplementationRun implementationRunFromJson(const nlohmann::ordered_json &document);
std::string serializeImplementationRun(const ImplementationRun &run);
ImplementationRun deserializeImplementationRun(const std::string &text);

// Stable filename stem for impl/<stem>.iceimpl and build/impl/<stem>/.
std::string implementationRunStem(const ImplementationRun &run);

struct ImplementationTool {
    std::string name;
    std::filesystem::path path;
    std::string source;
};

struct ImplementationToolchain {
    ImplementationTool openroad;
    ImplementationTool yosys;
    ImplementationTool klayout;

    bool ready() const;
    std::vector<std::string> missing() const;
};

ImplementationToolchain discoverImplementationToolchain();
nlohmann::ordered_json implementationToolchainJson(const ImplementationToolchain &toolchain);

// Discovers the optional C++ OpenROAD helper used for physical stages and
// ODB-backed previews/heat maps. Returns an empty optional when unavailable.
std::optional<std::filesystem::path> findOpenRoadHelper();

// Resolved physical data for one package stack. Paths are absolute after
// resolution so generated scripts do not depend on the current directory.
struct ImplementationPlatform {
    std::string pdk;
    std::string stack;
    std::string engine = "openroad";
    std::string synthesis = "yosys";
    std::string site;
    std::filesystem::path techLef;
    std::filesystem::path cellLef;
    std::filesystem::path gds;
    // Standard-cell GDS decks merged into the final stream. A package may
    // provide one merged file; ORFS platform fallbacks may provide one file
    // per standard-cell master.
    std::vector<std::filesystem::path> gdsFiles;
    std::map<std::string, std::vector<std::filesystem::path>> liberty;
    std::vector<std::string> ioHorizontalLayers;
    std::vector<std::string> ioVerticalLayers;
    std::string routingMinLayer;
    std::string routingMaxLayer;
    std::filesystem::path tapcellScript;
    std::filesystem::path pdnScript;
    std::filesystem::path rcScript;
    std::filesystem::path rcxRules;
    std::vector<std::string> fillCells;
};

// Resolves the package/stack implementation contract. Diagnostics are filled
// with actionable errors instead of throwing for missing optional data.
ImplementationPlatform resolveImplementationPlatform(const PackageSpec &package, const std::string &stack,
                                                     std::vector<Diagnostic> &diagnostics);
nlohmann::ordered_json implementationPlatformJson(const ImplementationPlatform &platform);

std::vector<Diagnostic> validateImplementationConfig(const Project &project, const ImplementationConfig &config);

ObjectId createImplementationRun(Project &project, ImplementationConfig config);
void removeImplementationRun(Project &project, ObjectId runId);
ImplementationRun *findImplementationRun(Project &project, ObjectId runId);
const ImplementationRun *findImplementationRun(const Project &project, ObjectId runId);
ImplementationRun *findImplementationRun(Project &project, const std::string &name);
const ImplementationRun *findImplementationRun(const Project &project, const std::string &name);

struct ImplementationPreparation {
    bool ready = false;
    std::filesystem::path runDirectory;
    std::filesystem::path manifestPath;
    std::vector<std::filesystem::path> scripts;
    std::vector<std::string> rtlFiles;
    std::vector<Diagnostic> diagnostics;
};

// Creates build/impl/<run>/ with config snapshots, copied RTL sources, a Yosys
// synthesis script, and one OpenROAD Tcl script per physical stage. This step
// does not execute any external tool.
ImplementationPreparation prepareImplementationRun(Project &project, ImplementationRun &run,
                                                   const ImplementationPlatform &platform,
                                                   const std::filesystem::path &projectRoot);

// Hash used to detect whether a run's inputs/configuration changed.
std::string implementationConfigHash(const ImplementationConfig &config);

// Applies edited configuration values and invalidates checkpoints from
// `invalidateFrom` onward. The config hash is refreshed so a subsequent stage
// run does not reset the whole run.
void updateImplementationConfig(ImplementationRun &run, ImplementationConfig config,
                                ImplementationStage invalidateFrom);

// Relative path (from the run directory) of the generated driver script for a
// stage. Returns an empty string for the pseudo-stage `created`.
std::string implementationStageScriptRelativePath(ImplementationStage stage);
// Relative path (from the run directory) of the merged process log for a
// stage. Returns an empty string for the pseudo-stage `created`.
std::string implementationStageLogRelativePath(ImplementationStage stage);
// Technology mapping converts the generic synthesized netlist into a netlist
// built from standard cells. Export writes these run-relative files.
std::string implementationTechnologyMapScriptRelativePath();
std::string implementationTechnologyMapLogRelativePath();
std::string implementationMappedNetlistRelativePath();

struct ImplementationProgress {
    ImplementationStage stage = ImplementationStage::created;
    ImplementationStageStatus status = ImplementationStageStatus::pending;
    std::string message;
};

// Callbacks let the CLI persist intermediate states and the GUI update its
// phase tabs without making icelib depend on either frontend.
struct ImplementationExecutionOptions {
    ImplementationStage from = ImplementationStage::synthesis;
    ImplementationStage to = ImplementationStage::final;
    ProcessCancellation *cancellation = nullptr;
    std::function<void(const ImplementationProgress &)> progress;
    std::function<void(const std::string &)> log;
    // Invoked after every persisted state transition. The caller may save the
    // project so an interrupted run leaves recoverable stage checkpoints.
    std::function<void(const ImplementationRun &)> changed;
    // Rebuild the generated configuration, RTL snapshots, and stage scripts
    // before executing. Safe to leave enabled; scripts are deterministic.
    bool prepare = true;
    // Reset the requested stage and every later stage before running so stale
    // downstream checkpoints are not reported as current.
    bool resetDownstream = true;
};

struct ImplementationExecutionResult {
    bool success = false;
    bool cancelled = false;
    ImplementationStage completedStage = ImplementationStage::created;
    std::vector<Diagnostic> diagnostics;
};

// Executes the generated Yosys/OpenROAD stage scripts synchronously. Tool
// versions, stage timestamps, artifacts, and OpenROAD metrics are recorded on
// the run. The caller owns persistence through `changed`.
ImplementationExecutionResult executeImplementationRun(Project &project, ImplementationRun &run,
                                                       const ImplementationPlatform &platform,
                                                       const std::filesystem::path &projectRoot,
                                                       const ImplementationExecutionOptions &options = {});

struct ImplementationMapResult {
    bool success = false;
    std::string error;
    std::filesystem::path netlist;
};

// Writes scripts/01-techmap.ys from the run's RTL snapshot and the platform
// Liberty files. Returns an empty path when the inputs are unavailable.
std::filesystem::path writeTechnologyMapScript(const ImplementationRun &run, const ImplementationPlatform &platform,
                                               const std::filesystem::path &projectRoot);

// Maps the generic synthesized netlist to the platform standard cells with
// Yosys (dfflibmap/abc) and records synth/2_tech_map.v on the run. An optional
// destination receives a copy of the mapped netlist.
ImplementationMapResult mapImplementationRun(ImplementationRun &run, const ImplementationPlatform &platform,
                                             const std::filesystem::path &projectRoot,
                                             const std::filesystem::path &destination = {},
                                             const std::function<void(const std::string &)> &log = {});

// Returns the bounded tail of a stage log. Missing logs return an empty string.
std::string readImplementationLog(const ImplementationRun &run, const std::filesystem::path &projectRoot,
                                  ImplementationStage stage, std::size_t maximumBytes = 64 * 1024);

// Sends SIGTERM to the process recorded in build/impl/<run>/run.pid. Returns
// false and fills `error` when no process is recorded or signalling fails.
bool cancelImplementationRun(const ImplementationRun &run, const std::filesystem::path &projectRoot,
                             std::string &error);

struct ImplementationLayoutImport {
    ObjectId layout = 0;
    std::filesystem::path defPath;
    std::vector<Diagnostic> diagnostics;
};

// Imports the newest available physical checkpoint (final DEF when present)
// as an ICE layout cell with package layers, top-level pins, nets, routes, and
// standard-cell instances. `replace` reuses the layout previously produced by
// this run instead of creating another cell.
ImplementationLayoutImport importImplementationLayout(Project &project, ImplementationRun &run,
                                                      const PackageSpec &package,
                                                      const std::filesystem::path &projectRoot,
                                                      const std::string &layoutName = {}, bool replace = false);

struct ImplementationGdsExport {
    bool success = false;
    std::filesystem::path path;
    std::filesystem::path logPath;
    std::filesystem::path defPath;
    std::vector<std::string> missingCells;
    std::vector<std::string> emptyCells;
    std::vector<Diagnostic> diagnostics;
    std::string error;
};

// Streams the newest physical checkpoint (final DEF when present) to GDSII.
// The DEF routing and standard-cell instances are merged with the platform's
// standard-cell GDS decks through KLayout. The default output is
// build/impl/<run>/final/6_final.gds. Missing KLayout or standard-cell GDS
// decks produce an explicit error instead of silently writing an incomplete
// abstract-only file.
ImplementationGdsExport exportImplementationGds(ImplementationRun &run, const ImplementationPlatform &platform,
                                                const std::filesystem::path &projectRoot,
                                                const std::filesystem::path &output = {});

} // namespace ice
