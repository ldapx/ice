#pragma once

#include "icelib/core.hpp"
#include "icelib/simulation.hpp"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ice {

// Starter source used when a new simulation script is created.
std::string defaultSimulationReportTemplate();

// Safe, deterministic stem for a simulation script file. The returned name has
// no path separators and is never empty.
std::string sanitizedReportSlug(const std::string &name);

std::filesystem::path projectReportPath(const std::filesystem::path &projectRoot, const ProjectReport &report);
// Graph payload written by the latest run of a simulation script.
std::filesystem::path projectSimulationScriptGraphPath(const std::filesystem::path &projectRoot,
                                                       const ProjectReport &report);

// Creates scripts/<unique-slug>.py and records it in the project. The project
// metadata is modified in memory; callers decide when to save the project.
ObjectId createProjectReport(Project &project, const std::filesystem::path &projectRoot, const std::string &name,
                             const std::string &source = {});

// Reads/writes a simulation script source file atomically. Paths come from
// project metadata and are constrained to the project scripts/ directory by
// project validation.
std::string readProjectReportSource(const std::filesystem::path &projectRoot, const ProjectReport &report);
void writeProjectReportSource(const std::filesystem::path &projectRoot, const ProjectReport &report,
                              const std::string &source);

struct SimulationScriptGraph {
    std::string name;
    std::string xAxis = "time";
    std::vector<double> x;
    std::vector<SimulationTrace> series;
    std::optional<double> xMin;
    std::optional<double> xMax;
    std::optional<double> yMin;
    std::optional<double> yMax;
};

struct ReportProcessResult {
    bool timedOut = false;
    int exitCode = -1;
    std::string stdoutText;
    std::string stderrText;
    // Graph payload emitted by the simulation script, if any.
    std::string graphsText;
};

// Parses the graph JSON emitted by assets/simulation/report_runner.py.
std::vector<SimulationScriptGraph> parseSimulationScriptGraphs(const std::string &text);

// Locates the shared Python script runner and runs it with a 30-second timeout
// by default. The executable path is normally argv[0], used to find the checkout
// or installed assets relative to the CLI/MCP binary.
ReportProcessResult runProjectReport(const std::filesystem::path &projectRoot, const ProjectReport &report,
                                     const std::filesystem::path &executablePath,
                                     std::chrono::milliseconds timeout = std::chrono::seconds(30));

std::filesystem::path currentExecutablePath();
std::filesystem::path reportRunnerScriptPath(const std::filesystem::path &executablePath);
std::string reportPythonPath(const std::filesystem::path &executablePath);
std::string reportPythonExecutable();

} // namespace ice
