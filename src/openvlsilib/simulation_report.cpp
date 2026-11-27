#include "openvlsilib/simulation_report.hpp"

#include "openvlsilib/platform.hpp"
#include "openvlsilib/process.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

namespace openvlsi {
namespace {

std::string trimmed(std::string text) {
    const auto notSpace = [](unsigned char character) { return !std::isspace(character); };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), notSpace));
    text.erase(std::find_if(text.rbegin(), text.rend(), notSpace).base(), text.end());
    return text;
}

std::filesystem::path normalizedProjectRoot(const std::filesystem::path &path) {
    try {
        return projectRootDirectory(path);
    } catch (const ProjectError &) {
        throw ProjectError("report.path", "Project root is empty");
    }
}

void atomicWriteFile(const std::filesystem::path &path, const std::string &text) {
    std::error_code status;
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path(), status);
    if (status)
        throw ProjectError("report.write", "Unable to create report directory: " + path.parent_path().string());
    const auto temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output)
            throw ProjectError("report.write", "Unable to write report source: " + path.string());
        output << text;
        if (!output.good())
            throw ProjectError("report.write", "Unable to write report source: " + path.string());
    }
    std::filesystem::rename(temporary, path, status);
    if (status) {
        std::filesystem::remove(temporary, status);
        throw ProjectError("report.write", "Unable to finalize report source: " + path.string());
    }
}

bool isRegularFile(const std::filesystem::path &path) {
    std::error_code status;
    return std::filesystem::is_regular_file(path, status) && !status;
}

std::string readFile(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    return {std::istreambuf_iterator<char>(input), {}};
}

std::filesystem::path normalizedExecutable(const std::filesystem::path &executablePath) {
    if (executablePath.empty())
        return {};
    std::error_code status;
    auto absolute = std::filesystem::absolute(executablePath, status);
    if (status)
        return executablePath;
    return absolute;
}

} // namespace

std::vector<SimulationScriptGraph> parseSimulationScriptGraphs(const std::string &text) {
    if (trimmed(text).empty())
        return {};
    try {
        const auto document = nlohmann::json::parse(text);
        if (document.value("format", std::string{}) != "openvlsi-simulation-script-graphs")
            throw ProjectError("script.graphs.format", "Unsupported simulation script graph format");
        std::vector<SimulationScriptGraph> graphs;
        const auto entries = document.find("graphs");
        if (entries == document.end() || !entries->is_array())
            return graphs;
        const auto optionalDouble = [](const nlohmann::json &object, const char *key) -> std::optional<double> {
            const auto value = object.find(key);
            if (value == object.end() || value->is_null())
                return std::nullopt;
            return value->get<double>();
        };
        for (const auto &entry : *entries) {
            SimulationScriptGraph graph;
            graph.name = entry.value("name", std::string{});
            graph.xAxis = entry.value("x_axis", std::string{"time"});
            if (graph.name.empty())
                graph.name = "Graph " + std::to_string(graphs.size() + 1);
            if (!entry.contains("x") || !entry.at("x").is_array())
                throw ProjectError("script.graphs.x", "Simulation script graph is missing x data");
            graph.x = entry.at("x").get<std::vector<double>>();
            if (!entry.contains("series") || !entry.at("series").is_array())
                throw ProjectError("script.graphs.series", "Simulation script graph is missing series data");
            for (const auto &series : entry.at("series")) {
                SimulationTrace trace;
                trace.name = series.value("name", std::string{});
                if (trace.name.empty() || !series.contains("values") || !series.at("values").is_array())
                    throw ProjectError("script.graphs.signal", "Simulation script graph contains an invalid signal");
                trace.values = series.at("values").get<std::vector<double>>();
                if (trace.values.size() != graph.x.size())
                    throw ProjectError("script.graphs.signal",
                                       "Simulation script graph signal length does not match x data");
                graph.series.push_back(std::move(trace));
            }
            graph.xMin = optionalDouble(entry, "x_min");
            graph.xMax = optionalDouble(entry, "x_max");
            graph.yMin = optionalDouble(entry, "y_min");
            graph.yMax = optionalDouble(entry, "y_max");
            graphs.push_back(std::move(graph));
        }
        return graphs;
    } catch (const ProjectError &) {
        throw;
    } catch (const std::exception &error) {
        throw ProjectError("script.graphs.parse", "Invalid simulation script graph JSON: " + std::string(error.what()));
    }
}

std::string defaultSimulationReportTemplate() {
    return R"PY(# OpenVLSI simulation script
# Return Markdown from build_script(ctx). The GUI appends messages and output
# to the Messages tab.
#
# Context helpers:
#   ctx.simulations()       list simulation runs available in the project
#   ctx.runs(simulation)    run metadata for one or all simulations
#   ctx.latest(simulation)  newest run for one simulation or the project
#   ctx.latest_many()       newest run for each simulation
#   ctx.load(path)          load an explicit stored run
#
# Add GUI graphs with either stored-run data or explicit arrays:
#   ctx.graph("RLC response", run=run, signals=["in", "n2"])
#   ctx.graph("Derived metric", x=time, y={"gain": values})

def _fmt(value, unit=""):
    return "n/a" if value is None else f"{value:.6g}{unit}"


def build_script(ctx):
    runs = ctx.latest_many()
    if not runs:
        return "# Simulation script\n\nNo stored simulation runs yet."

    for name, run in sorted(runs.items()):
        if run.trace_names:
            ctx.graph(name, run=run, signals=run.trace_names)

    rows = []
    for name, run in sorted(runs.items()):
        if not run.has_signal("v(n1)"):
            continue
        metrics = run.trace("v(n1)").metrics()
        rows.append(
            "| {} | {} | {} | {} | {} |".format(
                name,
                _fmt(metrics.overshoot_percent, "%"),
                _fmt(metrics.rise_time, " s"),
                _fmt(metrics.settling_time, " s"),
                _fmt(metrics.ringing_frequency, " Hz"),
            )
        )

    if not rows:
        return "# Simulation script\n\nNo runs capture `v(n1)`."

    return """# Simulation comparison

| Simulation | Overshoot | Rise time | Settling time | Ringing |
| --- | ---: | ---: | ---: | ---: |
{}
""".format("\n".join(rows))
)PY";
}

std::string sanitizedReportSlug(const std::string &name) {
    std::string slug;
    slug.reserve(name.size());
    bool lastWasSeparator = false;
    for (const unsigned char character : name) {
        if (std::isalnum(character) || character == '-' || character == '_') {
            slug.push_back(static_cast<char>(std::tolower(character)));
            lastWasSeparator = false;
        } else if (!lastWasSeparator && !slug.empty()) {
            slug.push_back('-');
            lastWasSeparator = true;
        }
    }
    while (!slug.empty() && slug.back() == '-')
        slug.pop_back();
    if (slug.size() > 80)
        slug.resize(80);
    while (!slug.empty() && slug.back() == '-')
        slug.pop_back();
    return slug.empty() ? "simulation-script" : slug;
}

std::filesystem::path projectReportPath(const std::filesystem::path &projectRoot, const ProjectReport &report) {
    return normalizedProjectRoot(projectRoot) / report.path;
}

std::filesystem::path projectSimulationScriptGraphPath(const std::filesystem::path &projectRoot,
                                                       const ProjectReport &report) {
    return normalizedProjectRoot(projectRoot) / "build" / "simulation-scripts" /
           (std::to_string(report.id) + "-" + sanitizedReportSlug(report.name)) / "graphs.json";
}

ObjectId createProjectReport(Project &project, const std::filesystem::path &projectRoot, const std::string &name,
                             const std::string &source) {
    const auto reportName = trimmed(name);
    if (reportName.empty())
        throw ProjectError("report.name", "Report name is required");
    for (const auto &[_, report] : project.reports) {
        if (report.name == reportName)
            throw ProjectError("report.name.duplicate", "Report name is already in use: " + reportName);
    }

    const auto root = normalizedProjectRoot(projectRoot);
    const auto scriptsDirectory = root / "scripts";
    std::error_code status;
    std::filesystem::create_directories(scriptsDirectory, status);
    if (status)
        throw ProjectError("report.write",
                           "Unable to create simulation scripts directory: " + scriptsDirectory.string());

    const auto stem = sanitizedReportSlug(reportName);
    auto path = scriptsDirectory / (stem + ".py");
    for (int suffix = 2; std::filesystem::exists(path, status) && !status; ++suffix)
        path = scriptsDirectory / (stem + "-" + std::to_string(suffix) + ".py");
    atomicWriteFile(path, source.empty() ? defaultSimulationReportTemplate() : source);

    const auto id = project.nextId();
    ProjectReport report;
    report.id = id;
    report.name = reportName;
    report.path = "scripts/" + path.filename().string();
    project.reports.emplace(id, std::move(report));
    return id;
}

std::string readProjectReportSource(const std::filesystem::path &projectRoot, const ProjectReport &report) {
    const auto path = projectReportPath(projectRoot, report);
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw ProjectError("report.read", "Unable to read report source: " + path.string());
    return {std::istreambuf_iterator<char>(input), {}};
}

void writeProjectReportSource(const std::filesystem::path &projectRoot, const ProjectReport &report,
                              const std::string &source) {
    atomicWriteFile(projectReportPath(projectRoot, report), source);
}

std::filesystem::path reportRunnerScriptPath(const std::filesystem::path &executablePath) {
    if (const char *configured = std::getenv("OPENVLSI_REPORT_RUNNER"); configured && *configured) {
        if (isRegularFile(configured))
            return configured;
    }

    std::vector<std::filesystem::path> candidates;
    const auto executable = normalizedExecutable(executablePath);
    if (!executable.empty()) {
        auto directory = executable.parent_path();
        for (int level = 0; level < 6 && !directory.empty(); ++level) {
            candidates.push_back(directory / "assets" / "simulation" / "report_runner.py");
            candidates.push_back(directory / "share" / "openvlsi" / "assets" / "simulation" / "report_runner.py");
            directory = directory.parent_path();
        }
    }
    for (const auto &root : applicationResourceDirectories())
        candidates.push_back(root / "assets" / "simulation" / "report_runner.py");
    const auto home = homeDirectory();
    if (!home.empty())
        candidates.push_back(home / ".local" / "share" / "openvlsi" / "assets" / "simulation" / "report_runner.py");
    for (const auto &candidate : candidates) {
        if (isRegularFile(candidate))
            return candidate;
    }
    return {};
}

std::string reportPythonPath(const std::filesystem::path &executablePath) {
    std::vector<std::string> paths;
    const auto appendIfDirectory = [&](const std::filesystem::path &path) {
        std::error_code status;
        if (!path.empty() && std::filesystem::is_directory(path, status) && !status)
            paths.push_back(path.string());
    };
    for (const auto &root : applicationResourceDirectories()) {
        appendIfDirectory(root / "python");
        appendIfDirectory(root);
    }
    const auto executable = normalizedExecutable(executablePath);
    if (!executable.empty()) {
        const auto directory = executable.parent_path();
        appendIfDirectory(directory / "python");
        appendIfDirectory(directory / ".." / "lib");
        appendIfDirectory(directory / ".." / "share" / "openvlsi" / "python");
    }
    const auto home = homeDirectory();
    if (!home.empty()) {
        appendIfDirectory(home / ".local" / "share" / "openvlsi" / "python");
        appendIfDirectory(home / ".local" / "lib");
    }
    if (const char *inherited = std::getenv("PYTHONPATH"); inherited && *inherited) {
        for (auto &entry : splitPathList(inherited)) {
            if (!entry.empty())
                paths.push_back(std::move(entry));
        }
    }

    std::ostringstream result;
    for (std::size_t index = 0; index < paths.size(); ++index) {
        if (index)
            result << pathListSeparator();
        result << paths[index];
    }
    return result.str();
}

std::string reportPythonExecutable() { return defaultPythonExecutable(); }

ReportProcessResult runProjectReport(const std::filesystem::path &projectRoot, const ProjectReport &report,
                                     const std::filesystem::path &executablePath, std::chrono::milliseconds timeout) {
    const auto root = normalizedProjectRoot(projectRoot);
    const auto reportFile = projectReportPath(root, report);
    if (!isRegularFile(reportFile))
        throw ProjectError("report.run", "Report file is missing: " + reportFile.string());
    const auto runner = reportRunnerScriptPath(executablePath);
    if (runner.empty())
        throw ProjectError("report.runner", "Python report runner is not available. Set OPENVLSI_REPORT_RUNNER to "
                                            "assets/simulation/report_runner.py or install it under "
                                            "<prefix>/share/openvlsi/assets/simulation/report_runner.py.");
    const auto python = reportPythonExecutable();
    const auto pythonPath = reportPythonPath(executablePath);
    const auto graphsPath = projectSimulationScriptGraphPath(root, report);
    std::error_code graphStatus;
    std::filesystem::create_directories(graphsPath.parent_path(), graphStatus);
    std::filesystem::remove(graphsPath, graphStatus);

    ProcessOptions options;
    options.arguments = {python,     runner.string(),     "--project-root", root.string(),
                         "--report", reportFile.string(), "--graph-output", graphsPath.string()};
    options.workingDirectory = root;
    options.logTailBytes = 256 * 1024;
    if (timeout.count() > 0)
        options.timeoutSeconds = static_cast<int>(std::max<std::int64_t>(1, (timeout.count() + 999) / 1000));
    if (!pythonPath.empty())
        options.environment["PYTHONPATH"] = pythonPath;

    const auto process = runProcess(options);
    ReportProcessResult result;
    result.timedOut = process.timedOut;
    result.exitCode = process.exitCode;
    result.stdoutText = process.logTail;
    result.stderrText = process.error;
    result.graphsText = readFile(graphsPath);
    if (result.exitCode == -1 && result.stderrText.empty())
        result.stderrText = "Unable to start the report process.";
    return result;
}

} // namespace openvlsi
