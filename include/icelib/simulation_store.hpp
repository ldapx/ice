#pragma once

#include "icelib/core.hpp"
#include "icelib/simulation.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ice {

// Metadata required to store a simulation run under <project>/build.
struct SimulationRunContext {
    std::filesystem::path projectRoot;
    std::string projectIdentity;
    std::string projectName;
    ObjectId simulationId = 0;
    std::string simulationName;
    ObjectId schematicId = 0;
    std::string schematicName;
    SimulationRequest request;
    std::string directives;
    std::vector<std::string> netlistNotes;
    std::vector<SimulationGraph> graphs;
};

struct SimulationRunArtifact {
    std::filesystem::path path;
    std::string runId;
};

struct StoredSimulationRun {
    std::filesystem::path path;
    std::string runId;
    std::string createdAt;
    ObjectId simulationId = 0;
    std::string simulationName;
    ObjectId schematicId = 0;
    std::string schematicName;
    SimulationResult result;
};

// Reads a stored run file. Throws ProjectError if the file is not a valid
// stored ICE result.
StoredSimulationRun readSimulationRun(const std::filesystem::path &path);

// Finds the newest stored run. Filters are optional; when both are provided
// both must match.
std::optional<StoredSimulationRun> latestSimulationRun(const std::filesystem::path &projectRoot,
                                                       std::optional<ObjectId> simulationId = std::nullopt,
                                                       std::optional<ObjectId> schematicId = std::nullopt);

// Serialize a result using the stored-run schema. The format is deliberately
// new-only: report tooling is not expected to understand the old bare result
// JSON.
std::string serializeSimulationRun(const SimulationRunContext &context, const SimulationResult &result);

// Writes atomically under <projectRoot>/build/simulations/<id>-<slug>/runs/.
// Throws ProjectError if the project root is empty or the write fails.
SimulationRunArtifact writeSimulationRun(const SimulationRunContext &context, const SimulationResult &result);

std::filesystem::path simulationRunDirectory(const std::filesystem::path &projectRoot,
                                             const SimulationRunContext &context);

} // namespace ice
