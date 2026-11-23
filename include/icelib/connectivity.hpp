#pragma once

#include "icelib/core.hpp"

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace ice {

// Read-only geometric net extraction for a layout cell. Objects that carry the
// same LayoutNet id are always merged. Shapes and ports that touch or overlap on
// the same layer are merged as well, providing a net view even before a full
// device extractor is available. Pin-purpose variants such as Metal1.pin are
// electrically connected to their Metal1 drawing layer.
struct LayoutNetGroup {
    ObjectId net = 0;
    std::string name;
    // Explicit nets that this geometric component also touches through
    // unlabeled geometry (for example a resistor body between two nets). The
    // group keeps `net`/`name` for the first one and lists the rest here so a
    // merged component is visible instead of silently dropping a net name.
    std::vector<std::string> aliases;
    std::vector<ObjectId> shapes;
    std::vector<ObjectId> ports;
    std::vector<ObjectId> connections;
};

std::vector<LayoutNetGroup> extractLayoutNets(const Project &project, ObjectId cellId);

// Layer-level connectivity graph used by net tracing and the planned router.
// The graph is built from the package's `technology/connectivity.json` when
// available and falls back to generic layer-name conventions for custom PDKs.
struct LayoutConnectivity {
    std::set<ObjectId> conductors;
    std::map<ObjectId, std::set<ObjectId>> viaConductors;
    std::set<std::pair<ObjectId, ObjectId>> directPairs;
    // Conductors usable as routing layers, and their vertical order.
    std::set<ObjectId> routingLayers;
    std::vector<ObjectId> routingOrder;
};

LayoutConnectivity inferLayoutConnectivity(const Project &project, const std::string &pdkId);

// Electrical identity of a layout layer. Drawing and pin-purpose variants of
// the same base layer share a key (for example Metal1 and Metal1.pin), while
// marker/keepout purposes remain distinct.
std::string layoutLayerConnectivityKey(const Layer &layer);

} // namespace ice
