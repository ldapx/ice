#pragma once

#include "icelib/core.hpp"

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
    // Cell name reported by an external DRC tool. Empty for local checks.
    std::string cell;
};

struct LvsMismatch {
    int schemaVersion = 1;
    std::string category;
    std::string message;
    std::optional<ObjectId> schematicObject;
    std::optional<ObjectId> layoutObject;
    VerificationSeverity severity = VerificationSeverity::error;
};

struct LvsOptions {
    bool compareParameters = true;
};

// Result of comparing a schematic cell against a layout cell. Layout
// connectivity is taken from planned-device connections; device identity is
// matched through planned-device schematic links first and reference names
// second.
struct LvsReport {
    int schemaVersion = 1;
    std::string schematic;
    std::string layout;
    // Number of top-level pins matched by normalized name.
    int portsMatched = 0;
    std::vector<LvsMismatch> mismatches;
    bool matched() const;
};

// Infers the schematic a layout was derived from. Planned-device links take
// precedence; otherwise exact and "<name> Layout" name matches are used.
ObjectId schematicForLayout(const Project &project, ObjectId layoutCellId);
LvsReport runLvs(const Project &project, ObjectId schematicCellId, ObjectId layoutCellId,
                 const LvsOptions &options = {});
std::string serializeLvsReport(const LvsReport &report);

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

} // namespace ice
