#include "icelib/interchange.hpp"

#include "icelib/geometry.hpp"
#include "icelib/operations.hpp"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
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
constexpr std::uint8_t kPath = 0x09;
constexpr std::uint8_t kSref = 0x0A;
constexpr std::uint8_t kAref = 0x0B;
constexpr std::uint8_t kText = 0x0C;
constexpr std::uint8_t kLayer = 0x0D;
constexpr std::uint8_t kDataType = 0x0E;
constexpr std::uint8_t kWidth = 0x0F;
constexpr std::uint8_t kXy = 0x10;
constexpr std::uint8_t kEndEl = 0x11;
constexpr std::uint8_t kSname = 0x12;
constexpr std::uint8_t kColrow = 0x13;
constexpr std::uint8_t kNode = 0x15;
constexpr std::uint8_t kTextType = 0x16;
constexpr std::uint8_t kPresentation = 0x17;
constexpr std::uint8_t kString = 0x19;
constexpr std::uint8_t kStrans = 0x1A;
constexpr std::uint8_t kMag = 0x1B;
constexpr std::uint8_t kAngle = 0x1C;
constexpr std::uint8_t kPathType = 0x21;
constexpr std::uint8_t kBox = 0x2D;
constexpr std::uint8_t kBoxType = 0x2E;
constexpr std::uint8_t kNodeType = 0x2F;
constexpr std::uint8_t kBgnExtn = 0x30;
constexpr std::uint8_t kEndExtn = 0x31;

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

std::uint16_t readI16(const unsigned char *bytes) {
    return static_cast<std::uint16_t>((static_cast<std::uint32_t>(bytes[0]) << 8) |
                                      static_cast<std::uint32_t>(bytes[1]));
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

double readReal8(const unsigned char *bytes) {
    const bool negative = (bytes[0] & 0x80) != 0;
    const auto exponent = static_cast<int>(bytes[0] & 0x7f) - 64;
    std::uint64_t mantissa = 0;
    for (int index = 1; index < 8; ++index)
        mantissa = (mantissa << 8) | static_cast<std::uint64_t>(bytes[index]);
    if (mantissa == 0)
        return 0.0;
    long double value = std::ldexp(static_cast<long double>(mantissa), -56);
    value *= std::pow(16.0L, exponent);
    return negative ? -static_cast<double>(value) : static_cast<double>(value);
}

std::array<unsigned char, 8> real8Bytes(double value) {
    std::array<unsigned char, 8> bytes{};
    if (!std::isfinite(value) || value == 0.0)
        return bytes;
    const bool negative = value < 0.0;
    auto magnitude = std::abs(value);
    auto exponent = 0;
    while (magnitude >= 1.0) {
        magnitude /= 16.0;
        ++exponent;
    }
    while (magnitude < 1.0 / 16.0) {
        magnitude *= 16.0;
        --exponent;
    }
    const auto scaled = static_cast<long double>(magnitude) * static_cast<long double>(1ULL << 56);
    auto mantissa = static_cast<std::uint64_t>(std::llround(scaled));
    if (mantissa >= (1ULL << 56)) {
        mantissa >>= 4;
        ++exponent;
    }
    const auto biased = std::clamp(exponent + 64, 0, 127);
    bytes[0] = static_cast<unsigned char>((negative ? 0x80 : 0) | biased);
    for (int index = 0; index < 7; ++index)
        bytes[1 + index] = static_cast<unsigned char>((mantissa >> (48 - 8 * index)) & 0xff);
    return bytes;
}

void writeReal8Record(std::ostream &output, std::uint8_t type, double value) {
    const auto bytes = real8Bytes(value);
    writeRecord(output, type, 0x05, std::vector<unsigned char>(bytes.begin(), bytes.end()));
}

void writeI16Record(std::ostream &output, std::uint8_t type, int value) {
    const auto wide = static_cast<std::uint32_t>(value & 0xffff);
    writeRecord(output, type, 0x02,
                {static_cast<unsigned char>((wide >> 8) & 0xff), static_cast<unsigned char>(wide & 0xff)});
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

double projectNanometersPerUnit(const Project &project) {
    if (project.units.denominator > 0 && project.units.nanometers_per_unit > 0)
        return static_cast<double>(project.units.nanometers_per_unit) / static_cast<double>(project.units.denominator);
    return 1.0;
}

void writeUnitsRecord(std::ostream &output, double nanometersPerUnit) {
    if (!(nanometersPerUnit > 0.0))
        nanometersPerUnit = 1.0;
    const auto dbUnitMeters = nanometersPerUnit * 1.0e-9;
    const auto userUnitsPerDatabaseUnit = dbUnitMeters / 1.0e-6;
    const auto first = real8Bytes(userUnitsPerDatabaseUnit);
    const auto second = real8Bytes(dbUnitMeters);
    std::vector<unsigned char> data;
    data.reserve(16);
    data.insert(data.end(), first.begin(), first.end());
    data.insert(data.end(), second.begin(), second.end());
    writeRecord(output, kUnits, 0x05, data);
}

void writeLibraryHeader(std::ostream &output, double nanometersPerUnit) {
    writeRecord(output, kHeader, 0x02, {0, 5});
    writeRecord(output, kBgnLib, 0x02, timestampRecord());
    writeStringRecord(output, kLibName, "ICE LAYOUT");
    writeUnitsRecord(output, nanometersPerUnit);
}

void writeBoundary(std::ostream &output, int layerNumber, int datatype, std::vector<LayoutPoint> polygon) {
    if (layerNumber < 0 || layerNumber > 65535)
        throw ProjectError("interchange.export", "Layer number out of GDSII range");
    if (datatype < 0 || datatype > 65535)
        throw ProjectError("interchange.export", "Layer datatype out of GDSII range");
    writeEmptyRecord(output, kBoundary);
    writeI16Record(output, kLayer, layerNumber);
    writeI16Record(output, kDataType, datatype);
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
}

void writeTextElement(std::ostream &output, int layerNumber, int datatype, Coordinate x, Coordinate y,
                      const std::string &text) {
    if (layerNumber < 0 || layerNumber > 65535)
        throw ProjectError("interchange.export", "Layer number out of GDSII range");
    if (datatype < 0 || datatype > 65535)
        throw ProjectError("interchange.export", "Layer datatype out of GDSII range");
    writeEmptyRecord(output, kText);
    writeI16Record(output, kLayer, layerNumber);
    writeI16Record(output, kTextType, datatype);
    writeXyRecord(output, x, y);
    writeStringRecord(output, kString, text);
    writeEmptyRecord(output, kEndEl);
}

void writePathElement(std::ostream &output, int layerNumber, int datatype, const std::vector<LayoutPoint> &centerline,
                      Coordinate width) {
    if (layerNumber < 0 || layerNumber > 65535)
        throw ProjectError("interchange.export", "Layer number out of GDSII range");
    if (datatype < 0 || datatype > 65535)
        throw ProjectError("interchange.export", "Layer datatype out of GDSII range");
    if (centerline.size() < 2 || width <= 0)
        return;
    writeEmptyRecord(output, kPath);
    writeI16Record(output, kLayer, layerNumber);
    writeI16Record(output, kDataType, datatype);
    const auto wide = static_cast<std::uint32_t>(checkedI32(width, "path width"));
    writeRecord(output, kWidth, 0x03,
                {static_cast<unsigned char>((wide >> 24) & 0xff), static_cast<unsigned char>((wide >> 16) & 0xff),
                 static_cast<unsigned char>((wide >> 8) & 0xff), static_cast<unsigned char>(wide & 0xff)});
    std::vector<unsigned char> points;
    points.reserve(centerline.size() * 8);
    for (const auto &point : centerline) {
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
}

void writeShape(std::ostream &output, const Project &project, const Shape &shape) {
    if (shape.isText()) {
        const auto [layerNumber, datatype] = layerStream(project, shape.layer);
        // The nominal text box uses a lower-left origin; GDS text uses the
        // top-left with the default presentation.
        writeTextElement(output, layerNumber, datatype, shape.rectangle.x, shape.rectangle.y + shape.rectangle.height,
                         shape.text);
        return;
    }
    if (shape.isPath()) {
        const auto simplePath = shape.segmentLayers.empty() && shape.segmentWidths.empty() &&
                                std::all_of(shape.pointVias.begin(), shape.pointVias.end(),
                                            [](const std::vector<ObjectId> &vias) { return vias.empty(); });
        if (simplePath && shape.width > 0) {
            const auto [layerNumber, datatype] = layerStream(project, shape.layer);
            writePathElement(output, layerNumber, datatype, shape.centerline, shape.width);
            return;
        }
        const auto viaExtent = [&](std::size_t pointIndex) {
            Coordinate extent = 0;
            if (pointIndex > 0)
                extent = std::max(extent, shape.widthForSegment(pointIndex - 1));
            if (pointIndex + 1 < shape.centerline.size())
                extent = std::max(extent, shape.widthForSegment(pointIndex));
            return extent > 0 ? extent : shape.width;
        };
        const auto hasVia = [&](std::size_t pointIndex) {
            return pointIndex < shape.pointVias.size() && !shape.pointVias[pointIndex].empty();
        };
        for (std::size_t index = 0; index + 1 < shape.centerline.size(); ++index) {
            const auto layerId = shape.layerForSegment(index);
            const auto [layerNumber, datatype] = layerStream(project, layerId);
            const auto width = shape.widthForSegment(index);
            const auto startExtension = hasVia(index) ? viaExtent(index) / 2 : 0;
            const auto endExtension = hasVia(index + 1) ? viaExtent(index + 1) / 2 : 0;
            writeBoundary(output, layerNumber, datatype,
                          pathSegmentPolygon(shape.centerline[index], shape.centerline[index + 1], width,
                                             startExtension, endExtension));
        }
        for (std::size_t index = 0; index < shape.pointVias.size() && index < shape.centerline.size(); ++index) {
            const auto &point = shape.centerline[index];
            const auto half = viaExtent(index) / 2;
            for (const auto viaLayer : shape.pointVias[index]) {
                const auto [layerNumber, datatype] = layerStream(project, viaLayer);
                writeBoundary(output, layerNumber, datatype,
                              {{point.x - half, point.y - half},
                               {point.x + half, point.y - half},
                               {point.x + half, point.y + half},
                               {point.x - half, point.y + half}});
            }
        }
        return;
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
    writeBoundary(output, layerNumber, datatype, std::move(polygon));
}

void writePort(std::ostream &output, const Project &project, const Port &port) {
    if (!port.visible)
        return;
    const auto [layerNumber, datatype] = layerStream(project, port.layer);
    const auto &rect = port.rectangle;
    writeBoundary(output, layerNumber, datatype,
                  {{rect.x, rect.y},
                   {rect.x + rect.width, rect.y},
                   {rect.x + rect.width, rect.y + rect.height},
                   {rect.x, rect.y + rect.height}});
    writeTextElement(output, layerNumber, datatype, rect.x + rect.width / 2, rect.y + rect.height / 2,
                     port.label.empty() ? port.name : port.label);
}

void writeSref(std::ostream &output, const std::string &target, Coordinate x, Coordinate y, Orientation orientation) {
    bool reflect = false;
    double angle = 0.0;
    switch (orientation) {
    case Orientation::r0:
        break;
    case Orientation::r90:
        angle = 90.0;
        break;
    case Orientation::r180:
        angle = 180.0;
        break;
    case Orientation::r270:
        angle = 270.0;
        break;
    case Orientation::mx:
        reflect = true;
        break;
    case Orientation::my:
        reflect = true;
        angle = 180.0;
        break;
    case Orientation::m45:
        reflect = true;
        angle = 90.0;
        break;
    case Orientation::m135:
        reflect = true;
        angle = 270.0;
        break;
    }
    writeEmptyRecord(output, kSref);
    writeStringRecord(output, kSname, target);
    if (reflect || angle != 0.0) {
        const auto bits = static_cast<std::uint32_t>(reflect ? 0x8000 : 0x0000);
        writeRecord(output, kStrans, 0x01,
                    {static_cast<unsigned char>((bits >> 8) & 0xff), static_cast<unsigned char>(bits & 0xff)});
        if (angle != 0.0)
            writeReal8Record(output, kAngle, angle);
    }
    writeXyRecord(output, x, y);
    writeEmptyRecord(output, kEndEl);
}

void writeStructure(std::ostream &output, const Project &project, const LayoutCell &cell,
                    const std::string &structureName, const std::map<ObjectId, std::string> *structureNames) {
    writeRecord(output, kBgnStr, 0x02, timestampRecord());
    writeStringRecord(output, kStrName, structureName);
    for (const auto &[_, shape] : cell.shapes)
        writeShape(output, project, shape);
    for (const auto &[_, port] : cell.ports)
        writePort(output, project, port);
    if (structureNames) {
        for (const auto &[_, instance] : cell.instances) {
            const auto target = structureNames->find(instance.cell);
            if (target == structureNames->end())
                continue;
            writeSref(output, target->second, instance.x, instance.y, instance.orientation);
        }
    }
    writeEmptyRecord(output, kEndStr);
}

std::string sanitizeGdsName(std::string value) {
    for (auto &character : value) {
        if (!std::isalnum(static_cast<unsigned char>(character)) && character != '_' && character != '$')
            character = '_';
    }
    if (value.empty())
        value = "CELL";
    if (!value.empty() && std::isdigit(static_cast<unsigned char>(value.front())))
        value = "_" + value;
    if (value.size() > 32)
        value.resize(32);
    return value;
}

std::string uniqueGdsName(std::string value, std::set<std::string> &used) {
    auto base = sanitizeGdsName(std::move(value));
    auto candidate = base;
    for (int suffix = 2; !used.insert(candidate).second; ++suffix) {
        const auto suffixText = "_" + std::to_string(suffix);
        const auto stemLength = suffixText.size() < 32 ? 32 - suffixText.size() : 1;
        candidate = base.substr(0, std::min(base.size(), stemLength)) + suffixText;
    }
    return candidate;
}

void assignStructureNames(const Project &project, ObjectId cellId, std::map<ObjectId, std::string> &names,
                          std::set<std::string> &used, std::set<ObjectId> &visited) {
    if (!visited.insert(cellId).second)
        return;
    const auto cell = project.layoutCells.find(cellId);
    if (cell == project.layoutCells.end())
        return;
    names[cellId] = uniqueGdsName(cell->second.name, used);
    for (const auto &[_, instance] : cell->second.instances)
        assignStructureNames(project, instance.cell, names, used, visited);
}

void collectStructureOrder(const Project &project, ObjectId cellId, std::vector<ObjectId> &order,
                           std::set<ObjectId> &visited) {
    if (!visited.insert(cellId).second)
        return;
    const auto cell = project.layoutCells.find(cellId);
    if (cell == project.layoutCells.end())
        return;
    order.push_back(cellId);
    for (const auto &[_, instance] : cell->second.instances)
        collectStructureOrder(project, instance.cell, order, visited);
}

void writeCell(const Project &project, const LayoutCell &cell, const std::filesystem::path &path) {
    std::ofstream output(path, std::ios::binary);
    if (!output)
        throw ProjectError("interchange.export", "Unable to open GDSII output");
    writeLibraryHeader(output, projectNanometersPerUnit(project));
    writeStructure(output, project, cell, "TOP", nullptr);
    writeEmptyRecord(output, kEndLib);
    if (!output)
        throw ProjectError("interchange.export", "Unable to write GDSII output");
}

void writeHierarchicalCell(const Project &project, ObjectId cellId, const std::filesystem::path &path) {
    const auto top = project.layoutCells.find(cellId);
    if (top == project.layoutCells.end())
        throw ProjectError("interchange.cell", "Layout cell does not exist");
    std::map<ObjectId, std::string> names;
    std::set<std::string> used;
    std::set<ObjectId> assigned;
    assignStructureNames(project, cellId, names, used, assigned);
    std::vector<ObjectId> order;
    std::set<ObjectId> visited;
    collectStructureOrder(project, cellId, order, visited);
    std::ofstream output(path, std::ios::binary);
    if (!output)
        throw ProjectError("interchange.export", "Unable to open GDSII output");
    writeLibraryHeader(output, projectNanometersPerUnit(project));
    for (const auto id : order) {
        const auto cell = project.layoutCells.find(id);
        const auto name = names.find(id);
        if (cell == project.layoutCells.end() || name == names.end())
            continue;
        writeStructure(output, project, cell->second, name->second, &names);
    }
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
                            std::set<ObjectId> &ancestors, Project &targetProject, LayoutCell &targetCell,
                            ObjectId &nextId) {
    const auto cellIt = project.layoutCells.find(cellId);
    if (cellIt == project.layoutCells.end())
        return;
    const auto &cell = cellIt->second;
    for (const auto &[_, shape] : cell.shapes) {
        if (shape.isText()) {
            const auto origin = applyTransformChain({shape.rectangle.x, shape.rectangle.y}, chain);
            const auto shapeId = nextId++;
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
        const auto shapeId = nextId++;
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
            flattened.segmentWidths = shape.segmentWidths;
        }
        targetCell.shapes.emplace(shapeId, std::move(flattened));
    }
    for (const auto &[_, instance] : cell.instances) {
        if (!project.layoutCells.contains(instance.cell) || ancestors.contains(instance.cell))
            continue;
        ancestors.insert(instance.cell);
        auto childChain = chain;
        childChain.push_back(Transform{instance.x, instance.y, instance.orientation});
        collectFlattenedShapes(project, instance.cell, childChain, ancestors, targetProject, targetCell, nextId);
        ancestors.erase(instance.cell);
    }
}
} // namespace

void LayoutInterchange::exportGds(const Project &project, ObjectId cellId, const std::filesystem::path &path) {
    writeHierarchicalCell(project, cellId, path);
}

Project LayoutInterchange::flattenProject(const Project &project, ObjectId cellId) {
    const auto cell = project.layoutCells.find(cellId);
    if (cell == project.layoutCells.end())
        throw ProjectError("interchange.cell", "Layout cell does not exist");
    Project flattened = project;
    LayoutCell flatCell;
    flatCell.id = cellId;
    flatCell.name = cell->second.name;
    flatCell.pdk = cell->second.pdk;
    flatCell.stack = cell->second.stack;
    // Flattening removes the instance hierarchy but must preserve the top
    // cell's logical interface so pin labels survive the export.
    flatCell.ports = cell->second.ports;
    flatCell.nets = cell->second.nets;
    std::set<ObjectId> ancestors{cellId};
    auto nextId = flattened.nextId();
    collectFlattenedShapes(project, cellId, {}, ancestors, flattened, flatCell, nextId);
    flattened.layoutCells.clear();
    flattened.layoutCells.emplace(cellId, std::move(flatCell));
    return flattened;
}

void LayoutInterchange::exportFlattenedGds(const Project &project, ObjectId cellId, const std::filesystem::path &path) {
    const auto flattened = flattenProject(project, cellId);
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
std::string interchangeIdentifier(std::string name) {
    for (auto &character : name) {
        if (std::isalnum(static_cast<unsigned char>(character)) == 0 && character != '_')
            character = '_';
    }
    if (name.empty())
        name = "cell";
    if (std::isdigit(static_cast<unsigned char>(name.front())) != 0)
        name = "_" + name;
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
    const auto macroName = interchangeIdentifier(cell->second.name);
    output << "VERSION 5.8 ;\n"
           << "UNITS\n  DATABASE MICRONS 1000 ;\nEND UNITS\n"
           << "MACRO " << macroName << "\n"
           << "  CLASS BLOCK ;\n"
           << "  ORIGIN " << bounds.x / 1000.0 << " " << bounds.y / 1000.0 << " ;\n"
           << "  SIZE " << bounds.width / 1000.0 << " BY " << bounds.height / 1000.0 << " ;\n";
    for (const auto &[_, port] : cell->second.ports) {
        if (!port.visible || port.internal)
            continue;
        output << "  PIN " << interchangeIdentifier(port.name) << "\n"
               << "    DIRECTION " << lefDirection(port.direction) << " ;\n"
               << "    USE " << lefUse(port.direction) << " ;\n"
               << "    PORT\n"
               << "      LAYER " << layerNameFor(project, port.layer) << " ;\n"
               << "        RECT " << port.rectangle.x / 1000.0 << " " << port.rectangle.y / 1000.0 << " "
               << (port.rectangle.x + port.rectangle.width) / 1000.0 << " "
               << (port.rectangle.y + port.rectangle.height) / 1000.0 << " ;\n"
               << "    END\n"
               << "  END " << interchangeIdentifier(port.name) << "\n";
    }
    output << "END " << macroName << "\nEND LIBRARY\n";
}

void LayoutInterchange::exportDef(const Project &project, ObjectId cellId, const std::filesystem::path &path) {
    const auto cell = project.layoutCells.find(cellId);
    if (cell == project.layoutCells.end())
        throw ProjectError("interchange.cell", "Layout cell does not exist");
    const auto bounds = layoutContentBounds(cell->second);
    std::ofstream output(path);
    if (!output)
        throw ProjectError("interchange.export", "Unable to open DEF output");
    const auto designName = interchangeIdentifier(cell->second.name);
    output << "VERSION 5.8 ;\nDIVIDERCHAR \"/\" ;\nBUSBITCHARS \"[]\" ;\n"
           << "DESIGN " << designName << " ;\n"
           << "UNITS DISTANCE MICRONS 1000 ;\n"
           << "DIEAREA ( " << bounds.x << " " << bounds.y << " ) ( " << bounds.x + bounds.width << " "
           << bounds.y + bounds.height << " ) ;\n";
    int count = 0;
    for (const auto &[_, port] : cell->second.ports)
        if (port.visible && !port.internal)
            ++count;
    output << "PINS " << count << " ;\n";
    for (const auto &[_, port] : cell->second.ports) {
        if (!port.visible || port.internal)
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
    // Use from_chars so exchange files remain locale-independent. Qt switches
    // the numeric locale at startup (for example es_CL uses a comma decimal
    // separator), which makes std::stod reject "0.230".
    double value = 0.0;
    const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size())
        throw ProjectError("interchange.parse", "Invalid interchange coordinate: " + token);
    return value;
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

std::string lowercaseInterchange(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return text;
}

struct ParsedLefPin {
    std::string name;
    std::string layer;
    PortDirection direction = PortDirection::inout;
    Rect rectangle;
    bool hasRectangle = false;
};

struct ParsedLefMacro {
    std::string name;
    Rect bounds;
    std::vector<ParsedLefPin> pins;
};

struct ParsedLef {
    std::map<std::string, ParsedLefMacro> macros;
    // Lower-case routing layer name -> width in database units.
    std::map<std::string, Coordinate> layerWidths;
    // Lower-case via name -> lower-case cut layer name.
    std::map<std::string, std::string> viaLayers;
};

void mergeParsedLef(ParsedLef &target, ParsedLef source) {
    for (auto &[name, macro] : source.macros)
        target.macros.emplace(name, std::move(macro));
    for (auto &[name, width] : source.layerWidths)
        target.layerWidths.emplace(name, width);
    for (auto &[name, layer] : source.viaLayers)
        target.viaLayers.emplace(name, layer);
}

bool readLefPoint(const std::vector<std::string> &tokens, std::size_t &index, double &x, double &y) {
    if (index + 3 >= tokens.size() || tokens[index] != "(" || tokens[index + 3] != ")")
        return false;
    x = parseInterchangeNumber(tokens[index + 1]);
    y = parseInterchangeNumber(tokens[index + 2]);
    index += 4;
    return true;
}

// DEF routing may omit one coordinate with `( x * )` to reuse the previous
// point's Y (or X). Keep the running coordinate while parsing a path.
bool readDefPoint(const std::vector<std::string> &tokens, std::size_t &index, double &lastX, double &lastY, double &x,
                  double &y) {
    if (index + 3 >= tokens.size() || tokens[index] != "(" || tokens[index + 3] != ")")
        return false;
    if (tokens[index + 1] == "*")
        x = lastX;
    else
        x = parseInterchangeNumber(tokens[index + 1]);
    if (tokens[index + 2] == "*")
        y = lastY;
    else
        y = parseInterchangeNumber(tokens[index + 2]);
    lastX = x;
    lastY = y;
    index += 4;
    return true;
}

void parseLefPin(const std::vector<std::string> &tokens, std::size_t &index, ParsedLefMacro &macro, double toDatabase) {
    const auto pinName = tokens[index + 1];
    ParsedLefPin pin;
    pin.name = pinName;
    std::string currentLayer;
    std::size_t cursor = index + 2;
    while (cursor + 1 < tokens.size()) {
        if (tokens[cursor] == "END" && tokens[cursor + 1] == pinName) {
            cursor += 2;
            break;
        }
        if (tokens[cursor] == "DIRECTION" && cursor + 1 < tokens.size()) {
            if (const auto direction = portDirectionFromInterchange(tokens[cursor + 1]))
                pin.direction = *direction;
            cursor += 2;
            continue;
        }
        if (tokens[cursor] == "USE" && cursor + 1 < tokens.size()) {
            const auto use = uppercase(tokens[cursor + 1]);
            if (use == "POWER")
                pin.direction = PortDirection::power;
            else if (use == "GROUND")
                pin.direction = PortDirection::ground;
            cursor += 2;
            continue;
        }
        if (tokens[cursor] == "LAYER" && cursor + 1 < tokens.size()) {
            currentLayer = tokens[cursor + 1];
            cursor += 2;
            continue;
        }
        if (tokens[cursor] == "RECT" && !pin.hasRectangle) {
            std::vector<double> values;
            std::size_t valueCursor = cursor + 1;
            while (valueCursor < tokens.size() && tokens[valueCursor] != ";" && values.size() < 4) {
                if (tokens[valueCursor] == "(" || tokens[valueCursor] == ")") {
                    ++valueCursor;
                    continue;
                }
                try {
                    values.push_back(parseInterchangeNumber(tokens[valueCursor]));
                } catch (const ProjectError &) {
                    break;
                }
                ++valueCursor;
            }
            if (values.size() == 4) {
                const auto x1 = static_cast<Coordinate>(std::llround(values[0] * toDatabase));
                const auto y1 = static_cast<Coordinate>(std::llround(values[1] * toDatabase));
                const auto x2 = static_cast<Coordinate>(std::llround(values[2] * toDatabase));
                const auto y2 = static_cast<Coordinate>(std::llround(values[3] * toDatabase));
                pin.rectangle = {std::min(x1, x2), std::min(y1, y2), std::abs(x2 - x1), std::abs(y2 - y1)};
                pin.hasRectangle = pin.rectangle.valid();
            }
            cursor = valueCursor;
            continue;
        }
        ++cursor;
    }
    pin.layer = currentLayer;
    if (!pin.layer.empty() && pin.hasRectangle)
        macro.pins.push_back(std::move(pin));
    index = cursor;
}

ParsedLef parseLefFile(const std::filesystem::path &path) {
    ParsedLef result;
    const auto tokens = tokenizeInterchange(readInterchangeFile(path, "LEF"));
    double databaseMicrons = 1000.0;
    for (std::size_t index = 0; index + 2 < tokens.size(); ++index) {
        if (tokens[index] == "DATABASE" && tokens[index + 1] == "MICRONS") {
            databaseMicrons = parseInterchangeNumber(tokens[index + 2]);
            break;
        }
    }
    if (databaseMicrons <= 0)
        throw ProjectError("interchange.parse", "LEF database units must be positive");
    // LEF coordinates are expressed in microns. ICE uses nanometres.
    constexpr double toDatabase = 1000.0;

    std::size_t index = 0;
    while (index + 1 < tokens.size()) {
        if (tokens[index] == "PROPERTYDEFINITIONS") {
            std::size_t cursor = index + 1;
            while (cursor + 1 < tokens.size()) {
                if (tokens[cursor] == "END" && tokens[cursor + 1] == "PROPERTYDEFINITIONS") {
                    cursor += 2;
                    break;
                }
                ++cursor;
            }
            index = cursor;
            continue;
        }
        if (tokens[index] == "VIARULE" || tokens[index] == "NONDEFAULTRULES") {
            const auto sectionName = tokens[index];
            const auto name = tokens[index + 1];
            std::size_t cursor = index + 2;
            while (cursor + 1 < tokens.size()) {
                if (tokens[cursor] == "END" && (tokens[cursor + 1] == name || tokens[cursor + 1] == sectionName)) {
                    cursor += 2;
                    break;
                }
                ++cursor;
            }
            index = cursor;
            continue;
        }
        if (tokens[index] == "LAYER") {
            const auto layerName = tokens[index + 1];
            std::size_t cursor = index + 2;
            while (cursor + 1 < tokens.size()) {
                if (tokens[cursor] == "END" && tokens[cursor + 1] == layerName) {
                    cursor += 2;
                    break;
                }
                if (tokens[cursor] == "WIDTH" && cursor + 1 < tokens.size()) {
                    const auto width =
                        static_cast<Coordinate>(std::llround(parseInterchangeNumber(tokens[cursor + 1]) * toDatabase));
                    result.layerWidths[lowercaseInterchange(layerName)] = std::max<Coordinate>(1, width);
                    cursor += 2;
                    continue;
                }
                ++cursor;
            }
            index = cursor;
            continue;
        }
        if (tokens[index] == "VIA") {
            const auto viaName = tokens[index + 1];
            std::size_t cursor = index + 2;
            std::string cutLayer;
            std::string currentLayer;
            while (cursor + 1 < tokens.size()) {
                if (tokens[cursor] == "END" && tokens[cursor + 1] == viaName) {
                    cursor += 2;
                    break;
                }
                if (tokens[cursor] == "LAYER" && cursor + 1 < tokens.size()) {
                    currentLayer = tokens[cursor + 1];
                    const auto lower = lowercaseInterchange(currentLayer);
                    if (cutLayer.empty() &&
                        (lower.find("via") != std::string::npos || lower.find("cut") != std::string::npos))
                        cutLayer = currentLayer;
                    cursor += 2;
                    continue;
                }
                ++cursor;
            }
            if (!cutLayer.empty())
                result.viaLayers[lowercaseInterchange(viaName)] = lowercaseInterchange(cutLayer);
            index = cursor;
            continue;
        }
        if (tokens[index] == "MACRO" && index + 1 < tokens.size()) {
            ParsedLefMacro macro;
            macro.name = tokens[index + 1];
            double width = 0.0;
            double height = 0.0;
            std::size_t cursor = index + 2;
            while (cursor + 1 < tokens.size()) {
                if (tokens[cursor] == "END" && tokens[cursor + 1] == macro.name) {
                    cursor += 2;
                    break;
                }
                if (tokens[cursor] == "SIZE" && cursor + 3 < tokens.size() && tokens[cursor + 2] == "BY") {
                    width = parseInterchangeNumber(tokens[cursor + 1]);
                    height = parseInterchangeNumber(tokens[cursor + 3]);
                    cursor += 4;
                    continue;
                }
                if (tokens[cursor] == "PIN" && cursor + 1 < tokens.size()) {
                    parseLefPin(tokens, cursor, macro, toDatabase);
                    continue;
                }
                ++cursor;
            }
            if (width > 0.0 && height > 0.0) {
                macro.bounds = {0, 0, static_cast<Coordinate>(std::llround(width * toDatabase)),
                                static_cast<Coordinate>(std::llround(height * toDatabase))};
            }
            result.macros[macro.name] = std::move(macro);
            index = cursor;
            continue;
        }
        ++index;
    }
    return result;
}

Orientation defOrientation(const std::string &value) {
    const auto text = uppercase(value);
    if (text == "S")
        return Orientation::r180;
    if (text == "W")
        return Orientation::r90;
    if (text == "E")
        return Orientation::r270;
    if (text == "FN")
        return Orientation::my;
    if (text == "FS")
        return Orientation::mx;
    if (text == "FW")
        return Orientation::m135;
    if (text == "FE")
        return Orientation::m45;
    return Orientation::r0;
}

ObjectId findLayerByName(const Project &project, const std::string &name, const std::string &pdk,
                         const std::string &stack) {
    const auto key = lowercaseInterchange(name);
    for (const auto &[id, layer] : project.layers) {
        if (lowercaseInterchange(layer.name) != key)
            continue;
        if (!pdk.empty() && !layer.pdk.empty() && layer.pdk != pdk)
            continue;
        if (!stack.empty() && !layer.stack.empty() && layer.stack != stack)
            continue;
        return id;
    }
    return 0;
}

ObjectId findLayoutCellByName(const Project &project, const std::string &name, const std::string &pdk,
                              const std::string &stack) {
    for (const auto &[id, cell] : project.layoutCells) {
        if (cell.name == name && cell.pdk == pdk && cell.stack == stack)
            return id;
    }
    return 0;
}

std::string uniqueLayoutName(const Project &project, const std::string &name) {
    const auto taken = [&](const std::string &candidate) {
        for (const auto &[_, cell] : project.layoutCells)
            if (cell.name == candidate)
                return true;
        return false;
    };
    if (!taken(name))
        return name;
    for (int suffix = 2;; ++suffix) {
        const auto candidate = name + " " + std::to_string(suffix);
        if (!taken(candidate))
            return candidate;
    }
}

Rect boundsForPoints(const std::vector<LayoutPoint> &points, Coordinate margin) {
    if (points.empty())
        return {};
    Coordinate minX = points.front().x;
    Coordinate minY = points.front().y;
    Coordinate maxX = points.front().x;
    Coordinate maxY = points.front().y;
    for (const auto &point : points) {
        minX = std::min(minX, point.x);
        minY = std::min(minY, point.y);
        maxX = std::max(maxX, point.x);
        maxY = std::max(maxY, point.y);
    }
    minX -= margin;
    minY -= margin;
    maxX += margin;
    maxY += margin;
    return {minX, minY, std::max<Coordinate>(1, maxX - minX), std::max<Coordinate>(1, maxY - minY)};
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
        const auto &pinName = tokens[index + 1];
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

Project LayoutInterchange::importDef(const std::filesystem::path &path, const DefImportOptions &options) {
    Project project;
    project.identity = path.stem().string();
    project.name = project.identity;
    importDefIntoProject(project, path, options);
    return project;
}

ObjectId LayoutInterchange::importDefIntoProject(Project &project, const std::filesystem::path &path,
                                                 const DefImportOptions &options) {
    const auto tokens = tokenizeInterchange(readInterchangeFile(path, "DEF"));
    double unitsPerMicron = 1000.0;
    std::string designName;
    Rect dieArea;
    for (std::size_t index = 0; index + 1 < tokens.size(); ++index) {
        if (tokens[index] == "UNITS" && tokens[index + 1] == "DISTANCE" && index + 3 < tokens.size() &&
            tokens[index + 2] == "MICRONS") {
            unitsPerMicron = parseInterchangeNumber(tokens[index + 3]);
        } else if (tokens[index] == "DESIGN" && designName.empty()) {
            designName = tokens[index + 1];
        } else if (tokens[index] == "DIEAREA") {
            std::vector<double> values;
            for (std::size_t cursor = index + 1; cursor < tokens.size() && tokens[cursor] != ";"; ++cursor) {
                if (tokens[cursor] == "(" || tokens[cursor] == ")")
                    continue;
                try {
                    values.push_back(parseInterchangeNumber(tokens[cursor]));
                } catch (const ProjectError &) {
                    break;
                }
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
    const auto toDatabase = [unitsPerMicron](double value) {
        return static_cast<Coordinate>(std::llround(value * 1000.0 / unitsPerMicron));
    };
    if (dieArea.valid()) {
        // DIEAREA is parsed directly from the DEF token stream, so it is still
        // in DEF database units. Convert it like every other DEF coordinate;
        // otherwise the die boundary frame does not match the cells, pins, and
        // routes and the fitted preview shrinks away from the viewport.
        dieArea = {toDatabase(dieArea.x), toDatabase(dieArea.y), toDatabase(dieArea.width), toDatabase(dieArea.height)};
    }

    ParsedLef lef;
    std::error_code fileStatus;
    if (!options.techLef.empty() && std::filesystem::is_regular_file(options.techLef, fileStatus) && !fileStatus)
        mergeParsedLef(lef, parseLefFile(options.techLef));
    if (!options.cellLef.empty() && std::filesystem::is_regular_file(options.cellLef, fileStatus) && !fileStatus)
        mergeParsedLef(lef, parseLefFile(options.cellLef));

    std::map<std::string, ObjectId> importedLayers;
    const auto layerFor = [&](const std::string &name) {
        const auto key = lowercaseInterchange(name);
        if (const auto existing = importedLayers.find(key); existing != importedLayers.end())
            return existing->second;
        if (const auto existing = findLayerByName(project, name, options.pdk, options.stack); existing != 0) {
            importedLayers.emplace(key, existing);
            return existing;
        }
        int number = 0;
        int datatype = 0;
        if (const auto definition = options.layers.find(key); definition != options.layers.end()) {
            number = definition->second.number;
            datatype = definition->second.datatype;
        } else {
            std::size_t start = name.size();
            while (start > 0 && std::isdigit(static_cast<unsigned char>(name[start - 1])))
                --start;
            if (start < name.size()) {
                number = std::stoi(name.substr(start));
            } else {
                number = static_cast<int>(project.layers.size()) + 1;
            }
        }
        const auto id = addLayer(project, name, std::max(0, number), 0, options.pdk, datatype, options.stack);
        importedLayers.emplace(key, id);
        return id;
    };

    ObjectId boundaryLayer = 0;
    const auto boundaryLayerFor = [&] {
        if (boundaryLayer != 0)
            return boundaryLayer;
        if (const auto existing = findLayerByName(project, "Cell boundary", {}, {}); existing != 0) {
            boundaryLayer = existing;
            return boundaryLayer;
        }
        boundaryLayer = addLayer(project, "Cell boundary", 0);
        project.layers.at(boundaryLayer).color = "#6b7280";
        return boundaryLayer;
    };

    ObjectId cellId = options.replaceCell;
    if (cellId != 0) {
        if (!project.layoutCells.contains(cellId))
            throw ProjectError("interchange.import", "Layout cell to replace does not exist");
        auto &cell = project.layoutCells.at(cellId);
        cell.shapes.clear();
        cell.ports.clear();
        cell.nets.clear();
        cell.instances.clear();
        cell.plannedDevices.clear();
        cell.plannedConnections.clear();
        cell.generatedBy = 0;
        cell.pdk = options.pdk;
        cell.stack = options.stack;
    } else {
        auto name = options.cellName.empty() ? designName : options.cellName;
        if (name.empty())
            name = "DEF import";
        cellId = createLayoutCell(project, uniqueLayoutName(project, name), options.pdk, options.stack);
    }
    auto &cell = project.layoutCells.at(cellId);
    if (!dieArea.valid() && !tokens.empty()) {
        // A DEF without DIEAREA still gets a boundary from the imported
        // geometry once components and routes are known.
    }

    const auto addRouteShape = [&](const std::string &routeLayer, const std::vector<LayoutPoint> &points,
                                   const std::vector<std::vector<ObjectId>> &pointVias, ObjectId net) {
        if (!options.importRoutes || points.size() < 2)
            return;
        Coordinate width = 200;
        if (const auto found = lef.layerWidths.find(lowercaseInterchange(routeLayer)); found != lef.layerWidths.end())
            width = found->second;
        Shape shape;
        shape.id = project.nextId();
        shape.layer = layerFor(routeLayer);
        shape.net = net;
        shape.centerline = points;
        shape.width = width;
        shape.rectangle = boundsForPoints(points, width / 2);
        const auto hasVias =
            std::any_of(pointVias.begin(), pointVias.end(), [](const auto &entry) { return !entry.empty(); });
        if (hasVias)
            shape.pointVias = pointVias;
        cell.shapes.emplace(shape.id, std::move(shape));
    };

    if (dieArea.valid()) {
        // Insert the boundary first so the canvas paints it below cells and
        // routing instead of covering them with its filled shape.
        const auto shapeId = project.nextId();
        const std::vector<LayoutPoint> polygon = {{dieArea.x, dieArea.y},
                                                  {dieArea.x + dieArea.width, dieArea.y},
                                                  {dieArea.x + dieArea.width, dieArea.y + dieArea.height},
                                                  {dieArea.x, dieArea.y + dieArea.height}};
        Shape boundary{shapeId, boundaryLayerFor(), std::nullopt, dieArea, polygon};
        boundary.name = "Die area";
        cell.shapes.emplace(shapeId, std::move(boundary));
    }

    struct DefComponent {
        std::string name;
        std::string master;
        Coordinate x = 0;
        Coordinate y = 0;
        Orientation orientation = Orientation::r0;
    };
    std::vector<DefComponent> components;
    std::size_t index = 0;
    while (index < tokens.size() && tokens[index] != "COMPONENTS")
        ++index;
    if (index < tokens.size())
        ++index;
    while (index < tokens.size()) {
        if (tokens[index] == "END" && index + 1 < tokens.size() && tokens[index + 1] == "COMPONENTS")
            break;
        if (tokens[index] != "-" || index + 2 >= tokens.size()) {
            ++index;
            continue;
        }
        DefComponent component;
        component.name = tokens[index + 1];
        component.master = tokens[index + 2];
        index += 3;
        bool placed = false;
        while (index < tokens.size() && tokens[index] != ";") {
            if (tokens[index] == "PLACED" || tokens[index] == "FIXED" || tokens[index] == "COVER") {
                ++index;
                if (index + 3 < tokens.size() && tokens[index] == "(" && tokens[index + 3] == ")") {
                    component.x = toDatabase(parseInterchangeNumber(tokens[index + 1]));
                    component.y = toDatabase(parseInterchangeNumber(tokens[index + 2]));
                    index += 4;
                    placed = true;
                }
                continue;
            }
            if (placed && component.orientation == Orientation::r0) {
                const auto orientation = uppercase(tokens[index]);
                if (orientation == "N" || orientation == "S" || orientation == "E" || orientation == "W" ||
                    orientation == "FN" || orientation == "FS" || orientation == "FE" || orientation == "FW") {
                    component.orientation = defOrientation(orientation);
                    ++index;
                    continue;
                }
            }
            ++index;
        }
        if (index < tokens.size() && tokens[index] == ";")
            ++index;
        if (placed)
            components.push_back(std::move(component));
    }

    struct DefPin {
        Port port;
        std::string netName;
    };
    std::vector<DefPin> pins;
    index = 0;
    while (index < tokens.size() && tokens[index] != "PINS")
        ++index;
    if (index < tokens.size())
        ++index;
    while (index < tokens.size()) {
        if (tokens[index] == "END" && index + 1 < tokens.size() && tokens[index + 1] == "PINS")
            break;
        if (tokens[index] != "-" || index + 1 >= tokens.size()) {
            ++index;
            continue;
        }
        DefPin pin;
        pin.port.name = tokens[index + 1];
        pin.port.label = pin.port.name;
        index += 2;
        std::string currentLayer;
        while (index < tokens.size()) {
            if (tokens[index] == "-" ||
                (tokens[index] == "END" && index + 1 < tokens.size() && tokens[index + 1] == "PINS"))
                break;
            if (tokens[index] == "NET" && index + 1 < tokens.size()) {
                pin.netName = tokens[index + 1];
                index += 2;
                continue;
            }
            if (tokens[index] == "DIRECTION" && index + 1 < tokens.size()) {
                if (const auto direction = portDirectionFromInterchange(tokens[index + 1]))
                    pin.port.direction = *direction;
                index += 2;
                continue;
            }
            if (tokens[index] == "USE" && index + 1 < tokens.size()) {
                const auto use = uppercase(tokens[index + 1]);
                if (use == "POWER")
                    pin.port.direction = PortDirection::power;
                else if (use == "GROUND")
                    pin.port.direction = PortDirection::ground;
                index += 2;
                continue;
            }
            if (tokens[index] == "LAYER" && index + 1 < tokens.size()) {
                currentLayer = tokens[index + 1];
                index += 2;
                if (index < tokens.size() && tokens[index] == "(") {
                    double x1 = 0.0;
                    double y1 = 0.0;
                    double x2 = 0.0;
                    double y2 = 0.0;
                    auto cursor = index;
                    if (readLefPoint(tokens, cursor, x1, y1) && readLefPoint(tokens, cursor, x2, y2)) {
                        const auto left = toDatabase(std::min(x1, x2));
                        const auto bottom = toDatabase(std::min(y1, y2));
                        const auto right = toDatabase(std::max(x1, x2));
                        const auto top = toDatabase(std::max(y1, y2));
                        pin.port.rectangle = {left, bottom, right - left, top - bottom};
                        index = cursor;
                    }
                }
                continue;
            }
            ++index;
        }
        if (!currentLayer.empty() && pin.port.rectangle.valid()) {
            pin.port.layer = layerFor(currentLayer);
            pins.push_back(std::move(pin));
        }
    }

    std::map<std::string, ObjectId> netIds;
    const auto ensureNet = [&](const std::string &name) {
        if (name.empty())
            return ObjectId{0};
        if (const auto existing = netIds.find(name); existing != netIds.end())
            return existing->second;
        const auto id = project.nextId();
        cell.nets.emplace(id, LayoutNet{id, name});
        netIds.emplace(name, id);
        return id;
    };

    index = 0;
    while (index < tokens.size() && tokens[index] != "NETS")
        ++index;
    if (index < tokens.size())
        ++index;
    while (index < tokens.size()) {
        if (tokens[index] == "END" && index + 1 < tokens.size() && tokens[index + 1] == "NETS")
            break;
        if (tokens[index] != "-" || index + 1 >= tokens.size()) {
            ++index;
            continue;
        }
        const auto netName = tokens[index + 1];
        index += 2;
        const auto netId = ensureNet(netName);
        while (index < tokens.size()) {
            if (tokens[index] == ";") {
                ++index;
                break;
            }
            if (tokens[index] == "-" ||
                (tokens[index] == "END" && index + 1 < tokens.size() && tokens[index + 1] == "NETS"))
                break;
            if (tokens[index] == "ROUTED") {
                ++index;
                if (index < tokens.size() && tokens[index] != ";" && tokens[index] != "+" && tokens[index] != "-" &&
                    tokens[index] != "NEW") {
                    const auto routeLayer = tokens[index++];
                    std::vector<LayoutPoint> points;
                    std::vector<std::vector<ObjectId>> pointVias;
                    double lastX = 0.0;
                    double lastY = 0.0;
                    while (index < tokens.size()) {
                        if (tokens[index] == "(") {
                            double x = 0.0;
                            double y = 0.0;
                            auto cursor = index;
                            if (readDefPoint(tokens, cursor, lastX, lastY, x, y)) {
                                points.push_back({toDatabase(x), toDatabase(y)});
                                pointVias.emplace_back();
                                index = cursor;
                                continue;
                            }
                        }
                        if (tokens[index] == "NEW" || tokens[index] == "+" || tokens[index] == ";" ||
                            tokens[index] == "-" ||
                            (tokens[index] == "END" && index + 1 < tokens.size() && tokens[index + 1] == "NETS"))
                            break;
                        if (const auto via = lef.viaLayers.find(lowercaseInterchange(tokens[index]));
                            via != lef.viaLayers.end() && !pointVias.empty()) {
                            pointVias.back().push_back(layerFor(via->second));
                        }
                        ++index;
                    }
                    addRouteShape(routeLayer, points, pointVias, netId);
                }
                continue;
            }
            if (tokens[index] == "NEW") {
                ++index;
                if (index < tokens.size() && tokens[index] != ";" && tokens[index] != "+" && tokens[index] != "-" &&
                    tokens[index] != "NEW") {
                    const auto routeLayer = tokens[index++];
                    std::vector<LayoutPoint> points;
                    std::vector<std::vector<ObjectId>> pointVias;
                    double lastX = 0.0;
                    double lastY = 0.0;
                    while (index < tokens.size()) {
                        if (tokens[index] == "(") {
                            double x = 0.0;
                            double y = 0.0;
                            auto cursor = index;
                            if (readDefPoint(tokens, cursor, lastX, lastY, x, y)) {
                                points.push_back({toDatabase(x), toDatabase(y)});
                                pointVias.emplace_back();
                                index = cursor;
                                continue;
                            }
                        }
                        if (tokens[index] == "NEW" || tokens[index] == "+" || tokens[index] == ";" ||
                            tokens[index] == "-" ||
                            (tokens[index] == "END" && index + 1 < tokens.size() && tokens[index + 1] == "NETS"))
                            break;
                        if (const auto via = lef.viaLayers.find(lowercaseInterchange(tokens[index]));
                            via != lef.viaLayers.end() && !pointVias.empty())
                            pointVias.back().push_back(layerFor(via->second));
                        ++index;
                    }
                    addRouteShape(routeLayer, points, pointVias, netId);
                }
                continue;
            }
            ++index;
        }
    }

    std::map<std::string, ObjectId> masterCells;
    const auto masterCellFor = [&](const std::string &master) {
        if (const auto cached = masterCells.find(master); cached != masterCells.end())
            return cached->second;
        if (const auto existing = findLayoutCellByName(project, master, options.pdk, options.stack); existing != 0) {
            masterCells.emplace(master, existing);
            return existing;
        }
        const auto masterId = createLayoutCell(project, uniqueLayoutName(project, master), options.pdk, options.stack);
        auto &masterCell = project.layoutCells.at(masterId);
        if (options.importMasters) {
            if (const auto macro = lef.macros.find(master); macro != lef.macros.end()) {
                if (macro->second.bounds.valid()) {
                    const auto shapeId = project.nextId();
                    Shape boundary{shapeId, boundaryLayerFor(), std::nullopt, macro->second.bounds};
                    boundary.name = "Cell boundary";
                    masterCell.shapes.emplace(shapeId, std::move(boundary));
                }
                for (const auto &pin : macro->second.pins) {
                    const auto portId = project.nextId();
                    Port port;
                    port.id = portId;
                    port.name = pin.name;
                    port.label = pin.name;
                    port.layer = layerFor(pin.layer);
                    port.rectangle = pin.rectangle;
                    port.direction = pin.direction;
                    const auto bounds = macro->second.bounds.valid() ? macro->second.bounds : pin.rectangle;
                    port.side = sideForRectangle(port.rectangle, bounds);
                    masterCell.ports.emplace(portId, std::move(port));
                }
            }
        }
        masterCells.emplace(master, masterId);
        return masterId;
    };

    for (const auto &component : components) {
        const auto instanceId = project.nextId();
        LayoutInstance instance;
        instance.id = instanceId;
        instance.cell = masterCellFor(component.master);
        instance.name = component.name;
        instance.x = component.x;
        instance.y = component.y;
        instance.orientation = component.orientation;
        cell.instances.emplace(instanceId, std::move(instance));
    }

    for (auto &pin : pins) {
        if (!pin.netName.empty())
            pin.port.net = ensureNet(pin.netName);
        const auto bounds = dieArea.valid() ? dieArea : pin.port.rectangle;
        pin.port.side = sideForRectangle(pin.port.rectangle, bounds);
        const auto portId = project.nextId();
        pin.port.id = portId;
        cell.ports.emplace(portId, std::move(pin.port));
    }

    return cellId;
}

struct GdsTransform {
    bool reflect = false;
    double angle = 0.0;
    double magnification = 1.0;
    Coordinate x = 0;
    Coordinate y = 0;
};

struct GdsReference {
    std::string target;
    // OASIS cell-name table index; negative for explicit names.
    int targetIndex = -1;
    GdsTransform transform;
    bool array = false;
    std::uint16_t columns = 0;
    std::uint16_t rows = 0;
    Coordinate columnStepX = 0;
    Coordinate columnStepY = 0;
    Coordinate rowStepX = 0;
    Coordinate rowStepY = 0;
};

struct GdsShape {
    enum class Kind { polygon, path, box, text };
    Kind kind = Kind::polygon;
    int layer = 0;
    int datatype = 0;
    std::vector<LayoutPoint> points;
    std::vector<LayoutPoint> centerline;
    Coordinate width = 0;
    int pathtype = 0;
    Coordinate beginExtension = 0;
    Coordinate endExtension = 0;
    LayoutPoint origin;
    std::string text;
};

struct GdsStructure {
    std::string name;
    std::vector<GdsShape> shapes;
    std::vector<GdsReference> references;
};

struct GdsDocument {
    std::vector<GdsStructure> structures;
    double unitNanometers = 1.0;
};

std::string trimmed(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string lowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}

GdsDocument parseGdsDocument(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw ProjectError("interchange.import", "Unable to open GDSII input");
    GdsDocument document;
    std::size_t currentStructure = std::string::npos;
    enum class Element { none, boundary, path, box, text, sref, aref, node };
    Element element = Element::none;
    int layer = 0;
    int datatype = 0;
    std::vector<LayoutPoint> points;
    Coordinate width = 0;
    int pathtype = 0;
    Coordinate beginExtension = 0;
    Coordinate endExtension = 0;
    LayoutPoint origin;
    std::string text;
    std::string sname;
    bool reflect = false;
    double magnification = 1.0;
    double angle = 0.0;
    std::uint16_t columns = 0;
    std::uint16_t rows = 0;

    const auto resetElement = [&] {
        element = Element::none;
        layer = 0;
        datatype = 0;
        points.clear();
        width = 0;
        pathtype = 0;
        beginExtension = 0;
        endExtension = 0;
        origin = {};
        text.clear();
        sname.clear();
        reflect = false;
        magnification = 1.0;
        angle = 0.0;
        columns = 0;
        rows = 0;
    };
    const auto current = [&]() -> GdsStructure & {
        if (currentStructure == std::string::npos)
            throw ProjectError("interchange.parse", "GDSII element appears outside a structure");
        return document.structures[currentStructure];
    };

    while (input.peek() != std::char_traits<char>::eof()) {
        const auto length = readU16(input);
        if (length < 4)
            throw ProjectError("interchange.parse", "Invalid GDSII record length");
        const auto type = static_cast<unsigned char>(input.get());
        input.get(); // GDSII data type, implied by the record contents.
        std::vector<unsigned char> data(length - 4);
        input.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!input)
            throw ProjectError("interchange.parse", "Truncated GDSII record");

        if (type == kUnits && data.size() >= 16) {
            auto meters = readReal8(data.data() + 8);
            if (!(meters > 0.0))
                meters = readReal8(data.data()) * 1.0e-6;
            if (meters > 0.0)
                document.unitNanometers = meters * 1.0e9;
            continue;
        }
        if (type == kBgnStr) {
            document.structures.emplace_back();
            currentStructure = document.structures.size() - 1;
            resetElement();
            continue;
        }
        if (type == kStrName && currentStructure != std::string::npos) {
            std::string name(data.begin(), data.end());
            while (!name.empty() && name.back() == '\0')
                name.pop_back();
            document.structures[currentStructure].name = name;
            continue;
        }
        if (type == kEndStr) {
            currentStructure = std::string::npos;
            resetElement();
            continue;
        }
        if (type == kEndLib)
            break;
        if (type == kBoundary) {
            resetElement();
            element = Element::boundary;
            continue;
        }
        if (type == kPath) {
            resetElement();
            element = Element::path;
            continue;
        }
        if (type == kBox) {
            resetElement();
            element = Element::box;
            continue;
        }
        if (type == kText) {
            resetElement();
            element = Element::text;
            continue;
        }
        if (type == kSref) {
            resetElement();
            element = Element::sref;
            continue;
        }
        if (type == kAref) {
            resetElement();
            element = Element::aref;
            continue;
        }
        if (type == kNode) {
            resetElement();
            element = Element::node;
            continue;
        }
        if (type == kEndEl) {
            if (currentStructure != std::string::npos) {
                auto &structure = current();
                if (element == Element::boundary && points.size() >= 4) {
                    GdsShape shape;
                    shape.kind = GdsShape::Kind::polygon;
                    shape.layer = layer;
                    shape.datatype = datatype;
                    shape.points = points;
                    structure.shapes.push_back(std::move(shape));
                } else if (element == Element::path && points.size() >= 2) {
                    GdsShape shape;
                    shape.kind = GdsShape::Kind::path;
                    shape.layer = layer;
                    shape.datatype = datatype;
                    shape.centerline = points;
                    shape.width = width;
                    shape.pathtype = pathtype;
                    shape.beginExtension = beginExtension;
                    shape.endExtension = endExtension;
                    structure.shapes.push_back(std::move(shape));
                } else if (element == Element::box && points.size() >= 5) {
                    GdsShape shape;
                    shape.kind = GdsShape::Kind::box;
                    shape.layer = layer;
                    shape.datatype = datatype;
                    shape.points = points;
                    structure.shapes.push_back(std::move(shape));
                } else if (element == Element::text && !points.empty() && !text.empty()) {
                    GdsShape shape;
                    shape.kind = GdsShape::Kind::text;
                    shape.layer = layer;
                    shape.datatype = datatype;
                    shape.origin = points.front();
                    shape.text = text;
                    structure.shapes.push_back(std::move(shape));
                } else if (element == Element::sref && !points.empty() && !sname.empty()) {
                    GdsReference reference;
                    reference.target = sname;
                    reference.transform.reflect = reflect;
                    reference.transform.angle = angle;
                    reference.transform.magnification = magnification;
                    reference.transform.x = points.front().x;
                    reference.transform.y = points.front().y;
                    structure.references.push_back(std::move(reference));
                } else if (element == Element::aref && points.size() >= 3 && !sname.empty() && columns > 0 &&
                           rows > 0) {
                    GdsReference reference;
                    reference.target = sname;
                    reference.transform.reflect = reflect;
                    reference.transform.angle = angle;
                    reference.transform.magnification = magnification;
                    reference.transform.x = points[0].x;
                    reference.transform.y = points[0].y;
                    reference.array = true;
                    reference.columns = columns;
                    reference.rows = rows;
                    reference.columnStepX = (points[1].x - points[0].x) / static_cast<Coordinate>(columns);
                    reference.columnStepY = (points[1].y - points[0].y) / static_cast<Coordinate>(columns);
                    reference.rowStepX = (points[2].x - points[0].x) / static_cast<Coordinate>(rows);
                    reference.rowStepY = (points[2].y - points[0].y) / static_cast<Coordinate>(rows);
                    structure.references.push_back(std::move(reference));
                }
            }
            resetElement();
            continue;
        }
        if (type == kLayer && data.size() >= 2)
            layer = static_cast<int>(readI16(data.data()));
        else if ((type == kDataType || type == kTextType || type == kBoxType || type == kNodeType) && data.size() >= 2)
            datatype = static_cast<int>(readI16(data.data()));
        else if (type == kXy && data.size() % 8 == 0) {
            points.clear();
            for (std::size_t index = 0; index < data.size(); index += 8)
                points.push_back({readI32(data.data() + index), readI32(data.data() + index + 4)});
        } else if (type == kWidth && data.size() >= 4) {
            const auto rawWidth = readI32(data.data());
            width = rawWidth < 0 ? static_cast<Coordinate>(-static_cast<std::int64_t>(rawWidth)) : rawWidth;
        } else if (type == kPathType && data.size() >= 2) {
            pathtype = static_cast<int>(readI16(data.data()));
        } else if (type == kBgnExtn && data.size() >= 4) {
            beginExtension = readI32(data.data());
        } else if (type == kEndExtn && data.size() >= 4) {
            endExtension = readI32(data.data());
        } else if (type == kString) {
            text.assign(data.begin(), data.end());
            while (!text.empty() && text.back() == '\0')
                text.pop_back();
        } else if (type == kSname) {
            sname.assign(data.begin(), data.end());
            while (!sname.empty() && sname.back() == '\0')
                sname.pop_back();
        } else if (type == kStrans && data.size() >= 2) {
            reflect = (readI16(data.data()) & 0x8000) != 0;
        } else if (type == kMag && data.size() >= 8) {
            magnification = readReal8(data.data());
        } else if (type == kAngle && data.size() >= 8) {
            angle = readReal8(data.data());
        } else if (type == kColrow && data.size() >= 4) {
            columns = readI16(data.data());
            rows = readI16(data.data() + 2);
        }
    }
    if (document.structures.empty())
        throw ProjectError("interchange.parse", "GDSII file contains no structures");
    for (std::size_t index = 0; index < document.structures.size(); ++index) {
        if (document.structures[index].name.empty())
            document.structures[index].name = "CELL_" + std::to_string(index + 1);
    }
    return document;
}

struct Affine {
    double a = 1.0;
    double b = 0.0;
    double c = 0.0;
    double d = 1.0;
    double e = 0.0;
    double f = 0.0;

    Point apply(const Point &point) const {
        return {static_cast<Coordinate>(
                    std::llround(a * static_cast<double>(point.x) + b * static_cast<double>(point.y) + e)),
                static_cast<Coordinate>(
                    std::llround(c * static_cast<double>(point.x) + d * static_cast<double>(point.y) + f))};
    }
};

Affine compose(const Affine &parent, const Affine &child) {
    return {parent.a * child.a + parent.b * child.c,
            parent.a * child.b + parent.b * child.d,
            parent.c * child.a + parent.d * child.c,
            parent.c * child.b + parent.d * child.d,
            parent.a * child.e + parent.b * child.f + parent.e,
            parent.c * child.e + parent.d * child.f + parent.f};
}

Affine affineForOrientation(Orientation orientation, Coordinate x, Coordinate y) {
    const auto tx = static_cast<double>(x);
    const auto ty = static_cast<double>(y);
    switch (orientation) {
    case Orientation::r0:
        return {1, 0, 0, 1, tx, ty};
    case Orientation::r90:
        return {0, -1, 1, 0, tx, ty};
    case Orientation::r180:
        return {-1, 0, 0, -1, tx, ty};
    case Orientation::r270:
        return {0, 1, -1, 0, tx, ty};
    case Orientation::mx:
        return {1, 0, 0, -1, tx, ty};
    case Orientation::my:
        return {-1, 0, 0, 1, tx, ty};
    case Orientation::m45:
        return {0, 1, 1, 0, tx, ty};
    case Orientation::m135:
        return {0, -1, -1, 0, tx, ty};
    }
    return {1, 0, 0, 1, tx, ty};
}

Affine affineForGds(const GdsTransform &transform) {
    constexpr double pi = 3.14159265358979323846;
    const auto radians = transform.angle * pi / 180.0;
    const auto cosine = std::cos(radians) * transform.magnification;
    const auto sine = std::sin(radians) * transform.magnification;
    return {cosine,
            transform.reflect ? sine : -sine,
            sine,
            transform.reflect ? -cosine : cosine,
            static_cast<double>(transform.x),
            static_cast<double>(transform.y)};
}

std::optional<Orientation> orientationForGds(bool reflect, double angle) {
    auto normalized = std::fmod(angle, 360.0);
    if (normalized < 0.0)
        normalized += 360.0;
    const auto quarter = static_cast<int>(std::llround(normalized / 90.0));
    if (std::abs(normalized - static_cast<double>(quarter) * 90.0) > 1.0e-6)
        return std::nullopt;
    switch ((quarter % 4 + 4) % 4) {
    case 0:
        return reflect ? Orientation::mx : Orientation::r0;
    case 1:
        return reflect ? Orientation::m45 : Orientation::r90;
    case 2:
        return reflect ? Orientation::my : Orientation::r180;
    case 3:
        return reflect ? Orientation::m135 : Orientation::r270;
    default:
        break;
    }
    return std::nullopt;
}

void addImportWarning(const LayoutImportOptions &options, std::string message) {
    if (options.warnings)
        options.warnings->push_back(std::move(message));
}

void flattenCellGeometry(Project &project, ObjectId targetCellId, const Affine &transform, LayoutCell &destination,
                         std::set<ObjectId> &ancestors) {
    if (!ancestors.insert(targetCellId).second)
        return;
    const auto target = project.layoutCells.find(targetCellId);
    if (target == project.layoutCells.end()) {
        ancestors.erase(targetCellId);
        return;
    }
    const auto magnitude = std::max(1.0e-9, std::hypot(transform.a, transform.c));
    for (const auto &[_, shape] : target->second.shapes) {
        const auto id = project.nextId();
        if (shape.isText()) {
            const auto origin = transform.apply({shape.rectangle.x, shape.rectangle.y});
            const auto width = std::max<Coordinate>(
                1, static_cast<Coordinate>(std::llround(static_cast<double>(shape.rectangle.width) * magnitude)));
            const auto height = std::max<Coordinate>(
                1, static_cast<Coordinate>(std::llround(static_cast<double>(shape.rectangle.height) * magnitude)));
            Shape flattened{id, shape.layer, shape.net, {origin.x, origin.y, width, height}, {}};
            flattened.text = shape.text;
            destination.shapes.emplace(id, std::move(flattened));
            continue;
        }
        if (shape.isPath()) {
            std::vector<LayoutPoint> centerline;
            centerline.reserve(shape.centerline.size());
            Coordinate minX = std::numeric_limits<Coordinate>::max();
            Coordinate minY = std::numeric_limits<Coordinate>::max();
            Coordinate maxX = std::numeric_limits<Coordinate>::lowest();
            Coordinate maxY = std::numeric_limits<Coordinate>::lowest();
            for (const auto &source : shape.centerline) {
                const auto point = transform.apply({source.x, source.y});
                centerline.push_back({point.x, point.y});
                minX = std::min(minX, point.x);
                minY = std::min(minY, point.y);
                maxX = std::max(maxX, point.x);
                maxY = std::max(maxY, point.y);
            }
            if (centerline.size() < 2 || maxX < minX || maxY < minY) {
                continue;
            }
            const auto width = std::max<Coordinate>(
                1, static_cast<Coordinate>(std::llround(static_cast<double>(shape.width) * magnitude)));
            const auto half = width / 2;
            Shape flattened{id,
                            shape.layer,
                            shape.net,
                            {minX - half, minY - half, maxX - minX + 2 * half, maxY - minY + 2 * half},
                            {}};
            flattened.centerline = std::move(centerline);
            flattened.width = width;
            flattened.segmentLayers = shape.segmentLayers;
            flattened.pointVias = shape.pointVias;
            if (!shape.segmentWidths.empty()) {
                flattened.segmentWidths.reserve(shape.segmentWidths.size());
                for (const auto segmentWidth : shape.segmentWidths)
                    flattened.segmentWidths.push_back(std::max<Coordinate>(
                        1, static_cast<Coordinate>(std::llround(static_cast<double>(segmentWidth) * magnitude))));
            }
            destination.shapes.emplace(id, std::move(flattened));
            continue;
        }
        std::vector<LayoutPoint> polygon = shape.polygon;
        if (polygon.empty()) {
            const auto x = shape.rectangle.x;
            const auto y = shape.rectangle.y;
            const auto right = shape.rectangle.x + shape.rectangle.width;
            const auto top = shape.rectangle.y + shape.rectangle.height;
            polygon = {{x, y}, {right, y}, {right, top}, {x, top}};
        }
        std::vector<LayoutPoint> flattenedPolygon;
        flattenedPolygon.reserve(polygon.size());
        Coordinate minX = std::numeric_limits<Coordinate>::max();
        Coordinate minY = std::numeric_limits<Coordinate>::max();
        Coordinate maxX = std::numeric_limits<Coordinate>::lowest();
        Coordinate maxY = std::numeric_limits<Coordinate>::lowest();
        for (const auto &source : polygon) {
            const auto point = transform.apply({source.x, source.y});
            flattenedPolygon.push_back({point.x, point.y});
            minX = std::min(minX, point.x);
            minY = std::min(minY, point.y);
            maxX = std::max(maxX, point.x);
            maxY = std::max(maxY, point.y);
        }
        if (flattenedPolygon.empty() || maxX <= minX || maxY <= minY)
            continue;
        destination.shapes.emplace(
            id, Shape{id, shape.layer, shape.net, {minX, minY, maxX - minX, maxY - minY}, std::move(flattenedPolygon)});
    }
    for (const auto &[_, instance] : target->second.instances) {
        const auto child = affineForOrientation(instance.orientation, instance.x, instance.y);
        flattenCellGeometry(project, instance.cell, compose(transform, child), destination, ancestors);
    }
    ancestors.erase(targetCellId);
}

Project buildGdsProject(const std::filesystem::path &path, const GdsDocument &document,
                        const LayoutImportOptions &options) {
    Project project;
    project.identity = path.stem().string();
    project.name = project.identity;
    project.units.nanometers_per_unit = 1;
    project.units.denominator = 1;

    std::vector<std::string> names(document.structures.size());
    std::set<std::string> usedNames;
    for (std::size_t index = 0; index < document.structures.size(); ++index) {
        auto base = document.structures[index].name;
        if (base.empty())
            base = "CELL_" + std::to_string(index + 1);
        auto candidate = base;
        for (int suffix = 2; !usedNames.insert(candidate).second; ++suffix)
            candidate = base + " (" + std::to_string(suffix) + ")";
        names[index] = std::move(candidate);
    }

    std::set<std::string> referenced;
    for (const auto &structure : document.structures)
        for (const auto &reference : structure.references)
            referenced.insert(reference.target);
    std::size_t topIndex = std::string::npos;
    if (!options.topCell.empty()) {
        for (std::size_t index = 0; index < document.structures.size(); ++index) {
            if (document.structures[index].name == options.topCell || names[index] == options.topCell) {
                topIndex = index;
                break;
            }
        }
        if (topIndex == std::string::npos)
            throw ProjectError("interchange.top", "Layout top cell '" + options.topCell + "' was not found");
    } else {
        std::vector<std::size_t> candidates;
        for (std::size_t index = 0; index < document.structures.size(); ++index)
            if (!referenced.contains(document.structures[index].name))
                candidates.push_back(index);
        topIndex = candidates.empty() ? document.structures.size() - 1 : candidates.back();
    }

    std::vector<std::size_t> order;
    order.push_back(topIndex);
    for (std::size_t index = 0; index < document.structures.size(); ++index)
        if (index != topIndex)
            order.push_back(index);
    std::vector<ObjectId> cellIds(document.structures.size(), 0);
    for (const auto index : order) {
        LayoutCell cell;
        cell.id = project.nextId();
        cell.name = names[index];
        cellIds[index] = cell.id;
        project.layoutCells.emplace(cell.id, std::move(cell));
    }

    std::map<std::pair<int, int>, ObjectId> layers;
    const auto layerFor = [&](int number, int datatype) {
        const auto key = std::make_pair(number, datatype);
        if (const auto existing = layers.find(key); existing != layers.end())
            return existing->second;
        Layer layer;
        layer.id = project.nextId();
        layer.name = "GDS layer " + std::to_string(number);
        if (datatype != 0)
            layer.name += " (dt " + std::to_string(datatype) + ")";
        layer.number = number;
        layer.datatype = datatype;
        layer.visible = true;
        const auto id = layer.id;
        project.layers.emplace(id, std::move(layer));
        layers.emplace(key, id);
        return id;
    };

    const auto unit = document.unitNanometers > 0.0 ? document.unitNanometers : 1.0;
    const auto scale = [unit](Coordinate value) {
        return static_cast<Coordinate>(std::llround(static_cast<double>(value) * unit));
    };

    std::map<ObjectId, std::set<std::string>> portNames;
    for (std::size_t index = 0; index < document.structures.size(); ++index) {
        auto &cell = project.layoutCells.at(cellIds[index]);
        for (const auto &shape : document.structures[index].shapes) {
            const auto layer = layerFor(shape.layer, shape.datatype);
            if (shape.kind == GdsShape::Kind::polygon) {
                std::vector<LayoutPoint> polygon;
                polygon.reserve(shape.points.size());
                Coordinate minX = std::numeric_limits<Coordinate>::max();
                Coordinate minY = std::numeric_limits<Coordinate>::max();
                Coordinate maxX = std::numeric_limits<Coordinate>::lowest();
                Coordinate maxY = std::numeric_limits<Coordinate>::lowest();
                for (const auto &source : shape.points) {
                    const auto point = LayoutPoint{scale(source.x), scale(source.y)};
                    polygon.push_back(point);
                    minX = std::min(minX, point.x);
                    minY = std::min(minY, point.y);
                    maxX = std::max(maxX, point.x);
                    maxY = std::max(maxY, point.y);
                }
                if (polygon.empty() || maxX <= minX || maxY <= minY)
                    continue;
                const auto id = project.nextId();
                cell.shapes.emplace(
                    id, Shape{id, layer, std::nullopt, {minX, minY, maxX - minX, maxY - minY}, std::move(polygon)});
            } else if (shape.kind == GdsShape::Kind::box) {
                Coordinate minX = std::numeric_limits<Coordinate>::max();
                Coordinate minY = std::numeric_limits<Coordinate>::max();
                Coordinate maxX = std::numeric_limits<Coordinate>::lowest();
                Coordinate maxY = std::numeric_limits<Coordinate>::lowest();
                for (const auto &source : shape.points) {
                    const auto x = scale(source.x);
                    const auto y = scale(source.y);
                    minX = std::min(minX, x);
                    minY = std::min(minY, y);
                    maxX = std::max(maxX, x);
                    maxY = std::max(maxY, y);
                }
                if (maxX <= minX || maxY <= minY)
                    continue;
                const auto id = project.nextId();
                cell.shapes.emplace(id, Shape{id, layer, std::nullopt, {minX, minY, maxX - minX, maxY - minY}, {}});
            } else if (shape.kind == GdsShape::Kind::path) {
                std::vector<LayoutPoint> centerline;
                centerline.reserve(shape.centerline.size());
                Coordinate minX = std::numeric_limits<Coordinate>::max();
                Coordinate minY = std::numeric_limits<Coordinate>::max();
                Coordinate maxX = std::numeric_limits<Coordinate>::lowest();
                Coordinate maxY = std::numeric_limits<Coordinate>::lowest();
                for (const auto &source : shape.centerline) {
                    const auto point = LayoutPoint{scale(source.x), scale(source.y)};
                    centerline.push_back(point);
                    minX = std::min(minX, point.x);
                    minY = std::min(minY, point.y);
                    maxX = std::max(maxX, point.x);
                    maxY = std::max(maxY, point.y);
                }
                if (centerline.size() < 2 || maxX < minX || maxY < minY)
                    continue;
                const auto width = std::max<Coordinate>(1, scale(shape.width));
                auto beginExtension = static_cast<Coordinate>(0);
                auto endExtension = static_cast<Coordinate>(0);
                if (shape.pathtype == 1 || shape.pathtype == 2) {
                    beginExtension = width / 2;
                    endExtension = width / 2;
                } else if (shape.pathtype == 4) {
                    beginExtension = std::max<Coordinate>(0, scale(shape.beginExtension));
                    endExtension = std::max<Coordinate>(0, scale(shape.endExtension));
                }
                const auto half = width / 2;
                const auto extension = std::max(beginExtension, endExtension);
                const auto id = project.nextId();
                Shape path{id,
                           layer,
                           std::nullopt,
                           {minX - half - extension, minY - half - extension, maxX - minX + 2 * (half + extension),
                            maxY - minY + 2 * (half + extension)},
                           {}};
                path.centerline = std::move(centerline);
                path.width = width;
                cell.shapes.emplace(id, std::move(path));
            } else if (shape.kind == GdsShape::Kind::text) {
                const auto x = scale(shape.origin.x);
                const auto y = scale(shape.origin.y);
                const auto height = std::max<Coordinate>(1, std::llround(1000.0 * unit));
                const auto textWidth = std::max<Coordinate>(
                    1, static_cast<Coordinate>(
                           std::llround(static_cast<double>(shape.text.size()) * static_cast<double>(height) * 0.6)));
                const auto id = project.nextId();
                Shape textShape{id, layer, std::nullopt, {x, y - height, textWidth, height}, {}};
                textShape.text = shape.text;
                cell.shapes.emplace(id, textShape);
                if (options.pinLabelLayer && *options.pinLabelLayer == shape.layer) {
                    const auto name = trimmed(shape.text);
                    if (!name.empty()) {
                        auto &used = portNames[cellIds[index]];
                        auto uniqueName = name;
                        for (int suffix = 2; used.contains(lowerCopy(uniqueName)); ++suffix)
                            uniqueName = name + "_" + std::to_string(suffix);
                        used.insert(lowerCopy(uniqueName));
                        const auto pinWidth = std::max<Coordinate>(1, options.pinWidth);
                        const auto pinHeight = std::max<Coordinate>(1, options.pinHeight);
                        Port port;
                        port.id = project.nextId();
                        port.name = uniqueName;
                        port.label = shape.text;
                        port.layer = layer;
                        port.rectangle = {x - pinWidth / 2, y - pinHeight / 2, pinWidth, pinHeight};
                        port.direction = PortDirection::inout;
                        port.side = PortSide::left;
                        cell.ports.emplace(port.id, std::move(port));
                    }
                }
            }
        }
    }

    std::map<std::string, std::size_t> indexByName;
    for (std::size_t index = 0; index < document.structures.size(); ++index) {
        if (!indexByName.emplace(document.structures[index].name, index).second)
            addImportWarning(options, "Duplicate GDSII structure name '" + document.structures[index].name +
                                          "'; references use the first definition.");
    }
    for (std::size_t index = 0; index < document.structures.size(); ++index) {
        auto &cell = project.layoutCells.at(cellIds[index]);
        std::set<std::string> instanceNames;
        const auto uniqueInstanceName = [&](const std::string &base) {
            auto candidate = base.empty() ? "I" : base;
            for (int suffix = 2; !instanceNames.insert(candidate).second; ++suffix)
                candidate = base + "_" + std::to_string(suffix);
            return candidate;
        };
        for (const auto &reference : document.structures[index].references) {
            const auto target = indexByName.find(reference.target);
            if (target == indexByName.end()) {
                addImportWarning(options,
                                 "GDSII reference to missing structure '" + reference.target + "' was skipped.");
                continue;
            }
            const auto targetId = cellIds[target->second];
            if (targetId == cellIds[index]) {
                addImportWarning(options, "GDSII self-reference in structure '" + document.structures[index].name +
                                              "' was skipped.");
                continue;
            }
            const auto orientation = orientationForGds(reference.transform.reflect, reference.transform.angle);
            const auto supported =
                orientation.has_value() && std::abs(reference.transform.magnification - 1.0) < 1.0e-6;
            const auto maxColumns = reference.array ? reference.columns : 1;
            const auto maxRows = reference.array ? reference.rows : 1;
            if (static_cast<std::uint64_t>(maxColumns) * maxRows > 1'000'000) {
                addImportWarning(options, "GDSII array reference to '" + reference.target +
                                              "' exceeds one million elements and was skipped.");
                continue;
            }
            if (supported) {
                for (std::uint16_t row = 0; row < maxRows; ++row) {
                    for (std::uint16_t column = 0; column < maxColumns; ++column) {
                        const auto x =
                            scale(reference.transform.x + static_cast<Coordinate>(column) * reference.columnStepX +
                                  static_cast<Coordinate>(row) * reference.rowStepX);
                        const auto y =
                            scale(reference.transform.y + static_cast<Coordinate>(column) * reference.columnStepY +
                                  static_cast<Coordinate>(row) * reference.rowStepY);
                        LayoutInstance instance;
                        instance.id = project.nextId();
                        instance.cell = targetId;
                        instance.name = uniqueInstanceName(names[target->second]);
                        instance.x = x;
                        instance.y = y;
                        instance.orientation = *orientation;
                        cell.instances.emplace(instance.id, std::move(instance));
                    }
                }
            } else {
                addImportWarning(options, "GDSII reference to '" + reference.target +
                                              "' uses an unsupported magnification or rotation; geometry was flattened "
                                              "and child ports were not promoted.");
                auto transform = reference.transform;
                transform.x = scale(transform.x);
                transform.y = scale(transform.y);
                const auto base = affineForGds(transform);
                for (std::uint16_t row = 0; row < maxRows; ++row) {
                    for (std::uint16_t column = 0; column < maxColumns; ++column) {
                        auto element = base;
                        element.e += scale(static_cast<Coordinate>(column) * reference.columnStepX +
                                           static_cast<Coordinate>(row) * reference.rowStepX);
                        element.f += scale(static_cast<Coordinate>(column) * reference.columnStepY +
                                           static_cast<Coordinate>(row) * reference.rowStepY);
                        std::set<ObjectId> ancestors{cellIds[index]};
                        flattenCellGeometry(project, targetId, element, cell, ancestors);
                    }
                }
            }
        }
    }
    return project;
}

Project LayoutInterchange::importGds(const std::filesystem::path &path, const LayoutImportOptions &options) {
    return buildGdsProject(path, parseGdsDocument(path), options);
}

std::string LayoutInterchange::gdsStructureName(const std::string &cellName) { return sanitizeGdsName(cellName); }

struct OasisInput {
    const std::vector<unsigned char> *bytes = nullptr;
    std::size_t position = 0;

    bool eof() const { return !bytes || position >= bytes->size(); }

    unsigned char get() {
        if (eof())
            throw ProjectError("interchange.oasis", "Unexpected end of OASIS stream");
        return (*bytes)[position++];
    }

    unsigned char peek() const {
        if (eof())
            throw ProjectError("interchange.oasis", "Unexpected end of OASIS stream");
        return (*bytes)[position];
    }

    void read(void *destination, std::size_t count) {
        if (!bytes || position + count > bytes->size())
            throw ProjectError("interchange.oasis", "Truncated OASIS stream");
        std::memcpy(destination, bytes->data() + position, count);
        position += count;
    }

    void skip(std::size_t count) {
        if (!bytes || position + count > bytes->size())
            throw ProjectError("interchange.oasis", "Truncated OASIS stream");
        position += count;
    }
};

std::uint64_t readOasisUnsigned(OasisInput &input) {
    std::uint64_t result = 0;
    int shift = 0;
    while (true) {
        const auto byte = input.get();
        result |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0)
            return result;
        shift += 7;
        if (shift > 63)
            throw ProjectError("interchange.oasis", "OASIS unsigned integer is too large");
    }
}

std::uint8_t readOasisIntInternal(OasisInput &input, int skipBits, std::int64_t &value) {
    auto byte = input.get();
    value = static_cast<std::int64_t>((byte & 0x7f) >> skipBits);
    const auto flags = static_cast<std::uint8_t>(byte & ((1u << skipBits) - 1u));
    int shift = 7 - skipBits;
    while (byte & 0x80) {
        byte = input.get();
        value |= static_cast<std::int64_t>(byte & 0x7f) << shift;
        shift += 7;
        if (shift > 63)
            throw ProjectError("interchange.oasis", "OASIS integer is too large");
    }
    return flags;
}

std::int64_t readOasisSigned(OasisInput &input) {
    std::int64_t value = 0;
    return readOasisIntInternal(input, 1, value) ? -value : value;
}

void readOasis2Delta(OasisInput &input, std::int64_t &x, std::int64_t &y) {
    std::int64_t value = 0;
    switch (readOasisIntInternal(input, 2, value)) {
    case 0:
        x = value;
        y = 0;
        break;
    case 1:
        x = 0;
        y = value;
        break;
    case 2:
        x = -value;
        y = 0;
        break;
    case 3:
        x = 0;
        y = -value;
        break;
    default:
        x = 0;
        y = 0;
    }
}

void readOasis3Delta(OasisInput &input, std::int64_t &x, std::int64_t &y) {
    std::int64_t value = 0;
    switch (readOasisIntInternal(input, 3, value)) {
    case 0:
        x = value;
        y = 0;
        break;
    case 1:
        x = 0;
        y = value;
        break;
    case 2:
        x = -value;
        y = 0;
        break;
    case 3:
        x = 0;
        y = -value;
        break;
    case 4:
        x = value;
        y = value;
        break;
    case 5:
        x = -value;
        y = value;
        break;
    case 6:
        x = -value;
        y = -value;
        break;
    case 7:
        x = value;
        y = -value;
        break;
    default:
        x = 0;
        y = 0;
    }
}

void readOasisGDelta(OasisInput &input, std::int64_t &x, std::int64_t &y) {
    if ((input.peek() & 0x01) == 0) {
        std::int64_t value = 0;
        const auto direction = static_cast<int>(readOasisIntInternal(input, 4, value) >> 1);
        switch (direction) {
        case 0:
            x = value;
            y = 0;
            break;
        case 1:
            x = 0;
            y = value;
            break;
        case 2:
            x = -value;
            y = 0;
            break;
        case 3:
            x = 0;
            y = -value;
            break;
        case 4:
            x = value;
            y = value;
            break;
        case 5:
            x = -value;
            y = value;
            break;
        case 6:
            x = -value;
            y = -value;
            break;
        case 7:
            x = value;
            y = -value;
            break;
        default:
            x = 0;
            y = 0;
        }
    } else {
        std::int64_t value = 0;
        if (readOasisIntInternal(input, 2, value) & 0x02)
            value = -value;
        x = value;
        if (readOasisIntInternal(input, 1, value) & 0x01)
            value = -value;
        y = value;
    }
}

float readOasisFloat(OasisInput &input) {
    unsigned char bytes[4];
    input.read(bytes, 4);
    const auto raw = static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
                     (static_cast<std::uint32_t>(bytes[2]) << 16) | (static_cast<std::uint32_t>(bytes[3]) << 24);
    float value = 0.0f;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

double readOasisDouble(OasisInput &input) {
    unsigned char bytes[8];
    input.read(bytes, 8);
    std::uint64_t raw = 0;
    for (int index = 0; index < 8; ++index)
        raw |= static_cast<std::uint64_t>(bytes[index]) << (8 * index);
    double value = 0.0;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

double readOasisReal(OasisInput &input) {
    const auto type = input.get();
    switch (type) {
    case 0:
        return static_cast<double>(readOasisUnsigned(input));
    case 1:
        return -static_cast<double>(readOasisUnsigned(input));
    case 2:
        return 1.0 / static_cast<double>(readOasisUnsigned(input));
    case 3:
        return -1.0 / static_cast<double>(readOasisUnsigned(input));
    case 4: {
        const auto numerator = static_cast<double>(readOasisUnsigned(input));
        return numerator / static_cast<double>(readOasisUnsigned(input));
    }
    case 5: {
        const auto numerator = static_cast<double>(readOasisUnsigned(input));
        return -numerator / static_cast<double>(readOasisUnsigned(input));
    }
    case 6:
        return readOasisFloat(input);
    case 7:
        return readOasisDouble(input);
    default:
        throw ProjectError("interchange.oasis", "Unsupported OASIS real number type");
    }
}

std::string readOasisString(OasisInput &input) {
    const auto count = readOasisUnsigned(input);
    if (count > 100000000)
        throw ProjectError("interchange.oasis", "OASIS string is too large");
    std::string value(count, '\0');
    if (count > 0)
        input.read(value.data(), count);
    return value;
}

void skipOasisInterval(OasisInput &input) {
    const auto type = readOasisUnsigned(input);
    if (type == 1 || type == 2 || type == 3)
        readOasisUnsigned(input);
    else if (type == 4) {
        readOasisUnsigned(input);
        readOasisUnsigned(input);
    }
}

std::vector<LayoutPoint> readOasisPointList(OasisInput &input, bool closed) {
    const auto type = input.get();
    const auto count = readOasisUnsigned(input);
    if (count > 100000000)
        throw ProjectError("interchange.oasis", "OASIS point list is too large");
    std::vector<LayoutPoint> points;
    points.push_back({0, 0});
    const auto add = [&](std::int64_t x, std::int64_t y) {
        const auto &last = points.back();
        points.push_back({last.x + static_cast<Coordinate>(x), last.y + static_cast<Coordinate>(y)});
    };
    switch (type) {
    case 0:
    case 1: {
        bool horizontal = type == 0;
        for (std::uint64_t index = 0; index < count; ++index) {
            const auto delta = readOasisSigned(input);
            if (horizontal)
                add(delta, 0);
            else
                add(0, delta);
            horizontal = !horizontal;
        }
        if (closed) {
            const auto &last = points.back();
            points.push_back(horizontal ? LayoutPoint{0, last.y} : LayoutPoint{last.x, 0});
        }
        break;
    }
    case 2:
        for (std::uint64_t index = 0; index < count; ++index) {
            std::int64_t x = 0;
            std::int64_t y = 0;
            readOasis2Delta(input, x, y);
            add(x, y);
        }
        break;
    case 3:
        for (std::uint64_t index = 0; index < count; ++index) {
            std::int64_t x = 0;
            std::int64_t y = 0;
            readOasis3Delta(input, x, y);
            add(x, y);
        }
        break;
    case 4:
        for (std::uint64_t index = 0; index < count; ++index) {
            std::int64_t x = 0;
            std::int64_t y = 0;
            readOasisGDelta(input, x, y);
            add(x, y);
        }
        break;
    case 5: {
        LayoutPoint delta{0, 0};
        for (std::uint64_t index = 0; index < count; ++index) {
            std::int64_t x = 0;
            std::int64_t y = 0;
            readOasisGDelta(input, x, y);
            delta.x += static_cast<Coordinate>(x);
            delta.y += static_cast<Coordinate>(y);
            const auto &last = points.back();
            points.push_back({last.x + delta.x, last.y + delta.y});
        }
        break;
    }
    default:
        throw ProjectError("interchange.oasis", "Unsupported OASIS point-list type");
    }
    return points;
}

std::vector<LayoutPoint> readOasisRepetition(OasisInput &input) {
    const auto type = input.get();
    if (type == 0)
        return {{0, 0}};
    std::vector<LayoutPoint> offsets;
    offsets.push_back({0, 0});
    const auto add = [&](std::int64_t x, std::int64_t y) {
        offsets.push_back({static_cast<Coordinate>(x), static_cast<Coordinate>(y)});
    };
    const auto limit = [&] {
        if (offsets.size() > 1000000)
            throw ProjectError("interchange.oasis", "OASIS repetition exceeds one million elements");
    };
    switch (type) {
    case 1:
    case 2:
    case 3: {
        const auto columns = type == 3 ? 1 : 2 + readOasisUnsigned(input);
        const auto rows = type == 2 ? 1 : 2 + (type == 3 ? readOasisUnsigned(input) : readOasisUnsigned(input));
        const auto stepX = type == 3 ? 0 : static_cast<Coordinate>(readOasisUnsigned(input));
        const auto stepY = type == 2 ? 0 : static_cast<Coordinate>(readOasisUnsigned(input));
        offsets.clear();
        for (std::uint64_t column = 0; column < columns; ++column)
            for (std::uint64_t row = 0; row < rows; ++row) {
                add(static_cast<Coordinate>(column) * stepX, static_cast<Coordinate>(row) * stepY);
                limit();
            }
        break;
    }
    case 4:
    case 5:
    case 6:
    case 7: {
        const auto count = 1 + readOasisUnsigned(input);
        const auto grid = (type == 5 || type == 7) ? static_cast<Coordinate>(readOasisUnsigned(input)) : 1;
        std::int64_t coordinate = 0;
        for (std::uint64_t index = 0; index < count; ++index) {
            coordinate += static_cast<std::int64_t>(readOasisUnsigned(input)) * grid;
            if (type == 4 || type == 5)
                add(coordinate, 0);
            else
                add(0, coordinate);
            limit();
        }
        break;
    }
    case 8:
    case 9: {
        const auto columns = 2 + readOasisUnsigned(input);
        const auto rows = type == 9 ? 1 : 2 + readOasisUnsigned(input);
        std::int64_t x1 = 0;
        std::int64_t y1 = 0;
        readOasisGDelta(input, x1, y1);
        std::int64_t x2 = -y1;
        std::int64_t y2 = x1;
        if (type == 8)
            readOasisGDelta(input, x2, y2);
        offsets.clear();
        for (std::uint64_t column = 0; column < columns; ++column)
            for (std::uint64_t row = 0; row < rows; ++row) {
                add(static_cast<std::int64_t>(column) * x1 + static_cast<std::int64_t>(row) * x2,
                    static_cast<std::int64_t>(column) * y1 + static_cast<std::int64_t>(row) * y2);
                limit();
            }
        break;
    }
    case 10:
    case 11: {
        const auto count = 1 + readOasisUnsigned(input);
        const auto grid = type == 11 ? static_cast<Coordinate>(readOasisUnsigned(input)) : 1;
        LayoutPoint offset{0, 0};
        for (std::uint64_t index = 0; index < count; ++index) {
            std::int64_t x = 0;
            std::int64_t y = 0;
            readOasisGDelta(input, x, y);
            offset.x += static_cast<Coordinate>(x) * grid;
            offset.y += static_cast<Coordinate>(y) * grid;
            add(offset.x, offset.y);
            limit();
        }
        break;
    }
    default:
        throw ProjectError("interchange.oasis", "Unsupported OASIS repetition type");
    }
    return offsets;
}

void skipOasisPropertyValue(OasisInput &input) {
    const auto type = input.get();
    switch (type) {
    case 0:
    case 1:
    case 2:
    case 3:
    case 8:
    case 13:
    case 14:
    case 15:
        readOasisUnsigned(input);
        break;
    case 4:
    case 5:
        readOasisUnsigned(input);
        readOasisUnsigned(input);
        break;
    case 6:
        input.skip(4);
        break;
    case 7:
        input.skip(8);
        break;
    case 9:
        readOasisSigned(input);
        break;
    case 10:
    case 11:
    case 12:
        readOasisString(input);
        break;
    default:
        throw ProjectError("interchange.oasis", "Unsupported OASIS property value type");
    }
}

void skipOasisProperty(OasisInput &input, bool implicitModal) {
    std::uint8_t info = 0x08;
    if (!implicitModal)
        info = input.get();
    if (info & 0x04) {
        if (info & 0x02)
            readOasisUnsigned(input);
        else
            readOasisString(input);
    }
    if (info & 0x08)
        return;
    auto count = static_cast<std::uint64_t>(info >> 4);
    if (count == 15)
        count = readOasisUnsigned(input);
    for (std::uint64_t index = 0; index < count; ++index)
        skipOasisPropertyValue(input);
}

struct OasisParseState {
    std::vector<GdsStructure> structures;
    std::vector<int> structureCellIndex;
    std::vector<std::string> cellNames;
    std::vector<std::string> textStrings;
    std::vector<std::string> propertyNames;
    std::vector<std::string> propertyStrings;
    std::size_t current = std::string::npos;
    bool absolute = true;
    std::int64_t layer = 0;
    std::int64_t datatype = 0;
    std::int64_t textLayer = 0;
    std::int64_t textType = 0;
    LayoutPoint placementPosition{};
    LayoutPoint textPosition{};
    LayoutPoint geometryPosition{};
    LayoutPoint geometrySize{};
    Coordinate pathHalfWidth = 0;
    Coordinate pathBeginExtension = 0;
    Coordinate pathEndExtension = 0;
    std::string modalText;
    int modalPlacementIndex = -1;
    std::string modalPlacementName;
    std::vector<LayoutPoint> modalPolygonPoints{{0, 0}};
    std::vector<LayoutPoint> modalPathPoints{{0, 0}};
    double unitNanometers = 1.0;
    const LayoutImportOptions *options = nullptr;
};

GdsStructure &currentOasisStructure(OasisParseState &state) {
    if (state.current == std::string::npos || state.current >= state.structures.size())
        throw ProjectError("interchange.oasis", "OASIS cell content appears outside a CELL record");
    return state.structures[state.current];
}

std::string oasisCellNameAt(const OasisParseState &state, std::uint64_t index) {
    if (index < state.cellNames.size())
        return state.cellNames[index];
    return "#" + std::to_string(index);
}

void parseOasisRecords(OasisInput &input, OasisParseState &state) {
    const auto readPosition = [&](std::uint8_t info, std::uint8_t xBit, std::uint8_t yBit, LayoutPoint &position) {
        if (info & xBit) {
            const auto x = readOasisSigned(input);
            position.x = state.absolute ? x : position.x + x;
        }
        if (info & yBit) {
            const auto y = readOasisSigned(input);
            position.y = state.absolute ? y : position.y + y;
        }
    };
    const auto addShape = [&](GdsShape shape) { currentOasisStructure(state).shapes.push_back(std::move(shape)); };
    const auto appendCopy = [&](const std::vector<LayoutPoint> &base, const LayoutPoint &offset) {
        std::vector<LayoutPoint> points;
        points.reserve(base.size());
        for (const auto &point : base)
            points.push_back({point.x + offset.x, point.y + offset.y});
        return points;
    };
    const auto addReference = [&](const GdsTransform &transform, const LayoutPoint &position, int targetIndex,
                                  const std::string &targetName) {
        GdsReference reference;
        reference.target = targetName;
        reference.targetIndex = targetIndex;
        reference.transform = transform;
        reference.transform.x = position.x;
        reference.transform.y = position.y;
        currentOasisStructure(state).references.push_back(std::move(reference));
    };

    while (!input.eof()) {
        const auto record = input.get();
        switch (record) {
        case 0: // PAD
            break;
        case 1: // START
            throw ProjectError("interchange.oasis", "Unexpected START record inside OASIS body");
        case 2: // END
            return;
        case 3: // CELLNAME_IMPLICIT
            state.cellNames.push_back(readOasisString(input));
            break;
        case 4: { // CELLNAME
            const auto name = readOasisString(input);
            const auto index = readOasisUnsigned(input);
            if (index >= 10000000)
                throw ProjectError("interchange.oasis", "OASIS cell-name table index is too large");
            if (state.cellNames.size() <= index)
                state.cellNames.resize(index + 1);
            state.cellNames[index] = name;
            break;
        }
        case 5: // TEXTSTRING_IMPLICIT
            state.textStrings.push_back(readOasisString(input));
            break;
        case 6: { // TEXTSTRING
            const auto text = readOasisString(input);
            const auto index = readOasisUnsigned(input);
            if (state.textStrings.size() <= index)
                state.textStrings.resize(index + 1);
            state.textStrings[index] = text;
            break;
        }
        case 7: // PROPNAME_IMPLICIT
            state.propertyNames.push_back(readOasisString(input));
            break;
        case 8: { // PROPNAME
            const auto name = readOasisString(input);
            const auto index = readOasisUnsigned(input);
            if (state.propertyNames.size() <= index)
                state.propertyNames.resize(index + 1);
            state.propertyNames[index] = name;
            break;
        }
        case 9: // PROPSTRING_IMPLICIT
            state.propertyStrings.push_back(readOasisString(input));
            break;
        case 10: { // PROPSTRING
            const auto value = readOasisString(input);
            const auto index = readOasisUnsigned(input);
            if (state.propertyStrings.size() <= index)
                state.propertyStrings.resize(index + 1);
            state.propertyStrings[index] = value;
            break;
        }
        case 11: // LAYERNAME_DATA
        case 12: // LAYERNAME_TEXT
            readOasisString(input);
            skipOasisInterval(input);
            skipOasisInterval(input);
            break;
        case 13: { // CELL_REF_NUM
            const auto index = readOasisUnsigned(input);
            state.structures.emplace_back();
            state.current = state.structures.size() - 1;
            state.structures.back().name = oasisCellNameAt(state, index);
            state.structureCellIndex.push_back(static_cast<int>(index));
            state.absolute = true;
            state.placementPosition = {};
            state.geometryPosition = {};
            state.textPosition = {};
            break;
        }
        case 14: { // CELL
            state.structures.emplace_back();
            state.current = state.structures.size() - 1;
            state.structures.back().name = readOasisString(input);
            state.structureCellIndex.push_back(-1);
            state.absolute = true;
            state.placementPosition = {};
            state.geometryPosition = {};
            state.textPosition = {};
            break;
        }
        case 15: // XYABSOLUTE
            state.absolute = true;
            break;
        case 16: // XYRELATIVE
            state.absolute = false;
            break;
        case 17:   // PLACEMENT
        case 18: { // PLACEMENT_TRANSFORM
            const auto info = input.get();
            int targetIndex = -1;
            std::string targetName;
            if (info & 0x80) {
                if (info & 0x40) {
                    targetIndex = static_cast<int>(readOasisUnsigned(input));
                    targetName = oasisCellNameAt(state, static_cast<std::uint64_t>(targetIndex));
                } else {
                    targetName = readOasisString(input);
                }
                state.modalPlacementIndex = targetIndex;
                state.modalPlacementName = targetName;
            } else {
                targetIndex = state.modalPlacementIndex;
                targetName = state.modalPlacementName;
            }
            if (targetName.empty() && targetIndex < 0) {
                addImportWarning(*state.options, "OASIS placement without a target cell was skipped.");
                break;
            }
            GdsTransform transform;
            transform.reflect = (info & 0x01) != 0;
            if (record == 17) {
                switch (info & 0x06) {
                case 0x02:
                    transform.angle = 90.0;
                    break;
                case 0x04:
                    transform.angle = 180.0;
                    break;
                case 0x06:
                    transform.angle = 270.0;
                    break;
                default:
                    break;
                }
            } else {
                if (info & 0x04)
                    transform.magnification = readOasisReal(input);
                if (info & 0x02)
                    transform.angle = readOasisReal(input);
            }
            readPosition(info, 0x20, 0x10, state.placementPosition);
            const auto offsets = (info & 0x08) ? readOasisRepetition(input) : std::vector<LayoutPoint>{{0, 0}};
            for (const auto &offset : offsets) {
                auto position = state.placementPosition;
                position.x += offset.x;
                position.y += offset.y;
                addReference(transform, position, targetIndex, targetName);
            }
            break;
        }
        case 19: { // TEXT
            const auto info = input.get();
            if (info & 0x40) {
                if (info & 0x20) {
                    const auto index = readOasisUnsigned(input);
                    state.modalText = index < state.textStrings.size() ? state.textStrings[index] : std::string{};
                } else {
                    state.modalText = readOasisString(input);
                }
            }
            if (info & 0x01)
                state.textLayer = static_cast<std::int64_t>(readOasisUnsigned(input));
            if (info & 0x02)
                state.textType = static_cast<std::int64_t>(readOasisUnsigned(input));
            readPosition(info, 0x10, 0x08, state.textPosition);
            const auto offsets = (info & 0x04) ? readOasisRepetition(input) : std::vector<LayoutPoint>{{0, 0}};
            for (const auto &offset : offsets) {
                if (state.modalText.empty())
                    continue;
                GdsShape shape;
                shape.kind = GdsShape::Kind::text;
                shape.layer = static_cast<int>(state.textLayer);
                shape.datatype = static_cast<int>(state.textType);
                shape.origin = {state.textPosition.x + offset.x, state.textPosition.y + offset.y};
                shape.text = state.modalText;
                addShape(std::move(shape));
            }
            break;
        }
        case 20: { // RECTANGLE
            const auto info = input.get();
            if (info & 0x01)
                state.layer = static_cast<std::int64_t>(readOasisUnsigned(input));
            if (info & 0x02)
                state.datatype = static_cast<std::int64_t>(readOasisUnsigned(input));
            if (info & 0x40)
                state.geometrySize.x = static_cast<Coordinate>(readOasisUnsigned(input));
            if (info & 0x20)
                state.geometrySize.y = static_cast<Coordinate>(readOasisUnsigned(input));
            else if (info & 0x80)
                state.geometrySize.y = state.geometrySize.x;
            readPosition(info, 0x10, 0x08, state.geometryPosition);
            const auto offsets = (info & 0x04) ? readOasisRepetition(input) : std::vector<LayoutPoint>{{0, 0}};
            for (const auto &offset : offsets) {
                if (state.geometrySize.x <= 0 || state.geometrySize.y <= 0)
                    continue;
                GdsShape shape;
                shape.kind = GdsShape::Kind::box;
                shape.layer = static_cast<int>(state.layer);
                shape.datatype = static_cast<int>(state.datatype);
                const auto x = state.geometryPosition.x + offset.x;
                const auto y = state.geometryPosition.y + offset.y;
                shape.points = {{x, y}, {x + state.geometrySize.x, y + state.geometrySize.y}};
                addShape(std::move(shape));
            }
            break;
        }
        case 21: { // POLYGON
            const auto info = input.get();
            if (info & 0x01)
                state.layer = static_cast<std::int64_t>(readOasisUnsigned(input));
            if (info & 0x02)
                state.datatype = static_cast<std::int64_t>(readOasisUnsigned(input));
            if (info & 0x20)
                state.modalPolygonPoints = readOasisPointList(input, true);
            readPosition(info, 0x10, 0x08, state.geometryPosition);
            const auto offsets = (info & 0x04) ? readOasisRepetition(input) : std::vector<LayoutPoint>{{0, 0}};
            for (const auto &offset : offsets) {
                if (state.modalPolygonPoints.size() < 3)
                    continue;
                GdsShape shape;
                shape.kind = GdsShape::Kind::polygon;
                shape.layer = static_cast<int>(state.layer);
                shape.datatype = static_cast<int>(state.datatype);
                shape.points = appendCopy(state.modalPolygonPoints,
                                          {state.geometryPosition.x + offset.x, state.geometryPosition.y + offset.y});
                addShape(std::move(shape));
            }
            break;
        }
        case 22: { // PATH
            const auto info = input.get();
            if (info & 0x01)
                state.layer = static_cast<std::int64_t>(readOasisUnsigned(input));
            if (info & 0x02)
                state.datatype = static_cast<std::int64_t>(readOasisUnsigned(input));
            if (info & 0x40)
                state.pathHalfWidth = static_cast<Coordinate>(readOasisUnsigned(input));
            if (info & 0x80) {
                const auto scheme = input.get();
                switch ((scheme & 0x0c) >> 2) {
                case 1:
                    state.pathBeginExtension = 0;
                    break;
                case 2:
                    state.pathBeginExtension = state.pathHalfWidth;
                    break;
                case 3:
                    state.pathBeginExtension = static_cast<Coordinate>(readOasisSigned(input));
                    break;
                default:
                    break;
                }
                switch (scheme & 0x03) {
                case 1:
                    state.pathEndExtension = 0;
                    break;
                case 2:
                    state.pathEndExtension = state.pathHalfWidth;
                    break;
                case 3:
                    state.pathEndExtension = static_cast<Coordinate>(readOasisSigned(input));
                    break;
                default:
                    break;
                }
            }
            if (info & 0x20)
                state.modalPathPoints = readOasisPointList(input, false);
            readPosition(info, 0x10, 0x08, state.geometryPosition);
            const auto offsets = (info & 0x04) ? readOasisRepetition(input) : std::vector<LayoutPoint>{{0, 0}};
            for (const auto &offset : offsets) {
                if (state.modalPathPoints.size() < 2 || state.pathHalfWidth <= 0)
                    continue;
                GdsShape shape;
                shape.kind = GdsShape::Kind::path;
                shape.layer = static_cast<int>(state.layer);
                shape.datatype = static_cast<int>(state.datatype);
                shape.centerline = appendCopy(
                    state.modalPathPoints, {state.geometryPosition.x + offset.x, state.geometryPosition.y + offset.y});
                shape.width = 2 * state.pathHalfWidth;
                if (state.pathBeginExtension == 0 && state.pathEndExtension == 0)
                    shape.pathtype = 0;
                else if (state.pathBeginExtension == state.pathHalfWidth &&
                         state.pathEndExtension == state.pathHalfWidth)
                    shape.pathtype = 2;
                else {
                    shape.pathtype = 4;
                    shape.beginExtension = state.pathBeginExtension;
                    shape.endExtension = state.pathEndExtension;
                }
                addShape(std::move(shape));
            }
            break;
        }
        case 23:   // TRAPEZOID_AB
        case 24:   // TRAPEZOID_A
        case 25: { // TRAPEZOID_B
            const auto info = input.get();
            if (info & 0x01)
                state.layer = static_cast<std::int64_t>(readOasisUnsigned(input));
            if (info & 0x02)
                state.datatype = static_cast<std::int64_t>(readOasisUnsigned(input));
            if (info & 0x40)
                state.geometrySize.x = static_cast<Coordinate>(readOasisUnsigned(input));
            if (info & 0x20)
                state.geometrySize.y = static_cast<Coordinate>(readOasisUnsigned(input));
            std::int64_t deltaA = 0;
            std::int64_t deltaB = 0;
            if (record == 23) {
                deltaA = readOasisSigned(input);
                deltaB = readOasisSigned(input);
            } else if (record == 24) {
                deltaA = readOasisSigned(input);
            } else {
                deltaB = readOasisSigned(input);
            }
            readPosition(info, 0x10, 0x08, state.geometryPosition);
            const auto offsets = (info & 0x04) ? readOasisRepetition(input) : std::vector<LayoutPoint>{{0, 0}};
            const auto width = state.geometrySize.x;
            const auto height = state.geometrySize.y;
            const auto a = static_cast<Coordinate>(deltaA);
            const auto b = static_cast<Coordinate>(deltaB);
            LayoutPoint r{};
            LayoutPoint s{};
            LayoutPoint q{};
            LayoutPoint p{};
            if (info & 0x80) {
                p.x = q.x = 0;
                r.x = s.x = width;
                if (a < 0) {
                    p.y = 0;
                    r.y = p.y - a;
                } else {
                    r.y = 0;
                    p.y = r.y + a;
                }
                if (b < 0) {
                    s.y = height;
                    q.y = s.y + b;
                } else {
                    q.y = height;
                    s.y = q.y - b;
                }
            } else {
                p.y = q.y = height;
                r.y = s.y = 0;
                if (a < 0) {
                    p.x = 0;
                    r.x = p.x - a;
                } else {
                    r.x = 0;
                    p.x = r.x + a;
                }
                if (b < 0) {
                    s.x = width;
                    q.x = s.x + b;
                } else {
                    q.x = width;
                    s.x = q.x - b;
                }
            }
            for (const auto &offset : offsets) {
                if (width <= 0 || height <= 0)
                    continue;
                const auto x = state.geometryPosition.x + offset.x;
                const auto y = state.geometryPosition.y + offset.y;
                GdsShape shape;
                shape.kind = GdsShape::Kind::polygon;
                shape.layer = static_cast<int>(state.layer);
                shape.datatype = static_cast<int>(state.datatype);
                shape.points = {{x + r.x, y + r.y}, {x + s.x, y + s.y}, {x + q.x, y + q.y}, {x + p.x, y + p.y}};
                addShape(std::move(shape));
            }
            break;
        }
        case 26: { // CTRAPEZOID
            const auto info = input.get();
            if (info & 0x01)
                state.layer = static_cast<std::int64_t>(readOasisUnsigned(input));
            if (info & 0x02)
                state.datatype = static_cast<std::int64_t>(readOasisUnsigned(input));
            auto type = 0;
            if (info & 0x80)
                type = input.get();
            if (info & 0x40)
                state.geometrySize.x = static_cast<Coordinate>(readOasisUnsigned(input));
            if (info & 0x20)
                state.geometrySize.y = static_cast<Coordinate>(readOasisUnsigned(input));
            readPosition(info, 0x10, 0x08, state.geometryPosition);
            const auto offsets = (info & 0x04) ? readOasisRepetition(input) : std::vector<LayoutPoint>{{0, 0}};
            auto dimensionX = state.geometrySize.x;
            auto dimensionY = state.geometrySize.y;
            const auto triangle = type > 15 && type < 24;
            std::vector<LayoutPoint> templatePoints;
            if (triangle)
                templatePoints = {{0, 0}, {0, 0}, {0, 0}};
            else
                templatePoints = {{0, 0}, {dimensionX, 0}, {dimensionX, dimensionY}, {0, dimensionY}};
            const auto addScalar = [](LayoutPoint &point, Coordinate value) {
                point.x += value;
                point.y += value;
            };
            switch (type) {
            case 0:
                templatePoints[2].x -= dimensionY;
                break;
            case 1:
                templatePoints[1].x -= dimensionY;
                break;
            case 2:
                templatePoints[3].x += dimensionY;
                break;
            case 3:
                templatePoints[0].x += dimensionY;
                break;
            case 4:
                templatePoints[2].x -= dimensionY;
                templatePoints[3].x += dimensionY;
                break;
            case 5:
                templatePoints[0].x += dimensionY;
                templatePoints[1].x -= dimensionY;
                break;
            case 6:
                templatePoints[1].x -= dimensionY;
                templatePoints[3].x += dimensionY;
                break;
            case 7:
                templatePoints[0].x += dimensionY;
                templatePoints[2].x -= dimensionY;
                break;
            case 8:
                templatePoints[2].y -= dimensionX;
                break;
            case 9:
                templatePoints[3].y -= dimensionX;
                break;
            case 10:
                templatePoints[1].y += dimensionX;
                break;
            case 11:
                templatePoints[0].y += dimensionX;
                break;
            case 12:
                templatePoints[1].y += dimensionX;
                templatePoints[2].y -= dimensionX;
                break;
            case 13:
                templatePoints[0].y += dimensionX;
                templatePoints[3].y -= dimensionX;
                break;
            case 14:
                templatePoints[1].y += dimensionX;
                templatePoints[3].y -= dimensionX;
                break;
            case 15:
                templatePoints[0].y += dimensionX;
                templatePoints[2].y -= dimensionX;
                break;
            case 16:
                templatePoints[1].x += dimensionX;
                templatePoints[2].y += dimensionX;
                dimensionY = dimensionX;
                break;
            case 17:
                addScalar(templatePoints[1], dimensionX);
                templatePoints[2].y += dimensionX;
                dimensionY = dimensionX;
                break;
            case 18:
                templatePoints[1].x += dimensionX;
                addScalar(templatePoints[2], dimensionX);
                dimensionY = dimensionX;
                break;
            case 19:
                templatePoints[0].x += dimensionX;
                addScalar(templatePoints[1], dimensionX);
                templatePoints[2].y += dimensionX;
                dimensionY = dimensionX;
                break;
            case 20:
                templatePoints[1].x += 2 * dimensionY;
                addScalar(templatePoints[2], dimensionY);
                dimensionX = 2 * dimensionY;
                break;
            case 21:
                templatePoints[0].x += dimensionY;
                templatePoints[1].x += 2 * dimensionY;
                templatePoints[1].y += dimensionY;
                templatePoints[2].y += dimensionY;
                dimensionX = 2 * dimensionY;
                break;
            case 22:
                addScalar(templatePoints[1], dimensionX);
                templatePoints[2].y += 2 * dimensionX;
                dimensionY = 2 * dimensionX;
                break;
            case 23:
                templatePoints[0].x += dimensionX;
                templatePoints[1].x += dimensionX;
                templatePoints[1].y += 2 * dimensionX;
                templatePoints[2].y += dimensionX;
                dimensionY = 2 * dimensionX;
                break;
            case 25:
                templatePoints[2].y = templatePoints[3].y = dimensionX;
                break;
            default:
                if (type > 25)
                    addImportWarning(*state.options, "OASIS compact trapezoid type " + std::to_string(type) +
                                                         " was approximated by its bounding box.");
                break;
            }
            state.geometrySize.x = dimensionX;
            state.geometrySize.y = dimensionY;
            for (const auto &offset : offsets) {
                const auto x = state.geometryPosition.x + offset.x;
                const auto y = state.geometryPosition.y + offset.y;
                if (dimensionX <= 0 || dimensionY <= 0)
                    continue;
                GdsShape shape;
                shape.kind = GdsShape::Kind::polygon;
                shape.layer = static_cast<int>(state.layer);
                shape.datatype = static_cast<int>(state.datatype);
                shape.points.reserve(templatePoints.size());
                for (const auto &point : templatePoints)
                    shape.points.push_back({x + point.x, y + point.y});
                addShape(std::move(shape));
            }
            break;
        }
        case 27: { // CIRCLE
            const auto info = input.get();
            if (info & 0x01)
                state.layer = static_cast<std::int64_t>(readOasisUnsigned(input));
            if (info & 0x02)
                state.datatype = static_cast<std::int64_t>(readOasisUnsigned(input));
            auto radius = 0.0;
            if (info & 0x20)
                radius = static_cast<double>(readOasisUnsigned(input));
            readPosition(info, 0x10, 0x08, state.geometryPosition);
            const auto offsets = (info & 0x04) ? readOasisRepetition(input) : std::vector<LayoutPoint>{{0, 0}};
            for (const auto &offset : offsets) {
                if (radius <= 0.0)
                    continue;
                GdsShape shape;
                shape.kind = GdsShape::Kind::polygon;
                shape.layer = static_cast<int>(state.layer);
                shape.datatype = static_cast<int>(state.datatype);
                const auto centerX = state.geometryPosition.x + offset.x;
                const auto centerY = state.geometryPosition.y + offset.y;
                constexpr int segments = 32;
                for (int index = 0; index < segments; ++index) {
                    const auto angle = 2.0 * 3.14159265358979323846 * index / segments;
                    shape.points.push_back({centerX + static_cast<Coordinate>(std::llround(radius * std::cos(angle))),
                                            centerY + static_cast<Coordinate>(std::llround(radius * std::sin(angle)))});
                }
                addShape(std::move(shape));
            }
            break;
        }
        case 28: // PROPERTY
            skipOasisProperty(input, false);
            break;
        case 29: // LAST_PROPERTY
            skipOasisProperty(input, true);
            break;
        case 30: // XNAME_IMPLICIT
            readOasisUnsigned(input);
            readOasisString(input);
            break;
        case 31: // XNAME
            readOasisUnsigned(input);
            readOasisString(input);
            readOasisUnsigned(input);
            break;
        case 32: // XELEMENT
            readOasisUnsigned(input);
            readOasisString(input);
            break;
        case 33: { // XGEOMETRY
            const auto info = input.get();
            readOasisUnsigned(input);
            if (info & 0x01)
                state.layer = static_cast<std::int64_t>(readOasisUnsigned(input));
            if (info & 0x02)
                state.datatype = static_cast<std::int64_t>(readOasisUnsigned(input));
            readOasisString(input);
            readPosition(info, 0x10, 0x08, state.geometryPosition);
            if (info & 0x04)
                readOasisRepetition(input);
            break;
        }
        case 34: { // CBLOCK
            const auto compression = readOasisUnsigned(input);
            const auto uncompressed = readOasisUnsigned(input);
            const auto compressed = readOasisUnsigned(input);
            if (compressed > 1000000000)
                throw ProjectError("interchange.oasis", "OASIS CBLOCK is too large");
            std::vector<unsigned char> compressedBytes(compressed);
            if (compressed > 0)
                input.read(compressedBytes.data(), compressed);
            if (compression != 0) {
                addImportWarning(*state.options,
                                 "OASIS CBLOCK compression method " + std::to_string(compression) + " was skipped.");
                break;
            }
            if (uncompressed > 1000000000)
                throw ProjectError("interchange.oasis", "OASIS CBLOCK expands beyond the size limit");
            std::vector<unsigned char> expanded(uncompressed);
            z_stream stream{};
            stream.next_in = compressedBytes.data();
            stream.avail_in = static_cast<uInt>(compressedBytes.size());
            stream.next_out = expanded.data();
            stream.avail_out = static_cast<uInt>(expanded.size());
            if (inflateInit2(&stream, -15) != Z_OK)
                throw ProjectError("interchange.oasis", "Unable to initialize OASIS CBLOCK decompression");
            const auto status = inflate(&stream, Z_FINISH);
            inflateEnd(&stream);
            if (status != Z_STREAM_END)
                throw ProjectError("interchange.oasis", "Unable to decompress OASIS CBLOCK");
            OasisInput nested{&expanded, 0};
            parseOasisRecords(nested, state);
            break;
        }
        default:
            throw ProjectError("interchange.oasis", "Unsupported OASIS record " + std::to_string(record));
        }
    }
}

std::vector<unsigned char> readOasisFile(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw ProjectError("interchange.oasis", "Unable to open OASIS input");
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

Project importOasisNative(const std::filesystem::path &path, const LayoutImportOptions &options) {
    const auto bytes = readOasisFile(path);
    constexpr char magic[] = "%SEMI-OASIS\r\n";
    if (bytes.size() < 14 || std::memcmp(bytes.data(), magic, 13) != 0 || bytes[13] != 1)
        throw ProjectError("interchange.oasis", "Invalid OASIS header");
    OasisInput input{&bytes, 14};
    OasisParseState state;
    state.options = &options;
    const auto version = readOasisString(input);
    if (version != "1.0")
        addImportWarning(options, "OASIS version '" + version + "' was read as OASIS 1.0.");
    const auto unitReal = readOasisReal(input);
    state.unitNanometers = unitReal > 0.0 ? 1000.0 / unitReal : 1.0;
    const auto offsetTable = readOasisUnsigned(input);
    if (offsetTable == 0) {
        for (int index = 0; index < 12; ++index)
            readOasisUnsigned(input);
    }
    parseOasisRecords(input, state);
    if (state.structures.empty())
        throw ProjectError("interchange.oasis", "OASIS file contains no cells");
    for (std::size_t index = 0; index < state.structures.size(); ++index) {
        const auto cellIndex = state.structureCellIndex[index];
        if (cellIndex >= 0 && static_cast<std::size_t>(cellIndex) < state.cellNames.size())
            state.structures[index].name = state.cellNames[cellIndex];
        if (state.structures[index].name.empty())
            state.structures[index].name = "CELL_" + std::to_string(index + 1);
        for (auto &reference : state.structures[index].references) {
            if (reference.targetIndex >= 0) {
                if (static_cast<std::size_t>(reference.targetIndex) < state.cellNames.size())
                    reference.target = state.cellNames[reference.targetIndex];
                else
                    reference.target = "#" + std::to_string(reference.targetIndex);
            }
        }
    }
    GdsDocument document;
    document.structures = std::move(state.structures);
    document.unitNanometers = state.unitNanometers;
    return buildGdsProject(path, document, options);
}

void oasisPut(std::vector<unsigned char> &output, unsigned char value) { output.push_back(value); }

void oasisWriteUnsigned(std::vector<unsigned char> &output, std::uint64_t value) {
    while (value >= 0x80) {
        output.push_back(static_cast<unsigned char>((value & 0x7f) | 0x80));
        value >>= 7;
    }
    output.push_back(static_cast<unsigned char>(value));
}

std::size_t oasisUnsignedLength(std::uint64_t value) {
    std::size_t length = 1;
    while (value >= 0x80) {
        value >>= 7;
        ++length;
    }
    return length;
}

void oasisWriteIntInternal(std::vector<unsigned char> &output, std::uint64_t value, int numBits, std::uint8_t bits) {
    auto first = static_cast<std::uint8_t>(bits | ((value & ((1ULL << (7 - numBits)) - 1)) << numBits));
    value >>= 7 - numBits;
    if (value == 0) {
        output.push_back(first);
        return;
    }
    output.push_back(static_cast<unsigned char>(first | 0x80));
    while (value > 0) {
        auto byte = static_cast<std::uint8_t>(value & 0x7f);
        value >>= 7;
        if (value > 0)
            byte |= 0x80;
        output.push_back(byte);
    }
}

void oasisWriteSigned(std::vector<unsigned char> &output, std::int64_t value) {
    if (value < 0)
        oasisWriteIntInternal(output, static_cast<std::uint64_t>(-value), 1, 1);
    else
        oasisWriteIntInternal(output, static_cast<std::uint64_t>(value), 1, 0);
}

void oasisWriteGDelta(std::vector<unsigned char> &output, std::int64_t x, std::int64_t y) {
    if (x == 0 && y == 0) {
        oasisWriteIntInternal(output, 0, 4, 1 << 1);
        return;
    }
    if (x == 0) {
        oasisWriteIntInternal(output, static_cast<std::uint64_t>(y < 0 ? -y : y), 4,
                              static_cast<std::uint8_t>((y < 0 ? 3 : 1) << 1));
        return;
    }
    if (y == 0) {
        oasisWriteIntInternal(output, static_cast<std::uint64_t>(x < 0 ? -x : x), 4,
                              static_cast<std::uint8_t>((x < 0 ? 2 : 0) << 1));
        return;
    }
    if (x == y) {
        oasisWriteIntInternal(output, static_cast<std::uint64_t>(x < 0 ? -x : x), 4,
                              static_cast<std::uint8_t>((x < 0 ? 6 : 4) << 1));
        return;
    }
    if (x == -y) {
        oasisWriteIntInternal(output, static_cast<std::uint64_t>(x < 0 ? -x : x), 4,
                              static_cast<std::uint8_t>((x < 0 ? 5 : 7) << 1));
        return;
    }
    if (x < 0)
        oasisWriteIntInternal(output, static_cast<std::uint64_t>(-x), 2, 3);
    else
        oasisWriteIntInternal(output, static_cast<std::uint64_t>(x), 2, 1);
    if (y < 0)
        oasisWriteIntInternal(output, static_cast<std::uint64_t>(-y), 1, 1);
    else
        oasisWriteIntInternal(output, static_cast<std::uint64_t>(y), 1, 0);
}

void oasisWriteReal(std::vector<unsigned char> &output, double value) {
    if (std::isfinite(value) && value >= 0.0 && value == std::floor(value) && value <= 9007199254740992.0) {
        oasisPut(output, 0);
        oasisWriteUnsigned(output, static_cast<std::uint64_t>(value));
        return;
    }
    if (std::isfinite(value) && value < 0.0 && -value == std::floor(-value) && -value <= 9007199254740992.0) {
        oasisPut(output, 1);
        oasisWriteUnsigned(output, static_cast<std::uint64_t>(-value));
        return;
    }
    oasisPut(output, 7);
    std::uint64_t raw = 0;
    std::memcpy(&raw, &value, sizeof(value));
    for (int index = 0; index < 8; ++index)
        output.push_back(static_cast<unsigned char>((raw >> (8 * index)) & 0xff));
}

void oasisWriteString(std::vector<unsigned char> &output, const std::string &value) {
    oasisWriteUnsigned(output, value.size());
    output.insert(output.end(), value.begin(), value.end());
}

void oasisWritePointList(std::vector<unsigned char> &output, std::vector<LayoutPoint> points) {
    if (points.empty())
        return;
    if (points.size() > 1 && points.front().x == points.back().x && points.front().y == points.back().y)
        points.pop_back();
    if (points.empty())
        return;
    oasisPut(output, 4);
    oasisWriteUnsigned(output, points.size() - 1);
    for (std::size_t index = 1; index < points.size(); ++index)
        oasisWriteGDelta(output, points[index].x - points[index - 1].x, points[index].y - points[index - 1].y);
}

void writeOasisShape(std::vector<unsigned char> &output, const Project &project, const Shape &shape) {
    const auto writeRectangle = [&](int layer, int datatype, const Rect &rectangle) {
        if (rectangle.width <= 0 || rectangle.height <= 0)
            return;
        const auto square = rectangle.width == rectangle.height;
        oasisPut(output, 20);
        oasisPut(output, static_cast<unsigned char>(square ? 0xDB : 0x7B));
        oasisWriteUnsigned(output, static_cast<std::uint64_t>(layer));
        oasisWriteUnsigned(output, static_cast<std::uint64_t>(datatype));
        oasisWriteUnsigned(output, static_cast<std::uint64_t>(rectangle.width));
        if (!square)
            oasisWriteUnsigned(output, static_cast<std::uint64_t>(rectangle.height));
        oasisWriteSigned(output, rectangle.x);
        oasisWriteSigned(output, rectangle.y);
    };
    if (shape.isText()) {
        const auto [layer, datatype] = layerStream(project, shape.layer);
        oasisPut(output, 19);
        oasisPut(output, 0x5B);
        oasisWriteString(output, shape.text);
        oasisWriteUnsigned(output, static_cast<std::uint64_t>(layer));
        oasisWriteUnsigned(output, static_cast<std::uint64_t>(datatype));
        oasisWriteSigned(output, shape.rectangle.x);
        oasisWriteSigned(output, shape.rectangle.y);
        return;
    }
    if (shape.isPath()) {
        const auto simple = shape.segmentLayers.empty() && shape.segmentWidths.empty() &&
                            std::all_of(shape.pointVias.begin(), shape.pointVias.end(),
                                        [](const std::vector<ObjectId> &vias) { return vias.empty(); });
        if (simple && shape.centerline.size() >= 2 && shape.width > 0) {
            const auto [layer, datatype] = layerStream(project, shape.layer);
            oasisPut(output, 22);
            oasisPut(output, 0xFB);
            oasisWriteUnsigned(output, static_cast<std::uint64_t>(layer));
            oasisWriteUnsigned(output, static_cast<std::uint64_t>(datatype));
            oasisWriteUnsigned(output, static_cast<std::uint64_t>(std::max<Coordinate>(1, shape.width / 2)));
            oasisPut(output, 0x05);
            oasisWritePointList(output, shape.centerline);
            oasisWriteSigned(output, shape.centerline.front().x);
            oasisWriteSigned(output, shape.centerline.front().y);
            return;
        }
        for (std::size_t index = 0; index + 1 < shape.centerline.size(); ++index) {
            const auto layerId = shape.layerForSegment(index);
            const auto [layer, datatype] = layerStream(project, layerId);
            const auto width = std::max<Coordinate>(1, shape.widthForSegment(index));
            oasisPut(output, 22);
            oasisPut(output, 0xFB);
            oasisWriteUnsigned(output, static_cast<std::uint64_t>(layer));
            oasisWriteUnsigned(output, static_cast<std::uint64_t>(datatype));
            oasisWriteUnsigned(output, static_cast<std::uint64_t>(std::max<Coordinate>(1, width / 2)));
            oasisPut(output, 0x05);
            oasisWritePointList(output, {shape.centerline[index], shape.centerline[index + 1]});
            oasisWriteSigned(output, shape.centerline[index].x);
            oasisWriteSigned(output, shape.centerline[index].y);
        }
        for (std::size_t index = 0; index < shape.pointVias.size() && index < shape.centerline.size(); ++index) {
            auto half = std::max<Coordinate>(1, shape.width / 2);
            if (index > 0)
                half = std::max(half, shape.widthForSegment(index - 1) / 2);
            if (index + 1 < shape.centerline.size())
                half = std::max(half, shape.widthForSegment(index) / 2);
            for (const auto viaLayer : shape.pointVias[index]) {
                const auto [layer, datatype] = layerStream(project, viaLayer);
                writeRectangle(
                    layer, datatype,
                    {shape.centerline[index].x - half, shape.centerline[index].y - half, 2 * half, 2 * half});
            }
        }
        return;
    }
    const auto [layer, datatype] = layerStream(project, shape.layer);
    if (shape.polygon.empty() && shape.rectangle.valid()) {
        writeRectangle(layer, datatype, shape.rectangle);
        return;
    }
    auto polygon = shape.polygon;
    if (polygon.empty()) {
        const auto x = shape.rectangle.x;
        const auto y = shape.rectangle.y;
        const auto right = x + shape.rectangle.width;
        const auto top = y + shape.rectangle.height;
        polygon = {{x, y}, {right, y}, {right, top}, {x, top}};
    }
    if (polygon.size() < 3)
        return;
    oasisPut(output, 21);
    oasisPut(output, 0x3B);
    oasisWriteUnsigned(output, static_cast<std::uint64_t>(layer));
    oasisWriteUnsigned(output, static_cast<std::uint64_t>(datatype));
    oasisWritePointList(output, polygon);
    oasisWriteSigned(output, polygon.front().x);
    oasisWriteSigned(output, polygon.front().y);
}

void writeOasisPort(std::vector<unsigned char> &output, const Project &project, const Port &port) {
    if (!port.visible)
        return;
    const auto [layer, datatype] = layerStream(project, port.layer);
    const auto square = port.rectangle.width == port.rectangle.height;
    oasisPut(output, 20);
    oasisPut(output, static_cast<unsigned char>(square ? 0xDB : 0x7B));
    oasisWriteUnsigned(output, static_cast<std::uint64_t>(layer));
    oasisWriteUnsigned(output, static_cast<std::uint64_t>(datatype));
    oasisWriteUnsigned(output, static_cast<std::uint64_t>(port.rectangle.width));
    if (!square)
        oasisWriteUnsigned(output, static_cast<std::uint64_t>(port.rectangle.height));
    oasisWriteSigned(output, port.rectangle.x);
    oasisWriteSigned(output, port.rectangle.y);
    oasisPut(output, 19);
    oasisPut(output, 0x5B);
    oasisWriteString(output, port.label.empty() ? port.name : port.label);
    oasisWriteUnsigned(output, static_cast<std::uint64_t>(layer));
    oasisWriteUnsigned(output, static_cast<std::uint64_t>(datatype));
    oasisWriteSigned(output, port.rectangle.x + port.rectangle.width / 2);
    oasisWriteSigned(output, port.rectangle.y + port.rectangle.height / 2);
}

void writeOasisProject(const Project &project, ObjectId cellId, const std::filesystem::path &path) {
    if (!project.layoutCells.contains(cellId))
        throw ProjectError("interchange.cell", "Layout cell does not exist");
    std::vector<ObjectId> order;
    std::set<ObjectId> visited;
    collectStructureOrder(project, cellId, order, visited);
    std::map<ObjectId, std::size_t> index;
    std::set<std::string> usedNames;
    std::vector<std::string> names;
    for (std::size_t position = 0; position < order.size(); ++position) {
        index[order[position]] = position;
        const auto &cell = project.layoutCells.at(order[position]);
        auto base = cell.name.empty() ? "CELL" : cell.name;
        auto name = base;
        for (int suffix = 2; !usedNames.insert(name).second; ++suffix)
            name = base + " " + std::to_string(suffix);
        names.push_back(std::move(name));
    }
    std::vector<unsigned char> output;
    constexpr char magic[] = "%SEMI-OASIS\r\n";
    output.insert(output.end(), magic, magic + 13);
    oasisPut(output, 1);
    oasisWriteString(output, "1.0");
    oasisWriteReal(output, 1000.0 / projectNanometersPerUnit(project));
    // Offset table is stored in the END record.
    oasisWriteUnsigned(output, 1);
    for (std::size_t position = 0; position < order.size(); ++position) {
        const auto &cell = project.layoutCells.at(order[position]);
        oasisPut(output, 13);
        oasisWriteUnsigned(output, position);
        for (const auto &[_, shape] : cell.shapes)
            writeOasisShape(output, project, shape);
        for (const auto &[_, port] : cell.ports)
            writeOasisPort(output, project, port);
        for (const auto &[_, instance] : cell.instances) {
            const auto target = index.find(instance.cell);
            if (target == index.end())
                continue;
            bool reflect = false;
            int angleCode = 0;
            switch (instance.orientation) {
            case Orientation::r0:
                break;
            case Orientation::r90:
                angleCode = 1;
                break;
            case Orientation::r180:
                angleCode = 2;
                break;
            case Orientation::r270:
                angleCode = 3;
                break;
            case Orientation::mx:
                reflect = true;
                break;
            case Orientation::my:
                reflect = true;
                angleCode = 2;
                break;
            case Orientation::m45:
                reflect = true;
                angleCode = 1;
                break;
            case Orientation::m135:
                reflect = true;
                angleCode = 3;
                break;
            }
            oasisPut(output, 17);
            oasisPut(output, static_cast<unsigned char>(0xF0 | (angleCode << 1) | (reflect ? 0x01 : 0x00)));
            oasisWriteUnsigned(output, target->second);
            oasisWriteSigned(output, instance.x);
            oasisWriteSigned(output, instance.y);
        }
    }
    const auto cellNameOffset = output.size();
    for (const auto &name : names) {
        oasisPut(output, 3);
        oasisWriteString(output, name);
    }
    const auto endPosition = output.size();
    oasisPut(output, 2);
    // Offset table: cell names, text strings, property names, property
    // strings, layer names, and XNAMEs. Only the cell-name table is present.
    oasisPut(output, 1);
    oasisWriteUnsigned(output, cellNameOffset);
    for (int table = 0; table < 5; ++table) {
        oasisPut(output, 1);
        oasisWriteUnsigned(output, 0);
    }
    // OASIS readers expect at least 254 bytes after the END record.
    std::size_t padding = 0;
    while (output.size() + oasisUnsignedLength(padding) + padding + 1 < endPosition + 256)
        ++padding;
    oasisWriteUnsigned(output, padding);
    output.insert(output.end(), padding, 0);
    oasisPut(output, 0);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
        throw ProjectError("interchange.oasis", "Unable to open OASIS output");
    file.write(reinterpret_cast<const char *>(output.data()), static_cast<std::streamsize>(output.size()));
    if (!file)
        throw ProjectError("interchange.oasis", "Unable to write OASIS output");
}

void exportOasisNative(const Project &project, ObjectId cellId, const std::filesystem::path &path, bool flatten) {
    if (!flatten) {
        writeOasisProject(project, cellId, path);
        return;
    }
    const auto flattened = LayoutInterchange::flattenProject(project, cellId);
    writeOasisProject(flattened, cellId, path);
}

Project LayoutInterchange::importOasis(const std::filesystem::path &path, const LayoutImportOptions &options) {
    return importOasisNative(path, options);
}

Project LayoutInterchange::importStream(const std::filesystem::path &path, const LayoutImportOptions &options) {
    auto extension = lowerCopy(path.extension().string());
    if (extension == ".gds" || extension == ".gdsii")
        return importGds(path, options);
    if (extension == ".oas" || extension == ".oasis")
        return importOasis(path, options);
    throw ProjectError("interchange.format", "Unsupported layout format: " + path.string());
}

std::string LayoutInterchange::exportStream(const Project &project, ObjectId cellId, const std::filesystem::path &path,
                                            bool flatten) {
    const auto cell = project.layoutCells.find(cellId);
    if (cell == project.layoutCells.end())
        throw ProjectError("interchange.cell", "Layout cell does not exist");
    auto extension = lowerCopy(path.extension().string());
    if (extension == ".gds" || extension == ".gdsii") {
        if (flatten) {
            exportFlattenedGds(project, cellId, path);
            return "TOP";
        }
        exportGds(project, cellId, path);
        return gdsStructureName(cell->second.name);
    }
    if (extension == ".oas" || extension == ".oasis") {
        exportOasisNative(project, cellId, path, flatten);
        return cell->second.name;
    }
    throw ProjectError("interchange.format", "Unsupported layout format: " + path.string());
}

} // namespace ice
