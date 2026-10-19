#include "icelib/interchange.hpp"

#include <fstream>
#include <cstdint>
#include <cstring>
#include <map>
#include <limits>
#include <nlohmann/json.hpp>

namespace ice {
using json = nlohmann::ordered_json;

std::uint16_t readU16(std::istream& input) {
    unsigned char bytes[2];
    input.read(reinterpret_cast<char*>(bytes), 2);
    if (!input) throw ProjectError("interchange.parse", "Unexpected end of GDSII file");
    return static_cast<std::uint16_t>((bytes[0] << 8) | bytes[1]);
}

std::int32_t readI32(const unsigned char* bytes) {
    return static_cast<std::int32_t>((static_cast<std::uint32_t>(bytes[0]) << 24) |
                                     (static_cast<std::uint32_t>(bytes[1]) << 16) |
                                     (static_cast<std::uint32_t>(bytes[2]) << 8) |
                                     bytes[3]);
}

void writeU16(std::ostream& output, std::uint16_t value) {
    const unsigned char bytes[] = {static_cast<unsigned char>(value >> 8),
                                   static_cast<unsigned char>(value & 0xff)};
    output.write(reinterpret_cast<const char*>(bytes), 2);
}

void writeI32(std::ostream& output, std::int32_t value) {
    const unsigned char bytes[] = {static_cast<unsigned char>(value >> 24),
                                   static_cast<unsigned char>(value >> 16),
                                   static_cast<unsigned char>(value >> 8),
                                   static_cast<unsigned char>(value)};
    output.write(reinterpret_cast<const char*>(bytes), 4);
}

void writeRecord(std::ostream& output, std::uint8_t type, std::uint8_t dataType,
                 const std::vector<unsigned char>& data) {
    writeU16(output, static_cast<std::uint16_t>(data.size() + 4));
    output.put(static_cast<char>(type));
    output.put(static_cast<char>(dataType));
    output.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

void writeEmptyRecord(std::ostream& output, std::uint8_t type) {
    writeRecord(output, type, 0x00, {});
}

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

void LayoutInterchange::exportGds(const Project& project, ObjectId cellId,
                                  const std::filesystem::path& path) {
    const auto cell = project.layoutCells.find(cellId);
    if (cell == project.layoutCells.end())
        throw ProjectError("interchange.cell", "Layout cell does not exist");
    std::ofstream output(path, std::ios::binary);
    if (!output) throw ProjectError("interchange.export", "Unable to open GDSII output");
    writeRecord(output, 0x00, 0x02, {0, 6});
    writeRecord(output, 0x01, 0x02, {0x07, 0xEA, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0});
    writeRecord(output, 0x02, 0x06, {'I','C','E',' ','L','A','Y','O','U','T'});
    writeRecord(output, 0x03, 0x05, {0x3E, 0x41, 0x89, 0x37, 0x4B, 0xC6, 0xA7, 0xF0,
                                     0x3E, 0x41, 0x89, 0x37, 0x4B, 0xC6, 0xA7, 0xF0});
    writeEmptyRecord(output, 0x05);
    writeRecord(output, 0x06, 0x06, {'T','O','P'});
    writeEmptyRecord(output, 0x0A);
    for (const auto& [id, shape] : cell->second.shapes) {
        std::vector<unsigned char> layer{static_cast<unsigned char>(shape.layer >> 8),
                                         static_cast<unsigned char>(shape.layer)};
        writeRecord(output, 0x0D, 0x02, layer);
        writeEmptyRecord(output, 0x08);
        std::vector<unsigned char> points;
        std::vector<LayoutPoint> polygon = shape.polygon;
        if (polygon.empty()) {
            const auto x = shape.rectangle.x;
            const auto y = shape.rectangle.y;
            const auto right = shape.rectangle.x + shape.rectangle.width;
            const auto top = shape.rectangle.y + shape.rectangle.height;
            polygon = {{x, y}, {right, y}, {right, top}, {x, top}, {x, y}};
        } else if (polygon.front().x != polygon.back().x || polygon.front().y != polygon.back().y) {
            polygon.push_back(polygon.front());
        }
        for (const auto& point : polygon) {
            const auto px = static_cast<std::int32_t>(point.x);
            const auto py = static_cast<std::int32_t>(point.y);
            points.insert(points.end(), {static_cast<unsigned char>(px >> 24),
                                         static_cast<unsigned char>(px >> 16),
                                         static_cast<unsigned char>(px >> 8),
                                         static_cast<unsigned char>(px),
                                         static_cast<unsigned char>(py >> 24),
                                         static_cast<unsigned char>(py >> 16),
                                         static_cast<unsigned char>(py >> 8),
                                         static_cast<unsigned char>(py)});
        }
        writeRecord(output, 0x10, 0x03, points);
        writeEmptyRecord(output, 0x11);
    }
    writeEmptyRecord(output, 0x07);
    writeEmptyRecord(output, 0x04);
    writeEmptyRecord(output, 0x00);
}

Project LayoutInterchange::importGds(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw ProjectError("interchange.import", "Unable to open GDSII input");
    Project project;
    project.identity = path.stem().string();
    project.name = path.stem().string();
    const auto cellId = project.nextId();
    LayoutCell layout{cellId, project.name};
    std::map<std::uint16_t, ObjectId> layers;
    std::uint16_t currentLayer = 0;
    std::vector<std::pair<std::int32_t, std::int32_t>> points;
    bool inBoundary = false;
    while (input.peek() != std::char_traits<char>::eof()) {
        const auto length = readU16(input);
        if (length < 4) throw ProjectError("interchange.parse", "Invalid GDSII record length");
        const auto type = static_cast<unsigned char>(input.get());
        const auto dataType = static_cast<unsigned char>(input.get());
        std::vector<unsigned char> data(length - 4);
        input.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!input) throw ProjectError("interchange.parse", "Truncated GDSII record");
        if (type == 0x0D && dataType == 0x02 && data.size() >= 2)
            currentLayer = static_cast<std::uint16_t>((data[0] << 8) | data[1]);
        else if (type == 0x08) {
            inBoundary = true;
            points.clear();
        } else if (type == 0x10 && dataType == 0x03 && inBoundary && data.size() % 8 == 0) {
            for (std::size_t i = 0; i < data.size(); i += 8)
                points.emplace_back(readI32(data.data() + i), readI32(data.data() + i + 4));
        } else if (type == 0x11 && inBoundary && points.size() >= 4) {
            auto layer = layers.find(currentLayer);
            if (layer == layers.end()) {
                const auto id = project.nextId();
                project.layers.emplace(id, Layer{id, "GDS layer " + std::to_string(currentLayer),
                                                 static_cast<int>(currentLayer)});
                layer = layers.emplace(currentLayer, id).first;
            }
            Coordinate minX = std::numeric_limits<Coordinate>::max();
            Coordinate minY = std::numeric_limits<Coordinate>::max();
            Coordinate maxX = std::numeric_limits<Coordinate>::lowest();
            Coordinate maxY = std::numeric_limits<Coordinate>::lowest();
            std::vector<LayoutPoint> polygon;
            for (const auto& [x, y] : points) {
                minX = std::min(minX, static_cast<Coordinate>(x));
                minY = std::min(minY, static_cast<Coordinate>(y));
                maxX = std::max(maxX, static_cast<Coordinate>(x));
                maxY = std::max(maxY, static_cast<Coordinate>(y));
                polygon.push_back({static_cast<Coordinate>(x), static_cast<Coordinate>(y)});
            }
            if (maxX <= minX || maxY <= minY) {
                inBoundary = false;
                continue;
            }
            const auto shapeId = project.nextId();
            layout.shapes.emplace(shapeId, Shape{shapeId, layer->second, std::nullopt,
                                                 {minX, minY, maxX - minX, maxY - minY}, std::move(polygon)});
            inBoundary = false;
        }
    }
    project.layoutCells.emplace(cellId, std::move(layout));
    return project;
}

Project LayoutInterchange::importGdsSubset(const std::filesystem::path& path,
                                           const InterchangeOptions& options) {
    std::ifstream stream(path, std::ios::binary);
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
