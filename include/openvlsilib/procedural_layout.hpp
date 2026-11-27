#pragma once

#include "openvlsilib/core.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace openvlsi {

// A small starter script that demonstrates the layout builder API.
std::string defaultProceduralLayoutScript();

// Locates the Python procedural-layout runner. `executablePath` is the OpenVLSI
// executable (or an empty path when the caller has no executable context).
std::filesystem::path proceduralRunnerScriptPath(const std::filesystem::path &executablePath = {});

// Stable content hashes used to detect whether generated geometry is stale.
std::string proceduralScriptHash(const std::string &script);
std::string proceduralParametersHash(const std::vector<Parameter> &parameters);

// True when the script or parameter values changed since the last successful
// generation.
bool proceduralLayoutNeedsRegeneration(const ProceduralLayout &layout);

struct ProceduralLayoutRunResult {
    bool ok = false;
    std::string error;
    std::string log;
    std::string scriptHash;
    std::string parameterHash;
    std::string outputHash;
    std::string pdkVersion;
    // Structured agent metadata from the runner, as JSON text.
    std::string layoutQuality;
    std::string layoutPlan;
    std::string layoutReview;
};

// JSON result from the Python layout contract tools (API manifest or script
// validation). The payload is produced by procedural_runner.py.
struct ProceduralLayoutJsonResult {
    bool ok = false;
    std::string error;
    std::string log;
    std::string json;
};

// Describe the live Python layout API. `options` are extra runner arguments,
// for example {"--api-name", "device"}.
ProceduralLayoutJsonResult describeProceduralLayoutApi(const std::string &pdk,
                                                       const std::filesystem::path &pdkRoot,
                                                       const std::vector<std::string> &options = {},
                                                       const std::filesystem::path &executablePath = {},
                                                       int timeoutSeconds = 30);

// Validate a layout script without executing package code.
ProceduralLayoutJsonResult validateProceduralLayoutScript(const std::string &script,
                                                          const std::string &pdk,
                                                          const std::filesystem::path &pdkRoot,
                                                          bool requirePlan = true,
                                                          bool requireFinish = true,
                                                          const std::filesystem::path &executablePath = {},
                                                          int timeoutSeconds = 30);

// Runs the procedural layout script in a subprocess and replaces its generated
// static layout cell. The project is left unchanged when generation fails.
ProceduralLayoutRunResult regenerateProceduralLayout(Project &project, ObjectId proceduralLayoutId,
                                                     const std::filesystem::path &executablePath = {},
                                                     int timeoutSeconds = 60);

} // namespace openvlsi
