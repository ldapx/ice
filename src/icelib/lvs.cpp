#include "icelib/verification.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
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

bool parameterValuesEqual(const std::string &left, const std::string &right) {
    const auto leftQuantity = parseQuantity(left);
    const auto rightQuantity = parseQuantity(right);
    if (leftQuantity && rightQuantity) {
        const auto scale = std::max({1.0, std::abs(*leftQuantity), std::abs(*rightQuantity)});
        return std::abs(*leftQuantity - *rightQuantity) <= 1.0e-9 * scale;
    }
    return normalizedValue(left) == normalizedValue(right);
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

std::string instanceType(const SchematicCell &cell, ObjectId instanceId) {
    const auto instance = cell.instances.find(instanceId);
    if (instance == cell.instances.end())
        return {};
    const auto symbol = cell.symbols.find(instance->second.symbol);
    return symbol == cell.symbols.end() ? std::string{} : symbol->second.name;
}

struct LogicalDevice {
    ObjectId object = 0;
    std::string name;
    std::string type;
    std::map<std::string, std::string> parameters;
    bool hasParameters = false;
    ObjectId sourceCell = 0;
    ObjectId childCell = 0;
    ObjectId schematicInstance = 0;
};

struct LogicalNet {
    std::string name;
    std::set<ObjectId> devices;
    ObjectId object = 0;
};

struct LogicalPort {
    ObjectId object = 0;
    std::string name;
    std::string direction;
    std::string net;
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

LogicalSide collectSchematicSide(const Project &project, ObjectId cellId, std::vector<LvsMismatch> &mismatches) {
    const auto cellIt = project.schematicCells.find(cellId);
    if (cellIt == project.schematicCells.end())
        throw ProjectError("lvs.schematic", "Schematic cell does not exist");
    const auto &cell = cellIt->second;
    if (cell.textSchematic)
        throw ProjectError("lvs.schematic", "LVS currently requires a drawn schematic; text SPICE schematics are not "
                                            "supported yet");

    LogicalSide side;
    std::map<ObjectId, std::vector<ObjectId>> pinOwners;
    struct PortInstance {
        ObjectId id = 0;
        std::string name;
        std::string direction;
        std::vector<ObjectId> pins;
    };
    std::vector<PortInstance> portInstances;
    for (const auto &[id, instance] : cell.instances) {
        LogicalDevice device;
        device.object = id;
        const auto symbol = cell.symbols.find(instance.symbol);
        std::string symbolType;
        if (symbol != cell.symbols.end())
            symbolType = normalizedIdentifier(symbol->second.name);
        const bool isPort = symbolType == "input" || symbolType == "output";
        if (symbol != cell.symbols.end()) {
            device.type = symbol->second.name;
            device.sourceCell = symbol->second.sourceCell;
            if (isPort) {
                PortInstance port;
                port.id = id;
                port.name = instance.name.empty() ? symbol->second.name : instance.name;
                port.direction = symbolType;
                for (const auto &pin : symbol->second.pins)
                    port.pins.push_back(pin.id);
                portInstances.push_back(std::move(port));
            } else {
                for (const auto &pin : symbol->second.pins)
                    pinOwners[pin.id].push_back(id);
            }
        } else {
            device.type = "unknown";
            mismatches.push_back({1, "schematic-symbol",
                                  "Schematic instance '" + instance.name + "' references a missing symbol", id,
                                  std::nullopt, VerificationSeverity::error});
        }
        if (isPort)
            continue;
        device.name = instance.name.empty() ? device.type + " " + std::to_string(id) : instance.name;
        device.parameters = parameterMap(instance.parameters);
        device.hasParameters = true;
        side.devices.emplace(id, std::move(device));
    }

    std::map<ObjectId, std::string> netByPin;
    for (const auto &[id, net] : cell.nets) {
        const auto netName = net.name.empty() ? "N" + std::to_string(id) : net.name;
        for (const auto pinId : net.pins)
            netByPin.emplace(pinId, netName);
    }
    for (auto &port : portInstances) {
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
        std::set<ObjectId> devices;
        for (const auto pinId : net.pins) {
            const auto owners = pinOwners.find(pinId);
            if (owners == pinOwners.end())
                continue;
            for (const auto owner : owners->second)
                devices.insert(owner);
        }
        if (devices.size() < 2)
            continue;
        side.nets.push_back({net.name.empty() ? "N" + std::to_string(id) : net.name, std::move(devices), id});
    }
    return side;
}

LogicalDevice actualLayoutDevice(const LayoutInstance &instance, const LayoutCell &child) {
    LogicalDevice device;
    device.object = instance.id;
    device.name = instance.name;
    device.type = child.device.empty() ? child.name : child.device;
    device.childCell = child.id;
    device.parameters = parameterMap(child.parameters);
    device.hasParameters = true;
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
        auto device = actualLayoutDevice(instance, child->second);
        actualByName[normalizedIdentifier(instance.name)].push_back(id);
        actualDevices.push_back({id, std::move(device)});
    }
    for (auto &entry : actualByName)
        std::sort(entry.second.begin(), entry.second.end());

    std::set<ObjectId> usedActual;
    for (const auto &[plannedId, planned] : cell.plannedDevices) {
        ObjectId actualId = 0;
        const auto matches = actualByName.find(normalizedIdentifier(planned.name));
        if (matches != actualByName.end()) {
            for (const auto candidate : matches->second) {
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
        if (actualId != 0) {
            const auto actual = std::find_if(actualDevices.begin(), actualDevices.end(),
                                             [actualId](const auto &entry) { return entry.instance == actualId; });
            if (actual != actualDevices.end())
                device = actual->device;
            device.schematicInstance = planned.schematicInstance;
        } else if (planned.schematicInstance != 0) {
            if (!schematic.instances.contains(planned.schematicInstance)) {
                mismatches.push_back({1, "layout-link",
                                      "Planned device '" + planned.name +
                                          "' links to a schematic instance that is not in this schematic",
                                      std::nullopt, plannedId, VerificationSeverity::warning});
            } else {
                device.type = instanceType(schematic, planned.schematicInstance);
            }
        }

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

    // Layout connectivity comes from planned connections. Named connections
    // keep their logical net name; unnamed connections are merged by connected
    // component. This preserves parallel nets between the same device pair
    // when the layout carries net names.
    std::map<ObjectId, ObjectId> parent;
    for (const auto &[plannedId, _] : cell.plannedDevices)
        parent[plannedId] = plannedId;
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

    bool validConnection = false;
    std::map<std::string, std::set<ObjectId>> namedGroups;
    std::map<std::string, ObjectId> namedObjects;
    std::set<ObjectId> unnamedMembers;
    std::map<ObjectId, ObjectId> unnamedObjects;
    for (const auto &[_, connection] : cell.plannedConnections) {
        const bool haveFrom = parent.contains(connection.from);
        const bool haveTo = parent.contains(connection.to);
        if (!haveFrom || !haveTo) {
            mismatches.push_back({1, "layout-connection", "Planned connection references a missing planned device",
                                  std::nullopt, connection.id, VerificationSeverity::error});
            continue;
        }
        validConnection = true;
        const auto layoutNet = cell.nets.find(connection.net);
        const std::string netName =
            layoutNet != cell.nets.end() && !layoutNet->second.name.empty() ? layoutNet->second.name : std::string{};
        if (!netName.empty()) {
            auto &members = namedGroups[netName];
            members.insert(connection.from);
            members.insert(connection.to);
            const auto [entry, inserted] = namedObjects.emplace(netName, connection.id);
            if (!inserted && connection.id < entry->second)
                entry->second = connection.id;
        } else {
            unite(connection.from, connection.to);
            unnamedMembers.insert(connection.from);
            unnamedMembers.insert(connection.to);
            const auto root = findRoot(connection.from);
            const auto [entry, inserted] = unnamedObjects.emplace(root, connection.id);
            if (!inserted && connection.id < entry->second)
                entry->second = connection.id;
        }
    }
    result.hasConnectivity = validConnection;

    for (const auto &[name, members] : namedGroups) {
        LogicalNet net;
        net.name = name;
        net.object = namedObjects.at(name);
        for (const auto plannedId : members) {
            const auto logical = result.plannedToLogical.find(plannedId);
            if (logical != result.plannedToLogical.end())
                net.devices.insert(logical->second);
        }
        if (net.devices.size() >= 2)
            result.side.nets.push_back(std::move(net));
    }

    std::map<ObjectId, std::vector<ObjectId>> unnamedComponents;
    for (const auto plannedId : unnamedMembers)
        unnamedComponents[findRoot(plannedId)].push_back(plannedId);
    for (const auto &[root, members] : unnamedComponents) {
        if (members.size() < 2)
            continue;
        LogicalNet net;
        net.name = "net " + std::to_string(*std::min_element(members.begin(), members.end()));
        net.object = unnamedObjects.contains(root) ? unnamedObjects.at(root) : 0;
        for (const auto plannedId : members) {
            const auto logical = result.plannedToLogical.find(plannedId);
            if (logical != result.plannedToLogical.end())
                net.devices.insert(logical->second);
        }
        if (net.devices.size() >= 2)
            result.side.nets.push_back(std::move(net));
    }

    for (const auto &[_, port] : cell.ports) {
        if (!port.visible)
            continue;
        std::string netName;
        if (port.net != 0) {
            if (const auto net = cell.nets.find(port.net); net != cell.nets.end())
                netName = net->second.name;
        }
        result.side.ports.push_back({port.id, port.name, portDirectionName(port.direction), netName});
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

void validateDevices(const LogicalSide &schematic, const LayoutLogicalSide &layout, const LvsOptions &options,
                     const std::map<ObjectId, ObjectId> &schematicToLayout,
                     const std::map<ObjectId, ObjectId> &layoutToSchematic,
                     std::map<ObjectId, std::string> &schematicKeys, std::map<ObjectId, std::string> &layoutKeys,
                     std::map<std::string, std::string> &keyNames, std::vector<LvsMismatch> &mismatches) {
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
    for (const auto &[schematicId, _] : schematic.devices) {
        if (!schematicToLayout.contains(schematicId)) {
            schematicKeys.emplace(schematicId, "s" + std::to_string(schematicId));
            keyNames.emplace("s" + std::to_string(schematicId), schematic.devices.at(schematicId).name);
            mismatches.push_back(
                {1, "missing-device",
                 "Schematic device '" + schematic.devices.at(schematicId).name + "' has no matching layout device",
                 schematicId, std::nullopt, VerificationSeverity::error});
        }
    }
    for (const auto &[layoutId, device] : layout.side.devices) {
        if (!layoutToSchematic.contains(layoutId)) {
            layoutKeys.emplace(layoutId, "l" + std::to_string(layoutId));
            keyNames.emplace("l" + std::to_string(layoutId), device.name);
            mismatches.push_back({1, "extra-device",
                                  "Layout device '" + device.name + "' has no matching schematic device", std::nullopt,
                                  layoutId, VerificationSeverity::error});
        }
    }

    for (const auto &[schematicId, layoutId] : schematicToLayout) {
        const auto &schematicDevice = schematic.devices.at(schematicId);
        const auto &layoutDevice = layout.side.devices.at(layoutId);
        const auto displayName = schematicDevice.name.empty() ? layoutDevice.name : schematicDevice.name;
        if (normalizedIdentifier(schematicDevice.name) != normalizedIdentifier(layoutDevice.name)) {
            mismatches.push_back(
                {1, "device-name",
                 "Schematic device '" + schematicDevice.name + "' is named '" + layoutDevice.name + "' in the layout",
                 schematicId, layoutId, VerificationSeverity::warning});
        }
        if (!compatibleType(schematicDevice.type, layoutDevice.type)) {
            mismatches.push_back({1, "device-type",
                                  "Device '" + displayName + "' is '" + kindName(true, schematicDevice) +
                                      "' in the schematic but '" + kindName(false, layoutDevice) + "' in the layout",
                                  schematicId, layoutId, VerificationSeverity::error});
        }
        if (!options.compareParameters || !layoutDevice.hasParameters)
            continue;
        for (const auto &[name, schematicValue] : schematicDevice.parameters) {
            const auto layoutParameter = layoutDevice.parameters.find(name);
            if (layoutParameter == layoutDevice.parameters.end())
                continue;
            if (parameterValuesEqual(schematicValue, layoutParameter->second))
                continue;
            mismatches.push_back({1, "parameter",
                                  "Device '" + displayName + "' parameter '" + name + "' is '" + schematicValue +
                                      "' in the schematic but '" + layoutParameter->second + "' in the layout",
                                  schematicId, layoutId, VerificationSeverity::error});
        }
    }
}

using NetSignature = std::vector<std::string>;

struct NetRecord {
    NetSignature signature;
    std::string name;
    ObjectId object = 0;
};

NetRecord netRecord(const LogicalNet &net, const std::map<ObjectId, std::string> &keys) {
    NetRecord record;
    record.name = net.name;
    record.object = net.object;
    for (const auto device : net.devices) {
        const auto key = keys.find(device);
        if (key == keys.end())
            continue;
        record.signature.push_back(key->second);
    }
    std::sort(record.signature.begin(), record.signature.end());
    return record;
}

std::string signatureNames(const NetSignature &signature, const std::map<std::string, std::string> &keyNames) {
    std::vector<std::string> names;
    names.reserve(signature.size());
    for (const auto &key : signature) {
        const auto name = keyNames.find(key);
        names.push_back(name == keyNames.end() ? key : name->second);
    }
    return joinNames(names);
}

int comparePorts(const LogicalSide &schematic, const LogicalSide &layout, std::vector<LvsMismatch> &mismatches) {
    if (schematic.ports.empty() && layout.ports.empty())
        return 0;
    std::map<std::string, LogicalPort> schematicPorts;
    std::map<std::string, LogicalPort> layoutPorts;
    for (const auto &port : schematic.ports)
        schematicPorts[normalizedIdentifier(port.name)] = port;
    for (const auto &port : layout.ports)
        layoutPorts[normalizedIdentifier(port.name)] = port;
    int matched = 0;
    for (const auto &[name, port] : schematicPorts) {
        const auto layoutPort = layoutPorts.find(name);
        if (layoutPort == layoutPorts.end()) {
            mismatches.push_back({1, "missing-port", "Schematic port '" + port.name + "' has no matching layout pin",
                                  port.object, std::nullopt, VerificationSeverity::error});
            continue;
        }
        ++matched;
        if (!port.direction.empty() && !layoutPort->second.direction.empty() &&
            port.direction != layoutPort->second.direction)
            mismatches.push_back({1, "port-direction",
                                  "Port '" + port.name + "' is '" + port.direction + "' in the schematic but '" +
                                      layoutPort->second.direction + "' in the layout",
                                  port.object, layoutPort->second.object, VerificationSeverity::warning});
        const auto schematicNet = normalizedIdentifier(port.net);
        const auto layoutNet = normalizedIdentifier(layoutPort->second.net);
        if (!schematicNet.empty() && schematicNet != layoutNet)
            mismatches.push_back({1, "port-net",
                                  "Port '" + port.name + "' is connected to net '" + port.net +
                                      "' in the schematic but '" +
                                      (layoutPort->second.net.empty() ? "an unassigned pin" : layoutPort->second.net) +
                                      "' in the layout",
                                  port.object, layoutPort->second.object, VerificationSeverity::error});
    }
    for (const auto &[name, port] : layoutPorts) {
        if (!schematicPorts.contains(name))
            mismatches.push_back({1, "extra-port", "Layout pin '" + port.name + "' has no matching schematic port",
                                  std::nullopt, port.object, VerificationSeverity::error});
    }
    return matched;
}

void compareNets(const LogicalSide &schematic, const LayoutLogicalSide &layout,
                 const std::map<ObjectId, std::string> &schematicKeys,
                 const std::map<ObjectId, std::string> &layoutKeys, const std::map<std::string, std::string> &keyNames,
                 std::vector<LvsMismatch> &mismatches) {
    if (schematic.nets.empty())
        return;
    if (!layout.hasConnectivity) {
        mismatches.push_back({1, "layout-netlist",
                              "Layout has no planned-connection connectivity; LVS compared devices only", std::nullopt,
                              std::nullopt, VerificationSeverity::warning});
        return;
    }

    std::vector<NetRecord> schematicNets;
    for (const auto &net : schematic.nets) {
        auto record = netRecord(net, schematicKeys);
        if (record.signature.size() >= 2)
            schematicNets.push_back(std::move(record));
    }
    std::vector<NetRecord> layoutNets;
    for (const auto &net : layout.side.nets) {
        auto record = netRecord(net, layoutKeys);
        if (record.signature.size() >= 2)
            layoutNets.push_back(std::move(record));
    }

    std::map<NetSignature, int> layoutCounts;
    for (const auto &record : layoutNets)
        ++layoutCounts[record.signature];
    for (const auto &record : schematicNets) {
        auto &count = layoutCounts[record.signature];
        if (count > 0) {
            --count;
            continue;
        }
        mismatches.push_back({1, "missing-connection",
                              "Schematic net '" + record.name + "' connects " +
                                  signatureNames(record.signature, keyNames) + " but no matching layout net was found",
                              record.object, std::nullopt, VerificationSeverity::error});
    }

    std::map<NetSignature, int> schematicCounts;
    for (const auto &record : schematicNets)
        ++schematicCounts[record.signature];
    for (const auto &record : layoutNets) {
        auto &count = schematicCounts[record.signature];
        if (count > 0) {
            --count;
            continue;
        }
        mismatches.push_back({1, "extra-connection",
                              "Layout net '" + record.name + "' connects " +
                                  signatureNames(record.signature, keyNames) +
                                  " but no matching schematic net was found",
                              std::nullopt, record.object, VerificationSeverity::error});
    }
}

std::string schematicBaseName(std::string name) {
    const auto normalized = normalizedIdentifier(name);
    if (normalized.ends_with("_layout"))
        return normalized.substr(0, normalized.size() - 7);
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

    std::set<ObjectId> linkedInstances;
    for (const auto &[_, device] : layout->second.plannedDevices) {
        if (device.schematicInstance != 0)
            linkedInstances.insert(device.schematicInstance);
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
    for (const auto &[cellId, cell] : project.schematicCells) {
        const auto schematicName = normalizedIdentifier(cell.name);
        if (schematicName == layoutName || schematicName == layoutBase)
            return cellId;
    }
    return 0;
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
    report.portsMatched = comparePorts(schematic, layout.side, report.mismatches);

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
    const auto match = [&](ObjectId schematicId, ObjectId layoutId) {
        schematicToLayout.emplace(schematicId, layoutId);
        layoutToSchematic.emplace(layoutId, schematicId);
    };

    for (const auto &[layoutId, device] : layout.side.devices) {
        if (device.schematicInstance == 0 || !schematic.devices.contains(device.schematicInstance))
            continue;
        if (layoutToSchematic.contains(layoutId) || schematicToLayout.contains(device.schematicInstance))
            continue;
        match(device.schematicInstance, layoutId);
    }
    for (const auto &[layoutId, device] : layout.side.devices) {
        if (layoutToSchematic.contains(layoutId))
            continue;
        const auto candidates = schematicByName.find(normalizedIdentifier(device.name));
        if (candidates == schematicByName.end())
            continue;
        for (const auto schematicId : candidates->second) {
            if (schematicToLayout.contains(schematicId))
                continue;
            match(schematicId, layoutId);
            break;
        }
    }

    std::map<ObjectId, std::string> schematicKeys;
    std::map<ObjectId, std::string> layoutKeys;
    std::map<std::string, std::string> keyNames;
    validateDevices(schematic, layout, options, schematicToLayout, layoutToSchematic, schematicKeys, layoutKeys,
                    keyNames, report.mismatches);
    compareNets(schematic, layout, schematicKeys, layoutKeys, keyNames, report.mismatches);
    return report;
}

std::string serializeLvsReport(const LvsReport &report) {
    json output{{"schema_version", report.schemaVersion},
                {"schematic", report.schematic},
                {"layout", report.layout},
                {"matched", report.matched()},
                {"ports_matched", report.portsMatched},
                {"mismatches", json::array()}};
    const auto optionalId = [](std::optional<ObjectId> id) -> json { return id ? json(*id) : json(nullptr); };
    for (const auto &mismatch : report.mismatches) {
        output["mismatches"].push_back({{"schema_version", mismatch.schemaVersion},
                                        {"severity", verificationSeverityName(mismatch.severity)},
                                        {"category", mismatch.category},
                                        {"message", mismatch.message},
                                        {"schematic_object", optionalId(mismatch.schematicObject)},
                                        {"layout_object", optionalId(mismatch.layoutObject)}});
    }
    return output.dump(2) + "\n";
}

} // namespace ice
