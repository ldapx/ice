#include "icelib/operations.hpp"

#include "icelib/hdl.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <initializer_list>
#include <limits>
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

std::string trimLine(const std::string &text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

bool isOwnedAnalysisDirective(const std::string &line) {
    const auto normalized = lower(line);
    const auto isDirective = [&](const std::string &name) {
        return normalized == name || (normalized.starts_with(name) && normalized.size() > name.size() &&
                                      std::isspace(static_cast<unsigned char>(normalized[name.size()])));
    };
    if (normalized == ".end")
        return true;
    if (normalized.size() > 4 && normalized.starts_with(".end") &&
        (std::isspace(static_cast<unsigned char>(normalized[4])) || normalized[4] == ';' || normalized[4] == '*'))
        return true;
    return isDirective(".tran") || isDirective(".dc") || isDirective(".ac") || isDirective(".op");
}

bool schematicNameTaken(const Project &project, const std::string &name) {
    return std::any_of(project.schematicCells.begin(), project.schematicCells.end(),
                       [&](const auto &entry) { return entry.second.name == name; });
}

bool layoutNameTaken(const Project &project, const std::string &name) {
    const auto layoutCell = std::any_of(project.layoutCells.begin(), project.layoutCells.end(),
                                        [&](const auto &entry) { return entry.second.name == name; });
    if (layoutCell)
        return true;
    return std::any_of(project.proceduralLayouts.begin(), project.proceduralLayouts.end(),
                       [&](const auto &entry) { return entry.second.name == name; });
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
    if (name.find("nmos") != std::string::npos || name.find("pmos") != std::string::npos ||
        name.find("mosfet") != std::string::npos)
        return 'M';
    if (name.find("npn") != std::string::npos || name.find("pnp") != std::string::npos ||
        name.find("bjt") != std::string::npos)
        return 'Q';
    if (name.find("op amp") != std::string::npos || name.find("opamp") != std::string::npos ||
        name.find("operational amplifier") != std::string::npos)
        return 'X';
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
            case 'm':
                return 'M';
            case 'q':
                return 'Q';
            case 'x':
                return 'X';
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
    if (schematicNameTaken(project, name))
        throw ProjectError("schematic.name", "A schematic named '" + name + "' already exists");
    if (pdk.empty() || pdk.find_first_of("/\\") != std::string::npos)
        throw ProjectError("schematic.pdk", "Schematic PDK must be a valid PDK identifier");
    ObjectId id = project.nextId();
    project.schematicCells.emplace(id, SchematicCell{id, std::move(name), std::move(pdk)});
    return id;
}
ObjectId createTextSchematicCell(Project &project, std::string name, std::string pdk, std::string spiceText) {
    const auto id = createSchematicCell(project, std::move(name), std::move(pdk));
    auto &cell = project.schematicCells.at(id);
    cell.textSchematic = true;
    cell.spiceText = std::move(spiceText);
    return id;
}
ObjectId createSystemVerilogCell(Project &project, std::string name, std::string pdk, std::string source) {
    const auto id = createSchematicCell(project, std::move(name), std::move(pdk));
    auto &cell = project.schematicCells.at(id);
    cell.systemVerilogSchematic = true;
    cell.systemVerilogText = std::move(source);
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
void setSchematicParameter(Project &project, ObjectId cellId, ObjectId instanceId, std::string name,
                           std::string value) {
    if (name.empty())
        throw ProjectError("schematic.parameter.name", "Parameter name must not be empty");
    auto &cell = require(project.schematicCells, cellId, "schematic.cell", "Schematic cell does not exist");
    auto instance = cell.instances.find(instanceId);
    if (instance == cell.instances.end())
        throw ProjectError("schematic.parameter.instance", "Instance does not exist");
    for (auto &parameter : instance->second.parameters) {
        if (lower(parameter.name) == lower(name)) {
            parameter.value = std::move(value);
            return;
        }
    }
    instance->second.parameters.push_back({project.nextId(), std::move(name), std::move(value)});
}

ObjectId addSystemVerilogInstance(Project &project, ObjectId parentCellId, ObjectId sourceCellId,
                                  std::string instanceName, Coordinate x, Coordinate y, ObjectId *symbolId,
                                  bool flattenBuses) {
    if (instanceName.empty())
        throw ProjectError("schematic.instance", "Instance name must not be empty");
    auto &parent = require(project.schematicCells, parentCellId, "schematic.cell", "Schematic cell does not exist");
    if (parent.isTextSource())
        throw ProjectError("schematic.hdl.parent", "A SystemVerilog block can only be placed in a drawn schematic");
    const auto &source = require(project.schematicCells, sourceCellId, "schematic.hdl.source",
                                 "SystemVerilog source cell does not exist");
    if (!source.systemVerilogSchematic)
        throw ProjectError("schematic.hdl.source", "Source cell is not a SystemVerilog cell");
    const auto parsed = parseSystemVerilogModule(source);
    if (!parsed.ok)
        throw ProjectError("schematic.hdl.parse", parsed.error);
    const auto tmpl = makeHdlSymbolTemplate(parsed.module, flattenBuses);
    if (tmpl.pins.empty())
        throw ProjectError("schematic.hdl.ports", "SystemVerilog block has no ports");

    auto next = project.nextId();
    const auto symbolIdValue = next++;
    std::string symbolName = source.name + " HDL";
    if (std::any_of(parent.symbols.begin(), parent.symbols.end(),
                    [&](const auto &entry) { return entry.second.name == symbolName; })) {
        const auto base = symbolName;
        for (int suffix = 2;; ++suffix) {
            symbolName = base + " " + std::to_string(suffix);
            if (std::none_of(parent.symbols.begin(), parent.symbols.end(),
                             [&](const auto &entry) { return entry.second.name == symbolName; }))
                break;
        }
    }
    Symbol symbol;
    symbol.id = symbolIdValue;
    symbol.name = symbolName;
    symbol.artwork = tmpl.artwork;
    symbol.sourceCell = sourceCellId;
    symbol.label = SymbolLabel{0, tmpl.labelY};
    for (auto pin : tmpl.pins) {
        pin.id = next++;
        symbol.pins.push_back(std::move(pin));
    }
    parent.symbols.emplace(symbolIdValue, std::move(symbol));

    const auto instanceId = next++;
    parent.instances.emplace(instanceId, SchematicInstance{instanceId, symbolIdValue, std::move(instanceName), x, y});
    if (symbolId)
        *symbolId = symbolIdValue;
    return instanceId;
}

std::string schematicNetNodeName(const SchematicCell &cell, const Net &net) {
    for (const auto pinId : net.pins) {
        for (const auto &[_, instance] : cell.instances) {
            const auto symbol = cell.symbols.find(instance.symbol);
            if (symbol == cell.symbols.end())
                continue;
            const bool ownsPin = std::any_of(symbol->second.pins.begin(), symbol->second.pins.end(),
                                             [&](const Pin &pin) { return pin.id == pinId; });
            if (ownsPin && spiceElementForSymbol(symbol->second.name) == 'G')
                return "0";
        }
    }
    if (!net.name.empty())
        return net.name;
    return "n" + std::to_string(net.id);
}

ObjectId addNet(Project &project, ObjectId cellId, std::string name, std::vector<ObjectId> pins, Coordinate x,
                Coordinate y) {
    if (pins.size() < 2)
        throw ProjectError("schematic.net", "A net must connect at least two pins");
    auto &cell = require(project.schematicCells, cellId, "schematic.cell", "Schematic cell does not exist");
    for (const auto pinId : pins) {
        const bool found = std::any_of(cell.symbols.begin(), cell.symbols.end(), [&](const auto &symbol) {
            return std::any_of(symbol.second.pins.begin(), symbol.second.pins.end(),
                               [&](const auto &pin) { return pin.id == pinId; });
        });
        if (!found)
            throw ProjectError("schematic.net.pin", "Net references a missing pin");
    }
    ObjectId id = project.nextId();
    if (name.empty())
        name = "N" + std::to_string(id);
    cell.nets.emplace(id, Net{id, std::move(name), std::move(pins), x, y});
    return id;
}

ObjectId addSchematicComment(Project &project, ObjectId cellId, std::string text, Coordinate x, Coordinate y) {
    auto &cell = require(project.schematicCells, cellId, "schematic.cell", "Schematic cell does not exist");
    const auto id = project.nextId();
    cell.comments.emplace(id, SchematicComment{id, std::move(text), x, y});
    return id;
}

void removeSchematicComment(Project &project, ObjectId cellId, ObjectId commentId) {
    auto &cell = require(project.schematicCells, cellId, "schematic.cell", "Schematic cell does not exist");
    cell.comments.erase(commentId);
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
ObjectId createLayoutCell(Project &project, std::string name, std::string pdk, std::string stack) {
    if (name.empty())
        throw ProjectError("layout.cell", "Layout cell name must not be empty");
    if (layoutNameTaken(project, name))
        throw ProjectError("layout.name", "A layout named '" + name + "' already exists");
    if (pdk.empty() || pdk.find_first_of("/\\") != std::string::npos)
        throw ProjectError("layout.pdk", "Layout PDK must be a valid PDK identifier");
    if (stack.find_first_of("/\\") != std::string::npos)
        throw ProjectError("layout.stack", "Layout stack must be a valid stack identifier");
    ObjectId id = project.nextId();
    project.layoutCells.emplace(id, LayoutCell{id, std::move(name), std::move(pdk), std::move(stack)});
    return id;
}

ObjectId createProceduralLayout(Project &project, std::string name, std::string pdk, std::string script,
                                std::string stack) {
    if (name.empty())
        throw ProjectError("layout.procedural.name", "Procedural layout name must not be empty");
    if (pdk.empty() || pdk.find_first_of("/\\") != std::string::npos)
        throw ProjectError("layout.pdk", "Layout PDK must be a valid PDK identifier");
    if (stack.find_first_of("/\\") != std::string::npos)
        throw ProjectError("layout.stack", "Layout stack must be a valid stack identifier");

    const auto nameTaken = [&](const std::string &candidate) { return layoutNameTaken(project, candidate); };
    if (nameTaken(name))
        throw ProjectError("layout.name", "A layout named '" + name + "' already exists");

    auto generatedName = name + " (generated)";
    if (nameTaken(generatedName)) {
        const auto stem = generatedName;
        generatedName = stem + " 2";
        for (int index = 3; nameTaken(generatedName); ++index)
            generatedName = stem + " " + std::to_string(index);
    }

    const auto generatedCell = createLayoutCell(project, generatedName, pdk, stack);
    const auto id = project.nextId();
    project.layoutCells.at(generatedCell).generatedBy = id;
    project.proceduralLayouts.emplace(
        id, ProceduralLayout{id, std::move(name), std::move(pdk), std::move(stack), std::move(script), generatedCell});
    return id;
}

ObjectId createLayoutFromSchematic(Project &project, ObjectId schematicId, std::string name, std::string stack) {
    const auto &schematic =
        require(project.schematicCells, schematicId, "schematic.cell", "Schematic cell does not exist");

    std::string layoutName = name.empty() ? schematic.name : name;
    if (layoutName.empty())
        layoutName = "Layout";
    const auto nameTaken = [&](const std::string &candidate) { return layoutNameTaken(project, candidate); };
    if (nameTaken(layoutName)) {
        const auto stem = layoutName + " Layout";
        layoutName = stem;
        for (int index = 2; nameTaken(layoutName); ++index)
            layoutName = stem + " " + std::to_string(index);
    }

    const auto pdk = schematic.pdk.empty() ? std::string{genericPdkId} : schematic.pdk;
    const auto layoutId = createLayoutCell(project, layoutName, pdk, std::move(stack));
    auto &layout = project.layoutCells.at(layoutId);

    std::vector<const SchematicInstance *> orderedInstances;
    orderedInstances.reserve(schematic.instances.size());
    for (const auto &[_, instance] : schematic.instances)
        orderedInstances.push_back(&instance);
    std::sort(orderedInstances.begin(), orderedInstances.end(), [](const auto *left, const auto *right) {
        if (left->y != right->y)
            return left->y > right->y;
        if (left->x != right->x)
            return left->x < right->x;
        if (left->name != right->name)
            return left->name < right->name;
        return left->id < right->id;
    });

    // Keep planned-device boxes on the project grid even for coarse grids.
    const auto gridStep = [&](Coordinate value) { return std::max(project.grid, project.snap(value)); };
    const auto boxWidth = gridStep(1200);
    const auto boxHeight = gridStep(700);

    // Schematic instances are positioned in symbol-canvas units. Map those
    // coordinates into layout nanometers so the planned boxes keep the
    // relative arrangement they had in the schematic.
    constexpr Coordinate schematicToLayoutScale = 10;
    struct PlannedPlacement {
        Coordinate x = 0;
        Coordinate y = 0;
    };
    std::vector<PlannedPlacement> placements(orderedInstances.size());
    Coordinate minimumX = 0;
    Coordinate minimumY = 0;
    bool havePlacement = false;
    for (std::size_t index = 0; index < orderedInstances.size(); ++index) {
        const auto *instance = orderedInstances[index];
        const auto x = project.snap(instance->x * schematicToLayoutScale - boxWidth / 2);
        const auto y = project.snap(instance->y * schematicToLayoutScale - boxHeight / 2);
        placements[index] = {x, y};
        if (!havePlacement) {
            minimumX = x;
            minimumY = y;
            havePlacement = true;
        } else {
            minimumX = std::min(minimumX, x);
            minimumY = std::min(minimumY, y);
        }
    }
    const auto offsetX = havePlacement ? -minimumX : 0;
    const auto offsetY = havePlacement ? -minimumY : 0;

    const auto count = static_cast<int>(orderedInstances.size());

    std::map<ObjectId, std::vector<ObjectId>> pinDevices;
    std::map<ObjectId, std::string> pinNames;
    for (int index = 0; index < count; ++index) {
        const auto *instance = orderedInstances[static_cast<std::size_t>(index)];
        std::string deviceName = instance->name;
        const auto symbol = schematic.symbols.find(instance->symbol);
        if (deviceName.empty() && symbol != schematic.symbols.end())
            deviceName = symbol->second.name;
        if (deviceName.empty())
            deviceName = "Device " + std::to_string(index + 1);

        const auto deviceId = project.nextId();
        const auto &placement = placements[static_cast<std::size_t>(index)];
        layout.plannedDevices.emplace(deviceId,
                                      PlannedDevice{deviceId, std::move(deviceName), instance->id,
                                                    placement.x + offsetX, placement.y + offsetY, boxWidth, boxHeight});
        if (symbol == schematic.symbols.end())
            continue;
        for (const auto &pin : symbol->second.pins) {
            pinDevices[pin.id].push_back(deviceId);
            pinNames[pin.id] = pin.name;
        }
    }

    struct NetEndpoint {
        ObjectId device = 0;
        std::string pin;
    };

    std::set<std::string> layoutNetNames;
    for (const auto &[_, net] : schematic.nets) {
        std::set<std::pair<ObjectId, ObjectId>> connectedPairs;
        std::vector<NetEndpoint> endpoints;
        for (const auto pinId : net.pins) {
            const auto entry = pinDevices.find(pinId);
            if (entry == pinDevices.end())
                continue;
            const auto pinName = pinNames.find(pinId);
            for (const auto deviceId : entry->second) {
                const bool exists = std::any_of(endpoints.begin(), endpoints.end(),
                                                [&](const auto &endpoint) { return endpoint.device == deviceId; });
                if (!exists)
                    endpoints.push_back({deviceId, pinName == pinNames.end() ? std::string{} : pinName->second});
            }
        }
        if (endpoints.size() < 2)
            continue;
        auto netName = net.name.empty() ? "N" + std::to_string(net.id) : net.name;
        if (!layoutNetNames.insert(lower(netName)).second)
            netName += "_" + std::to_string(net.id);
        layoutNetNames.insert(lower(netName));
        const auto layoutNetId = project.nextId();
        layout.nets.emplace(layoutNetId, LayoutNet{layoutNetId, netName});
        const auto &from = endpoints.front();
        for (std::size_t index = 1; index < endpoints.size(); ++index) {
            const auto &to = endpoints[index];
            if (from.device == to.device)
                continue;
            const auto pair = from.device < to.device ? std::make_pair(from.device, to.device)
                                                      : std::make_pair(to.device, from.device);
            if (!connectedPairs.insert(pair).second)
                continue;
            const auto connectionId = project.nextId();
            PlannedConnection connection{connectionId, from.device, to.device, layoutNetId};
            connection.fromPin = from.pin;
            connection.toPin = to.pin;
            layout.plannedConnections.emplace(connectionId, std::move(connection));
        }
    }
    return layoutId;
}
std::string nextShapeName(const LayoutCell &cell, const std::string &type) {
    std::set<std::string> names;
    for (const auto &[_, shape] : cell.shapes)
        if (!shape.name.empty())
            names.insert(shape.name);
    for (int index = 1;; ++index) {
        auto name = type + " " + std::to_string(index);
        if (!names.contains(name))
            return name;
    }
}

ObjectId addLayer(Project &project, std::string name, int number, ObjectId layout, std::string pdk, int datatype,
                  std::string stack) {
    if (name.empty())
        throw ProjectError("layout.layer", "Layer name must not be empty");
    if (number < 0)
        throw ProjectError("layout.layer", "Layer number must not be negative");
    if (datatype < 0 || datatype > 65535)
        throw ProjectError("layout.layer", "Layer datatype out of GDSII range");
    if (stack.find_first_of("/\\") != std::string::npos)
        throw ProjectError("layout.stack", "Layer stack must be a valid stack identifier");
    ObjectId id = project.nextId();
    Layer layer{id, std::move(name), number, true, layout, std::move(pdk), datatype};
    layer.stack = std::move(stack);
    project.layers.emplace(id, std::move(layer));
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
    Shape shape{id, layerId, net, rectangle};
    shape.name = nextShapeName(cell, "Rectangle");
    cell.shapes.emplace(id, std::move(shape));
    return id;
}
ObjectId addText(Project &project, ObjectId cellId, ObjectId layerId, Coordinate x, Coordinate y, std::string text,
                 Coordinate height, std::optional<ObjectId> net) {
    if (text.empty())
        throw ProjectError("layout.text", "Text must not be empty");
    if (height <= 0)
        throw ProjectError("layout.text", "Text height must be positive");
    auto &cell = require(project.layoutCells, cellId, "layout.cell", "Layout cell does not exist");
    if (!project.layers.contains(layerId))
        throw ProjectError("layout.layer", "Layer does not exist");
    const auto grid = std::max<Coordinate>(1, project.grid);
    const auto snappedHeight = ((height + grid - 1) / grid) * grid;
    const auto estimatedWidth = static_cast<Coordinate>(
        std::llround(static_cast<double>(text.size()) * static_cast<double>(snappedHeight) * 0.6));
    const auto width = std::max(grid, ((estimatedWidth + grid - 1) / grid) * grid);
    const auto id = project.nextId();
    Shape shape{id, layerId, net, {x, y, width, snappedHeight}, {}};
    shape.text = std::move(text);
    shape.name = nextShapeName(cell, "Text");
    cell.shapes.emplace(id, std::move(shape));
    return id;
}

ObjectId addPolygon(Project &project, ObjectId cellId, ObjectId layerId, const std::vector<LayoutPoint> &points,
                    std::optional<ObjectId> net) {
    if (points.size() < 3)
        throw ProjectError("layout.polygon", "Polygon must contain at least three points");
    auto &cell = require(project.layoutCells, cellId, "layout.cell", "Layout cell does not exist");
    if (!project.layers.contains(layerId))
        throw ProjectError("layout.layer", "Layer does not exist");
    Coordinate minX = std::numeric_limits<Coordinate>::max();
    Coordinate minY = std::numeric_limits<Coordinate>::max();
    Coordinate maxX = std::numeric_limits<Coordinate>::lowest();
    Coordinate maxY = std::numeric_limits<Coordinate>::lowest();
    for (const auto &point : points) {
        minX = std::min(minX, point.x);
        minY = std::min(minY, point.y);
        maxX = std::max(maxX, point.x);
        maxY = std::max(maxY, point.y);
    }
    if (maxX <= minX || maxY <= minY)
        throw ProjectError("layout.polygon", "Polygon must have a positive bounding box");
    ObjectId id = project.nextId();
    Shape shape{id, layerId, net, Rect{minX, minY, maxX - minX, maxY - minY}, points};
    shape.name = nextShapeName(cell, "Polygon");
    cell.shapes.emplace(id, std::move(shape));
    return id;
}
namespace {
std::string normalizedLayoutName(std::string value) { return lower(std::move(value)); }

void validatePortForCell(const Project &project, const LayoutCell &cell, const Port &port, ObjectId ignorePort) {
    if (port.name.empty())
        throw ProjectError("layout.port", "Port name must not be empty");
    if (port.label.empty())
        throw ProjectError("layout.port.label", "Port label must not be empty");
    if (!port.rectangle.valid())
        throw ProjectError("layout.port", "Port rectangle must have positive dimensions");
    if (!project.layers.contains(port.layer))
        throw ProjectError("layout.layer", "Layer does not exist");
    if (port.net != 0 && !cell.nets.contains(port.net))
        throw ProjectError("layout.port.net", "Port references a missing layout net");
    const auto wanted = normalizedLayoutName(port.name);
    for (const auto &[id, existing] : cell.ports) {
        if (id != ignorePort && id != port.id && normalizedLayoutName(existing.name) == wanted)
            throw ProjectError("layout.port.name", "A port named '" + port.name + "' already exists");
    }
    if (port.sourceInstance != 0 || port.sourcePort != 0) {
        const auto instance = cell.instances.find(port.sourceInstance);
        if (instance == cell.instances.end())
            throw ProjectError("layout.port.source_instance", "Promoted port references a missing layout instance");
        const auto child = project.layoutCells.find(instance->second.cell);
        if (child == project.layoutCells.end() || !child->second.ports.contains(port.sourcePort))
            throw ProjectError("layout.port.source_port", "Promoted port references a missing child port");
    }
}
} // namespace

ObjectId addPort(Project &project, ObjectId cellId, std::string name, ObjectId layerId, Rect rectangle,
                 PortDirection direction, PortSide side, ObjectId net, std::string label) {
    auto &cell = require(project.layoutCells, cellId, "layout.cell", "Layout cell does not exist");
    Port port;
    port.id = project.nextId();
    port.name = std::move(name);
    port.layer = layerId;
    port.rectangle = rectangle;
    port.direction = direction;
    port.side = side;
    port.net = net;
    port.label = label.empty() ? port.name : std::move(label);
    validatePortForCell(project, cell, port, port.id);
    const auto id = port.id;
    cell.ports.emplace(id, std::move(port));
    return id;
}

bool updatePort(Project &project, ObjectId cellId, const Port &port) {
    auto &cell = require(project.layoutCells, cellId, "layout.cell", "Layout cell does not exist");
    if (!cell.ports.contains(port.id))
        return false;
    validatePortForCell(project, cell, port, port.id);
    cell.ports.at(port.id) = port;
    return true;
}

void movePort(Project &project, ObjectId cellId, ObjectId portId, Coordinate dx, Coordinate dy) {
    auto &cell = require(project.layoutCells, cellId, "layout.cell", "Layout cell does not exist");
    auto &port = require(cell.ports, portId, "layout.port", "Port does not exist");
    port.rectangle.x += dx;
    port.rectangle.y += dy;
}

void resizePort(Project &project, ObjectId cellId, ObjectId portId, Rect rectangle) {
    auto &cell = require(project.layoutCells, cellId, "layout.cell", "Layout cell does not exist");
    auto &port = require(cell.ports, portId, "layout.port", "Port does not exist");
    if (!rectangle.valid())
        throw ProjectError("layout.port", "Port rectangle must have positive dimensions");
    port.rectangle = rectangle;
}

void removePort(Project &project, ObjectId cellId, ObjectId portId) {
    auto &cell = require(project.layoutCells, cellId, "layout.cell", "Layout cell does not exist");
    if (!cell.ports.contains(portId))
        throw ProjectError("layout.port", "Port does not exist");
    for (const auto &[otherId, other] : project.layoutCells) {
        if (otherId == cellId)
            continue;
        for (const auto &[_, port] : other.ports) {
            if (port.sourceInstance == 0)
                continue;
            const auto instance = other.instances.find(port.sourceInstance);
            if (instance != other.instances.end() && instance->second.cell == cellId && port.sourcePort == portId)
                throw ProjectError("layout.port.referenced", "Port is promoted by another layout cell");
        }
    }
    std::erase_if(cell.plannedConnections,
                  [&](const auto &entry) { return entry.second.from == portId || entry.second.to == portId; });
    cell.ports.erase(portId);
}

ObjectId createLayoutNet(Project &project, ObjectId cellId, std::string name) {
    if (name.empty())
        throw ProjectError("layout.net", "Layout net name must not be empty");
    auto &cell = require(project.layoutCells, cellId, "layout.cell", "Layout cell does not exist");
    const auto wanted = normalizedLayoutName(name);
    for (const auto &[_, net] : cell.nets) {
        if (normalizedLayoutName(net.name) == wanted)
            throw ProjectError("layout.net.name", "A layout net named '" + name + "' already exists");
    }
    const auto id = project.nextId();
    cell.nets.emplace(id, LayoutNet{id, std::move(name)});
    return id;
}

void assignPortNet(Project &project, ObjectId cellId, ObjectId portId, ObjectId net) {
    auto &cell = require(project.layoutCells, cellId, "layout.cell", "Layout cell does not exist");
    auto &port = require(cell.ports, portId, "layout.port", "Port does not exist");
    if (net != 0 && !cell.nets.contains(net))
        throw ProjectError("layout.port.net", "Port references a missing layout net");
    port.net = net;
}
ObjectId addLayoutInstance(Project &project, ObjectId parentCellId, ObjectId childCellId, std::string name,
                           Coordinate x, Coordinate y, Orientation orientation) {
    if (name.empty())
        throw ProjectError("layout.instance", "Instance name must not be empty");
    if (parentCellId == childCellId)
        throw ProjectError("layout.instance", "A layout cell cannot instance itself");
    auto &parent = require(project.layoutCells, parentCellId, "layout.cell", "Parent layout cell does not exist");
    const auto &child = require(project.layoutCells, childCellId, "layout.cell", "Child layout cell does not exist");
    if (!parent.pdk.empty() && !child.pdk.empty() && parent.pdk != child.pdk)
        throw ProjectError("layout.instance.technology", "Parent and child layouts must use the same PDK");
    if (parent.stack != child.stack)
        throw ProjectError("layout.instance.stack", "Parent and child layouts must use the same PDK stack");
    ObjectId id = project.nextId();
    parent.instances.emplace(id, LayoutInstance{id, childCellId, std::move(name), x, y, orientation});
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
    for (auto &point : shape.centerline) {
        point.x += dx;
        point.y += dy;
    }
}

void resizeRectangle(Project &project, ObjectId cellId, ObjectId shapeId, Rect rectangle) {
    if (!rectangle.valid())
        throw ProjectError("layout.shape", "Rectangle must have positive dimensions");
    auto &cell = require(project.layoutCells, cellId, "layout.cell", "Layout cell does not exist");
    auto &shape = require(cell.shapes, shapeId, "layout.shape", "Shape does not exist");
    if (!shape.polygon.empty())
        throw ProjectError("layout.shape", "Only rectangular shapes can be resized");
    shape.rectangle = rectangle;
}

void movePolygonPoint(Project &project, ObjectId cellId, ObjectId shapeId, std::size_t index, LayoutPoint point) {
    auto &cell = require(project.layoutCells, cellId, "layout.cell", "Layout cell does not exist");
    auto &shape = require(cell.shapes, shapeId, "layout.shape", "Shape does not exist");
    if (shape.isPath() || shape.polygon.size() < 3)
        throw ProjectError("layout.shape", "Only polygon shapes can have points edited");
    if (index >= shape.polygon.size())
        throw ProjectError("layout.shape", "Polygon point index is out of range");
    shape.polygon[index] = point;
    Coordinate minX = std::numeric_limits<Coordinate>::max();
    Coordinate minY = std::numeric_limits<Coordinate>::max();
    Coordinate maxX = std::numeric_limits<Coordinate>::lowest();
    Coordinate maxY = std::numeric_limits<Coordinate>::lowest();
    for (const auto &polygonPoint : shape.polygon) {
        minX = std::min(minX, polygonPoint.x);
        minY = std::min(minY, polygonPoint.y);
        maxX = std::max(maxX, polygonPoint.x);
        maxY = std::max(maxY, polygonPoint.y);
    }
    shape.rectangle = {minX, minY, std::max<Coordinate>(1, maxX - minX), std::max<Coordinate>(1, maxY - minY)};
}

std::vector<ObjectId> booleanLayoutShapes(Project &project, ObjectId cellId, const std::vector<ObjectId> &shapeIds,
                                          BooleanOperation operation) {
    auto &cell = require(project.layoutCells, cellId, "layout.cell", "Layout cell does not exist");
    if (shapeIds.size() < 2)
        throw ProjectError("layout.boolean", "Select at least two shapes for a boolean operation");

    std::vector<Shape *> operands;
    ObjectId layer = 0;
    std::optional<ObjectId> net;
    bool firstNet = true;
    std::set<ObjectId> seen;
    for (const auto id : shapeIds) {
        if (!seen.insert(id).second)
            continue;
        auto shapeIt = cell.shapes.find(id);
        if (shapeIt == cell.shapes.end())
            throw ProjectError("layout.shape", "Shape does not exist");
        auto &shape = shapeIt->second;
        if (shape.isPath() || shape.isText())
            throw ProjectError("layout.boolean", "Boolean operations support rectangles and polygons only");
        if (shape.polygon.empty() && !shape.rectangle.valid())
            throw ProjectError("layout.shape", "Shape does not contain a valid filled outline");
        if (layer != 0 && layer != shape.layer)
            throw ProjectError("layout.boolean", "Select shapes on the same layer");
        layer = shape.layer;
        if (firstNet) {
            net = shape.net;
            firstNet = false;
        } else if (net != shape.net) {
            net.reset();
        }
        operands.push_back(&shape);
    }
    if (operands.size() < 2)
        throw ProjectError("layout.boolean", "Select at least two shapes for a boolean operation");

    const auto outline = [](const Shape &shape) {
        if (shape.polygon.size() >= 3)
            return shape.polygon;
        return std::vector<LayoutPoint>{
            {shape.rectangle.x, shape.rectangle.y},
            {shape.rectangle.x + shape.rectangle.width, shape.rectangle.y},
            {shape.rectangle.x + shape.rectangle.width, shape.rectangle.y + shape.rectangle.height},
            {shape.rectangle.x, shape.rectangle.y + shape.rectangle.height}};
    };
    const auto toPolygon = [&](const Shape &shape) {
        Polygon polygon;
        const auto points = outline(shape);
        polygon.reserve(points.size());
        for (const auto &point : points)
            polygon.push_back({point.x, point.y});
        return polygon;
    };

    GeometryKernel kernel;
    MultiPolygon result;
    switch (operation) {
    case BooleanOperation::union_: {
        MultiPolygon polygons;
        polygons.reserve(operands.size());
        for (const auto *shape : operands)
            polygons.push_back(toPolygon(*shape));
        result = kernel.booleanOp(polygons, {}, BooleanOperation::union_);
        break;
    }
    case BooleanOperation::intersection:
        result = {toPolygon(*operands.front())};
        for (std::size_t index = 1; index < operands.size() && !result.empty(); ++index)
            result = kernel.booleanOp(result, {toPolygon(*operands[index])}, BooleanOperation::intersection);
        break;
    case BooleanOperation::difference: {
        MultiPolygon clips;
        clips.reserve(operands.size() - 1);
        for (std::size_t index = 1; index < operands.size(); ++index)
            clips.push_back(toPolygon(*operands[index]));
        if (clips.size() > 1)
            clips = kernel.booleanOp(clips, {}, BooleanOperation::union_);
        result = kernel.booleanOp({toPolygon(*operands.front())}, clips, BooleanOperation::difference);
        break;
    }
    case BooleanOperation::xor_:
        result = {toPolygon(*operands.front())};
        for (std::size_t index = 1; index < operands.size() && !result.empty(); ++index)
            result = kernel.booleanOp(result, {toPolygon(*operands[index])}, BooleanOperation::xor_);
        break;
    }

    result = kernel.cleanup(result);
    if (result.empty())
        throw ProjectError("layout.boolean", "Boolean operation produced no geometry");

    std::string type;
    switch (operation) {
    case BooleanOperation::union_:
        type = "Union";
        break;
    case BooleanOperation::difference:
        type = "Difference";
        break;
    case BooleanOperation::intersection:
        type = "Intersection";
        break;
    case BooleanOperation::xor_:
        type = "Xor";
        break;
    }

    for (const auto &shape : operands)
        cell.shapes.erase(shape->id);
    std::vector<ObjectId> resultIds;
    resultIds.reserve(result.size());
    for (const auto &polygon : result) {
        const auto id = project.nextId();
        std::vector<LayoutPoint> points;
        points.reserve(polygon.size());
        for (const auto &point : polygon)
            points.push_back({point.x, point.y});
        Shape shape{id, layer, net, GeometryKernel::bounds(polygon), std::move(points)};
        shape.name = nextShapeName(cell, type);
        cell.shapes.emplace(id, std::move(shape));
        resultIds.push_back(id);
    }
    return resultIds;
}

std::vector<std::string> buildSchematicNetlist(const Project &project, ObjectId cellId, std::vector<std::string> *notes,
                                               SchematicNetlistMode mode) {
    const auto &topCell = require(project.schematicCells, cellId, "schematic.cell", "Schematic cell does not exist");
    if (topCell.textSchematic || topCell.systemVerilogSchematic)
        throw ProjectError("schematic.netlist", "SPICE netlisting requires a drawn schematic");
    auto note = [notes](std::string message) {
        if (notes)
            notes->push_back(std::move(message));
    };

    // SPICE identifiers cannot contain arbitrary whitespace. Schematic names
    // and port names normally are simple identifiers, but sanitize them here
    // so hierarchy still produces a valid deck for names like "my block".
    const auto spiceIdentifier = [](const std::string &name) {
        std::string result;
        result.reserve(name.size());
        for (const auto character : name) {
            const auto byte = static_cast<unsigned char>(character);
            result.push_back(std::isalnum(byte) || character == '_' ? character : '_');
        }
        if (result.empty())
            result = "cell";
        if (std::isdigit(static_cast<unsigned char>(result.front())))
            result.insert(result.begin(), '_');
        return result;
    };

    // Hierarchical block pins are generated from the child schematic's Input
    // and Output instances, in the same order as generatedSchematicSymbol().
    const auto portInstances = [&](const SchematicCell &cell) {
        std::vector<const SchematicInstance *> inputs;
        std::vector<const SchematicInstance *> outputs;
        for (const auto &[_, instance] : cell.instances) {
            const auto symbol = cell.symbols.find(instance.symbol);
            if (symbol == cell.symbols.end())
                continue;
            const auto type = lower(symbol->second.name);
            if (type == "input")
                inputs.push_back(&instance);
            else if (type == "output")
                outputs.push_back(&instance);
        }
        const auto sortPorts = [](std::vector<const SchematicInstance *> &ports) {
            std::sort(ports.begin(), ports.end(), [](const SchematicInstance *left, const SchematicInstance *right) {
                if (left->y != right->y)
                    return left->y > right->y;
                return lower(left->name) < lower(right->name);
            });
        };
        sortPorts(inputs);
        sortPorts(outputs);
        std::vector<const SchematicInstance *> ports;
        ports.reserve(inputs.size() + outputs.size());
        ports.insert(ports.end(), inputs.begin(), inputs.end());
        ports.insert(ports.end(), outputs.begin(), outputs.end());
        return ports;
    };

    // A child Input/Output instance defines a port. Its connected net becomes
    // that port inside the subcircuit body.
    const auto portNodes = [&](const SchematicCell &cell) {
        std::map<ObjectId, std::string> result;
        for (const auto *port : portInstances(cell)) {
            const auto symbol = cell.symbols.find(port->symbol);
            if (symbol == cell.symbols.end() || symbol->second.pins.empty())
                continue;
            const auto pinId = symbol->second.pins.front().id;
            for (const auto &[netId, net] : cell.nets) {
                if (std::find(net.pins.begin(), net.pins.end(), pinId) == net.pins.end())
                    continue;
                if (!result.emplace(netId, spiceIdentifier(port->name)).second)
                    note("multiple schematic ports share a net in " + cell.name + "; using " + port->name);
                break;
            }
        }
        return result;
    };

    std::ostringstream out;
    out << "* ICE schematic netlist: " << topCell.name << "\n";
    std::ostringstream subcircuits;
    std::set<ObjectId> emittedSubcircuits;
    std::set<ObjectId> visiting;
    std::function<std::string(const SchematicCell &, const std::map<ObjectId, std::string> &, bool)> emitBody;
    std::function<void(ObjectId)> emitSubcircuit;

    emitBody = [&](const SchematicCell &cell, const std::map<ObjectId, std::string> &portNodeNames, bool topLevel) {
        // A symbol is reusable, so placing it twice duplicates its pin ids.
        // Detect symbols used by more than one instance; their pins cannot be
        // resolved to a unique instance for netlisting.
        std::map<ObjectId, int> symbolUseCount;
        for (const auto &[_, instance] : cell.instances)
            ++symbolUseCount[instance.symbol];

        const auto resolvePin = [&](ObjectId pinId, const SchematicInstance **owner, const Pin **pin) -> bool {
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

        const auto nodeFor = [&](const Net &net) -> std::string {
            if (const auto portNode = portNodeNames.find(net.id); portNode != portNodeNames.end())
                return portNode->second;
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

        std::ostringstream body;
        for (const auto &[instanceId, instance] : cell.instances) {
            const auto symbol = cell.symbols.find(instance.symbol);
            if (symbol == cell.symbols.end())
                continue;
            const auto type = lower(symbol->second.name);
            if (type == "input" || type == "output")
                continue; // hierarchical port markers, not devices
            const char element = spiceElementForSymbol(symbol->second.name);
            if (element == 'G')
                continue; // ground contributes the reference node only

            if (symbol->second.sourceCell != 0) {
                const auto child = project.schematicCells.find(symbol->second.sourceCell);
                if (child == project.schematicCells.end()) {
                    throw ProjectError("schematic.netlist.hierarchy", "Hierarchical symbol '" + symbol->second.name +
                                                                          "' (" + instance.name +
                                                                          ") references a missing schematic");
                }
                if (child->second.systemVerilogSchematic) {
                    if (mode != SchematicNetlistMode::mixed) {
                        throw ProjectError("schematic.netlist.hdl",
                                           "Schematic contains SystemVerilog block '" + child->second.name + "' (" +
                                               instance.name +
                                               "); use mixed-signal simulation instead of a pure SPICE netlist");
                    }
                    if (!topLevel) {
                        throw ProjectError("schematic.netlist.hdl_nested",
                                           "Nested SystemVerilog block '" + child->second.name + "' (" + instance.name +
                                               ") is not supported; place the HDL instance in the top-level "
                                               "schematic");
                    }
                    note("SystemVerilog block omitted from analog base netlist: " + child->second.name + " (" +
                         instance.name + ")");
                    body << "* SystemVerilog block: " << instance.name << " " << child->second.name << "\n";
                    continue;
                }
                if (child->second.textSchematic) {
                    throw ProjectError("schematic.netlist.text", "Text SPICE schematic '" + child->second.name + "' (" +
                                                                     instance.name +
                                                                     ") cannot be used as a hierarchical block");
                }
                if (visiting.contains(child->first)) {
                    throw ProjectError("schematic.netlist.cycle", "Hierarchical schematic cycle at '" +
                                                                      child->second.name + "' (" + instance.name + ")");
                }
                if (symbolUseCount[instance.symbol] > 1) {
                    throw ProjectError("schematic.netlist.ambiguous_symbol",
                                       "Symbol '" + symbol->second.name +
                                           "' is instantiated more than once; create one symbol per instance");
                }
                emitSubcircuit(child->first);
                if (!emittedSubcircuits.contains(child->first)) {
                    body << "* unavailable subcircuit: " << instance.name << " " << child->second.name << "\n";
                    continue;
                }

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

                std::string designator = instance.name;
                if (designator.empty() || std::toupper(static_cast<unsigned char>(designator.front())) != 'X')
                    designator.insert(designator.begin(), 'X');
                body << designator << " ";
                for (const auto &pin : symbol->second.pins) {
                    const auto node = pinToNode.find(pin.id);
                    body << (node != pinToNode.end() ? node->second : "0") << " ";
                }
                body << spiceIdentifier(child->second.name) << "\n";
                continue;
            }

            if (element == 0) {
                note("unsupported symbol skipped: " + symbol->second.name + " (" + instance.name + ")");
                body << "* unsupported: " << instance.name << " " << symbol->second.name << "\n";
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

            const auto pinNameToNode = [&] {
                std::map<std::string, std::string> result;
                for (const auto &pin : symbol->second.pins) {
                    const auto node = pinToNode.find(pin.id);
                    if (node != pinToNode.end())
                        result[lower(pin.name)] = node->second;
                }
                return result;
            }();
            const auto nodesForNames =
                [&](std::initializer_list<std::string_view> names) -> std::optional<std::vector<std::string>> {
                std::vector<std::string> result;
                result.reserve(names.size());
                for (const auto name : names) {
                    const auto node = pinNameToNode.find(lower(std::string(name)));
                    if (node == pinNameToNode.end())
                        return std::nullopt;
                    result.push_back(node->second);
                }
                return result;
            };
            const auto symbolNodes = [&] {
                std::vector<std::string> result;
                result.reserve(symbol->second.pins.size());
                for (const auto &pin : symbol->second.pins) {
                    const auto node = pinToNode.find(pin.id);
                    result.push_back(node != pinToNode.end() ? node->second : "0");
                }
                return result;
            };

            // PDK packages expose MOS and BJT devices as SPICE subcircuits,
            // while generic symbols use primitive M/Q elements. A configured
            // model is what distinguishes the two cases.
            const auto configuredModel = parameterNamed(instance, {"model", "spice_model", "subckt"});
            const bool pdkSubcircuit = !cell.pdk.empty() && cell.pdk != genericPdkId && !configuredModel.empty() &&
                                       (element == 'M' || element == 'Q');
            const char lineElement = pdkSubcircuit ? 'X' : element;

            // Transistors and subcircuits need a fixed SPICE pin order which
            // differs from the symbol's visual pin order.
            std::vector<std::string> nodes;
            if (element == 'M') {
                const auto ordered = nodesForNames({"D", "G", "S", "B"});
                if (!ordered) {
                    note("MOS symbol missing D/G/S/B pins: " + symbol->second.name + " (" + instance.name + ")");
                    body << "* missing MOS pins: " << instance.name << " " << symbol->second.name << "\n";
                    continue;
                }
                nodes = *ordered;
            } else if (element == 'Q') {
                const auto ordered = nodesForNames({"C", "B", "E"});
                if (!ordered) {
                    note("BJT symbol missing C/B/E pins: " + symbol->second.name + " (" + instance.name + ")");
                    body << "* missing BJT pins: " << instance.name << " " << symbol->second.name << "\n";
                    continue;
                }
                nodes = *ordered;
                if (lineElement == 'X') {
                    const auto substrate = nodesForNames({"S", "SUB", "BN"});
                    nodes.push_back(substrate ? substrate->front() : "0");
                }
            } else if (element == 'X') {
                const auto ordered = nodesForNames({"+", "-", "OUT"});
                nodes = ordered ? *ordered : symbolNodes();
            } else if (element == 'D') {
                const auto ordered = nodesForNames({"A", "K"});
                nodes = ordered ? *ordered : symbolNodes();
            } else {
                nodes = symbolNodes();
            }
            if (nodes.empty()) {
                note("instance has no connected pins: " + instance.name);
                continue;
            }

            // Instance names commonly already carry the element letter (V1, R1);
            // only prepend it when they do not, so we never emit "VV1".
            std::string designator = instance.name;
            const auto prefix = static_cast<char>(std::toupper(static_cast<unsigned char>(lineElement)));
            if (designator.empty() ||
                static_cast<char>(std::toupper(static_cast<unsigned char>(designator.front()))) != prefix)
                designator.insert(designator.begin(), prefix);

            const auto appendAttribute = [&](std::string_view label, std::initializer_list<std::string_view> names) {
                const auto value = parameterNamed(instance, names);
                if (!value.empty())
                    body << " " << label << "=" << value;
            };

            if (element == 'M' || element == 'Q' || element == 'X' || element == 'D') {
                const auto defaultModel = [&] {
                    const auto symbolName = lower(symbol->second.name);
                    if (element == 'M')
                        return symbolName.find("pmos") != std::string::npos ||
                                       symbolName.find("pfet") != std::string::npos
                                   ? std::string("PMOS")
                                   : std::string("NMOS");
                    if (element == 'Q')
                        return symbolName.find("pnp") != std::string::npos ? std::string("PNP") : std::string("NPN");
                    if (element == 'X')
                        return std::string("OPAMP");
                    return std::string("Dmodel");
                }();
                auto model = configuredModel;
                if (model.empty()) {
                    model = defaultModel;
                    note("missing model parameter for " + instance.name + ", using " + model);
                }
                body << designator << " ";
                for (const auto &node : nodes)
                    body << node << " ";
                body << spiceIdentifier(model);
                if (element == 'M') {
                    appendAttribute("W", {"w", "width"});
                    appendAttribute("L", {"l", "length"});
                    appendAttribute("M", {"m", "multiplier"});
                } else if (element == 'Q' && lineElement == 'Q') {
                    appendAttribute("M", {"m", "multiplier"});
                    appendAttribute("AREA", {"area"});
                } else if (element == 'D') {
                    appendAttribute("AREA", {"area"});
                }
                body << "\n";
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
                           : element == 'I' ? "1m"
                                            : "1";
                note("missing value parameter for " + instance.name + ", using " + resolved);
            }

            body << designator << " ";
            for (const auto &node : nodes)
                body << node << " ";
            body << resolved << "\n";
        }
        return body.str();
    };

    emitSubcircuit = [&](ObjectId childId) {
        if (emittedSubcircuits.contains(childId))
            return;
        if (visiting.contains(childId)) {
            note("hierarchical schematic cycle skipped: " + std::to_string(childId));
            return;
        }
        const auto child = project.schematicCells.find(childId);
        if (child == project.schematicCells.end())
            return;
        if (child->second.isTextSource()) {
            throw ProjectError("schematic.netlist.source_child", "Source schematic '" + child->second.name +
                                                                     "' cannot be used as a drawn hierarchical block");
        }

        visiting.insert(childId);
        const auto ports = portInstances(child->second);
        const auto portNodeNames = portNodes(child->second);
        const auto body = emitBody(child->second, portNodeNames, false);

        std::ostringstream definition;
        definition << ".subckt " << spiceIdentifier(child->second.name);
        for (const auto *port : ports)
            definition << " " << spiceIdentifier(port->name);
        definition << "\n" << body << ".ends " << spiceIdentifier(child->second.name) << "\n";
        visiting.erase(childId);
        emittedSubcircuits.insert(childId);
        subcircuits << definition.str();
    };

    visiting.insert(cellId);
    const auto topBody = emitBody(topCell, {}, true);
    visiting.erase(cellId);
    out << subcircuits.str() << topBody;

    std::vector<std::string> lines;
    std::istringstream stream(out.str());
    std::string line;
    while (std::getline(stream, line))
        if (!line.empty())
            lines.push_back(line);
    return lines;
}

std::vector<std::string> buildSimulationNetlist(const Project &project, ObjectId cellId,
                                                std::vector<std::string> *notes) {
    const auto &cell = require(project.schematicCells, cellId, "schematic.cell", "Schematic cell does not exist");
    if (cell.systemVerilogSchematic)
        throw ProjectError("simulation.hdl", "SystemVerilog cells must be simulated with the iverilog simulator");
    if (!cell.textSchematic)
        return buildSchematicNetlist(project, cellId, notes);

    std::vector<std::string> lines;
    std::istringstream stream(cell.spiceText);
    std::string line;
    while (std::getline(stream, line)) {
        const auto trimmed = trimLine(line);
        if (trimmed.empty() || isOwnedAnalysisDirective(trimmed))
            continue;
        lines.push_back(trimmed);
    }
    return lines;
}

ObjectId addSimulationGraph(Project &project, ObjectId simulationId, const SimulationGraph &graph) {
    auto simulation = project.simulations.find(simulationId);
    if (simulation == project.simulations.end())
        throw ProjectError("simulation.graph", "Unknown simulation id: " + std::to_string(simulationId));
    if (graph.name.empty())
        throw ProjectError("simulation.graph.name", "Graph name is required");
    auto added = graph;
    added.id = project.nextId();
    simulation->second.graphs.push_back(std::move(added));
    return simulation->second.graphs.back().id;
}

bool updateSimulationGraph(Project &project, ObjectId simulationId, ObjectId graphId, const SimulationGraph &graph) {
    auto simulation = project.simulations.find(simulationId);
    if (simulation == project.simulations.end())
        return false;
    const auto found = std::find_if(simulation->second.graphs.begin(), simulation->second.graphs.end(),
                                    [graphId](const SimulationGraph &entry) { return entry.id == graphId; });
    if (found == simulation->second.graphs.end())
        return false;
    if (graph.name.empty())
        throw ProjectError("simulation.graph.name", "Graph name is required");
    found->name = graph.name;
    found->xAxis = graph.xAxis;
    found->ySignals = graph.ySignals;
    found->signalLabels = graph.signalLabels;
    found->yAxisLabel = graph.yAxisLabel;
    found->xMin = graph.xMin;
    found->xMax = graph.xMax;
    found->yMin = graph.yMin;
    found->yMax = graph.yMax;
    return true;
}

namespace {
std::string joinNames(const std::vector<std::string> &names) {
    std::ostringstream result;
    for (std::size_t index = 0; index < names.size(); ++index) {
        if (index)
            result << ", ";
        result << names[index];
    }
    return result.str();
}
} // namespace

void removeSchematicCell(Project &project, ObjectId cellId) {
    const auto cell = project.schematicCells.find(cellId);
    if (cell == project.schematicCells.end())
        throw ProjectError("schematic.remove.unknown", "Unknown schematic id: " + std::to_string(cellId));

    std::vector<std::string> simulations;
    for (const auto &[_, simulation] : project.simulations) {
        if (simulation.schematic == cellId)
            simulations.push_back(simulation.name);
    }
    if (!simulations.empty())
        throw ProjectError("schematic.remove.referenced",
                           "Schematic '" + cell->second.name + "' is used by simulations: " + joinNames(simulations));

    std::vector<std::string> symbols;
    for (const auto &[_, other] : project.schematicCells) {
        if (other.id == cellId)
            continue;
        for (const auto &[_, symbol] : other.symbols) {
            if (symbol.sourceCell == cellId)
                symbols.push_back(symbol.name + " in " + other.name);
        }
    }
    if (!symbols.empty())
        throw ProjectError("schematic.remove.referenced",
                           "Schematic '" + cell->second.name +
                               "' is used by hierarchical symbols: " + joinNames(symbols));

    project.schematicCells.erase(cellId);
}

void removeLayoutItem(Project &project, ObjectId id) {
    if (const auto procedural = project.proceduralLayouts.find(id); procedural != project.proceduralLayouts.end()) {
        const auto generatedId = procedural->second.generatedCell;
        std::vector<std::string> references;
        for (const auto &[otherId, otherCell] : project.layoutCells) {
            if (otherId == generatedId)
                continue;
            for (const auto &[_, instance] : otherCell.instances) {
                if (instance.cell == generatedId)
                    references.push_back(instance.name + " in " + otherCell.name);
            }
        }
        if (!references.empty())
            throw ProjectError("layout.remove.referenced", "Generated layout for procedural layout '" +
                                                               procedural->second.name +
                                                               "' is used as an instance: " + joinNames(references));
        project.proceduralLayouts.erase(procedural);
        project.layoutCells.erase(generatedId);
        return;
    }

    const auto cell = project.layoutCells.find(id);
    if (cell == project.layoutCells.end())
        throw ProjectError("layout.remove.unknown", "Unknown layout or procedural layout id: " + std::to_string(id));
    if (cell->second.library)
        throw ProjectError("layout.remove.library", "Library layout cells are read-only and cannot be removed");
    if (cell->second.generatedBy != 0 && project.proceduralLayouts.contains(cell->second.generatedBy)) {
        const auto &procedural = project.proceduralLayouts.at(cell->second.generatedBy);
        throw ProjectError("layout.remove.generated", "Layout cell '" + cell->second.name +
                                                          "' is generated by procedural layout '" + procedural.name +
                                                          "'; remove the procedural layout instead");
    }

    std::vector<std::string> references;
    for (const auto &[otherId, otherCell] : project.layoutCells) {
        if (otherId == id)
            continue;
        for (const auto &[_, instance] : otherCell.instances) {
            if (instance.cell == id)
                references.push_back(instance.name + " in " + otherCell.name);
        }
    }
    if (!references.empty())
        throw ProjectError("layout.remove.referenced",
                           "Layout cell '" + cell->second.name + "' is used as an instance: " + joinNames(references));

    project.layoutCells.erase(id);
}

void removeSimulationItem(Project &project, ObjectId simulationId) {
    if (!project.simulations.contains(simulationId))
        throw ProjectError("simulation.remove.unknown", "Unknown simulation id: " + std::to_string(simulationId));
    project.simulations.erase(simulationId);
}

bool removeSimulationGraph(Project &project, ObjectId simulationId, ObjectId graphId) {
    auto simulation = project.simulations.find(simulationId);
    if (simulation == project.simulations.end())
        return false;
    auto &graphs = simulation->second.graphs;
    const auto found = std::find_if(graphs.begin(), graphs.end(),
                                    [graphId](const SimulationGraph &entry) { return entry.id == graphId; });
    if (found == graphs.end())
        return false;
    graphs.erase(found);
    return true;
}
} // namespace ice
