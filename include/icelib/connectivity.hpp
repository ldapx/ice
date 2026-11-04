#pragma once

#include "icelib/core.hpp"

#include <string>
#include <vector>

namespace ice {

// Read-only geometric net extraction for a layout cell. Objects that carry the
// same LayoutNet id are always merged. Shapes and pins that touch or overlap on
// the same layer are merged as well, providing a net view even before a full
// device extractor is available. Pin-purpose variants such as Metal1.pin are
// electrically connected to their Metal1 drawing layer.
struct LayoutNetGroup {
    ObjectId net = 0;
    std::string name;
    std::vector<ObjectId> shapes;
    std::vector<ObjectId> ports;
    std::vector<ObjectId> connections;
};

std::vector<LayoutNetGroup> extractLayoutNets(const Project &project, ObjectId cellId);

// Electrical identity of a layout layer. Drawing and pin-purpose variants of
// the same base layer share a key (for example Metal1 and Metal1.pin), while
// marker/keepout purposes remain distinct.
std::string layoutLayerConnectivityKey(const Layer &layer);

} // namespace ice
