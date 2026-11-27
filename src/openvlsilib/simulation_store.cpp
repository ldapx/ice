#include "openvlsilib/simulation_store.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <system_error>

namespace openvlsi {
namespace {

using json = nlohmann::json;

std::string sanitizedSlug(const std::string &name) {
    std::string result;
    result.reserve(name.size());
    bool lastWasSeparator = false;
    for (const unsigned char character : name) {
        if (std::isalnum(character) || character == '-' || character == '_') {
            result.push_back(static_cast<char>(character));
            lastWasSeparator = false;
        } else if (!lastWasSeparator) {
            result.push_back('-');
            lastWasSeparator = true;
        }
    }
    while (!result.empty() && result.front() == '-')
        result.erase(result.begin());
    while (!result.empty() && result.back() == '-')
        result.pop_back();
    return result.empty() ? "simulation" : result;
}

std::filesystem::path normalizedProjectRoot(const std::filesystem::path &path) {
    try {
        return projectRootDirectory(path);
    } catch (const ProjectError &) {
        throw ProjectError("simulation.store.path", "Project root is empty");
    }
}

std::string utcTimestamp(std::chrono::system_clock::time_point now) {
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    std::ostringstream value;
    value << std::put_time(&utc, "%Y%m%dT%H%M%S") << '.' << std::setw(3) << std::setfill('0') << milliseconds << 'Z';
    return value.str();
}

std::string randomSuffix() {
    thread_local std::mt19937 generator(std::random_device{}());
    std::uniform_int_distribution<std::uint32_t> distribution(0, 0xffffffffu);
    std::ostringstream value;
    value << std::hex << std::setw(8) << std::setfill('0') << distribution(generator);
    return value.str();
}

json jsonArrayOrEmpty(const std::vector<std::string> &values) {
    json result = json::array();
    for (const auto &value : values)
        result.push_back(value);
    return result;
}

json jsonArrayOrEmpty(const std::vector<SimulationTrace> &traces) {
    json result = json::array();
    for (const auto &trace : traces)
        result.push_back({{"name", trace.name}, {"values", trace.values}});
    return result;
}

JobState parseJobState(const std::string &state) {
    if (state == "queued")
        return JobState::queued;
    if (state == "running")
        return JobState::running;
    if (state == "succeeded")
        return JobState::succeeded;
    if (state == "failed")
        return JobState::failed;
    if (state == "cancelled")
        return JobState::cancelled;
    if (state == "timed_out")
        return JobState::timed_out;
    return JobState::failed;
}

} // namespace

std::filesystem::path simulationRunDirectory(const std::filesystem::path &projectRoot,
                                             const SimulationRunContext &context) {
    const auto root = normalizedProjectRoot(projectRoot);
    const auto simulation = context.simulationId == 0
                                ? std::string("manual") + "-" + sanitizedSlug(context.simulationName)
                                : std::to_string(context.simulationId) + "-" + sanitizedSlug(context.simulationName);
    return root / "build" / "simulations" / simulation / "runs";
}

static std::string serializeSimulationRunWithIdentity(const SimulationRunContext &context,
                                                      const SimulationResult &result, const std::string &runId,
                                                      const std::string &created) {
    json output = json::parse(serializeSimulationResult(result));
    output["format"] = "openvlsi-simulation-result";
    output["schema_version"] = 1;
    output["run"] = {
        {"id", runId},
        {"created_at", created},
        {"project", {{"identity", context.projectIdentity}, {"name", context.projectName}}},
        {"simulation", {{"id", context.simulationId}, {"name", context.simulationName}}},
        {"variant", context.variant},
        {"schematic", {{"id", context.schematicId}, {"name", context.schematicName}}},
        {"request",
         {{"analysis", context.request.analysis},
          {"simulator", context.request.simulator},
          {"simulator_version", context.request.simulator_version},
          {"stop", context.request.stop},
          {"step", context.request.step},
          {"amplitude", context.request.amplitude},
          {"time_constant", context.request.time_constant},
          {"use_initial_conditions", context.request.use_initial_conditions},
          {"capture_signals", jsonArrayOrEmpty(context.request.probeSignals)},
          {"directives", context.directives},
          {"netlist", jsonArrayOrEmpty(context.request.netlist)},
          {"netlist_notes", jsonArrayOrEmpty(context.netlistNotes)}}},
        {"simulator", result.simulator},
        {"simulator_version", result.simulator_version},
        {"input_fingerprint", result.input_fingerprint},
        {"state", jobStateName(result.state)},
        {"error", result.error},
        {"warnings", result.warnings},
    };
    output["analysis"] = result.analysis;
    output["time"] = result.time;
    output["traces"] = jsonArrayOrEmpty(result.traces);
    output["graphs"] = json::array();
    for (const auto &graph : context.graphs) {
        json entry = {{"id", graph.id}, {"name", graph.name}, {"x_axis", graph.xAxis}, {"y_signals", graph.ySignals}};
        if (!graph.signalLabels.empty())
            entry["signal_labels"] = graph.signalLabels;
        if (!graph.yAxisLabel.empty())
            entry["y_axis_label"] = graph.yAxisLabel;
        if (graph.xMin)
            entry["x_min"] = *graph.xMin;
        if (graph.xMax)
            entry["x_max"] = *graph.xMax;
        if (graph.yMin)
            entry["y_min"] = *graph.yMin;
        if (graph.yMax)
            entry["y_max"] = *graph.yMax;
        if (graph.view != "linear")
            entry["view"] = graph.view;
        if (graph.fft || graph.fftWindow != "hann" || graph.fftPoints != 0)
            entry["fft"] = {{"enabled", graph.fft}, {"window", graph.fftWindow}, {"points", graph.fftPoints}};
        if (!graph.showMeasurements)
            entry["show_measurements"] = false;
        if (!graph.derivedTraces.empty()) {
            entry["derived_traces"] = json::array();
            for (const auto &trace : graph.derivedTraces)
                entry["derived_traces"].push_back({{"name", trace.name}, {"expression", trace.expression}});
        }
        if (!graph.overlays.empty()) {
            entry["overlays"] = json::array();
            for (const auto &overlay : graph.overlays)
                entry["overlays"].push_back({{"run_id", overlay.runId}, {"label", overlay.label}});
        }
        if (!graph.markers.empty()) {
            entry["markers"] = json::array();
            for (const auto &marker : graph.markers) {
                json markerJson = {{"label", marker.label}, {"signal", marker.signal}, {"x", marker.x}};
                if (marker.hasY)
                    markerJson["y"] = marker.y;
                entry["markers"].push_back(std::move(markerJson));
            }
        }
        output["graphs"].push_back(std::move(entry));
    }
    return output.dump(2) + "\n";
}

std::string serializeSimulationRun(const SimulationRunContext &context, const SimulationResult &result) {
    const auto now = std::chrono::system_clock::now();
    const auto created = utcTimestamp(now);
    return serializeSimulationRunWithIdentity(context, result, created + "-" + randomSuffix(), created);
}

SimulationRunArtifact writeSimulationRun(const SimulationRunContext &context, const SimulationResult &result) {
    const auto root = normalizedProjectRoot(context.projectRoot);
    const auto directory = simulationRunDirectory(root, context);
    std::error_code status;
    std::filesystem::create_directories(directory, status);
    if (status)
        throw ProjectError("simulation.store.directory",
                           "Unable to create simulation result directory: " + directory.string());

    const auto created = utcTimestamp(std::chrono::system_clock::now());
    const auto runId = created + "-" + randomSuffix();
    auto path = directory / (runId + ".json");
    auto storedRunId = runId;
    for (int suffix = 1; std::filesystem::exists(path, status) && !status; ++suffix) {
        path = directory / (runId + "-" + std::to_string(suffix) + ".json");
        storedRunId = path.stem().string();
    }

    const auto text = serializeSimulationRunWithIdentity(context, result, storedRunId, created);
    const auto temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output)
            throw ProjectError("simulation.store.write", "Unable to write simulation result: " + path.string());
        output << text;
        if (!output.good())
            throw ProjectError("simulation.store.write", "Unable to write simulation result: " + path.string());
    }
    std::filesystem::rename(temporary, path, status);
    if (status) {
        std::filesystem::remove(temporary, status);
        throw ProjectError("simulation.store.write", "Unable to finalize simulation result: " + path.string());
    }
    return {path, storedRunId};
}

StoredSimulationRun readSimulationRun(const std::filesystem::path &path) {
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input)
            throw ProjectError("simulation.store.read", "Unable to read simulation result: " + path.string());
        json document = json::parse(input);
        if (document.value("format", std::string{}) != "openvlsi-simulation-result")
            throw ProjectError("simulation.store.format", "Not an OpenVLSI stored simulation result: " + path.string());
        const auto run = document.at("run");
        StoredSimulationRun stored;
        stored.path = path;
        stored.runId = run.value("id", std::string{});
        stored.createdAt = run.value("created_at", std::string{});
        const auto simulation = run.value("simulation", json::object());
        stored.simulationId = simulation.value("id", ObjectId{0});
        stored.simulationName = simulation.value("name", std::string{});
        stored.variant = run.value("variant", std::string{});
        const auto schematic = run.value("schematic", json::object());
        stored.schematicId = schematic.value("id", ObjectId{0});
        stored.schematicName = schematic.value("name", std::string{});

        auto &result = stored.result;
        result.schema_version = document.value("schema_version", 1);
        result.state = parseJobState(run.value("state", std::string{}));
        result.analysis = document.value("analysis", run.value("analysis", std::string{}));
        result.xAxis = document.value("x_axis", std::string{"time"});
        result.xUnit = document.value("x_unit", std::string{"s"});
        result.simulator = document.value("simulator", run.value("simulator", std::string{}));
        result.simulator_version = document.value("simulator_version", run.value("simulator_version", std::string{}));
        result.input_fingerprint = document.value("input_fingerprint", run.value("input_fingerprint", std::string{}));
        result.error = document.value("error", run.value("error", std::string{}));
        if (const auto scalars = document.find("scalars"); scalars != document.end() && scalars->is_object()) {
            for (auto entry = scalars->begin(); entry != scalars->end(); ++entry) {
                if (entry.value().is_number())
                    result.scalars[entry.key()] = entry.value().get<double>();
            }
        }
        if (const auto time = document.find("time"); time != document.end() && time->is_array())
            result.time = time->get<std::vector<double>>();
        if (const auto traces = document.find("traces"); traces != document.end() && traces->is_array()) {
            for (const auto &trace : *traces) {
                SimulationTrace entry;
                entry.name = trace.value("name", std::string{});
                if (const auto values = trace.find("values"); values != trace.end() && values->is_array())
                    entry.values = values->get<std::vector<double>>();
                result.traces.push_back(std::move(entry));
            }
        }
        if (const auto measurements = document.find("measurements");
            measurements != document.end() && measurements->is_array()) {
            for (const auto &measurement : *measurements) {
                SimulationMeasurementResult entry;
                entry.name = measurement.value("name", std::string{});
                entry.metric = measurement.value("metric", std::string{});
                entry.value = measurement.value("value", 0.0);
                entry.unit = measurement.value("unit", std::string{});
                entry.valid = measurement.value("valid", false);
                entry.error = measurement.value("error", std::string{});
                result.measurements.push_back(std::move(entry));
            }
        }
        return stored;
    } catch (const ProjectError &) {
        throw;
    } catch (const std::exception &error) {
        throw ProjectError("simulation.store.read", "Invalid simulation result " + path.string() + ": " + error.what());
    }
}

std::optional<StoredSimulationRun> latestSimulationRun(const std::filesystem::path &projectRoot,
                                                       std::optional<ObjectId> simulationId,
                                                       std::optional<ObjectId> schematicId) {
    const auto root = normalizedProjectRoot(projectRoot);
    const auto base = root / "build" / "simulations";
    std::error_code status;
    if (!std::filesystem::is_directory(base, status) || status)
        return std::nullopt;

    std::vector<std::filesystem::path> files;
    for (std::filesystem::directory_iterator simulations(base, status), end; !status && simulations != end;
         simulations.increment(status)) {
        if (!simulations->is_directory(status) || status)
            continue;
        const auto runs = simulations->path() / "runs";
        if (!std::filesystem::is_directory(runs, status) || status)
            continue;
        for (std::filesystem::directory_iterator entries(runs, status), runEnd; !status && entries != runEnd;
             entries.increment(status)) {
            if (entries->is_regular_file(status) && !status && entries->path().extension() == ".json")
                files.push_back(entries->path());
        }
    }
    std::sort(files.begin(), files.end(), [](const std::filesystem::path &left, const std::filesystem::path &right) {
        const auto leftName = left.filename().string();
        const auto rightName = right.filename().string();
        if (leftName != rightName)
            return leftName < rightName;
        return left < right;
    });
    for (auto it = files.rbegin(); it != files.rend(); ++it) {
        try {
            auto stored = readSimulationRun(*it);
            if (simulationId && stored.simulationId != *simulationId)
                continue;
            if (schematicId && stored.schematicId != *schematicId)
                continue;
            return stored;
        } catch (const ProjectError &) {
        }
    }
    return std::nullopt;
}

namespace {

std::vector<std::filesystem::path> storedRunFiles(const std::filesystem::path &projectRoot) {
    std::vector<std::filesystem::path> files;
    const auto base = projectRoot / "build" / "simulations";
    std::error_code status;
    if (!std::filesystem::is_directory(base, status) || status)
        return files;
    for (std::filesystem::directory_iterator simulations(base, status), end; !status && simulations != end;
         simulations.increment(status)) {
        if (!simulations->is_directory(status) || status)
            continue;
        const auto runs = simulations->path() / "runs";
        if (!std::filesystem::is_directory(runs, status) || status)
            continue;
        for (std::filesystem::directory_iterator entries(runs, status), runEnd; !status && entries != runEnd;
             entries.increment(status)) {
            if (entries->is_regular_file(status) && !status && entries->path().extension() == ".json")
                files.push_back(entries->path());
        }
    }
    return files;
}

} // namespace

std::vector<SimulationRunSummary> listSimulationRunSummaries(const std::filesystem::path &projectRoot,
                                                             std::optional<ObjectId> simulationId) {
    auto files = storedRunFiles(normalizedProjectRoot(projectRoot));
    std::sort(files.begin(), files.end(), [](const std::filesystem::path &left, const std::filesystem::path &right) {
        const auto leftName = left.filename().string();
        const auto rightName = right.filename().string();
        if (leftName != rightName)
            return leftName < rightName;
        return left < right;
    });
    std::vector<SimulationRunSummary> runs;
    for (auto it = files.rbegin(); it != files.rend(); ++it) {
        try {
            std::ifstream input(*it, std::ios::binary);
            if (!input)
                continue;
            const auto document = json::parse(input);
            if (document.value("format", std::string{}) != "openvlsi-simulation-result")
                continue;
            const auto run = document.find("run");
            if (run == document.end() || !run->is_object())
                continue;
            SimulationRunSummary summary;
            summary.path = *it;
            summary.runId = run->value("id", std::string{});
            summary.createdAt = run->value("created_at", std::string{});
            if (const auto simulation = run->find("simulation"); simulation != run->end() && simulation->is_object()) {
                summary.simulationId = simulation->value("id", ObjectId{0});
                summary.simulationName = simulation->value("name", std::string{});
            }
            if (simulationId && summary.simulationId != *simulationId)
                continue;
            summary.variant = run->value("variant", std::string{});
            if (const auto schematic = run->find("schematic"); schematic != run->end() && schematic->is_object()) {
                summary.schematicId = schematic->value("id", ObjectId{0});
                summary.schematicName = schematic->value("name", std::string{});
            }
            runs.push_back(std::move(summary));
        } catch (const std::exception &) {
            // Skip malformed or partially written files.
        }
    }
    return runs;
}

std::optional<StoredSimulationRun> findSimulationRun(const std::filesystem::path &projectRoot,
                                                     const std::string &runId) {
    if (runId.empty())
        return std::nullopt;
    const auto files = storedRunFiles(normalizedProjectRoot(projectRoot));
    for (const auto &path : files) {
        if (path.stem().string() != runId)
            continue;
        try {
            return readSimulationRun(path);
        } catch (const ProjectError &) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::vector<StoredSimulationRun> listSimulationRuns(const std::filesystem::path &projectRoot,
                                                    std::optional<ObjectId> simulationId) {
    auto files = storedRunFiles(normalizedProjectRoot(projectRoot));
    std::sort(files.begin(), files.end(), [](const std::filesystem::path &left, const std::filesystem::path &right) {
        const auto leftName = left.filename().string();
        const auto rightName = right.filename().string();
        if (leftName != rightName)
            return leftName < rightName;
        return left < right;
    });
    std::vector<StoredSimulationRun> runs;
    for (auto it = files.rbegin(); it != files.rend(); ++it) {
        try {
            auto stored = readSimulationRun(*it);
            if (simulationId && stored.simulationId != *simulationId)
                continue;
            runs.push_back(std::move(stored));
        } catch (const ProjectError &) {
        }
    }
    return runs;
}

} // namespace openvlsi
