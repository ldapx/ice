#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ice_openroad/types.hpp"

namespace ice::openroad {

enum class HeatMapKind {
    placement_density,
    routing_congestion,
    power_density,
    pin_density,
    rudy,
    ir_drop,
};

struct OdbSummary {
    std::string design_name;
    std::string top_cell;
    Rect die_area;
    Rect core_area;
    std::uint64_t instance_count = 0;
    std::uint64_t net_count = 0;
    std::uint64_t pin_count = 0;
    std::uint64_t row_count = 0;
    std::uint64_t special_net_count = 0;
    int dbu_per_micron = 0;
};

struct PreviewPoint {
    double x = 0.0;
    double y = 0.0;
};

struct PreviewShape {
    std::string layer;
    int layer_number = 0;
    int layer_datatype = 0;
    // polygon, path, or text. ODB routed shapes are emitted as polygons.
    std::string kind = "polygon";
    std::vector<PreviewPoint> points;
    double width = 0.0;
    std::string text;
};

struct PreviewPort {
    std::string name;
    std::string layer;
    int layer_number = 0;
    int layer_datatype = 0;
    Rect rectangle;
};

struct PreviewInstance {
    std::string name;
    std::string master;
    std::string orientation;
    Rect rectangle;
};

struct HeatMapData {
    HeatMapKind kind = HeatMapKind::placement_density;
    int columns = 0;
    int rows = 0;
    Rect bounds;
    std::vector<double> values;
    double minimum = 0.0;
    double maximum = 0.0;
};

struct OdbPreview {
    OdbSummary summary;
    Rect bounds;
    std::vector<PreviewShape> shapes;
    std::vector<PreviewPort> ports;
    std::vector<PreviewInstance> instances;
    std::optional<HeatMapData> placement_density;
    std::optional<HeatMapData> routing_congestion;
    bool truncated = false;
};

// Lightweight read-only OpenDB reader. It deliberately does not initialize
// the OpenROAD modules and links only against OpenDB.
class OdbReader {
  public:
    OdbReader();
    ~OdbReader();

    OdbReader(const OdbReader &) = delete;
    OdbReader &operator=(const OdbReader &) = delete;

    bool open(const std::filesystem::path &path, std::string &error);
    void close();
    bool isOpen() const;

    OdbSummary summary() const;
    OdbPreview preview(int heatmap_bins = 32) const;
    std::optional<HeatMapData> placementDensity(int bins_x, int bins_y) const;
    std::optional<HeatMapData> routingCongestion(int bins_x, int bins_y) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ice::openroad
