#pragma once

#include "openvlsilib/core.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace openvlsi {

struct LayoutImportOptions {
    // GDS layer number whose TEXT elements become layout ports. Unset imports text as
    // ordinary text shapes only.
    std::optional<int> portLabelLayer;
    Coordinate portWidth = 1000;
    Coordinate portHeight = 1000;
    // Optional top cell or GDSII structure name. Empty chooses the last
    // unreferenced structure, which matches the common GDS library convention.
    std::string topCell;
    // Optional warning sink for unsupported transforms, missing references,
    // and approximated records.
    std::vector<std::string> *warnings = nullptr;
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
    // Exports the cell as GDSII. Instance hierarchy is preserved; pass
    // flatten=true or use exportFlattenedGds for a single-structure stream.
    static void exportGds(const Project &project, ObjectId cellId, const std::filesystem::path &path);
    // Exports the cell with every instance hierarchy flattened into one cell.
    static void exportFlattenedGds(const Project &project, ObjectId cellId, const std::filesystem::path &path);
    // Exports GDSII or OASIS based on the path extension. Both writers preserve
    // instance hierarchy unless flatten is requested. Returns the top structure
    // name written to the stream.
    static std::string exportStream(const Project &project, ObjectId cellId, const std::filesystem::path &path,
                                    bool flatten = false);
    // Exports port metadata as a minimal LEF macro or DEF design. Geometry of
    // ordinary shapes is intentionally left to GDS.
    static void exportLef(const Project &project, ObjectId cellId, const std::filesystem::path &path);
    static void exportDef(const Project &project, ObjectId cellId, const std::filesystem::path &path);
    // Imports GDSII or OASIS as a hierarchical project. OASIS is read natively,
    // including compressed CBLOCK records.
    static Project importStream(const std::filesystem::path &path, const LayoutImportOptions &options = {});
    static Project importGds(const std::filesystem::path &path, const LayoutImportOptions &options = {});
    static Project importOasis(const std::filesystem::path &path, const LayoutImportOptions &options = {});
    // Returns a copy of the project with the cell's instance hierarchy
    // flattened into that cell. Ports and nets belonging to the top cell are
    // preserved.
    static Project flattenProject(const Project &project, ObjectId cellId);
    // Returns the GDSII structure name used for a cell by the hierarchical
    // writer. Useful for DRC decks that need an explicit top-cell name.
    static std::string gdsStructureName(const std::string &cellName);
    static Project importLef(const std::filesystem::path &path);
    static Project importDef(const std::filesystem::path &path, const DefImportOptions &options = {});
    // Imports a routed DEF as a layout cell in an existing project. Returns the
    // created or replaced cell id. Standard-cell masters are created lazily
    // from the cell LEF for the components referenced by the DEF.
    static ObjectId importDefIntoProject(Project &project, const std::filesystem::path &path,
                                         const DefImportOptions &options = {});
};

} // namespace openvlsi
