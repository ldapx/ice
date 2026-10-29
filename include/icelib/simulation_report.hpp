#pragma once

#include "icelib/core.hpp"

#include <chrono>
#include <filesystem>
#include <string>

namespace ice {

// Starter source used when a new project report is created.
std::string defaultSimulationReportTemplate();

// Safe, deterministic stem for a report file. The returned name has no path
// separators and is never empty.
std::string sanitizedReportSlug(const std::string &name);

std::filesystem::path projectReportPath(const std::filesystem::path &projectRoot, const ProjectReport &report);

// Creates reports/<unique-slug>.py and records it in the project. The project
// metadata is modified in memory; callers decide when to save the project.
ObjectId createProjectReport(Project &project, const std::filesystem::path &projectRoot, const std::string &name,
                             const std::string &source = {});

// Reads/writes a report source file atomically. Paths come from ProjectReport
// metadata and are constrained to the project reports/ directory by project
// validation.
std::string readProjectReportSource(const std::filesystem::path &projectRoot, const ProjectReport &report);
void writeProjectReportSource(const std::filesystem::path &projectRoot, const ProjectReport &report,
                              const std::string &source);

struct ReportProcessResult {
    bool timedOut = false;
    int exitCode = -1;
    std::string stdoutText;
    std::string stderrText;
};

// Locates the Python report runner and runs it with a 30-second timeout by
// default. The executable path is normally argv[0], used to find the checkout
// or installed assets relative to the CLI/MCP binary.
ReportProcessResult runProjectReport(const std::filesystem::path &projectRoot, const ProjectReport &report,
                                     const std::filesystem::path &executablePath,
                                     std::chrono::milliseconds timeout = std::chrono::seconds(30));

std::filesystem::path currentExecutablePath();
std::filesystem::path reportRunnerScriptPath(const std::filesystem::path &executablePath);
std::string reportPythonPath(const std::filesystem::path &executablePath);
std::string reportPythonExecutable();

} // namespace ice
