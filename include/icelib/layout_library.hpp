#pragma once

#include "icelib/core.hpp"
#include "icelib/geometry.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ice {

// A layout device library shipped with ICE. Each cell contains flattened
// polygon geometry and can be previewed, imported, and (when a generator is
// present) regenerated from its PCell parameters.
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

// Display metadata for a device-library PCell parameter. Cells with a
// generator regenerate their flattened geometry from these values.
struct LayoutDeviceParameter {
    std::string name;
    std::string value;
    std::string description;
};

// Identifies a PyCell implementation used to regenerate device geometry.
struct LayoutDeviceGenerator {
    std::string module;
    std::string class_name;
};

struct LayoutDevicePort {
    std::string name;
    int layer = 0;
    int datatype = 0;
    Rect rectangle;
    PortDirection direction = PortDirection::inout;
    PortSide side = PortSide::left;
    std::string label;
};

struct LayoutDeviceCell {
    std::string name;
    std::string category;
    std::string source;
    Rect bounds;
    std::vector<LayoutDeviceShape> shapes;
    std::vector<LayoutDevicePort> ports;
    std::vector<LayoutDeviceParameter> parameters;
    std::optional<LayoutDeviceGenerator> generator;
};

struct LayoutDeviceGeometry {
    Rect bounds;
    std::vector<LayoutDeviceShape> shapes;
    std::vector<LayoutDevicePort> ports;
};

struct LayoutDeviceLibrary {
    std::string name;
    std::string pdk;
    // Prefix used for instance cell names, for example "IHP " or "SKY130 ".
    std::string cellPrefix;
    std::vector<LayoutDeviceLayer> layers;
    std::vector<LayoutDeviceCell> cells;

    static LayoutDeviceLibrary load(const std::filesystem::path &path);
};

// Parses the JSON produced by a PDK layout/device_generator.py implementation.
// Bounds are encoded as [min_x, min_y, max_x, max_y] in database units.
LayoutDeviceGeometry parseLayoutDeviceGeometry(const std::string &text);

} // namespace ice
