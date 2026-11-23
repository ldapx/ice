#include "icelib/operations.hpp"

#include "icelib/hdl.hpp"
#include "icelib/pdk.hpp"
#include "icelib/spice.hpp"
#include "icelib/symbols.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <iomanip>
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

// Length conversion lives in pdk.cpp so PEX and the schematic netlister agree
// on the units selected by `.option scale=...`.
std::optional<double> scaledNetlistLength(const std::string &text, double geometryScale) {
    return netlistLengthNumber(text, geometryScale);
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

std::string symbolTypeName(std::string name) {
    // A trailing numeric disambiguator ("Input 2", "Resistor 3") is added when a
    // cell needs several symbols of the same kind. It is not part of the type.
    const auto trimmed = trimLine(name);
    const auto separator = trimmed.find_last_of(" _-");
    if (separator != std::string::npos && separator + 1 < trimmed.size() &&
        std::all_of(trimmed.begin() + static_cast<std::ptrdiff_t>(separator + 1), trimmed.end(),
                    [](char character) { return std::isdigit(static_cast<unsigned char>(character)) != 0; }))
        return trimmed.substr(0, separator);
    return trimmed;
}

std::string uniqueSchematicSymbolName(const SchematicCell &cell, const std::string &base) {
    const auto taken = [&](const std::string &candidate) {
        return std::any_of(cell.symbols.begin(), cell.symbols.end(),
                           [&](const auto &entry) { return entry.second.name == candidate; });
    };
    auto name = base;
    for (int suffix = 2; taken(name); ++suffix)
        name = base + " " + std::to_string(suffix);
    return name;
}

std::vector<const SchematicInstance *> schematicPortInstances(const SchematicCell &cell) {
    std::vector<const SchematicInstance *> inputs;
    std::vector<const SchematicInstance *> outputs;
    for (const auto &[_, instance] : cell.instances) {
        const auto symbol = cell.symbols.find(instance.symbol);
        if (symbol == cell.symbols.end())
            continue;
        switch (schematicPortKindForSymbol(symbol->second.name)) {
        case SchematicPortKind::input:
        case SchematicPortKind::inout:
            inputs.push_back(&instance);
            break;
        case SchematicPortKind::output:
            outputs.push_back(&instance);
            break;
        case SchematicPortKind::none:
            break;
        }
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
}

SchematicPortKind schematicPortKindForSymbol(const std::string &symbolName) {
    const auto type = lower(symbolTypeName(symbolName));
    if (type == "input")
        return SchematicPortKind::input;
    if (type == "output")
        return SchematicPortKind::output;
    if (type == "pin" || type == "inout" || type == "bidirectional")
        return SchematicPortKind::inout;
    return SchematicPortKind::none;
}

ObjectId addHierarchicalSymbol(Project &project, ObjectId parentCellId, ObjectId sourceCellId, std::string name,
                               std::string artwork) {
    auto &source = require(project.schematicCells, sourceCellId, "schematic.symbol.source",
                           "Source schematic cell does not exist");
    const auto &parent =
        require(project.schematicCells, parentCellId, "schematic.cell", "Schematic cell does not exist");
    if (parent.isTextSource())
        throw ProjectError("schematic.symbol.source", "Symbols can only be added to a drawn schematic");
    if (source.textSchematic) {
        const auto interface = spiceInterfaceForCell(source);
        if (!interface.ok)
            throw ProjectError("schematic.spice.parse", interface.error);
        // Refresh the cell's declared defaults so new instances can show and
        // override them. Manually authored values are preserved.
        for (const auto &parameter : interface.parameters) {
            if (parameter.name.empty())
                continue;
            const auto existing =
                std::find_if(source.parameters.begin(), source.parameters.end(), [&](const Parameter &candidate) {
                    return lower(candidate.name) == lower(parameter.name);
                });
            if (existing == source.parameters.end())
                source.parameters.push_back({project.nextId(), parameter.name, parameter.value});
            else if (existing->value.empty())
                existing->value = parameter.value;
        }
    }
    if (name.empty())
        name = source.textSchematic            ? source.name + " SPICE"
               : source.systemVerilogSchematic ? source.name + " HDL"
                                               : source.name;
    name = uniqueSchematicSymbolName(parent, name);
    auto tmpl = hierarchicalSymbolTemplate(source);
    if (!artwork.empty())
        tmpl.artwork = std::move(artwork);
    const auto symbolId = addSymbol(project, parentCellId, std::move(name), std::move(tmpl.artwork), sourceCellId);
    auto &symbol = project.schematicCells.at(parentCellId).symbols.at(symbolId);
    symbol.label = SymbolLabel{0, tmpl.labelY};
    auto nextPinId = project.nextId();
    for (auto pin : tmpl.pins) {
        pin.id = nextPinId++;
        symbol.pins.push_back(std::move(pin));
    }
    return symbolId;
}

HierarchicalSymbolTemplate hierarchicalSymbolTemplate(const SchematicCell &source) {
    const auto fromHdl = [](HdlSymbolTemplate tmpl) {
        return HierarchicalSymbolTemplate{std::move(tmpl.artwork), std::move(tmpl.pins), tmpl.labelY};
    };

    HdlModule module;
    module.name = source.name;
    if (source.systemVerilogSchematic) {
        const auto parsed = parseSystemVerilogModule(source);
        if (!parsed.ok)
            throw ProjectError("schematic.hdl.parse", parsed.error);
        return fromHdl(makeHdlSymbolTemplate(parsed.module, true));
    }
    if (source.textSchematic) {
        const auto interface = spiceInterfaceForCell(source);
        if (!interface.ok)
            throw ProjectError("schematic.spice.parse", interface.error);
        for (const auto &port : interface.ports)
            module.ports.push_back({port, HdlPortDirection::input, 1, true, false});
    } else {
        for (const auto *port : schematicPortInstances(source)) {
            const auto symbol = source.symbols.find(port->symbol);
            const auto kind = symbol == source.symbols.end() ? SchematicPortKind::none
                                                             : schematicPortKindForSymbol(symbol->second.name);
            const auto direction =
                kind == SchematicPortKind::output ? HdlPortDirection::output : HdlPortDirection::input;
            module.ports.push_back({port->name, direction, 1, true, false});
        }
    }
    return fromHdl(makeHdlSymbolTemplate(module, false));
}

std::vector<Pin> hierarchicalSymbolPins(const SchematicCell &source) { return hierarchicalSymbolTemplate(source).pins; }

std::size_t repairHierarchicalSymbolArtwork(Project &project) {
    std::size_t repaired = 0;
    for (auto &[_, parent] : project.schematicCells) {
        for (auto &[__, symbol] : parent.symbols) {
            if (symbol.sourceCell == 0 || !symbol.artwork.empty())
                continue;
            const auto source = project.schematicCells.find(symbol.sourceCell);
            if (source == project.schematicCells.end())
                continue;
            HierarchicalSymbolTemplate tmpl;
            try {
                tmpl = hierarchicalSymbolTemplate(source->second);
            } catch (const ProjectError &) {
                continue;
            }
            if (tmpl.artwork.empty())
                continue;
            std::map<std::string, const Pin *> targetByName;
            for (const auto &pin : tmpl.pins)
                targetByName.emplace(lower(pin.name), &pin);
            for (auto &pin : symbol.pins) {
                const auto target = targetByName.find(lower(pin.name));
                if (target == targetByName.end())
                    continue;
                pin.x = target->second->x;
                pin.y = target->second->y;
                pin.direction = target->second->direction;
                pin.width = target->second->width;
                pin.widthKnown = target->second->widthKnown;
            }
            symbol.artwork = std::move(tmpl.artwork);
            symbol.label = SymbolLabel{0, tmpl.labelY};
            ++repaired;
        }
    }
    return repaired;
}

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
ObjectId createTextSchematicCell(Project &project, std::string name, std::string pdk, std::string spiceText,
                                 std::string spiceSourcePath) {
    const auto id = createSchematicCell(project, std::move(name), std::move(pdk));
    auto &cell = project.schematicCells.at(id);
    cell.textSchematic = true;
    cell.spiceText = std::move(spiceText);
    cell.spiceSourcePath = std::move(spiceSourcePath);
    if (!cell.spiceSourcePath.empty()) {
        std::error_code status;
        const auto absolute = std::filesystem::absolute(cell.spiceSourcePath, status);
        if (!status)
            cell.spiceSourcePath = absolute.lexically_normal().string();
    }
    const auto interface = spiceInterfaceForText(cell.spiceText, cell.name, cell.spiceSourcePath);
    for (const auto &parameter : interface.parameters) {
        if (parameter.name.empty())
            continue;
        const auto existing = std::find_if(cell.parameters.begin(), cell.parameters.end(), [&](const Parameter &entry) {
            return lower(entry.name) == lower(parameter.name);
        });
        if (existing == cell.parameters.end())
            cell.parameters.push_back({project.nextId(), parameter.name, parameter.value});
    }
    return id;
}
ObjectId createSystemVerilogCell(Project &project, std::string name, std::string pdk, std::string source) {
    const auto id = createSchematicCell(project, std::move(name), std::move(pdk));
    auto &cell = project.schematicCells.at(id);
    cell.systemVerilogSchematic = true;
    cell.systemVerilogText = std::move(source);
    return id;
}
namespace {

void applySourceDefaults(std::vector<Parameter> &parameters, const std::vector<Parameter> &oldDefaults,
                         const std::vector<Parameter> &newDefaults, Project &project) {
    std::map<std::string, std::string> oldByName;
    for (const auto &parameter : oldDefaults)
        oldByName[lower(parameter.name)] = parameter.value;
    for (const auto &defaultParameter : newDefaults) {
        if (defaultParameter.name.empty())
            continue;
        const auto key = lower(defaultParameter.name);
        const auto existing = std::find_if(parameters.begin(), parameters.end(),
                                           [&](const Parameter &candidate) { return lower(candidate.name) == key; });
        if (existing == parameters.end()) {
            parameters.push_back({project.nextId(), defaultParameter.name, defaultParameter.value});
            continue;
        }
        const auto old = oldByName.find(key);
        if (existing->value.empty() || (old != oldByName.end() && existing->value == old->second))
            existing->value = defaultParameter.value;
    }
}

void updateInstanceDefaults(Project &project, SchematicCell &parent, ObjectId symbolId,
                            const std::vector<Parameter> &oldDefaults, const std::vector<Parameter> &newDefaults) {
    for (auto &[_, instance] : parent.instances) {
        if (instance.symbol == symbolId)
            applySourceDefaults(instance.parameters, oldDefaults, newDefaults, project);
    }
}

} // namespace

SchematicSourceEditResult setSchematicSourceText(Project &project, ObjectId cellId, bool systemVerilog,
                                                 std::string text, std::string sourcePath, bool convertEmptyDrawn) {
    auto &cell = require(project.schematicCells, cellId, "schematic.cell", "Schematic cell does not exist");
    SchematicSourceEditResult result;
    if (cell.isTextSource() && cell.systemVerilogSchematic != systemVerilog)
        throw ProjectError("schematic.source.kind", "Schematic '" + cell.name + "' is already a " +
                                                        (cell.systemVerilogSchematic ? "SystemVerilog" : "text SPICE") +
                                                        " cell; create a new cell to use the other source language");
    if (!cell.isTextSource()) {
        const bool empty =
            cell.symbols.empty() && cell.instances.empty() && cell.nets.empty() && cell.parameters.empty();
        if (!convertEmptyDrawn || !empty)
            throw ProjectError("schematic.source.convert",
                               "Schematic '" + cell.name +
                                   "' is a drawn schematic; pass --convert only for an empty cell");
        result.converted = true;
    }

    std::vector<Parameter> oldDefaults;
    if (cell.textSchematic) {
        const auto oldInterface = spiceInterfaceForCell(cell);
        if (oldInterface.ok) {
            for (const auto &parameter : oldInterface.parameters)
                oldDefaults.push_back({0, parameter.name, parameter.value});
        }
    }

    cell.textSchematic = !systemVerilog;
    cell.systemVerilogSchematic = systemVerilog;
    cell.spiceSourcePath.clear();
    if (systemVerilog) {
        cell.systemVerilogText = std::move(text);
        cell.spiceText.clear();
    } else {
        cell.spiceText = std::move(text);
        cell.systemVerilogText.clear();
        if (!sourcePath.empty()) {
            std::error_code status;
            const auto absolute = std::filesystem::absolute(sourcePath, status);
            cell.spiceSourcePath = (status ? std::filesystem::path(sourcePath) : absolute).lexically_normal().string();
        }
    }

    std::vector<Parameter> newDefaults;
    std::function<HierarchicalSymbolTemplate(const Symbol &)> templateForSymbol;
    if (systemVerilog) {
        const auto parsed = parseSystemVerilogModule(cell);
        if (!parsed.ok) {
            result.notes.push_back("SystemVerilog parse failed: " + parsed.error);
        } else {
            result.notes.insert(result.notes.end(), parsed.warnings.begin(), parsed.warnings.end());
            templateForSymbol = [parsed](const Symbol &symbol) {
                const bool hasBus =
                    std::any_of(parsed.module.ports.begin(), parsed.module.ports.end(),
                                [](const HdlPort &port) { return port.widthKnown && port.width > 1 && !port.real; });
                bool flatten = false;
                if (hasBus) {
                    flatten = std::any_of(symbol.pins.begin(), symbol.pins.end(), [](const Pin &pin) {
                        return pin.name.find('[') != std::string::npos || pin.name.find(']') != std::string::npos;
                    });
                    if (!flatten) {
                        std::size_t expanded = 0;
                        for (const auto &port : parsed.module.ports)
                            expanded +=
                                port.widthKnown && !port.real ? static_cast<std::size_t>(std::max(1, port.width)) : 1;
                        flatten = symbol.pins.size() == expanded;
                    }
                }
                auto hdl = makeHdlSymbolTemplate(parsed.module, flatten);
                return HierarchicalSymbolTemplate{std::move(hdl.artwork), std::move(hdl.pins), hdl.labelY};
            };
        }
    } else {
        const auto interface = spiceInterfaceForText(cell.spiceText, cell.name, cell.spiceSourcePath);
        result.notes.insert(result.notes.end(), interface.warnings.begin(), interface.warnings.end());
        if (!interface.ok) {
            result.notes.push_back("SPICE interface is not ready: " + interface.error);
        } else {
            for (const auto &parameter : interface.parameters)
                newDefaults.push_back({0, parameter.name, parameter.value});
            auto tmpl = hierarchicalSymbolTemplate(cell);
            templateForSymbol = [tmpl](const Symbol &) { return tmpl; };
        }
    }

    if (templateForSymbol) {
        for (auto &[parentId, parent] : project.schematicCells) {
            (void)parentId;
            for (auto &[symbolId, symbol] : parent.symbols) {
                if (symbol.sourceCell != cellId)
                    continue;
                const auto tmpl = templateForSymbol(symbol);
                const auto &targets = tmpl.pins;
                if (targets.empty())
                    continue;

                std::set<ObjectId> connectedPins;
                for (const auto &[_, net] : parent.nets)
                    connectedPins.insert(net.pins.begin(), net.pins.end());

                std::map<std::string, const Pin *> existingByName;
                for (const auto &pin : symbol.pins)
                    existingByName.emplace(lower(pin.name), &pin);

                std::set<std::string> targetNames;
                std::vector<Pin> reconciled;
                reconciled.reserve(targets.size());
                for (const auto &target : targets) {
                    targetNames.insert(lower(target.name));
                    const auto existing = existingByName.find(lower(target.name));
                    if (existing == existingByName.end()) {
                        Pin pin = target;
                        pin.id = project.nextId();
                        reconciled.push_back(pin);
                        ++result.addedPins;
                        continue;
                    }
                    Pin pin = *existing->second;
                    pin.name = target.name;
                    pin.direction = target.direction;
                    pin.width = target.width;
                    pin.widthKnown = target.widthKnown;
                    reconciled.push_back(std::move(pin));
                }

                std::set<ObjectId> removedPins;
                for (const auto &pin : symbol.pins) {
                    if (targetNames.contains(lower(pin.name)))
                        continue;
                    if (connectedPins.contains(pin.id))
                        throw ProjectError("schematic.source.connected_pin",
                                           "Cannot change the source of '" + cell.name + "': pin '" + pin.name +
                                               "' is still connected in '" + parent.name + "'");
                    removedPins.insert(pin.id);
                    ++result.removedPins;
                }
                for (auto &[_, net] : parent.nets) {
                    std::erase_if(net.pins, [&](ObjectId pin) { return removedPins.contains(pin); });
                }
                std::erase_if(parent.nets, [](const auto &entry) { return entry.second.pins.size() < 2; });
                symbol.pins = std::move(reconciled);
                symbol.artwork = tmpl.artwork;
                symbol.label = SymbolLabel{0, tmpl.labelY};
                updateInstanceDefaults(project, parent, symbolId, oldDefaults, newDefaults);
            }
        }
    }

    applySourceDefaults(cell.parameters, oldDefaults, newDefaults, project);
    return result;
}

ObjectId addSymbol(Project &project, ObjectId cellId, std::string name, std::string artwork, ObjectId sourceCell) {
    if (name.empty())
        throw ProjectError("schematic.symbol", "Symbol name must not be empty");
    auto &cell = require(project.schematicCells, cellId, "schematic.cell", "Schematic cell does not exist");
    ObjectId id = project.nextId();
    cell.symbols.emplace(id, Symbol{id, std::move(name), std::move(artwork), sourceCell});
    return id;
}
void setSchematicSymbolArtwork(Project &project, ObjectId cellId, ObjectId symbolId, std::string artwork) {
    validateSymbolArtwork(artwork);
    auto &cell = require(project.schematicCells, cellId, "schematic.cell", "Schematic cell does not exist");
    const auto symbol = cell.symbols.find(symbolId);
    if (symbol == cell.symbols.end())
        throw ProjectError("schematic.symbol", "Symbol does not exist");
    const auto sourceCellId = symbol->second.sourceCell;
    if (sourceCellId == 0)
        throw ProjectError("schematic.symbol.artwork",
                           "Only symbols created from a schematic cell can have a custom icon");
    const auto source = project.schematicCells.find(sourceCellId);
    if (source == project.schematicCells.end() || source->second.isTextSource())
        throw ProjectError("schematic.symbol.artwork", "The symbol does not reference a drawn schematic cell");
    // Placed instances own private copies of the block symbol, so match the
    // source schematic instead of the symbol id or a suffixed symbol name.
    for (auto &entry : cell.symbols)
        if (entry.second.sourceCell == sourceCellId)
            entry.second.artwork = artwork;
}
ObjectId addSchematicInstance(Project &project, ObjectId cellId, ObjectId symbolId, std::string name, Coordinate x,
                              Coordinate y) {
    if (name.empty())
        throw ProjectError("schematic.instance", "Instance name must not be empty");
    auto &cell = require(project.schematicCells, cellId, "schematic.cell", "Schematic cell does not exist");
    const auto existing = cell.symbols.find(symbolId);
    if (existing == cell.symbols.end())
        throw ProjectError("schematic.symbol", "Symbol does not exist");

    // Nets reference symbol pins, so two instances of one symbol would share
    // pin ids and their connectivity could not be told apart (a netlist would
    // wire both copies to the same nodes, and LVS could not attribute port
    // nets). Give every instance its own copy of the symbol instead; the copy
    // keeps the artwork, parameters and pin names.
    const bool symbolInUse = std::any_of(cell.instances.begin(), cell.instances.end(),
                                         [&](const auto &entry) { return entry.second.symbol == symbolId; });
    auto instanceSymbol = symbolId;
    if (symbolInUse) {
        Symbol copy = existing->second;
        auto nextCopyId = project.nextId();
        copy.id = nextCopyId++;
        copy.name = uniqueSchematicSymbolName(cell, existing->second.name);
        for (auto &pin : copy.pins)
            pin.id = nextCopyId++;
        instanceSymbol = copy.id;
        cell.symbols.emplace(copy.id, std::move(copy));
    }

    ObjectId id = project.nextId();
    cell.instances.emplace(id, SchematicInstance{id, instanceSymbol, std::move(name), x, y});
    // A hierarchical block starts with the child cell's parameter defaults so
    // they are visible and editable in the parent. Netlisting still treats
    // them as optional overrides over the subcircuit defaults.
    const auto &placedSymbol = cell.symbols.at(instanceSymbol);
    if (placedSymbol.sourceCell != 0) {
        if (const auto child = project.schematicCells.find(placedSymbol.sourceCell);
            child != project.schematicCells.end()) {
            auto &placed = cell.instances.at(id);
            std::set<std::string> names;
            for (auto parameter : child->second.parameters) {
                if (parameter.name.empty() || !names.insert(lower(parameter.name)).second)
                    continue;
                parameter.id = project.nextId();
                placed.parameters.push_back(std::move(parameter));
            }
        }
    }
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

void setSchematicCellParameter(Project &project, ObjectId cellId, std::string name, std::string value) {
    if (name.empty())
        throw ProjectError("schematic.parameter.name", "Parameter name must not be empty");
    auto &cell = require(project.schematicCells, cellId, "schematic.cell", "Schematic cell does not exist");
    std::string previousValue;
    bool existingParameter = false;
    for (auto &parameter : cell.parameters) {
        if (lower(parameter.name) == lower(name)) {
            previousValue = parameter.value;
            parameter.value = value;
            existingParameter = true;
            break;
        }
    }
    if (!existingParameter)
        cell.parameters.push_back({project.nextId(), name, value});

    // Keep already-placed hierarchical instances in sync with the child
    // declaration. Instances whose value still matches the previous default
    // are treated as unmodified and follow the new default; explicit
    // overrides are preserved.
    for (auto &[otherId, otherCell] : project.schematicCells) {
        if (otherId == cellId)
            continue;
        for (const auto &[symbolId, symbol] : otherCell.symbols) {
            if (symbol.sourceCell != cellId)
                continue;
            for (auto &[instanceId, instance] : otherCell.instances) {
                if (instance.symbol != symbolId)
                    continue;
                auto parameter =
                    std::find_if(instance.parameters.begin(), instance.parameters.end(),
                                 [&](const Parameter &candidate) { return lower(candidate.name) == lower(name); });
                if (parameter == instance.parameters.end())
                    instance.parameters.push_back({project.nextId(), name, value});
                else if (existingParameter && parameter->value == previousValue)
                    parameter->value = value;
            }
        }
    }
}

bool removeSchematicCellParameter(Project &project, ObjectId cellId, const std::string &name) {
    auto &cell = require(project.schematicCells, cellId, "schematic.cell", "Schematic cell does not exist");
    const auto before = cell.parameters.size();
    std::erase_if(cell.parameters, [&](const Parameter &parameter) { return lower(parameter.name) == lower(name); });
    return cell.parameters.size() != before;
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

ObjectId addTextSpiceInstance(Project &project, ObjectId parentCellId, ObjectId sourceCellId, std::string instanceName,
                              Coordinate x, Coordinate y, ObjectId *symbolId) {
    if (instanceName.empty())
        throw ProjectError("schematic.instance", "Instance name must not be empty");
    const auto &parent =
        require(project.schematicCells, parentCellId, "schematic.cell", "Schematic cell does not exist");
    if (parent.isTextSource())
        throw ProjectError("schematic.spice.parent", "A SPICE block can only be placed in a drawn schematic");
    const auto &source =
        require(project.schematicCells, sourceCellId, "schematic.spice.source", "SPICE source cell does not exist");
    if (!source.textSchematic)
        throw ProjectError("schematic.spice.source", "Source cell is not a text SPICE cell");
    const auto interface = spiceInterfaceForCell(source);
    if (!interface.ok)
        throw ProjectError("schematic.spice.parse", interface.error);
    if (interface.ports.empty())
        throw ProjectError("schematic.spice.ports", "SPICE subcircuit has no ports");

    const auto symbol = addHierarchicalSymbol(project, parentCellId, sourceCellId);
    const auto instance = addSchematicInstance(project, parentCellId, symbol, std::move(instanceName), x, y);
    if (symbolId)
        *symbolId = symbol;
    return instance;
}

std::string schematicNetNodeName(const SchematicCell &cell, const Net &net) {
    if (net.name == "0")
        return "0";
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
                                std::string stack, std::vector<Parameter> parameters) {
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
    project.layoutCells.at(generatedCell).readOnly = true;
    auto nextParameterId = project.nextId();
    for (auto &parameter : parameters) {
        if (parameter.id == 0)
            parameter.id = nextParameterId++;
    }
    ProceduralLayout layout;
    layout.id = id;
    layout.name = std::move(name);
    layout.pdk = std::move(pdk);
    layout.stack = std::move(stack);
    layout.script = std::move(script);
    layout.generatedCell = generatedCell;
    layout.parameters = std::move(parameters);
    project.proceduralLayouts.emplace(id, std::move(layout));
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
        PlannedDevice planned;
        planned.id = deviceId;
        planned.name = std::move(deviceName);
        planned.schematicInstance = instance->id;
        planned.x = placement.x + offsetX;
        planned.y = placement.y + offsetY;
        planned.width = boxWidth;
        planned.height = boxHeight;
        if (symbol != schematic.symbols.end()) {
            planned.device = symbolTypeName(symbol->second.name);
            planned.model = parameterNamed(*instance, {"model", "spice_model", "subckt"});
            for (const auto &parameter : instance->parameters) {
                const auto name = lower(parameter.name);
                if (name == "model" || name == "spice_model" || name == "subckt")
                    continue;
                planned.parameters.push_back(parameter);
            }
        }
        layout.plannedDevices.emplace(deviceId, std::move(planned));
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
        for (const auto &endpoint : endpoints) {
            if (endpoint.pin.empty())
                continue;
            if (auto planned = layout.plannedDevices.find(endpoint.device); planned != layout.plannedDevices.end())
                planned->second.terminals[endpoint.pin] = layoutNetId;
        }
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
                  std::string stack, Coordinate minimumWidth) {
    if (name.empty())
        throw ProjectError("layout.layer", "Layer name must not be empty");
    if (number < 0)
        throw ProjectError("layout.layer", "Layer number must not be negative");
    if (datatype < 0 || datatype > 65535)
        throw ProjectError("layout.layer", "Layer datatype out of GDSII range");
    if (minimumWidth < 0)
        throw ProjectError("layout.layer", "Layer minimum width must not be negative");
    if (stack.find_first_of("/\\") != std::string::npos)
        throw ProjectError("layout.stack", "Layer stack must be a valid stack identifier");
    ObjectId id = project.nextId();
    Layer layer{id, std::move(name), number, true, layout, std::move(pdk), datatype};
    layer.stack = std::move(stack);
    layer.minimumWidth = minimumWidth;
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
    if (port.layer == 0 || !project.layers.contains(port.layer))
        throw ProjectError("layout.layer", "Port layer does not exist");
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

namespace {
Polygon flattenedPathSegment(const LayoutPoint &start, const LayoutPoint &end, Coordinate width,
                             Coordinate startExtension, Coordinate endExtension) {
    const double dx = static_cast<double>(end.x) - static_cast<double>(start.x);
    const double dy = static_cast<double>(end.y) - static_cast<double>(start.y);
    const double length = std::hypot(dx, dy);
    if (length <= 0.0 || width <= 0)
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
        return Point{static_cast<Coordinate>(std::llround(base.x + nx * half * normalSign)),
                     static_cast<Coordinate>(std::llround(base.y + ny * half * normalSign))};
    };
    return {point(extendedStart, 1.0), point(extendedEnd, 1.0), point(extendedEnd, -1.0), point(extendedStart, -1.0)};
}
} // namespace

std::vector<ObjectId> flattenLayoutPath(Project &project, ObjectId cellId, ObjectId shapeId) {
    auto &cell = require(project.layoutCells, cellId, "layout.cell", "Layout cell does not exist");
    auto shapeIt = cell.shapes.find(shapeId);
    if (shapeIt == cell.shapes.end())
        throw ProjectError("layout.shape", "Shape does not exist");
    const auto &path = shapeIt->second;
    if (!path.isPath())
        throw ProjectError("layout.flatten", "Only path shapes can be flattened");

    const auto requireLayer = [&](ObjectId layerId) {
        if (!project.layers.contains(layerId))
            throw ProjectError("layout.flatten", "Path references a layer that does not exist");
    };
    const auto segmentCount = path.centerline.size() - 1;
    // A join is filled as if both segments were extended by half the larger
    // width. For right-angle routes this reproduces the mitered corner shown by
    // the canvas, and it also keeps the copper reaching the far edge of a via.
    const auto pointExtent = [&](std::size_t index) {
        Coordinate extent = 0;
        if (index > 0 && index - 1 < segmentCount)
            extent = std::max(extent, path.widthForSegment(index - 1));
        if (index < segmentCount)
            extent = std::max(extent, path.widthForSegment(index));
        return extent > 0 ? extent : path.width;
    };

    std::map<ObjectId, MultiPolygon> layerPolygons;
    for (std::size_t index = 0; index < segmentCount; ++index) {
        const auto width = path.widthForSegment(index);
        if (width <= 0)
            throw ProjectError("layout.flatten", "Path segment has no width");
        const auto layerId = path.layerForSegment(index);
        requireLayer(layerId);
        const auto startExtension = index == 0 ? 0 : pointExtent(index) / 2;
        const auto endExtension = index + 1 == segmentCount ? 0 : pointExtent(index + 1) / 2;
        auto polygon = flattenedPathSegment(path.centerline[index], path.centerline[index + 1], width, startExtension,
                                            endExtension);
        if (polygon.size() >= 3)
            layerPolygons[layerId].push_back(std::move(polygon));
    }
    for (std::size_t index = 0; index < path.pointVias.size() && index < path.centerline.size(); ++index) {
        if (path.pointVias[index].empty())
            continue;
        const auto half = pointExtent(index) / 2;
        if (half <= 0)
            continue;
        const auto &point = path.centerline[index];
        const Polygon square{{point.x - half, point.y - half},
                             {point.x + half, point.y - half},
                             {point.x + half, point.y + half},
                             {point.x - half, point.y + half}};
        std::set<ObjectId> seenLayers;
        for (const auto viaLayer : path.pointVias[index]) {
            requireLayer(viaLayer);
            if (seenLayers.insert(viaLayer).second)
                layerPolygons[viaLayer].push_back(square);
        }
    }
    if (layerPolygons.empty())
        throw ProjectError("layout.flatten", "Path does not contain flattenable geometry");

    struct FlattenedPolygon {
        ObjectId layer = 0;
        Polygon polygon;
    };
    GeometryKernel kernel;
    std::vector<FlattenedPolygon> flattened;
    for (auto &[layerId, polygons] : layerPolygons) {
        auto merged = kernel.booleanOp(polygons, {}, BooleanOperation::union_);
        merged = kernel.cleanup(merged);
        for (auto &polygon : merged) {
            if (polygon.size() >= 3)
                flattened.push_back({layerId, std::move(polygon)});
        }
    }
    if (flattened.empty())
        throw ProjectError("layout.flatten", "Path does not contain flattenable geometry");

    const auto net = path.net;
    const auto routedConnection = path.routedConnection;
    cell.shapes.erase(shapeIt);
    if (routedConnection != 0) {
        if (auto connection = cell.plannedConnections.find(routedConnection);
            connection != cell.plannedConnections.end())
            connection->second.routed = false;
    }

    std::vector<ObjectId> resultIds;
    resultIds.reserve(flattened.size());
    for (auto &entry : flattened) {
        const auto id = project.nextId();
        const auto bounds = GeometryKernel::bounds(entry.polygon);
        std::vector<LayoutPoint> points;
        points.reserve(entry.polygon.size());
        for (const auto &point : entry.polygon)
            points.push_back({point.x, point.y});
        Shape shape{id, entry.layer, net, bounds, std::move(points)};
        shape.name = nextShapeName(cell, "Polygon");
        cell.shapes.emplace(id, std::move(shape));
        resultIds.push_back(id);
    }
    return resultIds;
}

namespace {
std::vector<std::string> buildSchematicNetlistImpl(const Project &project, ObjectId cellId,
                                                   std::vector<std::string> *notes, SchematicNetlistMode mode,
                                                   double geometryScale, bool flattenHierarchy,
                                                   std::vector<SchematicNetlistBlock> *blocks,
                                                   std::map<std::string, std::string> *voltageSourceDrivers) {
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
    const auto portInstances = [&](const SchematicCell &cell) { return schematicPortInstances(cell); };

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

    // Cell parameters become SPICE subcircuit defaults. Components reference
    // them as `{name}` values, and a hierarchical instance may override them
    // with its own instance parameters of the same name.
    struct CellParameter {
        std::string name;
        std::string value;
    };
    std::map<ObjectId, std::vector<CellParameter>> parameterCache;
    const auto parametersFor = [&](const SchematicCell &cell) -> const std::vector<CellParameter> & {
        if (const auto cached = parameterCache.find(cell.id); cached != parameterCache.end())
            return cached->second;
        std::vector<CellParameter> parameters;
        std::set<std::string> seen;
        for (const auto &parameter : cell.parameters) {
            if (parameter.name.empty()) {
                note("schematic '" + cell.name + "' has a parameter with an empty name; it is ignored");
                continue;
            }
            auto name = spiceIdentifier(parameter.name);
            if (name != parameter.name)
                note("schematic parameter '" + parameter.name + "' is not a valid SPICE name; using '" + name + "'");
            if (!seen.insert(lower(name)).second) {
                note("schematic '" + cell.name + "' declares duplicate parameter '" + name +
                     "'; the first value is used");
                continue;
            }
            if (parameter.value.empty()) {
                note("schematic parameter '" + name + "' has no default value; its components cannot resolve {" + name +
                     "} unless an enclosing scope defines it");
                continue;
            }
            parameters.push_back({std::move(name), parameter.value});
        }
        return parameterCache.emplace(cell.id, std::move(parameters)).first->second;
    };
    const auto instanceParameterValue = [&](const SchematicInstance &instance, const std::string &name) {
        for (const auto &parameter : instance.parameters) {
            if (lower(parameter.name) == lower(name) || lower(spiceIdentifier(parameter.name)) == lower(name))
                return parameter.value;
        }
        return std::string{};
    };

    std::ostringstream out;
    out << "* ICE schematic netlist: " << topCell.name << "\n";
    std::ostringstream subcircuits;
    std::set<ObjectId> emittedSubcircuits;
    std::set<ObjectId> emittedTextSources;
    std::map<ObjectId, SpiceInterface> textInterfaces;
    std::set<ObjectId> visiting;
    std::function<std::string(const SchematicCell &, const std::map<ObjectId, std::string> &, bool, const std::string &,
                              const std::string &)>
        emitBody;
    std::function<void(ObjectId)> emitSubcircuit;

    emitBody = [&](const SchematicCell &cell, const std::map<ObjectId, std::string> &portNodeNames, bool topLevel,
                   const std::string &instancePrefix, const std::string &nodePrefix) {
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
            if (net.name == "0")
                return "0";
            if (!net.name.empty())
                return nodePrefix + net.name;
            return nodePrefix + "n" + std::to_string(net.id);
        };

        std::ostringstream body;
        for (const auto &[instanceId, instance] : cell.instances) {
            const auto symbol = cell.symbols.find(instance.symbol);
            if (symbol == cell.symbols.end())
                continue;
            if (schematicPortKindForSymbol(symbol->second.name) != SchematicPortKind::none)
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
                    if (flattenHierarchy) {
                        if (symbolUseCount[instance.symbol] > 1) {
                            throw ProjectError("schematic.netlist.ambiguous_symbol",
                                               "Symbol '" + symbol->second.name +
                                                   "' is instantiated more than once; create one HDL symbol per "
                                                   "instance");
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
                        if (blocks) {
                            SchematicNetlistBlock block;
                            block.instanceId = instanceId;
                            block.symbolId = instance.symbol;
                            block.sourceCell = child->first;
                            block.path = instancePrefix + instance.name;
                            for (const auto &pin : symbol->second.pins) {
                                block.pinNames.push_back(pin.name);
                                if (const auto node = pinToNode.find(pin.id); node != pinToNode.end())
                                    block.pinNodes[pin.name] = node->second;
                            }
                            blocks->push_back(std::move(block));
                        }
                        body << "* SystemVerilog block: " << instancePrefix << instance.name << " "
                             << child->second.name << "\n";
                        continue;
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
                    auto interface = textInterfaces.find(child->first);
                    if (interface == textInterfaces.end())
                        interface = textInterfaces.emplace(child->first, spiceInterfaceForCell(child->second)).first;
                    if (!interface->second.ok) {
                        throw ProjectError("schematic.netlist.spice",
                                           "Text SPICE schematic '" + child->second.name + "' (" + instance.name +
                                               ") has no usable subcircuit: " + interface->second.error);
                    }
                    if (emittedTextSources.insert(child->first).second) {
                        std::vector<std::string> includeWarnings;
                        for (const auto &line : spiceSourceLinesForHierarchy(
                                 child->second.spiceText, child->second.spiceSourcePath, &includeWarnings))
                            subcircuits << line << "\n";
                        for (const auto &warning : includeWarnings)
                            note(warning);
                    }
                    if (symbolUseCount[instance.symbol] > 1) {
                        throw ProjectError("schematic.netlist.ambiguous_symbol",
                                           "Symbol '" + symbol->second.name +
                                               "' is instantiated more than once; create one symbol per instance");
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

                    std::string designator = instancePrefix + instance.name;
                    if (designator.empty() || std::toupper(static_cast<unsigned char>(designator.front())) != 'X')
                        designator.insert(designator.begin(), 'X');
                    body << designator << " ";
                    bool matchedByName = !interface->second.ports.empty();
                    std::size_t portIndex = 0;
                    for (const auto &port : interface->second.ports) {
                        const Pin *connection = nullptr;
                        for (const auto &pin : symbol->second.pins) {
                            if (lower(pin.name) == lower(port)) {
                                connection = &pin;
                                break;
                            }
                        }
                        if (connection == nullptr && portIndex < symbol->second.pins.size()) {
                            connection = &symbol->second.pins[portIndex];
                            matchedByName = false;
                        }
                        if (connection != nullptr) {
                            if (lower(connection->name) != lower(port))
                                matchedByName = false;
                            const auto node = pinToNode.find(connection->id);
                            body << (node != pinToNode.end() ? node->second : "0") << " ";
                        } else {
                            body << "0 ";
                        }
                        ++portIndex;
                    }
                    if (!matchedByName || symbol->second.pins.size() != interface->second.ports.size())
                        throw ProjectError("schematic.netlist.port_mismatch",
                                           "SPICE block symbol '" + symbol->second.name +
                                               "' pins do not match the ports of '" + child->second.name +
                                               "'. Edit the source cell again or recreate the hierarchical symbol.");
                    body << interface->second.subcircuitName;
                    std::set<std::string> declaredNames;
                    for (const auto &parameter : interface->second.parameters) {
                        declaredNames.insert(lower(parameter.name));
                        const auto overrideValue = instanceParameterValue(instance, parameter.name);
                        if (!overrideValue.empty())
                            body << " " << parameter.name << "=" << overrideValue;
                    }
                    for (const auto &parameter : instance.parameters) {
                        if (parameter.name.empty() || declaredNames.contains(lower(parameter.name)))
                            continue;
                        note("SPICE instance '" + instance.name + "' parameter '" + parameter.name +
                             "' is not declared by subcircuit '" + interface->second.subcircuitName +
                             "'; it is ignored");
                    }
                    body << "\n";
                    continue;
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
                if (flattenHierarchy) {
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

                    const auto childPorts = schematicPortInstances(child->second);
                    std::map<ObjectId, std::string> childPortMap;
                    bool matchedByName = !childPorts.empty();
                    std::size_t portIndex = 0;
                    for (const auto *port : childPorts) {
                        const Pin *connection = nullptr;
                        for (const auto &pin : symbol->second.pins) {
                            if (lower(pin.name) == lower(port->name)) {
                                connection = &pin;
                                break;
                            }
                        }
                        if (connection == nullptr && portIndex < symbol->second.pins.size()) {
                            connection = &symbol->second.pins[portIndex];
                            matchedByName = false;
                        }
                        std::string portNode = "0";
                        if (connection != nullptr) {
                            if (lower(connection->name) != lower(port->name))
                                matchedByName = false;
                            if (const auto node = pinToNode.find(connection->id); node != pinToNode.end())
                                portNode = node->second;
                        }
                        const auto portSymbol = child->second.symbols.find(port->symbol);
                        if (portSymbol != child->second.symbols.end() && !portSymbol->second.pins.empty()) {
                            const auto portPin = portSymbol->second.pins.front().id;
                            for (const auto &[netId, net] : child->second.nets) {
                                if (std::find(net.pins.begin(), net.pins.end(), portPin) != net.pins.end()) {
                                    childPortMap[netId] = portNode;
                                    break;
                                }
                            }
                        }
                        ++portIndex;
                    }
                    if (!matchedByName || symbol->second.pins.size() != childPorts.size())
                        throw ProjectError("schematic.netlist.port_mismatch",
                                           "Hierarchical symbol '" + symbol->second.name +
                                               "' pins do not match the ports of '" + child->second.name +
                                               "'. Edit the source cell again or recreate the hierarchical symbol.");
                    const auto scope = spiceIdentifier(instance.name);
                    visiting.insert(child->first);
                    body << emitBody(child->second, childPortMap, false, instancePrefix + scope + "_",
                                     nodePrefix + scope + "_");
                    visiting.erase(child->first);
                    continue;
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

                std::string designator = instancePrefix + instance.name;
                if (designator.empty() || std::toupper(static_cast<unsigned char>(designator.front())) != 'X')
                    designator.insert(designator.begin(), 'X');
                body << designator << " ";
                // The `.subckt` port list follows the child's port instances
                // (inputs top-down, then outputs). A symbol may declare its own
                // pins in any order, so map every port to the parent pin with
                // the same name instead of trusting the declaration order;
                // fall back to position for hand-made or renamed symbols.
                const auto childPorts = schematicPortInstances(child->second);
                bool matchedByName = !childPorts.empty();
                std::size_t portIndex = 0;
                for (const auto *port : childPorts) {
                    const Pin *connection = nullptr;
                    for (const auto &pin : symbol->second.pins) {
                        if (lower(pin.name) == lower(port->name)) {
                            connection = &pin;
                            break;
                        }
                    }
                    if (connection == nullptr && portIndex < symbol->second.pins.size()) {
                        connection = &symbol->second.pins[portIndex];
                        matchedByName = false;
                    }
                    if (connection != nullptr) {
                        if (lower(connection->name) != lower(port->name))
                            matchedByName = false;
                        const auto node = pinToNode.find(connection->id);
                        body << (node != pinToNode.end() ? node->second : "0") << " ";
                    } else {
                        body << "0 ";
                    }
                    ++portIndex;
                }
                if (!matchedByName || symbol->second.pins.size() != childPorts.size())
                    throw ProjectError("schematic.netlist.port_mismatch",
                                       "Hierarchical symbol '" + symbol->second.name +
                                           "' pins do not match the ports of '" + child->second.name +
                                           "'. Edit the source cell again or recreate the hierarchical symbol.");
                body << spiceIdentifier(child->second.name);
                // Pass matching instance parameters as subcircuit overrides.
                const auto &childParameters = parametersFor(child->second);
                std::set<std::string> declaredNames;
                for (const auto &parameter : childParameters) {
                    declaredNames.insert(lower(parameter.name));
                    const auto overrideValue = instanceParameterValue(instance, parameter.name);
                    if (!overrideValue.empty())
                        body << " " << parameter.name << "=" << overrideValue;
                }
                for (const auto &parameter : instance.parameters) {
                    if (parameter.name.empty() || declaredNames.contains(lower(parameter.name)) ||
                        declaredNames.contains(lower(spiceIdentifier(parameter.name))))
                        continue;
                    note("hierarchical instance '" + instance.name + "' parameter '" + parameter.name +
                         "' is not declared by schematic '" + child->second.name + "'; it is ignored");
                }
                body << "\n";
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
            // First connected pin out of several aliases ("S", "SUB", ...).
            const auto firstConnectedNode =
                [&](std::initializer_list<std::string_view> names) -> std::optional<std::string> {
                for (const auto name : names) {
                    if (const auto node = pinNameToNode.find(lower(std::string(name))); node != pinNameToNode.end())
                        return node->second;
                }
                return std::nullopt;
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
                    // PDK bipolar subcircuits differ in whether they expose the
                    // substrate as a fourth node (sky130's NPN does, its
                    // substrate PNP does not). Read the node list from the
                    // package model file and match it exactly.
                    const auto declared = pdkSubcircuitNodes(cell.pdk, configuredModel);
                    const auto expected = declared ? declared->size() : 0;
                    const auto substrate = firstConnectedNode({"S", "SUB", "BN", "SUBSTRATE"});
                    if (expected == 4) {
                        nodes.push_back(substrate ? *substrate : "0");
                        if (!substrate)
                            note("BJT symbol '" + symbol->second.name + "' has no substrate pin; '" + configuredModel +
                                 "' expects one, so the substrate is grounded");
                    } else if (expected == 3) {
                        if (substrate)
                            note("BJT symbol '" + symbol->second.name + "' declares a substrate pin but '" +
                                 configuredModel + "' has three nodes; the pin is left unconnected");
                    } else if (substrate) {
                        nodes.push_back(*substrate);
                    } else {
                        note("BJT symbol '" + symbol->second.name + "' has no substrate pin; instantiating '" +
                             configuredModel + "' with collector/base/emitter only");
                    }
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
            std::string designator = instancePrefix + instance.name;
            const auto prefix = static_cast<char>(std::toupper(static_cast<unsigned char>(lineElement)));
            if (designator.empty() ||
                static_cast<char>(std::toupper(static_cast<unsigned char>(designator.front()))) != prefix)
                designator.insert(designator.begin(), prefix);
            if (element == 'V' && voltageSourceDrivers) {
                for (const auto &node : nodes)
                    (*voltageSourceDrivers)[node] = designator;
            }

            const auto appendAttribute = [&](std::string_view label, std::initializer_list<std::string_view> names) {
                const auto value = parameterNamed(instance, names);
                if (!value.empty())
                    body << " " << label << "=" << value;
            };
            // Device geometry is emitted in the units the PDK preamble selects
            // with `.option scale=...`. Values written with an SI suffix ("1u")
            // are converted to those units; bare numbers are already expressed in
            // them (SKY130 writes "60" for 60 um) and stay unchanged.
            const auto appendLengthAttribute = [&](std::string_view label,
                                                   std::initializer_list<std::string_view> names) {
                const auto value = parameterNamed(instance, names);
                if (value.empty())
                    return;
                if (!(geometryScale > 0.0) || geometryScale == 1.0) {
                    body << " " << label << "=" << value;
                    return;
                }
                const auto scaled = scaledNetlistLength(value, geometryScale);
                if (!scaled) {
                    body << " " << label << "=" << value;
                    return;
                }
                std::ostringstream formatted;
                formatted << std::setprecision(12) << *scaled;
                body << " " << label << "=" << formatted.str();
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
                    appendLengthAttribute("W", {"w", "width"});
                    appendLengthAttribute("L", {"l", "length"});
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
        const auto body = emitBody(child->second, portNodeNames, false, {}, {});

        std::ostringstream definition;
        definition << ".subckt " << spiceIdentifier(child->second.name);
        for (const auto *port : ports)
            definition << " " << spiceIdentifier(port->name);
        const auto &childParameters = parametersFor(child->second);
        if (!childParameters.empty()) {
            definition << " params:";
            for (const auto &parameter : childParameters)
                definition << " " << parameter.name << "=" << parameter.value;
        }
        definition << "\n" << body << ".ends " << spiceIdentifier(child->second.name) << "\n";
        visiting.erase(childId);
        emittedSubcircuits.insert(childId);
        subcircuits << definition.str();
    };

    visiting.insert(cellId);
    const auto topBody = emitBody(topCell, {}, true, {}, {});
    visiting.erase(cellId);
    std::ostringstream topParameters;
    for (const auto &parameter : parametersFor(topCell))
        topParameters << ".param " << parameter.name << "=" << parameter.value << "\n";
    out << topParameters.str() << subcircuits.str() << topBody;

    std::vector<std::string> lines;
    std::istringstream stream(out.str());
    std::string line;
    while (std::getline(stream, line))
        if (!line.empty())
            lines.push_back(line);
    return lines;
}
} // namespace

std::vector<std::string> buildSchematicNetlist(const Project &project, ObjectId cellId, std::vector<std::string> *notes,
                                               SchematicNetlistMode mode, double geometryScale) {
    return buildSchematicNetlistImpl(project, cellId, notes, mode, geometryScale, false, nullptr, nullptr);
}

SchematicNetlistBuild buildMixedSignalSchematicNetlist(const Project &project, ObjectId cellId, double geometryScale) {
    SchematicNetlistBuild result;
    result.lines = buildSchematicNetlistImpl(project, cellId, &result.notes, SchematicNetlistMode::mixed, geometryScale,
                                             true, &result.blocks, &result.voltageSourceDrivers);
    return result;
}

std::vector<std::string> buildSimulationNetlist(const Project &project, ObjectId cellId,
                                                std::vector<std::string> *notes) {
    const auto &cell = require(project.schematicCells, cellId, "schematic.cell", "Schematic cell does not exist");
    if (cell.systemVerilogSchematic)
        throw ProjectError("simulation.hdl", "SystemVerilog cells must be simulated with the iverilog simulator");
    if (!cell.textSchematic)
        return buildSchematicNetlist(project, cellId, notes, SchematicNetlistMode::analog,
                                     pdkNetlistGeometryScale(cell.pdk));

    std::vector<std::string> includeWarnings;
    const auto rewritten = cell.spiceSourcePath.empty()
                               ? cell.spiceText
                               : rewriteSpiceIncludePaths(cell.spiceText, cell.spiceSourcePath, &includeWarnings);
    if (notes)
        notes->insert(notes->end(), includeWarnings.begin(), includeWarnings.end());
    std::vector<std::string> lines;
    std::istringstream stream(rewritten);
    std::string line;
    while (std::getline(stream, line)) {
        const auto trimmed = trimLine(line);
        if (trimmed.empty() || isOwnedAnalysisDirective(trimmed))
            continue;
        lines.push_back(trimmed);
    }
    return lines;
}

namespace {
std::string lowerCopy(const std::string &text) {
    std::string result = text;
    for (auto &character : result)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return result;
}
} // namespace

void requireUniqueSimulationName(const Project &project, const std::string &name, ObjectId except) {
    const auto wanted = lowerCopy(name);
    for (const auto &[id, existing] : project.simulations) {
        if (id == except)
            continue;
        if (lowerCopy(existing.name) == wanted)
            throw ProjectError("simulation.name",
                               "A simulation named '" + name + "' already exists; choose a different name");
    }
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
    auto updated = graph;
    updated.id = graphId;
    *found = std::move(updated);
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
    if (cell->second.library || (cell->second.readOnly && !cell->second.reference))
        throw ProjectError("layout.remove.library", "Read-only layout cells cannot be removed");
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
