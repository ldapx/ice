#include "ice_openroad/engine.hpp"
#include "ice_openroad/odb_reader.hpp"

#include <array>
#include <clocale>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <tcl.h>

namespace {

std::string jsonEscape(const std::string &text) {
    std::string result;
    result.reserve(text.size() + 8);
    for (const char character : text) {
        switch (character) {
        case '"':
            result += "\\\"";
            break;
        case '\\':
            result += "\\\\";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            result += character;
            break;
        }
    }
    return result;
}

std::filesystem::path resolve(const std::filesystem::path &run_dir, const std::string &value) {
    if (value.empty())
        return {};
    std::filesystem::path path(value);
    if (path.is_relative())
        path = run_dir / path;
    return path.lexically_normal();
}

void printRect(std::ostream &output, const ice::openroad::Rect &rect) {
    output << "{\"x\":" << rect.x << ",\"y\":" << rect.y << ",\"width\":" << rect.width << ",\"height\":" << rect.height
           << "}";
}

void printSummary(std::ostream &output, const ice::openroad::OdbSummary &summary) {
    output << "{\"design_name\":\"" << jsonEscape(summary.design_name) << "\"";
    output << ",\"top_cell\":\"" << jsonEscape(summary.top_cell) << "\"";
    output << ",\"dbu_per_micron\":" << summary.dbu_per_micron;
    output << ",\"die_area\":";
    printRect(output, summary.die_area);
    output << ",\"core_area\":";
    printRect(output, summary.core_area);
    output << ",\"instance_count\":" << summary.instance_count;
    output << ",\"net_count\":" << summary.net_count;
    output << ",\"pin_count\":" << summary.pin_count;
    output << ",\"row_count\":" << summary.row_count;
    output << ",\"special_net_count\":" << summary.special_net_count;
    output << "}";
}

void printHeatMap(std::ostream &output, const ice::openroad::HeatMapData &map) {
    output << "{\"columns\":" << map.columns << ",\"rows\":" << map.rows << ",\"bounds\":";
    printRect(output, map.bounds);
    output << ",\"minimum\":" << map.minimum << ",\"maximum\":" << map.maximum;
    output << ",\"values\":[";
    for (std::size_t index = 0; index < map.values.size(); ++index) {
        if (index)
            output << ",";
        output << map.values[index];
    }
    output << "]}";
}

void printPreview(std::ostream &output, const ice::openroad::OdbPreview &preview) {
    output << "{\"summary\":";
    printSummary(output, preview.summary);
    output << ",\"bounds\":";
    printRect(output, preview.bounds);
    output << ",\"truncated\":" << (preview.truncated ? "true" : "false");
    output << ",\"shapes\":[";
    for (std::size_t index = 0; index < preview.shapes.size(); ++index) {
        if (index)
            output << ",";
        const auto &shape = preview.shapes[index];
        output << "{\"layer\":\"" << jsonEscape(shape.layer) << "\"";
        output << ",\"layer_number\":" << shape.layer_number;
        output << ",\"layer_datatype\":" << shape.layer_datatype;
        output << ",\"kind\":\"" << jsonEscape(shape.kind) << "\"";
        output << ",\"width\":" << shape.width;
        output << ",\"text\":\"" << jsonEscape(shape.text) << "\"";
        output << ",\"points\":[";
        for (std::size_t point = 0; point < shape.points.size(); ++point) {
            if (point)
                output << ",";
            output << "[" << shape.points[point].x << "," << shape.points[point].y << "]";
        }
        output << "]}";
    }
    output << "],\"ports\":[";
    for (std::size_t index = 0; index < preview.ports.size(); ++index) {
        if (index)
            output << ",";
        const auto &port = preview.ports[index];
        output << "{\"name\":\"" << jsonEscape(port.name) << "\"";
        output << ",\"layer\":\"" << jsonEscape(port.layer) << "\"";
        output << ",\"layer_number\":" << port.layer_number;
        output << ",\"layer_datatype\":" << port.layer_datatype;
        output << ",\"rectangle\":";
        printRect(output, port.rectangle);
        output << "}";
    }
    output << "],\"instances\":[";
    for (std::size_t index = 0; index < preview.instances.size(); ++index) {
        if (index)
            output << ",";
        const auto &instance = preview.instances[index];
        output << "{\"name\":\"" << jsonEscape(instance.name) << "\"";
        output << ",\"master\":\"" << jsonEscape(instance.master) << "\"";
        output << ",\"orientation\":\"" << jsonEscape(instance.orientation) << "\"";
        output << ",\"rectangle\":";
        printRect(output, instance.rectangle);
        output << "}";
    }
    output << "]";
    if (preview.placement_density) {
        output << ",\"placement_density\":";
        printHeatMap(output, *preview.placement_density);
    }
    if (preview.routing_congestion) {
        output << ",\"routing_congestion\":";
        printHeatMap(output, *preview.routing_congestion);
    }
    output << "}";
}

bool parseHeatMapKind(const std::string &value, ice::openroad::HeatMapKind &kind) {
    if (value == "placement" || value == "placement-density")
        kind = ice::openroad::HeatMapKind::placement_density;
    else if (value == "routing" || value == "routing-congestion")
        kind = ice::openroad::HeatMapKind::routing_congestion;
    else if (value == "power" || value == "power-density")
        kind = ice::openroad::HeatMapKind::power_density;
    else if (value == "pin" || value == "pin-density")
        kind = ice::openroad::HeatMapKind::pin_density;
    else if (value == "rudy")
        kind = ice::openroad::HeatMapKind::rudy;
    else if (value == "irdrop" || value == "ir-drop")
        kind = ice::openroad::HeatMapKind::ir_drop;
    else
        return false;
    return true;
}

std::vector<std::string> splitLayers(const std::string &value) {
    std::vector<std::string> result;
    std::stringstream stream(value);
    std::string item;
    while (std::getline(stream, item, ',')) {
        const auto first = item.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            continue;
        const auto last = item.find_last_not_of(" \t\r\n");
        result.push_back(item.substr(first, last - first + 1));
    }
    return result;
}

void printJson(const ice::openroad::StageResult &result) {
    std::cout << "{";
    std::cout << "\"success\":" << (result.success ? "true" : "false");
    std::cout << ",\"cancelled\":" << (result.cancelled ? "true" : "false");
    std::cout << ",\"error\":\"" << jsonEscape(result.error) << "\"";
    std::cout << ",\"artifacts\":[";
    for (std::size_t index = 0; index < result.artifacts.size(); ++index) {
        if (index)
            std::cout << ",";
        std::cout << "\"" << jsonEscape(result.artifacts[index].string()) << "\"";
    }
    std::cout << "],\"metrics\":{";
    bool first = true;
    for (const auto &[name, value] : result.metrics) {
        if (!first)
            std::cout << ",";
        first = false;
        std::cout << "\"" << jsonEscape(name) << "\":" << value;
    }
    std::cout << "},\"tools\":{";
    first = true;
    for (const auto &[name, value] : result.tool_versions) {
        if (!first)
            std::cout << ",";
        first = false;
        std::cout << "\"" << jsonEscape(name) << "\":\"" << jsonEscape(value) << "\"";
    }
    std::cout << "}}\n";
}

bool parseStage(const std::string &value, ice::openroad::Stage &stage) {
    if (value == "floorplan")
        stage = ice::openroad::Stage::floorplan;
    else if (value == "placement")
        stage = ice::openroad::Stage::placement;
    else if (value == "cts")
        stage = ice::openroad::Stage::cts;
    else if (value == "routing")
        stage = ice::openroad::Stage::routing;
    else if (value == "final")
        stage = ice::openroad::Stage::final;
    else
        return false;
    return true;
}

} // namespace

int main(int argc, char **argv) {
    std::array locales = {"en_US.UTF-8", "C.UTF-8", "C"};
    for (auto locale : locales) {
        if (std::setlocale(LC_ALL, locale) != nullptr)
            break;
    }
    setenv("OMP_WAIT_POLICY", "passive", 0);
    Tcl_FindExecutable(argv[0]);

    if (argc < 2) {
        std::cerr << "Usage: ice-openroad <floorplan|placement|cts|routing|final|odb-info|odb-preview|heatmap> "
                     "[options]\n";
        return 2;
    }

    const std::string command = argv[1];
    if (command == "odb-info" || command == "odb-preview") {
        if (argc < 3) {
            std::cerr << "Usage: ice-openroad " << command << " <odb> [options]\n";
            return 2;
        }
        int heatmap_bins = 32;
        for (int index = 3; index < argc; ++index) {
            const std::string option = argv[index];
            if (option == "--heatmap-bins" && index + 1 < argc) {
                heatmap_bins = std::stoi(argv[++index]);
                continue;
            }
            std::cerr << "Unknown option: " << option << "\n";
            return 2;
        }
        ice::openroad::OdbReader reader;
        std::string error;
        if (!reader.open(argv[2], error)) {
            std::cerr << error << "\n";
            return 1;
        }
        if (command == "odb-info") {
            printSummary(std::cout, reader.summary());
        } else {
            printPreview(std::cout, reader.preview(heatmap_bins));
        }
        std::cout << "\n";
        return 0;
    }

    if (command == "heatmap") {
        ice::openroad::HeatMapJob heatmap;
        for (int index = 2; index < argc; ++index) {
            const std::string option = argv[index];
            const auto value = [&]() -> std::string {
                if (++index >= argc) {
                    std::cerr << "Missing value for " << option << "\n";
                    std::exit(2);
                }
                return argv[index];
            };
            if (option == "--input-db")
                heatmap.input_db = value();
            else if (option == "--sdc")
                heatmap.sdc = value();
            else if (option == "--liberty")
                heatmap.liberty.push_back(value());
            else if (option == "--output")
                heatmap.output_csv = value();
            else if (option == "--log")
                heatmap.log_file = value();
            else if (option == "--threads")
                heatmap.threads = std::stoi(value());
            else if (option == "--kind") {
                const auto kind = value();
                if (!parseHeatMapKind(kind, heatmap.kind)) {
                    std::cerr << "Unknown heat-map kind: " << kind << "\n";
                    return 2;
                }
            } else {
                std::cerr << "Unknown option: " << option << "\n";
                return 2;
            }
        }
        if (heatmap.input_db.empty() || heatmap.output_csv.empty()) {
            std::cerr << "--input-db and --output are required\n";
            return 2;
        }
        heatmap.input_db = std::filesystem::absolute(heatmap.input_db).lexically_normal();
        heatmap.output_csv = std::filesystem::absolute(heatmap.output_csv).lexically_normal();
        for (auto &liberty : heatmap.liberty)
            liberty = std::filesystem::absolute(liberty).lexically_normal();
        if (!heatmap.sdc.empty())
            heatmap.sdc = std::filesystem::absolute(heatmap.sdc).lexically_normal();
        const auto log_path =
            heatmap.log_file.empty() ? std::filesystem::path("/dev/null") : std::filesystem::absolute(heatmap.log_file);
        ice::openroad::Engine engine;
        std::string error;
        if (!engine.initialize({log_path, {}, heatmap.threads}, error)) {
            std::cout << "{\"success\":false,\"error\":\"" << jsonEscape(error) << "\"}\n";
            return 1;
        }
        const auto result = engine.exportHeatMap(heatmap);
        std::cout << "{\"success\":" << (result.success ? "true" : "false") << ",\"error\":\""
                  << jsonEscape(result.error) << "\",\"csv\":\"" << jsonEscape(result.csv.string()) << "\"}\n";
        return result.success ? 0 : 1;
    }

    ice::openroad::StageJob job;
    if (!parseStage(argv[1], job.stage)) {
        std::cerr << "Unknown stage: " << argv[1] << "\n";
        return 2;
    }

    for (int index = 2; index < argc; ++index) {
        const std::string option = argv[index];
        const auto value = [&]() -> std::string {
            if (++index >= argc) {
                std::cerr << "Missing value for " << option << "\n";
                std::exit(2);
            }
            return argv[index];
        };
        if (option == "--run-dir")
            job.run_dir = value();
        else if (option == "--top")
            job.top = value();
        else if (option == "--pdk")
            job.pdk = value();
        else if (option == "--stack")
            job.stack = value();
        else if (option == "--corner")
            job.corner = value();
        else if (option == "--tech-lef")
            job.tech_lef.push_back(value());
        else if (option == "--cell-lef")
            job.cell_lef.push_back(value());
        else if (option == "--liberty")
            job.liberty.push_back(value());
        else if (option == "--verilog")
            job.verilog = value();
        else if (option == "--sdc")
            job.sdc = value();
        else if (option == "--input-db")
            job.input_db = value();
        else if (option == "--output-db")
            job.output_db = value();
        else if (option == "--output-def")
            job.output_def = value();
        else if (option == "--output-verilog")
            job.output_verilog = value();
        else if (option == "--output-sdc")
            job.output_sdc = value();
        else if (option == "--output-spef")
            job.output_spef = value();
        else if (option == "--output-drc")
            job.output_drc = value();
        else if (option == "--metrics")
            job.metrics_file = value();
        else if (option == "--log")
            job.log_file = value();
        else if (option == "--tapcell-script")
            job.tapcell_script = value();
        else if (option == "--pdn-script")
            job.pdn_script = value();
        else if (option == "--utilization")
            job.options.utilization = std::stod(value());
        else if (option == "--aspect-ratio")
            job.options.aspect_ratio = std::stod(value());
        else if (option == "--core-space")
            job.options.core_space = std::stod(value());
        else if (option == "--die-area") {
            const double x1 = std::stod(value());
            const double y1 = std::stod(value());
            const double x2 = std::stod(value());
            const double y2 = std::stod(value());
            job.options.die_area = ice::openroad::Rect{x1, y1, x2 - x1, y2 - y1};
        } else if (option == "--core-area") {
            const double x1 = std::stod(value());
            const double y1 = std::stod(value());
            const double x2 = std::stod(value());
            const double y2 = std::stod(value());
            job.options.core_area = ice::openroad::Rect{x1, y1, x2 - x1, y2 - y1};
        } else if (option == "--site")
            job.options.site = value();
        else if (option == "--io-horizontal")
            job.options.io_horizontal_layers = splitLayers(value());
        else if (option == "--io-vertical")
            job.options.io_vertical_layers = splitLayers(value());
        else if (option == "--placement-density")
            job.options.placement_density = std::stod(value());
        else if (option == "--timing-driven")
            job.options.timing_driven = true;
        else if (option == "--routability-driven")
            job.options.routability_driven = true;
        else if (option == "--cts-root-buffer")
            job.options.cts_root_buffer = value();
        else if (option == "--cts-buffers")
            job.options.cts_buffers = splitLayers(value());
        else if (option == "--routing-min-layer")
            job.options.routing_min_layer = value();
        else if (option == "--routing-max-layer")
            job.options.routing_max_layer = value();
        else if (option == "--no-antenna-repair")
            job.options.antenna_repair = false;
        else if (option == "--fill-cell")
            job.options.fill_cells.push_back(value());
        else if (option == "--extraction-model")
            job.options.extraction_model = value();
        else if (option == "--threads")
            job.threads = std::stoi(value());
        else {
            std::cerr << "Unknown option: " << option << "\n";
            return 2;
        }
    }

    if (job.run_dir.empty()) {
        std::cerr << "--run-dir is required\n";
        return 2;
    }
    job.run_dir = std::filesystem::absolute(job.run_dir).lexically_normal();
    job.verilog = resolve(job.run_dir, job.verilog.string());
    job.sdc = resolve(job.run_dir, job.sdc.string());
    job.input_db = resolve(job.run_dir, job.input_db.string());
    job.output_db = resolve(job.run_dir, job.output_db.string());
    job.output_def = resolve(job.run_dir, job.output_def.string());
    job.output_verilog = resolve(job.run_dir, job.output_verilog.string());
    job.output_sdc = resolve(job.run_dir, job.output_sdc.string());
    job.output_spef = resolve(job.run_dir, job.output_spef.string());
    job.output_drc = resolve(job.run_dir, job.output_drc.string());
    job.metrics_file = resolve(job.run_dir, job.metrics_file.string());
    job.log_file = resolve(job.run_dir, job.log_file.string());
    job.tapcell_script = resolve(job.run_dir, job.tapcell_script.string());
    job.pdn_script = resolve(job.run_dir, job.pdn_script.string());
    for (auto &lef : job.tech_lef)
        lef = resolve(job.run_dir, lef.string());
    for (auto &lef : job.cell_lef)
        lef = resolve(job.run_dir, lef.string());
    for (auto &liberty : job.liberty)
        liberty = resolve(job.run_dir, liberty.string());

    ice::openroad::Engine engine;
    std::string error;
    if (!engine.initialize({job.log_file, job.metrics_file, job.threads}, error)) {
        printJson({false, false, error, {}, {}, {}});
        return 1;
    }

    const auto result = engine.runStage(job, [](const std::string &line) {
        if (!line.empty())
            std::cout << line << "\n";
    });
    printJson(result);
    return result.success ? 0 : 1;
}
