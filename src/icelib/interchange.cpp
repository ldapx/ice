#include "icelib/interchange.hpp"

#include "icelib/geometry.hpp"
#include "icelib/operations.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <vector>

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
constexpr std::uint8_t kText = 0x0C;
constexpr std::uint8_t kLayer = 0x0D;
constexpr std::uint8_t kDataType = 0x0E;
constexpr std::uint8_t kXy = 0x10;
constexpr std::uint8_t kEndEl = 0x11;
constexpr std::uint8_t kTextType = 0x16;
constexpr std::uint8_t kString = 0x19;

std::uint16_t readU16(std::istream &input) {
    unsigned char bytes[2];
    input.read(reinterpret_cast<char *>(bytes), 2);
    if (!input)
        throw ProjectError("interchange.parse", "Unexpected end of GDSII file");
    const auto high = static_cast<std::uint32_t>(bytes[0]) << 8;
    return static_cast<std::uint16_t>(high | static_cast<std::uint32_t>(bytes[1]));
}

std::int32_t readI32(const unsigned char *bytes) {
    return static_cast<std::int32_t>(
        (static_cast<std::uint32_t>(bytes[0]) << 24) | (static_cast<std::uint32_t>(bytes[1]) << 16) |
        (static_cast<std::uint32_t>(bytes[2]) << 8) | static_cast<std::uint32_t>(bytes[3]));
}

void writeU16(std::ostream &output, std::uint16_t value) {
    const auto wide = static_cast<std::uint32_t>(value);
    const unsigned char bytes[] = {static_cast<unsigned char>((wide >> 8) & 0xff),
                                   static_cast<unsigned char>(wide & 0xff)};
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

void writeStringRecord(std::ostream &output, std::uint8_t type, std::string value) {
    if (value.size() % 2 != 0)
        value.push_back('\0');
    writeRecord(output, type, 0x06, std::vector<unsigned char>(value.begin(), value.end()));
}

const std::vector<unsigned char> &timestampRecord() {
    // Six 16-bit date fields for last modification and six for last access.
    static const std::vector<unsigned char> value = {0x07, 0xEA, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0,
                                                     0x07, 0xEA, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0};
    return value;
}

std::int32_t checkedI32(Coordinate value, const char *field) {
    if (value < std::numeric_limits<std::int32_t>::min() || value > std::numeric_limits<std::int32_t>::max())
        throw ProjectError("interchange.export", std::string("Coordinate out of GDSII range: ") + field);
    return static_cast<std::int32_t>(value);
}

std::pair<int, int> layerStream(const Project &project, ObjectId layerId) {
    if (const auto layer = project.layers.find(layerId); layer != project.layers.end())
        return {layer->second.number, layer->second.datatype};
    return {0, 0};
}

void writeXyRecord(std::ostream &output, Coordinate x, Coordinate y) {
    const auto px = static_cast<std::uint32_t>(checkedI32(x, "x"));
    const auto py = static_cast<std::uint32_t>(checkedI32(y, "y"));
    writeRecord(output, kXy, 0x03,
                {static_cast<unsigned char>((px >> 24) & 0xff), static_cast<unsigned char>((px >> 16) & 0xff),
                 static_cast<unsigned char>((px >> 8) & 0xff), static_cast<unsigned char>(px & 0xff),
                 static_cast<unsigned char>((py >> 24) & 0xff), static_cast<unsigned char>((py >> 16) & 0xff),
                 static_cast<unsigned char>((py >> 8) & 0xff), static_cast<unsigned char>(py & 0xff)});
}

std::vector<LayoutPoint> pathSegmentPolygon(const LayoutPoint &start, const LayoutPoint &end, Coordinate width,
                                            Coordinate startExtension, Coordinate endExtension) {
    const double dx = static_cast<double>(end.x - start.x);
    const double dy = static_cast<double>(end.y - start.y);
    const double length = std::hypot(dx, dy);
    if (length < 1.0e-9 || width <= 0)
        return {};
    const double ux = dx / length;
    const double uy = dy / length;
    const double nx = -uy;
    const double ny = ux;
    const double half = static_cast<double>(width) * 0.5;
    const auto extendedStart =
        LayoutPoint{static_cast<Coordinate>(std::llround(start.x - ux * static_cast<double>(startExtension))),
                    static_cast<Coordinate>(std::llround(start.y - uy * static_cast<double>(startExtension)))};
    const auto extendedEnd =
        LayoutPoint{static_cast<Coordinate>(std::llround(end.x + ux * static_cast<double>(endExtension))),
                    static_cast<Coordinate>(std::llround(end.y + uy * static_cast<double>(endExtension)))};
    const auto point = [&](const LayoutPoint &base, double normalSign) {
        return LayoutPoint{static_cast<Coordinate>(std::llround(base.x + nx * half * normalSign)),
                           static_cast<Coordinate>(std::llround(base.y + ny * half * normalSign))};
    };
    return {point(extendedStart, 1.0), point(extendedEnd, 1.0), point(extendedEnd, -1.0), point(extendedStart, -1.0)};
}

void writeCell(const Project &project, const LayoutCell &cell, const std::filesystem::path &path) {
    std::ofstream output(path, std::ios::binary);
    if (!output)
        throw ProjectError("interchange.export", "Unable to open GDSII output");
    writeRecord(output, kHeader, 0x02, {0, 5});
    writeRecord(output, kBgnLib, 0x02, timestampRecord());
    writeStringRecord(output, kLibName, "ICE LAYOUT");
    writeRecord(output, kUnits, 0x05,
                {0x3E, 0x41, 0x89, 0x37, 0x4B, 0xC6, 0xA7, 0xF0, 0x39, 0x44, 0xB8, 0x2F, 0xA0, 0x9B, 0x5A, 0x54});
    writeRecord(output, kBgnStr, 0x02, timestampRecord());
    writeStringRecord(output, kStrName, "TOP");
    const auto writeBoundary = [&](int layerNumber, int datatype, std::vector<LayoutPoint> polygon) {
        if (layerNumber < 0 || layerNumber > 65535)
            throw ProjectError("interchange.export", "Layer number out of GDSII range");
        if (datatype < 0 || datatype > 65535)
            throw ProjectError("interchange.export", "Layer datatype out of GDSII range");
        const auto layerValue = static_cast<std::uint32_t>(layerNumber);
        const auto datatypeValue = static_cast<std::uint32_t>(datatype);
        std::vector<unsigned char> layer{static_cast<unsigned char>((layerValue >> 8) & 0xff),
                                         static_cast<unsigned char>(layerValue & 0xff)};
        std::vector<unsigned char> layerDatatype{static_cast<unsigned char>((datatypeValue >> 8) & 0xff),
                                                 static_cast<unsigned char>(datatypeValue & 0xff)};
        writeEmptyRecord(output, kBoundary);
        writeRecord(output, kLayer, 0x02, layer);
        writeRecord(output, kDataType, 0x02, layerDatatype);
        if (polygon.empty())
            return;
        if (polygon.front().x != polygon.back().x || polygon.front().y != polygon.back().y)
            polygon.push_back(polygon.front());
        std::vector<unsigned char> points;
        points.reserve(polygon.size() * 8);
        for (const auto &point : polygon) {
            const auto px = static_cast<std::uint32_t>(checkedI32(point.x, "x"));
            const auto py = static_cast<std::uint32_t>(checkedI32(point.y, "y"));
            points.insert(points.end(),
                          {static_cast<unsigned char>((px >> 24) & 0xff), static_cast<unsigned char>((px >> 16) & 0xff),
                           static_cast<unsigned char>((px >> 8) & 0xff), static_cast<unsigned char>(px & 0xff),
                           static_cast<unsigned char>((py >> 24) & 0xff), static_cast<unsigned char>((py >> 16) & 0xff),
                           static_cast<unsigned char>((py >> 8) & 0xff), static_cast<unsigned char>(py & 0xff)});
        }
        writeRecord(output, kXy, 0x03, points);
        writeEmptyRecord(output, kEndEl);
    };

    for (const auto &[_, shape] : cell.shapes) {
        if (shape.isText()) {
            const auto [layerNumber, datatype] = layerStream(project, shape.layer);
            if (layerNumber < 0 || layerNumber > 65535)
                throw ProjectError("interchange.export", "Layer number out of GDSII range");
            if (datatype < 0 || datatype > 65535)
                throw ProjectError("interchange.export", "Layer datatype out of GDSII range");
            const auto layerValue = static_cast<std::uint32_t>(layerNumber);
            const auto datatypeValue = static_cast<std::uint32_t>(datatype);
            writeEmptyRecord(output, kText);
            writeRecord(
                output, kLayer, 0x02,
                {static_cast<unsigned char>((layerValue >> 8) & 0xff), static_cast<unsigned char>(layerValue & 0xff)});
            writeRecord(output, kTextType, 0x02,
                        {static_cast<unsigned char>((datatypeValue >> 8) & 0xff),
                         static_cast<unsigned char>(datatypeValue & 0xff)});
            // The nominal text box uses a lower-left origin; GDS text uses the
            // top-left with the default presentation.
            writeXyRecord(output, shape.rectangle.x, shape.rectangle.y + shape.rectangle.height);
            writeStringRecord(output, kString, shape.text);
            writeEmptyRecord(output, kEndEl);
            continue;
        }
        if (shape.isPath()) {
            const auto half = shape.width / 2;
            const auto hasVia = [&](std::size_t pointIndex) {
                return pointIndex < shape.pointVias.size() && !shape.pointVias[pointIndex].empty();
            };
            for (std::size_t index = 0; index + 1 < shape.centerline.size(); ++index) {
                const auto layerId = shape.layerForSegment(index);
                const auto [layerNumber, datatype] = layerStream(project, layerId);
                const auto startExtension = hasVia(index) ? half : 0;
                const auto endExtension = hasVia(index + 1) ? half : 0;
                writeBoundary(layerNumber, datatype,
                              pathSegmentPolygon(shape.centerline[index], shape.centerline[index + 1], shape.width,
                                                 startExtension, endExtension));
            }
            for (std::size_t index = 0; index < shape.pointVias.size() && index < shape.centerline.size(); ++index) {
                const auto &point = shape.centerline[index];
                for (const auto viaLayer : shape.pointVias[index]) {
                    const auto [layerNumber, datatype] = layerStream(project, viaLayer);
                    writeBoundary(layerNumber, datatype, {{point.x - half, point.y - half},
                                                          {point.x + half, point.y - half},
                                                          {point.x + half, point.y + half},
                                                          {point.x - half, point.y + half}});
                }
            }
            continue;
        }
        const auto [layerNumber, datatype] = layerStream(project, shape.layer);
        std::vector<LayoutPoint> polygon = shape.polygon;
        if (polygon.empty()) {
            const auto x = shape.rectangle.x;
            const auto y = shape.rectangle.y;
            const auto right = shape.rectangle.x + shape.rectangle.width;
            const auto top = shape.rectangle.y + shape.rectangle.height;
            polygon = {{x, y}, {right, y}, {right, top}, {x, top}, {x, y}};
        }
        writeBoundary(layerNumber, datatype, std::move(polygon));
    }
    for (const auto &[_, port] : cell.ports) {
        if (!port.visible)
            continue;
        const auto [layerNumber, datatype] = layerStream(project, port.layer);
        if (layerNumber < 0 || layerNumber > 65535)
            throw ProjectError("interchange.export", "Layer number out of GDSII range");
        if (datatype < 0 || datatype > 65535)
            throw ProjectError("interchange.export", "Layer datatype out of GDSII range");
        const auto &rect = port.rectangle;
        writeBoundary(layerNumber, datatype, {{rect.x, rect.y},
                                              {rect.x + rect.width, rect.y},
                                              {rect.x + rect.width, rect.y + rect.height},
                                              {rect.x, rect.y + rect.height}});
        const auto layerValue = static_cast<std::uint32_t>(layerNumber);
        const auto datatypeValue = static_cast<std::uint32_t>(datatype);
        writeEmptyRecord(output, kText);
        writeRecord(
            output, kLayer, 0x02,
            {static_cast<unsigned char>((layerValue >> 8) & 0xff), static_cast<unsigned char>(layerValue & 0xff)});
        writeRecord(output, kTextType, 0x02,
                    {static_cast<unsigned char>((datatypeValue >> 8) & 0xff),
                     static_cast<unsigned char>(datatypeValue & 0xff)});
        writeXyRecord(output, rect.x + rect.width / 2, rect.y + rect.height / 2);
        writeStringRecord(output, kString, port.label.empty() ? port.name : port.label);
        writeEmptyRecord(output, kEndEl);
    }
    writeEmptyRecord(output, kEndStr);
    writeEmptyRecord(output, kEndLib);
    if (!output)
        throw ProjectError("interchange.export", "Unable to write GDSII output");
}

Point applyTransformChain(Point point, const std::vector<Transform> &chain) {
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        point = it->apply(point);
    return point;
}

void collectFlattenedShapes(const Project &project, ObjectId cellId, const std::vector<Transform> &chain,
                            std::set<ObjectId> &ancestors, Project &targetProject, LayoutCell &targetCell) {
    const auto cellIt = project.layoutCells.find(cellId);
    if (cellIt == project.layoutCells.end())
        return;
    const auto &cell = cellIt->second;
    for (const auto &[_, shape] : cell.shapes) {
        if (shape.isText()) {
            const auto origin = applyTransformChain({shape.rectangle.x, shape.rectangle.y}, chain);
            const auto shapeId = targetProject.nextId();
            Shape flattened{shapeId,
                            shape.layer,
                            shape.net,
                            {origin.x, origin.y, shape.rectangle.width, shape.rectangle.height},
                            {}};
            flattened.text = shape.text;
            targetCell.shapes.emplace(shapeId, std::move(flattened));
            continue;
        }
        std::vector<LayoutPoint> sourcePolygon = shape.polygon;
        if (sourcePolygon.empty()) {
            const auto x = shape.rectangle.x;
            const auto y = shape.rectangle.y;
            const auto right = shape.rectangle.x + shape.rectangle.width;
            const auto top = shape.rectangle.y + shape.rectangle.height;
            sourcePolygon = {{x, y}, {right, y}, {right, top}, {x, top}};
        }
        std::vector<LayoutPoint> polygon;
        polygon.reserve(sourcePolygon.size());
        Coordinate minX = std::numeric_limits<Coordinate>::max();
        Coordinate minY = std::numeric_limits<Coordinate>::max();
        Coordinate maxX = std::numeric_limits<Coordinate>::lowest();
        Coordinate maxY = std::numeric_limits<Coordinate>::lowest();
        for (const auto &sourcePoint : sourcePolygon) {
            const auto point = applyTransformChain({sourcePoint.x, sourcePoint.y}, chain);
            polygon.push_back({point.x, point.y});
            minX = std::min(minX, point.x);
            minY = std::min(minY, point.y);
            maxX = std::max(maxX, point.x);
            maxY = std::max(maxY, point.y);
        }
        if (polygon.empty() || maxX <= minX || maxY <= minY)
            continue;
        const auto shapeId = targetProject.nextId();
        Shape flattened{shapeId, shape.layer, shape.net, {minX, minY, maxX - minX, maxY - minY}, std::move(polygon)};
        if (shape.isPath()) {
            flattened.centerline.reserve(shape.centerline.size());
            for (const auto &sourcePoint : shape.centerline) {
                const auto point = applyTransformChain({sourcePoint.x, sourcePoint.y}, chain);
                flattened.centerline.push_back({point.x, point.y});
            }
            flattened.segmentLayers = shape.segmentLayers;
            flattened.pointVias = shape.pointVias;
            flattened.width = shape.width;
        }
        targetCell.shapes.emplace(shapeId, std::move(flattened));
    }
    for (const auto &[_, instance] : cell.instances) {
        if (!project.layoutCells.contains(instance.cell) || ancestors.contains(instance.cell))
            continue;
        ancestors.insert(instance.cell);
        auto childChain = chain;
        childChain.push_back(Transform{instance.x, instance.y, instance.orientation});
        collectFlattenedShapes(project, instance.cell, childChain, ancestors, targetProject, targetCell);
        ancestors.erase(instance.cell);
    }
}
} // namespace

void LayoutInterchange::exportGds(const Project &project, ObjectId cellId, const std::filesystem::path &path) {
    const auto cell = project.layoutCells.find(cellId);
    if (cell == project.layoutCells.end())
        throw ProjectError("interchange.cell", "Layout cell does not exist");
    writeCell(project, cell->second, path);
}

void LayoutInterchange::exportFlattenedGds(const Project &project, ObjectId cellId, const std::filesystem::path &path) {
    const auto cell = project.layoutCells.find(cellId);
    if (cell == project.layoutCells.end())
        throw ProjectError("interchange.cell", "Layout cell does not exist");
    Project flattened = project;
    LayoutCell flatCell;
    flatCell.id = cellId;
    flatCell.name = cell->second.name;
    flatCell.pdk = cell->second.pdk;
    std::set<ObjectId> ancestors{cellId};
    collectFlattenedShapes(project, cellId, {}, ancestors, flattened, flatCell);
    flattened.layoutCells.clear();
    flattened.layoutCells.emplace(cellId, std::move(flatCell));
    writeCell(flattened, flattened.layoutCells.at(cellId), path);
}

namespace {
Rect layoutContentBounds(const LayoutCell &cell) {
    Coordinate minX = std::numeric_limits<Coordinate>::max();
    Coordinate minY = std::numeric_limits<Coordinate>::max();
    Coordinate maxX = std::numeric_limits<Coordinate>::lowest();
    Coordinate maxY = std::numeric_limits<Coordinate>::lowest();
    const auto include = [&](const Rect &rect) {
        if (!rect.valid())
            return;
        minX = std::min(minX, rect.x);
        minY = std::min(minY, rect.y);
        maxX = std::max(maxX, rect.x + rect.width);
        maxY = std::max(maxY, rect.y + rect.height);
    };
    for (const auto &[_, shape] : cell.shapes)
        include(shape.rectangle);
    for (const auto &[_, port] : cell.ports)
        include(port.rectangle);
    if (maxX <= minX || maxY <= minY)
        return Rect{0, 0, 1, 1};
    return Rect{minX, minY, maxX - minX, maxY - minY};
}

std::string lefDirection(PortDirection direction) {
    switch (direction) {
    case PortDirection::input:
        return "INPUT";
    case PortDirection::output:
        return "OUTPUT";
    case PortDirection::inout:
        return "INOUT";
    case PortDirection::power:
    case PortDirection::ground:
        return "INOUT";
    }
    return "INOUT";
}

std::string lefUse(PortDirection direction) {
    switch (direction) {
    case PortDirection::power:
        return "POWER";
    case PortDirection::ground:
        return "GROUND";
    default:
        return "SIGNAL";
    }
}

std::string layerNameFor(const Project &project, ObjectId layerId) {
    const auto layer = project.layers.find(layerId);
    if (layer == project.layers.end())
        return "M1";
    // LEF/DEF have no datatype field. Pins placed on a purpose layer such as
    // Metal1.pin belong to the base Metal1 layer there.
    auto name = layer->second.name;
    if (const auto dot = name.find('.'); dot != std::string::npos)
        name.erase(dot);
    if (const auto datatype = name.find(" (dt "); datatype != std::string::npos)
        name.erase(datatype);
    return name;
}
} // namespace

void LayoutInterchange::exportLef(const Project &project, ObjectId cellId, const std::filesystem::path &path) {
    const auto cell = project.layoutCells.find(cellId);
    if (cell == project.layoutCells.end())
        throw ProjectError("interchange.cell", "Layout cell does not exist");
    const auto bounds = layoutContentBounds(cell->second);
    std::ofstream output(path);
    if (!output)
        throw ProjectError("interchange.export", "Unable to open LEF output");
    output << "VERSION 5.8 ;\n"
           << "UNITS\n  DATABASE MICRONS 1000 ;\nEND UNITS\n"
           << "MACRO " << cell->second.name << "\n"
           << "  CLASS BLOCK ;\n"
           << "  ORIGIN " << bounds.x / 1000.0 << " " << bounds.y / 1000.0 << " ;\n"
           << "  SIZE " << bounds.width / 1000.0 << " BY " << bounds.height / 1000.0 << " ;\n";
    for (const auto &[_, port] : cell->second.ports) {
        if (!port.visible)
            continue;
        output << "  PIN " << port.name << "\n"
               << "    DIRECTION " << lefDirection(port.direction) << " ;\n"
               << "    USE " << lefUse(port.direction) << " ;\n"
               << "    PORT\n"
               << "      LAYER " << layerNameFor(project, port.layer) << " ;\n"
               << "        RECT " << port.rectangle.x / 1000.0 << " " << port.rectangle.y / 1000.0 << " "
               << (port.rectangle.x + port.rectangle.width) / 1000.0 << " "
               << (port.rectangle.y + port.rectangle.height) / 1000.0 << " ;\n"
               << "    END\n"
               << "  END " << port.name << "\n";
    }
    output << "END " << cell->second.name << "\nEND LIBRARY\n";
}

void LayoutInterchange::exportDef(const Project &project, ObjectId cellId, const std::filesystem::path &path) {
    const auto cell = project.layoutCells.find(cellId);
    if (cell == project.layoutCells.end())
        throw ProjectError("interchange.cell", "Layout cell does not exist");
    const auto bounds = layoutContentBounds(cell->second);
    std::ofstream output(path);
    if (!output)
        throw ProjectError("interchange.export", "Unable to open DEF output");
    output << "VERSION 5.8 ;\nDIVIDERCHAR \"/\" ;\nBUSBITCHARS \"[]\" ;\n"
           << "DESIGN " << cell->second.name << " ;\n"
           << "UNITS DISTANCE MICRONS 1000 ;\n"
           << "DIEAREA ( " << bounds.x << " " << bounds.y << " ) ( " << bounds.x + bounds.width << " "
           << bounds.y + bounds.height << " ) ;\n";
    int count = 0;
    for (const auto &[_, port] : cell->second.ports)
        if (port.visible)
            ++count;
    output << "PINS " << count << " ;\n";
    for (const auto &[_, port] : cell->second.ports) {
        if (!port.visible)
            continue;
        std::string netName = port.name;
        if (port.net != 0) {
            if (const auto net = cell->second.nets.find(port.net);
                net != cell->second.nets.end() && !net->second.name.empty())
                netName = net->second.name;
        }
        output << "  - " << port.name << " + NET " << netName << "\n"
               << "    + DIRECTION " << lefDirection(port.direction) << "\n"
               << "    + USE " << lefUse(port.direction) << "\n"
               << "    + LAYER " << layerNameFor(project, port.layer) << " ( " << port.rectangle.x << " "
               << port.rectangle.y << " ) ( " << port.rectangle.x + port.rectangle.width << " "
               << port.rectangle.y + port.rectangle.height << " )\n";
    }
    output << "END PINS\nEND DESIGN\n";
}

namespace {
std::vector<std::string> tokenizeInterchange(const std::string &text) {
    std::vector<std::string> tokens;
    std::string current;
    bool comment = false;
    const auto flush = [&] {
        if (!current.empty()) {
            tokens.push_back(current);
            current.clear();
        }
    };
    for (const auto character : text) {
        if (comment) {
            if (character == '\n')
                comment = false;
            continue;
        }
        if (character == '#') {
            flush();
            comment = true;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(character))) {
            flush();
            continue;
        }
        if (character == '(' || character == ')' || character == ';') {
            flush();
            tokens.emplace_back(1, character);
            continue;
        }
        current.push_back(character);
    }
    flush();
    return tokens;
}

double parseInterchangeNumber(const std::string &token) {
    try {
        std::size_t consumed = 0;
        const auto value = std::stod(token, &consumed);
        if (consumed != token.size())
            throw std::invalid_argument("trailing characters");
        return value;
    } catch (const std::exception &) {
        throw ProjectError("interchange.parse", "Invalid interchange coordinate: " + token);
    }
}

std::string uppercase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char character) { return static_cast<char>(std::toupper(character)); });
    return text;
}

std::optional<PortDirection> portDirectionFromInterchange(const std::string &value) {
    const auto text = uppercase(value);
    if (text == "INPUT" || text == "IN")
        return PortDirection::input;
    if (text == "OUTPUT" || text == "OUT")
        return PortDirection::output;
    if (text == "INOUT" || text == "FEEDTHRU")
        return PortDirection::inout;
    return std::nullopt;
}

ObjectId interchangeLayer(Project &project, std::map<std::string, ObjectId> &layers, const std::string &name) {
    if (const auto existing = layers.find(name); existing != layers.end())
        return existing->second;
    int number = static_cast<int>(layers.size()) + 1;
    const auto digits = [&] {
        std::size_t start = name.size();
        while (start > 0 && std::isdigit(static_cast<unsigned char>(name[start - 1])))
            --start;
        return start == name.size() ? std::optional<int>{} : std::optional<int>{std::stoi(name.substr(start))};
    }();
    if (digits)
        number = *digits;
    const auto id = ice::addLayer(project, name, number);
    layers.emplace(name, id);
    return id;
}

PortSide sideForRectangle(const Rect &rectangle, const Rect &boundary) {
    if (!rectangle.valid() || !boundary.valid())
        return PortSide::left;
    const auto centerX = rectangle.x + rectangle.width / 2;
    const auto centerY = rectangle.y + rectangle.height / 2;
    const auto left = std::abs(centerX - boundary.x);
    const auto right = std::abs(boundary.x + boundary.width - centerX);
    const auto bottom = std::abs(centerY - boundary.y);
    const auto top = std::abs(boundary.y + boundary.height - centerY);
    const auto minimum = std::min({left, right, bottom, top});
    if (minimum == top)
        return PortSide::top;
    if (minimum == bottom)
        return PortSide::bottom;
    if (minimum == right)
        return PortSide::right;
    return PortSide::left;
}

std::string readInterchangeFile(const std::filesystem::path &path, const char *kind) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw ProjectError("interchange.import", std::string("Unable to open ") + kind + " input");
    return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}
} // namespace

Project LayoutInterchange::importLef(const std::filesystem::path &path) {
    const auto tokens = tokenizeInterchange(readInterchangeFile(path, "LEF"));
    double databaseMicrons = 1000.0;
    std::string macroName;
    for (std::size_t index = 0; index + 1 < tokens.size(); ++index) {
        if (tokens[index] == "DATABASE" && tokens[index + 1] == "MICRONS" && index + 2 < tokens.size())
            databaseMicrons = parseInterchangeNumber(tokens[index + 2]);
        if (tokens[index] == "MACRO" && macroName.empty())
            macroName = tokens[index + 1];
    }
    if (macroName.empty())
        macroName = path.stem().string();
    if (databaseMicrons <= 0)
        throw ProjectError("interchange.parse", "LEF database units must be positive");

    Project project;
    project.identity = macroName;
    project.name = macroName;
    const auto cellId = project.nextId();
    project.layoutCells.emplace(cellId, LayoutCell{cellId, macroName});
    auto &layout = project.layoutCells.at(cellId);
    std::map<std::string, ObjectId> layers;
    Rect macroBounds;
    Coordinate originX = 0;
    Coordinate originY = 0;

    // LEF geometry is expressed in microns; ICE database units are nanometres.
    const auto toDatabase = [](double value) { return static_cast<Coordinate>(std::llround(value * 1000.0)); };

    std::size_t macroIndex = tokens.size();
    for (std::size_t index = 0; index + 1 < tokens.size(); ++index) {
        if (tokens[index] == "MACRO" && tokens[index + 1] == macroName) {
            macroIndex = index;
            break;
        }
    }
    for (std::size_t index = macroIndex; index + 1 < tokens.size(); ++index) {
        if (tokens[index] == "ORIGIN" && index + 2 < tokens.size()) {
            originX = toDatabase(parseInterchangeNumber(tokens[index + 1]));
            originY = toDatabase(parseInterchangeNumber(tokens[index + 2]));
        } else if (tokens[index] == "SIZE" && index + 3 < tokens.size()) {
            macroBounds.width = toDatabase(parseInterchangeNumber(tokens[index + 1]));
            macroBounds.height = toDatabase(parseInterchangeNumber(tokens[index + 3]));
            macroBounds.x = originX;
            macroBounds.y = originY;
        }
        if (tokens[index] != "PIN" || index + 1 >= tokens.size())
            continue;
        const auto pinName = tokens[index + 1];
        Port port;
        port.name = pinName;
        port.label = pinName;
        bool haveRectangle = false;
        std::size_t cursor = index + 2;
        for (; cursor + 1 < tokens.size(); ++cursor) {
            if (tokens[cursor] == "END" && tokens[cursor + 1] == pinName)
                break;
            if (tokens[cursor] == "DIRECTION" && cursor + 1 < tokens.size()) {
                if (const auto direction = portDirectionFromInterchange(tokens[cursor + 1]))
                    port.direction = *direction;
            } else if (tokens[cursor] == "USE" && cursor + 1 < tokens.size()) {
                const auto use = uppercase(tokens[cursor + 1]);
                if (use == "POWER")
                    port.direction = PortDirection::power;
                else if (use == "GROUND")
                    port.direction = PortDirection::ground;
            } else if (tokens[cursor] == "LAYER" && cursor + 1 < tokens.size() && tokens[cursor + 1] != ";") {
                port.layer = interchangeLayer(project, layers, tokens[cursor + 1]);
            } else if (tokens[cursor] == "RECT" && port.layer != 0 && !haveRectangle) {
                std::vector<double> values;
                std::size_t valueIndex = cursor + 1;
                for (; valueIndex < tokens.size() && tokens[valueIndex] != ";" && values.size() < 4; ++valueIndex)
                    values.push_back(parseInterchangeNumber(tokens[valueIndex]));
                if (values.size() == 4) {
                    const auto x1 = toDatabase(values[0]);
                    const auto y1 = toDatabase(values[1]);
                    const auto x2 = toDatabase(values[2]);
                    const auto y2 = toDatabase(values[3]);
                    port.rectangle = {std::min(x1, x2), std::min(y1, y2), std::abs(x2 - x1), std::abs(y2 - y1)};
                    haveRectangle = port.rectangle.valid();
                }
            }
        }
        if (port.layer != 0 && haveRectangle) {
            const auto sideBounds = macroBounds.valid() ? macroBounds : port.rectangle;
            port.side = sideForRectangle(port.rectangle, sideBounds);
            const auto id = project.nextId();
            port.id = id;
            layout.ports.emplace(id, std::move(port));
        }
        index = cursor > index ? cursor : index;
    }
    return project;
}

Project LayoutInterchange::importDef(const std::filesystem::path &path) {
    const auto tokens = tokenizeInterchange(readInterchangeFile(path, "DEF"));
    double unitsPerMicron = 1000.0;
    std::string designName;
    Rect dieArea;
    for (std::size_t index = 0; index + 1 < tokens.size(); ++index) {
        if (tokens[index] == "DISTANCE" && tokens[index + 1] == "MICRONS" && index + 2 < tokens.size())
            unitsPerMicron = parseInterchangeNumber(tokens[index + 2]);
        else if (tokens[index] == "DESIGN" && designName.empty())
            designName = tokens[index + 1];
        else if (tokens[index] == "DIEAREA") {
            std::vector<double> values;
            for (std::size_t cursor = index + 1; cursor < tokens.size() && tokens[cursor] != ";"; ++cursor) {
                if (tokens[cursor] == "(" || tokens[cursor] == ")")
                    continue;
                values.push_back(parseInterchangeNumber(tokens[cursor]));
            }
            if (values.size() >= 4) {
                const auto x1 = values[0];
                const auto y1 = values[1];
                const auto x2 = values[2];
                const auto y2 = values[3];
                dieArea = {static_cast<Coordinate>(std::llround(std::min(x1, x2))),
                           static_cast<Coordinate>(std::llround(std::min(y1, y2))),
                           static_cast<Coordinate>(std::llround(std::abs(x2 - x1))),
                           static_cast<Coordinate>(std::llround(std::abs(y2 - y1)))};
            }
        }
    }
    if (designName.empty())
        designName = path.stem().string();
    if (unitsPerMicron <= 0)
        throw ProjectError("interchange.parse", "DEF distance units must be positive");

    Project project;
    project.identity = designName;
    project.name = designName;
    const auto cellId = project.nextId();
    project.layoutCells.emplace(cellId, LayoutCell{cellId, designName});
    auto &layout = project.layoutCells.at(cellId);
    std::map<std::string, ObjectId> layers;
    std::map<std::string, ObjectId> nets;

    const auto toDatabase = [unitsPerMicron](double value) {
        return static_cast<Coordinate>(std::llround(value * 1000.0 / unitsPerMicron));
    };

    std::size_t pinsIndex = tokens.size();
    for (std::size_t index = 0; index + 1 < tokens.size(); ++index) {
        if (tokens[index] == "PINS" && tokens[index + 1] != ";") {
            pinsIndex = index + 1;
            break;
        }
    }
    std::size_t index = pinsIndex;
    while (index < tokens.size()) {
        if (tokens[index] == "END" && index + 1 < tokens.size() && tokens[index + 1] == "PINS")
            break;
        if (tokens[index] != "-" || index + 1 >= tokens.size()) {
            ++index;
            continue;
        }
        const auto pinName = tokens[index + 1];
        index += 2;
        Port port;
        port.name = pinName;
        port.label = pinName;
        std::string netName;
        bool haveRectangle = false;
        while (index < tokens.size()) {
            if (tokens[index] == "-" ||
                (tokens[index] == "END" && index + 1 < tokens.size() && tokens[index + 1] == "PINS"))
                break;
            if (tokens[index] == "+") {
                ++index;
                continue;
            }
            if (tokens[index] == "NET" && index + 1 < tokens.size()) {
                netName = tokens[index + 1];
                if (netName == "*")
                    netName.clear();
                index += 2;
                continue;
            }
            if (tokens[index] == "DIRECTION" && index + 1 < tokens.size()) {
                if (const auto direction = portDirectionFromInterchange(tokens[index + 1]))
                    port.direction = *direction;
                index += 2;
                continue;
            }
            if (tokens[index] == "USE" && index + 1 < tokens.size()) {
                const auto use = uppercase(tokens[index + 1]);
                if (use == "POWER")
                    port.direction = PortDirection::power;
                else if (use == "GROUND")
                    port.direction = PortDirection::ground;
                index += 2;
                continue;
            }
            if (tokens[index] == "LAYER" && index + 1 < tokens.size()) {
                port.layer = interchangeLayer(project, layers, tokens[index + 1]);
                index += 2;
                if (!haveRectangle) {
                    std::vector<double> values;
                    while (index < tokens.size() && values.size() < 4) {
                        if (tokens[index] == "(" || tokens[index] == ")") {
                            ++index;
                            continue;
                        }
                        if (tokens[index] == "+" || tokens[index] == ";" || tokens[index] == "PLACED" ||
                            tokens[index] == "FIXED" || tokens[index] == "COVER")
                            break;
                        try {
                            values.push_back(parseInterchangeNumber(tokens[index]));
                        } catch (const ProjectError &) {
                            break;
                        }
                        ++index;
                    }
                    if (values.size() == 4) {
                        const auto x1 = toDatabase(values[0]);
                        const auto y1 = toDatabase(values[1]);
                        const auto x2 = toDatabase(values[2]);
                        const auto y2 = toDatabase(values[3]);
                        port.rectangle = {std::min(x1, x2), std::min(y1, y2), std::abs(x2 - x1), std::abs(y2 - y1)};
                        haveRectangle = port.rectangle.valid();
                    }
                }
                continue;
            }
            ++index;
        }
        if (port.layer != 0 && haveRectangle) {
            if (!netName.empty()) {
                if (!nets.contains(netName)) {
                    const auto netId = project.nextId();
                    layout.nets.emplace(netId, LayoutNet{netId, netName});
                    nets.emplace(netName, netId);
                }
                port.net = nets.at(netName);
            }
            const auto bounds = dieArea.valid() ? dieArea : port.rectangle;
            port.side = sideForRectangle(port.rectangle, bounds);
            const auto id = project.nextId();
            port.id = id;
            layout.ports.emplace(id, std::move(port));
        }
    }
    return project;
}

Project LayoutInterchange::importGds(const std::filesystem::path &path, const LayoutImportOptions &options) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw ProjectError("interchange.import", "Unable to open GDSII input");
    Project project;
    project.identity = path.stem().string();
    project.name = path.stem().string();
    const auto cellId = project.nextId();
    project.layoutCells.emplace(cellId, LayoutCell{cellId, project.name});
    auto &layout = project.layoutCells.at(cellId);
    std::map<std::pair<std::uint16_t, std::uint16_t>, ObjectId> layers;
    std::uint16_t currentLayer = 0;
    std::uint16_t currentDatatype = 0;
    const auto currentLayerId = [&]() {
        const auto key = std::make_pair(currentLayer, currentDatatype);
        if (const auto existing = layers.find(key); existing != layers.end())
            return existing->second;
        std::string name = "GDS layer " + std::to_string(currentLayer);
        if (currentDatatype != 0)
            name += " (dt " + std::to_string(currentDatatype) + ")";
        const auto id = project.nextId();
        project.layers.emplace(id, Layer{id, name, static_cast<int>(currentLayer), true, 0, std::string{},
                                         static_cast<int>(currentDatatype)});
        layers.emplace(key, id);
        return id;
    };
    std::vector<std::pair<std::int32_t, std::int32_t>> points;
    bool inBoundary = false;
    bool inText = false;
    bool hasTextOrigin = false;
    std::pair<std::int32_t, std::int32_t> textOrigin{};
    std::string textValue;
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
            currentLayer = static_cast<std::uint16_t>((static_cast<std::uint32_t>(data[0]) << 8) |
                                                      static_cast<std::uint32_t>(data[1]));
        else if ((type == kDataType || type == kTextType) && dataType == 0x02 && data.size() >= 2)
            currentDatatype = static_cast<std::uint16_t>((static_cast<std::uint32_t>(data[0]) << 8) |
                                                         static_cast<std::uint32_t>(data[1]));
        else if (type == kBoundary) {
            inBoundary = true;
            currentDatatype = 0;
            points.clear();
        } else if (type == kText) {
            inText = true;
            currentDatatype = 0;
            hasTextOrigin = false;
            textValue.clear();
        } else if (type == kXy && dataType == 0x03 && inText && data.size() >= 8) {
            textOrigin = {readI32(data.data()), readI32(data.data() + 4)};
            hasTextOrigin = true;
        } else if (type == kString && inText && dataType == 0x06) {
            textValue.assign(data.begin(), data.end());
            while (!textValue.empty() && textValue.back() == '\0')
                textValue.pop_back();
        } else if (type == kXy && dataType == 0x03 && inBoundary && data.size() % 8 == 0) {
            for (std::size_t i = 0; i < data.size(); i += 8)
                points.emplace_back(readI32(data.data() + i), readI32(data.data() + i + 4));
        } else if (type == kEndEl && inText) {
            if (hasTextOrigin && !textValue.empty()) {
                const auto layerId = currentLayerId();
                constexpr Coordinate height = 1000;
                const auto width =
                    std::max<Coordinate>(1, static_cast<Coordinate>(std::llround(static_cast<double>(textValue.size()) *
                                                                                 static_cast<double>(height) * 0.6)));
                const auto shapeId = project.nextId();
                Shape shape{shapeId,
                            layerId,
                            std::nullopt,
                            {textOrigin.first, textOrigin.second - height, width, height},
                            {}};
                shape.text = textValue;
                layout.shapes.emplace(shapeId, std::move(shape));
                if (options.pinLabelLayer && *options.pinLabelLayer == static_cast<int>(currentLayer)) {
                    auto portName = textValue;
                    const auto first = portName.find_first_not_of(" \t\r\n");
                    const auto last = portName.find_last_not_of(" \t\r\n");
                    portName = first == std::string::npos ? std::string{} : portName.substr(first, last - first + 1);
                    if (!portName.empty()) {
                        const auto key = [](std::string value) {
                            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
                                return static_cast<char>(std::tolower(character));
                            });
                            return value;
                        };
                        std::set<std::string> used;
                        for (const auto &[_, port] : layout.ports)
                            used.insert(key(port.name));
                        const auto baseName = portName;
                        for (int suffix = 1; used.contains(key(portName)); ++suffix)
                            portName = baseName + "_" + std::to_string(suffix);
                        const auto pinWidth = std::max<Coordinate>(1, options.pinWidth);
                        const auto pinHeight = std::max<Coordinate>(1, options.pinHeight);
                        const auto portId = project.nextId();
                        Port port{
                            portId,
                            portName,
                            layerId,
                            {textOrigin.first - pinWidth / 2, textOrigin.second - pinHeight / 2, pinWidth, pinHeight}};
                        port.label = textValue;
                        layout.ports.emplace(portId, std::move(port));
                    }
                }
            }
            inText = false;
            hasTextOrigin = false;
            textValue.clear();
        } else if (type == kEndEl && inBoundary && points.size() >= 4) {
            const auto layerId = currentLayerId();
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
                    shapeId, layerId, std::nullopt, {minX, minY, maxX - minX, maxY - minY}, std::move(polygon)});
            inBoundary = false;
        }
    }
    return project;
}
} // namespace ice
