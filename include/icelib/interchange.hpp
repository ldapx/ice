#pragma once

#include "icelib/core.hpp"

#include <filesystem>
#include <map>
#include <optional>

namespace ice {

struct LayoutImportOptions {
    // GDS layer number whose TEXT elements become pins. Unset imports text as
    // ordinary text shapes only.
    std::optional<int> pinLabelLayer;
    Coordinate pinWidth = 1000;
    Coordinate pinHeight = 1000;
};

// Physical layer lookup for DEF/LEF imports. Keys are lower-case layer names
// so package manifests ("metal1") and DEF names ("Metal1") match.
struct InterchangeLayerDefinition {
    int number = 0;
    int datatype = 0;
};

struct DefImportOptions {
    // Optional LEF sources. The technology LEF supplies routing widths and via
    // cut layers; the cell LEF supplies macro sizes and pin geometry.
    std::filesystem::path techLef;
    std::filesystem::path cellLef;
    std::map<std::string, InterchangeLayerDefinition> layers;
    std::string pdk = genericPdkId;
    std::string stack;
    // Empty uses the DEF DESIGN name.
    std::string cellName;
    // Non-zero replaces the contents of this existing layout cell.
    ObjectId replaceCell = 0;
    bool importMasters = true;
    bool importRoutes = true;
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
    static Project importDef(const std::filesystem::path &path, const DefImportOptions &options = {});
    // Imports a routed DEF as a layout cell in an existing project. Returns the
    // created or replaced cell id. Standard-cell masters are created lazily
    // from the cell LEF for the components referenced by the DEF.
    static ObjectId importDefIntoProject(Project &project, const std::filesystem::path &path,
                                         const DefImportOptions &options = {});
};

} // namespace ice
