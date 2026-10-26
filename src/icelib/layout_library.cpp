#include "icelib/layout_library.hpp"

#include <fstream>
#include <nlohmann/json.hpp>

namespace ice {
namespace {
using json = nlohmann::json;

Coordinate coordinateAt(const json &value, const char *what) {
    if (!value.is_number_integer())
        throw ProjectError("layout.library", std::string("Layout library ") + what + " is not an integer");
    return value.get<Coordinate>();
}
} // namespace

LayoutDeviceLibrary LayoutDeviceLibrary::load(const std::filesystem::path &path) {
    try {
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
            throw ProjectError("layout.library", "Unable to open layout device library: " + path.string());
        const auto document = json::parse(stream);
        if (document.value("format", std::string{}) != "ice-layout-device-library")
            throw ProjectError("layout.library", "Layout library has an unsupported format: " + path.string());
        if (document.value("schema_version", 0) != 1)
            throw ProjectError("layout.library", "Layout library has an unsupported schema: " + path.string());

        LayoutDeviceLibrary library;
        library.name = document.value("name", std::string{});
        library.pdk = document.value("pdk", std::string{});
        for (const auto &layerJson : document.at("layers")) {
            LayoutDeviceLayer layer;
            layer.number = layerJson.at("number").get<int>();
            layer.datatype = layerJson.at("datatype").get<int>();
            layer.name = layerJson.value("name", std::string{});
            library.layers.push_back(std::move(layer));
        }
        for (const auto &cellJson : document.at("cells")) {
            LayoutDeviceCell cell;
            cell.name = cellJson.at("name").get<std::string>();
            cell.category = cellJson.value("category", std::string{});
            cell.source = cellJson.value("source", std::string{});
            const auto &bounds = cellJson.at("bounds");
            if (!bounds.is_array() || bounds.size() != 4)
                throw ProjectError("layout.library", "Layout library cell bounds are invalid: " + cell.name);
            const auto minX = coordinateAt(bounds.at(0), "minimum X");
            const auto minY = coordinateAt(bounds.at(1), "minimum Y");
            const auto maxX = coordinateAt(bounds.at(2), "maximum X");
            const auto maxY = coordinateAt(bounds.at(3), "maximum Y");
            cell.bounds = Rect{minX, minY, maxX - minX, maxY - minY};
            for (const auto &shapeJson : cellJson.at("shapes")) {
                LayoutDeviceShape shape;
                shape.layer = shapeJson.at("layer").get<int>();
                shape.datatype = shapeJson.at("datatype").get<int>();
                for (const auto &point : shapeJson.at("points")) {
                    if (!point.is_array() || point.size() != 2)
                        throw ProjectError("layout.library", "Layout library point is invalid: " + cell.name);
                    shape.polygon.push_back(
                        Point{coordinateAt(point.at(0), "point X"), coordinateAt(point.at(1), "point Y")});
                }
                if (shape.polygon.size() >= 3)
                    cell.shapes.push_back(std::move(shape));
            }
            library.cells.push_back(std::move(cell));
        }
        return library;
    } catch (const ProjectError &) {
        throw;
    } catch (const std::exception &error) {
        throw ProjectError("layout.library", "Unable to load layout device library: " + std::string(error.what()));
    }
}

} // namespace ice
