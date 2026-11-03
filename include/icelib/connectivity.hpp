#pragma once

#include "icelib/core.hpp"

#include <string>
#include <vector>

namespace ice {

// Read-only geometric net extraction for a layout cell. Objects that carry the
// same LayoutNet id are always merged. Shapes and pins that touch or overlap on
// the same layer are merged as well, providing a net view even before a full
// device extractor is available.
struct LayoutNetGroup {
    ObjectId net = 0;
    std::string name;
    std::vector<ObjectId> shapes;
    std::vector<ObjectId> ports;
    std::vector<ObjectId> connections;
};

std::vector<LayoutNetGroup> extractLayoutNets(const Project &project, ObjectId cellId);

} // namespace ice
