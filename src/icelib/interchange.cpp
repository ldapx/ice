#include "icelib/interchange.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>

namespace ice {

namespace {
// GDSII record types used by the minimal subset reader/writer.
constexpr std::uint8_t kHeader = 0x00;
constexpr std::uint8_t kBgnLib = 0x01;
constexpr std::uint8_t kLibName = 0x02;
constexpr std::uint8_t kUnits = 0x03;
constexpr std::uint8_t kEndLib = 0x04;
constexpr std::uint8_t kBgnStr = 0x05;
constexpr std::uint8_t kStrName = 0x06;
constexpr std::uint8_t kEndStr = 0x07;
constexpr std::uint8_t kBoundary = 0x08;
constexpr std::uint8_t kLayer = 0x0D;
constexpr std::uint8_t kXy = 0x10;
constexpr std::uint8_t kEndEl = 0x11;

std::uint16_t readU16(std::istream &input) {
    unsigned char bytes[2];
    input.read(reinterpret_cast<char *>(bytes), 2);
    if (!input)
        throw ProjectError("interchange.parse", "Unexpected end of GDSII file");
    return static_cast<std::uint16_t>((bytes[0] << 8) | bytes[1]);
}

std::int32_t readI32(const unsigned char *bytes) {
    return static_cast<std::int32_t>((static_cast<std::uint32_t>(bytes[0]) << 24) |
                                     (static_cast<std::uint32_t>(bytes[1]) << 16) |
                                     (static_cast<std::uint32_t>(bytes[2]) << 8) | bytes[3]);
}

void writeU16(std::ostream &output, std::uint16_t value) {
    const unsigned char bytes[] = {static_cast<unsigned char>(value >> 8), static_cast<unsigned char>(value & 0xff)};
    output.write(reinterpret_cast<const char *>(bytes), 2);
}

void writeRecord(std::ostream &output, std::uint8_t type, std::uint8_t dataType,
                 const std::vector<unsigned char> &data) {
    if (data.size() > 65532)
        throw ProjectError("interchange.export", "GDSII record is too large");
    writeU16(output, static_cast<std::uint16_t>(data.size() + 4));
    output.put(static_cast<char>(type));
    output.put(static_cast<char>(dataType));
    if (!data.empty())
        output.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
}

void writeEmptyRecord(std::ostream &output, std::uint8_t type) { writeRecord(output, type, 0x00, {}); }

std::int32_t checkedI32(Coordinate value, const char *field) {
    if (value < std::numeric_limits<std::int32_t>::min() || value > std::numeric_limits<std::int32_t>::max())
        throw ProjectError("interchange.export", std::string("Coordinate out of GDSII range: ") + field);
    return static_cast<std::int32_t>(value);
}
} // namespace

void LayoutInterchange::exportGds(const Project &project, ObjectId cellId, const std::filesystem::path &path) {
    const auto cell = project.layoutCells.find(cellId);
    if (cell == project.layoutCells.end())
        throw ProjectError("interchange.cell", "Layout cell does not exist");
    std::ofstream output(path, std::ios::binary);
    if (!output)
        throw ProjectError("interchange.export", "Unable to open GDSII output");
    writeRecord(output, kHeader, 0x02, {0, 6});
    writeRecord(output, kBgnLib, 0x02, {0x07, 0xEA, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0});
    writeRecord(output, kLibName, 0x06, {'I', 'C', 'E', ' ', 'L', 'A', 'Y', 'O', 'U', 'T'});
    writeRecord(output, kUnits, 0x05,
                {0x3E, 0x41, 0x89, 0x37, 0x4B, 0xC6, 0xA7, 0xF0, 0x3E, 0x41, 0x89, 0x37, 0x4B, 0xC6, 0xA7, 0xF0});
    writeEmptyRecord(output, kBgnStr);
    writeRecord(output, kStrName, 0x06, {'T', 'O', 'P'});
    writeEmptyRecord(output, 0x0A);
    for (const auto &[id, shape] : cell->second.shapes) {
        int layerNumber = 0;
        if (const auto layerIt = project.layers.find(shape.layer); layerIt != project.layers.end())
            layerNumber = layerIt->second.number;
        if (layerNumber < 0 || layerNumber > 65535)
            throw ProjectError("interchange.export", "Layer number out of GDSII range");
        const auto layerValue = static_cast<std::uint16_t>(layerNumber);
        std::vector<unsigned char> layer{static_cast<unsigned char>(layerValue >> 8),
                                         static_cast<unsigned char>(layerValue & 0xff)};
        writeRecord(output, kLayer, 0x02, layer);
        writeEmptyRecord(output, kBoundary);
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
        std::vector<unsigned char> points;
        points.reserve(polygon.size() * 8);
        for (const auto &point : polygon) {
            const auto px = checkedI32(point.x, "x");
            const auto py = checkedI32(point.y, "y");
            points.insert(points.end(), {static_cast<unsigned char>(px >> 24), static_cast<unsigned char>(px >> 16),
                                         static_cast<unsigned char>(px >> 8), static_cast<unsigned char>(px),
                                         static_cast<unsigned char>(py >> 24), static_cast<unsigned char>(py >> 16),
                                         static_cast<unsigned char>(py >> 8), static_cast<unsigned char>(py)});
        }
        writeRecord(output, kXy, 0x03, points);
        writeEmptyRecord(output, kEndEl);
        (void)id;
    }
    writeEmptyRecord(output, kEndStr);
    writeEmptyRecord(output, kEndLib);
    writeEmptyRecord(output, kHeader);
    if (!output)
        throw ProjectError("interchange.export", "Unable to write GDSII output");
}

Project LayoutInterchange::importGds(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw ProjectError("interchange.import", "Unable to open GDSII input");
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
        if (length < 4)
            throw ProjectError("interchange.parse", "Invalid GDSII record length");
        const auto type = static_cast<unsigned char>(input.get());
        const auto dataType = static_cast<unsigned char>(input.get());
        std::vector<unsigned char> data(length - 4);
        input.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!input)
            throw ProjectError("interchange.parse", "Truncated GDSII record");
        if (type == kLayer && dataType == 0x02 && data.size() >= 2)
            currentLayer = static_cast<std::uint16_t>((data[0] << 8) | data[1]);
        else if (type == kBoundary) {
            inBoundary = true;
            points.clear();
        } else if (type == kXy && dataType == 0x03 && inBoundary && data.size() % 8 == 0) {
            for (std::size_t i = 0; i < data.size(); i += 8)
                points.emplace_back(readI32(data.data() + i), readI32(data.data() + i + 4));
        } else if (type == kEndEl && inBoundary && points.size() >= 4) {
            auto layer = layers.find(currentLayer);
            if (layer == layers.end()) {
                const auto id = project.nextId();
                project.layers.emplace(
                    id, Layer{id, "GDS layer " + std::to_string(currentLayer), static_cast<int>(currentLayer)});
                layer = layers.emplace(currentLayer, id).first;
            }
            Coordinate minX = std::numeric_limits<Coordinate>::max();
            Coordinate minY = std::numeric_limits<Coordinate>::max();
            Coordinate maxX = std::numeric_limits<Coordinate>::lowest();
            Coordinate maxY = std::numeric_limits<Coordinate>::lowest();
            std::vector<LayoutPoint> polygon;
            for (const auto &[x, y] : points) {
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
            layout.shapes.emplace(
                shapeId,
                Shape{
                    shapeId, layer->second, std::nullopt, {minX, minY, maxX - minX, maxY - minY}, std::move(polygon)});
            inBoundary = false;
        }
    }
    project.layoutCells.emplace(cellId, std::move(layout));
    return project;
}
} // namespace ice
