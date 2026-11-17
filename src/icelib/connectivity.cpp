#include "icelib/connectivity.hpp"
#include "icelib/geometry.hpp"
#include "icelib/pdk.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <numeric>
#include <optional>
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
    // Electrical layer identity. A purpose variant of a drawing layer (for
    // example Metal1.pin) connects to the drawing layer it belongs to, while
    // marker/keepout purposes remain separate.
    std::string connectKey;
    // Lower-case layer base name, used to restrict a via/contact cut to the
    // layer pair it actually connects.
    std::string base;
};

bool isConnectLayer(const std::string &name, const PdkConnectivity *declared) {
    if (declared) {
        const auto role = declared->roleFor(name);
        if (!role.empty())
            return role == "via";
    }
    std::string lower;
    lower.reserve(name.size());
    for (const auto character : name)
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    const auto dot = lower.find('.');
    const auto base = dot == std::string::npos ? lower : lower.substr(0, dot);
    return base.find("via") != std::string::npos || base.find("contact") != std::string::npos ||
           base.find("licon") != std::string::npos || base.find("mcon") != std::string::npos;
}

bool isNonConductingLayer(const std::string &name, const PdkConnectivity *declared) {
    if (declared) {
        const auto role = declared->roleFor(name);
        if (!role.empty()) {
            return role == "marker" || role == "implant" || role == "well" || role == "boundary" || role == "label" ||
                   role == "text" || role == "ignored" || role == "insulator";
        }
    }
    std::string lower;
    lower.reserve(name.size());
    for (const auto character : name)
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    const auto dot = lower.find('.');
    const auto base = dot == std::string::npos ? lower : lower.substr(0, dot);
    static const std::vector<std::string> markers = {"well",   "implant", "marker", "boundary", "outline",
                                                     "areaid", "mim",     "nsdm",   "psdm",     "nplus",
                                                     "pplus",  "npc",     "label",  "text"};
    for (const auto &marker : markers) {
        if (base.find(marker) != std::string::npos)
            return true;
    }
    return false;
}

std::string baseFromLayerName(const std::string &name) {
    auto base = name;
    if (const auto dot = base.rfind('.'); dot != std::string::npos)
        base = base.substr(0, dot);
    for (auto &character : base)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return base;
}

struct ViaEndpoints {
    std::vector<std::string> lower;
    std::vector<std::string> upper;
    bool known = false;
};

bool containsBase(const std::vector<std::string> &bases, const std::string &base) {
    return std::find(bases.begin(), bases.end(), base) != bases.end();
}

int trailingNumber(const std::string &text, int fallback) {
    std::size_t start = text.size();
    while (start > 0 && std::isdigit(static_cast<unsigned char>(text[start - 1])) != 0)
        --start;
    if (start == text.size())
        return fallback;
    try {
        return std::stoi(text.substr(start));
    } catch (...) {
        return fallback;
    }
}

ViaEndpoints viaEndpoints(const std::string &viaName, const PdkConnectivity *declared) {
    const auto base = baseFromLayerName(viaName);
    ViaEndpoints result;
    if (declared) {
        if (const auto *via = declared->viaFor(base); via != nullptr) {
            result.lower = via->lower;
            result.upper = via->upper;
            result.known = true;
            return result;
        }
    }
    if (base.find("licon") != std::string::npos) {
        result.lower = {"diff", "poly", "poly2", "gatpoly", "comp", "activ", "active"};
        result.upper = {"li1"};
        result.known = true;
        return result;
    }
    if (base == "mcon") {
        result.lower = {"li1"};
        result.upper = {"met1", "metal1", "m1"};
        result.known = true;
        return result;
    }
    if (base == "contact" || base == "cont") {
        result.lower = {"diff", "poly", "poly2", "gatpoly", "comp", "activ", "active"};
        result.upper = {"met1", "metal1", "m1"};
        result.known = true;
        return result;
    }
    if (base.starts_with("topvia")) {
        const auto index = trailingNumber(base.substr(6), 1);
        result.lower = {"topmetal" + std::to_string(index)};
        result.upper = {"topmetal" + std::to_string(index + 1)};
        result.known = true;
        return result;
    }
    if (base.starts_with("via")) {
        const auto index = trailingNumber(base.substr(3), 1);
        result.lower = {"met" + std::to_string(index), "metal" + std::to_string(index), "m" + std::to_string(index)};
        result.upper = {"met" + std::to_string(index + 1), "metal" + std::to_string(index + 1),
                        "m" + std::to_string(index + 1)};
        if (index == 5)
            result.upper.push_back("metaltop");
        result.known = true;
        return result;
    }
    return result;
}

bool viaAllowsConductor(const std::string &viaBase, const std::string &conductorBase, const PdkConnectivity *declared) {
    if (conductorBase.empty())
        return true;
    const auto endpoints = viaEndpoints(viaBase, declared);
    if (!endpoints.known)
        return true;
    return containsBase(endpoints.lower, conductorBase) || containsBase(endpoints.upper, conductorBase);
}

std::string layerBase(const Layer &layer) {
    auto base = layer.name;
    if (const auto dot = base.rfind('.'); dot != std::string::npos)
        base = base.substr(0, dot);
    for (auto &character : base)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return base;
}

bool isDrawingPurpose(const Layer &layer) {
    const auto dot = layer.name.rfind('.');
    if (dot == std::string::npos)
        return true;
    auto purpose = layer.name.substr(dot + 1);
    for (auto &character : purpose)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return purpose == "drawing" || purpose == "pin";
}

bool isActiveLayer(const Layer &layer, const PdkConnectivity *declared) {
    if (!isDrawingPurpose(layer))
        return false;
    if (declared) {
        const auto role = declared->roleFor(layer.name);
        if (!role.empty())
            return role == "active" || role == "diffusion";
    }
    const auto base = layerBase(layer);
    return base == "diff" || base == "activ" || base == "active" || base == "comp";
}

bool isGateLayer(const Layer &layer, const PdkConnectivity *declared) {
    if (!isDrawingPurpose(layer))
        return false;
    if (declared) {
        const auto role = declared->roleFor(layer.name);
        if (!role.empty())
            return role == "gate" || role == "poly";
    }
    const auto base = layerBase(layer);
    return base == "poly" || base == "poly2" || base == "gate" || base == "gatpoly";
}

Polygon shapePolygon(const Shape &shape) {
    Polygon polygon;
    if (!shape.polygon.empty()) {
        polygon.reserve(shape.polygon.size());
        for (const auto &point : shape.polygon)
            polygon.push_back({point.x, point.y});
        return polygon;
    }
    if (!shape.rectangle.valid())
        return polygon;
    const auto &rect = shape.rectangle;
    polygon = {{rect.x, rect.y},
               {rect.x + rect.width, rect.y},
               {rect.x + rect.width, rect.y + rect.height},
               {rect.x, rect.y + rect.height}};
    return polygon;
}

std::vector<LayoutPoint> toLayoutPoints(const Polygon &polygon) {
    std::vector<LayoutPoint> points;
    points.reserve(polygon.size());
    for (const auto &point : polygon)
        points.push_back({point.x, point.y});
    return points;
}

// The active area under a gate is a channel, not a conductor. Split active
// shapes around the gate so source and drain do not appear shorted in the
// connectivity graph. This is a no-op after a PDK-aware device split.
void splitActiveUnderGate(const Project &project, const LayoutCell &cell, std::map<ObjectId, Shape> &shapes,
                          const PdkConnectivity *declared) {
    MultiPolygon gates;
    for (const auto &[_, shape] : cell.shapes) {
        const auto layer = project.layers.find(shape.layer);
        if (layer == project.layers.end() || !isGateLayer(layer->second, declared))
            continue;
        auto polygon = shapePolygon(shape);
        if (!polygon.empty())
            gates.push_back(std::move(polygon));
    }
    if (gates.empty())
        return;
    GeometryKernel kernel;
    const auto gateUnion = kernel.booleanOp(gates, {}, BooleanOperation::union_);
    ObjectId nextId = 1;
    for (const auto &[id, _] : cell.shapes)
        nextId = std::max(nextId, id + 1);
    for (const auto &[id, shape] : cell.shapes) {
        const auto layer = project.layers.find(shape.layer);
        if (layer == project.layers.end() || !isActiveLayer(layer->second, declared))
            continue;
        auto polygon = shapePolygon(shape);
        if (polygon.empty())
            continue;
        const auto fragments = kernel.booleanOp({polygon}, gateUnion, BooleanOperation::difference);
        if (fragments.size() <= 1)
            continue;
        shapes.erase(id);
        bool first = true;
        for (const auto &fragment : fragments) {
            Shape split;
            split.id = first ? id : nextId++;
            split.layer = shape.layer;
            split.net = shape.net;
            split.name = shape.name;
            split.polygon = toLayoutPoints(fragment);
            split.rectangle = GeometryKernel::bounds(fragment);
            shapes.emplace(split.id, std::move(split));
            first = false;
        }
    }
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
        key += ':';
        key += purpose;
        key += ":" + std::to_string(layer.datatype);
    }
    return key;
}

std::vector<LayoutNetGroup> extractLayoutNets(const Project &project, ObjectId cellId) {
    const auto cell = project.layoutCells.find(cellId);
    if (cell == project.layoutCells.end())
        throw ProjectError("layout.connectivity", "Layout cell does not exist");

    const auto declared = pdkConnectivityFor(cell->second.pdk);
    const auto *connectivity = declared ? &*declared : nullptr;
    std::map<ObjectId, Shape> effectiveShapes = cell->second.shapes;
    splitActiveUnderGate(project, cell->second, effectiveShapes, connectivity);

    std::vector<ObjectRecord> objects;
    objects.reserve(effectiveShapes.size() + cell->second.ports.size());
    for (const auto &[id, shape] : effectiveShapes) {
        if (shape.isText() || !shape.rectangle.valid())
            continue;
        const auto layer = project.layers.find(shape.layer);
        if (layer != project.layers.end() && isNonConductingLayer(layer->second.name, connectivity))
            continue;
        const auto viaLayer = layer != project.layers.end() && isConnectLayer(layer->second.name, connectivity);
        const auto layerName = layer == project.layers.end() ? std::string{} : layer->second.name;
        const auto connectKey = layer == project.layers.end() ? "missing:" + std::to_string(shape.layer)
                                                              : layoutLayerConnectivityKey(layer->second);
        objects.push_back({ObjectRecord::Kind::shape, id, shape.layer, shape.net.value_or(0), shape.rectangle, viaLayer,
                           connectKey, baseFromLayerName(layerName)});
    }
    for (const auto &[id, port] : cell->second.ports) {
        if (!port.visible)
            continue;
        const auto layer = project.layers.find(port.layer);
        if (layer != project.layers.end() && isNonConductingLayer(layer->second.name, connectivity))
            continue;
        const auto viaLayer = layer != project.layers.end() && isConnectLayer(layer->second.name, connectivity);
        const auto layerName = layer == project.layers.end() ? std::string{} : layer->second.name;
        const auto connectKey = layer == project.layers.end() ? "missing:" + std::to_string(port.layer)
                                                              : layoutLayerConnectivityKey(layer->second);
        objects.push_back({ObjectRecord::Kind::port, id, port.layer, port.net, port.rectangle, viaLayer, connectKey,
                           baseFromLayerName(layerName)});
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
            bool connected = sameLayer;
            if (!connected) {
                if (objects[left].viaLayer && objects[right].viaLayer) {
                    connected = false;
                } else if (objects[left].viaLayer) {
                    connected = viaAllowsConductor(objects[left].base, objects[right].base, connectivity);
                } else if (objects[right].viaLayer) {
                    connected = viaAllowsConductor(objects[right].base, objects[left].base, connectivity);
                }
            }
            if (!connected)
                continue;
            if (objects[left].net != 0 && objects[right].net != 0 && objects[left].net != objects[right].net)
                continue;
            if (overlapsOrTouches(objects[left].rectangle, objects[right].rectangle))
                unite(parents, left, right);
        }
    }

    std::map<std::size_t, LayoutNetGroup> groups;
    std::map<std::size_t, ObjectId> groupNets;
    std::map<std::size_t, std::vector<ObjectId>> groupNetIds;
    for (std::size_t index = 0; index < objects.size(); ++index) {
        const auto root = findRoot(parents, index);
        auto &group = groups[root];
        if (objects[index].kind == ObjectRecord::Kind::shape)
            group.shapes.push_back(objects[index].id);
        else
            group.ports.push_back(objects[index].id);
        if (objects[index].net != 0) {
            if (groupNets[root] == 0)
                groupNets[root] = objects[index].net;
            auto &ids = groupNetIds[root];
            if (std::find(ids.begin(), ids.end(), objects[index].net) == ids.end())
                ids.push_back(objects[index].net);
        }
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
        // Report any other explicit net that this component reached through
        // unlabeled geometry so a merged net is not silently reported as one.
        if (const auto ids = groupNetIds.find(root); ids != groupNetIds.end()) {
            for (const auto id : ids->second) {
                if (id == group.net)
                    continue;
                const auto net = cell->second.nets.find(id);
                group.aliases.push_back(net == cell->second.nets.end() ? "N" + std::to_string(id)
                                                                       : net->second.name);
            }
            std::sort(group.aliases.begin(), group.aliases.end());
        }
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

namespace {

bool parseIndexedLayer(const std::string &key, const std::string &prefix, int &index) {
    if (!key.starts_with(prefix))
        return false;
    const auto digits = key.substr(prefix.size());
    if (digits.empty() ||
        !std::all_of(digits.begin(), digits.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
        return false;
    try {
        index = std::stoi(digits);
    } catch (const std::exception &) {
        return false;
    }
    return index >= 0;
}

bool looksLikeMetal(const std::string &base) {
    return base.starts_with("metal") || base.starts_with("met") || base.starts_with("li") ||
           base.starts_with("topmetal") || base == "pad" || base.starts_with("dfpad") ||
           (base.size() == 2 && base[0] == 'm' && std::isdigit(static_cast<unsigned char>(base[1])) != 0);
}

} // namespace

LayoutConnectivity inferLayoutConnectivity(const Project &project, const std::string &pdkId) {
    LayoutConnectivity connectivity;
    const auto declared = pdkConnectivityFor(pdkId);
    const auto *pdk = declared ? &*declared : nullptr;

    std::map<std::string, ObjectId> layersByBase;
    std::map<std::string, ObjectId> fallbackByBase;
    for (const auto &[id, layer] : project.layers) {
        const auto base = baseFromLayerName(layer.name);
        if (base.empty())
            continue;
        const auto dot = layer.name.rfind('.');
        const auto purpose = dot == std::string::npos ? std::string{} : layer.name.substr(dot + 1);
        if (purpose.empty() || purpose == "drawing")
            layersByBase[base] = id;
        else
            fallbackByBase.emplace(base, id);
    }
    for (const auto &[base, id] : fallbackByBase)
        layersByBase.emplace(base, id);

    const auto layerFor = [&](const std::string &base) -> ObjectId {
        const auto found = layersByBase.find(baseFromLayerName(base));
        return found == layersByBase.end() ? 0 : found->second;
    };
    const auto addConductor = [&](ObjectId layer) {
        if (layer != 0)
            connectivity.conductors.insert(layer);
    };

    if (pdk) {
        // Declared roles are authoritative. Layers the package does not name
        // keep the generic name fallback so custom layers still participate.
        for (const auto &[id, layer] : project.layers) {
            const auto role = pdk->roleFor(layer.name);
            if (role.empty()) {
                const auto base = baseFromLayerName(layer.name);
                if (!isNonConductingLayer(layer.name, nullptr) && looksLikeMetal(base))
                    addConductor(id);
                continue;
            }
            // Active and gate layers participate in via transitions but are not
            // treated as general conductors: active under a gate is split by
            // the device extractor and must not become a global short in net
            // tracing.
            if (role == "conductor" || role == "metal" || role == "pad")
                addConductor(id);
        }

        const auto conductive = [&](ObjectId id) {
            const auto layer = project.layers.find(id);
            if (layer == project.layers.end())
                return false;
            const auto role = pdk->roleFor(layer->second.name);
            if (!role.empty())
                return role == "conductor" || role == "metal" || role == "active" || role == "gate" ||
                       role == "diffusion" || role == "pad";
            return !isNonConductingLayer(layer->second.name, nullptr);
        };
        std::set<ObjectId> declaredViaLayers;
        for (const auto &via : pdk->vias) {
            const auto viaLayer = layerFor(via.name);
            std::set<ObjectId> endpoints;
            for (const auto &name : via.lower)
                if (const auto layer = layerFor(name); layer != 0 && conductive(layer))
                    endpoints.insert(layer);
            for (const auto &name : via.upper)
                if (const auto layer = layerFor(name); layer != 0 && conductive(layer))
                    endpoints.insert(layer);
            if (viaLayer != 0) {
                declaredViaLayers.insert(viaLayer);
                if (endpoints.size() >= 2) {
                    auto &allowed = connectivity.viaConductors[viaLayer];
                    allowed.insert(endpoints.begin(), endpoints.end());
                    for (const auto layer : endpoints)
                        addConductor(layer);
                }
            }
        }

        // A via layer declared in package.toml but absent from the connectivity
        // document still gets the generic endpoint fallback.
        for (const auto &[id, layer] : project.layers) {
            if (declaredViaLayers.contains(id) || !isConnectLayer(layer.name, nullptr))
                continue;
            const auto endpoints = viaEndpoints(layer.name, pdk);
            std::set<ObjectId> allowed;
            for (const auto &[otherId, other] : project.layers) {
                if (otherId == id)
                    continue;
                const auto base = baseFromLayerName(other.name);
                if (!endpoints.known || containsBase(endpoints.lower, base) || containsBase(endpoints.upper, base))
                    allowed.insert(otherId);
            }
            if (!allowed.empty()) {
                connectivity.viaConductors[id] = std::move(allowed);
                for (const auto conductor : connectivity.viaConductors[id])
                    addConductor(conductor);
            }
        }
    } else {
        std::vector<std::pair<int, ObjectId>> metals;
        std::vector<std::pair<int, ObjectId>> topMetals;
        for (const auto &[base, id] : layersByBase) {
            int index = 0;
            if (parseIndexedLayer(base, "metal", index))
                metals.emplace_back(index, id);
            else if (parseIndexedLayer(base, "topmetal", index))
                topMetals.emplace_back(index, id);
        }
        std::sort(metals.begin(), metals.end(),
                  [](const auto &left, const auto &right) { return left.first < right.first; });
        std::sort(topMetals.begin(), topMetals.end(),
                  [](const auto &left, const auto &right) { return left.first < right.first; });

        std::vector<std::pair<int, bool>> stack; // layer index, is top metal
        for (const auto &entry : metals)
            stack.emplace_back(entry.first, false);
        for (const auto &entry : topMetals)
            stack.emplace_back(entry.first, true);
        if (!stack.empty()) {
            for (const auto &[_, id] : metals)
                addConductor(id);
            for (const auto &[_, id] : topMetals)
                addConductor(id);
            for (std::size_t index = 0; index + 1 < stack.size(); ++index) {
                const auto lowerLayer = stack[index].second ? layerFor("topmetal" + std::to_string(stack[index].first))
                                                            : layerFor("metal" + std::to_string(stack[index].first));
                const auto upperLayer = stack[index + 1].second
                                            ? layerFor("topmetal" + std::to_string(stack[index + 1].first))
                                            : layerFor("metal" + std::to_string(stack[index + 1].first));
                if (lowerLayer == 0 || upperLayer == 0)
                    continue;
                std::string viaKey;
                if (!stack[index].second && !stack[index + 1].second)
                    viaKey = "via" + std::to_string(stack[index].first);
                else
                    viaKey = "topvia" + std::to_string(stack[index + 1].first);
                const auto viaLayer = layerFor(viaKey);
                if (viaLayer != 0) {
                    auto &allowed = connectivity.viaConductors[viaLayer];
                    allowed.insert(lowerLayer);
                    allowed.insert(upperLayer);
                } else {
                    connectivity.directPairs.emplace(std::min(lowerLayer, upperLayer),
                                                     std::max(lowerLayer, upperLayer));
                }
            }
        }

        const auto contactLayer = layerFor("contact");
        if (contactLayer != 0) {
            std::set<ObjectId> contactConductors;
            for (const auto &key : {"activ", "gatpoly", "poly2", "metal1"}) {
                if (const auto layer = layerFor(key); layer != 0)
                    contactConductors.insert(layer);
            }
            if (contactConductors.size() >= 2) {
                auto &allowed = connectivity.viaConductors[contactLayer];
                for (const auto layer : contactConductors) {
                    allowed.insert(layer);
                    addConductor(layer);
                }
            }
        }

        if (connectivity.conductors.empty()) {
            std::vector<ObjectId> viaLayers;
            for (const auto &[base, id] : layersByBase) {
                const bool via = base.find("via") != std::string::npos || base.find("contact") != std::string::npos;
                const bool ignored = base.find("text") != std::string::npos ||
                                     base.find("label") != std::string::npos ||
                                     base.find("boundary") != std::string::npos;
                if (via) {
                    viaLayers.push_back(id);
                } else if (!ignored) {
                    addConductor(id);
                }
            }
            if (connectivity.conductors.empty()) {
                for (const auto &[_, layer] : project.layers)
                    addConductor(layer.id);
            }
            for (const auto via : viaLayers)
                connectivity.viaConductors[via] = connectivity.conductors;
        }
    }

    // Routing layers follow the declared stack. Without package metadata, keep
    // the generic metal-name convention used by older projects.
    if (pdk && !pdk->routingStack.empty()) {
        for (const auto &name : pdk->routingStack) {
            const auto layer = layerFor(name);
            if (layer == 0)
                continue;
            if (connectivity.routingLayers.insert(layer).second)
                connectivity.routingOrder.push_back(layer);
        }
    }
    if (connectivity.routingOrder.empty()) {
        for (const auto conductor : connectivity.conductors) {
            const auto layer = project.layers.find(conductor);
            if (layer == project.layers.end())
                continue;
            if (looksLikeMetal(baseFromLayerName(layer->second.name))) {
                connectivity.routingLayers.insert(conductor);
                connectivity.routingOrder.push_back(conductor);
            }
        }
        std::sort(
            connectivity.routingOrder.begin(), connectivity.routingOrder.end(), [&](ObjectId left, ObjectId right) {
                const auto leftLayer = project.layers.find(left);
                const auto rightLayer = project.layers.find(right);
                const auto leftBase =
                    leftLayer == project.layers.end() ? std::string{} : baseFromLayerName(leftLayer->second.name);
                const auto rightBase =
                    rightLayer == project.layers.end() ? std::string{} : baseFromLayerName(rightLayer->second.name);
                int leftIndex = 0;
                int rightIndex = 0;
                const auto leftMetal =
                    parseIndexedLayer(leftBase, "metal", leftIndex) || parseIndexedLayer(leftBase, "met", leftIndex);
                const auto rightMetal = parseIndexedLayer(rightBase, "metal", rightIndex) ||
                                        parseIndexedLayer(rightBase, "met", rightIndex);
                if (leftMetal != rightMetal)
                    return leftMetal;
                if (leftMetal && leftIndex != rightIndex)
                    return leftIndex < rightIndex;
                return left < right;
            });
        connectivity.routingLayers.clear();
        for (const auto layer : connectivity.routingOrder)
            connectivity.routingLayers.insert(layer);
    }
    return connectivity;
}

} // namespace ice
