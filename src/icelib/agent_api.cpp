#include "icelib/agent_api.hpp"

#include "icelib/hdl.hpp"
#include "icelib/operations.hpp"
#include "icelib/pdk.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <sstream>

namespace ice {
namespace {

template <typename Map> auto &require(Map &map, ObjectId id, const char *code, const char *message) {
    auto it = map.find(id);
    if (it == map.end())
        throw ProjectError(code, message);
    return it->second;
}

std::string lowerCopy(std::string text) {
    for (auto &character : text)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return text;
}

std::string trimCopy(std::string text) {
    const auto notSpace = [](unsigned char character) { return !std::isspace(character); };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), notSpace));
    text.erase(std::find_if(text.rbegin(), text.rend(), notSpace).base(), text.end());
    return text;
}

std::string normalizedType(std::string text) {
    text = lowerCopy(trimCopy(std::move(text)));
    for (auto &character : text) {
        if (character == '-' || character == '_')
            character = ' ';
    }
    return text;
}

std::vector<std::string> words(const std::string &text) {
    std::istringstream stream(text);
    std::vector<std::string> result;
    std::string word;
    while (stream >> word)
        result.push_back(std::move(word));
    return result;
}

bool hasParameter(const SchematicInstance &instance, std::initializer_list<const char *> names) {
    for (const auto &parameter : instance.parameters) {
        const auto name = lowerCopy(parameter.name);
        for (const auto *candidate : names) {
            if (name == lowerCopy(candidate))
                return true;
        }
    }
    return false;
}

std::string instancePrefix(const SchematicCell &cell, const std::string &symbolName) {
    const auto lower = lowerCopy(symbolName);
    if (lower == "input")
        return "IN";
    if (lower == "output")
        return "OUT";
    char prefix = spiceElementForSymbol(symbolName);
    if (prefix == 0)
        prefix = 'X';
    if (!cell.pdk.empty() && cell.pdk != genericPdkId && (prefix == 'M' || prefix == 'Q'))
        prefix = 'X';
    return std::string(1, prefix);
}

std::string nextInstanceName(const SchematicCell &cell, const std::string &symbolName, const std::string &requested) {
    if (!requested.empty()) {
        const auto duplicate = std::any_of(cell.instances.begin(), cell.instances.end(),
                                           [&](const auto &entry) { return entry.second.name == requested; });
        if (duplicate)
            throw ProjectError("schematic.instance.name", "Instance name already exists: " + requested);
        return requested;
    }
    const auto prefix = instancePrefix(cell, symbolName);
    for (std::size_t index = 1;; ++index) {
        const auto candidate = prefix + std::to_string(index);
        const auto duplicate = std::any_of(cell.instances.begin(), cell.instances.end(),
                                           [&](const auto &entry) { return entry.second.name == candidate; });
        if (!duplicate)
            return candidate;
    }
}

const FileSymbol *findComponentSymbol(const std::vector<FileSymbol> &symbols, const ComponentRequest &request) {
    const auto requested = trimCopy(request.symbol.empty() ? request.type : request.symbol);
    if (requested.empty())
        throw ProjectError("schematic.component.type", "Component type or symbol name is required");
    const auto normalized = normalizedType(requested);

    const auto exact = std::find_if(symbols.begin(), symbols.end(), [&](const FileSymbol &symbol) {
        return lowerCopy(symbol.name) == lowerCopy(requested);
    });
    if (exact != symbols.end())
        return &*exact;

    if (!request.symbol.empty()) {
        const auto contains = std::find_if(symbols.begin(), symbols.end(), [&](const FileSymbol &symbol) {
            return lowerCopy(symbol.name).find(lowerCopy(requested)) != std::string::npos;
        });
        if (contains != symbols.end())
            return &*contains;
        throw ProjectError("schematic.component.symbol", "Unknown symbol: " + requested);
    }

    static const std::map<std::string, std::string> aliases = {
        {"r", "resistor"},
        {"res", "resistor"},
        {"resistor", "resistor"},
        {"c", "capacitor"},
        {"cap", "capacitor"},
        {"capacitor", "capacitor"},
        {"l", "inductor"},
        {"inductor", "inductor"},
        {"d", "diode"},
        {"diode", "diode"},
        {"v", "voltage source"},
        {"vsource", "voltage source"},
        {"vdd", "voltage source"},
        {"vcc", "voltage source"},
        {"vsup", "voltage source"},
        {"supply", "voltage source"},
        {"voltage source", "voltage source"},
        {"voltage", "voltage source"},
        {"i", "current source"},
        {"idc", "current source"},
        {"iref", "current source"},
        {"isource", "current source"},
        {"current source", "current source"},
        {"current", "current source"},
        {"g", "ground"},
        {"gnd", "ground"},
        {"ground", "ground"},
        {"nmos", "nmos"},
        {"nfet", "nmos"},
        {"pmos", "pmos"},
        {"pfet", "pmos"},
        {"opamp", "op amp"},
        {"op amp", "op amp"},
        {"pulse", "pulse voltage source"},
        {"pulse source", "pulse voltage source"},
        {"pulse voltage source", "pulse voltage source"},
        {"sine", "sine voltage source"},
        {"sine source", "sine voltage source"},
        {"sine voltage source", "sine voltage source"},
        {"input", "input"},
        {"port input", "input"},
        {"output", "output"},
        {"port output", "output"},
    };
    const auto alias = aliases.find(normalized);
    const auto wanted = alias != aliases.end() ? alias->second : normalized;
    auto match = std::find_if(symbols.begin(), symbols.end(),
                              [&](const FileSymbol &symbol) { return lowerCopy(symbol.name) == wanted; });
    if (match == symbols.end()) {
        match = std::find_if(symbols.begin(), symbols.end(), [&](const FileSymbol &symbol) {
            const auto name = lowerCopy(symbol.name);
            return name.find(wanted) != std::string::npos || wanted.find(name) != std::string::npos;
        });
    }
    if (match != symbols.end())
        return &*match;

    std::string available;
    for (std::size_t index = 0; index < symbols.size() && index < 24; ++index) {
        if (!available.empty())
            available += ", ";
        available += symbols[index].name;
    }
    throw ProjectError("schematic.component.type",
                       "Unknown component type '" + requested + "'. Available symbols include: " + available);
}

Symbol makeSymbolFromFile(Project &project, const FileSymbol &fileSymbol, std::string name) {
    auto next = project.nextId();
    Symbol symbol;
    symbol.id = next++;
    symbol.name = std::move(name);
    symbol.artwork = fileSymbol.svg;
    symbol.label = fileSymbol.label;
    for (auto pin : fileSymbol.pins) {
        pin.id = next++;
        symbol.pins.push_back(std::move(pin));
    }
    return symbol;
}

std::string uniqueSymbolName(const SchematicCell &cell, const std::string &base) {
    auto name = base;
    const auto taken = [&](const std::string &candidate) {
        return std::any_of(cell.symbols.begin(), cell.symbols.end(),
                           [&](const auto &entry) { return entry.second.name == candidate; });
    };
    for (int suffix = 2; taken(name); ++suffix)
        name = base + " " + std::to_string(suffix);
    return name;
}

void addDiagnostic(std::vector<Diagnostic> &diagnostics, Diagnostic::Severity severity, std::string code,
                   std::string message, std::optional<ObjectId> object = std::nullopt) {
    diagnostics.push_back({severity, std::move(code), std::move(message), object});
}

} // namespace

ObjectId resolveSchematicPin(const SchematicCell &cell, const std::string &text) {
    if (text.empty())
        throw ProjectError("schematic.pin.name", "Pin reference must not be empty");
    if (std::all_of(text.begin(), text.end(), [](unsigned char character) { return std::isdigit(character) != 0; })) {
        try {
            const auto id = static_cast<ObjectId>(std::stoull(text));
            for (const auto &[_, symbol] : cell.symbols) {
                if (std::any_of(symbol.pins.begin(), symbol.pins.end(), [&](const Pin &pin) { return pin.id == id; }))
                    return id;
            }
        } catch (const std::exception &) {
        }
    }

    const auto dot = text.find('.');
    if (dot != std::string::npos) {
        const auto owner = text.substr(0, dot);
        const auto pinName = text.substr(dot + 1);
        for (const auto &[_, instance] : cell.instances) {
            if (instance.name != owner)
                continue;
            const auto symbol = cell.symbols.find(instance.symbol);
            if (symbol == cell.symbols.end())
                continue;
            for (const auto &pin : symbol->second.pins) {
                if (pin.name == pinName)
                    return pin.id;
            }
        }
        for (const auto &[_, symbol] : cell.symbols) {
            if (symbol.name != owner)
                continue;
            for (const auto &pin : symbol.pins) {
                if (pin.name == pinName)
                    return pin.id;
            }
        }
    }

    std::optional<ObjectId> found;
    for (const auto &[_, symbol] : cell.symbols) {
        for (const auto &pin : symbol.pins) {
            if (pin.name != text)
                continue;
            if (found)
                throw ProjectError("schematic.pin.ambiguous",
                                   "Pin name is ambiguous; use <instance>.<pin> or <symbol>.<pin>: " + text);
            found = pin.id;
        }
    }
    if (!found)
        throw ProjectError("schematic.pin.unknown", "Unknown pin: " + text);
    return *found;
}

ComponentPlacement addSchematicComponent(Project &project, ObjectId cellId, const ComponentRequest &request) {
    auto &cell = require(project.schematicCells, cellId, "schematic.cell", "Schematic cell does not exist");
    if (cell.isTextSource())
        throw ProjectError("schematic.component.source", "Components can only be added to a drawn schematic");
    const auto symbols = availableSymbols(cell.pdk);
    const auto *fileSymbol = findComponentSymbol(symbols, request);
    const auto symbolName = uniqueSymbolName(cell, fileSymbol->name);
    auto symbol = makeSymbolFromFile(project, *fileSymbol, symbolName);
    const auto symbolId = symbol.id;
    const auto pins = symbol.pins;
    cell.symbols.emplace(symbolId, std::move(symbol));

    const auto instanceName = nextInstanceName(cell, fileSymbol->name, request.name);
    const auto instanceId = project.nextId();
    cell.instances.emplace(instanceId, SchematicInstance{instanceId, symbolId, instanceName, request.x, request.y});
    auto &instance = cell.instances.at(instanceId);
    seedDefaultParameters(project, instance, fileSymbol->defaultParameters);
    for (const auto &parameter : request.parameters)
        setSchematicParameter(project, cellId, instanceId, parameter.name, parameter.value);

    return {instanceId, symbolId, symbolName, instanceName, pins};
}

CircuitPlacement addSchematicCircuit(Project &project, ObjectId cellId, const CircuitRequest &request) {
    CircuitPlacement placement;
    placement.components.reserve(request.components.size());
    for (const auto &component : request.components)
        placement.components.push_back(addSchematicComponent(project, cellId, component));

    const auto &cell = require(project.schematicCells, cellId, "schematic.cell", "Schematic cell does not exist");
    placement.nets.reserve(request.nets.size());
    for (const auto &net : request.nets) {
        std::vector<ObjectId> pins;
        pins.reserve(net.pins.size());
        for (const auto &pin : net.pins)
            pins.push_back(resolveSchematicPin(cell, pin));
        if (pins.size() < 2)
            throw ProjectError("schematic.net.pins", "Net '" + net.name + "' needs at least two pins");
        placement.nets.push_back(addNet(project, cellId, net.name, pins, net.x, net.y));
    }
    return placement;
}

std::vector<Diagnostic> validateSchematic(const Project &project, ObjectId cellId) {
    std::vector<Diagnostic> diagnostics;
    const auto cell = project.schematicCells.find(cellId);
    if (cell == project.schematicCells.end()) {
        addDiagnostic(diagnostics, Diagnostic::Severity::error, "schematic.missing", "Schematic cell does not exist",
                      cellId);
        return diagnostics;
    }

    if (cell->second.pdk.empty() || cell->second.pdk.find_first_of("/\\") != std::string::npos) {
        addDiagnostic(diagnostics, Diagnostic::Severity::error, "schematic.pdk", "Schematic PDK is invalid", cellId);
    } else if (cell->second.pdk != genericPdkId) {
        const auto pdks = findPdks(cell->second.pdk);
        if (pdks.empty()) {
            addDiagnostic(diagnostics, Diagnostic::Severity::error, "schematic.pdk.missing",
                          "Schematic PDK is not installed: " + cell->second.pdk, cellId);
        } else if (!pdks.front().simulationReady) {
            addDiagnostic(diagnostics, Diagnostic::Severity::warning, "schematic.pdk.not_ready",
                          "Schematic PDK is not simulation-ready: " + cell->second.pdk, cellId);
        }
    }

    if (cell->second.systemVerilogSchematic) {
        const auto parsed = parseSystemVerilogModule(cell->second);
        if (!parsed.ok)
            addDiagnostic(diagnostics, Diagnostic::Severity::error, "schematic.hdl.parse",
                          "SystemVerilog parse failed: " + parsed.error, cellId);
        for (const auto &warning : parsed.warnings)
            addDiagnostic(diagnostics, Diagnostic::Severity::warning, "schematic.hdl.parse", warning, cellId);
        return diagnostics;
    }

    if (cell->second.textSchematic) {
        std::istringstream source(cell->second.spiceText);
        std::string line;
        while (std::getline(source, line)) {
            const auto trimmed = trimCopy(line);
            const auto lower = lowerCopy(trimmed);
            if (lower.starts_with(".include ") || lower.starts_with(".inc ") || lower.starts_with(".lib ")) {
                addDiagnostic(diagnostics, Diagnostic::Severity::warning, "schematic.spice.pdk_manual",
                              "Text schematic contains a manual model/setup line; ICE applies package PDK setup "
                              "automatically based on the cell PDK",
                              cellId);
                break;
            }
        }
        return diagnostics;
    }

    std::map<std::string, int> instanceNames;
    for (const auto &[instanceId, instance] : cell->second.instances) {
        if (instance.name.empty())
            addDiagnostic(diagnostics, Diagnostic::Severity::error, "schematic.instance.name", "Instance name is empty",
                          instanceId);
        else if (!instanceNames.emplace(instance.name, 1).second)
            addDiagnostic(diagnostics, Diagnostic::Severity::error, "schematic.instance.name.duplicate",
                          "Instance name is duplicated: " + instance.name, instanceId);
        const auto symbol = cell->second.symbols.find(instance.symbol);
        if (symbol == cell->second.symbols.end()) {
            addDiagnostic(diagnostics, Diagnostic::Severity::error, "schematic.instance.symbol",
                          "Instance references a missing symbol", instanceId);
            continue;
        }
        if (symbol->second.pins.empty()) {
            addDiagnostic(diagnostics, Diagnostic::Severity::error, "schematic.symbol.pins",
                          "Symbol '" + symbol->second.name +
                              "' has no pins; recreate it with schematic_add_component or "
                              "schematic_add_hdl_instance, or add pins explicitly",
                          symbol->first);
            continue;
        }
        if (symbol->second.artwork.empty()) {
            addDiagnostic(diagnostics, Diagnostic::Severity::warning, "schematic.symbol.artwork",
                          "Symbol '" + symbol->second.name + "' has no artwork and will render as a placeholder",
                          symbol->first);
        }
        const auto element = spiceElementForSymbol(symbol->second.name);
        const auto lowerSymbol = lowerCopy(symbol->second.name);
        if (element == 'G' || lowerSymbol == "input" || lowerSymbol == "output" || symbol->second.sourceCell != 0)
            continue;
        if (element == 'R' && !hasParameter(instance, {"resistance", "value"}))
            addDiagnostic(diagnostics, Diagnostic::Severity::warning, "schematic.component.value",
                          "Resistor '" + instance.name + "' has no resistance/value parameter", instanceId);
        else if (element == 'C' && !hasParameter(instance, {"capacitance", "value"}))
            addDiagnostic(diagnostics, Diagnostic::Severity::warning, "schematic.component.value",
                          "Capacitor '" + instance.name + "' has no capacitance/value parameter", instanceId);
        else if (element == 'L' && !hasParameter(instance, {"inductance", "value"}))
            addDiagnostic(diagnostics, Diagnostic::Severity::warning, "schematic.component.value",
                          "Inductor '" + instance.name + "' has no inductance/value parameter", instanceId);
        else if ((element == 'V' || element == 'I') &&
                 !hasParameter(instance, {"value", "voltage", "current", "amplitude", "initial_voltage",
                                          "pulsed_voltage", "offset_voltage", "waveform"}))
            addDiagnostic(diagnostics, Diagnostic::Severity::warning, "schematic.component.value",
                          "Source '" + instance.name + "' has no value or waveform parameter", instanceId);
        else if ((element == 'D' || element == 'M' || element == 'Q' || element == 'X') &&
                 !hasParameter(instance, {"model", "spice_model", "subckt"}))
            addDiagnostic(diagnostics, Diagnostic::Severity::warning, "schematic.component.model",
                          "Device '" + instance.name + "' has no model/subckt parameter", instanceId);
    }

    std::set<std::string> netNames;
    std::set<ObjectId> connectedPins;
    for (const auto &[netId, net] : cell->second.nets) {
        if (!net.name.empty() && !netNames.insert(net.name).second)
            addDiagnostic(diagnostics, Diagnostic::Severity::warning, "schematic.net.name.duplicate",
                          "Net name is duplicated: " + net.name, netId);
        if (net.pins.size() < 2)
            addDiagnostic(diagnostics, Diagnostic::Severity::warning, "schematic.net.single",
                          "Net '" + (net.name.empty() ? "n" + std::to_string(net.id) : net.name) +
                              "' has fewer than two pins",
                          netId);
        connectedPins.insert(net.pins.begin(), net.pins.end());
    }

    for (const auto &[_, instance] : cell->second.instances) {
        const auto symbol = cell->second.symbols.find(instance.symbol);
        if (symbol == cell->second.symbols.end())
            continue;
        for (const auto &pin : symbol->second.pins) {
            if (!connectedPins.contains(pin.id)) {
                addDiagnostic(diagnostics, Diagnostic::Severity::warning, "schematic.pin.unconnected",
                              "Pin '" + instance.name + "." + pin.name + "' is not connected", instance.id);
            }
        }
    }

    if (schematicContainsHdl(project, cellId)) {
        const auto hdlDiagnostics = validateHdlHierarchy(project, cellId);
        diagnostics.insert(diagnostics.end(), hdlDiagnostics.begin(), hdlDiagnostics.end());
    }
    return diagnostics;
}

SchematicDeletePlan planSchematicDeletion(const Project &project, ObjectId cellId) {
    const auto cell = project.schematicCells.find(cellId);
    if (cell == project.schematicCells.end())
        throw ProjectError("schematic.delete.unknown", "Schematic cell does not exist");
    SchematicDeletePlan plan;
    plan.cell = cellId;
    plan.name = cell->second.name;
    for (const auto &[id, simulation] : project.simulations) {
        if (simulation.schematic == cellId)
            plan.simulations.push_back({id, simulation.name});
    }
    for (const auto &[parentId, parent] : project.schematicCells) {
        if (parentId == cellId)
            continue;
        for (const auto &[symbolId, symbol] : parent.symbols) {
            if (symbol.sourceCell != cellId)
                continue;
            SchematicSymbolDependency dependency;
            dependency.parentCell = parentId;
            dependency.parentName = parent.name;
            dependency.symbol = symbolId;
            dependency.symbolName = symbol.name;
            for (const auto &[instanceId, instance] : parent.instances) {
                if (instance.symbol == symbolId)
                    dependency.instances.push_back(instanceId);
            }
            plan.symbols.push_back(std::move(dependency));
        }
    }
    return plan;
}

void removeSchematicCascade(Project &project, ObjectId cellId) {
    const auto plan = planSchematicDeletion(project, cellId);
    for (const auto &simulation : plan.simulations)
        project.simulations.erase(simulation.id);

    for (const auto &dependency : plan.symbols) {
        auto parent = project.schematicCells.find(dependency.parentCell);
        if (parent == project.schematicCells.end())
            continue;
        auto &cell = parent->second;
        std::set<ObjectId> removedPins;
        const auto symbol = cell.symbols.find(dependency.symbol);
        if (symbol != cell.symbols.end()) {
            for (const auto &pin : symbol->second.pins)
                removedPins.insert(pin.id);
            cell.symbols.erase(symbol);
        }
        for (auto it = cell.instances.begin(); it != cell.instances.end();) {
            if (it->second.symbol == dependency.symbol)
                it = cell.instances.erase(it);
            else
                ++it;
        }
        for (auto it = cell.nets.begin(); it != cell.nets.end();) {
            auto &pins = it->second.pins;
            pins.erase(
                std::remove_if(pins.begin(), pins.end(), [&](ObjectId pin) { return removedPins.contains(pin); }),
                pins.end());
            if (pins.size() < 2)
                it = cell.nets.erase(it);
            else
                ++it;
        }
    }
    project.schematicCells.erase(cellId);
}

bool spiceNetlistSupportsDrawn(const std::string &source) {
    std::istringstream stream(source);
    std::string line;
    bool hasComponent = false;
    while (std::getline(stream, line)) {
        const auto trimmed = trimCopy(line);
        if (trimmed.empty() || trimmed.starts_with('*') || trimmed.starts_with('+'))
            continue;
        if (trimmed.starts_with('.')) {
            const auto lower = lowerCopy(trimmed);
            if (lower.starts_with(".subckt") || lower.starts_with(".ends"))
                return false;
            continue;
        }
        const auto tokens = words(trimmed);
        if (tokens.empty())
            continue;
        const char element = static_cast<char>(std::tolower(static_cast<unsigned char>(tokens.front().front())));
        if (element != 'r' && element != 'c' && element != 'l' && element != 'v' && element != 'i' && element != 'd')
            return false;
        hasComponent = true;
    }
    return hasComponent;
}

FileSymbol resolveSchematicSymbol(const std::string &pdk, const std::string &name) {
    auto symbols = availableSymbols(pdk);
    ComponentRequest request;
    request.type = name;
    const auto *found = findComponentSymbol(symbols, request);
    return *found;
}

SpiceImportResult createSchematicFromSpice(Project &project, std::string name, std::string pdk,
                                           const std::string &source, bool drawn) {
    if (!drawn) {
        return {createTextSchematicCell(project, std::move(name), std::move(pdk), source), {}};
    }
    const auto cell = createSchematicCell(project, std::move(name), std::move(pdk));
    SpiceImportResult result{cell, {}};

    std::vector<std::string> lines;
    std::string current;
    std::istringstream stream(source);
    std::string line;
    while (std::getline(stream, line)) {
        const auto trimmed = trimCopy(line);
        if (trimmed.empty() || trimmed.starts_with('*'))
            continue;
        if (trimmed.starts_with('+') && !current.empty()) {
            current += " " + trimCopy(trimmed.substr(1));
        } else {
            if (!current.empty())
                lines.push_back(std::move(current));
            current = trimmed;
        }
    }
    if (!current.empty())
        lines.push_back(std::move(current));

    struct ImportedComponent {
        std::string name;
        std::string type;
        std::string parameterName;
        std::string parameterValue;
        std::string firstPin;
        std::string secondPin;
        std::string firstNode;
        std::string secondNode;
    };
    std::vector<ImportedComponent> imported;
    std::map<std::string, std::vector<std::string>> nodePins;

    for (const auto &logical : lines) {
        if (logical.starts_with('.')) {
            const auto lower = lowerCopy(logical);
            if (lower.starts_with(".subckt") || lower.starts_with(".ends"))
                throw ProjectError("schematic.import.subckt",
                                   "Drawn SPICE import does not support subcircuits; use a text SPICE cell");
            result.warnings.push_back("Directive ignored during drawn import: " + logical);
            continue;
        }
        const auto tokens = words(logical);
        if (tokens.size() < 4)
            throw ProjectError("schematic.import.line", "Unsupported SPICE line: " + logical);
        const char element = static_cast<char>(std::tolower(static_cast<unsigned char>(tokens[0].front())));
        ImportedComponent component;
        component.name = tokens[0];
        component.firstNode = tokens[1];
        component.secondNode = tokens[2];
        switch (element) {
        case 'r':
            component.type = "resistor";
            component.parameterName = "resistance";
            component.parameterValue = tokens[3];
            component.firstPin = "1";
            component.secondPin = "2";
            break;
        case 'c':
            component.type = "capacitor";
            component.parameterName = "capacitance";
            component.parameterValue = tokens[3];
            component.firstPin = "1";
            component.secondPin = "2";
            break;
        case 'l':
            component.type = "inductor";
            component.parameterName = "inductance";
            component.parameterValue = tokens[3];
            component.firstPin = "1";
            component.secondPin = "2";
            break;
        case 'v':
        case 'i': {
            component.type = element == 'v' ? "voltage source" : "current source";
            component.parameterName = "value";
            std::ostringstream value;
            for (std::size_t index = 3; index < tokens.size(); ++index) {
                if (index > 3)
                    value << " ";
                value << tokens[index];
            }
            component.parameterValue = value.str();
            component.firstPin = "+";
            component.secondPin = "-";
            break;
        }
        case 'd':
            component.type = "diode";
            component.parameterName = "model";
            component.parameterValue = tokens[3];
            component.firstPin = "A";
            component.secondPin = "K";
            break;
        default:
            throw ProjectError("schematic.import.element",
                               "Drawn SPICE import supports R/C/L/V/I/D only; unsupported element: " + tokens[0]);
        }
        imported.push_back(component);
        nodePins[component.firstNode].push_back(component.name + "." + component.firstPin);
        nodePins[component.secondNode].push_back(component.name + "." + component.secondPin);
    }

    if (imported.empty())
        throw ProjectError("schematic.import.empty", "No supported components were found in the SPICE source");

    int layoutIndex = 0;
    for (const auto &component : imported) {
        ComponentRequest request;
        request.type = component.type;
        request.name = component.name;
        request.x = static_cast<Coordinate>(layoutIndex % 4) * 180;
        request.y = static_cast<Coordinate>(layoutIndex / 4) * 150;
        request.parameters.push_back({0, component.parameterName, component.parameterValue});
        addSchematicComponent(project, cell, request);
        ++layoutIndex;
    }

    auto &importedCell = project.schematicCells.at(cell);
    for (const auto &[node, pins] : nodePins) {
        if (pins.size() < 2) {
            result.warnings.push_back("Node '" + node + "' has only one connection and was not netted");
            continue;
        }
        std::vector<ObjectId> pinIds;
        pinIds.reserve(pins.size());
        for (const auto &pin : pins)
            pinIds.push_back(resolveSchematicPin(importedCell, pin));
        addNet(project, cell, node, pinIds);
    }
    return result;
}

} // namespace ice
