#include "icelib/pex.hpp"

#include "icelib/connectivity.hpp"
#include "icelib/extraction.hpp"
#include "icelib/geometry.hpp"
#include "icelib/operations.hpp"
#include "icelib/verification.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace ice {
namespace {

using json = nlohmann::ordered_json;

std::string lowerCopy(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return text;
}

bool containsAny(const std::string &text, std::initializer_list<const char *> needles) {
    return std::any_of(needles.begin(), needles.end(),
                       [&](const char *needle) { return text.find(needle) != std::string::npos; });
}

std::string trimCopy(std::string text) {
    const auto notSpace = [](unsigned char character) { return !std::isspace(character); };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), notSpace));
    text.erase(std::find_if(text.rbegin(), text.rend(), notSpace).base(), text.end());
    return text;
}

std::string spiceIdentifier(std::string text) {
    text = trimCopy(std::move(text));
    std::string result;
    result.reserve(text.size());
    for (const char character : text) {
        const auto byte = static_cast<unsigned char>(character);
        result.push_back(std::isalnum(byte) || character == '_' ? character : '_');
    }
    if (result.empty())
        result = "n";
    if (std::isdigit(static_cast<unsigned char>(result.front())))
        result.insert(result.begin(), 'n');
    if (result == "0")
        result = "n_0";
    return result;
}

class SpiceNameAllocator {
  public:
    std::string unique(const std::string &requested) {
        auto base = spiceIdentifier(requested);
        auto candidate = base;
        for (int suffix = 2; names_.contains(lowerCopy(candidate)); ++suffix)
            candidate = base + "_" + std::to_string(suffix);
        names_.insert(lowerCopy(candidate));
        return candidate;
    }

  private:
    std::set<std::string> names_;
};

std::string formatNumber(double value, int precision = 6) {
    if (std::abs(value) < 1.0e-15)
        value = 0.0;
    std::ostringstream stream;
    stream << std::setprecision(precision) << std::defaultfloat << value;
    return stream.str();
}

enum class LayerKind { ignored, metal, poly, diffusion, via };

struct ParsedLayer {
    LayerKind kind = LayerKind::ignored;
    std::string base;
    std::string purpose;
};

ParsedLayer parseLayer(const std::string &name) {
    ParsedLayer parsed;
    const auto dot = name.rfind('.');
    parsed.base = dot == std::string::npos ? name : name.substr(0, dot);
    parsed.purpose = dot == std::string::npos ? std::string{} : name.substr(dot + 1);

    const auto base = lowerCopy(parsed.base);
    if (containsAny(base, {"via", "contact", "licon", "mcon", "vmim"}))
        parsed.kind = LayerKind::via;
    else if (containsAny(base, {"well", "implant", "marker", "boundary", "keepout", "text", "label", "outline",
                                "areaid", "mim"}))
        parsed.kind = LayerKind::ignored;
    else if (containsAny(base, {"poly", "gate"}))
        parsed.kind = LayerKind::poly;
    else if (containsAny(base, {"activ", "diff"}))
        parsed.kind = LayerKind::diffusion;
    else if (containsAny(base, {"metal", "dfpad", "pad"}))
        parsed.kind = LayerKind::metal;
    return parsed;
}

bool isDrawingPurpose(const ParsedLayer &layer) {
    const auto purpose = lowerCopy(layer.purpose);
    return purpose.empty() || purpose == "drawing";
}

struct ShapeMetrics {
    double area = 0.0;      // um^2
    double perimeter = 0.0; // um
    double squares = 0.0;   // squares along the longest dimension
};

ShapeMetrics rectangleMetrics(const Rect &rectangle, const DatabaseUnits &units) {
    ShapeMetrics metrics;
    if (!rectangle.valid())
        return metrics;
    const auto width = units.toNanometers(rectangle.width);
    const auto height = units.toNanometers(rectangle.height);
    if (width <= 0.0 || height <= 0.0)
        return metrics;
    metrics.area = width * height / 1'000'000.0;
    metrics.perimeter = 2.0 * (width + height) / 1000.0;
    const auto shortSide = std::min(width, height);
    if (shortSide > 0.0)
        metrics.squares = std::max(width, height) / shortSide;
    return metrics;
}

ShapeMetrics polygonMetrics(const std::vector<LayoutPoint> &points, const DatabaseUnits &units) {
    ShapeMetrics metrics;
    if (points.size() < 3)
        return metrics;
    std::vector<std::pair<double, double>> converted;
    converted.reserve(points.size());
    for (const auto &point : points)
        converted.emplace_back(units.toNanometers(point.x), units.toNanometers(point.y));

    double twiceArea = 0.0;
    double perimeter = 0.0;
    double minX = converted.front().first;
    double maxX = minX;
    double minY = converted.front().second;
    double maxY = minY;
    for (std::size_t index = 0; index < converted.size(); ++index) {
        const auto &current = converted[index];
        const auto &next = converted[(index + 1) % converted.size()];
        twiceArea += current.first * next.second - next.first * current.second;
        perimeter += std::hypot(next.first - current.first, next.second - current.second);
        minX = std::min(minX, current.first);
        maxX = std::max(maxX, current.first);
        minY = std::min(minY, current.second);
        maxY = std::max(maxY, current.second);
    }
    const auto area = std::abs(twiceArea) * 0.5;
    metrics.area = area / 1'000'000.0;
    metrics.perimeter = perimeter / 1000.0;
    const auto length = std::max(maxX - minX, maxY - minY);
    if (area > 0.0 && length > 0.0) {
        const auto width = area / length;
        if (width > 0.0)
            metrics.squares = length / width;
    }
    return metrics;
}

ShapeMetrics pathMetrics(const Shape &shape, const DatabaseUnits &units) {
    ShapeMetrics metrics;
    if (shape.centerline.size() < 2 || shape.width <= 0)
        return metrics;
    double length = 0.0;
    for (std::size_t index = 1; index < shape.centerline.size(); ++index) {
        const auto &previous = shape.centerline[index - 1];
        const auto &current = shape.centerline[index];
        length += std::hypot(units.toNanometers(current.x) - units.toNanometers(previous.x),
                             units.toNanometers(current.y) - units.toNanometers(previous.y));
    }
    const auto width = units.toNanometers(shape.width);
    if (length <= 0.0 || width <= 0.0)
        return metrics;
    metrics.area = length * width / 1'000'000.0;
    metrics.perimeter = 2.0 * (length + width) / 1000.0;
    metrics.squares = length / width;
    return metrics;
}

ShapeMetrics shapeMetrics(const Shape &shape, const DatabaseUnits &units) {
    if (shape.isPath())
        return pathMetrics(shape, units);
    if (shape.polygon.size() >= 3)
        return polygonMetrics(shape.polygon, units);
    return rectangleMetrics(shape.rectangle, units);
}

struct NetObjectGeometry {
    ObjectId layer = 0;
    Rect rectangle;
};

struct TerminalMatch {
    std::size_t group = static_cast<std::size_t>(-1);
    int matches = 0;
};

bool rectanglesOverlapOrTouch(const Rect &left, const Rect &right) {
    if (!left.valid() || !right.valid())
        return false;
    return left.x <= right.x + right.width && right.x <= left.x + left.width && left.y <= right.y + right.height &&
           right.y <= left.y + left.height;
}

Rect transformRectangle(const Rect &rectangle, const Transform &transform) {
    if (!rectangle.valid())
        return {};
    const auto first = transform.apply({rectangle.x, rectangle.y});
    const auto second = transform.apply({rectangle.x + rectangle.width, rectangle.y});
    const auto third = transform.apply({rectangle.x, rectangle.y + rectangle.height});
    const auto fourth = transform.apply({rectangle.x + rectangle.width, rectangle.y + rectangle.height});
    const auto minX = std::min({first.x, second.x, third.x, fourth.x});
    const auto maxX = std::max({first.x, second.x, third.x, fourth.x});
    const auto minY = std::min({first.y, second.y, third.y, fourth.y});
    const auto maxY = std::max({first.y, second.y, third.y, fourth.y});
    if (maxX <= minX || maxY <= minY)
        return {};
    return {minX, minY, maxX - minX, maxY - minY};
}

std::string parameterValue(const LayoutCell &cell, std::initializer_list<std::string_view> wanted) {
    for (const auto &parameter : cell.parameters) {
        const auto name = lowerCopy(parameter.name);
        for (const auto candidate : wanted) {
            if (name == lowerCopy(std::string(candidate)))
                return trimCopy(parameter.value);
        }
    }
    return {};
}

enum class PlacedDeviceKind { unknown, mos, bjt, capacitor, resistor, diode, inductor };

PlacedDeviceKind classifyPlacedDevice(const LayoutCell &child, const std::string &deviceName,
                                      const std::string &model) {
    const auto device = lowerCopy(deviceName);
    const auto modelName = lowerCopy(model);
    const auto matches = [&](std::initializer_list<const char *> needles) {
        return containsAny(device, needles) || containsAny(modelName, needles);
    };
    if (matches({"nmos", "pmos", "nfet", "pfet", "mosfet"}))
        return PlacedDeviceKind::mos;
    if (matches({"npn", "pnp", "bjt", "hbt"}))
        return PlacedDeviceKind::bjt;
    if (child.ports.size() >= 2 && matches({"capacitor", "mim", "cap"}))
        return PlacedDeviceKind::capacitor;
    if (child.ports.size() == 2 && matches({"resistor", "res"}))
        return PlacedDeviceKind::resistor;
    if (child.ports.size() == 2 && matches({"diode", "schottky"}))
        return PlacedDeviceKind::diode;
    if (child.ports.size() == 2 && matches({"inductor"}))
        return PlacedDeviceKind::inductor;
    return PlacedDeviceKind::unknown;
}

std::string placedDeviceKindName(PlacedDeviceKind kind) {
    switch (kind) {
    case PlacedDeviceKind::mos:
        return "mos";
    case PlacedDeviceKind::bjt:
        return "bjt";
    case PlacedDeviceKind::capacitor:
        return "capacitor";
    case PlacedDeviceKind::resistor:
        return "resistor";
    case PlacedDeviceKind::diode:
        return "diode";
    case PlacedDeviceKind::inductor:
        return "inductor";
    case PlacedDeviceKind::unknown:
        break;
    }
    return "unknown";
}

std::string nodeForPin(const std::map<std::string, std::string> &byPin, std::initializer_list<std::string_view> names) {
    for (const auto name : names) {
        const auto it = byPin.find(lowerCopy(std::string(name)));
        if (it != byPin.end())
            return it->second;
    }
    return {};
}

std::string makeDeviceDesignator(const std::string &name, char letter, std::set<std::string> &used) {
    std::string base = trimCopy(name);
    if (base.empty())
        base = std::string(1, letter);
    if (static_cast<char>(std::toupper(static_cast<unsigned char>(base.front()))) !=
        static_cast<char>(std::toupper(static_cast<unsigned char>(letter))))
        base.insert(base.begin(), letter);
    std::string candidate = base;
    for (int suffix = 2; used.contains(lowerCopy(candidate)); ++suffix)
        candidate = base + "_" + std::to_string(suffix);
    used.insert(lowerCopy(candidate));
    return candidate;
}

std::string appendAttribute(std::string line, const std::string &name, const std::string &value) {
    if (!value.empty())
        line += " " + name + "=" + value;
    return line;
}

TerminalMatch findTerminalGroup(const Rect &rectangle, ObjectId layer,
                                const std::vector<std::vector<NetObjectGeometry>> &netGeometry,
                                const std::map<ObjectId, std::string> &layerKeys) {
    TerminalMatch result;
    const auto keyIt = layerKeys.find(layer);
    if (keyIt == layerKeys.end() || keyIt->second.empty())
        return result;
    for (std::size_t group = 0; group < netGeometry.size(); ++group) {
        bool groupMatches = false;
        for (const auto &geometry : netGeometry[group]) {
            const auto geometryKey = layerKeys.find(geometry.layer);
            if (geometryKey == layerKeys.end() || geometryKey->second != keyIt->second)
                continue;
            if (rectanglesOverlapOrTouch(rectangle, geometry.rectangle)) {
                groupMatches = true;
                break;
            }
        }
        if (!groupMatches)
            continue;
        if (result.matches == 0)
            result.group = group;
        ++result.matches;
    }
    return result;
}

std::optional<PexDevice>
extractPlacedDevice(const LayoutCell &parent, const LayoutInstance &instance, const LayoutCell &child,
                    const std::vector<std::vector<NetObjectGeometry>> &netGeometry, const std::vector<PexNet> &nets,
                    const std::map<ObjectId, std::string> &layerKeys, SpiceNameAllocator &names,
                    std::set<std::string> &designators, std::vector<std::string> &notes) {
    const auto deviceName = child.device.empty() ? child.name : child.device;
    const auto model = parameterValue(child, {"model", "spice_model", "subckt"});
    const auto kind = classifyPlacedDevice(child, deviceName, model);
    if (child.ports.empty()) {
        if (!model.empty() || !child.device.empty())
            notes.push_back("Placed instance '" + instance.name + "' has no terminal pins and was not extracted.");
        return std::nullopt;
    }
    if (kind == PlacedDeviceKind::unknown) {
        notes.push_back("Placed instance '" + instance.name + "' (" + deviceName +
                        ") is not a recognized device and was skipped.");
        return std::nullopt;
    }

    const Transform transform{instance.x, instance.y, instance.orientation};
    std::vector<std::pair<std::string, std::string>> pins;
    std::map<std::string, std::string> byPin;
    for (const auto &[_, port] : child.ports) {
        const auto rectangle = transformRectangle(port.rectangle, transform);
        std::string node;
        if (rectangle.valid()) {
            const auto match = findTerminalGroup(rectangle, port.layer, netGeometry, layerKeys);
            if (match.matches > 0 && match.group < nets.size()) {
                node = nets[match.group].node;
                if (match.matches > 1)
                    notes.push_back("Device '" + instance.name + "' terminal '" + port.name +
                                    "' overlaps multiple parent nets; using net '" + nets[match.group].name + "'.");
            }
        }
        if (node.empty()) {
            node = names.unique(instance.name + "_" + port.name);
            notes.push_back("Device '" + instance.name + "' terminal '" + port.name +
                            "' is not connected to a parent net; using node '" + node + "'.");
        }
        pins.emplace_back(port.name, node);
        byPin[lowerCopy(port.name)] = node;
    }

    PexDevice device;
    device.instance = instance.id;
    device.name = instance.name;
    device.type = placedDeviceKindName(kind);
    device.model = model;

    const auto effectivePdk = parent.pdk.empty() || parent.pdk == genericPdkId ? child.pdk : parent.pdk;
    const bool pdkSubckt = !effectivePdk.empty() && effectivePdk != genericPdkId && !model.empty();

    if (kind == PlacedDeviceKind::mos) {
        const auto drain = nodeForPin(byPin, {"D", "DRAIN"});
        const auto gate = nodeForPin(byPin, {"G", "GATE"});
        const auto source = nodeForPin(byPin, {"S", "SOURCE"});
        auto bulk = nodeForPin(byPin, {"B", "BULK", "BODY", "SUB"});
        if (drain.empty() || gate.empty() || source.empty()) {
            notes.push_back("MOS device '" + instance.name + "' is missing D/G/S pins and was skipped.");
            return std::nullopt;
        }
        if (bulk.empty()) {
            bulk = "0";
            notes.push_back("MOS device '" + instance.name + "' has no bulk pin; bulk tied to node 0.");
        }
        const auto modelName =
            model.empty() ? (containsAny(lowerCopy(deviceName), {"pmos", "pfet"}) ? "PMOS" : "NMOS") : model;
        if (model.empty())
            notes.push_back("MOS device '" + instance.name + "' has no model parameter; using " + modelName + ".");
        const char letter = pdkSubckt ? 'X' : 'M';
        auto line = makeDeviceDesignator(instance.name, letter, designators);
        line += " " + drain + " " + gate + " " + source + " " + bulk + " " + modelName;
        const auto width = parameterValue(child, {"w", "width"});
        const auto length = parameterValue(child, {"l", "length"});
        const auto multiplier = parameterValue(child, {"m", "mult"});
        if (pdkSubckt) {
            line = appendAttribute(std::move(line), "w", width);
            line = appendAttribute(std::move(line), "l", length);
            line = appendAttribute(std::move(line), "m", multiplier);
        } else {
            line = appendAttribute(std::move(line), "W", width);
            line = appendAttribute(std::move(line), "L", length);
            line = appendAttribute(std::move(line), "M", multiplier);
        }
        device.pins = {"D", "G", "S", "B"};
        device.nodes = {drain, gate, source, bulk};
        device.spiceLine = std::move(line);
        return device;
    }

    if (kind == PlacedDeviceKind::bjt) {
        const auto collector = nodeForPin(byPin, {"C", "COLLECTOR"});
        const auto base = nodeForPin(byPin, {"B", "BASE"});
        const auto emitter = nodeForPin(byPin, {"E", "EMITTER"});
        if (collector.empty() || base.empty() || emitter.empty()) {
            notes.push_back("BJT device '" + instance.name + "' is missing C/B/E pins and was skipped.");
            return std::nullopt;
        }
        const auto modelName = model.empty() ? (containsAny(lowerCopy(deviceName), {"pnp"}) ? "PNP" : "NPN") : model;
        if (model.empty())
            notes.push_back("BJT device '" + instance.name + "' has no model parameter; using " + modelName + ".");
        const char letter = pdkSubckt ? 'X' : 'Q';
        auto line = makeDeviceDesignator(instance.name, letter, designators);
        line += " " + collector + " " + base + " " + emitter;
        if (pdkSubckt) {
            auto substrate = nodeForPin(byPin, {"S", "SUB", "BULK", "BN"});
            if (substrate.empty())
                substrate = "0";
            line += " " + substrate;
            device.pins = {"C", "B", "E", "SUB"};
            device.nodes = {collector, base, emitter, substrate};
        } else {
            device.pins = {"C", "B", "E"};
            device.nodes = {collector, base, emitter};
        }
        line += " " + modelName;
        line = appendAttribute(std::move(line), "m", parameterValue(child, {"m", "mult"}));
        if (!pdkSubckt)
            line = appendAttribute(std::move(line), "area", parameterValue(child, {"area"}));
        device.spiceLine = std::move(line);
        return device;
    }

    if (kind == PlacedDeviceKind::capacitor || kind == PlacedDeviceKind::resistor || kind == PlacedDeviceKind::diode ||
        kind == PlacedDeviceKind::inductor) {
        if (pins.size() < 2) {
            notes.push_back("Device '" + instance.name + "' has fewer than two terminals and was skipped.");
            return std::nullopt;
        }
        const auto firstPin = pins[0].first;
        const auto secondPin = pins[1].first;
        const auto first = pins[0].second;
        const auto second = pins[1].second;
        char letter = 'X';
        std::string value;
        if (kind == PlacedDeviceKind::capacitor) {
            letter = 'C';
            value = parameterValue(child, {"capacitance", "value", "c"});
        } else if (kind == PlacedDeviceKind::resistor) {
            letter = 'R';
            value = parameterValue(child, {"resistance", "value", "r"});
        } else if (kind == PlacedDeviceKind::inductor) {
            letter = 'L';
            value = parameterValue(child, {"inductance", "value", "l"});
        } else {
            letter = 'D';
            value = model.empty() ? "D" : model;
        }
        if (value.empty()) {
            notes.push_back("Device '" + instance.name + "' (" + placedDeviceKindName(kind) +
                            ") has no value/model parameter and was skipped.");
            return std::nullopt;
        }
        auto line = makeDeviceDesignator(instance.name, letter, designators);
        line += " " + first + " " + second + " " + value;
        line = appendAttribute(std::move(line), "m", parameterValue(child, {"m", "mult"}));
        if (kind == PlacedDeviceKind::diode)
            line = appendAttribute(std::move(line), "area", parameterValue(child, {"area"}));
        if (kind == PlacedDeviceKind::capacitor && pins.size() > 2) {
            std::string ignored;
            for (std::size_t index = 2; index < pins.size(); ++index) {
                if (!ignored.empty())
                    ignored += ", ";
                ignored += pins[index].first;
            }
            notes.push_back("Capacitor device '" + instance.name + "' ignored extra pin(s): " + ignored + ".");
        }
        device.pins = {firstPin, secondPin};
        device.nodes = {first, second};
        device.spiceLine = std::move(line);
        return device;
    }

    return std::nullopt;
}

std::string nextDeviceDesignator(char letter, std::set<std::string> &used) {
    for (int index = 1;; ++index) {
        const auto candidate = std::string(1, letter) + std::to_string(index);
        if (used.contains(lowerCopy(candidate)))
            continue;
        used.insert(lowerCopy(candidate));
        return candidate;
    }
}

Polygon toGeometryPolygon(const std::vector<LayoutPoint> &points) {
    Polygon polygon;
    polygon.reserve(points.size());
    for (const auto &point : points)
        polygon.push_back({point.x, point.y});
    return polygon;
}

std::vector<LayoutPoint> toLayoutPoints(const Polygon &polygon) {
    std::vector<LayoutPoint> points;
    points.reserve(polygon.size());
    for (const auto &point : polygon)
        points.push_back({point.x, point.y});
    return points;
}

std::optional<Polygon> shapeOutline(const Shape &shape) {
    if (shape.isText())
        return std::nullopt;
    if (shape.polygon.size() >= 3)
        return toGeometryPolygon(shape.polygon);
    if (!shape.rectangle.valid())
        return std::nullopt;
    return Polygon{{shape.rectangle.x, shape.rectangle.y},
                   {shape.rectangle.x + shape.rectangle.width, shape.rectangle.y},
                   {shape.rectangle.x + shape.rectangle.width, shape.rectangle.y + shape.rectangle.height},
                   {shape.rectangle.x, shape.rectangle.y + shape.rectangle.height}};
}

Polygon rectangleOutline(const Rect &rectangle) {
    return Polygon{{rectangle.x, rectangle.y},
                   {rectangle.x + rectangle.width, rectangle.y},
                   {rectangle.x + rectangle.width, rectangle.y + rectangle.height},
                   {rectangle.x, rectangle.y + rectangle.height}};
}

double polygonAreaUm2(const Polygon &polygon, const DatabaseUnits &units) {
    if (polygon.size() < 3)
        return 0.0;
    long double twiceArea = 0.0L;
    for (std::size_t index = 0; index < polygon.size(); ++index) {
        const auto &current = polygon[index];
        const auto &next = polygon[(index + 1) % polygon.size()];
        twiceArea += static_cast<long double>(units.toNanometers(current.x)) * units.toNanometers(next.y) -
                     static_cast<long double>(units.toNanometers(next.x)) * units.toNanometers(current.y);
    }
    return std::abs(static_cast<double>(twiceArea)) * 0.5 / 1'000'000.0;
}

double multiAreaUm2(const MultiPolygon &polygons, const DatabaseUnits &units) {
    double area = 0.0;
    for (const auto &polygon : polygons)
        area += polygonAreaUm2(polygon, units);
    return area;
}

Rect multiBounds(const MultiPolygon &polygons) {
    if (polygons.empty())
        return {};
    auto bounds = GeometryKernel::bounds(polygons.front());
    if (!bounds.valid())
        return {};
    auto minX = bounds.x;
    auto minY = bounds.y;
    auto maxX = bounds.x + bounds.width;
    auto maxY = bounds.y + bounds.height;
    for (std::size_t index = 1; index < polygons.size(); ++index) {
        const auto next = GeometryKernel::bounds(polygons[index]);
        if (!next.valid())
            continue;
        minX = std::min(minX, next.x);
        minY = std::min(minY, next.y);
        maxX = std::max(maxX, next.x + next.width);
        maxY = std::max(maxY, next.y + next.height);
    }
    return {minX, minY, maxX - minX, maxY - minY};
}

std::optional<std::string> extractionLayerKey(const Project &project, const std::string &name) {
    const auto wanted = lowerCopy(name);
    std::optional<std::string> exact;
    std::optional<std::string> base;
    std::optional<std::string> any;
    for (const auto &[_, layer] : project.layers) {
        auto full = lowerCopy(layer.name);
        auto baseName = full;
        std::string purpose;
        if (const auto dot = full.rfind('.'); dot != std::string::npos) {
            baseName = full.substr(0, dot);
            purpose = full.substr(dot + 1);
        }
        const auto key = layoutLayerConnectivityKey(layer);
        const bool drawing = purpose.empty() || purpose == "drawing";
        if (full == wanted) {
            if (drawing && !exact)
                exact = key;
            else if (!any)
                any = key;
        }
        if (baseName == wanted && drawing && !base)
            base = key;
    }
    if (exact)
        return exact;
    if (base)
        return base;
    return any;
}

struct ExtractionShape {
    ObjectId id = 0;
    ObjectId layer = 0;
    Polygon polygon;
    Rect bounds;
    std::size_t group = static_cast<std::size_t>(-1);
    ObjectId net = 0;
    bool port = false;
};

struct ExtractionGeometry {
    std::vector<ExtractionShape> shapes;
    std::map<std::string, std::vector<std::size_t>> byKey;
    std::map<std::string, std::vector<std::size_t>> terminals;
};

ExtractionGeometry buildExtractionGeometry(const Project &project, const LayoutCell &layout,
                                           const std::vector<LayoutNetGroup> &groups) {
    ExtractionGeometry geometry;
    std::map<ObjectId, std::size_t> shapeGroup;
    std::map<ObjectId, std::size_t> portGroup;
    for (std::size_t index = 0; index < groups.size(); ++index) {
        for (const auto shape : groups[index].shapes)
            shapeGroup.emplace(shape, index);
        for (const auto port : groups[index].ports)
            portGroup.emplace(port, index);
    }

    const auto addShape = [&](ObjectId id, ObjectId layer, Polygon polygon, std::size_t group, ObjectId net,
                              bool port) {
        if (polygon.size() < 3)
            return;
        const auto layerIt = project.layers.find(layer);
        if (layerIt == project.layers.end())
            return;
        ExtractionShape shape;
        shape.id = id;
        shape.layer = layer;
        shape.polygon = std::move(polygon);
        shape.bounds = GeometryKernel::bounds(shape.polygon);
        shape.group = group;
        shape.net = net;
        shape.port = port;
        const auto key = layoutLayerConnectivityKey(layerIt->second);
        const auto index = geometry.shapes.size();
        geometry.shapes.push_back(std::move(shape));
        if (!port)
            geometry.byKey[key].push_back(index);
        if (group < groups.size())
            geometry.terminals[key].push_back(index);
    };

    for (const auto &[id, shape] : layout.shapes) {
        const auto outline = shapeOutline(shape);
        if (!outline)
            continue;
        const auto group = shapeGroup.find(id);
        addShape(id, shape.layer, *outline, group == shapeGroup.end() ? groups.size() : group->second,
                 shape.net.value_or(0), false);
    }
    for (const auto &[id, port] : layout.ports) {
        if (!port.visible || !port.rectangle.valid())
            continue;
        const auto group = portGroup.find(id);
        addShape(id, port.layer, rectangleOutline(port.rectangle),
                 group == portGroup.end() ? groups.size() : group->second, port.net, true);
    }
    return geometry;
}

bool overlapsLayer(const ExtractionGeometry &geometry, const std::string &key, const Polygon &region,
                   Clipper2Adapter &kernel) {
    const auto shapes = geometry.byKey.find(key);
    if (shapes == geometry.byKey.end())
        return false;
    const MultiPolygon regionPolygons{region};
    for (const auto shapeIndex : shapes->second) {
        const auto &shape = geometry.shapes[shapeIndex];
        if (!rectanglesOverlapOrTouch(shape.bounds, GeometryKernel::bounds(region)))
            continue;
        if (!kernel.intersection({shape.polygon}, regionPolygons).empty())
            return true;
    }
    return false;
}

bool matchesRequiredLayers(const ExtractionGeometry &geometry, const std::vector<std::string> &requireKeys,
                           const std::vector<std::string> &excludeKeys, const Polygon &region,
                           Clipper2Adapter &kernel) {
    for (const auto &key : requireKeys) {
        if (!overlapsLayer(geometry, key, region, kernel))
            return false;
    }
    for (const auto &key : excludeKeys) {
        if (overlapsLayer(geometry, key, region, kernel))
            return false;
    }
    return true;
}

std::string netNodeForShape(const ExtractionShape &shape, const std::vector<PexNet> &nets,
                            const std::map<ObjectId, std::string> &netNodeById) {
    if (shape.net != 0) {
        const auto node = netNodeById.find(shape.net);
        if (node != netNodeById.end())
            return node->second;
    }
    if (shape.group < nets.size())
        return nets[shape.group].node;
    return {};
}

std::string bodyNodeForRegion(const ExtractionGeometry &geometry, const std::string &bodyKey, const Polygon &region,
                              const std::vector<PexNet> &nets, const std::map<ObjectId, std::string> &netNodeById,
                              const std::string &fallback, Clipper2Adapter &kernel) {
    if (bodyKey.empty())
        return fallback;
    const auto shapes = geometry.byKey.find(bodyKey);
    if (shapes == geometry.byKey.end())
        return fallback;
    const MultiPolygon regionPolygons{region};
    for (const auto shapeIndex : shapes->second) {
        const auto &shape = geometry.shapes[shapeIndex];
        if (!rectanglesOverlapOrTouch(shape.bounds, GeometryKernel::bounds(region)))
            continue;
        if (kernel.intersection({shape.polygon}, regionPolygons).empty())
            continue;
        const auto node = netNodeForShape(shape, nets, netNodeById);
        if (!node.empty())
            return node;
    }
    return fallback;
}

struct TerminalHit {
    std::string node;
    double area = 0.0;
    bool explicitNet = false;
};

std::vector<TerminalHit> terminalHits(const ExtractionGeometry &geometry, const std::vector<std::string> &keys,
                                      const MultiPolygon &region, const DatabaseUnits &units,
                                      const std::vector<PexNet> &nets,
                                      const std::map<ObjectId, std::string> &netNodeById, Clipper2Adapter &kernel) {
    std::map<std::string, TerminalHit> byNode;
    const auto regionBounds = multiBounds(region);
    for (const auto &key : keys) {
        const auto shapes = geometry.terminals.find(key);
        if (shapes == geometry.terminals.end())
            continue;
        for (const auto shapeIndex : shapes->second) {
            const auto &shape = geometry.shapes[shapeIndex];
            if (!rectanglesOverlapOrTouch(shape.bounds, regionBounds))
                continue;
            const auto intersection = kernel.intersection({shape.polygon}, region);
            const auto area = multiAreaUm2(intersection, units);
            if (area <= 0.0)
                continue;
            const auto node = netNodeForShape(shape, nets, netNodeById);
            if (node.empty())
                continue;
            auto &hit = byNode[node];
            hit.node = node;
            hit.area += area;
            hit.explicitNet = hit.explicitNet || shape.net != 0;
        }
    }
    std::vector<TerminalHit> hits;
    hits.reserve(byNode.size());
    for (auto &[_, hit] : byNode)
        hits.push_back(std::move(hit));
    std::sort(hits.begin(), hits.end(), [](const TerminalHit &left, const TerminalHit &right) {
        if (left.explicitNet != right.explicitNet)
            return left.explicitNet;
        if (left.area != right.area)
            return left.area > right.area;
        return left.node < right.node;
    });
    return hits;
}

Project buildDeviceAwareConnectivity(const Project &project, ObjectId cellId, const DeviceExtractionRules &rules) {
    Project connectivity;
    connectivity.identity = project.identity;
    connectivity.name = project.name;
    connectivity.units = project.units;
    connectivity.grid = project.grid;
    connectivity.layers = project.layers;
    const auto cell = project.layoutCells.find(cellId);
    if (cell == project.layoutCells.end())
        return connectivity;
    connectivity.layoutCells.emplace(cellId, cell->second);

    std::map<std::string, MultiPolygon> gatesByActiveKey;
    for (const auto &rule : rules.rules) {
        if (rule.type != "mos")
            continue;
        const auto activeKey = extractionLayerKey(project, rule.active);
        const auto gateKey = extractionLayerKey(project, rule.gate);
        if (!activeKey || !gateKey)
            continue;
        auto &gates = gatesByActiveKey[*activeKey];
        for (const auto &[_, shape] : cell->second.shapes) {
            const auto layer = project.layers.find(shape.layer);
            if (layer == project.layers.end() || layoutLayerConnectivityKey(layer->second) != *gateKey)
                continue;
            const auto outline = shapeOutline(shape);
            if (outline)
                gates.push_back(*outline);
        }
    }
    if (gatesByActiveKey.empty())
        return connectivity;

    Clipper2Adapter kernel;
    auto &copyCell = connectivity.layoutCells.at(cellId);
    for (const auto &[activeKey, gates] : gatesByActiveKey) {
        if (gates.empty())
            continue;
        const auto gateUnion = kernel.booleanOp(gates, {}, BooleanOperation::union_);
        std::vector<ObjectId> activeShapes;
        for (const auto &[id, shape] : cell->second.shapes) {
            const auto layer = project.layers.find(shape.layer);
            if (layer == project.layers.end() || layoutLayerConnectivityKey(layer->second) != activeKey)
                continue;
            if (!shapeOutline(shape))
                continue;
            activeShapes.push_back(id);
        }
        for (const auto id : activeShapes) {
            const auto original = cell->second.shapes.find(id);
            if (original == cell->second.shapes.end())
                continue;
            const auto outline = shapeOutline(original->second);
            if (!outline)
                continue;
            const auto fragments = kernel.difference({*outline}, gateUnion);
            copyCell.shapes.erase(id);
            for (const auto &fragment : fragments) {
                const auto bounds = GeometryKernel::bounds(fragment);
                if (!bounds.valid())
                    continue;
                const auto newId = connectivity.nextId();
                Shape replacement;
                replacement.id = newId;
                replacement.layer = original->second.layer;
                replacement.polygon = toLayoutPoints(fragment);
                replacement.rectangle = bounds;
                replacement.name = original->second.name;
                copyCell.shapes.emplace(newId, std::move(replacement));
            }
        }
    }
    return connectivity;
}

std::vector<PexDevice> extractPolygonDevices(const Project &project, const LayoutCell &layout,
                                             const std::vector<LayoutNetGroup> &groups, const std::vector<PexNet> &nets,
                                             const std::map<ObjectId, std::string> &netNodeById,
                                             const DeviceExtractionRules &rules, std::set<std::string> &designators,
                                             std::vector<std::string> &notes) {
    std::vector<PexDevice> devices;
    if (rules.rules.empty())
        return devices;
    auto geometry = buildExtractionGeometry(project, layout, groups);
    Clipper2Adapter kernel;
    std::map<std::string, int> counters;
    std::set<std::string> seen;

    const auto resolveKeys = [&](const std::vector<std::string> &names, std::vector<std::string> &keys,
                                 const std::string &ruleName, const char *label, bool warn) {
        for (const auto &name : names) {
            if (const auto key = extractionLayerKey(project, name))
                keys.push_back(*key);
            else if (warn)
                notes.push_back("Extraction rule '" + ruleName + "' references missing " + label + " layer '" + name +
                                "'.");
        }
    };

    for (const auto &rule : rules.rules) {
        if (rule.type == "mos") {
            const auto activeKey = extractionLayerKey(project, rule.active);
            const auto gateKey = extractionLayerKey(project, rule.gate);
            if (!activeKey || !gateKey) {
                notes.push_back("Extraction rule '" + rule.name + "' references a missing active/gate layer.");
                continue;
            }
            std::vector<std::string> terminalKeys;
            resolveKeys(rule.terminalLayers, terminalKeys, rule.name, "terminal", true);
            if (terminalKeys.empty())
                continue;
            std::vector<std::string> requireKeys;
            std::vector<std::string> excludeKeys;
            resolveKeys(rule.requireLayers, requireKeys, rule.name, "required", false);
            resolveKeys(rule.excludeLayers, excludeKeys, rule.name, "excluded", false);
            bool missingRequired = false;
            for (const auto &name : rule.requireLayers) {
                if (!extractionLayerKey(project, name)) {
                    missingRequired = true;
                    break;
                }
            }
            if (missingRequired)
                continue;
            const auto bodyKey =
                rule.bodyLayer.empty() ? std::optional<std::string>{} : extractionLayerKey(project, rule.bodyLayer);

            const auto activeShapes = geometry.byKey.find(*activeKey);
            const auto gateShapes = geometry.byKey.find(*gateKey);
            if (activeShapes == geometry.byKey.end() || gateShapes == geometry.byKey.end())
                continue;
            bool terminalWarning = false;
            for (const auto gateIndex : gateShapes->second) {
                const auto &gate = geometry.shapes[gateIndex];
                const auto gateNode = netNodeForShape(gate, nets, netNodeById);
                if (gateNode.empty())
                    continue;
                for (const auto activeIndex : activeShapes->second) {
                    const auto &active = geometry.shapes[activeIndex];
                    if (!rectanglesOverlapOrTouch(active.bounds, gate.bounds))
                        continue;
                    const auto channels = kernel.intersection({active.polygon}, {gate.polygon});
                    for (const auto &channel : channels) {
                        const auto channelBounds = GeometryKernel::bounds(channel);
                        if (!channelBounds.valid())
                            continue;
                        if (!matchesRequiredLayers(geometry, requireKeys, excludeKeys, channel, kernel))
                            continue;
                        const auto dedupeKey =
                            gateNode + "|" + std::to_string(channelBounds.x) + "," + std::to_string(channelBounds.y) +
                            "," + std::to_string(channelBounds.width) + "," + std::to_string(channelBounds.height);
                        if (!seen.insert(dedupeKey).second)
                            continue;

                        auto axis = lowerCopy(rule.lengthAxis);
                        if (axis != "x" && axis != "y")
                            axis = gate.bounds.width <= gate.bounds.height ? "x" : "y";
                        const auto channelCenter = axis == "x" ? channelBounds.x + channelBounds.width / 2.0
                                                               : channelBounds.y + channelBounds.height / 2.0;

                        struct Side {
                            bool have = false;
                            Polygon polygon;
                            double distance = std::numeric_limits<double>::max();
                            double area = 0.0;
                        };
                        Side low;
                        Side high;
                        const auto remainders = kernel.difference({active.polygon}, {gate.polygon});
                        for (const auto &remainder : remainders) {
                            const auto bounds = GeometryKernel::bounds(remainder);
                            if (!bounds.valid())
                                continue;
                            const auto center =
                                axis == "x" ? bounds.x + bounds.width / 2.0 : bounds.y + bounds.height / 2.0;
                            const auto distance = std::abs(center - channelCenter);
                            const auto area = polygonAreaUm2(remainder, project.units);
                            Side &side = center <= channelCenter ? low : high;
                            if (!side.have || distance < side.distance ||
                                (distance == side.distance && area > side.area)) {
                                side.have = true;
                                side.polygon = remainder;
                                side.distance = distance;
                                side.area = area;
                            }
                        }
                        if (!low.have || !high.have) {
                            if (!terminalWarning) {
                                notes.push_back("Extraction rule '" + rule.name +
                                                "' could not split an active region into source/drain sides.");
                                terminalWarning = true;
                            }
                            continue;
                        }
                        const auto lowHits = terminalHits(geometry, terminalKeys, {low.polygon}, project.units, nets,
                                                          netNodeById, kernel);
                        const auto highHits = terminalHits(geometry, terminalKeys, {high.polygon}, project.units, nets,
                                                           netNodeById, kernel);
                        const auto pickHit = [](const std::vector<TerminalHit> &hits) -> std::string {
                            for (const auto &hit : hits) {
                                if (hit.explicitNet)
                                    return hit.node;
                            }
                            return hits.empty() ? std::string{} : hits.front().node;
                        };
                        const auto sourceNode = pickHit(lowHits);
                        const auto drainNode = pickHit(highHits);
                        if (sourceNode.empty() || drainNode.empty()) {
                            if (!terminalWarning) {
                                notes.push_back("Extraction rule '" + rule.name +
                                                "' found no source/drain terminal net for a channel; "
                                                "assign nets or add contacts/metal.");
                                terminalWarning = true;
                            }
                            continue;
                        }
                        if (lowerCopy(sourceNode) == lowerCopy(drainNode)) {
                            if (!terminalWarning) {
                                notes.push_back("Extraction rule '" + rule.name +
                                                "' found the same net on both sides of a channel; skipping the shorted "
                                                "device.");
                                terminalWarning = true;
                            }
                            continue;
                        }
                        const auto bodyNode = bodyNodeForRegion(geometry, bodyKey ? *bodyKey : std::string{}, channel,
                                                                nets, netNodeById, rule.substrate, kernel);
                        const auto lengthDb = axis == "x" ? channelBounds.width : channelBounds.height;
                        const auto widthDb = axis == "x" ? channelBounds.height : channelBounds.width;
                        if (lengthDb <= 0 || widthDb <= 0)
                            continue;
                        const auto lengthUm = project.units.toNanometers(lengthDb) / 1000.0;
                        const auto widthUm = project.units.toNanometers(widthDb) / 1000.0;
                        if (lengthUm <= 0.0 || widthUm <= 0.0)
                            continue;
                        const auto element =
                            rule.element.empty()
                                ? 'X'
                                : static_cast<char>(std::toupper(static_cast<unsigned char>(rule.element.front())));
                        if (element != 'X' && element != 'M') {
                            notes.push_back("MOS extraction rule '" + rule.name + "' uses unsupported SPICE element '" +
                                            std::string(1, element) + "'.");
                            continue;
                        }
                        if (rule.model.empty()) {
                            notes.push_back("MOS extraction rule '" + rule.name + "' has no model.");
                            continue;
                        }
                        const auto designator = nextDeviceDesignator(element, designators);
                        std::string line = designator + " " + drainNode + " " + gateNode + " " + sourceNode + " " +
                                           bodyNode + " " + rule.model;
                        if (element == 'X') {
                            line += " w=" + formatNumber(widthUm) + "u l=" + formatNumber(lengthUm) + "u";
                        } else {
                            line += " W=" + formatNumber(widthUm) + "u L=" + formatNumber(lengthUm) + "u";
                        }
                        PexDevice device;
                        device.name = rule.name + "_" + std::to_string(++counters[rule.name]);
                        device.type = "mos";
                        device.model = rule.model;
                        device.pins = {"D", "G", "S", "B"};
                        device.nodes = {drainNode, gateNode, sourceNode, bodyNode};
                        device.spiceLine = std::move(line);
                        devices.push_back(std::move(device));
                    }
                }
            }
        } else if (rule.type == "capacitor") {
            if (rule.plates.size() < 2)
                continue;
            std::vector<std::string> requireKeys;
            std::vector<std::string> excludeKeys;
            resolveKeys(rule.requireLayers, requireKeys, rule.name, "required", false);
            resolveKeys(rule.excludeLayers, excludeKeys, rule.name, "excluded", false);
            bool missingRequired = false;
            for (const auto &name : rule.requireLayers) {
                if (!extractionLayerKey(project, name)) {
                    missingRequired = true;
                    break;
                }
            }
            if (missingRequired)
                continue;
            const auto firstKey = extractionLayerKey(project, rule.plates[0]);
            const auto secondKey = extractionLayerKey(project, rule.plates[1]);
            if (!firstKey || !secondKey || *firstKey == *secondKey) {
                notes.push_back("Extraction rule '" + rule.name + "' references a missing capacitor plate layer.");
                continue;
            }
            const auto firstShapes = geometry.byKey.find(*firstKey);
            const auto secondShapes = geometry.byKey.find(*secondKey);
            if (firstShapes == geometry.byKey.end() || secondShapes == geometry.byKey.end())
                continue;
            for (const auto firstIndex : firstShapes->second) {
                const auto &first = geometry.shapes[firstIndex];
                const auto firstNode = netNodeForShape(first, nets, netNodeById);
                if (firstNode.empty())
                    continue;
                for (const auto secondIndex : secondShapes->second) {
                    const auto &second = geometry.shapes[secondIndex];
                    if (!rectanglesOverlapOrTouch(first.bounds, second.bounds))
                        continue;
                    const auto secondNode = netNodeForShape(second, nets, netNodeById);
                    if (secondNode.empty() || lowerCopy(firstNode) == lowerCopy(secondNode))
                        continue;
                    const auto overlap = kernel.intersection({first.polygon}, {second.polygon});
                    const auto area = multiAreaUm2(overlap, project.units);
                    if (area <= 0.0)
                        continue;
                    bool requirementsMatch = false;
                    for (const auto &overlapPolygon : overlap) {
                        if (matchesRequiredLayers(geometry, requireKeys, excludeKeys, overlapPolygon, kernel)) {
                            requirementsMatch = true;
                            break;
                        }
                    }
                    if (!requirementsMatch)
                        continue;
                    const auto bounds = multiBounds(overlap);
                    const auto dedupeKey = rule.name + "|cap|" + firstNode + "|" + secondNode + "|" +
                                           std::to_string(bounds.x) + "," + std::to_string(bounds.y) + "," +
                                           std::to_string(bounds.width) + "," + std::to_string(bounds.height);
                    if (!seen.insert(dedupeKey).second)
                        continue;
                    const auto capacitance = area * rule.capacitancePerArea;
                    if (capacitance <= 0.0)
                        continue;
                    const auto element =
                        rule.element.empty()
                            ? 'C'
                            : static_cast<char>(std::toupper(static_cast<unsigned char>(rule.element.front())));
                    const auto designator = nextDeviceDesignator(element, designators);
                    const auto value = rule.value.empty() ? formatNumber(capacitance) + "f" : rule.value;
                    PexDevice device;
                    device.name = rule.name + "_" + std::to_string(++counters[rule.name]);
                    device.type = "capacitor";
                    device.model = rule.model;
                    device.pins = {"1", "2"};
                    device.nodes = {firstNode, secondNode};
                    device.spiceLine = designator + " " + firstNode + " " + secondNode + " " + value;
                    devices.push_back(std::move(device));
                }
            }
        } else if (rule.type == "resistor") {
            std::vector<std::string> requireKeys;
            std::vector<std::string> excludeKeys;
            resolveKeys(rule.requireLayers, requireKeys, rule.name, "required", false);
            resolveKeys(rule.excludeLayers, excludeKeys, rule.name, "excluded", false);
            bool missingRequired = false;
            for (const auto &name : rule.requireLayers) {
                if (!extractionLayerKey(project, name)) {
                    missingRequired = true;
                    break;
                }
            }
            if (missingRequired)
                continue;
            const auto bodyKey = extractionLayerKey(project, rule.body);
            if (!bodyKey) {
                notes.push_back("Extraction rule '" + rule.name + "' references a missing resistor body layer.");
                continue;
            }
            std::vector<std::string> terminalKeys;
            resolveKeys(rule.terminalLayers, terminalKeys, rule.name, "terminal", true);
            if (terminalKeys.empty())
                continue;
            const auto bodyShapes = geometry.byKey.find(*bodyKey);
            if (bodyShapes == geometry.byKey.end())
                continue;
            bool terminalWarning = false;
            for (const auto bodyIndex : bodyShapes->second) {
                const auto &body = geometry.shapes[bodyIndex];
                if (!matchesRequiredLayers(geometry, requireKeys, excludeKeys, body.polygon, kernel))
                    continue;
                const auto hits =
                    terminalHits(geometry, terminalKeys, {body.polygon}, project.units, nets, netNodeById, kernel);
                if (hits.size() < 2) {
                    if (!terminalWarning) {
                        notes.push_back("Resistor rule '" + rule.name +
                                        "' found fewer than two terminal nets; assign nets or add contacts/metal.");
                        terminalWarning = true;
                    }
                    continue;
                }
                const auto firstNode = hits[0].node;
                const auto secondNode = hits[1].node;
                if (lowerCopy(firstNode) == lowerCopy(secondNode))
                    continue;
                const auto dedupeKey = firstNode + "|" + secondNode + "|" + std::to_string(body.bounds.x) + "," +
                                       std::to_string(body.bounds.y) + "," + std::to_string(body.bounds.width) + "," +
                                       std::to_string(body.bounds.height);
                if (!seen.insert(dedupeKey).second)
                    continue;
                const auto length = std::max(body.bounds.width, body.bounds.height);
                const auto width = std::min(body.bounds.width, body.bounds.height);
                if (length <= 0 || width <= 0)
                    continue;
                const auto resistance =
                    rule.value.empty()
                        ? formatNumber(rule.sheetResistance * static_cast<double>(length) / static_cast<double>(width))
                        : rule.value;
                if (resistance.empty())
                    continue;
                const auto element =
                    rule.element.empty()
                        ? 'R'
                        : static_cast<char>(std::toupper(static_cast<unsigned char>(rule.element.front())));
                const auto designator = nextDeviceDesignator(element, designators);
                PexDevice device;
                device.name = rule.name + "_" + std::to_string(++counters[rule.name]);
                device.type = "resistor";
                device.model = rule.model;
                device.pins = {"1", "2"};
                device.nodes = {firstNode, secondNode};
                device.spiceLine = designator + " " + firstNode + " " + secondNode + " " + resistance;
                devices.push_back(std::move(device));
            }
        }
    }
    return devices;
}

double sheetResistanceFor(const ParsedLayer &layer, const PexOptions &options) {
    switch (layer.kind) {
    case LayerKind::poly:
        return options.polySheetResistance;
    case LayerKind::diffusion:
        return options.diffusionSheetResistance;
    case LayerKind::metal:
        return options.metalSheetResistance;
    case LayerKind::via:
    case LayerKind::ignored:
        break;
    }
    return 0.0;
}

std::string joinNames(const std::vector<std::string> &names) {
    std::ostringstream stream;
    for (std::size_t index = 0; index < names.size(); ++index) {
        if (index)
            stream << ", ";
        stream << names[index];
    }
    return stream.str();
}

std::vector<std::string> stringArrayJson(const std::vector<std::string> &values) {
    std::vector<std::string> result;
    result.reserve(values.size());
    for (const auto &value : values)
        result.push_back(value);
    return result;
}

} // namespace

PexReport runPex(Project &project, ObjectId layoutCellId, const PexOptions &options) {
    const auto layoutIt = project.layoutCells.find(layoutCellId);
    if (layoutIt == project.layoutCells.end())
        throw ProjectError("pex.layout", "Layout cell does not exist");
    const auto &layout = layoutIt->second;
    if (layout.library)
        throw ProjectError("pex.layout", "PEX extracts user layout cells, not read-only library cells");
    if (options.metalSheetResistance < 0.0 || options.polySheetResistance < 0.0 ||
        options.diffusionSheetResistance < 0.0 || options.viaResistance < 0.0 || options.areaCapacitance < 0.0 ||
        options.perimeterCapacitance < 0.0)
        throw ProjectError("pex.options", "PEX coefficients must be non-negative");

    std::optional<DeviceExtractionRules> extractionRules;
    std::string extractionRuleError;
    if (options.extractPolygonDevices) {
        if (!options.extractionRulesPath.empty()) {
            std::error_code status;
            if (!std::filesystem::is_regular_file(options.extractionRulesPath, status) || status)
                throw ProjectError("pex.extraction",
                                   "Extraction rules file not found: " + options.extractionRulesPath.string());
            extractionRules = loadDeviceExtractionRules(options.extractionRulesPath, &extractionRuleError);
        } else {
            extractionRules = loadDeviceExtractionRulesForPdk(layout.pdk, &extractionRuleError);
        }
        if (!extractionRuleError.empty())
            throw ProjectError("pex.extraction", extractionRuleError);
    }

    Project connectivityProject;
    const Project *connectivitySource = &project;
    if (extractionRules && !extractionRules->rules.empty()) {
        connectivityProject = buildDeviceAwareConnectivity(project, layoutCellId, *extractionRules);
        connectivitySource = &connectivityProject;
    }
    const auto &connectivityLayout = connectivitySource->layoutCells.at(layoutCellId);
    const auto groups = extractLayoutNets(*connectivitySource, layoutCellId);
    if (groups.empty())
        throw ProjectError("pex.geometry", "Layout has no shapes or pins to extract");

    PexReport report;
    report.layoutCell = layoutCellId;
    report.layout = layout.name;
    report.pdk = layout.pdk;

    std::set<std::string> skippedLayers;
    std::vector<std::string> notes;
    for (const auto &group : groups) {
        PexNet net;
        net.net = group.net;
        net.name = group.name.empty() ? "N" + std::to_string(group.net) : group.name;

        for (const auto shapeId : group.shapes) {
            const auto shapeIt = connectivityLayout.shapes.find(shapeId);
            if (shapeIt == connectivityLayout.shapes.end())
                continue;
            const auto &shape = shapeIt->second;
            if (shape.isText())
                continue;
            const auto layerIt = project.layers.find(shape.layer);
            if (layerIt == project.layers.end()) {
                notes.push_back("Shape " + std::to_string(shapeId) + " references a missing layer");
                continue;
            }
            const auto layer = parseLayer(layerIt->second.name);
            if (!isDrawingPurpose(layer))
                continue;
            if (layer.kind == LayerKind::ignored) {
                skippedLayers.insert(layerIt->second.name);
                continue;
            }
            const auto metrics = shapeMetrics(shape, project.units);
            net.area += metrics.area;
            net.perimeter += metrics.perimeter;
            net.capacitance +=
                options.areaCapacitance * metrics.area + options.perimeterCapacitance * metrics.perimeter;
            if (layer.kind == LayerKind::via) {
                ++net.viaCount;
                net.resistance += options.viaResistance;
            } else {
                net.resistance += sheetResistanceFor(layer, options) * metrics.squares;
            }
        }

        std::set<std::string> portNames;
        for (const auto portId : group.ports) {
            const auto portIt = connectivityLayout.ports.find(portId);
            if (portIt == connectivityLayout.ports.end() || !portIt->second.visible)
                continue;
            portNames.insert(portIt->second.name);
        }
        net.ports.assign(portNames.begin(), portNames.end());

        report.totalResistance += net.resistance;
        report.totalCapacitance += net.capacitance;
        report.nets.push_back(std::move(net));
    }

    if (!skippedLayers.empty()) {
        std::ostringstream note;
        note << "Skipped non-conducting or unrecognized layers: ";
        bool first = true;
        for (const auto &layer : skippedLayers) {
            if (!first)
                note << ", ";
            note << layer;
            first = false;
        }
        notes.push_back(note.str());
    }
    if (report.totalResistance == 0.0 && report.totalCapacitance == 0.0)
        notes.push_back("No conductor geometry was recognized; use layers named Metal*, GatPoly, Activ, or Via*.");
    if (!layout.plannedDevices.empty())
        notes.push_back("Planned-device placeholders do not contribute geometry or devices to this extraction.");
    if (options.extractPolygonDevices) {
        if (!extractionRules) {
            if (!layout.pdk.empty() && layout.pdk != genericPdkId)
                notes.push_back("No PDK extraction rules found for '" + layout.pdk +
                                "'; polygon devices were not extracted.");
        } else if (extractionRules->rules.empty()) {
            notes.push_back("The PDK extraction rule file contains no device rules.");
        }
    }

    // Reserve every visible pin name first so a net without a pin cannot take a
    // name that later needs to be used for an actual layout pin.
    SpiceNameAllocator names;
    std::map<ObjectId, std::string> portNodeById;
    std::vector<std::pair<ObjectId, const Port *>> visiblePorts;
    for (const auto &[id, port] : connectivityLayout.ports) {
        if (port.visible)
            visiblePorts.emplace_back(id, &port);
    }
    std::sort(visiblePorts.begin(), visiblePorts.end(), [](const auto &left, const auto &right) {
        const auto leftName = lowerCopy(left.second->name);
        const auto rightName = lowerCopy(right.second->name);
        if (leftName != rightName)
            return leftName < rightName;
        return left.first < right.first;
    });
    for (const auto &[id, port] : visiblePorts)
        portNodeById[id] = names.unique(port->name);

    std::map<std::string, std::string> portNodeByName;
    for (const auto &[id, port] : visiblePorts)
        portNodeByName.emplace(port->name, portNodeById.at(id));

    for (auto &net : report.nets) {
        std::string matchingPort;
        if (!net.name.empty()) {
            for (const auto &port : net.ports) {
                if (lowerCopy(port) == lowerCopy(net.name)) {
                    matchingPort = port;
                    break;
                }
            }
        }
        if (!matchingPort.empty())
            net.node = portNodeByName.at(matchingPort);
        else if (net.net != 0 && !net.name.empty())
            net.node = names.unique(net.name);
        else if (!net.ports.empty())
            net.node = portNodeByName.at(net.ports.front());
        else
            net.node = names.unique(net.name.empty() ? "N" : net.name);
    }

    std::map<ObjectId, std::string> netNodeById;
    for (const auto &net : report.nets) {
        if (net.net != 0)
            netNodeById.emplace(net.net, net.node);
    }
    for (const auto &[netId, net] : layout.nets) {
        if (!netNodeById.contains(netId))
            netNodeById.emplace(netId, names.unique(net.name.empty() ? "N" + std::to_string(netId) : net.name));
    }

    // Build a geometry lookup aligned with the extracted nets so placed-device
    // terminal pins can be matched to the parent net that touches them.
    std::vector<std::vector<NetObjectGeometry>> netGeometry(groups.size());
    for (std::size_t index = 0; index < groups.size(); ++index) {
        const auto &group = groups[index];
        for (const auto shapeId : group.shapes) {
            const auto shape = connectivityLayout.shapes.find(shapeId);
            if (shape == connectivityLayout.shapes.end() || shape->second.isText() || !shape->second.rectangle.valid())
                continue;
            netGeometry[index].push_back({shape->second.layer, shape->second.rectangle});
        }
        for (const auto portId : group.ports) {
            const auto port = connectivityLayout.ports.find(portId);
            if (port == connectivityLayout.ports.end() || !port->second.visible || !port->second.rectangle.valid())
                continue;
            netGeometry[index].push_back({port->second.layer, port->second.rectangle});
        }
    }

    std::set<std::string> designators;
    const auto placedDeviceStart = report.devices.size();
    if (options.extractPlacedDevices) {
        std::map<ObjectId, std::string> layerKeys;
        for (const auto &[id, layer] : project.layers)
            layerKeys.emplace(id, layoutLayerConnectivityKey(layer));
        for (const auto &[_, instance] : layout.instances) {
            const auto child = project.layoutCells.find(instance.cell);
            if (child == project.layoutCells.end()) {
                notes.push_back("Placed instance '" + instance.name +
                                "' references a missing layout cell and was skipped.");
                continue;
            }
            auto device = extractPlacedDevice(layout, instance, child->second, netGeometry, report.nets, layerKeys,
                                              names, designators, notes);
            if (device)
                report.devices.push_back(std::move(*device));
        }
    } else if (!layout.instances.empty()) {
        notes.push_back("Placed layout device extraction is disabled.");
    }
    const auto placedDeviceCount = report.devices.size() - placedDeviceStart;

    std::size_t polygonDeviceCount = 0;
    if (options.extractPolygonDevices && extractionRules && !extractionRules->rules.empty()) {
        auto polygonDevices = extractPolygonDevices(project, layout, groups, report.nets, netNodeById, *extractionRules,
                                                    designators, notes);
        polygonDeviceCount = polygonDevices.size();
        for (auto &device : polygonDevices)
            report.devices.push_back(std::move(device));
    }
    report.devicesFromLayout = !report.devices.empty();
    if (report.devicesFromLayout) {
        notes.push_back("Extracted " + std::to_string(placedDeviceCount) + " placed layout device(s) and " +
                        std::to_string(polygonDeviceCount) +
                        " polygon device(s); linked schematic devices were not copied.");
    } else if (!layout.instances.empty()) {
        notes.push_back("No placed layout devices were recognized; the linked schematic netlist is used if available.");
    }

    // Fall back to the inferred source schematic only when no layout devices
    // were extracted. This avoids duplicating devices that already came from
    // the physical layout.
    std::vector<std::string> linkedLines;
    std::string linkedSchematicName;
    if (report.devices.empty() && options.includeLinkedSchematic) {
        const auto linkedId = schematicForLayout(project, layoutCellId);
        const auto linked = project.schematicCells.find(linkedId);
        const auto generatedPex = linked != project.schematicCells.end() &&
                                  linked->second.spiceText.find("* ICE PEX layout-cell:") != std::string::npos;
        if (linkedId != 0 && linked != project.schematicCells.end() && !generatedPex) {
            linkedSchematicName = linked->second.name;
            std::vector<std::string> linkedNotes;
            linkedLines = buildSimulationNetlist(project, linkedId, &linkedNotes);
            for (const auto &note : linkedNotes)
                notes.push_back("Linked schematic: " + note);
            linkedLines.erase(std::remove_if(linkedLines.begin(), linkedLines.end(),
                                             [](const std::string &line) {
                                                 const auto lower = lowerCopy(line);
                                                 return lower.starts_with(".title") || lower == ".end";
                                             }),
                              linkedLines.end());
            if (!linkedLines.empty()) {
                report.devicesFromSchematic = true;
                notes.push_back("Device netlist copied from linked schematic '" + linkedSchematicName +
                                "'; devices were not extracted from layout geometry. Verify connectivity with LVS.");
            }
        }
    }

    const auto marker = "* ICE PEX layout-cell: " + std::to_string(layoutCellId);
    auto existing = project.schematicCells.end();
    for (auto it = project.schematicCells.begin(); it != project.schematicCells.end(); ++it) {
        if (it->second.textSchematic && it->second.spiceText.find(marker) != std::string::npos) {
            existing = it;
            break;
        }
    }

    std::string requestedName = options.schematicName;
    if (requestedName.empty())
        requestedName = layout.name.empty() ? std::string("PEX") : layout.name + " PEX";
    requestedName = trimCopy(std::move(requestedName));
    if (requestedName.empty())
        requestedName = "PEX";

    const bool updateExisting = existing != project.schematicCells.end() &&
                                (options.schematicName.empty() || existing->second.name == requestedName);
    std::string name = updateExisting ? existing->second.name : requestedName;
    if (!updateExisting) {
        const auto nameTaken = [&](const std::string &candidate) {
            return std::any_of(project.schematicCells.begin(), project.schematicCells.end(),
                               [&](const auto &entry) { return entry.second.name == candidate; });
        };
        for (int suffix = 2; nameTaken(name); ++suffix)
            name = requestedName + " " + std::to_string(suffix);
    }

    std::ostringstream text;
    text << "* ICE PEX extraction of layout '" << layout.name << "'\n";
    text << marker << "\n";
    text << "* PDK: " << (layout.pdk.empty() ? genericPdkId : layout.pdk) << "\n";
    text << "* First-order lumped estimate. Units are ohm and fF.\n";
    text << "* Coefficients: metal=" << formatNumber(options.metalSheetResistance)
         << " ohm/sq, poly=" << formatNumber(options.polySheetResistance)
         << " ohm/sq, diffusion=" << formatNumber(options.diffusionSheetResistance)
         << " ohm/sq, via=" << formatNumber(options.viaResistance)
         << " ohm, area=" << formatNumber(options.areaCapacitance)
         << " fF/um^2, perimeter=" << formatNumber(options.perimeterCapacitance) << " fF/um\n";
    text << "* Extracted nets: " << report.nets.size() << "\n";
    text << "* Extracted layout devices: " << report.devices.size() << "\n";
    text << "* Device source: "
         << (report.devicesFromLayout ? "layout" : (report.devicesFromSchematic ? "linked schematic" : "none")) << "\n";
    for (const auto &note : notes)
        text << "* Note: " << note << "\n";
    text << "* Layout net names are top-level SPICE nodes (pin names are used when a net has no explicit name).\n";
    text << "* Add sources or devices in a testbench as needed.\n";
    text << ".title " << name << "\n";
    if (!report.devices.empty()) {
        text << "\n* Devices extracted from placed layout instances.\n";
        for (const auto &device : report.devices)
            text << device.spiceLine << "\n";
    } else if (!linkedLines.empty()) {
        text << "\n* Devices from linked schematic '" << linkedSchematicName
             << "' (copied, not extracted from layout geometry).\n";
        for (const auto &line : linkedLines)
            text << line << "\n";
    }
    text << "\n";

    int resistorIndex = 1;
    int capacitorIndex = 1;
    int internalNodeIndex = 1;
    for (const auto &net : report.nets) {
        text << "* Net " << net.name;
        if (!net.ports.empty())
            text << " (" << joinNames(net.ports) << ")";
        text << ": area=" << formatNumber(net.area) << " um^2, perimeter=" << formatNumber(net.perimeter)
             << " um, R=" << formatNumber(net.resistance) << " ohm, C=" << formatNumber(net.capacitance) << " fF\n";

        std::vector<std::string> terminals;
        for (const auto &port : net.ports) {
            const auto &portNode = portNodeByName.at(port);
            const bool sameAsNet = lowerCopy(portNode) == lowerCopy(net.node);
            const bool alreadyAdded =
                std::any_of(terminals.begin(), terminals.end(),
                            [&](const std::string &candidate) { return lowerCopy(candidate) == lowerCopy(portNode); });
            if (!sameAsNet && !alreadyAdded)
                terminals.push_back(portNode);
        }

        if (!terminals.empty()) {
            // For multiple pins, split the extracted net resistance so the
            // path between any two pins is approximately the extracted value.
            auto branchResistance = terminals.size() == 1 ? net.resistance : net.resistance / 2.0;
            if (branchResistance <= 0.0)
                branchResistance = 1.0e-6;
            for (const auto &terminal : terminals)
                text << "RPEX" << resistorIndex++ << " " << terminal << " " << net.node << " "
                     << formatNumber(branchResistance) << "\n";
            if (net.resistance <= 0.0)
                text << "* Ideal net tie: extracted resistance is zero.\n";
            if (net.capacitance > 0.0)
                text << "CPEX" << capacitorIndex++ << " " << net.node << " 0 " << formatNumber(net.capacitance)
                     << "f\n";
        } else if (net.resistance > 0.0 && net.capacitance > 0.0) {
            // A one-terminal (or pin-less) net has no second pin to carry a
            // series current. Model the extracted resistance in series with
            // the net capacitance to ground instead of leaving an open stub.
            const auto rcNode = names.unique("PEXRC" + std::to_string(internalNodeIndex++));
            text << "RPEX" << resistorIndex++ << " " << net.node << " " << rcNode << " " << formatNumber(net.resistance)
                 << "\n";
            text << "CPEX" << capacitorIndex++ << " " << rcNode << " 0 " << formatNumber(net.capacitance) << "f\n";
        } else {
            if (net.resistance > 0.0)
                text << "* R not represented: net '" << net.name
                     << "' has no second terminal and no capacitance branch.\n";
            if (net.capacitance > 0.0)
                text << "CPEX" << capacitorIndex++ << " " << net.node << " 0 " << formatNumber(net.capacitance)
                     << "f\n";
        }
        text << "\n";
    }
    text << ".end\n";

    report.notes = std::move(notes);
    report.spiceText = text.str();

    if (updateExisting) {
        existing->second.spiceText = report.spiceText;
        existing->second.pdk = layout.pdk.empty() ? genericPdkId : layout.pdk;
        existing->second.textSchematic = true;
        report.schematicCell = existing->first;
        report.schematic = existing->second.name;
        return report;
    }

    report.schematicCell =
        createTextSchematicCell(project, name, layout.pdk.empty() ? genericPdkId : layout.pdk, report.spiceText);
    report.schematic = name;
    return report;
}

std::string serializePexReport(const PexReport &report) {
    json output{{"schema_version", report.schemaVersion},
                {"layout_cell", report.layoutCell},
                {"layout", report.layout},
                {"pdk", report.pdk},
                {"schematic_cell", report.schematicCell},
                {"schematic", report.schematic},
                {"total_resistance", report.totalResistance},
                {"total_capacitance", report.totalCapacitance},
                {"nets", json::array()},
                {"devices", json::array()},
                {"devices_from_layout", report.devicesFromLayout},
                {"devices_from_schematic", report.devicesFromSchematic},
                {"notes", stringArrayJson(report.notes)},
                {"source", report.spiceText}};
    for (const auto &net : report.nets) {
        output["nets"].push_back({{"schema_version", net.schemaVersion},
                                  {"net", net.net == 0 ? json(nullptr) : json(net.net)},
                                  {"name", net.name},
                                  {"node", net.node},
                                  {"ports", stringArrayJson(net.ports)},
                                  {"area", net.area},
                                  {"perimeter", net.perimeter},
                                  {"resistance", net.resistance},
                                  {"capacitance", net.capacitance},
                                  {"via_count", net.viaCount}});
    }
    for (const auto &device : report.devices) {
        output["devices"].push_back({{"schema_version", device.schemaVersion},
                                     {"instance", device.instance},
                                     {"name", device.name},
                                     {"type", device.type},
                                     {"model", device.model},
                                     {"pins", stringArrayJson(device.pins)},
                                     {"nodes", stringArrayJson(device.nodes)},
                                     {"spice_line", device.spiceLine}});
    }
    return output.dump(2) + "\n";
}

} // namespace ice
