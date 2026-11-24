#include "icelib/connectivity.hpp"
#include "icelib/geometry.hpp"
#include "icelib/hdl.hpp"
#include "icelib/operations.hpp"
#include "icelib/spice.hpp"
#include "icelib/verification.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace ice {
namespace {
using json = nlohmann::ordered_json;

std::string trim(std::string text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::string lowerCopy(std::string text) {
    for (auto &character : text)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return text;
}

std::string normalizedIdentifier(const std::string &text) {
    std::string result;
    bool pendingSeparator = false;
    for (const auto character : text) {
        const auto value = static_cast<unsigned char>(character);
        if (std::isalnum(value)) {
            if (pendingSeparator && !result.empty())
                result.push_back('_');
            pendingSeparator = false;
            result.push_back(static_cast<char>(std::tolower(value)));
        } else {
            pendingSeparator = !result.empty();
        }
    }
    return result;
}

std::string normalizedValue(std::string text) {
    text = trim(std::move(text));
    std::string result;
    result.reserve(text.size());
    for (const auto character : text) {
        if (!std::isspace(static_cast<unsigned char>(character)))
            result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    }
    return result;
}

std::optional<double> parseQuantity(const std::string &text) {
    const auto normalized = normalizedValue(text);
    if (normalized.empty())
        return std::nullopt;
    char *end = nullptr;
    const auto number = std::strtod(normalized.c_str(), &end);
    if (end == normalized.c_str())
        return std::nullopt;
    std::string suffix(end);
    suffix = trim(std::move(suffix));
    double multiplier = 1.0;
    if (suffix.empty()) {
        multiplier = 1.0;
    } else if (suffix == "t") {
        multiplier = 1.0e12;
    } else if (suffix == "g") {
        multiplier = 1.0e9;
    } else if (suffix == "meg") {
        multiplier = 1.0e6;
    } else if (suffix == "k") {
        multiplier = 1.0e3;
    } else if (suffix == "m") {
        multiplier = 1.0e-3;
    } else if (suffix == "u") {
        multiplier = 1.0e-6;
    } else if (suffix == "n") {
        multiplier = 1.0e-9;
    } else if (suffix == "p") {
        multiplier = 1.0e-12;
    } else if (suffix == "f") {
        multiplier = 1.0e-15;
    } else {
        return std::nullopt;
    }
    return number * multiplier;
}

std::string normalizedPinName(std::string name) {
    const auto lowered = normalizedIdentifier(name);
    static const std::map<std::string, std::string> aliases = {
        {"drain", "d"}, {"gate", "g"},      {"source", "s"},    {"bulk", "b"},  {"body", "b"},
        {"sub", "b"},   {"substrate", "b"}, {"collector", "c"}, {"base", "b"},  {"emitter", "e"},
        {"anode", "a"}, {"cathode", "k"},   {"plus", "p"},      {"minus", "n"},
    };
    const auto alias = aliases.find(lowered);
    return alias == aliases.end() ? lowered : alias->second;
}

bool isMultiTerminalType(const std::string &type) {
    const auto lowered = normalizedIdentifier(type);
    return lowered.find("nmos") != std::string::npos || lowered.find("pmos") != std::string::npos ||
           lowered.find("nfet") != std::string::npos || lowered.find("pfet") != std::string::npos ||
           lowered.find("mosfet") != std::string::npos || lowered.find("npn") != std::string::npos ||
           lowered.find("pnp") != std::string::npos || lowered.find("bjt") != std::string::npos ||
           lowered.find("hbt") != std::string::npos || lowered.find("jfet") != std::string::npos;
}

std::vector<std::string> elementPinNames(const SpiceElement &element) {
    switch (element.kind) {
    case 'm':
        return {"D", "G", "S", "B"};
    case 'q':
        return element.nodes.size() >= 4 ? std::vector<std::string>{"C", "B", "E", "S"}
                                         : std::vector<std::string>{"C", "B", "E"};
    case 'j':
        return {"D", "G", "S"};
    default:
        return {};
    }
}

bool isGroundNetName(const std::string &name) {
    const auto normalized = normalizedIdentifier(name);
    return name == "0" || normalized == "gnd" || normalized == "ground" || normalized == "vss";
}

std::string portKey(const std::string &name) {
    // Bracket-aware encoding keeps `data[0]` distinct from `data_0`.
    std::string result;
    bool pendingSeparator = false;
    for (const auto character : name) {
        const auto value = static_cast<unsigned char>(character);
        if (character == '[')
            result += "_lb_";
        else if (character == ']')
            result += "_rb_";
        else if (character == ':')
            result += "_colon_";
        else if (std::isalnum(value)) {
            if (pendingSeparator && !result.empty())
                result.push_back('_');
            pendingSeparator = false;
            result.push_back(static_cast<char>(std::tolower(value)));
        } else {
            pendingSeparator = !result.empty();
        }
    }
    return result;
}

class SpiceExpressionEvaluator {
  public:
    SpiceExpressionEvaluator(std::string text, const std::map<std::string, std::string> &parameters, int depth)
        : text_(std::move(text)), parameters_(parameters), depth_(depth) {}

    std::optional<double> evaluate() {
        skipSpace();
        const auto value = parseExpression();
        skipSpace();
        if (!value || position_ != text_.size())
            return std::nullopt;
        return value;
    }

  private:
    std::optional<double> parseExpression() {
        auto left = parseTerm();
        if (!left)
            return std::nullopt;
        for (;;) {
            skipSpace();
            if (position_ >= text_.size() || (text_[position_] != '+' && text_[position_] != '-'))
                return left;
            const auto operation = text_[position_++];
            const auto right = parseTerm();
            if (!right)
                return std::nullopt;
            *left = operation == '+' ? *left + *right : *left - *right;
        }
    }

    std::optional<double> parseTerm() {
        auto left = parsePower();
        if (!left)
            return std::nullopt;
        for (;;) {
            skipSpace();
            if (position_ >= text_.size() || (text_[position_] != '*' && text_[position_] != '/'))
                return left;
            if (text_[position_] == '*' && position_ + 1 < text_.size() && text_[position_ + 1] == '*')
                return left; // handled by parsePower
            const auto operation = text_[position_++];
            const auto right = parsePower();
            if (!right || (operation == '/' && *right == 0.0))
                return std::nullopt;
            *left = operation == '*' ? *left * *right : *left / *right;
        }
    }

    std::optional<double> parsePower() {
        auto left = parsePrimary();
        if (!left)
            return std::nullopt;
        for (;;) {
            skipSpace();
            bool power = false;
            if (position_ < text_.size() && text_[position_] == '^') {
                ++position_;
                power = true;
            } else if (position_ + 1 < text_.size() && text_[position_] == '*' && text_[position_ + 1] == '*') {
                position_ += 2;
                power = true;
            }
            if (!power)
                return left;
            const auto right = parsePrimary();
            if (!right)
                return std::nullopt;
            *left = std::pow(*left, *right);
        }
    }

    std::optional<double> parsePrimary() {
        skipSpace();
        if (position_ >= text_.size())
            return std::nullopt;
        if (text_[position_] == '(') {
            ++position_;
            const auto value = parseExpression();
            skipSpace();
            if (!value || position_ >= text_.size() || text_[position_] != ')')
                return std::nullopt;
            ++position_;
            return value;
        }
        if (text_[position_] == '+' || text_[position_] == '-') {
            const auto operation = text_[position_++];
            const auto value = parsePrimary();
            if (!value)
                return std::nullopt;
            return operation == '-' ? -*value : *value;
        }
        if (std::isalpha(static_cast<unsigned char>(text_[position_])) || text_[position_] == '_') {
            const auto start = position_;
            while (position_ < text_.size()) {
                const auto character = text_[position_];
                if (!std::isalnum(static_cast<unsigned char>(character)) && character != '_' && character != '$')
                    break;
                ++position_;
            }
            const auto name = lowerCopy(text_.substr(start, position_ - start));
            const auto parameter = parameters_.find(name);
            if (parameter == parameters_.end() || depth_ > 16)
                return std::nullopt;
            return SpiceExpressionEvaluator(parameter->second, parameters_, depth_ + 1).evaluate();
        }
        const auto start = position_;
        while (position_ < text_.size()) {
            const auto character = text_[position_];
            if (std::isalnum(static_cast<unsigned char>(character)) || character == '.' ||
                ((character == '+' || character == '-') && position_ > start &&
                 (text_[position_ - 1] == 'e' || text_[position_ - 1] == 'E')))
                ++position_;
            else
                break;
        }
        if (position_ == start)
            return std::nullopt;
        return parseQuantity(text_.substr(start, position_ - start));
    }

    void skipSpace() {
        while (position_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[position_])))
            ++position_;
    }

    std::string text_;
    const std::map<std::string, std::string> &parameters_;
    std::size_t position_ = 0;
    int depth_ = 0;
};

std::string resolveSpiceValue(const std::string &value, const std::map<std::string, std::string> &parameters) {
    auto text = trim(value);
    while (text.size() >= 2 && text.front() == '{' && text.back() == '}')
        text = trim(text.substr(1, text.size() - 2));
    if (text.empty())
        return value;
    const auto evaluated = SpiceExpressionEvaluator(text, parameters, 0).evaluate();
    if (!evaluated)
        return value;
    std::ostringstream formatted;
    formatted << std::setprecision(12) << std::defaultfloat << *evaluated;
    return formatted.str();
}

std::map<std::string, std::string> resolvedParameters(const std::vector<SpiceParameter> &declarations,
                                                      const std::map<std::string, std::string> &base = {}) {
    auto parameters = base;
    for (const auto &declaration : declarations) {
        if (declaration.name.empty())
            continue;
        parameters[lowerCopy(declaration.name)] = resolveSpiceValue(declaration.value, parameters);
    }
    return parameters;
}

bool parameterValuesEqual(const std::string &left, const std::string &right) {
    const auto leftQuantity = parseQuantity(left);
    const auto rightQuantity = parseQuantity(right);
    if (leftQuantity && rightQuantity) {
        const auto scale = std::max({1.0, std::abs(*leftQuantity), std::abs(*rightQuantity)});
        return std::abs(*leftQuantity - *rightQuantity) <= 1.0e-9 * scale;
    }
    return normalizedValue(left) == normalizedValue(right);
}

bool hasUnitSuffix(const std::string &text) {
    const auto trimmed = trim(text);
    if (trimmed.empty())
        return false;
    if (std::isalpha(static_cast<unsigned char>(trimmed.back())) != 0)
        return true;
    // Resolved expressions are emitted in scientific notation, which is
    // already in SI units even though it has no alphabetic suffix.
    return trimmed.find('e') != std::string::npos || trimmed.find('E') != std::string::npos;
}

// Layout PCell lengths are conventionally written in micrometres ("2", "0.15")
// while schematic instances usually carry the SPICE suffix ("2u"). Compare
// unitless lengths as micrometres so both spellings describe the same device.
bool lengthParameterValuesEqual(const std::string &left, const std::string &right) {
    const auto leftQuantity = parseQuantity(left);
    const auto rightQuantity = parseQuantity(right);
    if (!leftQuantity || !rightQuantity)
        return normalizedValue(left) == normalizedValue(right);
    const auto leftValue = hasUnitSuffix(left) ? *leftQuantity : *leftQuantity * 1.0e-6;
    const auto rightValue = hasUnitSuffix(right) ? *rightQuantity : *rightQuantity * 1.0e-6;
    const auto scale = std::max({1.0e-9, std::abs(leftValue), std::abs(rightValue)});
    return std::abs(leftValue - rightValue) <= 1.0e-9 * scale;
}

std::map<std::string, std::string> parameterMap(const std::vector<Parameter> &parameters) {
    std::map<std::string, std::string> result;
    for (const auto &parameter : parameters) {
        const auto name = normalizedIdentifier(parameter.name);
        if (!name.empty())
            result[name] = parameter.value;
    }
    return result;
}

std::optional<std::string> findParameter(const std::map<std::string, std::string> &parameters,
                                         const std::string &name) {
    if (const auto exact = parameters.find(name); exact != parameters.end())
        return exact->second;
    static const std::map<std::string, std::vector<std::string>> aliases = {
        {"resistance", {"resistance", "value", "r"}},
        {"value", {"value", "resistance", "capacitance", "inductance"}},
        {"capacitance", {"capacitance", "value", "c"}},
        {"inductance", {"inductance", "value", "l"}},
        {"w", {"w", "width"}},
        {"width", {"width", "w"}},
        {"l", {"l", "length"}},
        {"length", {"length", "l"}},
        {"m", {"m", "multiplicity", "parallel", "mult"}},
        {"multiplicity", {"multiplicity", "m", "parallel", "mult"}},
        {"parallel", {"parallel", "m", "multiplicity", "mult"}},
    };
    const auto alias = aliases.find(name);
    if (alias == aliases.end())
        return std::nullopt;
    for (const auto &candidate : alias->second) {
        if (const auto found = parameters.find(candidate); found != parameters.end())
            return found->second;
    }
    return std::nullopt;
}

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

std::string instanceType(const SchematicCell &cell, ObjectId instanceId) {
    const auto instance = cell.instances.find(instanceId);
    if (instance == cell.instances.end())
        return {};
    const auto parameters = parameterMap(instance->second.parameters);
    if (const auto model = parameters.find("model"); model != parameters.end() && !model->second.empty())
        return model->second;
    const auto symbol = cell.symbols.find(instance->second.symbol);
    return symbol == cell.symbols.end() ? std::string{} : symbolTypeName(symbol->second.name);
}

struct LogicalDevice {
    ObjectId object = 0;
    std::string name;
    std::string type;
    std::map<std::string, std::string> parameters;
    bool hasParameters = false;
    bool multiTerminal = false;
    ObjectId sourceCell = 0;
    ObjectId childCell = 0;
    ObjectId schematicInstance = 0;
    LvsObjectKind kind = LvsObjectKind::device;
    Rect bounds;
};

struct LogicalNet {
    std::string name;
    std::set<ObjectId> devices;
    // Terminal names belonging to each device on this net. Two-terminal
    // passives intentionally leave this empty because their orientation is not
    // electrically meaningful.
    std::map<ObjectId, std::set<std::string>> terminals;
    ObjectId object = 0;
    LvsObjectKind kind = LvsObjectKind::net;
    Rect bounds;
};

struct LogicalPort {
    ObjectId object = 0;
    std::string name;
    std::string direction;
    std::string net;
    Rect bounds;
};

struct LogicalSide {
    std::map<ObjectId, LogicalDevice> devices;
    std::vector<LogicalNet> nets;
    std::vector<LogicalPort> ports;
};

struct LayoutLogicalSide {
    LogicalSide side;
    std::map<ObjectId, ObjectId> plannedToLogical;
    bool hasConnectivity = false;
};

LogicalSide collectSpiceSchematicSide(const SchematicCell &cell, std::vector<LvsMismatch> &mismatches) {
    LogicalSide side;
    std::vector<std::string> includeWarnings;
    auto document = parseSpiceDocumentResolved(cell.spiceText, cell.spiceSourcePath, &includeWarnings);
    for (const auto &warning : includeWarnings)
        mismatches.push_back({1, "spice-include", warning, std::nullopt, std::nullopt, VerificationSeverity::error});
    const auto *selected = findSpiceSubcircuit(document, cell.name);
    if (!selected && document.elements.empty()) {
        if (document.subcircuits.size() == 1)
            selected = &document.subcircuits.front();
        else if (!document.subcircuits.empty())
            throw ProjectError("lvs.spice", "Text SPICE schematic '" + cell.name +
                                                "' has several subcircuits; name the cell after the one to compare");
    }

    std::map<std::string, const SpiceSubcircuit *> subcircuits;
    for (const auto &subcircuit : document.subcircuits)
        subcircuits.emplace(normalizedIdentifier(subcircuit.name), &subcircuit);

    std::map<std::string, std::set<ObjectId>> netDevices;
    std::map<std::string, std::map<ObjectId, std::set<std::string>>> netTerminals;
    std::map<ObjectId, std::string> rawDeviceNames;
    ObjectId nextDevice = 1;
    ObjectId nextNet = 1;

    std::set<std::string> globalNets;
    for (const auto &global : document.globals)
        globalNets.insert(lowerCopy(trim(global)));

    const auto resolveNode = [&](const std::string &node, const std::string &path,
                                 const std::map<std::string, std::string> &portMap) -> std::string {
        if (const auto mapped = portMap.find(node); mapped != portMap.end())
            return mapped->second;
        if (node == "0" || path.empty() || globalNets.contains(lowerCopy(trim(node))))
            return node;
        return path + "." + node;
    };

    std::function<void(const SpiceSubcircuit &, const std::map<std::string, std::string> &, const std::string &,
                       const std::set<std::string> &, const std::map<std::string, std::string> &)>
        flattenSubcircuit;
    std::function<void(const std::vector<SpiceElement> &, const std::map<std::string, std::string> &,
                       const std::string &, const std::set<std::string> &, const std::map<std::string, std::string> &)>
        emitElements;

    emitElements = [&](const std::vector<SpiceElement> &elements, const std::map<std::string, std::string> &portMap,
                       const std::string &path, const std::set<std::string> &scope,
                       const std::map<std::string, std::string> &parameters) {
        for (const auto &element : elements) {
            const auto deviceName = path.empty() ? element.name : path + "_" + element.name;
            const auto nodeFor = [&](const std::string &node) { return resolveNode(node, path, portMap); };
            if (element.kind == 'x') {
                const auto subcircuit = subcircuits.find(normalizedIdentifier(element.model));
                if (subcircuit != subcircuits.end()) {
                    if (scope.contains(normalizedIdentifier(subcircuit->second->name))) {
                        mismatches.push_back(
                            {1, "spice-cycle",
                             "Recursive SPICE subcircuit '" + element.model + "' in '" + cell.name + "' was skipped",
                             std::nullopt, std::nullopt, VerificationSeverity::warning});
                        continue;
                    }
                    std::map<std::string, std::string> childPorts;
                    std::size_t nodeIndex = 0;
                    for (const auto &port : subcircuit->second->ports) {
                        const auto bits = expandSpiceBus(port);
                        for (const auto &bit : bits) {
                            if (nodeIndex < element.nodes.size())
                                childPorts[bit] = nodeFor(element.nodes[nodeIndex++]);
                            else
                                childPorts[bit] = path.empty() ? bit : path + "." + bit;
                        }
                    }
                    auto childParameters = resolvedParameters(subcircuit->second->parameters, parameters);
                    for (const auto &[name, value] : element.parameters) {
                        if (!name.empty())
                            childParameters[lowerCopy(name)] = resolveSpiceValue(value, parameters);
                    }
                    auto childScope = scope;
                    childScope.insert(normalizedIdentifier(subcircuit->second->name));
                    const auto childPath = path.empty() ? deviceName : path + "." + element.name;
                    flattenSubcircuit(*subcircuit->second, childPorts, childPath, childScope, childParameters);
                    continue;
                }
            }
            if (element.kind == 'v' || element.kind == 'i' || element.kind == 'e' || element.kind == 'g' ||
                element.kind == 'f' || element.kind == 'h' || element.kind == 'b' || element.kind == 'k')
                continue;
            if (element.kind != 'r' && element.kind != 'c' && element.kind != 'l' && element.kind != 'd' &&
                element.kind != 'm' && element.kind != 'q' && element.kind != 'j' && element.kind != 'x')
                continue;

            auto resolved = element;
            resolved.value = resolveSpiceValue(element.value, parameters);
            for (auto &[name, value] : resolved.parameters)
                value = resolveSpiceValue(value, parameters);

            LogicalDevice device;
            device.object = nextDevice++;
            device.name = deviceName;
            device.type = spiceElementType(resolved);
            device.parameters = spiceElementParameters(resolved);
            device.hasParameters = true;
            const auto pinNames = elementPinNames(resolved);
            device.multiTerminal = !pinNames.empty();
            rawDeviceNames[device.object] = element.name;
            for (std::size_t index = 0; index < resolved.nodes.size(); ++index) {
                const auto node = nodeFor(resolved.nodes[index]);
                netDevices[node].insert(device.object);
                if (index < pinNames.size())
                    netTerminals[node][device.object].insert(normalizedPinName(pinNames[index]));
            }
            side.devices.emplace(device.object, std::move(device));
        }
    };

    flattenSubcircuit = [&](const SpiceSubcircuit &subcircuit, const std::map<std::string, std::string> &portMap,
                            const std::string &path, const std::set<std::string> &parentScope,
                            const std::map<std::string, std::string> &parameters) {
        auto scope = parentScope;
        scope.insert(normalizedIdentifier(subcircuit.name));
        emitElements(subcircuit.elements, portMap, path, scope, parameters);
    };

    if (selected) {
        std::map<std::string, std::string> ports;
        for (const auto &port : selected->ports) {
            for (const auto &bit : expandSpiceBus(port))
                ports[bit] = bit;
        }
        const auto parameters = resolvedParameters(selected->parameters, resolvedParameters(document.parameters));
        flattenSubcircuit(*selected, ports, {}, {}, parameters);
        for (const auto &port : selected->ports) {
            for (const auto &bit : expandSpiceBus(port)) {
                side.ports.push_back({nextNet++, bit, "inout", bit});
            }
        }
    } else {
        emitElements(document.elements, {}, {}, {}, resolvedParameters(document.parameters));
    }

    // Prefer the source reference name when it is unique across the flattened
    // hierarchy; path-qualified names keep duplicates distinguishable.
    std::map<std::string, int> rawNameCounts;
    for (const auto &[_, name] : rawDeviceNames)
        ++rawNameCounts[normalizedIdentifier(name)];
    for (auto &[_, device] : side.devices) {
        const auto raw = rawDeviceNames.find(device.object);
        if (raw != rawDeviceNames.end() && rawNameCounts[normalizedIdentifier(raw->second)] == 1)
            device.name = raw->second;
    }

    for (const auto &[name, devices] : netDevices) {
        if (devices.empty())
            continue;
        LogicalNet net;
        net.name = name;
        net.devices = devices;
        if (const auto terminals = netTerminals.find(name); terminals != netTerminals.end())
            net.terminals = terminals->second;
        net.object = nextNet++;
        side.nets.push_back(std::move(net));
    }
    return side;
}

LogicalSide collectHdlSchematicSide(const SchematicCell &cell) {
    const auto parsed = parseSystemVerilogModule(cell);
    if (!parsed.ok)
        throw ProjectError("lvs.hdl", "SystemVerilog parse failed for '" + cell.name + "': " + parsed.error);
    const auto netlist = parseStructuralSystemVerilog(cell.systemVerilogText, parsed.module.name);
    if (!netlist.ok)
        throw ProjectError("lvs.hdl", "SystemVerilog LVS failed for '" + cell.name + "': " + netlist.error);
    if (!netlist.structural)
        throw ProjectError("lvs.hdl", netlist.error);

    std::map<std::string, std::string> parent;
    std::function<std::string(const std::string &)> find = [&](const std::string &name) -> std::string {
        const auto entry = parent.find(name);
        if (entry == parent.end()) {
            parent[name] = name;
            return name;
        }
        if (entry->second == name)
            return name;
        const auto root = find(entry->second);
        parent[name] = root;
        return root;
    };
    const auto unite = [&](const std::string &left, const std::string &right) {
        const auto leftRoot = find(left);
        const auto rightRoot = find(right);
        if (leftRoot != rightRoot)
            parent[rightRoot] = leftRoot;
    };
    for (const auto &[alias, target] : netlist.netAliases)
        unite(alias, target);

    std::set<std::string> usedNets;
    for (const auto &instance : netlist.instances) {
        for (const auto &net : instance.nets)
            usedNets.insert(find(normalizedIdentifier(net)));
    }

    LogicalSide side;
    std::map<std::string, std::set<ObjectId>> netDevices;
    std::map<std::string, std::map<ObjectId, std::set<std::string>>> netTerminals;
    ObjectId nextObject = 1;
    for (const auto &instance : netlist.instances) {
        LogicalDevice device;
        device.object = nextObject++;
        device.name = instance.name;
        device.type = instance.type;
        device.sourceCell = cell.id;
        device.hasParameters = false;
        device.multiTerminal = instance.portNames.size() > 2 || isMultiTerminalType(instance.type);
        for (std::size_t index = 0; index < instance.nets.size(); ++index) {
            const auto net = find(normalizedIdentifier(instance.nets[index]));
            netDevices[net].insert(device.object);
            if (index < instance.portNames.size())
                netTerminals[net][device.object].insert(normalizedPinName(instance.portNames[index]));
        }
        side.devices.emplace(device.object, std::move(device));
    }

    const auto directionName = [](HdlPortDirection direction) {
        switch (direction) {
        case HdlPortDirection::input:
            return "input";
        case HdlPortDirection::output:
            return "output";
        case HdlPortDirection::inout:
            return "inout";
        }
        return "inout";
    };
    for (const auto &port : parsed.module.ports) {
        if (port.width > 1 && port.widthKnown && !port.real) {
            for (int bit = 0; bit < port.width; ++bit) {
                const auto name = port.name + "[" + std::to_string(bit) + "]";
                const auto canonical = find(normalizedIdentifier(name));
                side.ports.push_back({nextObject++, name, directionName(port.direction),
                                      usedNets.contains(canonical) ? canonical : normalizedIdentifier(name)});
            }
            continue;
        }
        const auto canonical = find(normalizedIdentifier(port.name));
        side.ports.push_back({nextObject++, port.name, directionName(port.direction),
                              usedNets.contains(canonical) ? canonical : normalizedIdentifier(port.name)});
    }

    for (auto &[net, devices] : netDevices) {
        if (devices.empty())
            continue;
        LogicalNet logicalNet;
        logicalNet.name = net;
        logicalNet.devices = std::move(devices);
        if (const auto terminals = netTerminals.find(net); terminals != netTerminals.end())
            logicalNet.terminals = terminals->second;
        logicalNet.object = nextObject++;
        side.nets.push_back(std::move(logicalNet));
    }
    return side;
}

LogicalSide collectSchematicSide(const Project &project, ObjectId cellId, std::vector<LvsMismatch> &mismatches) {
    const auto cellIt = project.schematicCells.find(cellId);
    if (cellIt == project.schematicCells.end())
        throw ProjectError("lvs.schematic", "Schematic cell does not exist");
    const auto &cell = cellIt->second;
    if (cell.textSchematic)
        return collectSpiceSchematicSide(cell, mismatches);
    if (cell.systemVerilogSchematic)
        return collectHdlSchematicSide(cell);

    LogicalSide side;
    std::map<ObjectId, std::vector<ObjectId>> pinOwners;
    std::map<ObjectId, std::string> pinNames;
    struct PortInstance {
        ObjectId id = 0;
        ObjectId symbol = 0;
        std::string name;
        std::string direction;
        std::vector<ObjectId> pins;
    };
    std::vector<PortInstance> portInstances;
    for (const auto &[id, instance] : cell.instances) {
        LogicalDevice device;
        device.object = id;
        const auto symbol = cell.symbols.find(instance.symbol);
        std::string symbolName;
        auto symbolKind = SchematicPortKind::none;
        if (symbol != cell.symbols.end()) {
            symbolName = symbolTypeName(symbol->second.name);
            symbolKind = schematicPortKindForSymbol(symbolName);
        }
        const bool isPort = symbolKind != SchematicPortKind::none;
        bool sourceOrGround = false;
        if (symbol != cell.symbols.end()) {
            device.sourceCell = symbol->second.sourceCell;
            auto element = symbol->second.sourceCell != 0 ? 'X' : spiceElementForSymbol(symbolName);
            const auto normalized = normalizedIdentifier(symbolName);
            if (element == 'X' && (normalized == "gnd" || normalized == "0"))
                element = 'G';
            sourceOrGround = element == 'V' || element == 'I' || element == 'G';
            if (isPort) {
                PortInstance port;
                port.id = id;
                port.symbol = instance.symbol;
                port.name = instance.name.empty() ? symbolName : instance.name;
                port.direction = symbolKind == SchematicPortKind::input    ? "input"
                                 : symbolKind == SchematicPortKind::output ? "output"
                                                                           : "inout";
                for (const auto &pin : symbol->second.pins)
                    port.pins.push_back(pin.id);
                portInstances.push_back(std::move(port));
            } else if (!sourceOrGround) {
                for (const auto &pin : symbol->second.pins) {
                    pinOwners[pin.id].push_back(id);
                    pinNames[pin.id] = pin.name;
                }
            }
        } else {
            device.type = "unknown";
            mismatches.push_back({1, "schematic-symbol",
                                  "Schematic instance '" + instance.name + "' references a missing symbol", id,
                                  std::nullopt, VerificationSeverity::error});
        }
        if (isPort || sourceOrGround)
            continue;
        device.name = instance.name.empty() ? symbolName + " " + std::to_string(id) : instance.name;
        device.parameters = parameterMap(instance.parameters);
        device.hasParameters = !instance.parameters.empty();
        if (const auto model = device.parameters.find("model");
            model != device.parameters.end() && !model->second.empty())
            device.type = model->second;
        else
            device.type = symbolName;
        device.multiTerminal =
            (symbol != cell.symbols.end() && symbol->second.pins.size() > 2) || isMultiTerminalType(device.type);
        side.devices.emplace(id, std::move(device));
    }

    std::map<ObjectId, std::string> netByPin;
    for (const auto &[id, net] : cell.nets) {
        const auto netName = net.name.empty() ? "N" + std::to_string(id) : net.name;
        for (const auto pinId : net.pins)
            netByPin.emplace(pinId, netName);
    }
    std::map<ObjectId, int> symbolUseCount;
    for (const auto &[_, instance] : cell.instances)
        ++symbolUseCount[instance.symbol];
    for (auto &port : portInstances) {
        // Nets reference symbol pins, so instances that share one symbol have
        // identical pin ids and their port nets cannot be told apart. Report
        // that instead of silently attributing the first net to every copy.
        const auto symbol = cell.symbols.find(port.symbol);
        if (symbol != cell.symbols.end() && symbolUseCount[symbol->first] > 1) {
            mismatches.push_back({1, "port-net-ambiguous",
                                  "Port '" + port.name +
                                      "' shares its symbol with another instance, so its net cannot be "
                                      "resolved; give each port its own symbol (for example with "
                                      "schematic_add_component)",
                                  port.id, std::nullopt, VerificationSeverity::warning});
        }
        for (const auto pinId : port.pins) {
            if (const auto net = netByPin.find(pinId); net != netByPin.end()) {
                side.ports.push_back({port.id, port.name, port.direction, net->second});
                break;
            }
        }
        if (std::none_of(side.ports.begin(), side.ports.end(),
                         [&](const LogicalPort &candidate) { return candidate.object == port.id; }))
            side.ports.push_back({port.id, port.name, port.direction, {}});
    }

    for (const auto &[id, net] : cell.nets) {
        LogicalNet logicalNet;
        logicalNet.name = net.name.empty() ? "N" + std::to_string(id) : net.name;
        logicalNet.object = id;
        for (const auto pinId : net.pins) {
            const auto owners = pinOwners.find(pinId);
            if (owners == pinOwners.end())
                continue;
            const auto pinName = pinNames.find(pinId);
            for (const auto owner : owners->second) {
                logicalNet.devices.insert(owner);
                if (pinName != pinNames.end() && !pinName->second.empty())
                    logicalNet.terminals[owner].insert(normalizedPinName(pinName->second));
            }
        }
        if (logicalNet.devices.empty())
            continue;
        side.nets.push_back(std::move(logicalNet));
    }
    return side;
}

void includeBounds(Rect &target, const Rect &rectangle) {
    if (!rectangle.valid())
        return;
    if (!target.valid()) {
        target = rectangle;
        return;
    }
    const auto maxX = std::max(target.x + target.width, rectangle.x + rectangle.width);
    const auto maxY = std::max(target.y + target.height, rectangle.y + rectangle.height);
    target.x = std::min(target.x, rectangle.x);
    target.y = std::min(target.y, rectangle.y);
    target.width = maxX - target.x;
    target.height = maxY - target.y;
}

Rect layoutCellGeometryBounds(const LayoutCell &cell) {
    Rect bounds;
    for (const auto &[_, shape] : cell.shapes) {
        if (shape.rectangle.valid()) {
            includeBounds(bounds, shape.rectangle);
            continue;
        }
        if (!shape.polygon.empty()) {
            auto minX = shape.polygon.front().x;
            auto minY = shape.polygon.front().y;
            auto maxX = minX;
            auto maxY = minY;
            for (const auto &point : shape.polygon) {
                minX = std::min(minX, point.x);
                minY = std::min(minY, point.y);
                maxX = std::max(maxX, point.x);
                maxY = std::max(maxY, point.y);
            }
            includeBounds(bounds, Rect{minX, minY, maxX - minX, maxY - minY});
            continue;
        }
        if (!shape.centerline.empty()) {
            const auto expansion = std::max<Coordinate>(1, shape.width) / 2;
            auto minX = shape.centerline.front().x - expansion;
            auto minY = shape.centerline.front().y - expansion;
            auto maxX = shape.centerline.front().x + expansion;
            auto maxY = shape.centerline.front().y + expansion;
            for (const auto &point : shape.centerline) {
                minX = std::min(minX, point.x - expansion);
                minY = std::min(minY, point.y - expansion);
                maxX = std::max(maxX, point.x + expansion);
                maxY = std::max(maxY, point.y + expansion);
            }
            includeBounds(bounds, Rect{minX, minY, maxX - minX, maxY - minY});
        }
    }
    for (const auto &[_, port] : cell.ports)
        includeBounds(bounds, port.rectangle);
    return bounds;
}

LogicalDevice actualLayoutDevice(const LayoutInstance &instance, const LayoutCell &child) {
    LogicalDevice device;
    device.object = instance.id;
    device.name = instance.name;
    device.type = child.device.empty() ? child.name : child.device;
    device.childCell = child.id;
    device.parameters = parameterMap(child.parameters);
    device.hasParameters = !child.parameters.empty();
    device.multiTerminal = child.ports.size() > 2 || isMultiTerminalType(device.type);
    device.schematicInstance = instance.schematicInstance;
    device.kind = LvsObjectKind::instance;
    device.bounds =
        transformRectangle(layoutCellGeometryBounds(child), Transform{instance.x, instance.y, instance.orientation});
    return device;
}

LayoutLogicalSide collectLayoutSide(const Project &project, ObjectId cellId, const SchematicCell &schematic,
                                    std::vector<LvsMismatch> &mismatches) {
    const auto cellIt = project.layoutCells.find(cellId);
    if (cellIt == project.layoutCells.end())
        throw ProjectError("lvs.layout", "Layout cell does not exist");
    const auto &cell = cellIt->second;
    if (cell.library)
        throw ProjectError("lvs.layout", "LVS compares user layout cells, not read-only library cells");

    const auto elementForName = [](const std::string &name) {
        const auto normalized = normalizedIdentifier(name);
        if (normalized == "gnd" || normalized == "0")
            return 'G';
        return spiceElementForSymbol(name);
    };
    const auto elementForSchematicInstance = [&](ObjectId instanceId) {
        const auto instance = schematic.instances.find(instanceId);
        if (instance == schematic.instances.end())
            return '?';
        const auto symbol = schematic.symbols.find(instance->second.symbol);
        if (symbol == schematic.symbols.end())
            return '?';
        if (symbol->second.sourceCell != 0)
            return 'X';
        return elementForName(symbol->second.name);
    };
    const auto isSupply = [](char element) { return element == 'V' || element == 'I' || element == 'G'; };

    LayoutLogicalSide result;
    struct ActualDevice {
        ObjectId instance = 0;
        LogicalDevice device;
    };
    std::vector<ActualDevice> actualDevices;
    std::map<std::string, std::vector<ObjectId>> actualByName;
    for (const auto &[id, instance] : cell.instances) {
        const auto child = project.layoutCells.find(instance.cell);
        if (child == project.layoutCells.end()) {
            mismatches.push_back({1, "layout-instance",
                                  "Layout instance '" + instance.name + "' references a missing cell", std::nullopt, id,
                                  VerificationSeverity::error});
            continue;
        }
        char element = 'X';
        if (instance.schematicInstance != 0)
            element = elementForSchematicInstance(instance.schematicInstance);
        else if (child->second.library && !child->second.device.empty())
            element = elementForName(child->second.device);
        if (isSupply(element))
            continue;
        if (instance.schematicInstance != 0 && !schematic.instances.contains(instance.schematicInstance) &&
            !schematic.isTextSource()) {
            mismatches.push_back(
                {1, "layout-link",
                 "Layout instance '" + instance.name + "' links to a schematic instance that is not in this schematic",
                 std::nullopt, id, VerificationSeverity::warning});
        }
        auto device = actualLayoutDevice(instance, child->second);
        if (instance.schematicInstance != 0 && schematic.instances.contains(instance.schematicInstance))
            device.type = instanceType(schematic, instance.schematicInstance);
        actualByName[normalizedIdentifier(instance.name)].push_back(id);
        actualDevices.push_back({id, std::move(device)});
    }
    for (auto &entry : actualByName)
        std::sort(entry.second.begin(), entry.second.end());

    std::set<ObjectId> usedActual;
    for (const auto &[plannedId, planned] : cell.plannedDevices) {
        const auto element =
            planned.schematicInstance != 0 ? elementForSchematicInstance(planned.schematicInstance) : 'X';
        if (isSupply(element))
            continue;
        ObjectId actualId = 0;
        const auto matches = actualByName.find(normalizedIdentifier(planned.name));
        if (matches != actualByName.end()) {
            for (const auto &candidate : matches->second) {
                if (usedActual.contains(candidate))
                    continue;
                actualId = candidate;
                usedActual.insert(candidate);
                break;
            }
        }

        LogicalDevice device;
        device.name = planned.name;
        device.schematicInstance = planned.schematicInstance;
        device.kind = LvsObjectKind::plannedDevice;
        device.bounds = Rect{planned.x, planned.y, planned.width, planned.height};
        if (actualId != 0) {
            const auto actual = std::find_if(actualDevices.begin(), actualDevices.end(),
                                             [actualId](const auto &entry) { return entry.instance == actualId; });
            if (actual != actualDevices.end())
                device = actual->device;
            const bool actualLinkValid =
                device.schematicInstance != 0 && schematic.instances.contains(device.schematicInstance);
            if (!actualLinkValid && planned.schematicInstance != 0)
                device.schematicInstance = planned.schematicInstance;
        } else if (planned.schematicInstance != 0) {
            if (!schematic.instances.contains(planned.schematicInstance)) {
                // A layout created from a drawn view may still carry links
                // when it is compared against a text SPICE or HDL source cell.
                // Those views have no canvas instances, so the fallback to
                // planned model/device identity is expected.
                if (!schematic.isTextSource())
                    mismatches.push_back({1, "layout-link",
                                          "Planned device '" + planned.name +
                                              "' links to a schematic instance that is not in this schematic",
                                          std::nullopt, plannedId, VerificationSeverity::warning});
            } else {
                device.type = instanceType(schematic, planned.schematicInstance);
            }
        }

        if (actualId == 0) {
            if (!planned.model.empty())
                device.type = planned.model;
            else if (device.type.empty() && !planned.device.empty())
                device.type = planned.device;
            if (!planned.parameters.empty()) {
                device.parameters = parameterMap(planned.parameters);
                device.hasParameters = true;
            }
        } else {
            if (device.type.empty() && !planned.model.empty())
                device.type = planned.model;
            if (!device.hasParameters && !planned.parameters.empty()) {
                device.parameters = parameterMap(planned.parameters);
                device.hasParameters = true;
            }
        }
        device.multiTerminal = device.multiTerminal || planned.terminals.size() > 2 || isMultiTerminalType(device.type);
        const auto logicalId = actualId != 0 ? actualId : plannedId;
        device.object = logicalId;
        result.side.devices.emplace(logicalId, std::move(device));
        result.plannedToLogical.emplace(plannedId, logicalId);
    }

    for (auto &entry : actualDevices) {
        if (usedActual.contains(entry.instance))
            continue;
        result.side.devices.emplace(entry.instance, std::move(entry.device));
    }

    // Layout connectivity combines explicit/planned metadata with real
    // geometry. The geometric groups catch routed shapes, vias, and layout
    // devices whose terminals only touch metal; planned connections and
    // terminal maps remain authoritative for procedural layouts.
    const auto logicalId = [&](ObjectId id) {
        const auto logical = result.plannedToLogical.find(id);
        return logical != result.plannedToLogical.end() ? logical->second : id;
    };
    const auto netKeyFor = [&](ObjectId netId) {
        if (netId == 0)
            return std::string{};
        if (const auto net = cell.nets.find(netId); net != cell.nets.end() && !net->second.name.empty())
            return "name:" + normalizedIdentifier(net->second.name);
        return "net:" + std::to_string(netId);
    };
    const auto netNameFor = [&](ObjectId netId) {
        if (netId == 0)
            return std::string{};
        if (const auto net = cell.nets.find(netId); net != cell.nets.end() && !net->second.name.empty())
            return net->second.name;
        return "N" + std::to_string(netId);
    };

    struct NetAccumulator {
        std::string name;
        ObjectId object = 0;
        std::set<ObjectId> devices;
        std::map<ObjectId, std::set<std::string>> terminals;
        Rect bounds;
    };
    std::map<std::string, NetAccumulator> nets;
    const auto addToNet = [&](const std::string &key, const std::string &name, ObjectId object, ObjectId device,
                              const std::string &pin = {}, const Rect &bounds = {}) {
        if (key.empty() || !result.side.devices.contains(device))
            return;
        auto &net = nets[key];
        if (net.name.empty())
            net.name = name;
        if (net.object == 0)
            net.object = object;
        net.devices.insert(device);
        includeBounds(net.bounds, bounds);
        if (!pin.empty())
            net.terminals[device].insert(normalizedPinName(pin));
    };

    const auto geometryGroups = extractLayoutNets(project, cellId);
    std::map<ObjectId, std::size_t> groupByPort;
    std::vector<Rect> groupBounds(geometryGroups.size());
    const auto shapeBounds = [](const Shape &shape) {
        if (shape.rectangle.valid())
            return shape.rectangle;
        if (shape.centerline.empty())
            return Rect{};
        const auto expansion = std::max<Coordinate>(1, shape.width) / 2;
        auto minX = shape.centerline.front().x - expansion;
        auto minY = shape.centerline.front().y - expansion;
        auto maxX = shape.centerline.front().x + expansion;
        auto maxY = shape.centerline.front().y + expansion;
        for (const auto &point : shape.centerline) {
            minX = std::min(minX, point.x - expansion);
            minY = std::min(minY, point.y - expansion);
            maxX = std::max(maxX, point.x + expansion);
            maxY = std::max(maxY, point.y + expansion);
        }
        return Rect{minX, minY, maxX - minX, maxY - minY};
    };
    const auto includeBounds = [](Rect &target, const Rect &rectangle) {
        if (!rectangle.valid())
            return;
        if (!target.valid()) {
            target = rectangle;
            return;
        }
        const auto maxX = std::max(target.x + target.width, rectangle.x + rectangle.width);
        const auto maxY = std::max(target.y + target.height, rectangle.y + rectangle.height);
        target.x = std::min(target.x, rectangle.x);
        target.y = std::min(target.y, rectangle.y);
        target.width = maxX - target.x;
        target.height = maxY - target.y;
    };
    for (std::size_t index = 0; index < geometryGroups.size(); ++index) {
        for (const auto shapeId : geometryGroups[index].shapes) {
            if (const auto shape = cell.shapes.find(shapeId); shape != cell.shapes.end())
                includeBounds(groupBounds[index], shapeBounds(shape->second));
        }
        for (const auto portId : geometryGroups[index].ports) {
            groupByPort[portId] = index;
            if (const auto port = cell.ports.find(portId); port != cell.ports.end())
                includeBounds(groupBounds[index], port->second.rectangle);
        }
    }
    const auto groupKey = [&](std::size_t index) {
        const auto &group = geometryGroups[index];
        if (group.net != 0)
            return netKeyFor(group.net);
        if (!group.name.empty() && !group.name.starts_with("N"))
            return "name:" + normalizedIdentifier(group.name);
        return "geom:" + std::to_string(index);
    };
    const auto groupName = [&](std::size_t index) {
        const auto &group = geometryGroups[index];
        if (group.net != 0)
            return netNameFor(group.net);
        if (!group.name.empty())
            return group.name;
        return "geom " + std::to_string(index);
    };

    for (std::size_t index = 0; index < geometryGroups.size(); ++index) {
        const auto &group = geometryGroups[index];
        for (const auto &alias : group.aliases) {
            LvsMismatch mismatch{1,
                                 "layout-short",
                                 "Layout net '" + groupName(index) + "' is shorted to '" + alias +
                                     "' through unlabeled geometry",
                                 std::nullopt,
                                 group.net != 0 ? std::optional<ObjectId>(group.net) : std::nullopt,
                                 VerificationSeverity::error};
            LvsObjectRef ref;
            ref.kind = group.net != 0 ? LvsObjectKind::net : LvsObjectKind::geometry;
            ref.cell = cellId;
            ref.id = group.net;
            ref.name = groupName(index);
            if (groupBounds[index].valid())
                ref.bounds = groupBounds[index];
            mismatch.layoutRef = ref;
            mismatches.push_back(std::move(mismatch));
        }
        if (group.shapes.empty() && group.ports.empty())
            continue;
        result.hasConnectivity = true;
        if (group.net != 0) {
            const auto key = groupKey(index);
            for (const auto &[plannedId, planned] : cell.plannedDevices) {
                for (const auto &[pin, netId] : planned.terminals) {
                    if (netId == group.net)
                        addToNet(key, groupName(index), group.net, logicalId(plannedId), pin, groupBounds[index]);
                }
            }
        }
    }

    // Placed layout instances connect through the child ports transformed into
    // parent coordinates. Any overlapping parent group becomes a terminal net.
    const auto layerKeysMatch = [&](ObjectId left, ObjectId right) {
        const auto leftLayer = project.layers.find(left);
        const auto rightLayer = project.layers.find(right);
        if (leftLayer == project.layers.end() || rightLayer == project.layers.end())
            return true;
        return layoutLayerConnectivityKey(leftLayer->second) == layoutLayerConnectivityKey(rightLayer->second);
    };
    for (const auto &[instanceId, instance] : cell.instances) {
        const auto child = project.layoutCells.find(instance.cell);
        if (child == project.layoutCells.end())
            continue;
        const Transform transform{instance.x, instance.y, instance.orientation};
        for (const auto &[_, childPort] : child->second.ports) {
            const auto rectangle = transformRectangle(childPort.rectangle, transform);
            if (!rectangle.valid())
                continue;
            for (std::size_t index = 0; index < geometryGroups.size(); ++index) {
                if (!rectanglesOverlapOrTouch(rectangle, groupBounds[index]))
                    continue;
                bool matched = false;
                for (const auto shapeId : geometryGroups[index].shapes) {
                    const auto shape = cell.shapes.find(shapeId);
                    if (shape == cell.shapes.end() || !rectanglesOverlapOrTouch(rectangle, shapeBounds(shape->second)))
                        continue;
                    if (!layerKeysMatch(childPort.layer, shape->second.layer))
                        continue;
                    matched = true;
                    break;
                }
                if (!matched) {
                    for (const auto portId : geometryGroups[index].ports) {
                        const auto port = cell.ports.find(portId);
                        if (port == cell.ports.end() || !rectanglesOverlapOrTouch(rectangle, port->second.rectangle))
                            continue;
                        if (!layerKeysMatch(childPort.layer, port->second.layer))
                            continue;
                        matched = true;
                        break;
                    }
                }
                if (matched)
                    addToNet(groupKey(index), groupName(index), geometryGroups[index].net, logicalId(instanceId),
                             childPort.name, groupBounds[index]);
            }
        }
    }

    // Planned devices may carry terminal nets directly. Procedural layouts use
    // this because their PCell geometry is flattened but the device identity
    // and pin nets are still known.
    for (const auto &[plannedId, planned] : cell.plannedDevices) {
        for (const auto &[pin, netId] : planned.terminals) {
            if (netId == 0)
                continue;
            result.hasConnectivity = true;
            addToNet(netKeyFor(netId), netNameFor(netId), netId, logicalId(plannedId), pin,
                     Rect{planned.x, planned.y, planned.width, planned.height});
        }
    }

    std::map<ObjectId, ObjectId> parent;
    for (const auto &[plannedId, _] : cell.plannedDevices)
        parent[plannedId] = plannedId;
    for (const auto &[instanceId, _] : cell.instances)
        parent[instanceId] = instanceId;
    for (const auto &[portId, _] : cell.ports)
        parent[portId] = portId;
    const auto findRoot = [&parent](ObjectId id) {
        auto root = id;
        while (parent.at(root) != root)
            root = parent.at(root);
        while (parent.at(id) != id) {
            const auto next = parent.at(id);
            parent[id] = root;
            id = next;
        }
        return root;
    };
    const auto unite = [&parent, &findRoot](ObjectId left, ObjectId right) {
        const auto leftRoot = findRoot(left);
        const auto rightRoot = findRoot(right);
        if (leftRoot != rightRoot)
            parent[rightRoot] = leftRoot;
    };

    const auto connectionPin = [&](ObjectId endpoint, ObjectId portId, const std::string &pin) {
        if (!pin.empty())
            return pin;
        const auto instance = cell.instances.find(endpoint);
        if (instance != cell.instances.end() && portId != 0) {
            const auto child = project.layoutCells.find(instance->second.cell);
            if (child != project.layoutCells.end()) {
                const auto port = child->second.ports.find(portId);
                if (port != child->second.ports.end())
                    return port->second.name;
            }
        }
        return std::string{};
    };

    const auto endpointBounds = [&](ObjectId endpoint) -> Rect {
        if (const auto planned = cell.plannedDevices.find(endpoint); planned != cell.plannedDevices.end())
            return Rect{planned->second.x, planned->second.y, planned->second.width, planned->second.height};
        if (const auto port = cell.ports.find(endpoint); port != cell.ports.end())
            return port->second.rectangle;
        if (const auto instance = cell.instances.find(endpoint); instance != cell.instances.end()) {
            if (const auto child = project.layoutCells.find(instance->second.cell); child != project.layoutCells.end())
                return transformRectangle(
                    layoutCellGeometryBounds(child->second),
                    Transform{instance->second.x, instance->second.y, instance->second.orientation});
        }
        return {};
    };

    bool validConnection = false;
    std::set<ObjectId> unnamedMembers;
    std::map<ObjectId, ObjectId> unnamedObjects;
    for (const auto &[_, connection] : cell.plannedConnections) {
        const bool haveFrom = parent.contains(connection.from);
        const bool haveTo = parent.contains(connection.to);
        if (!haveFrom || !haveTo) {
            mismatches.push_back({1, "layout-connection",
                                  "Planned connection references a missing device or layout instance", std::nullopt,
                                  connection.id, VerificationSeverity::error});
            continue;
        }
        validConnection = true;
        if (connection.net != 0) {
            addToNet(netKeyFor(connection.net), netNameFor(connection.net), connection.net, logicalId(connection.from),
                     connectionPin(connection.from, connection.fromPort, connection.fromPin),
                     endpointBounds(connection.from));
            addToNet(netKeyFor(connection.net), netNameFor(connection.net), connection.net, logicalId(connection.to),
                     connectionPin(connection.to, connection.toPort, connection.toPin), endpointBounds(connection.to));
        } else {
            unite(connection.from, connection.to);
            unnamedMembers.insert(connection.from);
            unnamedMembers.insert(connection.to);
            const auto [entry, inserted] = unnamedObjects.emplace(findRoot(connection.from), connection.id);
            if (!inserted && connection.id < entry->second)
                entry->second = connection.id;
        }
    }
    result.hasConnectivity = result.hasConnectivity || validConnection;

    std::map<ObjectId, std::vector<ObjectId>> unnamedComponents;
    for (const auto member : unnamedMembers)
        unnamedComponents[findRoot(member)].push_back(member);
    for (const auto &[root, members] : unnamedComponents) {
        std::set<ObjectId> logicalMembers;
        for (const auto member : members) {
            const auto logical = logicalId(member);
            if (result.side.devices.contains(logical))
                logicalMembers.insert(logical);
        }
        if (logicalMembers.empty())
            continue;
        auto &net = nets["unnamed:" + std::to_string(root)];
        net.name = "net " + std::to_string(*std::min_element(logicalMembers.begin(), logicalMembers.end()));
        net.object = unnamedObjects.contains(root) ? unnamedObjects.at(root) : 0;
        net.devices = std::move(logicalMembers);
        for (const auto member : members)
            includeBounds(net.bounds, endpointBounds(member));
    }

    for (auto &[_, net] : nets) {
        if (net.devices.empty())
            continue;
        LogicalNet logicalNet;
        logicalNet.name = net.name.empty() ? "net" : net.name;
        logicalNet.object = net.object;
        logicalNet.bounds = net.bounds;
        logicalNet.kind = net.object == 0 ? LvsObjectKind::geometry : LvsObjectKind::net;
        logicalNet.devices = std::move(net.devices);
        logicalNet.terminals = std::move(net.terminals);
        result.side.nets.push_back(std::move(logicalNet));
    }

    const auto isConvertedSupplyPin = [&](const Port &port) {
        if (port.schematicInstance == 0)
            return false;
        const auto instance = schematic.instances.find(port.schematicInstance);
        if (instance == schematic.instances.end())
            return true;
        const auto symbol = schematic.symbols.find(instance->second.symbol);
        if (symbol == schematic.symbols.end())
            return true;
        return schematicPortKindForSymbol(symbol->second.name) == SchematicPortKind::none;
    };
    for (const auto &[_, port] : cell.ports) {
        if (!port.visible || port.internal || isConvertedSupplyPin(port))
            continue;
        std::string netName;
        if (port.net != 0) {
            if (const auto net = cell.nets.find(port.net); net != cell.nets.end())
                netName = net->second.name;
        } else if (const auto group = groupByPort.find(port.id); group != groupByPort.end()) {
            netName = groupName(group->second);
        }
        result.side.ports.push_back({port.id, port.name, portDirectionName(port.direction), netName, port.rectangle});
    }

    return result;
}

std::string kindName(bool schematic, const LogicalDevice &device) {
    if (!device.type.empty())
        return device.type;
    return schematic ? "device" : "layout device";
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

bool compatibleType(const std::string &left, const std::string &right) {
    const auto a = normalizedIdentifier(left);
    const auto b = normalizedIdentifier(right);
    if (a.empty() || b.empty() || a == "unknown" || b == "unknown")
        return true;
    if (a == b)
        return true;
    // PDK device names commonly extend a primitive name (nmos ->
    // nmos_lowvt). Treat that as the same device class.
    return a.starts_with(b + "_") || b.starts_with(a + "_");
}

int deviceMultiplicity(const LogicalDevice &device) {
    const auto value = findParameter(device.parameters, "m");
    if (!value)
        return 1;
    const auto trimmed = trim(*value);
    char *end = nullptr;
    const auto parsed = std::strtol(trimmed.c_str(), &end, 10);
    if (end == trimmed.c_str() || parsed <= 0 || parsed > 1024)
        return 1;
    return static_cast<int>(parsed);
}

void expandMultiplicity(LogicalSide &side) {
    ObjectId nextObject = 1;
    for (const auto &[id, _] : side.devices)
        nextObject = std::max(nextObject, id + 1);
    std::map<ObjectId, std::vector<ObjectId>> expanded;
    for (auto &[id, device] : side.devices) {
        const auto multiplicity = deviceMultiplicity(device);
        if (multiplicity <= 1)
            continue;
        for (auto parameter = device.parameters.begin(); parameter != device.parameters.end();) {
            const auto normalized = normalizedIdentifier(parameter->first);
            if (normalized == "m" || normalized == "multiplicity" || normalized == "parallel" || normalized == "mult")
                parameter = device.parameters.erase(parameter);
            else
                ++parameter;
        }
        for (int index = 1; index < multiplicity; ++index) {
            auto clone = device;
            clone.object = nextObject++;
            clone.name =
                (device.name.empty() ? "device" + std::to_string(id) : device.name) + "#" + std::to_string(index + 1);
            clone.schematicInstance = 0;
            expanded[id].push_back(clone.object);
            side.devices.emplace(clone.object, std::move(clone));
        }
    }
    for (auto &net : side.nets) {
        std::set<ObjectId> additions;
        for (const auto id : net.devices) {
            if (const auto found = expanded.find(id); found != expanded.end()) {
                additions.insert(found->second.begin(), found->second.end());
                if (const auto terminals = net.terminals.find(id); terminals != net.terminals.end()) {
                    for (const auto clone : found->second)
                        net.terminals[clone] = terminals->second;
                }
            }
        }
        net.devices.insert(additions.begin(), additions.end());
    }
}

std::string topologySignature(const LogicalSide &side, ObjectId deviceId) {
    const auto device = side.devices.find(deviceId);
    if (device == side.devices.end())
        return {};
    const auto type = normalizedIdentifier(device->second.type);
    if (type.empty() || type == "unknown")
        return {};
    std::vector<std::string> netSignatures;
    for (const auto &net : side.nets) {
        if (!net.devices.contains(deviceId))
            continue;
        std::vector<std::string> types;
        types.reserve(net.devices.size());
        for (const auto member : net.devices) {
            const auto memberDevice = side.devices.find(member);
            if (memberDevice != side.devices.end())
                types.push_back(normalizedIdentifier(memberDevice->second.type));
        }
        std::sort(types.begin(), types.end());
        netSignatures.push_back(joinNames(types));
    }
    std::sort(netSignatures.begin(), netSignatures.end());
    return type + "|" + joinNames(netSignatures);
}

using TerminalNetMap = std::map<ObjectId, std::map<std::string, const LogicalNet *>>;

TerminalNetMap terminalNetMap(const LogicalSide &side) {
    TerminalNetMap map;
    for (const auto &net : side.nets) {
        for (const auto &[device, pins] : net.terminals) {
            for (const auto &pin : pins)
                map[device][pin] = &net;
        }
    }
    return map;
}

LvsObjectRef deviceRef(const LogicalDevice &device, ObjectId cell) {
    LvsObjectRef ref;
    ref.kind = device.kind;
    ref.cell = cell;
    ref.id = device.object;
    ref.name = device.name;
    if (device.bounds.valid())
        ref.bounds = device.bounds;
    return ref;
}

double candidateScore(const LogicalDevice &target, const LogicalDevice &candidate, const LogicalSide &targetSide,
                      const LogicalSide &candidateSide, ObjectId targetId, ObjectId candidateId, std::string &reason) {
    double score = 0.0;
    std::vector<std::string> reasons;
    if (compatibleType(target.type, candidate.type)) {
        score += 0.35;
        reasons.push_back("same type");
    }
    if (!normalizedIdentifier(target.name).empty() &&
        normalizedIdentifier(target.name) == normalizedIdentifier(candidate.name)) {
        score += 0.35;
        reasons.push_back("same name");
    }
    const auto targetSignature = topologySignature(targetSide, targetId);
    const auto candidateSignature = topologySignature(candidateSide, candidateId);
    if (!targetSignature.empty() && targetSignature == candidateSignature) {
        score += 0.3;
        reasons.push_back("same terminal topology");
    }
    reason = joinNames(reasons);
    return std::min(1.0, score);
}

std::vector<LvsCandidate> deviceCandidates(const LogicalDevice &target, const LogicalSide &targetSide,
                                           ObjectId targetId, const LogicalSide &candidateSide, ObjectId candidateCell,
                                           bool candidatesAreLayout) {
    std::vector<LvsCandidate> candidates;
    for (const auto &[candidateId, candidate] : candidateSide.devices) {
        std::string reason;
        const auto score = candidateScore(target, candidate, targetSide, candidateSide, targetId, candidateId, reason);
        if (score <= 0.0)
            continue;
        LvsCandidate entry;
        entry.object = deviceRef(candidate, candidateCell);
        entry.score = score;
        entry.reason = reason.empty() ? (candidatesAreLayout ? "layout device" : "schematic device") : reason;
        candidates.push_back(std::move(entry));
    }
    std::sort(candidates.begin(), candidates.end(), [](const LvsCandidate &left, const LvsCandidate &right) {
        if (left.score != right.score)
            return left.score > right.score;
        return left.object.id < right.object.id;
    });
    if (candidates.size() > 3)
        candidates.resize(3);
    return candidates;
}

void matchByTopology(const LogicalSide &schematic, const LogicalSide &layout,
                     std::map<ObjectId, ObjectId> &schematicToLayout, std::map<ObjectId, ObjectId> &layoutToSchematic,
                     std::map<ObjectId, LvsMatchMethod> &layoutMethods) {
    std::map<std::string, std::vector<ObjectId>> unmatchedLayout;
    for (const auto &[layoutId, _] : layout.devices) {
        if (layoutToSchematic.contains(layoutId))
            continue;
        const auto signature = topologySignature(layout, layoutId);
        if (!signature.empty())
            unmatchedLayout[signature].push_back(layoutId);
    }
    for (auto &[_, candidates] : unmatchedLayout)
        std::sort(candidates.begin(), candidates.end());
    for (const auto &[schematicId, _] : schematic.devices) {
        if (schematicToLayout.contains(schematicId))
            continue;
        const auto signature = topologySignature(schematic, schematicId);
        if (signature.empty())
            continue;
        const auto found = unmatchedLayout.find(signature);
        if (found == unmatchedLayout.end())
            continue;
        for (const auto layoutId : found->second) {
            if (layoutToSchematic.contains(layoutId))
                continue;
            schematicToLayout.emplace(schematicId, layoutId);
            layoutToSchematic.emplace(layoutId, schematicId);
            layoutMethods[layoutId] = LvsMatchMethod::topology;
            break;
        }
    }
}

void assignDeviceKeys(const LogicalSide &schematic, const LayoutLogicalSide &layout,
                      const std::map<ObjectId, ObjectId> &schematicToLayout,
                      const std::map<ObjectId, ObjectId> &layoutToSchematic,
                      std::map<ObjectId, std::string> &schematicKeys, std::map<ObjectId, std::string> &layoutKeys,
                      std::map<std::string, std::string> &keyNames, ObjectId schematicCellId, ObjectId layoutCellId,
                      std::vector<LvsMismatch> &mismatches) {
    std::vector<std::pair<ObjectId, ObjectId>> pairs;
    pairs.reserve(schematicToLayout.size());
    for (const auto &[schematicId, layoutId] : schematicToLayout)
        pairs.emplace_back(schematicId, layoutId);
    std::sort(pairs.begin(), pairs.end(), [&](const auto &left, const auto &right) {
        const auto &leftDevice = schematic.devices.at(left.first);
        const auto &rightDevice = schematic.devices.at(right.first);
        const auto leftName = normalizedIdentifier(leftDevice.name);
        const auto rightName = normalizedIdentifier(rightDevice.name);
        if (leftName != rightName)
            return leftName < rightName;
        return left.first < right.first;
    });
    for (std::size_t index = 0; index < pairs.size(); ++index) {
        const auto key = "d" + std::to_string(index);
        schematicKeys.emplace(pairs[index].first, key);
        layoutKeys.emplace(pairs[index].second, key);
        keyNames.emplace(key, schematic.devices.at(pairs[index].first).name);
    }
    for (const auto &[schematicId, device] : schematic.devices) {
        if (schematicToLayout.contains(schematicId))
            continue;
        schematicKeys.emplace(schematicId, "s" + std::to_string(schematicId));
        keyNames.emplace("s" + std::to_string(schematicId), device.name);
        LvsMismatch mismatch{
            1,           "missing-device", "Schematic device '" + device.name + "' has no matching layout device",
            schematicId, std::nullopt,     VerificationSeverity::error};
        mismatch.schematicRef = deviceRef(device, schematicCellId);
        mismatch.candidates = deviceCandidates(device, schematic, schematicId, layout.side, layoutCellId, true);
        if (!mismatch.candidates.empty())
            mismatch.hint = "Closest layout candidate: " + mismatch.candidates.front().object.name;
        mismatches.push_back(std::move(mismatch));
    }
    for (const auto &[layoutId, device] : layout.side.devices) {
        if (layoutToSchematic.contains(layoutId))
            continue;
        layoutKeys.emplace(layoutId, "l" + std::to_string(layoutId));
        keyNames.emplace("l" + std::to_string(layoutId), device.name);
        LvsMismatch mismatch{1,
                             "extra-device",
                             "Layout device '" + device.name + "' has no matching schematic device",
                             std::nullopt,
                             layoutId,
                             VerificationSeverity::error};
        mismatch.layoutRef = deviceRef(device, layoutCellId);
        mismatch.candidates = deviceCandidates(device, layout.side, layoutId, schematic, schematicCellId, false);
        if (!mismatch.candidates.empty())
            mismatch.hint = "Closest schematic candidate: " + mismatch.candidates.front().object.name;
        mismatches.push_back(std::move(mismatch));
    }
}

bool netPairMatches(const LogicalNet *schematic, const LogicalNet *layout,
                    const std::map<const LogicalNet *, const LogicalNet *> &netPairs) {
    if (!schematic || !layout)
        return false;
    if (isGroundNetName(schematic->name) && isGroundNetName(layout->name))
        return true;
    const auto found = netPairs.find(schematic);
    if (found != netPairs.end() && found->second == layout)
        return true;
    return portKey(schematic->name) == portKey(layout->name);
}

std::vector<LvsPinMatch> compareDevicePins(const LogicalDevice &schematicDevice, ObjectId schematicId,
                                           const LogicalDevice &layoutDevice, ObjectId layoutId,
                                           const TerminalNetMap &schematicTerminals,
                                           const TerminalNetMap &layoutTerminals,
                                           const std::map<const LogicalNet *, const LogicalNet *> &netPairs) {
    const auto schematicPins = schematicTerminals.find(schematicId);
    const auto layoutPins = layoutTerminals.find(layoutId);
    if (schematicPins == schematicTerminals.end() || layoutPins == layoutTerminals.end())
        return {};
    if (schematicPins->second.empty() || layoutPins->second.empty())
        return {};

    const auto makeEntry = [&](const std::string &schematicPin, const std::string &layoutPin, bool swapped) {
        LvsPinMatch entry;
        entry.schematicPin = schematicPin;
        entry.layoutPin = layoutPin;
        if (const auto net = schematicPins->second.find(schematicPin); net != schematicPins->second.end()) {
            entry.schematicNet = net->second->name;
            if (const auto layoutNet = layoutPins->second.find(layoutPin); layoutNet != layoutPins->second.end()) {
                entry.layoutNet = layoutNet->second->name;
                entry.matched = netPairMatches(net->second, layoutNet->second, netPairs);
            }
        } else if (const auto layoutNet = layoutPins->second.find(layoutPin); layoutNet != layoutPins->second.end()) {
            entry.layoutNet = layoutNet->second->name;
        }
        entry.swapped = swapped;
        return entry;
    };

    std::vector<LvsPinMatch> entries;
    if (!schematicDevice.multiTerminal && !layoutDevice.multiTerminal && schematicPins->second.size() == 2 &&
        layoutPins->second.size() == 2) {
        std::vector<std::string> schematicNames;
        std::vector<std::string> layoutNames;
        for (const auto &[pin, _] : schematicPins->second)
            schematicNames.push_back(pin);
        for (const auto &[pin, _] : layoutPins->second)
            layoutNames.push_back(pin);
        std::sort(schematicNames.begin(), schematicNames.end());
        std::sort(layoutNames.begin(), layoutNames.end());
        bool straight = true;
        for (std::size_t index = 0; index < 2; ++index) {
            const auto entry = makeEntry(schematicNames[index], layoutNames[index], false);
            straight = straight && entry.matched;
        }
        bool reversed = true;
        for (std::size_t index = 0; index < 2; ++index) {
            const auto entry = makeEntry(schematicNames[index], layoutNames[1 - index], false);
            reversed = reversed && entry.matched;
        }
        if (reversed && !straight) {
            for (std::size_t index = 0; index < 2; ++index)
                entries.push_back(makeEntry(schematicNames[index], layoutNames[1 - index], true));
            return entries;
        }
        for (std::size_t index = 0; index < 2; ++index)
            entries.push_back(makeEntry(schematicNames[index], layoutNames[index], false));
        return entries;
    }

    std::set<std::string> usedLayoutPins;
    for (const auto &[pin, _] : schematicPins->second) {
        if (layoutPins->second.contains(pin)) {
            entries.push_back(makeEntry(pin, pin, false));
            usedLayoutPins.insert(pin);
        } else {
            entries.push_back(makeEntry(pin, {}, false));
        }
    }
    for (const auto &[pin, _] : layoutPins->second) {
        if (!usedLayoutPins.contains(pin))
            entries.push_back(makeEntry({}, pin, false));
    }
    return entries;
}

LvsParameterMatch parameterMatch(const std::string &name, const std::string &schematicValue,
                                 const std::string &layoutValue, bool matched) {
    return {name, schematicValue, layoutValue, matched};
}

void validateDevices(const LogicalSide &schematic, const LayoutLogicalSide &layout, const LvsOptions &options,
                     const std::map<ObjectId, ObjectId> &schematicToLayout,
                     const std::map<ObjectId, LvsMatchMethod> &layoutMethods, const TerminalNetMap &schematicTerminals,
                     const TerminalNetMap &layoutTerminals,
                     const std::map<const LogicalNet *, const LogicalNet *> &netPairs, ObjectId schematicCellId,
                     ObjectId layoutCellId, std::vector<LvsMismatch> &mismatches,
                     std::vector<LvsDeviceMatch> &matches) {
    std::vector<std::pair<ObjectId, ObjectId>> pairs;
    pairs.reserve(schematicToLayout.size());
    for (const auto &[schematicId, layoutId] : schematicToLayout)
        pairs.emplace_back(schematicId, layoutId);
    std::sort(pairs.begin(), pairs.end(), [&](const auto &left, const auto &right) {
        const auto &leftDevice = schematic.devices.at(left.first);
        const auto &rightDevice = schematic.devices.at(right.first);
        const auto leftName = normalizedIdentifier(leftDevice.name);
        const auto rightName = normalizedIdentifier(rightDevice.name);
        if (leftName != rightName)
            return leftName < rightName;
        return left.first < right.first;
    });

    for (const auto &[schematicId, layoutId] : pairs) {
        const auto &schematicDevice = schematic.devices.at(schematicId);
        const auto &layoutDevice = layout.side.devices.at(layoutId);
        LvsDeviceMatch match;
        match.schematic = deviceRef(schematicDevice, schematicCellId);
        match.layout = deviceRef(layoutDevice, layoutCellId);
        match.schematicType = schematicDevice.type;
        match.layoutType = layoutDevice.type;
        if (const auto method = layoutMethods.find(layoutId); method != layoutMethods.end())
            match.method = method->second;
        match.matched = true;
        const auto displayName = schematicDevice.name.empty() ? layoutDevice.name : schematicDevice.name;
        if (normalizedIdentifier(schematicDevice.name) != normalizedIdentifier(layoutDevice.name)) {
            mismatches.push_back(
                {1, "device-name",
                 "Schematic device '" + schematicDevice.name + "' is named '" + layoutDevice.name + "' in the layout",
                 schematicId, layoutId, VerificationSeverity::info});
        }
        if (!compatibleType(schematicDevice.type, layoutDevice.type)) {
            match.matched = false;
            mismatches.push_back({1, "device-type",
                                  "Device '" + displayName + "' is '" + kindName(true, schematicDevice) +
                                      "' in the schematic but '" + kindName(false, layoutDevice) + "' in the layout",
                                  schematicId, layoutId, VerificationSeverity::error});
        }

        if (options.compareParameters) {
            std::map<std::string, std::string> layoutParameters;
            for (const auto &[name, value] : layoutDevice.parameters)
                layoutParameters[normalizedIdentifier(name)] = value;
            std::set<std::string> comparedNames;
            for (const auto &[name, schematicValue] : schematicDevice.parameters) {
                const auto normalizedName = normalizedIdentifier(name);
                if (normalizedName == "model" || normalizedName == "spice_model" || normalizedName == "subckt" ||
                    normalizedName == "symbol" || normalizedName == "device" || normalizedName == "type" ||
                    normalizedName == "cell" || normalizedName == "name")
                    continue;
                if ((normalizedName == "m" || normalizedName == "multiplicity" || normalizedName == "parallel" ||
                     normalizedName == "mult") &&
                    parameterValuesEqual(schematicValue, "1"))
                    continue;
                comparedNames.insert(normalizedName);
                match.parametersCompared = true;
                if (!layoutDevice.hasParameters) {
                    match.matched = false;
                    match.parameters.push_back(parameterMatch(name, schematicValue, {}, false));
                    continue;
                }
                const auto layoutValue = findParameter(layoutDevice.parameters, normalizedName);
                if (!layoutValue) {
                    match.matched = false;
                    match.parameters.push_back(parameterMatch(name, schematicValue, {}, false));
                    mismatches.push_back(
                        {1, "parameter-missing",
                         "Device '" + displayName + "' parameter '" + name + "' is missing from the layout device",
                         schematicId, layoutId, VerificationSeverity::error});
                    continue;
                }
                bool equal = parameterValuesEqual(schematicValue, *layoutValue);
                if (!equal &&
                    (normalizedName == "w" || normalizedName == "l" || normalizedName == "width" ||
                     normalizedName == "length") &&
                    lengthParameterValuesEqual(schematicValue, *layoutValue))
                    equal = true;
                match.parameters.push_back(parameterMatch(name, schematicValue, *layoutValue, equal));
                if (!equal) {
                    match.matched = false;
                    mismatches.push_back({1, "parameter",
                                          "Device '" + displayName + "' parameter '" + name + "' is '" +
                                              schematicValue + "' in the schematic but '" + *layoutValue +
                                              "' in the layout",
                                          schematicId, layoutId, VerificationSeverity::error});
                }
            }
            if (!layoutDevice.hasParameters && match.parametersCompared) {
                mismatches.push_back({1, "parameter-unavailable",
                                      "Layout device '" + displayName +
                                          "' has no parameter metadata; schematic parameters were not compared",
                                      schematicId, layoutId, VerificationSeverity::error});
            }
            for (const auto &[name, value] : layoutDevice.parameters) {
                const auto normalizedName = normalizedIdentifier(name);
                if (comparedNames.contains(normalizedName))
                    continue;
                if (normalizedName == "model" || normalizedName == "spice_model" || normalizedName == "subckt" ||
                    normalizedName == "symbol" || normalizedName == "device" || normalizedName == "type" ||
                    normalizedName == "cell" || normalizedName == "name")
                    continue;
                match.parameters.push_back(parameterMatch(name, {}, value, true));
            }
        }

        match.pins = compareDevicePins(schematicDevice, schematicId, layoutDevice, layoutId, schematicTerminals,
                                       layoutTerminals, netPairs);
        for (const auto &pin : match.pins) {
            if (!pin.matched)
                match.matched = false;
        }
        if (!match.matched && !match.pins.empty() && match.schematic && match.layout) {
            // Keep the type/parameter mismatch as the primary entry; pin
            // failures are already covered by the net comparison.
        }
        matches.push_back(std::move(match));
    }
}

using NetSignature = std::vector<std::string>;

struct NetRecord {
    NetSignature signature;
    std::string name;
    ObjectId object = 0;
    const LogicalNet *net = nullptr;
    LvsObjectKind kind = LvsObjectKind::net;
    Rect bounds;
};

NetRecord netRecord(const LogicalNet &net, const LogicalSide &side, const std::map<ObjectId, std::string> &keys) {
    NetRecord record;
    record.name = net.name;
    record.object = net.object;
    record.net = &net;
    record.kind = net.kind;
    record.bounds = net.bounds;
    for (const auto device : net.devices) {
        const auto key = keys.find(device);
        if (key == keys.end())
            continue;
        const auto logicalDevice = side.devices.find(device);
        const auto terminals = net.terminals.find(device);
        if (terminals != net.terminals.end() && !terminals->second.empty()) {
            if (logicalDevice != side.devices.end() && logicalDevice->second.multiTerminal) {
                for (const auto &pin : terminals->second)
                    record.signature.push_back(key->second + ":" + pin);
            } else {
                record.signature.push_back(key->second);
            }
        } else if (logicalDevice != side.devices.end() && logicalDevice->second.multiTerminal) {
            // Missing terminal labels on a multi-terminal device cannot be
            // assumed to match any particular pin assignment.
            record.signature.push_back(key->second + ":?");
        } else {
            record.signature.push_back(key->second);
        }
    }
    std::sort(record.signature.begin(), record.signature.end());
    return record;
}

std::vector<std::string> signatureConnectionList(const NetSignature &signature,
                                                 const std::map<std::string, std::string> &keyNames) {
    std::vector<std::string> names;
    names.reserve(signature.size());
    for (const auto &key : signature) {
        const auto separator = key.find(':');
        const auto base = separator == std::string::npos ? key : key.substr(0, separator);
        const auto name = keyNames.find(base);
        std::string text = name == keyNames.end() ? base : name->second;
        if (separator != std::string::npos) {
            const auto pin = key.substr(separator + 1);
            if (!pin.empty())
                text += ":" + pin;
        }
        names.push_back(std::move(text));
    }
    return names;
}

std::string joinConnections(const std::vector<std::string> &names) { return joinNames(names); }

std::string signatureNames(const NetSignature &signature, const std::map<std::string, std::string> &keyNames) {
    return joinConnections(signatureConnectionList(signature, keyNames));
}

LvsObjectRef netRef(const NetRecord &record, ObjectId cell) {
    LvsObjectRef ref;
    ref.kind = record.kind;
    ref.cell = cell;
    ref.id = record.object;
    ref.name = record.name;
    if (record.bounds.valid())
        ref.bounds = record.bounds;
    return ref;
}

double signatureOverlap(const NetSignature &left, const NetSignature &right) {
    if (left.empty() || right.empty())
        return 0.0;
    std::size_t intersection = 0;
    std::size_t leftIndex = 0;
    std::size_t rightIndex = 0;
    while (leftIndex < left.size() && rightIndex < right.size()) {
        if (left[leftIndex] == right[rightIndex]) {
            ++intersection;
            ++leftIndex;
            ++rightIndex;
        } else if (left[leftIndex] < right[rightIndex]) {
            ++leftIndex;
        } else {
            ++rightIndex;
        }
    }
    const auto unionSize = left.size() + right.size() - intersection;
    return unionSize == 0 ? 0.0 : static_cast<double>(intersection) / static_cast<double>(unionSize);
}

std::vector<LvsCandidate> netCandidates(const NetRecord &target, const std::vector<NetRecord> &candidates,
                                        ObjectId candidateCell) {
    std::vector<LvsCandidate> result;
    for (const auto &candidate : candidates) {
        auto score = signatureOverlap(target.signature, candidate.signature);
        std::size_t shared = 0;
        std::size_t leftIndex = 0;
        std::size_t rightIndex = 0;
        while (leftIndex < target.signature.size() && rightIndex < candidate.signature.size()) {
            if (target.signature[leftIndex] == candidate.signature[rightIndex]) {
                ++shared;
                ++leftIndex;
                ++rightIndex;
            } else if (target.signature[leftIndex] < candidate.signature[rightIndex]) {
                ++leftIndex;
            } else {
                ++rightIndex;
            }
        }
        if (portKey(target.name) == portKey(candidate.name))
            score += 0.2;
        if (score <= 0.0)
            continue;
        LvsCandidate entry;
        entry.object = netRef(candidate, candidateCell);
        entry.score = std::min(1.0, score);
        entry.reason = "shares " + std::to_string(shared) + " device terminal(s)";
        result.push_back(std::move(entry));
    }
    std::sort(result.begin(), result.end(), [](const LvsCandidate &left, const LvsCandidate &right) {
        if (left.score != right.score)
            return left.score > right.score;
        return left.object.name < right.object.name;
    });
    if (result.size() > 3)
        result.resize(3);
    return result;
}

int comparePorts(const LogicalSide &schematic, const LogicalSide &layout, ObjectId schematicCellId,
                 ObjectId layoutCellId, std::vector<LvsMismatch> &mismatches, std::vector<LvsPortMatch> &matches) {
    if (schematic.ports.empty() && layout.ports.empty())
        return 0;
    std::map<std::string, LogicalPort> schematicPorts;
    std::map<std::string, LogicalPort> layoutPorts;
    const auto addPort = [&](std::map<std::string, LogicalPort> &ports, const LogicalPort &port) {
        const auto key = portKey(port.name);
        if (!ports.emplace(key, port).second)
            mismatches.push_back({1, "duplicate-port",
                                  "Port name '" + port.name + "' collides with another port after normalization",
                                  std::optional<ObjectId>(port.object), std::nullopt, VerificationSeverity::error});
    };
    for (const auto &port : schematic.ports)
        addPort(schematicPorts, port);
    for (const auto &port : layout.ports)
        addPort(layoutPorts, port);

    int matched = 0;
    for (const auto &[name, port] : schematicPorts) {
        const auto layoutPort = layoutPorts.find(name);
        LvsPortMatch pair;
        pair.schematic = LvsObjectRef{LvsObjectKind::port, schematicCellId, port.object, port.name, std::nullopt};
        pair.schematicDirection = port.direction;
        pair.schematicNet = port.net;
        if (layoutPort == layoutPorts.end()) {
            pair.matched = false;
            matches.push_back(std::move(pair));
            mismatches.push_back({1, "missing-port", "Schematic port '" + port.name + "' has no matching layout port",
                                  port.object, std::nullopt, VerificationSeverity::error});
            continue;
        }
        pair.layout = LvsObjectRef{LvsObjectKind::port, layoutCellId, layoutPort->second.object,
                                   layoutPort->second.name, layoutPort->second.bounds};
        pair.layoutDirection = layoutPort->second.direction;
        pair.layoutNet = layoutPort->second.net;
        ++matched;
        pair.matched = true;
        const auto flexibleDirection = [](const std::string &direction) {
            return direction.empty() || direction == "ground" || direction == "power" || direction == "inout";
        };
        pair.directionMatched = flexibleDirection(port.direction) || flexibleDirection(layoutPort->second.direction) ||
                                port.direction == layoutPort->second.direction;
        if (!pair.directionMatched)
            mismatches.push_back({1, "port-direction",
                                  "Port '" + port.name + "' is '" + port.direction + "' in the schematic but '" +
                                      layoutPort->second.direction + "' in the layout",
                                  port.object, layoutPort->second.object, VerificationSeverity::warning});
        const bool schematicGround = isGroundNetName(port.net);
        const bool layoutGround = isGroundNetName(layoutPort->second.net);
        pair.netMatched = port.net.empty() || (schematicGround && layoutGround) ||
                          (!schematicGround && !layoutGround && portKey(port.net) == portKey(layoutPort->second.net));
        if (!port.net.empty() && !pair.netMatched)
            mismatches.push_back({1, "port-net",
                                  "Port '" + port.name + "' is connected to net '" + port.net +
                                      "' in the schematic but '" +
                                      (layoutPort->second.net.empty() ? "an unassigned port" : layoutPort->second.net) +
                                      "' in the layout",
                                  port.object, layoutPort->second.object, VerificationSeverity::error});
        matches.push_back(std::move(pair));
    }
    for (const auto &[name, port] : layoutPorts) {
        if (schematicPorts.contains(name))
            continue;
        LvsPortMatch pair;
        pair.layout = LvsObjectRef{LvsObjectKind::port, layoutCellId, port.object, port.name, port.bounds};
        pair.layoutDirection = port.direction;
        pair.layoutNet = port.net;
        pair.matched = false;
        matches.push_back(std::move(pair));
        mismatches.push_back({1, "extra-port", "Layout port '" + port.name + "' has no matching schematic port",
                              std::nullopt, port.object, VerificationSeverity::error});
    }
    return matched;
}

void compareNets(const LogicalSide &schematic, const LayoutLogicalSide &layout,
                 const std::map<ObjectId, std::string> &schematicKeys,
                 const std::map<ObjectId, std::string> &layoutKeys, const std::map<std::string, std::string> &keyNames,
                 ObjectId schematicCellId, ObjectId layoutCellId, std::vector<LvsMismatch> &mismatches,
                 std::vector<LvsNetMatch> &matches, std::map<const LogicalNet *, const LogicalNet *> &schematicToLayout,
                 std::map<const LogicalNet *, const LogicalNet *> &layoutToSchematic) {
    if (schematic.nets.empty()) {
        if (!schematic.devices.empty())
            mismatches.push_back({1, "schematic-netlist",
                                  "Schematic has devices but no nets; LVS cannot verify connectivity", std::nullopt,
                                  std::nullopt, VerificationSeverity::error});
        return;
    }
    if (!layout.hasConnectivity) {
        mismatches.push_back({1, "layout-netlist",
                              "Layout has no planned-connection connectivity; LVS compared devices only", std::nullopt,
                              std::nullopt, VerificationSeverity::warning});
        return;
    }

    const auto isSignificantNet = [](const LogicalNet &net, const LogicalSide &side) {
        if (net.devices.size() >= 2)
            return true;
        // A single-terminal net still matters for multi-terminal devices; it
        // is where a swapped or dangling gate/drain/source would hide.
        return std::any_of(net.devices.begin(), net.devices.end(), [&](ObjectId device) {
            const auto found = side.devices.find(device);
            return found != side.devices.end() && found->second.multiTerminal;
        });
    };
    std::vector<NetRecord> schematicNets;
    for (const auto &net : schematic.nets) {
        if (isSignificantNet(net, schematic))
            schematicNets.push_back(netRecord(net, schematic, schematicKeys));
    }
    std::vector<NetRecord> layoutNets;
    for (const auto &net : layout.side.nets) {
        if (isSignificantNet(net, layout.side))
            layoutNets.push_back(netRecord(net, layout.side, layoutKeys));
    }
    std::sort(schematicNets.begin(), schematicNets.end(), [](const NetRecord &left, const NetRecord &right) {
        if (left.signature != right.signature)
            return left.signature < right.signature;
        return left.object < right.object;
    });
    std::sort(layoutNets.begin(), layoutNets.end(), [](const NetRecord &left, const NetRecord &right) {
        if (left.signature != right.signature)
            return left.signature < right.signature;
        return left.object < right.object;
    });

    std::map<NetSignature, std::vector<std::size_t>> layoutBySignature;
    for (std::size_t index = 0; index < layoutNets.size(); ++index)
        layoutBySignature[layoutNets[index].signature].push_back(index);
    std::set<std::size_t> pairedLayout;
    std::set<std::size_t> pairedSchematic;

    for (std::size_t schematicIndex = 0; schematicIndex < schematicNets.size(); ++schematicIndex) {
        const auto &record = schematicNets[schematicIndex];
        const auto found = layoutBySignature.find(record.signature);
        if (found == layoutBySignature.end())
            continue;
        for (const auto layoutIndex : found->second) {
            if (pairedLayout.contains(layoutIndex))
                continue;
            pairedSchematic.insert(schematicIndex);
            pairedLayout.insert(layoutIndex);
            const auto &layoutRecord = layoutNets[layoutIndex];
            schematicToLayout[record.net] = layoutRecord.net;
            layoutToSchematic[layoutRecord.net] = record.net;
            LvsNetMatch pair;
            pair.schematic = netRef(record, schematicCellId);
            pair.layout = netRef(layoutRecord, layoutCellId);
            pair.schematicConnections = signatureConnectionList(record.signature, keyNames);
            pair.layoutConnections = signatureConnectionList(layoutRecord.signature, keyNames);
            pair.matched = true;
            pair.ground = isGroundNetName(record.name) || isGroundNetName(layoutRecord.name);
            matches.push_back(std::move(pair));
            break;
        }
    }

    for (std::size_t index = 0; index < schematicNets.size(); ++index) {
        if (pairedSchematic.contains(index))
            continue;
        const auto &record = schematicNets[index];
        LvsMismatch mismatch{1,
                             "missing-connection",
                             "Schematic net '" + record.name + "' connects " +
                                 signatureNames(record.signature, keyNames) + " but no matching layout net was found",
                             record.object,
                             std::nullopt,
                             VerificationSeverity::error};
        mismatch.schematicRef = netRef(record, schematicCellId);
        mismatch.candidates = netCandidates(record, layoutNets, layoutCellId);
        if (!mismatch.candidates.empty())
            mismatch.hint = "Closest layout net: " + mismatch.candidates.front().object.name;
        mismatches.push_back(std::move(mismatch));
        LvsNetMatch pair;
        pair.schematic = netRef(record, schematicCellId);
        pair.schematicConnections = signatureConnectionList(record.signature, keyNames);
        pair.matched = false;
        matches.push_back(std::move(pair));
    }

    for (std::size_t index = 0; index < layoutNets.size(); ++index) {
        if (pairedLayout.contains(index))
            continue;
        const auto &record = layoutNets[index];
        LvsMismatch mismatch{1,
                             "extra-connection",
                             "Layout net '" + record.name + "' connects " + signatureNames(record.signature, keyNames) +
                                 " but no matching schematic net was found",
                             std::nullopt,
                             record.object,
                             VerificationSeverity::error};
        mismatch.layoutRef = netRef(record, layoutCellId);
        mismatch.candidates = netCandidates(record, schematicNets, schematicCellId);
        if (!mismatch.candidates.empty())
            mismatch.hint = "Closest schematic net: " + mismatch.candidates.front().object.name;
        mismatches.push_back(std::move(mismatch));
        LvsNetMatch pair;
        pair.layout = netRef(record, layoutCellId);
        pair.layoutConnections = signatureConnectionList(record.signature, keyNames);
        pair.matched = false;
        matches.push_back(std::move(pair));
    }
}

std::string schematicBaseName(std::string name) {
    auto normalized = normalizedIdentifier(name);
    // Strip decorators repeatedly: a procedural layout keeps both its own
    // "_layout" suffix and the engine's "_generated" one
    // ("miller_opamp_layout (generated)" -> "miller_opamp").
    for (bool stripped = true; stripped;) {
        stripped = false;
        for (const auto suffix : {"_generated", "_layout", "_dut", "_lvs"}) {
            const std::string text(suffix);
            if (normalized.size() > text.size() && normalized.ends_with(text)) {
                normalized.resize(normalized.size() - text.size());
                stripped = true;
                break;
            }
        }
    }
    return normalized;
}

} // namespace

bool LvsReport::matched() const {
    return std::none_of(mismatches.begin(), mismatches.end(), [](const LvsMismatch &mismatch) {
        return mismatch.severity == VerificationSeverity::error || mismatch.severity == VerificationSeverity::warning;
    });
}

ObjectId schematicForLayout(const Project &project, ObjectId layoutCellId) {
    const auto layout = project.layoutCells.find(layoutCellId);
    if (layout == project.layoutCells.end())
        return 0;
    if (layout->second.schematicCell != 0 && project.schematicCells.contains(layout->second.schematicCell))
        return layout->second.schematicCell;

    std::set<ObjectId> linkedInstances;
    for (const auto &[_, device] : layout->second.plannedDevices) {
        if (device.schematicInstance != 0)
            linkedInstances.insert(device.schematicInstance);
    }
    for (const auto &[_, instance] : layout->second.instances) {
        if (instance.schematicInstance != 0)
            linkedInstances.insert(instance.schematicInstance);
    }
    for (const auto &[_, port] : layout->second.ports) {
        if (port.schematicInstance != 0)
            linkedInstances.insert(port.schematicInstance);
    }

    std::set<ObjectId> candidates;
    if (!linkedInstances.empty()) {
        for (const auto &[cellId, cell] : project.schematicCells) {
            for (const auto &[instanceId, _] : cell.instances) {
                if (linkedInstances.contains(instanceId)) {
                    candidates.insert(cellId);
                    break;
                }
            }
        }
    }
    if (candidates.size() == 1)
        return *candidates.begin();

    const auto layoutName = normalizedIdentifier(layout->second.name);
    const auto layoutBase = schematicBaseName(layout->second.name);
    std::map<std::string, ObjectId> byName;
    for (const auto &[cellId, cell] : project.schematicCells)
        byName[normalizedIdentifier(cell.name)] = cellId;
    const auto pdkMatches = [&](ObjectId cellId) {
        const auto cell = project.schematicCells.find(cellId);
        return cell == project.schematicCells.end() || layout->second.pdk.empty() ||
               layout->second.pdk == genericPdkId || cell->second.pdk.empty() || cell->second.pdk == layout->second.pdk;
    };
    for (const auto &candidate :
         {layoutBase + "_lvs", layoutBase + "_dut", layoutBase + "_layout", layoutBase, layoutName}) {
        const auto it = byName.find(candidate);
        if (it != byName.end() && pdkMatches(it->second))
            return it->second;
    }
    std::vector<ObjectId> fallback;
    for (const auto &[_, cellId] : byName) {
        if (pdkMatches(cellId))
            fallback.push_back(cellId);
    }
    return fallback.size() == 1 ? fallback.front() : 0;
}

void decorateMismatches(LvsReport &report, const LogicalSide &schematic, const LogicalSide &layout,
                        ObjectId schematicCellId, ObjectId layoutCellId, const LayoutCell &layoutCell,
                        const SchematicCell &schematicCell) {
    const auto schematicDeviceRef = [&](ObjectId id) -> std::optional<LvsObjectRef> {
        const auto found = schematic.devices.find(id);
        return found == schematic.devices.end()
                   ? std::nullopt
                   : std::optional<LvsObjectRef>(deviceRef(found->second, schematicCellId));
    };
    const auto layoutDeviceRef = [&](ObjectId id) -> std::optional<LvsObjectRef> {
        const auto found = layout.devices.find(id);
        return found == layout.devices.end() ? std::nullopt
                                             : std::optional<LvsObjectRef>(deviceRef(found->second, layoutCellId));
    };
    const auto schematicNetRef = [&](ObjectId id) -> std::optional<LvsObjectRef> {
        for (const auto &net : schematic.nets) {
            if (net.object == id)
                return LvsObjectRef{LvsObjectKind::net, schematicCellId, net.object, net.name, std::nullopt};
        }
        return std::nullopt;
    };
    const auto layoutNetRef = [&](ObjectId id) -> std::optional<LvsObjectRef> {
        for (const auto &net : layout.nets) {
            if (net.object == id) {
                LvsObjectRef ref{net.kind, layoutCellId, net.object, net.name, std::nullopt};
                if (net.bounds.valid())
                    ref.bounds = net.bounds;
                return ref;
            }
        }
        return std::nullopt;
    };
    const auto schematicPortRef = [&](ObjectId id) -> std::optional<LvsObjectRef> {
        for (const auto &port : schematic.ports) {
            if (port.object == id)
                return LvsObjectRef{LvsObjectKind::port, schematicCellId, port.object, port.name, std::nullopt};
        }
        return std::nullopt;
    };
    const auto layoutPortRef = [&](ObjectId id) -> std::optional<LvsObjectRef> {
        for (const auto &port : layout.ports) {
            if (port.object == id) {
                LvsObjectRef ref{LvsObjectKind::port, layoutCellId, port.object, port.name, std::nullopt};
                if (port.bounds.valid())
                    ref.bounds = port.bounds;
                return ref;
            }
        }
        return std::nullopt;
    };

    for (auto &mismatch : report.mismatches) {
        const auto category = mismatch.category;
        if (!mismatch.schematicRef && mismatch.schematicObject) {
            const auto id = *mismatch.schematicObject;
            if (category.find("port") != std::string::npos)
                mismatch.schematicRef = schematicPortRef(id);
            else if (category.find("connection") != std::string::npos || category.find("net") != std::string::npos ||
                     category.find("short") != std::string::npos)
                mismatch.schematicRef = schematicNetRef(id);
            else
                mismatch.schematicRef = schematicDeviceRef(id);
            if (!mismatch.schematicRef)
                mismatch.schematicRef = schematicNetRef(id);
            if (!mismatch.schematicRef)
                mismatch.schematicRef = schematicPortRef(id);
            if (!mismatch.schematicRef) {
                if (const auto instance = schematicCell.instances.find(id); instance != schematicCell.instances.end()) {
                    LvsObjectRef ref;
                    ref.kind = LvsObjectKind::device;
                    ref.cell = schematicCellId;
                    ref.id = id;
                    ref.name = instance->second.name;
                    mismatch.schematicRef = ref;
                }
            }
        }
        if (!mismatch.layoutRef && mismatch.layoutObject) {
            const auto id = *mismatch.layoutObject;
            if (category == "layout-connection") {
                LvsObjectRef ref;
                ref.kind = LvsObjectKind::connection;
                ref.cell = layoutCellId;
                ref.id = id;
                ref.name = "Connection " + std::to_string(id);
                mismatch.layoutRef = ref;
            } else if (category.find("port") != std::string::npos) {
                mismatch.layoutRef = layoutPortRef(id);
            } else if (category.find("connection") != std::string::npos || category.find("net") != std::string::npos ||
                       category.find("short") != std::string::npos) {
                mismatch.layoutRef = layoutNetRef(id);
            } else {
                mismatch.layoutRef = layoutDeviceRef(id);
            }
            if (!mismatch.layoutRef) {
                if (const auto instance = layoutCell.instances.find(id); instance != layoutCell.instances.end()) {
                    LvsObjectRef ref;
                    ref.kind = LvsObjectKind::instance;
                    ref.cell = layoutCellId;
                    ref.id = id;
                    ref.name = instance->second.name;
                    mismatch.layoutRef = ref;
                } else if (const auto planned = layoutCell.plannedDevices.find(id);
                           planned != layoutCell.plannedDevices.end()) {
                    LvsObjectRef ref;
                    ref.kind = LvsObjectKind::plannedDevice;
                    ref.cell = layoutCellId;
                    ref.id = id;
                    ref.name = planned->second.name;
                    ref.bounds =
                        Rect{planned->second.x, planned->second.y, planned->second.width, planned->second.height};
                    mismatch.layoutRef = ref;
                }
            }
        }
        if (mismatch.hint.empty() && !mismatch.candidates.empty()) {
            mismatch.hint = "Closest candidate: " + mismatch.candidates.front().object.name;
        }
    }
}

void summarizeReport(LvsReport &report) {
    report.summary = {};
    for (const auto &match : report.devices) {
        if (match.schematic && match.layout) {
            if (match.matched)
                ++report.summary.devicesMatched;
            else
                ++report.summary.devicesMismatched;
        } else if (match.schematic) {
            ++report.summary.devicesSchematicOnly;
        } else if (match.layout) {
            ++report.summary.devicesLayoutOnly;
        }
    }
    for (const auto &match : report.nets) {
        if (match.schematic && match.layout)
            ++report.summary.netsMatched;
        else if (match.schematic)
            ++report.summary.netsSchematicOnly;
        else if (match.layout)
            ++report.summary.netsLayoutOnly;
    }
    for (const auto &match : report.ports) {
        if (match.schematic && match.layout)
            ++report.summary.portsMatched;
    }
}

LvsReport runLvs(const Project &project, ObjectId schematicCellId, ObjectId layoutCellId, const LvsOptions &options) {
    const auto schematicIt = project.schematicCells.find(schematicCellId);
    if (schematicIt == project.schematicCells.end())
        throw ProjectError("lvs.schematic", "Schematic cell does not exist");
    const auto layoutIt = project.layoutCells.find(layoutCellId);
    if (layoutIt == project.layoutCells.end())
        throw ProjectError("lvs.layout", "Layout cell does not exist");
    if (layoutIt->second.library)
        throw ProjectError("lvs.layout", "LVS compares user layout cells, not read-only library cells");

    LvsReport report;
    report.schematic = schematicIt->second.name;
    report.layout = layoutIt->second.name;

    auto schematic = collectSchematicSide(project, schematicCellId, report.mismatches);
    auto layout = collectLayoutSide(project, layoutCellId, schematicIt->second, report.mismatches);
    expandMultiplicity(schematic);
    expandMultiplicity(layout.side);
    report.portsMatched =
        comparePorts(schematic, layout.side, schematicCellId, layoutCellId, report.mismatches, report.ports);

    std::map<std::string, std::vector<ObjectId>> schematicByName;
    for (const auto &[id, device] : schematic.devices)
        schematicByName[normalizedIdentifier(device.name)].push_back(id);
    std::map<std::string, std::vector<ObjectId>> layoutByName;
    for (const auto &[id, device] : layout.side.devices)
        layoutByName[normalizedIdentifier(device.name)].push_back(id);
    for (auto &entry : schematicByName)
        std::sort(entry.second.begin(), entry.second.end());
    for (auto &entry : layoutByName)
        std::sort(entry.second.begin(), entry.second.end());

    std::map<ObjectId, ObjectId> schematicToLayout;
    std::map<ObjectId, ObjectId> layoutToSchematic;
    std::map<ObjectId, LvsMatchMethod> layoutMethods;
    const auto match = [&](ObjectId schematicId, ObjectId layoutId, LvsMatchMethod method) {
        if (!schematicToLayout.emplace(schematicId, layoutId).second)
            return;
        layoutToSchematic.emplace(layoutId, schematicId);
        layoutMethods[layoutId] = method;
    };

    for (const auto &[layoutId, device] : layout.side.devices) {
        if (device.schematicInstance == 0 || !schematic.devices.contains(device.schematicInstance))
            continue;
        if (layoutToSchematic.contains(layoutId) || schematicToLayout.contains(device.schematicInstance))
            continue;
        match(device.schematicInstance, layoutId, LvsMatchMethod::linked);
    }
    for (const auto &[layoutId, device] : layout.side.devices) {
        if (layoutToSchematic.contains(layoutId))
            continue;
        const auto name = normalizedIdentifier(device.name);
        const auto candidates = schematicByName.find(name);
        if (candidates == schematicByName.end() || candidates->second.size() != 1)
            continue;
        if (const auto peers = layoutByName.find(name); peers == layoutByName.end() || peers->second.size() != 1)
            continue;
        const auto schematicId = candidates->second.front();
        if (schematicToLayout.contains(schematicId))
            continue;
        match(schematicId, layoutId, LvsMatchMethod::name);
    }
    matchByTopology(schematic, layout.side, schematicToLayout, layoutToSchematic, layoutMethods);

    std::map<ObjectId, std::string> schematicKeys;
    std::map<ObjectId, std::string> layoutKeys;
    std::map<std::string, std::string> keyNames;
    assignDeviceKeys(schematic, layout, schematicToLayout, layoutToSchematic, schematicKeys, layoutKeys, keyNames,
                     schematicCellId, layoutCellId, report.mismatches);

    const auto schematicTerminals = terminalNetMap(schematic);
    const auto layoutTerminals = terminalNetMap(layout.side);
    std::map<const LogicalNet *, const LogicalNet *> schematicNetToLayout;
    std::map<const LogicalNet *, const LogicalNet *> layoutNetToSchematic;
    compareNets(schematic, layout, schematicKeys, layoutKeys, keyNames, schematicCellId, layoutCellId,
                report.mismatches, report.nets, schematicNetToLayout, layoutNetToSchematic);
    validateDevices(schematic, layout, options, schematicToLayout, layoutMethods, schematicTerminals, layoutTerminals,
                    schematicNetToLayout, schematicCellId, layoutCellId, report.mismatches, report.devices);

    std::map<ObjectId, std::vector<ObjectId>> linkedLayoutDevices;
    for (const auto &[layoutId, device] : layout.side.devices) {
        if (device.schematicInstance != 0)
            linkedLayoutDevices[device.schematicInstance].push_back(layoutId);
    }
    for (const auto &[schematicInstance, layoutDevices] : linkedLayoutDevices) {
        if (layoutDevices.size() < 2 || !schematic.devices.contains(schematicInstance))
            continue;
        std::string names;
        for (const auto layoutId : layoutDevices) {
            if (!names.empty())
                names += ", ";
            names += layout.side.devices.at(layoutId).name;
        }
        report.mismatches.push_back({1, "duplicate-link",
                                     "Schematic device '" + schematic.devices.at(schematicInstance).name +
                                         "' is linked from multiple layout devices: " + names,
                                     schematicInstance, std::nullopt, VerificationSeverity::info});
    }

    decorateMismatches(report, schematic, layout.side, schematicCellId, layoutCellId, layoutIt->second,
                       schematicIt->second);
    summarizeReport(report);
    return report;
}

const char *lvsObjectKindName(LvsObjectKind kind) {
    switch (kind) {
    case LvsObjectKind::device:
        return "device";
    case LvsObjectKind::net:
        return "net";
    case LvsObjectKind::port:
        return "port";
    case LvsObjectKind::plannedDevice:
        return "planned_device";
    case LvsObjectKind::instance:
        return "instance";
    case LvsObjectKind::shape:
        return "shape";
    case LvsObjectKind::connection:
        return "connection";
    case LvsObjectKind::geometry:
        return "geometry";
    }
    return "device";
}

const char *lvsMatchMethodName(LvsMatchMethod method) {
    switch (method) {
    case LvsMatchMethod::none:
        return "none";
    case LvsMatchMethod::linked:
        return "linked";
    case LvsMatchMethod::name:
        return "name";
    case LvsMatchMethod::topology:
        return "topology";
    }
    return "none";
}

std::string serializeLvsReport(const LvsReport &report) {
    json output{{"schema_version", report.schemaVersion},
                {"schematic", report.schematic},
                {"layout", report.layout},
                {"matched", report.matched()},
                {"ports_matched", report.portsMatched},
                {"summary",
                 {{"devices_matched", report.summary.devicesMatched},
                  {"devices_mismatched", report.summary.devicesMismatched},
                  {"devices_schematic_only", report.summary.devicesSchematicOnly},
                  {"devices_layout_only", report.summary.devicesLayoutOnly},
                  {"nets_matched", report.summary.netsMatched},
                  {"nets_schematic_only", report.summary.netsSchematicOnly},
                  {"nets_layout_only", report.summary.netsLayoutOnly},
                  {"ports_matched", report.summary.portsMatched}}},
                {"devices", json::array()},
                {"nets", json::array()},
                {"ports", json::array()},
                {"mismatches", json::array()}};
    const auto optionalId = [](std::optional<ObjectId> id) -> json { return id ? json(*id) : json(nullptr); };
    const auto boundsJson = [](const Rect &bounds) -> json {
        if (!bounds.valid())
            return nullptr;
        return json::array({bounds.x, bounds.y, bounds.width, bounds.height});
    };
    const auto refJson = [&](const LvsObjectRef &ref) {
        json value{{"kind", lvsObjectKindName(ref.kind)}, {"cell", ref.cell}, {"id", ref.id}, {"name", ref.name}};
        if (ref.bounds)
            value["bounds"] = boundsJson(*ref.bounds);
        return value;
    };
    const auto optionalRefJson = [&](const std::optional<LvsObjectRef> &ref) -> json {
        return ref ? refJson(*ref) : json(nullptr);
    };
    const auto candidateJson = [&](const LvsCandidate &candidate) {
        return json{{"object", refJson(candidate.object)}, {"score", candidate.score}, {"reason", candidate.reason}};
    };

    for (const auto &mismatch : report.mismatches) {
        json entry{{"schema_version", mismatch.schemaVersion},
                   {"severity", verificationSeverityName(mismatch.severity)},
                   {"category", mismatch.category},
                   {"message", mismatch.message},
                   {"schematic_object", optionalId(mismatch.schematicObject)},
                   {"layout_object", optionalId(mismatch.layoutObject)},
                   {"schematic_ref", optionalRefJson(mismatch.schematicRef)},
                   {"layout_ref", optionalRefJson(mismatch.layoutRef)},
                   {"hint", mismatch.hint},
                   {"candidates", json::array()}};
        for (const auto &candidate : mismatch.candidates)
            entry["candidates"].push_back(candidateJson(candidate));
        output["mismatches"].push_back(std::move(entry));
    }

    for (const auto &match : report.devices) {
        const char *status = "matched";
        if (!match.schematic && match.layout)
            status = "layout_only";
        else if (match.schematic && !match.layout)
            status = "schematic_only";
        else if (match.schematic && match.layout && !match.matched)
            status = "mismatched";
        json entry{{"status", status},
                   {"method", lvsMatchMethodName(match.method)},
                   {"matched", match.matched},
                   {"schematic", optionalRefJson(match.schematic)},
                   {"layout", optionalRefJson(match.layout)},
                   {"schematic_type", match.schematicType},
                   {"layout_type", match.layoutType},
                   {"parameters_compared", match.parametersCompared},
                   {"pins", json::array()},
                   {"parameters", json::array()}};
        for (const auto &pin : match.pins) {
            entry["pins"].push_back({{"schematic_pin", pin.schematicPin},
                                     {"layout_pin", pin.layoutPin},
                                     {"schematic_net", pin.schematicNet},
                                     {"layout_net", pin.layoutNet},
                                     {"matched", pin.matched},
                                     {"swapped", pin.swapped}});
        }
        for (const auto &parameter : match.parameters) {
            entry["parameters"].push_back({{"name", parameter.name},
                                           {"schematic", parameter.schematicValue},
                                           {"layout", parameter.layoutValue},
                                           {"matched", parameter.matched}});
        }
        output["devices"].push_back(std::move(entry));
    }

    for (const auto &match : report.nets) {
        output["nets"].push_back({{"matched", match.matched},
                                  {"ground", match.ground},
                                  {"schematic", optionalRefJson(match.schematic)},
                                  {"layout", optionalRefJson(match.layout)},
                                  {"schematic_connections", match.schematicConnections},
                                  {"layout_connections", match.layoutConnections}});
    }

    for (const auto &match : report.ports) {
        output["ports"].push_back({{"matched", match.matched},
                                   {"direction_matched", match.directionMatched},
                                   {"net_matched", match.netMatched},
                                   {"schematic", optionalRefJson(match.schematic)},
                                   {"layout", optionalRefJson(match.layout)},
                                   {"schematic_direction", match.schematicDirection},
                                   {"layout_direction", match.layoutDirection},
                                   {"schematic_net", match.schematicNet},
                                   {"layout_net", match.layoutNet}});
    }
    return output.dump(2) + "\n";
}

} // namespace ice
