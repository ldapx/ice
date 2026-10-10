#pragma once

#include "icelib/core.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ice {

enum class JobState { queued, running, succeeded, failed, cancelled, timed_out };

struct SimulationRequest {
    std::string analysis = "transient";
    double stop = 1.0;
    double step = 0.01;
    double amplitude = 1.0;
    double time_constant = 0.1;
    std::string simulator = "ngspice";
    std::string simulator_version = "1";
    std::vector<std::string> netlist;
};

struct SimulationTrace {
    std::string name;
    std::vector<double> values;
};

struct SimulationResult {
    int schema_version = 1;
    JobState state = JobState::queued;
    std::string analysis;
    std::vector<double> time;
    std::vector<SimulationTrace> traces;
    std::string simulator;
    std::string simulator_version;
    std::string input_fingerprint;
    std::string error;
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

using ProgressCallback = std::function<void(const SimulationProgress&)>;

class SimulatorAdapter {
public:
    virtual ~SimulatorAdapter() = default;
    virtual SimulationResult run(const SimulationRequest& request, const CancellationToken& token,
                                 const ProgressCallback& progress) = 0;
};

class NgspiceSimulator final : public SimulatorAdapter {
public:
    SimulationResult run(const SimulationRequest& request, const CancellationToken& token,
                         const ProgressCallback& progress) override;
};

class SimulationRunner {
public:
    explicit SimulationRunner(std::shared_ptr<SimulatorAdapter> adapter = std::make_shared<NgspiceSimulator>());
    SimulationResult run(const SimulationRequest& request, std::chrono::milliseconds timeout,
                         const ProgressCallback& progress = {});
    std::future<SimulationResult> runAsync(SimulationRequest request, std::chrono::milliseconds timeout,
                                            ProgressCallback progress = {});
    void cancel() noexcept;

private:
    std::shared_ptr<SimulatorAdapter> adapter_;
    std::shared_ptr<CancellationToken> activeToken_;
};

struct SweepParameter {
    std::string name;
    std::vector<double> values;
};

struct ExperimentPoint {
    std::map<std::string, double> parameters;
    SimulationResult result;
};

class Experiment {
public:
    void addParameter(std::string name, std::vector<double> values);
    std::vector<ExperimentPoint> run(const SimulationRequest& base, std::chrono::milliseconds timeout,
                                     const ProgressCallback& progress = {}) const;

private:
    std::vector<SweepParameter> parameters_;
};

std::string jobStateName(JobState state);
std::string serializeSimulationResult(const SimulationResult& result);

} // namespace ice
