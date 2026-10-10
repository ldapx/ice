#include "icelib/interchange.hpp"

#include <fstream>
#include <nlohmann/json.hpp>

namespace ice {
using json = nlohmann::ordered_json;

void LayoutInterchange::exportGdsSubset(const Project& project, ObjectId cellId,
                                        const std::filesystem::path& path,
                                        const InterchangeOptions& options) {
    auto cell = project.layoutCells.find(cellId);
    if (cell == project.layoutCells.end()) throw ProjectError("interchange.cell", "Layout cell does not exist");
    json output{{"format", "ice-gdsii-subset"}, {"schema_version", options.schemaVersion},
                {"project_identity", project.identity}, {"cell_id", cellId}, {"cell_name", cell->second.name},
                {"units_nm", project.units.toNanometers(1)}, {"rectangles", json::array()}};
    for (const auto& [id, shape] : cell->second.shapes)
        output["rectangles"].push_back({{"id", id}, {"layer", shape.layer}, {"x", shape.rectangle.x},
                                        {"y", shape.rectangle.y}, {"width", shape.rectangle.width},
                                        {"height", shape.rectangle.height}});
    std::ofstream stream(path);
    if (!stream) throw ProjectError("interchange.export", "Unable to open interchange output");
    stream << output.dump(2) << '\n';
}

Project LayoutInterchange::importGdsSubset(const std::filesystem::path& path,
                                           const InterchangeOptions& options) {
    std::ifstream stream(path);
    if (!stream) throw ProjectError("interchange.import", "Unable to open interchange input");
    try {
        auto input = json::parse(stream);
        if (input.at("format") != "ice-gdsii-subset" || input.at("schema_version") != options.schemaVersion)
            throw ProjectError("interchange.schema", "Unsupported GDSII subset schema");
        Project project;
        project.identity = input.at("project_identity");
        project.name = input.at("cell_name");
        auto layer = project.nextId();
        project.layers.emplace(layer, Layer{layer, "imported", 1});
        auto cell = project.nextId();
        LayoutCell layout{cell, input.at("cell_name")};
        for (const auto& rectangle : input.at("rectangles")) {
            auto id = project.nextId();
            layout.shapes.emplace(id, Shape{id, layer, std::nullopt,
                                            {rectangle.at("x"), rectangle.at("y"), rectangle.at("width"),
                                             rectangle.at("height")}});
        }
        project.layoutCells.emplace(cell, std::move(layout));
        return project;
    } catch (const ProjectError&) {
        throw;
    } catch (const std::exception& error) {
        throw ProjectError("interchange.parse", error.what());
    }
}
} // namespace ice
