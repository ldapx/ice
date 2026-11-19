#include "icelib/procedural_layout.hpp"

#include "icelib/geometry.hpp"
#include "icelib/operations.hpp"
#include "icelib/packages.hpp"
#include "icelib/pdk.hpp"
#include "icelib/process.hpp"
#include "icelib/simulation_report.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace ice {
namespace {

using json = nlohmann::ordered_json;

struct ParsedLayer {
    std::string name;
    std::optional<int> number;
    std::optional<int> datatype;
};

struct ParsedShape {
    std::string layer;
    std::optional<Rect> rectangle;
    std::vector<LayoutPoint> polygon;
    std::vector<std::vector<LayoutPoint>> holes;
    std::vector<LayoutPoint> centerline;
    Coordinate width = 0;
    std::string text;
    std::string name;
    std::string net;
    std::string booleanOperation;
    std::vector<ParsedShape> operands;
    bool booleanOperand = false;
};

struct ParsedPort {
    std::string name;
    std::string layer;
    Rect rectangle;
    PortDirection direction = PortDirection::inout;
    PortSide side = PortSide::left;
    std::string net;
    std::string label;
    bool visible = true;
    bool locked = false;
    bool internal = false;
};

struct ParsedInstance {
    std::string cell;
    std::string name;
    Coordinate x = 0;
    Coordinate y = 0;
    Orientation orientation = Orientation::r0;
};

struct ParsedParameter {
    std::string name;
    std::string value;
};

struct ParsedDevicePort {
    std::string name;
    std::string net;
    Rect rectangle;
};

struct ParsedDevice {
    std::string name;
    std::string device;
    std::string model;
    std::string type;
    Rect bounds;
    std::vector<ParsedParameter> parameters;
    std::vector<ParsedDevicePort> ports;
};

struct ParsedLayout {
    std::vector<ParsedLayer> layers;
    std::vector<ParsedShape> shapes;
    std::vector<ParsedPort> ports;
    std::vector<ParsedInstance> instances;
    std::vector<ParsedParameter> parameters;
    std::vector<ParsedDevice> devices;
    // Optional structured agent metadata emitted by the procedural runner.
    // Stored as JSON text so the core layout model remains unchanged.
    std::string layoutQuality;
    std::string layoutPlan;
    std::string layoutReview;
};

struct LayerDefaults {
    int number = 0;
    int datatype = 0;
    Coordinate minimumWidth = 0;
};

bool isRegularFile(const std::filesystem::path &path) {
    std::error_code status;
    return std::filesystem::is_regular_file(path, status) && !status;
}

std::filesystem::path normalizedExecutable(const std::filesystem::path &path) {
    if (path.empty())
        return {};
    std::error_code status;
    auto absolute = std::filesystem::absolute(path, status);
    if (status)
        return path;
    auto canonical = std::filesystem::weakly_canonical(absolute, status);
    return status ? absolute : canonical;
}

std::string proceduralPythonExecutable() {
    if (const char *python = std::getenv("ICE_PYTHON"); python && *python)
        return python;
#ifdef _WIN32
    return "python";
#else
    return "python3";
#endif
}

std::string hashText(const std::string &text) {
    constexpr std::uint64_t offset = 1469598103934665603ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    std::uint64_t hash = offset;
    for (const unsigned char character : text) {
        hash ^= character;
        hash *= prime;
    }
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}

std::string readTextFile(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    return std::string((std::istreambuf_iterator<char>(input)), {});
}

std::string stringField(const json &object, const char *key, std::string fallback = {}, std::string *error = nullptr) {
    const auto field = object.find(key);
    if (field == object.end() || field->is_null())
        return fallback;
    if (!field->is_string()) {
        if (error)
            *error = std::string("Field '") + key + "' must be a string.";
        return fallback;
    }
    return field->get<std::string>();
}

bool parseCoordinate(const json &value, const std::string &label, Coordinate &result, std::string *error) {
    if (!value.is_number()) {
        if (error)
            *error = label + " must be a number";
        return false;
    }
    const auto number = value.get<double>();
    if (!std::isfinite(number) || number < static_cast<double>(std::numeric_limits<Coordinate>::lowest()) ||
        number > static_cast<double>(std::numeric_limits<Coordinate>::max())) {
        if (error)
            *error = label + " is outside the supported coordinate range";
        return false;
    }
    result = static_cast<Coordinate>(std::llround(number));
    return true;
}

bool parsePoint(const json &value, const std::string &label, LayoutPoint &point, std::string *error) {
    if (!value.is_object()) {
        if (error)
            *error = label + " must be an object";
        return false;
    }
    return parseCoordinate(value.at("x"), label + " x", point.x, error) &&
           parseCoordinate(value.at("y"), label + " y", point.y, error);
}

bool parseRect(const json &value, const std::string &label, Rect &rect, std::string *error) {
    if (!value.is_object()) {
        if (error)
            *error = label + " must be an object";
        return false;
    }
    if (!parseCoordinate(value.at("x"), label + " x", rect.x, error) ||
        !parseCoordinate(value.at("y"), label + " y", rect.y, error) ||
        !parseCoordinate(value.at("width"), label + " width", rect.width, error) ||
        !parseCoordinate(value.at("height"), label + " height", rect.height, error))
        return false;
    if (!rect.valid()) {
        if (error)
            *error = label + " must have positive width and height";
        return false;
    }
    return true;
}

bool parseOrientation(const std::string &value, Orientation &orientation, std::string *error) {
    static const std::map<std::string, Orientation> values = {
        {"r0", Orientation::r0}, {"r90", Orientation::r90}, {"r180", Orientation::r180}, {"r270", Orientation::r270},
        {"mx", Orientation::mx}, {"my", Orientation::my},   {"m45", Orientation::m45},   {"m135", Orientation::m135},
    };
    const auto it = values.find(value);
    if (it == values.end()) {
        if (error)
            *error = "Unsupported orientation: " + value;
        return false;
    }
    orientation = it->second;
    return true;
}

bool parseShape(const json &entry, ParsedShape &shape, std::string *error) {
    if (!entry.is_object()) {
        if (error)
            *error = "A procedural layout shape is not an object.";
        return false;
    }
    shape.layer = stringField(entry, "layer", {}, error);
    if (shape.layer.empty()) {
        if (error)
            *error = "A procedural layout shape has no layer.";
        return false;
    }
    shape.name = stringField(entry, "name", {}, error);
    shape.net = stringField(entry, "net", {}, error);
    shape.booleanOperand = entry.value("boolean_operand", false);
    if (entry.contains("boolean")) {
        const auto boolean = entry.at("boolean");
        if (!boolean.is_object()) {
            if (error)
                *error = "A procedural layout boolean operation is not an object.";
            return false;
        }
        shape.booleanOperation = stringField(boolean, "operation", {}, error);
        if (shape.booleanOperation != "union" && shape.booleanOperation != "intersection" &&
            shape.booleanOperation != "difference" && shape.booleanOperation != "xor") {
            if (error)
                *error = "Unsupported procedural layout boolean operation: " + shape.booleanOperation;
            return false;
        }
        const auto operands = boolean.value("operands", json::array());
        if (!operands.is_array() || operands.size() < 2) {
            if (error)
                *error = "A procedural layout boolean operation needs at least two operands.";
            return false;
        }
        for (const auto &operand : operands) {
            ParsedShape child;
            if (!parseShape(operand, child, error))
                return false;
            shape.operands.push_back(std::move(child));
        }
        return true;
    }
    if (entry.contains("centerline")) {
        const auto points = entry.at("centerline");
        if (!points.is_array() || points.size() < 2) {
            if (error)
                *error = "A procedural layout path needs at least two points.";
            return false;
        }
        for (const auto &pointValue : points) {
            LayoutPoint point;
            if (!parsePoint(pointValue, "Path point", point, error))
                return false;
            shape.centerline.push_back(point);
        }
        if (!parseCoordinate(entry.at("width"), "Path width", shape.width, error))
            return false;
        if (shape.width <= 0) {
            if (error)
                *error = "A procedural layout path width must be positive.";
            return false;
        }
        Coordinate minX = std::numeric_limits<Coordinate>::max();
        Coordinate minY = std::numeric_limits<Coordinate>::max();
        Coordinate maxX = std::numeric_limits<Coordinate>::lowest();
        Coordinate maxY = std::numeric_limits<Coordinate>::lowest();
        for (const auto &point : shape.centerline) {
            minX = std::min(minX, point.x);
            minY = std::min(minY, point.y);
            maxX = std::max(maxX, point.x);
            maxY = std::max(maxY, point.y);
        }
        const auto halfWidth = (shape.width + 1) / 2;
        shape.rectangle =
            Rect{minX - halfWidth, minY - halfWidth, maxX - minX + 2 * halfWidth, maxY - minY + 2 * halfWidth};
        return true;
    }
    if (entry.contains("text") && entry.contains("rectangle")) {
        if (!parseRect(entry.at("rectangle"), "Text rectangle", shape.rectangle.emplace(), error))
            return false;
        shape.text = stringField(entry, "text", {}, error);
        if (shape.text.empty()) {
            if (error)
                *error = "A procedural layout text shape needs non-empty text.";
            return false;
        }
        return true;
    }
    if (entry.contains("rectangle")) {
        return parseRect(entry.at("rectangle"), "Shape rectangle", shape.rectangle.emplace(), error);
    }
    if (entry.contains("polygon")) {
        const auto points = entry.at("polygon");
        if (!points.is_array() || points.size() < 3) {
            if (error)
                *error = "A procedural layout polygon needs at least three points.";
            return false;
        }
        for (const auto &pointValue : points) {
            LayoutPoint point;
            if (!parsePoint(pointValue, "Polygon point", point, error))
                return false;
            shape.polygon.push_back(point);
        }
        Coordinate minX = std::numeric_limits<Coordinate>::max();
        Coordinate minY = std::numeric_limits<Coordinate>::max();
        Coordinate maxX = std::numeric_limits<Coordinate>::lowest();
        Coordinate maxY = std::numeric_limits<Coordinate>::lowest();
        for (const auto &point : shape.polygon) {
            minX = std::min(minX, point.x);
            minY = std::min(minY, point.y);
            maxX = std::max(maxX, point.x);
            maxY = std::max(maxY, point.y);
        }
        shape.rectangle = Rect{minX, minY, maxX - minX, maxY - minY};
        if (entry.contains("holes")) {
            const auto holes = entry.at("holes");
            if (!holes.is_array()) {
                if (error)
                    *error = "A procedural layout polygon holes entry is not an array.";
                return false;
            }
            for (const auto &holeValue : holes) {
                if (!holeValue.is_array() || holeValue.size() < 3) {
                    if (error)
                        *error = "A procedural layout polygon hole needs at least three points.";
                    return false;
                }
                std::vector<LayoutPoint> hole;
                for (const auto &pointValue : holeValue) {
                    LayoutPoint point;
                    if (!parsePoint(pointValue, "Hole point", point, error))
                        return false;
                    hole.push_back(point);
                }
                shape.holes.push_back(std::move(hole));
            }
        }
        return true;
    }
    if (error)
        *error = "A procedural layout shape has no rectangle, polygon, path, text, or boolean operation.";
    return false;
}

bool parseGeneratedLayout(const json &root, ParsedLayout &layout, std::string *error) {
    if (!root.is_object()) {
        if (error)
            *error = "The procedural layout output is not an object.";
        return false;
    }
    if (root.value("format", std::string{}) != "ice-procedural-layout") {
        if (error)
            *error = "The procedural layout output has an unsupported format.";
        return false;
    }
    if (root.value("schema_version", 0) != 2) {
        if (error)
            *error = "The procedural layout output has an unsupported schema version.";
        return false;
    }

    const auto readMetadata = [&](const char *key) {
        const auto entry = root.find(key);
        if (entry == root.end() || entry->is_null())
            return std::string{};
        if (!entry->is_object()) {
            if (error)
                *error = std::string("The procedural layout ") + key + " entry is not an object.";
            return std::string{};
        }
        return entry->dump();
    };
    layout.layoutQuality = readMetadata("layout_quality");
    layout.layoutPlan = readMetadata("layout_plan");
    layout.layoutReview = readMetadata("layout_review");
    if (error && !error->empty())
        return false;

    const auto layers = root.value("layers", json::array());
    if (!layers.is_array()) {
        if (error)
            *error = "The procedural layout output has no layers array.";
        return false;
    }
    for (const auto &entry : layers) {
        if (!entry.is_object()) {
            if (error)
                *error = "A procedural layout layer entry is not an object.";
            return false;
        }
        ParsedLayer layer;
        layer.name = stringField(entry, "name", {}, error);
        if (layer.name.empty()) {
            if (error)
                *error = "A procedural layout layer has no name.";
            return false;
        }
        if (entry.contains("number") && !entry.at("number").is_null()) {
            Coordinate number = 0;
            if (!parseCoordinate(entry.at("number"), "Layer " + layer.name + " number", number, error))
                return false;
            if (number < 0) {
                if (error)
                    *error = "Layer " + layer.name + " number is invalid.";
                return false;
            }
            layer.number = static_cast<int>(number);
        }
        if (entry.contains("datatype") && !entry.at("datatype").is_null()) {
            Coordinate datatype = 0;
            if (!parseCoordinate(entry.at("datatype"), "Layer " + layer.name + " datatype", datatype, error))
                return false;
            if (datatype < 0) {
                if (error)
                    *error = "Layer " + layer.name + " datatype is invalid.";
                return false;
            }
            layer.datatype = static_cast<int>(datatype);
        }
        layout.layers.push_back(std::move(layer));
    }

    const auto shapes = root.value("shapes", json::array());
    if (!shapes.is_array()) {
        if (error)
            *error = "The procedural layout output has no shapes array.";
        return false;
    }
    for (const auto &entry : shapes) {
        ParsedShape shape;
        if (!parseShape(entry, shape, error))
            return false;
        if (!shape.booleanOperand)
            layout.shapes.push_back(std::move(shape));
    }

    const auto parameters = root.value("parameters", json::array());
    if (!parameters.is_null()) {
        if (!parameters.is_array()) {
            if (error)
                *error = "The procedural layout parameters entry is not an array.";
            return false;
        }
        for (const auto &entry : parameters) {
            if (!entry.is_object()) {
                if (error)
                    *error = "A procedural layout parameter is not an object.";
                return false;
            }
            ParsedParameter parameter;
            parameter.name = stringField(entry, "name", {}, error);
            parameter.value = stringField(entry, "value", {}, error);
            if (parameter.name.empty()) {
                if (error)
                    *error = "A procedural layout parameter needs a name.";
                return false;
            }
            layout.parameters.push_back(std::move(parameter));
        }
    }

    const auto ports = root.value("ports", json::array());
    if (!ports.is_null()) {
        if (!ports.is_array()) {
            if (error)
                *error = "The procedural layout ports entry is not an array.";
            return false;
        }
        for (const auto &entry : ports) {
            if (!entry.is_object()) {
                if (error)
                    *error = "A procedural layout port is not an object.";
                return false;
            }
            ParsedPort port;
            port.name = stringField(entry, "name", {}, error);
            port.layer = stringField(entry, "layer", {}, error);
            if (port.name.empty() || port.layer.empty()) {
                if (error)
                    *error = "A procedural layout port needs a name and a layer.";
                return false;
            }
            if (!parseRect(entry.at("rectangle"), "Port " + port.name + " rectangle", port.rectangle, error))
                return false;
            const auto direction = parsePortDirection(entry.value("direction", std::string{"inout"}));
            const auto side = parsePortSide(entry.value("side", std::string{"left"}));
            if (!direction || !side) {
                if (error)
                    *error = "Port " + port.name + " has an unknown direction or side.";
                return false;
            }
            port.direction = *direction;
            port.side = *side;
            port.net = stringField(entry, "net", {}, error);
            port.label = stringField(entry, "label", port.name, error);
            port.visible = entry.value("visible", true);
            port.locked = entry.value("locked", false);
            port.internal = entry.value("internal", false);
            layout.ports.push_back(std::move(port));
        }
    }

    const auto instances = root.value("instances", json::array());
    if (!instances.is_null()) {
        if (!instances.is_array()) {
            if (error)
                *error = "The procedural layout instances entry is not an array.";
            return false;
        }
        for (const auto &entry : instances) {
            if (!entry.is_object()) {
                if (error)
                    *error = "A procedural layout instance is not an object.";
                return false;
            }
            ParsedInstance instance;
            instance.cell = stringField(entry, "cell", {}, error);
            instance.name = stringField(entry, "name", {}, error);
            if (instance.cell.empty() || instance.name.empty()) {
                if (error)
                    *error = "A procedural layout instance needs a cell name and an instance name.";
                return false;
            }
            if (!parseCoordinate(entry.at("x"), "Instance " + instance.name + " x", instance.x, error) ||
                !parseCoordinate(entry.at("y"), "Instance " + instance.name + " y", instance.y, error) ||
                !parseOrientation(stringField(entry, "orientation", "r0", error), instance.orientation, error))
                return false;
            layout.instances.push_back(std::move(instance));
        }
    }

    const auto devices = root.value("devices", json::array());
    if (!devices.is_null()) {
        if (!devices.is_array()) {
            if (error)
                *error = "The procedural layout devices entry is not an array.";
            return false;
        }
        for (const auto &entry : devices) {
            if (!entry.is_object()) {
                if (error)
                    *error = "A procedural layout device is not an object.";
                return false;
            }
            ParsedDevice device;
            device.name = stringField(entry, "name", {}, error);
            if (device.name.empty()) {
                if (error)
                    *error = "A procedural layout device needs a name.";
                return false;
            }
            device.device = stringField(entry, "device", {}, error);
            device.model = stringField(entry, "model", {}, error);
            device.type = stringField(entry, "type", {}, error);
            if (entry.contains("bounds")) {
                if (!parseRect(entry.at("bounds"), "Device bounds", device.bounds, error))
                    return false;
            }
            const auto parameters = entry.value("parameters", json::array());
            if (!parameters.is_null()) {
                if (!parameters.is_array()) {
                    if (error)
                        *error = "The procedural layout device parameters entry is not an array.";
                    return false;
                }
                for (const auto &parameterEntry : parameters) {
                    if (!parameterEntry.is_object()) {
                        if (error)
                            *error = "A procedural layout device parameter is not an object.";
                        return false;
                    }
                    ParsedParameter parameter;
                    parameter.name = stringField(parameterEntry, "name", {}, error);
                    parameter.value = stringField(parameterEntry, "value", {}, error);
                    if (!parameter.name.empty())
                        device.parameters.push_back(std::move(parameter));
                }
            }
            const auto devicePorts = entry.value("ports", json::array());
            if (!devicePorts.is_null()) {
                if (!devicePorts.is_array()) {
                    if (error)
                        *error = "The procedural layout device ports entry is not an array.";
                    return false;
                }
                for (const auto &portEntry : devicePorts) {
                    if (!portEntry.is_object()) {
                        if (error)
                            *error = "A procedural layout device port is not an object.";
                        return false;
                    }
                    ParsedDevicePort port;
                    port.name = stringField(portEntry, "name", {}, error);
                    port.net = stringField(portEntry, "net", {}, error);
                    if (port.name.empty()) {
                        if (error)
                            *error = "A procedural layout device port needs a name.";
                        return false;
                    }
                    if (portEntry.contains("rectangle") &&
                        !parseRect(portEntry.at("rectangle"), "Device port rectangle", port.rectangle, error))
                        return false;
                    device.ports.push_back(std::move(port));
                }
            }
            layout.devices.push_back(std::move(device));
        }
    }
    return true;
}

int inferredLayerNumber(const std::string &name, const Project &project) {
    int number = 0;
    int multiplier = 1;
    bool hasDigits = false;
    for (auto it = name.rbegin(); it != name.rend() && std::isdigit(static_cast<unsigned char>(*it)); ++it) {
        number += (*it - '0') * multiplier;
        multiplier *= 10;
        hasDigits = true;
    }
    if (hasDigits && number > 0)
        return number;
    int highest = 0;
    for (const auto &[_, layer] : project.layers)
        highest = std::max(highest, layer.number);
    return highest + 1;
}

std::map<std::string, LayerDefaults> packageLayerDefaults(const ProceduralLayout &procedural, std::string *pdkVersion) {
    std::map<std::string, LayerDefaults> result;
    if (procedural.pdk.empty() || procedural.pdk == genericPdkId)
        return result;
    const auto pdks = findPdks(procedural.pdk);
    if (pdks.empty())
        return result;
    if (pdkVersion)
        *pdkVersion = pdks.front().packageVersion;
    const auto manifest = PackageManager::readManifest(pdks.front().packageRoot);
    if (!manifest.pdk)
        return result;
    std::set<std::string> stackLayers;
    if (!procedural.stack.empty()) {
        const auto stack = std::find_if(manifest.pdk->stacks.begin(), manifest.pdk->stacks.end(),
                                        [&](const auto &entry) { return entry.id == procedural.stack; });
        if (stack != manifest.pdk->stacks.end()) {
            for (const auto &name : stack->layers)
                stackLayers.insert(name);
        }
    }
    for (const auto &layer : manifest.pdk->layers) {
        const auto dot = layer.name.find('.');
        const auto base = dot == std::string::npos ? layer.name : layer.name.substr(0, dot);
        if (!stackLayers.empty() && !stackLayers.contains(layer.name) && !stackLayers.contains(base))
            continue;
        result[layer.name] = LayerDefaults{layer.number, layer.datatype, static_cast<Coordinate>(layer.minimumWidth)};
    }
    return result;
}

void includeBounds(std::optional<Rect> &bounds, const Rect &rectangle) {
    if (!rectangle.valid())
        return;
    if (!bounds) {
        bounds = rectangle;
        return;
    }
    const auto left = std::min(bounds->x, rectangle.x);
    const auto bottom = std::min(bounds->y, rectangle.y);
    const auto right = std::max(bounds->x + bounds->width, rectangle.x + rectangle.width);
    const auto top = std::max(bounds->y + bounds->height, rectangle.y + rectangle.height);
    bounds = Rect{left, bottom, right - left, top - bottom};
}

std::optional<Rect> pointBounds(const std::vector<LayoutPoint> &points) {
    if (points.empty())
        return std::nullopt;
    auto minX = points.front().x;
    auto minY = points.front().y;
    auto maxX = minX;
    auto maxY = minY;
    for (const auto &point : points) {
        minX = std::min(minX, point.x);
        minY = std::min(minY, point.y);
        maxX = std::max(maxX, point.x);
        maxY = std::max(maxY, point.y);
    }
    return Rect{minX, minY, maxX - minX, maxY - minY};
}

std::optional<Rect> layoutCellBounds(const Project &project, ObjectId cellId, std::set<ObjectId> &ancestors,
                                     std::map<ObjectId, Rect> &cache) {
    if (const auto cached = cache.find(cellId); cached != cache.end())
        return cached->second;
    if (ancestors.contains(cellId))
        return std::nullopt;
    const auto cellIt = project.layoutCells.find(cellId);
    if (cellIt == project.layoutCells.end())
        return std::nullopt;
    ancestors.insert(cellId);
    std::optional<Rect> bounds;
    for (const auto &[_, shape] : cellIt->second.shapes) {
        if (shape.rectangle.valid()) {
            includeBounds(bounds, shape.rectangle);
            continue;
        }
        if (!shape.polygon.empty()) {
            if (const auto box = pointBounds(shape.polygon))
                includeBounds(bounds, *box);
            continue;
        }
        if (const auto box = pointBounds(shape.centerline)) {
            auto expanded = *box;
            const auto half = std::max<Coordinate>(0, shape.width) / 2;
            expanded.x -= half;
            expanded.y -= half;
            expanded.width += 2 * half;
            expanded.height += 2 * half;
            includeBounds(bounds, expanded);
        }
    }
    for (const auto &[_, port] : cellIt->second.ports)
        includeBounds(bounds, port.rectangle);
    for (const auto &[_, instance] : cellIt->second.instances) {
        const auto child = layoutCellBounds(project, instance.cell, ancestors, cache);
        if (!child)
            continue;
        const Transform transform{instance.x, instance.y, instance.orientation};
        const std::array<Point, 4> corners = {
            Point{child->x, child->y},
            Point{child->x + child->width, child->y},
            Point{child->x + child->width, child->y + child->height},
            Point{child->x, child->y + child->height},
        };
        auto minX = std::numeric_limits<Coordinate>::max();
        auto minY = std::numeric_limits<Coordinate>::max();
        auto maxX = std::numeric_limits<Coordinate>::lowest();
        auto maxY = std::numeric_limits<Coordinate>::lowest();
        for (const auto &corner : corners) {
            const auto point = transform.apply(corner);
            minX = std::min(minX, point.x);
            minY = std::min(minY, point.y);
            maxX = std::max(maxX, point.x);
            maxY = std::max(maxY, point.y);
        }
        includeBounds(bounds, Rect{minX, minY, maxX - minX, maxY - minY});
    }
    ancestors.erase(cellId);
    if (bounds)
        cache.emplace(cellId, *bounds);
    return bounds;
}

json layoutCellCatalog(const Project &project) {
    std::map<ObjectId, json> entries;
    std::set<std::string> names;
    std::map<ObjectId, Rect> cache;
    for (const auto &[id, cell] : project.layoutCells) {
        std::set<ObjectId> ancestors;
        const auto bounds = layoutCellBounds(project, id, ancestors, cache);
        json entry = json::object();
        entry["name"] = cell.name;
        if (bounds) {
            entry["bounds"] = {
                {"x", bounds->x}, {"y", bounds->y}, {"width", bounds->width}, {"height", bounds->height}};
            entry["width"] = bounds->width;
            entry["height"] = bounds->height;
        } else {
            entry["width"] = 0;
            entry["height"] = 0;
        }
        json ports = json::array();
        for (const auto &[_, port] : cell.ports) {
            json item = {{"name", port.name},
                         {"label", port.label},
                         {"direction", portDirectionName(port.direction)},
                         {"side", portSideName(port.side)},
                         {"rectangle",
                          {{"x", port.rectangle.x},
                           {"y", port.rectangle.y},
                           {"width", port.rectangle.width},
                           {"height", port.rectangle.height}}}};
            if (const auto layer = project.layers.find(port.layer); layer != project.layers.end())
                item["layer"] = layer->second.name;
            if (port.net != 0) {
                if (const auto net = cell.nets.find(port.net); net != cell.nets.end())
                    item["net"] = net->second.name;
            }
            ports.push_back(std::move(item));
        }
        entry["ports"] = std::move(ports);
        names.insert(cell.name);
        entries.emplace(id, std::move(entry));
    }
    json result = json::array();
    for (const auto &[_, entry] : entries)
        result.push_back(entry);
    for (const auto &[_, procedural] : project.proceduralLayouts) {
        if (procedural.name.empty() || names.contains(procedural.name))
            continue;
        const auto entry = entries.find(procedural.generatedCell);
        if (entry == entries.end())
            continue;
        auto alias = entry->second;
        alias["name"] = procedural.name;
        alias["generated_cell"] = entry->second.value("name", "");
        names.insert(procedural.name);
        result.push_back(std::move(alias));
    }
    return result;
}

bool applyGeneratedLayout(Project &project, ObjectId proceduralLayoutId, const ProceduralLayout &procedural,
                          const ParsedLayout &generated, std::string *error) {
    auto cellIt = project.layoutCells.find(procedural.generatedCell);
    if (cellIt == project.layoutCells.end()) {
        if (error)
            *error = "The generated layout cell is missing from the project.";
        return false;
    }

    std::map<std::string, ObjectId> cellsByName;
    for (const auto &[id, cell] : project.layoutCells)
        cellsByName[cell.name] = id;
    for (const auto &[_, layout] : project.proceduralLayouts) {
        if (!layout.name.empty())
            cellsByName.emplace(layout.name, layout.generatedCell);
    }
    for (const auto &instance : generated.instances) {
        const auto cell = cellsByName.find(instance.cell);
        if (cell == cellsByName.end()) {
            if (error)
                *error = "Instance '" + instance.name + "' references an unknown layout cell '" + instance.cell + "'.";
            return false;
        }
        if (cell->second == procedural.generatedCell) {
            if (error)
                *error = "Instance '" + instance.name + "' would make the generated layout contain itself.";
            return false;
        }
    }

    auto backup = project;
    try {
        const auto genericPdk = procedural.pdk.empty() || procedural.pdk == genericPdkId;
        const auto packageLayers = packageLayerDefaults(procedural, nullptr);
        const auto findLayer = [&](const std::string &name, std::optional<int> datatype) {
            return std::find_if(project.layers.begin(), project.layers.end(), [&](const auto &entry) {
                const auto &layer = entry.second;
                const auto pdkMatches =
                    genericPdk ? (layer.pdk.empty() || layer.pdk == genericPdkId) : layer.pdk == procedural.pdk;
                if (!pdkMatches || layer.name != name || layer.stack != procedural.stack)
                    return false;
                return !datatype || layer.datatype == *datatype;
            });
        };
        const auto addProceduralLayer = [&](const std::string &name, std::optional<int> number,
                                            std::optional<int> datatype) {
            auto defaults = packageLayers.find(name);
            int layerNumber = number.value_or(defaults != packageLayers.end() ? defaults->second.number
                                                                              : inferredLayerNumber(name, project));
            int layerDatatype = datatype.value_or(defaults != packageLayers.end() ? defaults->second.datatype : 0);
            Coordinate minimumWidth = defaults != packageLayers.end() ? defaults->second.minimumWidth : 0;
            return addLayer(project, name, layerNumber, 0, genericPdk ? std::string{} : procedural.pdk, layerDatatype,
                            procedural.stack, minimumWidth);
        };
        std::map<std::string, ParsedLayer> parsedLayers;
        for (const auto &layer : generated.layers)
            parsedLayers[layer.name] = layer;
        std::map<std::string, ObjectId> layerIds;
        for (const auto &[name, layer] : parsedLayers) {
            const auto existing = findLayer(name, layer.datatype);
            if (existing != project.layers.end()) {
                layerIds[name] = existing->first;
                continue;
            }
            layerIds[name] = addProceduralLayer(name, layer.number, layer.datatype);
        }
        const auto layerIdFor = [&](const std::string &name) {
            const auto parsed = parsedLayers.find(name);
            const auto datatype = parsed != parsedLayers.end() ? parsed->second.datatype : std::nullopt;
            const auto existing = findLayer(name, datatype);
            if (existing != project.layers.end())
                return existing->first;
            const auto generatedLayer = layerIds.find(name);
            if (generatedLayer != layerIds.end())
                return generatedLayer->second;
            return addProceduralLayer(name, parsed != parsedLayers.end() ? parsed->second.number : std::nullopt,
                                      datatype);
        };

        auto &cell = project.layoutCells.at(procedural.generatedCell);
        cell.shapes.clear();
        cell.ports.clear();
        cell.nets.clear();
        cell.instances.clear();
        cell.plannedDevices.clear();
        cell.plannedConnections.clear();
        cell.generatedBy = proceduralLayoutId;
        cell.readOnly = true;

        const auto netIdFor = [&](const std::string &name) -> ObjectId {
            if (name.empty())
                return 0;
            for (const auto &[id, net] : cell.nets) {
                if (net.name == name)
                    return id;
            }
            return createLayoutNet(project, procedural.generatedCell, name);
        };

        GeometryKernel kernel;
        const auto toKernelPolygon = [](const std::vector<LayoutPoint> &points) {
            Polygon polygon;
            polygon.reserve(points.size());
            for (const auto &point : points)
                polygon.push_back({point.x, point.y});
            return polygon;
        };
        const auto toLayoutPolygon = [](const Polygon &polygon) {
            std::vector<LayoutPoint> points;
            points.reserve(polygon.size());
            for (const auto &point : polygon)
                points.push_back({point.x, point.y});
            return points;
        };
        const auto rectanglePolygon = [](const Rect &rect) {
            return Polygon{{rect.x, rect.y},
                           {rect.x + rect.width, rect.y},
                           {rect.x + rect.width, rect.y + rect.height},
                           {rect.x, rect.y + rect.height}};
        };
        std::function<bool(const ParsedShape &, MultiPolygon &)> evaluateShape;
        evaluateShape = [&](const ParsedShape &parsed, MultiPolygon &out) -> bool {
            if (!parsed.booleanOperation.empty()) {
                std::vector<MultiPolygon> operands;
                operands.reserve(parsed.operands.size());
                for (const auto &operand : parsed.operands) {
                    MultiPolygon geometry;
                    if (!evaluateShape(operand, geometry))
                        return false;
                    operands.push_back(std::move(geometry));
                }
                if (operands.size() < 2) {
                    if (error)
                        *error = "A procedural layout boolean operation needs at least two operands.";
                    return false;
                }
                if (parsed.booleanOperation == "union") {
                    MultiPolygon all;
                    for (const auto &operand : operands)
                        all.insert(all.end(), operand.begin(), operand.end());
                    out = kernel.booleanOp(all, {}, BooleanOperation::union_);
                } else if (parsed.booleanOperation == "intersection") {
                    out = operands.front();
                    for (std::size_t index = 1; index < operands.size() && !out.empty(); ++index)
                        out = kernel.booleanOp(out, operands[index], BooleanOperation::intersection);
                } else if (parsed.booleanOperation == "difference") {
                    MultiPolygon clips;
                    for (std::size_t index = 1; index < operands.size(); ++index)
                        clips.insert(clips.end(), operands[index].begin(), operands[index].end());
                    if (clips.size() > 1)
                        clips = kernel.booleanOp(clips, {}, BooleanOperation::union_);
                    out = kernel.booleanOp(operands.front(), clips, BooleanOperation::difference);
                } else {
                    out = operands.front();
                    for (std::size_t index = 1; index < operands.size() && !out.empty(); ++index)
                        out = kernel.booleanOp(out, operands[index], BooleanOperation::xor_);
                }
                if (out.empty()) {
                    if (error)
                        *error = "The procedural layout boolean operation produced no geometry.";
                    return false;
                }
                return true;
            }
            if (!parsed.holes.empty()) {
                MultiPolygon contours;
                contours.push_back(toKernelPolygon(parsed.polygon));
                for (const auto &hole : parsed.holes)
                    contours.push_back(toKernelPolygon(hole));
                out = kernel.fillEvenOdd(contours);
                if (out.empty()) {
                    if (error)
                        *error = "The procedural layout polygon holes produced no geometry.";
                    return false;
                }
                return true;
            }
            if (!parsed.polygon.empty()) {
                out = {toKernelPolygon(parsed.polygon)};
                return true;
            }
            if (parsed.rectangle) {
                out = {rectanglePolygon(*parsed.rectangle)};
                return true;
            }
            if (error)
                *error = "Boolean operands and polygon holes require rectangles or polygons.";
            return false;
        };

        for (const auto &parsed : generated.shapes) {
            if (!parsed.booleanOperation.empty() || !parsed.holes.empty()) {
                MultiPolygon polygons;
                if (!evaluateShape(parsed, polygons))
                    return false;
                const auto layerId = layerIdFor(parsed.layer);
                const auto net =
                    parsed.net.empty() ? std::optional<ObjectId>{} : std::optional<ObjectId>{netIdFor(parsed.net)};
                for (std::size_t index = 0; index < polygons.size(); ++index) {
                    const auto id = project.nextId();
                    Shape shape{id, layerId, net, GeometryKernel::bounds(polygons[index]),
                                toLayoutPolygon(polygons[index])};
                    shape.name = parsed.name;
                    if (!shape.name.empty() && polygons.size() > 1)
                        shape.name += "_" + std::to_string(index + 1);
                    cell.shapes.emplace(id, std::move(shape));
                }
                continue;
            }
            const auto id = project.nextId();
            const auto net =
                parsed.net.empty() ? std::optional<ObjectId>{} : std::optional<ObjectId>{netIdFor(parsed.net)};
            Shape shape{id, layerIdFor(parsed.layer), net, parsed.rectangle.value_or(Rect{}), parsed.polygon};
            shape.centerline = parsed.centerline;
            shape.width = parsed.width;
            shape.text = parsed.text;
            shape.name = parsed.name;
            cell.shapes.emplace(id, std::move(shape));
        }
        for (const auto &port : generated.ports) {
            const auto netId = netIdFor(port.net);
            const auto id = project.nextId();
            Port value{id, port.name, layerIdFor(port.layer), port.rectangle};
            value.direction = port.direction;
            value.side = port.side;
            value.net = netId;
            value.label = port.label;
            value.visible = port.visible;
            value.locked = port.locked;
            value.internal = port.internal;
            cell.ports.emplace(id, std::move(value));
        }
        for (const auto &instance : generated.instances) {
            const auto id = project.nextId();
            cell.instances.emplace(id, LayoutInstance{id, cellsByName.at(instance.cell), instance.name, instance.x,
                                                      instance.y, instance.orientation});
        }
        for (const auto &device : generated.devices) {
            const auto id = project.nextId();
            PlannedDevice planned;
            planned.id = id;
            planned.name = device.name;
            planned.x = device.bounds.x;
            planned.y = device.bounds.y;
            planned.width = device.bounds.width;
            planned.height = device.bounds.height;
            planned.device = device.device;
            planned.model = device.model.empty() ? device.type : device.model;
            planned.placeholder = false;
            for (const auto &parameter : device.parameters)
                planned.parameters.push_back({project.nextId(), parameter.name, parameter.value});
            for (const auto &port : device.ports) {
                if (port.net.empty())
                    continue;
                planned.terminals[port.name] = netIdFor(port.net);
            }
            cell.plannedDevices.emplace(id, std::move(planned));
        }
    } catch (const std::exception &exception) {
        project = std::move(backup);
        if (error)
            *error = std::string("Unable to apply the generated layout: ") + exception.what();
        return false;
    }
    return true;
}

} // namespace

std::string proceduralScriptHash(const std::string &script) {
    // The builder and its built-in generators are part of the generated
    // layout contract. Include their contents so upgrading ICE cannot leave
    // stale generated geometry behind an unchanged user script.
    std::string text = script;
    const auto runner = proceduralRunnerScriptPath();
    if (!runner.empty()) {
        text.push_back('\0');
        text += readTextFile(runner);
        const auto builtins = runner.parent_path() / "generators" / "__init__.py";
        text.push_back('\0');
        text += readTextFile(builtins);
    }
    return hashText(text);
}

std::string proceduralParametersHash(const std::vector<Parameter> &parameters) {
    std::map<std::string, std::string> values;
    for (const auto &parameter : parameters)
        values[parameter.name] = parameter.value;
    std::string text;
    for (const auto &[name, value] : values) {
        text += name;
        text.push_back('\0');
        text += value;
        text.push_back('\0');
    }
    return hashText(text);
}

bool proceduralLayoutNeedsRegeneration(const ProceduralLayout &layout) {
    if (layout.generatedScriptHash.empty() || layout.generatedParameterHash.empty())
        return true;
    if (layout.generatedScriptHash != proceduralScriptHash(layout.script) ||
        layout.generatedParameterHash != proceduralParametersHash(layout.parameters))
        return true;
    if (layout.pdk != genericPdkId && !layout.generatedPdkVersion.empty()) {
        const auto pdks = findPdks(layout.pdk);
        if (!pdks.empty() && pdks.front().packageVersion != layout.generatedPdkVersion)
            return true;
    }
    return false;
}

std::filesystem::path proceduralRunnerScriptPath(const std::filesystem::path &executablePath) {
    if (const char *configured = std::getenv("ICE_PROCEDURAL_RUNNER"); configured && *configured) {
        if (isRegularFile(configured))
            return configured;
    }
    const auto executable = normalizedExecutable(executablePath);
    std::vector<std::filesystem::path> candidates;
#ifdef ICE_SOURCE_DIR
    candidates.emplace_back(std::filesystem::path(ICE_SOURCE_DIR) / "assets" / "layout" / "procedural_runner.py");
#endif
    auto directory = executable.parent_path();
    for (int level = 0; level < 6 && !directory.empty(); ++level) {
        candidates.push_back(directory / "assets" / "layout" / "procedural_runner.py");
        directory = directory.parent_path();
    }
    if (!executable.empty())
        candidates.push_back(executable.parent_path() / ".." / "share" / "ice" / "assets" / "layout" /
                             "procedural_runner.py");
    auto current = std::filesystem::current_path();
    for (int level = 0; level < 5 && !current.empty(); ++level) {
        candidates.push_back(current / "assets" / "layout" / "procedural_runner.py");
        current = current.parent_path();
    }
    candidates.push_back("/usr/share/ice/assets/layout/procedural_runner.py");
    candidates.push_back("/usr/local/share/ice/assets/layout/procedural_runner.py");
    if (const char *home = std::getenv("HOME"); home && *home)
        candidates.push_back(std::filesystem::path(home) / ".local" / "share" / "ice" / "assets" / "layout" /
                             "procedural_runner.py");
    for (const auto &candidate : candidates) {
        if (isRegularFile(candidate))
            return candidate;
    }
    return {};
}

std::string defaultProceduralLayoutScript() {
    return R"(# Procedural layout script.
# ICE runs this script with a `layout` builder object injected into the module.
# Use layout.nm(), layout.um(), and layout.db() for physical coordinates.
#
# layer = layout.layer("Metal1", 1)
# shape = layer.rectangle(0, 0, 4000, 600)
# shape.name = "M1"
# shape.net = "VDD"
# layer.path([(0, 0), (4000, 0)], width=400)
# layer.text(0, 0, "M1", height=500)
# layout.port("P1", "Metal1", 0, 0, 1000, 1000)
# layout.union("Metal1", shape_a, shape_b)
# layout.polygon("Metal1", points, holes=[hole_points])
# layout.device("nmos", name="M1", x=0, y=0, w="1u", l="0.15u")
# layout.connect(device_a, "G", device_b, "G", layer="Metal1", width=400, net="GATE")
# layout.route((0, 0), (4000, 0), layer="Metal1", width=400, net="VDD")
# layout.route(pin_a, pad_b, layers=["Metal1", "Metal2"], width=400, net="VDD")
# layout.instance("other_layout_cell", 0, 0, name="I1", orientation="r0")
# layout.array("unit_cell", (2, 2), (2000, 2000))
# layout.add_dummy_fill(layers=["Metal1", "Metal2"], density=50, deviation=10)
#
# For compact layouts, compose handles instead of hand-computing coordinates:
#   row = layout.row(gap=0, align="bottom")
#   row.add(layout.device("nmos", name="M1", w="1u", l="0.15u"))
#   row.add(layout.device("nmos", name="M2", w="1u", l="0.15u"))
#   layout.add(row)
# Use layout.constraint("Order", ..., abut=True), layout.align,
# layout.generate("mos_pair"), and layout.repeat for matching.
# Add spacing only where the router or DRC requires it.

metal1 = layout.layer("Metal1", 1)
metal2 = layout.layer("Metal2", 2)

width = layout.um(layout.parameter("width", "4", "Horizontal metal width in micrometers"))
metal1.rectangle(layout.um(0), layout.um(0), width, layout.um(0.6))
metal1.rectangle(layout.um(0), layout.um(2.4), width, layout.um(0.6))
metal2.rectangle(layout.um(1), layout.um(1.5), width / 2, layout.um(0.6))
layout.report(print_report=True)
)";
}

ProceduralLayoutRunResult regenerateProceduralLayout(Project &project, ObjectId proceduralLayoutId,
                                                     const std::filesystem::path &executablePath, int timeoutSeconds) {
    ProceduralLayoutRunResult result;
    const auto proceduralIt = project.proceduralLayouts.find(proceduralLayoutId);
    if (proceduralIt == project.proceduralLayouts.end()) {
        result.error = "The procedural layout does not exist.";
        return result;
    }
    auto &procedural = proceduralIt->second;
    result.scriptHash = proceduralScriptHash(procedural.script);
    result.parameterHash = proceduralParametersHash(procedural.parameters);

    const auto runner = proceduralRunnerScriptPath(executablePath);
    if (runner.empty()) {
        result.error = "The procedural layout runner script was not found.";
        return result;
    }
    const auto python = proceduralPythonExecutable();
    if (python.empty()) {
        result.error = "Python 3 was not found. Set ICE_PYTHON to the interpreter path.";
        return result;
    }

    std::random_device random;
    std::uniform_int_distribution<std::uint64_t> distribution;
    std::ostringstream stem;
    stem << "ice-procedural-" << std::hex << distribution(random);
    const auto directory = std::filesystem::temp_directory_path() / stem.str();
    std::error_code status;
    std::filesystem::create_directories(directory, status);
    if (status) {
        result.error = "Unable to create a temporary directory for procedural layout generation.";
        return result;
    }
    const auto cleanup = [&] {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    };
    const auto scriptPath = directory / "layout.py";
    const auto paramsPath = directory / "params.json";
    const auto cellsPath = directory / "cells.json";
    const auto outputPath = directory / "layout.json";
    const auto logPath = directory / "runner.log";
    {
        std::ofstream scriptFile(scriptPath, std::ios::binary | std::ios::trunc);
        if (!scriptFile) {
            result.error = "Unable to write the procedural layout script.";
            cleanup();
            return result;
        }
        scriptFile << procedural.script;
        json params = json::object();
        for (const auto &parameter : procedural.parameters)
            params[parameter.name] = parameter.value;
        std::ofstream paramsFile(paramsPath, std::ios::binary | std::ios::trunc);
        if (!paramsFile) {
            result.error = "Unable to write the procedural layout parameters.";
            cleanup();
            return result;
        }
        paramsFile << params.dump(2);
    }
    {
        std::ofstream cellsFile(cellsPath, std::ios::binary | std::ios::trunc);
        if (!cellsFile) {
            result.error = "Unable to write the procedural layout cell catalog.";
            cleanup();
            return result;
        }
        cellsFile << layoutCellCatalog(project).dump(2);
    }

    std::filesystem::path pdkRoot;
    if (procedural.pdk != genericPdkId) {
        const auto pdks = findPdks(procedural.pdk);
        if (!pdks.empty()) {
            pdkRoot = pdks.front().packageRoot;
            result.pdkVersion = pdks.front().packageVersion;
        }
    }
    double nanometersPerUnit = 1.0;
    if (project.units.denominator > 0)
        nanometersPerUnit =
            static_cast<double>(project.units.nanometers_per_unit) / static_cast<double>(project.units.denominator);

    ProcessOptions options;
    options.arguments = {
        python,          runner.string(),
        "--script",      scriptPath.string(),
        "--params-file", paramsPath.string(),
        "--cells-file",  cellsPath.string(),
        "--output",      outputPath.string(),
        "--pdk",         procedural.pdk,
        "--db-unit-nm",  std::to_string(nanometersPerUnit),
        "--grid",        std::to_string(std::max<Coordinate>(1, project.grid)),
    };
    if (!pdkRoot.empty()) {
        options.arguments.push_back("--pdk-root");
        options.arguments.push_back(pdkRoot.string());
    }
    options.workingDirectory = directory;
    options.logPath = logPath;
    options.timeoutSeconds = std::max(0, timeoutSeconds);
    const auto pythonPath = reportPythonPath(executablePath);
    if (!pythonPath.empty())
        options.environment["PYTHONPATH"] = pythonPath;
    const auto process = runProcess(options);
    result.log = process.logTail;
    if (!process.succeeded()) {
        if (process.timedOut)
            result.error = "The procedural layout script timed out.";
        else if (process.cancelled)
            result.error = "The procedural layout script was cancelled.";
        else if (!process.error.empty())
            result.error = process.error;
        else
            result.error = "The procedural layout script failed.";
        if (!process.logTail.empty())
            result.error += "\n" + process.logTail;
        cleanup();
        return result;
    }

    const auto outputText = readTextFile(outputPath);
    if (outputText.empty()) {
        result.error = "The procedural layout script did not produce an output file.";
        cleanup();
        return result;
    }
    result.outputHash = hashText(outputText);
    json document;
    try {
        document = json::parse(outputText);
    } catch (const std::exception &exception) {
        result.error = std::string("Unable to parse the procedural layout output: ") + exception.what();
        cleanup();
        return result;
    }
    ParsedLayout generated;
    try {
        if (!parseGeneratedLayout(document, generated, &result.error)) {
            cleanup();
            return result;
        }
    } catch (const std::exception &exception) {
        result.error = std::string("Unable to parse the procedural layout output: ") + exception.what();
        cleanup();
        return result;
    }
    if (!applyGeneratedLayout(project, proceduralLayoutId, procedural, generated, &result.error)) {
        cleanup();
        return result;
    }
    std::vector<Parameter> parameters;
    auto nextParameterId = project.nextId();
    for (const auto &parsed : generated.parameters) {
        auto existing = std::find_if(procedural.parameters.begin(), procedural.parameters.end(),
                                     [&](const auto &parameter) { return parameter.name == parsed.name; });
        Parameter parameter;
        parameter.id = existing != procedural.parameters.end() ? existing->id : nextParameterId++;
        parameter.name = parsed.name;
        parameter.value = parsed.value;
        parameters.push_back(std::move(parameter));
    }
    procedural.parameters = std::move(parameters);
    procedural.generatedScriptHash = result.scriptHash;
    procedural.generatedParameterHash = proceduralParametersHash(procedural.parameters);
    procedural.generatedPdkVersion = result.pdkVersion;
    procedural.generatedOutputHash = result.outputHash;
    procedural.generatedLayoutQuality = generated.layoutQuality;
    procedural.generatedLayoutPlan = generated.layoutPlan;
    procedural.generatedLayoutReview = generated.layoutReview;
    result.layoutQuality = std::move(generated.layoutQuality);
    result.layoutPlan = std::move(generated.layoutPlan);
    result.layoutReview = std::move(generated.layoutReview);
    result.ok = true;
    cleanup();
    return result;
}

} // namespace ice
