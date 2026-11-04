#include "icelib/connectivity.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <numeric>
#include <set>
#include <vector>

namespace ice {
namespace {

struct ObjectRecord {
    enum class Kind { shape, port };
    Kind kind = Kind::shape;
    ObjectId id = 0;
    ObjectId layer = 0;
    ObjectId net = 0;
    Rect rectangle;
    bool viaLayer = false;
    // Electrical layer identity. A pin-purpose variant of a drawing layer
    // (for example Metal1.pin) must connect to the drawing layer it belongs
    // to, while marker/keepout purposes remain separate.
    std::string connectKey;
};

bool isConnectLayer(const std::string &name) {
    std::string lower;
    lower.reserve(name.size());
    for (const auto character : name)
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    return lower.find("via") != std::string::npos || lower.find("contact") != std::string::npos ||
           lower.find("licon") != std::string::npos || lower.find("mcon") != std::string::npos;
}

bool overlapsOrTouches(const Rect &left, const Rect &right) {
    if (!left.valid() || !right.valid())
        return false;
    return left.x <= right.x + right.width && right.x <= left.x + left.width && left.y <= right.y + right.height &&
           right.y <= left.y + left.height;
}

std::size_t findRoot(std::vector<std::size_t> &parents, std::size_t index) {
    auto root = index;
    while (parents[root] != root)
        root = parents[root];
    while (parents[index] != index) {
        const auto next = parents[index];
        parents[index] = root;
        index = next;
    }
    return root;
}

void unite(std::vector<std::size_t> &parents, std::size_t left, std::size_t right) {
    const auto leftRoot = findRoot(parents, left);
    const auto rightRoot = findRoot(parents, right);
    if (leftRoot != rightRoot)
        parents[rightRoot] = leftRoot;
}

} // namespace

std::string layoutLayerConnectivityKey(const Layer &layer) {
    std::string base = layer.name;
    std::string purpose;
    if (const auto dot = layer.name.rfind('.'); dot != std::string::npos) {
        base = layer.name.substr(0, dot);
        purpose = layer.name.substr(dot + 1);
    }
    for (auto &character : base)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    for (auto &character : purpose)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));

    std::string key = base + "#" + std::to_string(layer.number);
    if (!purpose.empty() && purpose != "drawing" && purpose != "pin") {
        key += ":";
        key += purpose;
        key += ":" + std::to_string(layer.datatype);
    }
    return key;
}

std::vector<LayoutNetGroup> extractLayoutNets(const Project &project, ObjectId cellId) {
    const auto cell = project.layoutCells.find(cellId);
    if (cell == project.layoutCells.end())
        throw ProjectError("layout.connectivity", "Layout cell does not exist");

    std::vector<ObjectRecord> objects;
    objects.reserve(cell->second.shapes.size() + cell->second.ports.size());
    for (const auto &[id, shape] : cell->second.shapes) {
        if (shape.isText() || !shape.rectangle.valid())
            continue;
        const auto layer = project.layers.find(shape.layer);
        const auto viaLayer = layer != project.layers.end() && isConnectLayer(layer->second.name);
        const auto connectKey = layer == project.layers.end() ? "missing:" + std::to_string(shape.layer)
                                                              : layoutLayerConnectivityKey(layer->second);
        objects.push_back(
            {ObjectRecord::Kind::shape, id, shape.layer, shape.net.value_or(0), shape.rectangle, viaLayer, connectKey});
    }
    for (const auto &[id, port] : cell->second.ports) {
        if (!port.visible)
            continue;
        const auto layer = project.layers.find(port.layer);
        const auto viaLayer = layer != project.layers.end() && isConnectLayer(layer->second.name);
        const auto connectKey = layer == project.layers.end() ? "missing:" + std::to_string(port.layer)
                                                              : layoutLayerConnectivityKey(layer->second);
        objects.push_back({ObjectRecord::Kind::port, id, port.layer, port.net, port.rectangle, viaLayer, connectKey});
    }

    std::vector<std::size_t> parents(objects.size());
    std::iota(parents.begin(), parents.end(), 0);

    // Explicit layout nets are authoritative. Merge every object carrying the
    // same net id first.
    std::map<ObjectId, std::size_t> firstByNet;
    for (std::size_t index = 0; index < objects.size(); ++index) {
        if (objects[index].net == 0)
            continue;
        const auto [entry, inserted] = firstByNet.emplace(objects[index].net, index);
        if (!inserted)
            unite(parents, entry->second, index);
    }

    // Spatially connected objects on the same layer merge into one net. Via and
    // contact layers also connect overlapping objects on different layers. Two
    // objects already assigned to different explicit nets are kept separate so
    // a short between distinct logical nets remains visible as a conflict.
    for (std::size_t left = 0; left < objects.size(); ++left) {
        for (std::size_t right = left + 1; right < objects.size(); ++right) {
            const bool sameLayer = objects[left].connectKey == objects[right].connectKey;
            const bool connectedByVia = objects[left].viaLayer || objects[right].viaLayer;
            if (!sameLayer && !connectedByVia)
                continue;
            if (objects[left].net != 0 && objects[right].net != 0 && objects[left].net != objects[right].net)
                continue;
            if (overlapsOrTouches(objects[left].rectangle, objects[right].rectangle))
                unite(parents, left, right);
        }
    }

    std::map<std::size_t, LayoutNetGroup> groups;
    std::map<std::size_t, ObjectId> groupNets;
    for (std::size_t index = 0; index < objects.size(); ++index) {
        const auto root = findRoot(parents, index);
        auto &group = groups[root];
        if (objects[index].kind == ObjectRecord::Kind::shape)
            group.shapes.push_back(objects[index].id);
        else
            group.ports.push_back(objects[index].id);
        if (objects[index].net != 0 && groupNets[root] == 0)
            groupNets[root] = objects[index].net;
    }

    std::map<ObjectId, std::size_t> rootByNet;
    for (const auto &[root, net] : groupNets) {
        if (net != 0)
            rootByNet.emplace(net, root);
    }
    std::size_t nextRoot = objects.size();
    for (const auto &[_, connection] : cell->second.plannedConnections) {
        if (connection.net == 0)
            continue;
        auto rootIt = rootByNet.find(connection.net);
        if (rootIt == rootByNet.end()) {
            const auto root = nextRoot++;
            rootByNet.emplace(connection.net, root);
            groupNets[root] = connection.net;
            groups[root].net = connection.net;
            rootIt = rootByNet.find(connection.net);
        }
        groups[rootIt->second].connections.push_back(connection.id);
    }

    std::vector<LayoutNetGroup> result;
    result.reserve(groups.size());
    for (auto &[root, group] : groups) {
        if (group.shapes.empty() && group.ports.empty() && group.connections.empty())
            continue;
        const auto netIt = groupNets.find(root);
        if (netIt != groupNets.end())
            group.net = netIt->second;
        if (group.net != 0) {
            if (const auto net = cell->second.nets.find(group.net); net != cell->second.nets.end())
                group.name = net->second.name;
        }
        if (group.name.empty())
            group.name = "N" + std::to_string(group.net != 0 ? group.net : root);
        std::sort(group.shapes.begin(), group.shapes.end());
        std::sort(group.ports.begin(), group.ports.end());
        std::sort(group.connections.begin(), group.connections.end());
        result.push_back(std::move(group));
    }
    std::sort(result.begin(), result.end(), [](const LayoutNetGroup &left, const LayoutNetGroup &right) {
        if (left.name != right.name)
            return left.name < right.name;
        return left.net < right.net;
    });
    return result;
}

} // namespace ice
