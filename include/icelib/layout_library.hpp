#pragma once

#include "icelib/core.hpp"
#include "icelib/geometry.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace ice {

// A read-only layout device library shipped with ICE. Each cell contains
// flattened polygon geometry and can be previewed or imported into a project.
struct LayoutDeviceLayer {
    int number = 0;
    int datatype = 0;
    std::string name;
};

struct LayoutDeviceShape {
    int layer = 0;
    int datatype = 0;
    std::vector<Point> polygon;
};

struct LayoutDeviceCell {
    std::string name;
    std::string category;
    std::string source;
    Rect bounds;
    std::vector<LayoutDeviceShape> shapes;
};

struct LayoutDeviceLibrary {
    std::string name;
    std::string pdk;
    std::vector<LayoutDeviceLayer> layers;
    std::vector<LayoutDeviceCell> cells;

    static LayoutDeviceLibrary load(const std::filesystem::path &path);
};

} // namespace ice
