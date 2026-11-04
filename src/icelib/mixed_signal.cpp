#include "icelib/hdl.hpp"

#include "icelib/operations.hpp"
#include "icelib/simulation.hpp"

#include <algorithm>
#include <set>
#include <sstream>

namespace ice {
namespace {

std::string lowerCopy(std::string text) {
    for (auto &character : text)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return text;
}

const Net *netForPin(const SchematicCell &cell, ObjectId pinId) {
    for (const auto &[_, net] : cell.nets) {
        if (std::find(net.pins.begin(), net.pins.end(), pinId) != net.pins.end())
            return &net;
    }
    return nullptr;
}

const SchematicInstance *instanceForPin(const SchematicCell &cell, ObjectId pinId, const Symbol **symbolOut) {
    for (const auto &[_, instance] : cell.instances) {
        const auto symbol = cell.symbols.find(instance.symbol);
        if (symbol == cell.symbols.end())
            continue;
        if (std::any_of(symbol->second.pins.begin(), symbol->second.pins.end(),
                        [&](const Pin &pin) { return pin.id == pinId; })) {
            if (symbolOut)
                *symbolOut = &symbol->second;
            return &instance;
        }
    }
    return nullptr;
}

const Pin *pinForPort(const Symbol &symbol, const std::string &portName) {
    for (const auto &pin : symbol.pins) {
        if (lowerCopy(pin.name) == lowerCopy(portName))
            return &pin;
    }
    return nullptr;
}

bool sourceHasDumpCommands(const std::string &source) {
    return source.find("$dumpfile") != std::string::npos || source.find("$dumpvars") != std::string::npos;
}

bool containsHdlRecursive(const Project &project, ObjectId cellId, std::set<ObjectId> &visited) {
    if (!visited.insert(cellId).second)
        return false;
    const auto cell = project.schematicCells.find(cellId);
    if (cell == project.schematicCells.end() || cell->second.isTextSource())
        return false;
    for (const auto &[_, instance] : cell->second.instances) {
        const auto symbol = cell->second.symbols.find(instance.symbol);
        if (symbol == cell->second.symbols.end() || symbol->second.sourceCell == 0)
            continue;
        const auto child = project.schematicCells.find(symbol->second.sourceCell);
        if (child == project.schematicCells.end())
            continue;
        if (child->second.systemVerilogSchematic)
            return true;
        if (!child->second.isTextSource() && containsHdlRecursive(project, child->first, visited))
            return true;
    }
    return false;
}

void addError(MixedSignalPlan &plan, std::string message) {
    if (!plan.error.empty())
        plan.error += "\n";
    plan.error += std::move(message);
}

} // namespace

bool prepareMixedSignalRequest(SimulationRequest &request, const Project &project, ObjectId schematicId,
                               std::string *error) {
    const auto plan = buildMixedSignalPlan(project, schematicId);
    if (!plan.ok()) {
        if (error)
            *error = plan.error;
        return false;
    }
    if (plan.hdlInstances.empty()) {
        if (error)
            *error = "Schematic has no top-level SystemVerilog block instances";
        return false;
    }
    request.analysis = "transient";
    request.simulator = "mixed";
    request.netlist = plan.analogNetlist;
    request.mixedSignal.blocks = plan.hdlInstances;
    request.mixedSignal.analogInputNodes = plan.analogInputNodes;
    if (error)
        error->clear();
    return true;
}

bool schematicContainsHdl(const Project &project, ObjectId cellId) {
    std::set<ObjectId> visited;
    return containsHdlRecursive(project, cellId, visited);
}

MixedSignalPlan buildMixedSignalPlan(const Project &project, ObjectId cellId) {
    MixedSignalPlan plan;
    const auto cell = project.schematicCells.find(cellId);
    if (cell == project.schematicCells.end()) {
        plan.error = "Schematic cell does not exist";
        return plan;
    }
    if (cell->second.isTextSource()) {
        plan.error = "Mixed-signal plans require a drawn top-level schematic";
        return plan;
    }

    std::set<std::string> inputNodes;
    std::set<std::string> outputNodes;
    std::map<std::string, std::string> moduleSources;
    std::map<std::string, std::string> outputDrivers;
    std::map<ObjectId, int> symbolUseCount;
    for (const auto &[_, instance] : cell->second.instances)
        ++symbolUseCount[instance.symbol];

    for (const auto &[instanceId, instance] : cell->second.instances) {
        const auto symbol = cell->second.symbols.find(instance.symbol);
        if (symbol == cell->second.symbols.end() || symbol->second.sourceCell == 0)
            continue;
        const auto source = project.schematicCells.find(symbol->second.sourceCell);
        if (source == project.schematicCells.end())
            continue;
        if (!source->second.systemVerilogSchematic)
            continue;
        if (symbolUseCount[instance.symbol] > 1) {
            addError(plan, "SystemVerilog symbol '" + symbol->second.name +
                               "' is shared by multiple instances; create one HDL instance per symbol");
            continue;
        }

        const auto parsed = parseSystemVerilogModule(source->second);
        if (!parsed.ok) {
            addError(plan,
                     "SystemVerilog block '" + source->second.name + "' (" + instance.name + "): " + parsed.error);
            continue;
        }
        for (const auto &warning : parsed.warnings)
            plan.warnings.push_back(source->second.name + ": " + warning);
        if (sourceHasDumpCommands(source->second.systemVerilogText)) {
            addError(plan, "SystemVerilog block '" + source->second.name + "' (" + instance.name +
                               ") contains testbench dump commands; mixed-signal blocks must be plain modules");
            continue;
        }
        const auto &moduleName = parsed.module.name;
        const auto moduleSource = moduleSources.find(moduleName);
        if (moduleSource != moduleSources.end() && moduleSource->second != source->second.systemVerilogText) {
            addError(plan, "Multiple SystemVerilog cells define module '" + moduleName +
                               "' with different sources; rename the modules before mixing them");
            continue;
        }
        moduleSources.emplace(moduleName, source->second.systemVerilogText);
        if (moduleName == "ice_mixed_tb") {
            addError(plan, "SystemVerilog module name 'ice_mixed_tb' is reserved for the mixed-signal testbench");
            continue;
        }

        HdlInstanceBinding binding;
        binding.instanceId = instanceId;
        binding.symbolId = symbol->first;
        binding.instanceName = instance.name;
        binding.sourceCell = source->first;
        binding.moduleName = moduleName;
        binding.source = source->second.systemVerilogText;
        binding.ports = parsed.module.ports;

        std::set<std::string> modulePorts;
        for (const auto &port : binding.ports) {
            modulePorts.insert(lowerCopy(port.name));
            if (port.direction == HdlPortDirection::inout) {
                addError(plan, "SystemVerilog block '" + moduleName + "' port '" + port.name +
                                   "' is inout; mixed-signal interfaces support input and output only");
                continue;
            }
            if (!port.widthKnown || port.width != 1) {
                addError(plan, "SystemVerilog block '" + moduleName + "' port '" + port.name +
                                   "' is a bus; mixed-signal interfaces currently support scalar ports only");
                continue;
            }
            const auto pin = pinForPort(symbol->second, port.name);
            if (!pin) {
                addError(plan, "Symbol '" + symbol->second.name + "' is missing pin '" + port.name +
                                   "' for SystemVerilog module '" + moduleName + "'");
                continue;
            }
            const auto net = netForPin(cell->second, pin->id);
            if (!net) {
                addError(plan, "SystemVerilog block '" + instance.name + "' port '" + port.name +
                                   "' is not connected to a net");
                continue;
            }
            const auto node = schematicNetNodeName(cell->second, *net);
            binding.portNodes[port.name] = node;
            if (port.direction == HdlPortDirection::input) {
                if (node != "0")
                    inputNodes.insert(node);
            } else {
                if (node == "0") {
                    addError(plan, "SystemVerilog block '" + instance.name + "' output '" + port.name +
                                       "' cannot drive ground");
                    continue;
                }
                const auto previous = outputDrivers.find(node);
                if (previous != outputDrivers.end()) {
                    addError(plan, "Analog node '" + node + "' is driven by both '" + previous->second + "' and '" +
                                       instance.name + "." + port.name + "'");
                } else {
                    outputDrivers[node] = instance.name + "." + port.name;
                }
                for (const auto netPin : net->pins) {
                    const Symbol *ownerSymbol = nullptr;
                    const auto owner = instanceForPin(cell->second, netPin, &ownerSymbol);
                    if (!owner || !ownerSymbol || owner->id == instanceId)
                        continue;
                    if (spiceElementForSymbol(ownerSymbol->name) == 'V') {
                        addError(plan, "Analog node '" + node + "' is driven by SystemVerilog output '" +
                                           instance.name + "." + port.name + "' and by voltage source '" + owner->name +
                                           "'");
                        break;
                    }
                }
                outputNodes.insert(node);
            }
        }
        for (const auto &pin : symbol->second.pins) {
            if (!modulePorts.contains(lowerCopy(pin.name))) {
                addError(plan, "Symbol '" + symbol->second.name + "' has pin '" + pin.name +
                                   "' that is not a port of SystemVerilog module '" + moduleName + "'");
            }
        }
        plan.hdlInstances.push_back(std::move(binding));
    }

    if (!plan.error.empty())
        return plan;

    std::vector<std::string> notes;
    try {
        plan.analogNetlist = buildSchematicNetlist(project, cellId, &notes, SchematicNetlistMode::mixed);
    } catch (const ProjectError &error) {
        plan.error = error.what();
        return plan;
    }
    for (const auto &note : notes)
        plan.warnings.push_back(note);
    plan.analogInputNodes.assign(inputNodes.begin(), inputNodes.end());
    plan.digitalOutputNodes.assign(outputNodes.begin(), outputNodes.end());
    return plan;
}

std::vector<Diagnostic> validateHdlHierarchy(const Project &project, ObjectId cellId) {
    std::vector<Diagnostic> diagnostics;
    const auto cell = project.schematicCells.find(cellId);
    if (cell == project.schematicCells.end()) {
        diagnostics.push_back({Diagnostic::Severity::error, "hdl.schematic", "Schematic cell does not exist", cellId});
        return diagnostics;
    }
    if (!schematicContainsHdl(project, cellId))
        return diagnostics;

    const auto plan = buildMixedSignalPlan(project, cellId);
    if (!plan.ok()) {
        std::istringstream stream(plan.error);
        std::string line;
        while (std::getline(stream, line)) {
            if (!line.empty())
                diagnostics.push_back({Diagnostic::Severity::error, "hdl.mixed", line, cellId});
        }
        return diagnostics;
    }
    for (const auto &warning : plan.warnings)
        diagnostics.push_back({Diagnostic::Severity::warning, "hdl.netlist", warning, cellId});

    for (const auto &binding : plan.hdlInstances) {
        const auto symbol = cell->second.symbols.find(binding.symbolId);
        if (symbol == cell->second.symbols.end())
            continue;
        for (const auto &port : binding.ports) {
            const auto pin = pinForPort(symbol->second, port.name);
            if (!pin)
                continue;
            const auto expected = port.direction == HdlPortDirection::input ? PinDirection::left : PinDirection::right;
            if (pin->direction != expected) {
                diagnostics.push_back({Diagnostic::Severity::warning, "hdl.pin.direction",
                                       "Pin '" + port.name + "' on instance '" + binding.instanceName +
                                           "' does not face the expected side for a " +
                                           (port.direction == HdlPortDirection::input ? "input" : "output"),
                                       binding.instanceId});
            }
            const auto node = binding.portNodes.find(port.name);
            if (port.direction == HdlPortDirection::input && node != binding.portNodes.end() && node->second == "0") {
                diagnostics.push_back(
                    {Diagnostic::Severity::warning, "hdl.input.ground",
                     "Input port '" + port.name + "' on instance '" + binding.instanceName + "' is tied to ground",
                     binding.instanceId});
            }
        }
    }
    return diagnostics;
}

} // namespace ice
