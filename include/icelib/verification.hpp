#pragma once

#include "icelib/core.hpp"
#include "icelib/simulation.hpp"

#include <chrono>
#include <future>
#include <optional>

namespace ice {

enum class VerificationSeverity { info, warning, error };

struct DrcViolation {
    int schemaVersion = 1;
    VerificationSeverity severity = VerificationSeverity::error;
    std::string rule;
    std::string message;
    std::optional<ObjectId> object;
    Rect location;
};

struct LvsMismatch {
    int schemaVersion = 1;
    std::string category;
    std::string message;
    std::optional<ObjectId> schematicObject;
    std::optional<ObjectId> layoutObject;
};

struct ParasiticElement {
    int schemaVersion = 1;
    std::string net;
    double resistance = 0;
    double capacitance = 0;
};

struct VerificationReport {
    int schemaVersion = 1;
    std::vector<DrcViolation> drc;
    std::vector<LvsMismatch> lvs;
    std::vector<ParasiticElement> pex;
};

class VerificationAdapter {
  public:
    virtual ~VerificationAdapter() = default;
    virtual VerificationReport run(const Project &project, std::optional<ObjectId> cell = std::nullopt) = 0;
};

class LocalVerificationAdapter final : public VerificationAdapter {
  public:
    VerificationReport run(const Project &project, std::optional<ObjectId> cell = std::nullopt) override;
};

class VerificationScheduler {
  public:
    std::future<VerificationReport> schedule(const Project &project, std::shared_ptr<VerificationAdapter> adapter,
                                             std::optional<ObjectId> cell = std::nullopt);
};

std::string serializeVerificationReport(const VerificationReport &report);
std::string verificationSeverityName(VerificationSeverity severity);
SimulationRequest applyParasitics(const SimulationRequest &request, const VerificationReport &report);

} // namespace ice
