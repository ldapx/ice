#pragma once

#include "icelib/core.hpp"
#include "icelib/hdl.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ice {

enum class JobState { queued, running, succeeded, failed, cancelled, timed_out };

struct SimulationRequest {
    std::string analysis = "transient";
    // Transient window. Other analyses use their dedicated fields below.
    double start = 0.0;
    double stop = 1.0;
    double step = 0.01;
    double amplitude = 1.0;
    double time_constant = 0.1;
    // Process/temperature/supply overrides. `modelSection` rewrites the section
    // argument of .lib lines; `sourceOverrides` become ngspice `alter` commands.
    double temperature = 27.0;
    std::string modelSection;
    std::map<std::string, double> parameters;
    std::map<std::string, double> sourceOverrides;
    // DC sweep.
    std::string dcSource;
    double dcStart = 0.0;
    double dcStop = 1.0;
    double dcStep = 0.01;
    // AC sweep.
    std::string acVariation = "dec";
    int acPoints = 10;
    double acStart = 1.0;
    double acStop = 1e6;
    // Noise analysis.
    std::string noiseOutput;
    std::string noiseInput;
    std::string noiseVariation = "dec";
    int noisePoints = 10;
    double noiseStart = 1.0;
    double noiseStop = 1e6;
    // Transfer-function analysis.
    std::string tfOutput;
    std::string tfSource;
    std::vector<SimulationMeasurementSpec> measurements;
    std::string simulator = "ngspice";
    std::string simulator_version = "1";
    bool use_initial_conditions = false;
    // SPICE element/card lines. If the first line is not already a comment,
    // the adapter prepends a title comment because ngspice treats the first
    // netlist line as the circuit title.
    std::vector<std::string> netlist;
    // Node/signal names to fetch and return as traces (e.g. "N15", "out",
    // "v(out)"). Empty means the adapter falls back to "v(out)".
    // Named `probeSignals` (not `signals`) because Qt defines `signals` as a
    // macro and this struct is read from Qt translation units.
    std::vector<std::string> probeSignals;
    // Package-provided simulator commands applied before the circuit is loaded.
    // OSDI PDKs use this for `osdi '<path>'`; no global ~/.spiceinit is needed.
    std::vector<std::string> initCommands;
    // Optional source/include working directory for HDL compilation. The UI
    // sets this to the project root so `include paths resolve predictably.
    std::filesystem::path workingDirectory;
    // Wall-clock budget for a single Verilog simulation pass, in seconds.
    // The runner fills this from the caller's timeout so long algorithmic
    // testbenches are not cut off by a fixed internal cap. Zero keeps the
    // adapter default.
    int wallTimeoutSeconds = 0;
    // Configuration for the iterative ngspice + Icarus mixed-signal adapter.
    // `netlist` is the analog base netlist; HDL blocks are added from this
    // interface description.
    MixedSignalInterface mixedSignal;
};

struct SimulationTrace {
    std::string name;
    std::vector<double> values;
};

// Populates a SimulationRequest from a drawn schematic that instantiates
// SystemVerilog blocks. The caller is responsible for applying PDK setup and
// user directives afterwards. Returns false and sets `error` on failure.
bool prepareMixedSignalRequest(SimulationRequest &request, const Project &project, ObjectId schematicId,
                               std::string *error = nullptr);

// Fills an empty capture-signal list and empty graph Y selections from result
// traces. Full trace names are used so hierarchical VCD paths remain exact.
// Returns true when the simulation definition changed.
bool populateSimulationSignalsFromTraces(SimulationItem &simulation, const std::vector<SimulationTrace> &traces);

struct SimulationMeasurementResult {
    std::string name;
    std::string metric;
    double value = 0.0;
    std::string unit;
    bool valid = false;
    std::string error;
};

struct SimulationResult {
    int schema_version = 1;
    JobState state = JobState::queued;
    std::string analysis;
    // Primary x axis. `time` always holds the samples for compatibility;
    // `xAxis`/`xUnit` describe what those samples mean (time, frequency,
    // voltage sweep, operating point, ...).
    std::string xAxis = "time";
    std::string xUnit = "s";
    std::vector<double> time;
    std::vector<SimulationTrace> traces;
    // Scalar results for operating point, transfer function, and integrated
    // noise totals.
    std::map<std::string, double> scalars;
    std::vector<SimulationMeasurementResult> measurements;
    std::string simulator;
    std::string simulator_version;
    std::string input_fingerprint;
    std::string error;
    std::vector<std::string> warnings;
};

struct SimulationProgress {
    double fraction = 0.0;
    std::string message;
};

class CancellationToken {
  public:
    void cancel() noexcept;
    bool cancelled() const noexcept;

  private:
    std::atomic_bool cancelled_ = false;
};

using ProgressCallback = std::function<void(const SimulationProgress &)>;

class SimulatorAdapter {
  public:
    virtual ~SimulatorAdapter() = default;
    virtual SimulationResult run(const SimulationRequest &request, const CancellationToken &token,
                                 const ProgressCallback &progress) = 0;
};

class NgspiceSimulator final : public SimulatorAdapter {
  public:
    SimulationResult run(const SimulationRequest &request, const CancellationToken &token,
                         const ProgressCallback &progress) override;
};

// Compiles a SystemVerilog testbench with Icarus Verilog, runs vvp, and parses
// the generated VCD into SimulationResult traces.
class IcarusVerilogSimulator final : public SimulatorAdapter {
  public:
    SimulationResult run(const SimulationRequest &request, const CancellationToken &token,
                         const ProgressCallback &progress) override;
};

// Iterative mixed-signal co-simulation. The analog base netlist is solved with
// ngspice while a generated Icarus testbench evaluates the HDL blocks. Digital
// outputs drive PWL sources in the analog deck and analog input nodes are
// sampled through a logic threshold on each pass.
class MixedSignalSimulator final : public SimulatorAdapter {
  public:
    SimulationResult run(const SimulationRequest &request, const CancellationToken &token,
                         const ProgressCallback &progress) override;
};

// Dispatches to ngspice, Icarus Verilog, or the mixed-signal adapter from
// request.simulator.
class AutoSimulator final : public SimulatorAdapter {
  public:
    SimulationResult run(const SimulationRequest &request, const CancellationToken &token,
                         const ProgressCallback &progress) override;
};

class SimulationRunner {
  public:
    explicit SimulationRunner(std::shared_ptr<SimulatorAdapter> adapter = std::make_shared<AutoSimulator>());
    SimulationResult run(const SimulationRequest &request, std::chrono::milliseconds timeout,
                         const ProgressCallback &progress = {});
    std::future<SimulationResult> runAsync(SimulationRequest request, std::chrono::milliseconds timeout,
                                           ProgressCallback progress = {});
    void cancel() noexcept;

  private:
    std::shared_ptr<SimulatorAdapter> adapter_;
    std::shared_ptr<CancellationToken> activeToken_;
    mutable std::mutex tokenMutex_;
};

struct SweepParameter {
    std::string name;
    std::vector<double> values;
};

struct ExperimentPoint {
    // Human-readable variant label: corner name and/or Monte Carlo sample.
    std::string variant;
    std::string corner;
    int sample = 0;
    std::map<std::string, double> parameters;
    SimulationResult result;
};

class Experiment {
  public:
    void addParameter(std::string name, std::vector<double> values);
    void addCorner(SimulationCorner corner);
    void setMonteCarlo(SimulationMonteCarloOptions options);
    std::vector<ExperimentPoint> run(const SimulationRequest &base, std::chrono::milliseconds timeout,
                                     const ProgressCallback &progress = {}) const;

  private:
    std::vector<SweepParameter> parameters_;
    std::vector<SimulationCorner> corners_;
    SimulationMonteCarloOptions monteCarlo_;
};

// Build the concrete simulator request for one stored SimulationItem. This is
// shared by the GUI, CLI, MCP, and experiment runner so all surfaces apply the
// same netlist, PDK setup, analysis options, measurement specs, and directives.
SimulationRequest simulationRequestForItem(const Project &project, const SimulationItem &item,
                                           const std::filesystem::path &projectRoot = {},
                                           std::vector<std::string> *notes = nullptr);

// Returns standard TT/FF/SS-style corners declared by an installed PDK package.
std::vector<SimulationCorner> pdkSimulationCorners(const std::string &pdkId);

// Configure an Experiment from the item's sweeps, corners, and Monte Carlo
// options, then run every combination.
std::vector<ExperimentPoint> runSimulationExperiment(const Project &project, const SimulationItem &item,
                                                     std::chrono::milliseconds timeout,
                                                     const ProgressCallback &progress = {},
                                                     const std::filesystem::path &projectRoot = {});

std::string jobStateName(JobState state);
std::string serializeSimulationResult(const SimulationResult &result);

} // namespace ice
