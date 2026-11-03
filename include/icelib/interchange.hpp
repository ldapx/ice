#pragma once

#include "icelib/core.hpp"

#include <filesystem>
#include <optional>

namespace ice {

struct LayoutImportOptions {
    // GDS layer number whose TEXT elements become pins. Unset imports text as
    // ordinary text shapes only.
    std::optional<int> pinLabelLayer;
    Coordinate pinWidth = 1000;
    Coordinate pinHeight = 1000;
};

class LayoutInterchange {
  public:
    static void exportGds(const Project &project, ObjectId cellId, const std::filesystem::path &path);
    // Exports the cell with every instance hierarchy flattened into one cell.
    static void exportFlattenedGds(const Project &project, ObjectId cellId, const std::filesystem::path &path);
    // Exports pin metadata as a minimal LEF macro or DEF design. Geometry of
    // ordinary shapes is intentionally left to GDS.
    static void exportLef(const Project &project, ObjectId cellId, const std::filesystem::path &path);
    static void exportDef(const Project &project, ObjectId cellId, const std::filesystem::path &path);
    static Project importGds(const std::filesystem::path &path, const LayoutImportOptions &options = {});
    static Project importLef(const std::filesystem::path &path);
    static Project importDef(const std::filesystem::path &path);
};

} // namespace ice
