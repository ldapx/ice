#pragma once

#include "icelib/core.hpp"

#include <filesystem>

namespace ice {

class LayoutInterchange {
  public:
    static void exportGds(const Project &project, ObjectId cellId, const std::filesystem::path &path);
    // Exports the cell with every instance hierarchy flattened into one cell.
    static void exportFlattenedGds(const Project &project, ObjectId cellId, const std::filesystem::path &path);
    static Project importGds(const std::filesystem::path &path);
};

} // namespace ice
