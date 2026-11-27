#pragma once

#include "openvlsilib/core.hpp"

#include <chrono>
#include <future>
#include <optional>

namespace openvlsi {

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

// Identifies a schematic or layout object referenced by an LVS result. Layout
// geometry groups and synthetic topology nets may have id 0 but still carry a
// name and bounds so the GUI can navigate to them.
enum class LvsObjectKind { device, net, port, plannedDevice, instance, shape, connection, geometry };

// How a schematic device was paired with a layout device. `linked` comes from
// an authored schematic_instance link, `name` from a unique normalized name,
// and `topology` from a unique terminal signature.
enum class LvsMatchMethod { none, linked, name, topology };

const char *lvsObjectKindName(LvsObjectKind kind);
const char *lvsMatchMethodName(LvsMatchMethod method);

struct LvsObjectRef {
    LvsObjectKind kind = LvsObjectKind::device;
    ObjectId cell = 0;
    ObjectId id = 0;
    std::string name;
    std::optional<Rect> bounds;
};

struct LvsPinMatch {
    std::string schematicPin;
    std::string layoutPin;
    std::string schematicNet;
    std::string layoutNet;
    bool matched = false;
    // True for two-terminal passives whose pins are connected in reverse
    // order. This is electrically equivalent and is reported for inspection.
    bool swapped = false;
};

struct LvsParameterMatch {
    std::string name;
    std::string schematicValue;
    std::string layoutValue;
    bool matched = false;
};

struct LvsDeviceMatch {
    std::optional<LvsObjectRef> schematic;
    std::optional<LvsObjectRef> layout;
    std::string schematicType;
    std::string layoutType;
    LvsMatchMethod method = LvsMatchMethod::none;
    // False for one-sided and mismatched pairs. A pair with a type or
    // parameter mismatch still has both refs populated.
    bool matched = false;
    bool parametersCompared = false;
    std::vector<LvsPinMatch> pins;
    std::vector<LvsParameterMatch> parameters;
};

struct LvsNetMatch {
    std::optional<LvsObjectRef> schematic;
    std::optional<LvsObjectRef> layout;
    std::vector<std::string> schematicConnections;
    std::vector<std::string> layoutConnections;
    bool matched = false;
    bool ground = false;
};

struct LvsPortMatch {
    std::optional<LvsObjectRef> schematic;
    std::optional<LvsObjectRef> layout;
    std::string schematicDirection;
    std::string layoutDirection;
    std::string schematicNet;
    std::string layoutNet;
    bool matched = false;
    bool directionMatched = false;
    bool netMatched = false;
};

struct LvsCandidate {
    LvsObjectRef object;
    double score = 0.0;
    std::string reason;
};

struct LvsSummary {
    int devicesMatched = 0;
    int devicesMismatched = 0;
    int devicesSchematicOnly = 0;
    int devicesLayoutOnly = 0;
    int netsMatched = 0;
    int netsSchematicOnly = 0;
    int netsLayoutOnly = 0;
    int portsMatched = 0;
};

struct LvsMismatch {
    int schemaVersion = 1;
    std::string category;
    std::string message;
    std::optional<ObjectId> schematicObject;
    std::optional<ObjectId> layoutObject;
    VerificationSeverity severity = VerificationSeverity::error;
    // Typed, descriptive counterparts of the raw object ids above. Populated
    // for every mismatch when the object can be resolved.
    std::optional<LvsObjectRef> schematicRef;
    std::optional<LvsObjectRef> layoutRef;
    std::string hint;
    std::vector<LvsCandidate> candidates;
};

struct LvsOptions {
    bool compareParameters = true;
};

// Result of comparing a schematic cell against a layout cell. Drawn, text SPICE,
// and structural SystemVerilog source schematics are supported. Layout
// connectivity combines planned connections and terminal metadata with
// geometry-derived nets (shapes, routed paths, vias, and placed-device port
// overlaps). Device identity is matched through schematic links, names, and a
// topology fallback after multiplicity expansion.
//
// Mismatches describe failures. Devices, nets, and ports describe the complete
// correspondence, including matched objects, match methods, terminal maps,
// parameter values, and layout bounds. The correspondence is derived for each
// run; authored links remain stored on layout instances and planned devices.
struct LvsReport {
    int schemaVersion = 2;
    std::string schematic;
    std::string layout;
    // Number of top-level ports matched by normalized name.
    int portsMatched = 0;
    LvsSummary summary;
    std::vector<LvsMismatch> mismatches;
    std::vector<LvsDeviceMatch> devices;
    std::vector<LvsNetMatch> nets;
    std::vector<LvsPortMatch> ports;
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

} // namespace openvlsi
