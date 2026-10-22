#include "icelib/simulation.hpp"

#include <cmath>
#include <cstdint>
#include <future>
#include <iomanip>
#include <mutex>
#include <ngspice/sharedspice.h>
#include <nlohmann/json.hpp>
#include <sstream>

namespace ice {
using json = nlohmann::ordered_json;

namespace {
std::mutex ngspiceMutex;
int discardOutput(char *, int, void *) { return 0; }
int discardStatus(char *, int, void *) { return 0; }
int discardExit(int, NG_BOOL, NG_BOOL, int, void *) { return 0; }

std::string fingerprintRequest(const SimulationRequest &request) {
    // FNV-1a 64-bit over the analysis parameters and netlist for provenance.
    constexpr std::uint64_t kOffset = 1469598103934665603ULL;
    constexpr std::uint64_t kPrime = 1099511628211ULL;
    std::uint64_t hash = kOffset;
    const auto mix = [&](const void *data, std::size_t size) {
        const auto *bytes = static_cast<const unsigned char *>(data);
        for (std::size_t i = 0; i < size; ++i) {
            hash ^= bytes[i];
            hash *= kPrime;
        }
    };
    const auto mixString = [&](const std::string &text) {
        mix(text.data(), text.size());
        const char separator = '\0';
        mix(&separator, 1);
    };
    mixString(request.analysis);
    mixString(request.simulator);
    mixString(request.simulator_version);
    mix(reinterpret_cast<const char *>(&request.stop), sizeof(request.stop));
    mix(reinterpret_cast<const char *>(&request.step), sizeof(request.step));
    for (const auto &line : request.netlist)
        mixString(line);
    std::ostringstream text;
    text << std::hex << std::setw(16) << std::setfill('0') << hash;
    return "fnv1a64:" + text.str();
}
} // namespace

void CancellationToken::cancel() noexcept { cancelled_.store(true); }
bool CancellationToken::cancelled() const noexcept { return cancelled_.load(); }

SimulationResult NgspiceSimulator::run(const SimulationRequest &request, const CancellationToken &token,
                                       const ProgressCallback &progress) {
    std::lock_guard lock(ngspiceMutex);
    SimulationResult result{1, JobState::running, request.analysis, {}, {}, "ngspice", NGSPICE_PACKAGE_VERSION, {}, {}};
    result.input_fingerprint = fingerprintRequest(request);
    if (request.netlist.empty()) {
        result.state = JobState::failed;
        result.error = "ngspice requires a netlist";
        return result;
    }
    if (request.analysis != "transient" && request.analysis != "tran") {
        result.state = JobState::failed;
        result.error = "unsupported ngspice analysis: " + request.analysis;
        return result;
    }
    if (!(request.step > 0.0) || !(request.stop > 0.0)) {
        result.state = JobState::failed;
        result.error = "transient analysis requires positive step and stop times";
        return result;
    }
    std::vector<std::string> lines = request.netlist;
    lines.push_back(".end");
    std::vector<char *> circuit;
    for (auto &line : lines)
        circuit.push_back(line.data());
    circuit.push_back(nullptr);
    if (ngSpice_Init(discardOutput, discardStatus, discardExit, nullptr, nullptr, nullptr, nullptr) != 0 ||
        ngSpice_Circ(circuit.data()) != 0) {
        result.state = JobState::failed;
        result.error = "unable to initialize ngspice";
        return result;
    }
    std::ostringstream command;
    command << "tran " << request.step << " " << request.stop;
    if (token.cancelled()) {
        result.state = JobState::cancelled;
        result.error = "simulation cancelled";
        return result;
    }
    auto commandText = command.str();
    if (ngSpice_Command(commandText.data()) != 0) {
        result.state = JobState::failed;
        result.error = "ngspice transient analysis failed";
        return result;
    }
    auto *time = ngGet_Vec_Info(const_cast<char *>("time"));
    auto *output = ngGet_Vec_Info(const_cast<char *>("v(out)"));
    if (time == nullptr || output == nullptr || time->v_realdata == nullptr || output->v_realdata == nullptr ||
        time->v_length <= 0 || output->v_length <= 0) {
        result.state = JobState::failed;
        result.error = "ngspice did not produce time and v(out) vectors";
        return result;
    }
    const int count = std::min(time->v_length, output->v_length);
    auto trace = SimulationTrace{"v(out)", {}};
    trace.values.reserve(static_cast<std::size_t>(count));
    result.time.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        if (token.cancelled()) {
            result.state = JobState::cancelled;
            result.error = "simulation cancelled";
            return result;
        }
        result.time.push_back(time->v_realdata[index]);
        trace.values.push_back(output->v_realdata[index]);
        if (progress && (index % 64 == 0 || index + 1 == count)) {
            const double fraction = static_cast<double>(index + 1) / static_cast<double>(count);
            progress({fraction, "ngspice transient analysis running"});
        }
    }
    result.traces.push_back(std::move(trace));
    result.state = JobState::succeeded;
    if (progress)
        progress({1.0, "ngspice transient analysis complete"});
    return result;
}

SimulationRunner::SimulationRunner(std::shared_ptr<SimulatorAdapter> adapter) : adapter_(std::move(adapter)) {}

SimulationResult SimulationRunner::run(const SimulationRequest &request, std::chrono::milliseconds timeout,
                                       const ProgressCallback &progress) {
    auto token = std::make_shared<CancellationToken>();
    {
        std::lock_guard lock(tokenMutex_);
        activeToken_ = token;
    }
    // Copy the adapter so the worker does not depend on `this` outliving the call.
    auto adapter = adapter_;
    auto future = std::async(std::launch::async,
                             [adapter, request, token, progress] { return adapter->run(request, *token, progress); });
    if (future.wait_for(timeout) == std::future_status::timeout) {
        token->cancel();
        // NOTE: std::future from std::async blocks on destruction, so this still
        // waits for the worker. ngspice has no async abort here; the timeout
        // marks the result while cancellation propagates cooperatively.
        auto result = future.get();
        if (result.state == JobState::succeeded || result.state == JobState::running)
            result = {1,
                      JobState::timed_out,
                      request.analysis,
                      {},
                      {},
                      request.simulator,
                      request.simulator_version,
                      result.input_fingerprint,
                      "simulation timed out"};
        {
            std::lock_guard lock(tokenMutex_);
            if (activeToken_ == token)
                activeToken_.reset();
        }
        return result;
    }
    auto result = future.get();
    {
        std::lock_guard lock(tokenMutex_);
        if (activeToken_ == token)
            activeToken_.reset();
    }
    return result;
}

std::future<SimulationResult> SimulationRunner::runAsync(SimulationRequest request, std::chrono::milliseconds timeout,
                                                         ProgressCallback progress) {
    auto adapter = adapter_;
    return std::async(std::launch::async,
                      [adapter, request = std::move(request), timeout, progress = std::move(progress)] {
                          SimulationRunner runner(adapter);
                          return runner.run(request, timeout, progress);
                      });
}

void SimulationRunner::cancel() noexcept {
    std::lock_guard lock(tokenMutex_);
    if (activeToken_)
        activeToken_->cancel();
}

void Experiment::addParameter(std::string name, std::vector<double> values) {
    if (name.empty() || values.empty())
        throw ProjectError("experiment.parameter", "Experiment parameters require a name and values");
    parameters_.push_back({std::move(name), std::move(values)});
}

std::vector<ExperimentPoint> Experiment::run(const SimulationRequest &base, std::chrono::milliseconds timeout,
                                             const ProgressCallback &progress) const {
    std::vector<ExperimentPoint> points;
    std::function<void(std::size_t, std::map<std::string, double>)> visit;
    visit = [&](std::size_t index, std::map<std::string, double> values) {
        if (index == parameters_.size()) {
            auto request = base;
            if (auto it = values.find("amplitude"); it != values.end())
                request.amplitude = it->second;
            if (auto it = values.find("time_constant"); it != values.end())
                request.time_constant = it->second;
            points.push_back({values, SimulationRunner().run(request, timeout, progress)});
            return;
        }
        for (double value : parameters_[index].values) {
            auto next = values;
            next[parameters_[index].name] = value;
            visit(index + 1, std::move(next));
        }
    };
    visit(0, {});
    return points;
}

std::string jobStateName(JobState state) {
    switch (state) {
    case JobState::queued:
        return "queued";
    case JobState::running:
        return "running";
    case JobState::succeeded:
        return "succeeded";
    case JobState::failed:
        return "failed";
    case JobState::cancelled:
        return "cancelled";
    case JobState::timed_out:
        return "timed_out";
    }
    return "unknown";
}

std::string serializeSimulationResult(const SimulationResult &result) {
    json output{{"schema_version", result.schema_version},
                {"state", jobStateName(result.state)},
                {"analysis", result.analysis},
                {"time", result.time},
                {"simulator", result.simulator},
                {"simulator_version", result.simulator_version},
                {"input_fingerprint", result.input_fingerprint},
                {"error", result.error},
                {"traces", json::array()}};
    for (const auto &trace : result.traces)
        output["traces"].push_back({{"name", trace.name}, {"values", trace.values}});
    return output.dump(2) + "\n";
}
} // namespace ice
