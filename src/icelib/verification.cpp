#include "icelib/verification.hpp"

#include <algorithm>
#include <future>
#include <iterator>
#include <nlohmann/json.hpp>

namespace ice {
using json = nlohmann::ordered_json;

std::string verificationSeverityName(VerificationSeverity severity) {
    switch (severity) {
    case VerificationSeverity::info:
        return "info";
    case VerificationSeverity::warning:
        return "warning";
    case VerificationSeverity::error:
        return "error";
    }
    return "unknown";
}

VerificationReport LocalVerificationAdapter::run(const Project &project, std::optional<ObjectId> cell) {
    VerificationReport report;
    for (const auto &[cellId, layout] : project.layoutCells) {
        if (cell && *cell != cellId)
            continue;
        Rect contentBounds;
        bool haveContentBounds = false;
        for (const auto &[shapeId, shape] : layout.shapes) {
            if (!shape.rectangle.valid())
                report.drc.push_back({1, VerificationSeverity::error, "min-area", "Shape has non-positive area",
                                      shapeId, shape.rectangle});
            else if (!haveContentBounds) {
                contentBounds = shape.rectangle;
                haveContentBounds = true;
            } else {
                const auto left = std::min(contentBounds.x, shape.rectangle.x);
                const auto bottom = std::min(contentBounds.y, shape.rectangle.y);
                const auto right = std::max(contentBounds.x + contentBounds.width,
                                            shape.rectangle.x + shape.rectangle.width);
                const auto top = std::max(contentBounds.y + contentBounds.height,
                                          shape.rectangle.y + shape.rectangle.height);
                contentBounds = {left, bottom, right - left, top - bottom};
            }
            if (!project.layers.contains(shape.layer))
                report.drc.push_back({1, VerificationSeverity::error, "layer-exists",
                                      "Shape references a missing layer", shapeId, shape.rectangle});
            if (shape.rectangle.width < project.grid || shape.rectangle.height < project.grid)
                report.drc.push_back({1, VerificationSeverity::warning, "min-width",
                                      "Shape is narrower than the project grid", shapeId, shape.rectangle});
        }
        for (const auto &[portId, port] : layout.ports) {
            if (!port.rectangle.valid()) {
                report.drc.push_back({1, VerificationSeverity::error, "pin-area",
                                      "Pin has non-positive area", portId, port.rectangle});
                continue;
            }
            if (!project.layers.contains(port.layer))
                report.drc.push_back({1, VerificationSeverity::error, "pin-layer",
                                      "Pin references a missing layer", portId, port.rectangle});
            if (port.rectangle.width < project.grid || port.rectangle.height < project.grid)
                report.drc.push_back({1, VerificationSeverity::warning, "pin-min-width",
                                      "Pin is narrower than the project grid", portId, port.rectangle});
            if (port.net != 0 && !layout.nets.contains(port.net))
                report.drc.push_back({1, VerificationSeverity::error, "pin-net",
                                      "Pin references a missing layout net", portId, port.rectangle});
            if (haveContentBounds &&
                (port.rectangle.x < contentBounds.x || port.rectangle.y < contentBounds.y ||
                 port.rectangle.x + port.rectangle.width > contentBounds.x + contentBounds.width ||
                 port.rectangle.y + port.rectangle.height > contentBounds.y + contentBounds.height))
                report.drc.push_back({1, VerificationSeverity::warning, "pin-outside-cell",
                                      "Pin extends outside the layout content bounds", portId, port.rectangle});
        }
        for (auto left = layout.ports.begin(); left != layout.ports.end(); ++left) {
            for (auto right = std::next(left); right != layout.ports.end(); ++right) {
                const auto &first = left->second;
                const auto &second = right->second;
                if (first.layer != second.layer || first.net == 0 || second.net == 0 || first.net == second.net)
                    continue;
                if (!first.rectangle.valid() || !second.rectangle.valid())
                    continue;
                const auto &a = first.rectangle;
                const auto &b = second.rectangle;
                if (a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height)
                    report.drc.push_back({1, VerificationSeverity::error, "pin-short",
                                          "Pins '" + first.name + "' and '" + second.name +
                                              "' on the same layer overlap but belong to different nets",
                                          right->first, b});
            }
        }
    }
    for (const auto &[cellId, schematic] : project.schematicCells) {
        if (schematic.instances.size() != schematic.symbols.size())
            report.lvs.push_back(
                {1, "device-count", "Schematic symbol and instance counts differ", cellId, std::nullopt});
    }
    for (const auto &[_, layout] : project.layoutCells)
        for (const auto &[shapeId, shape] : layout.shapes)
            if (shape.net)
                report.pex.push_back({1, std::to_string(*shape.net),
                                      static_cast<double>(shape.rectangle.width) / 1000.0,
                                      static_cast<double>(shape.rectangle.width) *
                                          static_cast<double>(shape.rectangle.height) / 1'000'000.0});
    return report;
}

std::future<VerificationReport> VerificationScheduler::schedule(const Project &project,
                                                                std::shared_ptr<VerificationAdapter> adapter,
                                                                std::optional<ObjectId> cell) {
    return std::async(std::launch::async,
                      [project, adapter = std::move(adapter), cell] { return adapter->run(project, cell); });
}

std::string serializeVerificationReport(const VerificationReport &report) {
    json output{{"schema_version", report.schemaVersion},
                {"drc", json::array()},
                {"lvs", json::array()},
                {"pex", json::array()}};
    const auto optionalId = [](std::optional<ObjectId> id) -> json { return id ? json(*id) : json(nullptr); };
    for (const auto &item : report.drc)
        output["drc"].push_back({{"schema_version", item.schemaVersion},
                                 {"severity", verificationSeverityName(item.severity)},
                                 {"rule", item.rule},
                                 {"message", item.message},
                                 {"object", optionalId(item.object)},
                                 {"x", item.location.x},
                                 {"y", item.location.y},
                                 {"width", item.location.width},
                                 {"height", item.location.height},
                                 {"cell", item.cell}});
    for (const auto &item : report.lvs)
        output["lvs"].push_back({{"schema_version", item.schemaVersion},
                                 {"severity", verificationSeverityName(item.severity)},
                                 {"category", item.category},
                                 {"message", item.message},
                                 {"schematic_object", optionalId(item.schematicObject)},
                                 {"layout_object", optionalId(item.layoutObject)}});
    for (const auto &item : report.pex)
        output["pex"].push_back({{"schema_version", item.schemaVersion},
                                 {"net", item.net},
                                 {"resistance", item.resistance},
                                 {"capacitance", item.capacitance}});
    return output.dump(2) + "\n";
}

SimulationRequest applyParasitics(const SimulationRequest &request, const VerificationReport &report) {
    auto result = request;
    for (const auto &element : report.pex)
        result.time_constant += element.resistance * element.capacitance;
    return result;
}
} // namespace ice
