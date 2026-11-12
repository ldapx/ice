#include "icelib/implementation.hpp"

#include "icelib/simulation_report.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>

#ifndef _WIN32
#include <csignal>
#include <unistd.h>
#endif

namespace ice {
namespace {

using json = nlohmann::ordered_json;

std::string nowUtc() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm calendar{};
#ifdef _WIN32
    gmtime_s(&calendar, &time);
#else
    gmtime_r(&time, &calendar);
#endif
    std::ostringstream output;
    output << std::put_time(&calendar, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

void addDiagnostic(std::vector<Diagnostic> &diagnostics, Diagnostic::Severity severity, std::string code,
                   std::string message) {
    diagnostics.push_back({severity, std::move(code), std::move(message), std::nullopt});
}

bool hasErrors(const std::vector<Diagnostic> &diagnostics) {
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [](const Diagnostic &diagnostic) { return diagnostic.severity == Diagnostic::Severity::error; });
}

std::optional<double> parseLeadingNumber(const std::string &text, std::size_t &consumed) {
    consumed = 0;
    if (text.empty())
        return std::nullopt;
    double value = 0.0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    consumed = static_cast<std::size_t>(result.ptr - text.data());
    if (result.ec != std::errc{} || consumed == 0)
        return std::nullopt;
    return value;
}

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string firstLine(const std::string &text) {
    const auto end = text.find_first_of("\r\n");
    return trim(end == std::string::npos ? text : text.substr(0, end));
}

std::string tailSuffix(const std::string &text, std::size_t maximumBytes) {
    if (text.size() <= maximumBytes)
        return text;
    return text.substr(text.size() - maximumBytes);
}

std::string relativeArtifact(const std::filesystem::path &projectRoot, const std::filesystem::path &path) {
    return path.lexically_relative(projectRoot).generic_string();
}

void addArtifact(std::vector<std::string> &artifacts, const std::string &artifact) {
    if (artifact.empty())
        return;
    if (std::find(artifacts.begin(), artifacts.end(), artifact) == artifacts.end())
        artifacts.push_back(artifact);
}

// Mirror the Graphviz lookup used when the Yosys script is generated so the
// DOT conversion honours ICE_DOT and the same PATH.
std::optional<std::filesystem::path> findGraphviz() {
    const auto check = [](const std::filesystem::path &path) -> std::optional<std::filesystem::path> {
        std::error_code status;
        if (std::filesystem::is_regular_file(path, status) && !status)
            return path;
        return std::nullopt;
    };
    if (const auto *overridePath = std::getenv("ICE_DOT"); overridePath && *overridePath)
        if (const auto found = check(overridePath))
            return found;
    if (const auto *path = std::getenv("PATH"); path && *path) {
#ifdef _WIN32
        constexpr char separator = ';';
#else
        constexpr char separator = ':';
#endif
        std::stringstream entries(path);
        std::string entry;
        while (std::getline(entries, entry, separator)) {
            if (entry.empty())
                entry = ".";
            if (const auto found = check(std::filesystem::path(entry) / "dot"))
                return found;
        }
    }
    return std::nullopt;
}

const char *stageLabel(ImplementationStage stage) {
    switch (stage) {
    case ImplementationStage::created:
        return "Created";
    case ImplementationStage::elaboration:
        return "Elaboration";
    case ImplementationStage::synthesis:
        return "Synthesis";
    case ImplementationStage::floorplan:
        return "Floorplan";
    case ImplementationStage::placement:
        return "Placement";
    case ImplementationStage::cts:
        return "CTS";
    case ImplementationStage::routing:
        return "Routing";
    case ImplementationStage::final:
        return "Final";
    }
    return "Stage";
}

std::string stageMetricPath(ImplementationStage stage) {
    switch (stage) {
    case ImplementationStage::floorplan:
        return "logs/01-floorplan.metrics.json";
    case ImplementationStage::placement:
        return "logs/02-placement.metrics.json";
    case ImplementationStage::cts:
        return "logs/03-cts.metrics.json";
    case ImplementationStage::routing:
        return "logs/04-route.metrics.json";
    case ImplementationStage::final:
        return "logs/05-final.metrics.json";
    default:
        return {};
    }
}

void finalizeYosysGraph(const ImplementationExecutionOptions &options, const std::filesystem::path &runDirectory,
                        ImplementationStage stage) {
    const auto stem = stage == ImplementationStage::elaboration ? std::string("elaboration") : std::string("synthesis");
    const auto reports = runDirectory / "reports";
    const auto dot = reports / (stem + ".dot");
    const auto overviewDot = reports / (stem + "-overview.dot");
    const auto svg = reports / (stem + ".svg");
    const auto svgNew = reports / (stem + ".svg.new");
    std::error_code status;
    std::filesystem::remove(svgNew, status);

    const auto dotSize = [](const std::filesystem::path &path) -> std::optional<std::uintmax_t> {
        std::error_code sizeStatus;
        if (!std::filesystem::is_regular_file(path, sizeStatus) || sizeStatus)
            return std::nullopt;
        const auto size = std::filesystem::file_size(path, sizeStatus);
        if (sizeStatus)
            return std::nullopt;
        return size;
    };
    // Hierarchical `dot` layout does not scale to gate-level designs with
    // thousands of cells. Prefer the compact pre-mapping graph when it is
    // available and only fall back to the faster `sfdp` engine if needed.
    constexpr std::uintmax_t largeDotBytes = 1ULL * 1024ULL * 1024ULL;
    struct Candidate {
        std::filesystem::path dot;
        bool large = false;
    };
    std::vector<Candidate> candidates;
    const auto detailedSize = dotSize(dot);
    const auto overviewSize = dotSize(overviewDot);
    if (detailedSize && *detailedSize <= largeDotBytes) {
        candidates.push_back({dot, false});
        if (overviewSize)
            candidates.push_back({overviewDot, *overviewSize > largeDotBytes});
    } else if (overviewSize) {
        candidates.push_back({overviewDot, *overviewSize > largeDotBytes});
        if (detailedSize)
            candidates.push_back({dot, true});
    } else if (detailedSize) {
        candidates.push_back({dot, true});
    }
    if (candidates.empty())
        return;

    const auto dotExecutable = findGraphviz();
    if (!dotExecutable) {
        if (options.log) {
            std::ostringstream message;
            message << "Graphviz: " << stem << " graph not rendered because 'dot' is not available. DOT kept at "
                    << candidates.front().dot.string() << "\n";
            options.log(message.str());
        }
        return;
    }

    ProcessResult result;
    int timeoutSeconds = 0;
    for (const auto &candidate : candidates) {
        timeoutSeconds = candidate.large ? 300 : 60;
        if (options.log) {
            std::ostringstream message;
            message << "Graphviz: rendering " << stem << " graph";
            if (candidate.dot == overviewDot)
                message << " overview";
            if (candidate.large)
                message << " with sfdp";
            message << "...\n";
            options.log(message.str());
        }
        ProcessOptions graph;
        graph.arguments = {dotExecutable->string()};
        if (candidate.large)
            graph.arguments.push_back("-Ksfdp");
        graph.arguments.insert(graph.arguments.end(), {"-Tsvg", candidate.dot.string(), "-o", svg.string()});
        graph.workingDirectory = runDirectory;
        graph.timeoutSeconds = timeoutSeconds;
        result = runProcess(graph, options.cancellation);
        std::error_code renderedStatus;
        const bool rendered = result.succeeded() && std::filesystem::is_regular_file(svg, renderedStatus) &&
                              !renderedStatus && std::filesystem::file_size(svg, renderedStatus) > 0 && !renderedStatus;
        if (rendered) {
            std::filesystem::remove(dot, status);
            std::filesystem::remove(overviewDot, status);
            if (options.log) {
                std::ostringstream message;
                message << "Graphviz: wrote " << svg.string() << "\n";
                options.log(message.str());
            }
            return;
        }
        std::filesystem::remove(svg, status);
    }
    if (options.log) {
        std::ostringstream message;
        message << "Graphviz: " << stem << " graph skipped";
        if (result.timedOut)
            message << " after " << timeoutSeconds << " s";
        else if (!result.error.empty())
            message << ": " << result.error;
        message << ". DOT kept at " << candidates.front().dot.string() << "\n";
        options.log(message.str());
    }
}

// Outputs that must exist after a stage succeeds. Optional outputs are only
// recorded when present.
struct StageOutputs {
    std::vector<std::string> required;
    std::vector<std::string> optional;
};

StageOutputs stageOutputs(ImplementationStage stage, bool ctsEnabled) {
    switch (stage) {
    case ImplementationStage::elaboration:
        // The graph is optional so elaboration still succeeds when Graphviz
        // is not installed or the SVG conversion fails.
        return {{},
                {"reports/elaboration.svg", "reports/elaboration.dot", "logs/00-elaboration.log",
                 "scripts/00-elaborate.ys"}};
    case ImplementationStage::synthesis:
        return {{"synth/1_synth.v"},
                {"logs/00-synthesis.log", "scripts/00-synth.ys", "reports/synthesis.svg", "reports/synthesis.dot"}};
    case ImplementationStage::floorplan:
        return {{"db/2_floorplan.odb", "def/2_floorplan.def"},
                {"final/2_floorplan.sdc", "logs/01-floorplan.log", "logs/01-floorplan.metrics.json"}};
    case ImplementationStage::placement:
        return {{"db/3_place.odb", "def/3_place.def"},
                {"final/3_place.sdc", "logs/02-placement.log", "logs/02-placement.metrics.json"}};
    case ImplementationStage::cts:
        return {{"db/4_cts.odb", "def/4_cts.def"}, {"final/4_cts.sdc", "logs/03-cts.log", "logs/03-cts.metrics.json"}};
    case ImplementationStage::routing:
        return {{"db/5_route.odb", "def/5_route.def"},
                {"final/5_route.sdc", "reports/route.guide", "reports/route_drc.rpt", "reports/antenna.rpt",
                 "logs/04-route.log", "logs/04-route.metrics.json"}};
    case ImplementationStage::final:
        return {{"db/6_final.odb", "def/6_final.def", "final/6_final.v"},
                {"final/6_final.spef", "final/6_final.sdc", "logs/05-final.log", "logs/05-final.metrics.json"}};
    default:
        (void)ctsEnabled;
        return {};
    }
}

std::string stageInput(ImplementationStage stage, bool ctsEnabled) {
    switch (stage) {
    case ImplementationStage::elaboration:
        return "scripts/00-elaborate.ys";
    case ImplementationStage::synthesis:
        return "scripts/00-synth.ys";
    case ImplementationStage::floorplan:
        return implementationMappedNetlistRelativePath();
    case ImplementationStage::placement:
        return "db/2_floorplan.odb";
    case ImplementationStage::cts:
        return "db/3_place.odb";
    case ImplementationStage::routing:
        return ctsEnabled ? "db/4_cts.odb" : "db/3_place.odb";
    case ImplementationStage::final:
        return "db/5_route.odb";
    default:
        return {};
    }
}

std::optional<std::filesystem::path> findIceOpenRoadHelper() {
    const auto check = [](const std::filesystem::path &path) -> std::optional<std::filesystem::path> {
        std::error_code status;
        if (std::filesystem::is_regular_file(path, status) && !status)
            return path;
        return std::nullopt;
    };
    if (const auto *configured = std::getenv("ICE_OPENROAD_HELPER"); configured && *configured) {
        if (const auto found = check(configured))
            return found;
    }
    const auto executable = currentExecutablePath();
    if (!executable.empty()) {
        if (const auto found = check(executable.parent_path() / "ice-openroad"))
            return found;
    }
    if (const auto *path = std::getenv("PATH"); path && *path) {
        std::stringstream entries(path);
        std::string entry;
        while (std::getline(entries, entry, ':')) {
            if (entry.empty())
                entry = ".";
            if (const auto found = check(std::filesystem::path(entry) / "ice-openroad"))
                return found;
        }
    }
    return std::nullopt;
}

bool useCppOpenRoadBackend(const std::optional<std::filesystem::path> &helper) {
    if (!helper)
        return false;
    if (const auto *mode = std::getenv("ICE_OPENROAD_BACKEND"); mode && *mode) {
        const std::string value(mode);
        if (value == "tcl")
            return false;
        if (value == "cpp")
            return true;
    }
    return true;
}

std::string joinLayers(const std::vector<std::string> &layers) {
    std::string result;
    for (const auto &layer : layers) {
        if (!result.empty())
            result += ",";
        result += layer;
    }
    return result;
}

std::vector<std::filesystem::path> implementationLiberty(const ImplementationPlatform &platform,
                                                         const ImplementationRun &run) {
    if (const auto found = platform.liberty.find(run.config.corner); found != platform.liberty.end())
        return found->second;
    if (platform.liberty.size() == 1)
        return platform.liberty.begin()->second;
    return {};
}

std::string microns(const DatabaseUnits &units, Coordinate value) {
    return std::to_string(static_cast<double>(units.toNanometers(value)) / 1000.0);
}

std::string queryToolVersion(const std::filesystem::path &executable, const std::vector<std::string> &arguments) {
    if (executable.empty())
        return {};
    ProcessOptions options;
    options.arguments.push_back(executable.generic_string());
    options.arguments.insert(options.arguments.end(), arguments.begin(), arguments.end());
    options.timeoutSeconds = 30;
    const auto result = runProcess(options);
    if (!result.started || result.exitCode != 0)
        return {};
    return firstLine(result.logTail);
}

void parseYosysMetrics(const std::string &logText, std::map<std::string, double> &metrics) {
    static const std::map<std::string, std::string> labels = {
        {"cells", "synth.cells"},
        {"wires", "synth.wires"},
        {"wire bits", "synth.wire_bits"},
        {"public wires", "synth.public_wires"},
        {"public wire bits", "synth.public_wire_bits"},
        {"ports", "synth.ports"},
        {"port bits", "synth.port_bits"},
        {"memories", "synth.memories"},
        {"memory bits", "synth.memory_bits"},
        {"processes", "synth.processes"},
    };
    std::istringstream input(logText);
    std::string line;
    while (std::getline(input, line)) {
        const auto numberAfterColon = [&](const std::string &prefix) -> std::optional<double> {
            const auto position = line.find(prefix);
            if (position == std::string::npos)
                return std::nullopt;
            const auto colon = line.find(':', position + prefix.size());
            if (colon == std::string::npos)
                return std::nullopt;
            std::size_t used = 0;
            const auto text = trim(line.substr(colon + 1));
            return parseLeadingNumber(text, used);
        };
        if (const auto value = numberAfterColon("Number of cells"))
            metrics["synth.cells"] = *value;
        if (const auto value = numberAfterColon("Number of wires"))
            metrics["synth.wires"] = *value;
        if (const auto value = numberAfterColon("Number of wire bits"))
            metrics["synth.wire_bits"] = *value;
        if (const auto value = numberAfterColon("Number of public wires"))
            metrics["synth.public_wires"] = *value;
        if (const auto value = numberAfterColon("Chip area for module"))
            metrics["synth.chip_area_um2"] = *value;
        if (const auto value = numberAfterColon("Estimated number of transistors"))
            metrics["synth.transistors"] = *value;

        // Yosys 0.6x statistics use "24 cells" instead of the older
        // "Number of cells: 24" spelling. Parse the count and unit label.
        const auto text = trim(line);
        std::size_t used = 0;
        const auto value = parseLeadingNumber(text, used);
        if (!value)
            continue;
        auto label = trim(text.substr(used));
        if (const auto found = labels.find(label); found != labels.end())
            metrics[found->second] = *value;
    }
}

void mergeOpenRoadMetrics(const std::filesystem::path &path, std::map<std::string, double> &metrics) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return;
    const auto document = json::parse(input, nullptr, false);
    if (document.is_discarded() || !document.is_object())
        return;
    for (const auto &[name, value] : document.items()) {
        if (value.is_number())
            metrics[name] = value.get<double>();
    }
}

std::string stageFailureMessage(ImplementationStage stage, const ProcessResult &result) {
    std::ostringstream output;
    output << stageLabel(stage) << " failed";
    if (!result.error.empty())
        output << ": " << result.error;
    if (result.timedOut)
        output << ": timed out";
    if (result.cancelled)
        output << ": cancelled";
    if (!result.logTail.empty()) {
        output << "\n" << tailSuffix(result.logTail, 4000);
    }
    return output.str();
}

void resetStageState(ImplementationRun &run, ImplementationStage from) {
    for (auto &[stage, state] : run.stages) {
        if (stage == ImplementationStage::created)
            continue;
        if (static_cast<int>(stage) < static_cast<int>(from))
            continue;
        state.status = ImplementationStageStatus::pending;
        state.startedAt.clear();
        state.finishedAt.clear();
        state.error.clear();
        state.artifacts.clear();
    }
    run.metrics.clear();
    run.status = ImplementationStatus::pending;
    run.lastError.clear();
    run.resultLayout = 0;
}

void resetRunState(ImplementationRun &run) {
    resetStageState(run, ImplementationStage::elaboration);
    run.currentStage = ImplementationStage::created;
    run.artifacts.clear();
    run.toolVersions.clear();
}

bool writePidFile(const std::filesystem::path &directory, ImplementationStage stage, int pid) {
    std::ofstream output(directory / "run.pid", std::ios::trunc);
    if (!output)
        return false;
    output << pid << "\n" << implementationStageName(stage) << "\n";
    return static_cast<bool>(output);
}

std::string readStageLog(const std::filesystem::path &path, std::size_t maximumBytes) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    if (size <= 0)
        return {};
    const auto wanted =
        static_cast<std::streamoff>(std::min<std::uintmax_t>(static_cast<std::uintmax_t>(size), maximumBytes));
    input.seekg(size - wanted, std::ios::beg);
    std::string text(static_cast<std::size_t>(wanted), '\0');
    input.read(text.data(), static_cast<std::streamsize>(wanted));
    text.resize(static_cast<std::size_t>(input.gcount()));
    return text;
}

} // namespace

std::optional<std::filesystem::path> findOpenRoadHelper() { return findIceOpenRoadHelper(); }

void updateImplementationConfig(ImplementationRun &run, ImplementationConfig config,
                                ImplementationStage invalidateFrom) {
    run.config = std::move(config);
    run.configHash = implementationConfigHash(run.config);
    if (invalidateFrom == ImplementationStage::created)
        invalidateFrom = ImplementationStage::elaboration;
    for (auto &[stage, state] : run.stages) {
        if (stage == ImplementationStage::created)
            continue;
        if (static_cast<int>(stage) < static_cast<int>(invalidateFrom))
            continue;
        state.status = ImplementationStageStatus::pending;
        state.startedAt.clear();
        state.finishedAt.clear();
        state.error.clear();
        state.artifacts.clear();
    }
    run.currentStage = ImplementationStage::created;
    for (const auto stage : implementationStages()) {
        if (stage == ImplementationStage::created)
            continue;
        if (static_cast<int>(stage) >= static_cast<int>(invalidateFrom))
            break;
        const auto found = run.stages.find(stage);
        if (found != run.stages.end() && found->second.status == ImplementationStageStatus::succeeded)
            run.currentStage = stage;
    }
    run.status = ImplementationStatus::pending;
    run.lastError.clear();
    run.resultLayout = 0;
    run.updatedAt = nowUtc();
}

std::string implementationStageScriptRelativePath(ImplementationStage stage) {
    switch (stage) {
    case ImplementationStage::elaboration:
        return "scripts/00-elaborate.ys";
    case ImplementationStage::synthesis:
        return "scripts/00-synth.ys";
    case ImplementationStage::floorplan:
        return "scripts/01-floorplan.tcl";
    case ImplementationStage::placement:
        return "scripts/02-place.tcl";
    case ImplementationStage::cts:
        return "scripts/03-cts.tcl";
    case ImplementationStage::routing:
        return "scripts/04-route.tcl";
    case ImplementationStage::final:
        return "scripts/05-final.tcl";
    case ImplementationStage::created:
        return {};
    }
    return {};
}

std::string implementationStageLogRelativePath(ImplementationStage stage) {
    switch (stage) {
    case ImplementationStage::elaboration:
        return "logs/00-elaboration.log";
    case ImplementationStage::synthesis:
        return "logs/00-synthesis.log";
    case ImplementationStage::floorplan:
        return "logs/01-floorplan.log";
    case ImplementationStage::placement:
        return "logs/02-placement.log";
    case ImplementationStage::cts:
        return "logs/03-cts.log";
    case ImplementationStage::routing:
        return "logs/04-route.log";
    case ImplementationStage::final:
        return "logs/05-final.log";
    case ImplementationStage::created:
        return {};
    }
    return {};
}

std::string implementationTechnologyMapScriptRelativePath() { return "scripts/01-techmap.ys"; }

std::string implementationTechnologyMapLogRelativePath() { return "logs/01-techmap.log"; }

std::string implementationMappedNetlistRelativePath() { return "synth/2_tech_map.v"; }

ImplementationMapResult mapImplementationRun(ImplementationRun &run, const ImplementationPlatform &platform,
                                             const std::filesystem::path &projectRoot,
                                             const std::filesystem::path &destination,
                                             const std::function<void(const std::string &)> &log) {
    ImplementationMapResult result;
    const auto synthesis = run.stages.find(ImplementationStage::synthesis);
    if (synthesis == run.stages.end() || synthesis->second.status != ImplementationStageStatus::succeeded) {
        result.error = "Synthesis must complete before exporting a technology-mapped netlist.";
        return result;
    }
    const auto runDirectory = projectRoot / run.directory;
    auto scriptPath = runDirectory / implementationTechnologyMapScriptRelativePath();
    std::error_code scriptStatus;
    if (!std::filesystem::is_regular_file(scriptPath, scriptStatus) || scriptStatus)
        scriptPath = writeTechnologyMapScript(run, platform, projectRoot);
    if (scriptPath.empty()) {
        result.error = "Unable to generate the technology-mapping script. The run needs RTL sources and Liberty data.";
        return result;
    }
    const auto toolchain = discoverImplementationToolchain();
    if (toolchain.yosys.path.empty()) {
        result.error = "Yosys was not found. Install Yosys or set ICE_YOSYS to its executable.";
        return result;
    }
    ProcessOptions process;
    process.workingDirectory = runDirectory;
    process.logPath = runDirectory / implementationTechnologyMapLogRelativePath();
    process.timeoutSeconds = run.config.timeoutSeconds > 0 ? run.config.timeoutSeconds : 3600;
    process.logTailBytes = 256 * 1024;
    process.environment = {
        {"ICE_RUN_DIR", runDirectory.generic_string()},
        {"ICE_PROJECT_ROOT", projectRoot.generic_string()},
        {"ICE_PDK", run.config.pdk},
        {"ICE_STACK", run.config.stack},
        {"ICE_TOP", run.config.top},
    };
    process.arguments = {toolchain.yosys.path.generic_string(), "-s", scriptPath.generic_string()};
    ProcessOutputCallback outputCallback;
    if (log)
        outputCallback = log;
    const auto processResult = runProcess(process, nullptr, outputCallback);
    if (!processResult.succeeded()) {
        if (processResult.timedOut)
            result.error = "Technology mapping timed out.";
        else if (!processResult.error.empty())
            result.error = "Technology mapping failed: " + processResult.error;
        else
            result.error = "Technology mapping failed: " + firstLine(processResult.logTail);
        return result;
    }
    const auto netlist = runDirectory / implementationMappedNetlistRelativePath();
    std::error_code status;
    if (!std::filesystem::is_regular_file(netlist, status) || status) {
        result.error = "Technology mapping completed but did not produce " + netlist.string();
        return result;
    }
    const auto artifactFor = [&](const std::filesystem::path &path) {
        const auto relative = path.lexically_relative(projectRoot);
        if (!relative.empty() && !relative.generic_string().starts_with(".."))
            return relative.generic_string();
        return path.generic_string();
    };
    const auto record = [&](const std::filesystem::path &path) {
        const auto artifact = artifactFor(path);
        addArtifact(run.artifacts, artifact);
        addArtifact(run.stages[ImplementationStage::synthesis].artifacts, artifact);
    };
    record(netlist);
    record(process.logPath);
    record(scriptPath);
    if (!destination.empty()) {
        const auto parent = destination.parent_path();
        if (!parent.empty())
            std::filesystem::create_directories(parent, status);
        if (!status)
            std::filesystem::copy_file(netlist, destination, std::filesystem::copy_options::overwrite_existing, status);
        if (status) {
            result.error =
                "Unable to write technology-mapped netlist to " + destination.string() + ": " + status.message();
            return result;
        }
        record(destination);
    }
    run.toolVersions["yosys"] = queryToolVersion(toolchain.yosys.path, {"-V"});
    run.updatedAt = nowUtc();
    result.success = true;
    result.netlist = netlist;
    return result;
}

ImplementationExecutionResult executeImplementationRun(Project &project, ImplementationRun &run,
                                                       const ImplementationPlatform &platform,
                                                       const std::filesystem::path &projectRoot,
                                                       const ImplementationExecutionOptions &options) {
    ImplementationExecutionResult result;
    const auto report = [&](ImplementationStage stage, ImplementationStageStatus status, std::string message) {
        if (options.progress)
            options.progress({stage, status, std::move(message)});
    };
    const auto changed = [&] {
        if (options.changed)
            options.changed(run);
    };
    const auto fail = [&](ImplementationStage stage, std::string message) {
        auto &state = run.stages[stage];
        state.status = ImplementationStageStatus::failed;
        state.finishedAt = nowUtc();
        state.error = message;
        run.status = ImplementationStatus::failed;
        run.currentStage = stage;
        run.lastError = message;
        run.updatedAt = nowUtc();
        addDiagnostic(result.diagnostics, Diagnostic::Severity::error, "implementation.stage", message);
        report(stage, ImplementationStageStatus::failed, message);
        changed();
        result.success = false;
        return result;
    };

    if (run.config.backend != "openroad") {
        addDiagnostic(result.diagnostics, Diagnostic::Severity::error, "implementation.backend",
                      "Unsupported implementation backend '" + run.config.backend + "'");
        return result;
    }
    if (platform.engine != "openroad") {
        addDiagnostic(result.diagnostics, Diagnostic::Severity::error, "implementation.backend",
                      "Platform engine '" + platform.engine + "' is not the OpenROAD backend");
        return result;
    }
    if (platform.synthesis != "yosys") {
        addDiagnostic(result.diagnostics, Diagnostic::Severity::error, "implementation.backend",
                      "Platform synthesis engine '" + platform.synthesis + "' is not Yosys");
        return result;
    }
    if (static_cast<int>(options.from) > static_cast<int>(options.to)) {
        addDiagnostic(result.diagnostics, Diagnostic::Severity::error, "implementation.range",
                      "Implementation stage range starts after it ends");
        return result;
    }

    const auto toolchain = discoverImplementationToolchain();
    const bool needsYosys = static_cast<int>(options.from) <= static_cast<int>(ImplementationStage::synthesis) &&
                            static_cast<int>(options.to) >= static_cast<int>(ImplementationStage::elaboration);
    const bool needsOpenRoad = static_cast<int>(options.to) >= static_cast<int>(ImplementationStage::floorplan);
    if (needsYosys && toolchain.yosys.path.empty())
        addDiagnostic(result.diagnostics, Diagnostic::Severity::error, "implementation.toolchain",
                      "Yosys was not found. Install Yosys or set ICE_YOSYS to its executable.");
    if (needsOpenRoad && toolchain.openroad.path.empty())
        addDiagnostic(result.diagnostics, Diagnostic::Severity::error, "implementation.toolchain",
                      "OpenROAD was not found. Install OpenROAD or set ICE_OPENROAD to its executable.");
    if (hasErrors(result.diagnostics)) {
        run.status = ImplementationStatus::failed;
        run.lastError = result.diagnostics.front().message;
        run.updatedAt = nowUtc();
        changed();
        return result;
    }

    const auto runDirectory = projectRoot / run.directory;
    if (run.directory.empty())
        run.directory = "build/impl/" + std::to_string(run.id);
    std::filesystem::create_directories(runDirectory);

    const auto currentHash = implementationConfigHash(run.config);
    if (!run.configHash.empty() && run.configHash != currentHash) {
        resetRunState(run);
        if (options.prepare)
            addDiagnostic(result.diagnostics, Diagnostic::Severity::warning, "implementation.config",
                          "Configuration changed; downstream stage checkpoints were invalidated");
    }
    if (options.prepare) {
        try {
            const auto preparation = prepareImplementationRun(project, run, platform, projectRoot);
            result.diagnostics.insert(result.diagnostics.end(), preparation.diagnostics.begin(),
                                      preparation.diagnostics.end());
            if (!preparation.ready)
                return fail(options.from, "Implementation preparation failed; see diagnostics and generated files");
        } catch (const ProjectError &error) {
            return fail(options.from, error.what());
        }
    }
    if (options.resetDownstream) {
        resetStageState(run, options.from);
        const auto fromIndex = static_cast<int>(options.from);
        const auto staleGraph = [&](const std::string &artifact) {
            if (fromIndex <= static_cast<int>(ImplementationStage::elaboration) &&
                (artifact.ends_with("/reports/elaboration.svg") || artifact == "reports/elaboration.svg" ||
                 artifact.ends_with("/reports/elaboration.dot") || artifact == "reports/elaboration.dot"))
                return true;
            if (fromIndex <= static_cast<int>(ImplementationStage::synthesis) &&
                (artifact.ends_with("/reports/synthesis.svg") || artifact == "reports/synthesis.svg" ||
                 artifact.ends_with("/reports/synthesis.dot") || artifact == "reports/synthesis.dot" ||
                 artifact.ends_with("/reports/synthesis-overview.dot") || artifact == "reports/synthesis-overview.dot"))
                return true;
            return false;
        };
        run.artifacts.erase(std::remove_if(run.artifacts.begin(), run.artifacts.end(), staleGraph),
                            run.artifacts.end());
        if (fromIndex <= static_cast<int>(ImplementationStage::synthesis)) {
            // A new synthesis invalidates the previously exported mapping.
            std::error_code status;
            std::filesystem::remove(runDirectory / implementationMappedNetlistRelativePath(), status);
            const auto stale = [](const std::string &artifact) {
                return artifact.ends_with(implementationMappedNetlistRelativePath()) ||
                       artifact.ends_with(implementationTechnologyMapLogRelativePath()) ||
                       artifact.ends_with(implementationTechnologyMapScriptRelativePath());
            };
            run.artifacts.erase(std::remove_if(run.artifacts.begin(), run.artifacts.end(), stale), run.artifacts.end());
        }
    }

    if (needsYosys)
        run.toolVersions["yosys"] = queryToolVersion(toolchain.yosys.path, {"-V"});
    if (needsOpenRoad)
        run.toolVersions["openroad"] = queryToolVersion(toolchain.openroad.path, {"-version"});
    run.status = ImplementationStatus::running;
    run.lastError.clear();
    run.updatedAt = nowUtc();

    const auto cppHelper = findIceOpenRoadHelper();
    if (const auto *mode = std::getenv("ICE_OPENROAD_BACKEND");
        mode && std::string(mode) == "cpp" && !cppHelper &&
        static_cast<int>(options.to) >= static_cast<int>(ImplementationStage::floorplan)) {
        return fail(options.from,
                    "ICE_OPENROAD_BACKEND=cpp requires the ice-openroad helper next to ice-cli or in PATH");
    }
    const auto ctsEnabled = run.config.ctsEnabled;
    const auto stages = implementationStages();
    for (const auto stage : stages) {
        if (stage == ImplementationStage::created)
            continue;
        if (static_cast<int>(stage) < static_cast<int>(options.from) ||
            static_cast<int>(stage) > static_cast<int>(options.to))
            continue;
        if (options.cancellation && options.cancellation->cancelled()) {
            run.status = ImplementationStatus::cancelled;
            run.lastError = "Implementation run cancelled";
            run.updatedAt = nowUtc();
            report(stage, ImplementationStageStatus::cancelled, run.lastError);
            changed();
            result.cancelled = true;
            return result;
        }
        if (stage == ImplementationStage::cts && !ctsEnabled) {
            auto &state = run.stages[stage];
            state.status = ImplementationStageStatus::skipped;
            state.startedAt = nowUtc();
            state.finishedAt = state.startedAt;
            state.error.clear();
            state.artifacts.clear();
            run.currentStage = stage;
            run.updatedAt = nowUtc();
            report(stage, ImplementationStageStatus::skipped, "CTS is disabled by configuration");
            changed();
            result.completedStage = stage;
            continue;
        }

        const bool useCppStage = useCppOpenRoadBackend(cppHelper) && stage != ImplementationStage::elaboration &&
                                 stage != ImplementationStage::synthesis;
        const auto scriptPath = runDirectory / implementationStageScriptRelativePath(stage);
        const auto input = stageInput(stage, ctsEnabled);
        std::error_code fileStatus;
        if (!useCppStage && (!std::filesystem::is_regular_file(scriptPath, fileStatus) || fileStatus)) {
            return fail(stage, "Generated stage script is missing: " + scriptPath.string());
        }
        if (!input.empty()) {
            const auto inputPath = runDirectory / input;
            if (!std::filesystem::is_regular_file(inputPath, fileStatus) || fileStatus) {
                const auto hint = stage == ImplementationStage::floorplan
                                      ? "Export a technology-mapped netlist from the Synthesis tab first."
                                      : "Run the preceding stage first.";
                return fail(stage, "Stage input is missing: " + inputPath.string() + ". " + hint);
            }
        }

        auto &state = run.stages[stage];
        state.status = ImplementationStageStatus::running;
        state.startedAt = nowUtc();
        state.finishedAt.clear();
        state.error.clear();
        state.artifacts.clear();
        run.status = ImplementationStatus::running;
        run.currentStage = stage;
        run.updatedAt = nowUtc();
        report(stage, ImplementationStageStatus::running, std::string(stageLabel(stage)) + " started");
        changed();

        ProcessOptions process;
        process.workingDirectory = runDirectory;
        process.logPath = runDirectory / implementationStageLogRelativePath(stage);
        if (useCppStage)
            process.logPath = runDirectory / "logs" / (std::string("cpp-") + implementationStageName(stage) + ".log");
        process.timeoutSeconds = run.config.timeoutSeconds > 0 ? run.config.timeoutSeconds : 3600;
        process.logTailBytes = 256 * 1024;
        process.environment = {
            {"ICE_RUN_DIR", runDirectory.generic_string()},
            {"ICE_PROJECT_ROOT", projectRoot.generic_string()},
            {"ICE_PDK", run.config.pdk},
            {"ICE_STACK", run.config.stack},
            {"ICE_TOP", run.config.top},
            {"ICE_STAGE", implementationStageName(stage)},
        };
        if (stage == ImplementationStage::elaboration || stage == ImplementationStage::synthesis) {
            process.arguments = {toolchain.yosys.path.generic_string(), "-s", scriptPath.generic_string()};
        } else if (useCppStage) {
            std::vector<std::string> args = {cppHelper->generic_string(),
                                             implementationStageName(stage),
                                             "--run-dir",
                                             runDirectory.generic_string(),
                                             "--top",
                                             run.config.top};
            const auto add = [&](const std::string &name, const std::string &value) {
                args.push_back(name);
                args.push_back(value);
            };
            if (!run.config.pdk.empty())
                add("--pdk", run.config.pdk);
            if (!run.config.stack.empty())
                add("--stack", run.config.stack);
            for (const auto &liberty : implementationLiberty(platform, run))
                add("--liberty", liberty.generic_string());
            add("--threads", std::to_string(std::max(1, run.config.threads)));

            std::filesystem::path inputDb;
            std::filesystem::path inputSdc;
            std::filesystem::path outputDb;
            std::filesystem::path outputDef;
            std::filesystem::path outputSdc;
            std::filesystem::path outputVerilog;
            std::filesystem::path outputSpef;
            std::filesystem::path outputDrc;
            std::string metricsPath;
            std::string logPath;
            if (stage == ImplementationStage::floorplan) {
                inputSdc = runDirectory / "constraints" / "top.sdc";
                outputDb = runDirectory / "db" / "2_floorplan.odb";
                outputDef = runDirectory / "def" / "2_floorplan.def";
                outputSdc = runDirectory / "final" / "2_floorplan.sdc";
                metricsPath = "logs/01-floorplan.metrics.json";
                logPath = "logs/01-floorplan.log";
                add("--tech-lef", platform.techLef.generic_string());
                add("--cell-lef", platform.cellLef.generic_string());
                add("--verilog", (runDirectory / implementationMappedNetlistRelativePath()).generic_string());
                const auto site = run.config.site.empty() ? platform.site : run.config.site;
                if (!site.empty())
                    add("--site", site);
                const auto horizontal =
                    run.config.ioHorizontalLayers.empty() ? platform.ioHorizontalLayers : run.config.ioHorizontalLayers;
                const auto vertical =
                    run.config.ioVerticalLayers.empty() ? platform.ioVerticalLayers : run.config.ioVerticalLayers;
                if (!horizontal.empty())
                    add("--io-horizontal", joinLayers(horizontal));
                if (!vertical.empty())
                    add("--io-vertical", joinLayers(vertical));
                add("--utilization", std::to_string(run.config.utilization));
                add("--aspect-ratio", std::to_string(run.config.aspectRatio));
                if (run.config.dieArea) {
                    const auto &die = *run.config.dieArea;
                    args.push_back("--die-area");
                    args.push_back(microns(project.units, die.x));
                    args.push_back(microns(project.units, die.y));
                    args.push_back(microns(project.units, die.x + die.width));
                    args.push_back(microns(project.units, die.y + die.height));
                }
                if (run.config.coreArea) {
                    const auto &core = *run.config.coreArea;
                    args.push_back("--core-area");
                    args.push_back(microns(project.units, core.x));
                    args.push_back(microns(project.units, core.y));
                    args.push_back(microns(project.units, core.x + core.width));
                    args.push_back(microns(project.units, core.y + core.height));
                }
                if (!platform.tapcellScript.empty())
                    add("--tapcell-script", platform.tapcellScript.generic_string());
                if (!platform.pdnScript.empty())
                    add("--pdn-script", platform.pdnScript.generic_string());
            } else if (stage == ImplementationStage::placement) {
                inputDb = runDirectory / "db" / "2_floorplan.odb";
                inputSdc = runDirectory / "final" / "2_floorplan.sdc";
                outputDb = runDirectory / "db" / "3_place.odb";
                outputDef = runDirectory / "def" / "3_place.def";
                outputSdc = runDirectory / "final" / "3_place.sdc";
                metricsPath = "logs/02-placement.metrics.json";
                logPath = "logs/02-placement.log";
                add("--placement-density", std::to_string(run.config.placementDensity));
                if (run.config.timingDriven)
                    args.push_back("--timing-driven");
                if (run.config.routabilityDriven)
                    args.push_back("--routability-driven");
            } else if (stage == ImplementationStage::cts) {
                inputDb = runDirectory / "db" / "3_place.odb";
                inputSdc = runDirectory / "final" / "3_place.sdc";
                outputDb = runDirectory / "db" / "4_cts.odb";
                outputDef = runDirectory / "def" / "4_cts.def";
                outputSdc = runDirectory / "final" / "4_cts.sdc";
                metricsPath = "logs/03-cts.metrics.json";
                logPath = "logs/03-cts.log";
                if (!run.config.ctsRootBuffer.empty())
                    add("--cts-root-buffer", run.config.ctsRootBuffer);
                if (!run.config.ctsBuffers.empty())
                    add("--cts-buffers", joinLayers(run.config.ctsBuffers));
            } else if (stage == ImplementationStage::routing) {
                inputDb = runDirectory / "db" / (ctsEnabled ? "4_cts.odb" : "3_place.odb");
                inputSdc = runDirectory / "final" / (ctsEnabled ? "4_cts.sdc" : "3_place.sdc");
                outputDb = runDirectory / "db" / "5_route.odb";
                outputDef = runDirectory / "def" / "5_route.def";
                outputSdc = runDirectory / "final" / "5_route.sdc";
                outputDrc = runDirectory / "reports" / "route_drc.rpt";
                metricsPath = "logs/04-route.metrics.json";
                logPath = "logs/04-route.log";
                const auto routingMin =
                    run.config.routingMinLayer.empty() ? platform.routingMinLayer : run.config.routingMinLayer;
                const auto routingMax =
                    run.config.routingMaxLayer.empty() ? platform.routingMaxLayer : run.config.routingMaxLayer;
                if (!routingMin.empty())
                    add("--routing-min-layer", routingMin);
                if (!routingMax.empty())
                    add("--routing-max-layer", routingMax);
                for (const auto &filler : platform.fillCells)
                    add("--fill-cell", filler);
                if (!run.config.antennaRepair)
                    args.push_back("--no-antenna-repair");
            } else if (stage == ImplementationStage::final) {
                inputDb = runDirectory / "db" / "5_route.odb";
                inputSdc = runDirectory / "final" / "5_route.sdc";
                outputDb = runDirectory / "db" / "6_final.odb";
                outputDef = runDirectory / "def" / "6_final.def";
                outputVerilog = runDirectory / "final" / "6_final.v";
                outputSdc = runDirectory / "final" / "6_final.sdc";
                outputSpef = runDirectory / "final" / "6_final.spef";
                metricsPath = "logs/05-final.metrics.json";
                logPath = "logs/05-final.log";
                const auto extractionModel = !platform.rcxRules.empty() ? platform.rcxRules : platform.rcScript;
                if (!extractionModel.empty())
                    add("--extraction-model", extractionModel.generic_string());
            }
            if (!inputDb.empty())
                add("--input-db", inputDb.generic_string());
            if (!inputSdc.empty() && std::filesystem::is_regular_file(inputSdc, fileStatus) && !fileStatus)
                add("--sdc", inputSdc.generic_string());
            if (!outputDb.empty())
                add("--output-db", outputDb.generic_string());
            if (!outputDef.empty())
                add("--output-def", outputDef.generic_string());
            if (!outputVerilog.empty())
                add("--output-verilog", outputVerilog.generic_string());
            if (!outputSdc.empty())
                add("--output-sdc", outputSdc.generic_string());
            if (!outputSpef.empty())
                add("--output-spef", outputSpef.generic_string());
            if (!outputDrc.empty())
                add("--output-drc", outputDrc.generic_string());
            if (!metricsPath.empty())
                add("--metrics", (runDirectory / metricsPath).generic_string());
            if (!logPath.empty())
                add("--log", (runDirectory / logPath).generic_string());
            process.arguments = std::move(args);
        } else {
            process.arguments = {
                toolchain.openroad.path.generic_string(),       "-no_init", "-no_splash", "-exit", "-threads",
                std::to_string(std::max(1, run.config.threads))};
            const auto metrics = stageMetricPath(stage);
            if (!metrics.empty())
                process.arguments.insert(process.arguments.end(),
                                         {"-metrics", (runDirectory / metrics).generic_string()});
            process.arguments.push_back(scriptPath.generic_string());
        }

        if (stage == ImplementationStage::elaboration || stage == ImplementationStage::synthesis) {
            const auto stem =
                stage == ImplementationStage::elaboration ? std::string("elaboration") : std::string("synthesis");
            std::error_code graphStatus;
            std::filesystem::remove(runDirectory / "reports" / (stem + ".dot"), graphStatus);
            std::filesystem::remove(runDirectory / "reports" / (stem + "-overview.dot"), graphStatus);
            std::filesystem::remove(runDirectory / "reports" / (stem + ".svg"), graphStatus);
            std::filesystem::remove(runDirectory / "reports" / (stem + ".svg.new"), graphStatus);
        }
        const auto pidPath = runDirectory / "run.pid";
        ProcessOutputCallback outputCallback;
        if (options.log) {
            outputCallback = [&options](const std::string &chunk) {
                if (options.log)
                    options.log(chunk);
            };
        }
        const auto processResult = runProcess(process, options.cancellation, outputCallback,
                                              [&](int pid) { writePidFile(runDirectory, stage, pid); });
        std::filesystem::remove(pidPath, fileStatus);

        if (processResult.cancelled || (options.cancellation && options.cancellation->cancelled())) {
            state.status = ImplementationStageStatus::cancelled;
            state.finishedAt = nowUtc();
            state.error = "Cancelled";
            run.status = ImplementationStatus::cancelled;
            run.lastError = "Implementation run cancelled during " + std::string(stageLabel(stage));
            run.updatedAt = nowUtc();
            addArtifact(state.artifacts, relativeArtifact(projectRoot, process.logPath));
            addArtifact(run.artifacts, relativeArtifact(projectRoot, process.logPath));
            report(stage, ImplementationStageStatus::cancelled, run.lastError);
            changed();
            result.cancelled = true;
            return result;
        }
        if (!processResult.succeeded())
            return fail(stage, stageFailureMessage(stage, processResult));

        if (stage == ImplementationStage::elaboration || stage == ImplementationStage::synthesis) {
            parseYosysMetrics(processResult.logTail, run.metrics);
            finalizeYosysGraph(options, runDirectory, stage);
        } else {
            const auto metrics = stageMetricPath(stage);
            if (!metrics.empty())
                mergeOpenRoadMetrics(runDirectory / metrics, run.metrics);
        }

        const auto outputs = stageOutputs(stage, ctsEnabled);
        std::vector<std::string> missing;
        for (const auto &output : outputs.required) {
            const auto path = runDirectory / output;
            if (std::filesystem::is_regular_file(path, fileStatus) && !fileStatus)
                addArtifact(state.artifacts, relativeArtifact(projectRoot, path));
            else
                missing.push_back(output);
        }
        for (const auto &output : outputs.optional) {
            const auto path = runDirectory / output;
            if (std::filesystem::is_regular_file(path, fileStatus) && !fileStatus)
                addArtifact(state.artifacts, relativeArtifact(projectRoot, path));
        }
        addArtifact(state.artifacts, relativeArtifact(projectRoot, process.logPath));
        for (const auto &artifact : state.artifacts)
            addArtifact(run.artifacts, artifact);
        if (!missing.empty()) {
            std::ostringstream message;
            message << stageLabel(stage) << " completed but did not produce:";
            for (const auto &output : missing)
                message << " " << output;
            return fail(stage, message.str());
        }

        state.status = ImplementationStageStatus::succeeded;
        state.finishedAt = nowUtc();
        run.currentStage = stage;
        run.updatedAt = nowUtc();
        result.completedStage = stage;
        report(stage, ImplementationStageStatus::succeeded, std::string(stageLabel(stage)) + " completed");
        changed();
    }

    run.status = ImplementationStatus::succeeded;
    run.lastError.clear();
    run.updatedAt = nowUtc();
    report(options.to, ImplementationStageStatus::succeeded, "Implementation run completed");
    changed();
    result.success = true;
    return result;
}

std::string readImplementationLog(const ImplementationRun &run, const std::filesystem::path &projectRoot,
                                  ImplementationStage stage, std::size_t maximumBytes) {
    const auto relative = implementationStageLogRelativePath(stage);
    if (relative.empty())
        return {};
    return readStageLog(projectRoot / run.directory / relative, maximumBytes == 0 ? 64 * 1024 : maximumBytes);
}

bool cancelImplementationRun(const ImplementationRun &run, const std::filesystem::path &projectRoot,
                             std::string &error) {
#ifndef _WIN32
    const auto pidPath = projectRoot / run.directory / "run.pid";
    std::ifstream input(pidPath);
    int pid = 0;
    if (!(input >> pid) || pid <= 1) {
        error = "No implementation process is recorded for run '" + run.name + "'";
        return false;
    }
    std::ifstream commandLine("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary);
    std::string command;
    if (commandLine)
        command.assign(std::istreambuf_iterator<char>(commandLine), std::istreambuf_iterator<char>());
    if (!command.empty() && command.find("openroad") == std::string::npos &&
        command.find("yosys") == std::string::npos) {
        error = "Recorded PID " + std::to_string(pid) + " is not an OpenROAD or Yosys process";
        return false;
    }
    if (::kill(-pid, SIGTERM) != 0 && ::kill(pid, SIGTERM) != 0) {
        error = "Unable to cancel process " + std::to_string(pid) + ": " + std::strerror(errno);
        return false;
    }
    return true;
#else
    (void)run;
    (void)projectRoot;
    error = "Cancelling a running implementation is not supported on this platform";
    return false;
#endif
}

} // namespace ice
