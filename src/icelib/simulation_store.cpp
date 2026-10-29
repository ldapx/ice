#include "icelib/simulation_store.hpp"

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

namespace ice {
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
    if (path.empty())
        throw ProjectError("simulation.store.path", "Project root is empty");
    std::error_code status;
    if (std::filesystem::is_directory(path, status) && !status)
        return path;
    if (path.extension() == ".icepro" || path.filename() == "project.icepro")
        return path.parent_path();
    return path;
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
    output["format"] = "ice-simulation-result";
    output["schema_version"] = 1;
    output["run"] = {
        {"id", runId},
        {"created_at", created},
        {"project", {{"identity", context.projectIdentity}, {"name", context.projectName}}},
        {"simulation", {{"id", context.simulationId}, {"name", context.simulationName}}},
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
    };
    output["analysis"] = result.analysis;
    output["time"] = result.time;
    output["traces"] = jsonArrayOrEmpty(result.traces);
    output["graphs"] = json::array();
    for (const auto &graph : context.graphs) {
        json entry = {{"id", graph.id}, {"name", graph.name}, {"x_axis", graph.xAxis}, {"y_signals", graph.ySignals}};
        if (graph.xMin)
            entry["x_min"] = *graph.xMin;
        if (graph.xMax)
            entry["x_max"] = *graph.xMax;
        if (graph.yMin)
            entry["y_min"] = *graph.yMin;
        if (graph.yMax)
            entry["y_max"] = *graph.yMax;
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

} // namespace ice
