#include "icelib/operations.hpp"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <map>
#include <set>
#include <sstream>
#include <string_view>

namespace ice {
namespace {
template <typename Map> auto &require(Map &map, ObjectId id, const char *code, const char *message) {
    auto it = map.find(id);
    if (it == map.end())
        throw ProjectError(code, message);
    return it->second;
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return text;
}

// Reads an instance parameter by one of the accepted names (case-insensitive),
// else the sole parameter, else empty. Passive/source symbols name their value
// by quantity (resistance, capacitance, ...), so callers pass a fallback list.
std::string parameterValue(const SchematicInstance &instance, std::initializer_list<std::string_view> wanted) {
    std::string fallback;
    for (const auto &parameter : instance.parameters) {
        const auto name = lower(parameter.name);
        for (const auto &candidate : wanted)
            if (name == lower(std::string(candidate)))
                return parameter.value;
        if (fallback.empty())
            fallback = parameter.value;
    }
    return instance.parameters.size() == 1 ? fallback : std::string{};
}

// Exact parameter lookup. Unlike parameterValue(), this never falls back to
// the sole parameter, which matters for multi-parameter waveform sources.
std::string parameterNamed(const SchematicInstance &instance, std::initializer_list<std::string_view> wanted) {
    for (const auto &parameter : instance.parameters) {
        const auto name = lower(parameter.name);
        for (const auto &candidate : wanted)
            if (name == lower(std::string(candidate)))
                return parameter.value;
    }
    return {};
}

std::string sourceParameter(const SchematicInstance &instance, std::initializer_list<std::string_view> names,
                            std::string_view fallback) {
    const auto value = parameterNamed(instance, names);
    return value.empty() ? std::string(fallback) : value;
}

std::string pulseExpression(const SchematicInstance &instance) {
    return "PULSE(" + sourceParameter(instance, {"initial_voltage", "v1"}, "0") + " " +
           sourceParameter(instance, {"pulsed_voltage", "v2"}, "1") + " " +
           sourceParameter(instance, {"delay_time", "td"}, "0") + " " +
           sourceParameter(instance, {"rise_time", "tr"}, "1u") + " " +
           sourceParameter(instance, {"fall_time", "tf"}, "1u") + " " +
           sourceParameter(instance, {"pulse_width", "pw"}, "0.5") + " " +
           sourceParameter(instance, {"period", "per"}, "1") + " " +
           sourceParameter(instance, {"pulse_count", "number_of_pulses", "cycles", "ncycles"}, "0") + ")";
}

std::string sineExpression(const SchematicInstance &instance) {
    return "SIN(" + sourceParameter(instance, {"offset_voltage", "vo"}, "0") + " " +
           sourceParameter(instance, {"amplitude", "va"}, "1") + " " +
           sourceParameter(instance, {"frequency", "freq"}, "1") + " " +
           sourceParameter(instance, {"delay_time", "td"}, "0") + " " +
           sourceParameter(instance, {"damping_factor", "theta"}, "0") + " " +
           sourceParameter(instance, {"phase", "phi"}, "0") + ")";
}

std::string sourceExpression(const SchematicInstance &instance, const std::string &symbolName, char element) {
    // A raw value parameter is an explicit SPICE expression and wins over the
    // structured waveform parameters.
    if (const auto raw = parameterNamed(instance, {"value"}); !raw.empty())
        return raw;
    const auto name = lower(symbolName);
    if (name.find("pulse") != std::string::npos)
        return pulseExpression(instance);
    if (name.find("sine") != std::string::npos || name.find("sinusoid") != std::string::npos)
        return sineExpression(instance);
    if (element == 'I')
        return parameterValue(instance, {"current", "value"});
    return parameterValue(instance, {"voltage", "value"});
}
} // namespace

char spiceElementForSymbol(const std::string &symbolName) {
    const auto name = lower(symbolName);
    if (name.find("resistor") != std::string::npos)
        return 'R';
    if (name.find("capacitor") != std::string::npos)
        return 'C';
    if (name.find("inductor") != std::string::npos)
        return 'L';
    if (name.find("diode") != std::string::npos)
        return 'D';
    // Check current before the generic source/prefix rules: "Current Source"
    // must not be mistaken for a capacitor because it starts with 'C'.
    if (name.find("current") != std::string::npos)
        return 'I';
    if (name.find("voltage") != std::string::npos || name.find("source") != std::string::npos)
        return 'V';
    if (name.find("ground") != std::string::npos)
        return 'G';
    // Legacy short designators such as "R1" or "V2". Requiring a digit after
    // the letter avoids classifying names like "IHP NMOS" as current sources.
    if (!name.empty()) {
        const auto prefix = name.front();
        const bool digitFollows = name.size() > 1 && std::isdigit(static_cast<unsigned char>(name[1]));
        if (name.size() == 1 || digitFollows) {
            switch (prefix) {
            case 'r':
                return 'R';
            case 'c':
                return 'C';
            case 'l':
                return 'L';
            case 'd':
                return 'D';
            case 'i':
                return 'I';
            case 'v':
                return 'V';
            case 'g':
                return 'G';
            default:
                break;
            }
        }
    }
    return 0;
}

ObjectId createSchematicCell(Project &project, std::string name, std::string pdk) {
    if (name.empty())
        throw ProjectError("schematic.cell", "Schematic cell name must not be empty");
    if (pdk.empty() || pdk.find_first_of("/\\") != std::string::npos)
        throw ProjectError("schematic.pdk", "Schematic PDK must be a valid PDK identifier");
    ObjectId id = project.nextId();
    project.schematicCells.emplace(id, SchematicCell{id, std::move(name), std::move(pdk)});
    return id;
}
ObjectId addSymbol(Project &project, ObjectId cellId, std::string name, std::string artwork, ObjectId sourceCell) {
    if (name.empty())
        throw ProjectError("schematic.symbol", "Symbol name must not be empty");
    auto &cell = require(project.schematicCells, cellId, "schematic.cell", "Schematic cell does not exist");
    ObjectId id = project.nextId();
    cell.symbols.emplace(id, Symbol{id, std::move(name), std::move(artwork), sourceCell});
    return id;
}
ObjectId addSchematicInstance(Project &project, ObjectId cellId, ObjectId symbolId, std::string name, Coordinate x,
                              Coordinate y) {
    if (name.empty())
        throw ProjectError("schematic.instance", "Instance name must not be empty");
    auto &cell = require(project.schematicCells, cellId, "schematic.cell", "Schematic cell does not exist");
    if (!cell.symbols.contains(symbolId))
        throw ProjectError("schematic.symbol", "Symbol does not exist");
    ObjectId id = project.nextId();
    cell.instances.emplace(id, SchematicInstance{id, symbolId, std::move(name), x, y});
    return id;
}
std::size_t seedDefaultParameters(Project &project, SchematicInstance &instance,
                                  const std::vector<Parameter> &defaults) {
    std::size_t added = 0;
    for (auto parameter : defaults) {
        const auto name = lower(parameter.name);
        const bool exists = std::any_of(instance.parameters.begin(), instance.parameters.end(),
                                        [&](const Parameter &existing) { return lower(existing.name) == name; });
        if (exists)
            continue;
        parameter.id = project.nextId();
        instance.parameters.push_back(std::move(parameter));
        ++added;
    }
    return added;
}
ObjectId createLayoutCell(Project &project, std::string name) {
    if (name.empty())
        throw ProjectError("layout.cell", "Layout cell name must not be empty");
    ObjectId id = project.nextId();
    project.layoutCells.emplace(id, LayoutCell{id, std::move(name)});
    return id;
}
ObjectId addLayer(Project &project, std::string name, int number) {
    if (name.empty())
        throw ProjectError("layout.layer", "Layer name must not be empty");
    if (number < 0)
        throw ProjectError("layout.layer", "Layer number must not be negative");
    ObjectId id = project.nextId();
    project.layers.emplace(id, Layer{id, std::move(name), number});
    return id;
}
ObjectId addRectangle(Project &project, ObjectId cellId, ObjectId layerId, Rect rectangle,
                      std::optional<ObjectId> net) {
    if (!rectangle.valid())
        throw ProjectError("layout.shape", "Rectangle must have positive dimensions");
    auto &cell = require(project.layoutCells, cellId, "layout.cell", "Layout cell does not exist");
    if (!project.layers.contains(layerId))
        throw ProjectError("layout.layer", "Layer does not exist");
    ObjectId id = project.nextId();
    cell.shapes.emplace(id, Shape{id, layerId, net, rectangle});
    return id;
}
void moveRectangle(Project &project, ObjectId cellId, ObjectId shapeId, Coordinate dx, Coordinate dy) {
    auto &cell = require(project.layoutCells, cellId, "layout.cell", "Layout cell does not exist");
    auto &shape = require(cell.shapes, shapeId, "layout.shape", "Shape does not exist");
    shape.rectangle.x += dx;
    shape.rectangle.y += dy;
    for (auto &point : shape.polygon) {
        point.x += dx;
        point.y += dy;
    }
}

std::vector<std::string> buildSchematicNetlist(const Project &project, ObjectId cellId,
                                               std::vector<std::string> *notes) {
    const auto &cell = require(project.schematicCells, cellId, "schematic.cell", "Schematic cell does not exist");
    auto note = [notes](std::string message) {
        if (notes)
            notes->push_back(std::move(message));
    };

    // A symbol is reusable, so placing it twice duplicates its pin ids. Detect
    // symbols used by more than one instance; their pins cannot be resolved to
    // a unique instance and are skipped below.
    std::map<ObjectId, int> symbolUseCount;
    for (const auto &[_, instance] : cell.instances)
        ++symbolUseCount[instance.symbol];

    // For every net, map each member pin id to the unique (instance, symbol,
    // pin) that owns it, and derive the SPICE node name.
    std::ostringstream out;
    out << "* ICE schematic netlist: " << cell.name << "\n";

    // Resolve a pin id to its owning instance, or null when ambiguous/unknown.
    auto resolvePin = [&](ObjectId pinId, const SchematicInstance **owner, const Pin **pin) -> bool {
        const SchematicInstance *found = nullptr;
        const Pin *foundPin = nullptr;
        for (const auto &[_, instance] : cell.instances) {
            const auto symbol = cell.symbols.find(instance.symbol);
            if (symbol == cell.symbols.end())
                continue;
            for (const auto &candidate : symbol->second.pins) {
                if (candidate.id != pinId)
                    continue;
                if (symbolUseCount[instance.symbol] > 1)
                    return false; // ambiguous: shared symbol
                found = &instance;
                foundPin = &candidate;
            }
        }
        if (!found)
            return false;
        *owner = found;
        *pin = foundPin;
        return true;
    };

    // Node name for a net: ground nets collapse to 0; otherwise use the net
    // name when set, else a stable n<id>.
    auto nodeFor = [&](const Net &net) -> std::string {
        for (const auto &pinId : net.pins) {
            const SchematicInstance *owner = nullptr;
            const Pin *pin = nullptr;
            if (resolvePin(pinId, &owner, &pin)) {
                const auto symbol = cell.symbols.find(owner->symbol);
                if (symbol != cell.symbols.end() && spiceElementForSymbol(symbol->second.name) == 'G')
                    return "0";
            }
        }
        if (!net.name.empty())
            return net.name;
        return "n" + std::to_string(net.id);
    };

    // Emit one SPICE element per instance, wiring each pin to its net's node.
    for (const auto &[instanceId, instance] : cell.instances) {
        const auto symbol = cell.symbols.find(instance.symbol);
        if (symbol == cell.symbols.end())
            continue;
        const char element = spiceElementForSymbol(symbol->second.name);
        if (element == 'G')
            continue; // ground contributes the reference node only
        if (element == 0) {
            note("unsupported symbol skipped: " + symbol->second.name + " (" + instance.name + ")");
            out << "* unsupported: " << instance.name << " " << symbol->second.name << "\n";
            continue;
        }
        if (symbolUseCount[instance.symbol] > 1) {
            note("symbol instantiated more than once, cannot resolve pins: " + symbol->second.name);
            continue;
        }

        // Map this instance's pins to node names via the nets they belong to.
        std::map<ObjectId, std::string> pinToNode;
        for (const auto &[_, net] : cell.nets) {
            const auto node = nodeFor(net);
            for (const auto &pinId : net.pins) {
                const SchematicInstance *owner = nullptr;
                const Pin *pin = nullptr;
                if (resolvePin(pinId, &owner, &pin) && owner->id == instanceId)
                    pinToNode[pinId] = node;
            }
        }

        // Ordered node list follows the symbol's pin order.
        std::vector<std::string> nodes;
        for (const auto &pin : symbol->second.pins) {
            const auto node = pinToNode.find(pin.id);
            nodes.push_back(node != pinToNode.end() ? node->second : "0");
        }
        if (nodes.empty()) {
            note("instance has no connected pins: " + instance.name);
            continue;
        }

        const std::string value = [&] {
            switch (element) {
            case 'R':
                return parameterValue(instance, {"resistance", "value"});
            case 'C':
                return parameterValue(instance, {"capacitance", "value"});
            case 'L':
                return parameterValue(instance, {"inductance", "value"});
            case 'V':
            case 'I':
                return sourceExpression(instance, symbol->second.name, element);
            default:
                return parameterValue(instance, {"value"});
            }
        }();
        std::string resolved = value;
        if (resolved.empty()) {
            resolved = element == 'R'   ? "1k"
                       : element == 'C' ? "1n"
                       : element == 'L' ? "1u"
                       : element == 'D' ? "Dmodel"
                       : element == 'I' ? "1m"
                                        : "1";
            note("missing value parameter for " + instance.name + ", using " + resolved);
        }

        // Instance names commonly already carry the element letter (V1, R1);
        // only prepend it when they do not, so we never emit "VV1".
        std::string designator = instance.name;
        const auto prefix = static_cast<char>(std::toupper(static_cast<unsigned char>(element)));
        if (designator.empty() ||
            static_cast<char>(std::toupper(static_cast<unsigned char>(designator.front()))) != prefix)
            designator.insert(designator.begin(), prefix);
        out << designator << " ";
        if (element == 'D')
            out << resolved << " "; // diode: D<name> <node+> <node-> <model>
        for (const auto &node : nodes)
            out << node << " ";
        if (element != 'D')
            out << resolved << " ";
        out << "\n";
    }

    // Split into lines so callers can hand it to the simulator request.
    std::vector<std::string> lines;
    std::istringstream stream(out.str());
    std::string line;
    while (std::getline(stream, line))
        if (!line.empty())
            lines.push_back(line);
    return lines;
}
} // namespace ice
