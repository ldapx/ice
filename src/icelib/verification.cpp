#include "icelib/verification.hpp"

#include <future>
#include <nlohmann/json.hpp>

namespace ice {
using json = nlohmann::ordered_json;

std::string verificationSeverityName(VerificationSeverity severity) {
    switch (severity) {
    case VerificationSeverity::info: return "info";
    case VerificationSeverity::warning: return "warning";
    case VerificationSeverity::error: return "error";
    }
    return "unknown";
}

VerificationReport LocalVerificationAdapter::run(const Project& project, std::optional<ObjectId> cell) {
    VerificationReport report;
    for (const auto& [cellId, layout] : project.layoutCells) {
        if (cell && *cell != cellId) continue;
        for (const auto& [shapeId, shape] : layout.shapes) {
            if (!shape.rectangle.valid())
                report.drc.push_back({1, VerificationSeverity::error, "min-area",
                                      "Shape has non-positive area", shapeId, shape.rectangle});
            if (!project.layers.contains(shape.layer))
                report.drc.push_back({1, VerificationSeverity::error, "layer-exists",
                                      "Shape references a missing layer", shapeId, shape.rectangle});
            if (shape.rectangle.width < project.grid || shape.rectangle.height < project.grid)
                report.drc.push_back({1, VerificationSeverity::warning, "min-width",
                                      "Shape is narrower than the project grid", shapeId, shape.rectangle});
        }
    }
    for (const auto& [cellId, schematic] : project.schematicCells) {
        if (schematic.instances.size() != schematic.symbols.size())
            report.lvs.push_back({1, "device-count", "Schematic symbol and instance counts differ", cellId, std::nullopt});
    }
    for (const auto& [_, layout] : project.layoutCells)
        for (const auto& [shapeId, shape] : layout.shapes)
            if (shape.net) report.pex.push_back({1, std::to_string(*shape.net),
                                                  static_cast<double>(shape.rectangle.width) / 1000.0,
                                                  static_cast<double>(shape.rectangle.width * shape.rectangle.height) / 1'000'000.0});
    return report;
}

std::future<VerificationReport> VerificationScheduler::schedule(
    const Project& project, std::shared_ptr<VerificationAdapter> adapter, std::optional<ObjectId> cell) {
    return std::async(std::launch::async, [project, adapter = std::move(adapter), cell] {
        return adapter->run(project, cell);
    });
}

std::string serializeVerificationReport(const VerificationReport& report) {
    json output{{"schema_version", report.schemaVersion}, {"drc", json::array()}, {"lvs", json::array()}, {"pex", json::array()}};
    for (const auto& item : report.drc)
        output["drc"].push_back({{"schema_version", item.schemaVersion}, {"severity", verificationSeverityName(item.severity)},
                                 {"rule", item.rule}, {"message", item.message}, {"object", item.object.value_or(0)},
                                 {"x", item.location.x}, {"y", item.location.y}, {"width", item.location.width},
                                 {"height", item.location.height}});
    for (const auto& item : report.lvs)
        output["lvs"].push_back({{"schema_version", item.schemaVersion}, {"category", item.category},
                                 {"message", item.message}, {"schematic_object", item.schematicObject.value_or(0)},
                                 {"layout_object", item.layoutObject.value_or(0)}});
    for (const auto& item : report.pex)
        output["pex"].push_back({{"schema_version", item.schemaVersion}, {"net", item.net},
                                 {"resistance", item.resistance}, {"capacitance", item.capacitance}});
    return output.dump(2) + "\n";
}

SimulationRequest applyParasitics(const SimulationRequest& request, const VerificationReport& report) {
    auto result = request;
    for (const auto& element : report.pex) result.time_constant += element.resistance * element.capacitance;
    return result;
}
} // namespace ice
