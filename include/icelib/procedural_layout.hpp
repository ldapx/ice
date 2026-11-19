#pragma once

#include "icelib/core.hpp"

#include <filesystem>
#include <string>

namespace ice {

// A small starter script that demonstrates the layout builder API.
std::string defaultProceduralLayoutScript();

// Locates the Python procedural-layout runner. `executablePath` is the ICE
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

// Runs the procedural layout script in a subprocess and replaces its generated
// static layout cell. The project is left unchanged when generation fails.
ProceduralLayoutRunResult regenerateProceduralLayout(Project &project, ObjectId proceduralLayoutId,
                                                     const std::filesystem::path &executablePath = {},
                                                     int timeoutSeconds = 60);

} // namespace ice
