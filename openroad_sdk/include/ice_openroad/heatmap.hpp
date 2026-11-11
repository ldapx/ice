#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "ice_openroad/odb_reader.hpp"

namespace ice::openroad {

struct HeatMapJob {
    HeatMapKind kind = HeatMapKind::placement_density;
    std::filesystem::path input_db;
    std::filesystem::path sdc;
    std::vector<std::filesystem::path> liberty;
    std::filesystem::path output_csv;
    std::filesystem::path log_file;
    int threads = 1;
};

struct HeatMapResult {
    bool success = false;
    std::string error;
    std::filesystem::path csv;
};

} // namespace ice::openroad
