#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ice::openroad {

enum class Stage { floorplan, placement, cts, routing, final };

struct Rect {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
};

struct MacroPlacement {
    std::string cell;
    double x = 0.0;
    double y = 0.0;
    std::string orientation;
};

struct StageOptions {
    // Floorplan (dimensions in microns).
    double utilization = 0.5;
    double aspect_ratio = 1.0;
    double core_space = 2.0;
    std::optional<Rect> die_area;
    std::optional<Rect> core_area;
    std::string site;
    std::vector<std::string> io_horizontal_layers;
    std::vector<std::string> io_vertical_layers;
    std::vector<MacroPlacement> macros;

    // Placement.
    double placement_density = 0.0;
    bool timing_driven = false;
    bool routability_driven = false;

    // CTS.
    bool cts_enabled = true;
    std::string cts_root_buffer;
    std::vector<std::string> cts_buffers;

    // Routing.
    std::string routing_min_layer;
    std::string routing_max_layer;
    bool antenna_repair = true;
    std::vector<std::string> fill_cells;

    // Final.
    std::string extraction_model;
};

struct StageJob {
    Stage stage = Stage::floorplan;
    std::filesystem::path run_dir;
    std::string top;
    std::string pdk;
    std::string stack;
    std::string corner;

    std::vector<std::filesystem::path> tech_lef;
    std::vector<std::filesystem::path> cell_lef;
    std::vector<std::filesystem::path> liberty;

    std::filesystem::path verilog;
    std::filesystem::path sdc;
    std::filesystem::path input_db;
    std::filesystem::path output_db;
    std::filesystem::path output_def;
    std::filesystem::path output_verilog;
    std::filesystem::path output_sdc;
    std::filesystem::path output_spef;
    std::filesystem::path output_drc;
    std::filesystem::path metrics_file;
    std::filesystem::path log_file;
    std::filesystem::path tapcell_script;
    std::filesystem::path pdn_script;

    StageOptions options;
    int threads = 1;
};

struct StageResult {
    bool success = false;
    bool cancelled = false;
    std::string error;
    std::map<std::string, double> metrics;
    std::vector<std::filesystem::path> artifacts;
    std::map<std::string, std::string> tool_versions;
};

} // namespace ice::openroad
