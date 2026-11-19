#include "icelib/implementation.hpp"

#include "icelib/interchange.hpp"
#include "icelib/operations.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>

namespace ice {
namespace {

std::string nowUtc() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm calendar{};
#ifdef _WIN32
    gmtime_s(&calendar, &time);
#else
    gmtime_r(&time, &calendar);
#endif
    std::ostringstream output;
    output << std::put_time(&calendar, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}

std::string referenceFormatForPath(const std::filesystem::path &path) {
    const auto extension = lowercase(path.extension().string());
    if (extension == ".oas" || extension == ".oasis")
        return "oasis";
    if (extension == ".gds" || extension == ".gdsii")
        return "gds";
    return {};
}

std::filesystem::path findImplementationStream(const std::filesystem::path &runDirectory) {
    for (const auto *candidate :
         {"final/6_final.gds", "final/6_final.gdsii", "final/6_final.oas", "final/6_final.oasis"}) {
        const auto path = runDirectory / candidate;
        std::error_code status;
        if (std::filesystem::is_regular_file(path, status) && !status)
            return path;
    }
    std::error_code status;
    const auto directory = runDirectory / "final";
    if (!std::filesystem::is_directory(directory, status) || status)
        return {};
    std::filesystem::path newest;
    std::filesystem::file_time_type newestTime{};
    for (std::filesystem::directory_iterator iterator(directory, status), end; !status && iterator != end;
         iterator.increment(status)) {
        if (!iterator->is_regular_file())
            continue;
        if (referenceFormatForPath(iterator->path()).empty())
            continue;
        const auto time = iterator->last_write_time(status);
        if (status)
            continue;
        if (newest.empty() || time > newestTime) {
            newest = iterator->path();
            newestTime = time;
        }
    }
    return newest;
}

std::filesystem::path findImplementationDef(const std::filesystem::path &runDirectory) {
    for (const auto *candidate :
         {"def/6_final.def", "def/5_route.def", "def/4_cts.def", "def/3_place.def", "def/2_floorplan.def"}) {
        const auto path = runDirectory / candidate;
        std::error_code status;
        if (std::filesystem::is_regular_file(path, status) && !status)
            return path;
    }
    return {};
}

ObjectId findImportedLayer(const Project &project, int number, int datatype, const std::string &pdk,
                           const std::string &stack) {
    ObjectId fallback = 0;
    for (const auto &[id, layer] : project.layers) {
        if (layer.layout != 0 || layer.number != number || layer.datatype != datatype)
            continue;
        if (!layer.pdk.empty() && layer.pdk != pdk)
            continue;
        if (!layer.stack.empty() && layer.stack != stack)
            continue;
        // Prefer the layer that belongs to the target PDK/stack. A generic
        // layer is still usable when no PDK-specific copy exists.
        if (layer.pdk == pdk && layer.stack == stack)
            return id;
        if (fallback == 0)
            fallback = id;
    }
    return fallback;
}

// The final stream is flattened for a compact reusable device. The DEF
// checkpoint still carries the top-level pin names, directions, nets, and
// geometry, so import those into the same cell as its logical interface.
int copyDefPins(Project &project, LayoutCell &cell, const std::filesystem::path &defPath, const PackageSpec &package,
                const std::string &pdk, const std::string &stack) {
    if (!package.pdk)
        return 0;
    DefImportOptions options;
    options.pdk = pdk;
    options.stack = stack;
    options.cellName = cell.name + " pins";
    options.importMasters = false;
    options.importRoutes = false;
    for (const auto &layer : package.pdk->layers)
        options.layers[lowercase(layer.name)] = InterchangeLayerDefinition{layer.number, layer.datatype};

    Project temp;
    temp.identity = options.cellName;
    temp.name = options.cellName;
    const auto tempCellId = LayoutInterchange::importDefIntoProject(temp, defPath, options);
    const auto tempCell = temp.layoutCells.find(tempCellId);
    if (tempCell == temp.layoutCells.end())
        return 0;

    std::map<ObjectId, ObjectId> layers;
    const auto layerFor = [&](ObjectId tempLayerId) -> ObjectId {
        if (const auto existing = layers.find(tempLayerId); existing != layers.end())
            return existing->second;
        const auto tempLayer = temp.layers.find(tempLayerId);
        if (tempLayer == temp.layers.end())
            return 0;
        auto target = findImportedLayer(project, tempLayer->second.number, tempLayer->second.datatype, pdk, stack);
        if (target == 0)
            target = addLayer(project, tempLayer->second.name, tempLayer->second.number, 0, pdk,
                              tempLayer->second.datatype, stack);
        layers.emplace(tempLayerId, target);
        return target;
    };

    const auto normalizedName = [](std::string value) {
        for (auto &character : value)
            character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        return value;
    };
    std::map<std::string, ObjectId> nets;
    for (const auto &[id, net] : cell.nets)
        nets[normalizedName(net.name)] = id;
    const auto ensureNet = [&](const std::string &name) -> ObjectId {
        if (name.empty())
            return 0;
        const auto key = normalizedName(name);
        if (const auto existing = nets.find(key); existing != nets.end())
            return existing->second;
        const auto id = createLayoutNet(project, cell.id, name);
        nets.emplace(key, id);
        return id;
    };

    std::set<std::string> portNames;
    for (const auto &[_, port] : cell.ports)
        portNames.insert(normalizedName(port.name));
    int copied = 0;
    for (const auto &[_, port] : tempCell->second.ports) {
        if (!port.rectangle.valid())
            continue;
        const auto layer = layerFor(port.layer);
        if (layer == 0)
            continue;
        const auto baseName = port.name.empty() ? port.label : port.name;
        if (baseName.empty())
            continue;
        auto name = baseName;
        auto key = normalizedName(name);
        for (int suffix = 2; portNames.contains(key); ++suffix) {
            name = baseName + "_" + std::to_string(suffix);
            key = normalizedName(name);
        }
        auto copy = port;
        copy.id = project.nextId();
        copy.name = name;
        copy.label = port.label.empty() ? name : port.label;
        copy.layer = layer;
        copy.net = 0;
        copy.sourceInstance = 0;
        copy.sourcePort = 0;
        copy.schematicInstance = 0;
        if (port.net != 0) {
            if (const auto net = tempCell->second.nets.find(port.net); net != tempCell->second.nets.end())
                copy.net = ensureNet(net->second.name);
        }
        portNames.insert(key);
        cell.ports.emplace(copy.id, std::move(copy));
        ++copied;
    }
    return copied;
}

void copyFlattenedStream(Project &project, LayoutCell &cell, const Project &imported, const PackageSpec &package,
                         const std::string &pdk, const std::string &stack) {
    if (imported.layoutCells.empty())
        throw ProjectError("implementation.device", "The flattened device stream contains no layout cell");
    const auto &source = imported.layoutCells.begin()->second;
    std::map<ObjectId, ObjectId> layers;
    for (const auto &[id, layer] : imported.layers) {
        auto target = findImportedLayer(project, layer.number, layer.datatype, pdk, stack);
        if (target == 0) {
            std::string name = layer.name;
            if (package.pdk) {
                for (const auto &packageLayer : package.pdk->layers) {
                    if (packageLayer.number == layer.number && packageLayer.datatype == layer.datatype) {
                        name = packageLayer.name;
                        break;
                    }
                }
            }
            target = addLayer(project, name, layer.number, 0, pdk, layer.datatype, stack);
        }
        layers[id] = target;
    }
    for (const auto &[_, shape] : source.shapes) {
        const auto layer = layers.find(shape.layer);
        if (layer == layers.end())
            continue;
        auto copy = shape;
        copy.id = project.nextId();
        copy.layer = layer->second;
        copy.net.reset();
        copy.routedConnection = 0;
        cell.shapes.emplace(copy.id, std::move(copy));
    }
    for (const auto &[_, port] : source.ports) {
        const auto layer = layers.find(port.layer);
        if (layer == layers.end())
            continue;
        auto copy = port;
        copy.id = project.nextId();
        copy.layer = layer->second;
        copy.net = 0;
        copy.sourceInstance = 0;
        copy.sourcePort = 0;
        cell.ports.emplace(copy.id, std::move(copy));
    }
}

std::string uniqueLayoutName(const Project &project, const std::string &name) {
    const auto taken = [&](const std::string &candidate) {
        for (const auto &[_, cell] : project.layoutCells)
            if (cell.name == candidate)
                return true;
        for (const auto &[_, procedural] : project.proceduralLayouts)
            if (procedural.name == candidate)
                return true;
        return false;
    };
    if (!taken(name))
        return name;
    for (int suffix = 2;; ++suffix) {
        const auto candidate = name + " " + std::to_string(suffix);
        if (!taken(candidate))
            return candidate;
    }
}

} // namespace

ImplementationLayoutImport importImplementationLayout(Project &project, ImplementationRun &run,
                                                      const PackageSpec &package,
                                                      const std::filesystem::path &projectRoot,
                                                      const std::string &layoutName, bool replace) {
    if (!package.pdk)
        throw ProjectError("implementation.pdk", "Package does not declare a PDK");
    if (run.directory.empty())
        throw ProjectError("implementation.import", "Implementation run has no build directory");

    const auto stream = findImplementationStream(projectRoot / run.directory);
    if (stream.empty()) {
        auto message = "No final GDS/OASIS stream exists for run '" + run.name +
                       "'. The final stage did not produce one. Check the PDK GDS decks, KLayout, and "
                       "the final-stage log, then run implementation-export-gds.";
        // The final stage treats a failed stream export as a warning, so the
        // recorded reason is the only way to tell the user what to fix.
        std::ifstream log(projectRoot / run.directory / "logs" / "06-gds.log", std::ios::binary);
        if (log) {
            std::ostringstream text;
            text << log.rdbuf();
            auto details = text.str();
            while (!details.empty() && (details.back() == '\n' || details.back() == '\r' || details.back() == ' '))
                details.pop_back();
            if (details.size() > 1200)
                details.erase(0, details.size() - 1200);
            if (!details.empty())
                message += "\n\nLast GDS export:\n" + details;
        }
        throw ProjectError("implementation.reference", message);
    }

    const auto format = referenceFormatForPath(stream);
    if (format.empty())
        throw ProjectError("implementation.reference", "Unsupported layout reference format: " + stream.string());

    auto relative = stream.lexically_relative(projectRoot);
    if (relative.empty() || relative.generic_string().starts_with(".."))
        relative = stream;
    const auto referencePath = relative.generic_string();
    const auto deviceName = "digital:" + run.name;

    ObjectId cellId = 0;
    if (replace) {
        if (!layoutName.empty()) {
            for (const auto &[id, cell] : project.layoutCells) {
                if (cell.name == layoutName) {
                    cellId = id;
                    break;
                }
            }
        }
        if (cellId == 0 && run.resultLayout != 0 && project.layoutCells.contains(run.resultLayout))
            cellId = run.resultLayout;
        if (cellId == 0) {
            for (const auto &[id, cell] : project.layoutCells) {
                if (cell.reference && cell.reference->path == referencePath &&
                    cell.reference->topCell == run.config.top) {
                    cellId = id;
                    break;
                }
            }
        }
        if (cellId == 0) {
            for (const auto &[id, cell] : project.layoutCells) {
                if (cell.device == deviceName) {
                    cellId = id;
                    break;
                }
            }
        }
    }
    const auto pdk = package.pdk->id.empty() ? package.name : package.pdk->id;
    const auto stack = resolvePdkStack(*package.pdk, run.config.stack);
    if (cellId == 0) {
        const auto name = uniqueLayoutName(project, layoutName.empty() ? run.name + " device" : layoutName);
        cellId = createLayoutCell(project, name, pdk, stack);
    }

    auto &cell = project.layoutCells.at(cellId);
    if (layoutName.empty() && cell.name == run.name + " layout")
        cell.name = uniqueLayoutName(project, run.name + " device");
    cell.shapes.clear();
    cell.ports.clear();
    cell.nets.clear();
    cell.instances.clear();
    cell.plannedDevices.clear();
    cell.plannedConnections.clear();
    cell.generatedBy = 0;
    // The imported stream becomes a reusable library device. The user
    // instances it from the Devices tab instead of getting a top-level layout.
    cell.library = true;
    cell.readOnly = true;
    cell.device = deviceName;
    cell.pdk = pdk;
    cell.stack = stack;
    cell.reference.reset();
    LayoutImportOptions importOptions;
    importOptions.topCell = run.config.top;
    auto imported = LayoutInterchange::importStream(stream, importOptions);
    if (imported.layoutCells.empty())
        throw ProjectError("implementation.device", "The final device stream contains no layout cell");
    auto flattened = LayoutInterchange::flattenProject(imported, imported.layoutCells.begin()->first);
    copyFlattenedStream(project, cell, flattened, package, pdk, stack);

    std::vector<Diagnostic> pinDiagnostics;
    const auto def = findImplementationDef(projectRoot / run.directory);
    if (!def.empty()) {
        // Keep the import transactional if a malformed DEF pin section fails
        // after some ports/nets were already copied.
        const auto portsBackup = cell.ports;
        const auto netsBackup = cell.nets;
        try {
            const auto pins = copyDefPins(project, cell, def, package, pdk, stack);
            if (pins == 0)
                pinDiagnostics.push_back({Diagnostic::Severity::warning, "implementation.pins",
                                          "No top-level pin geometry was found in " + def.filename().string() +
                                              "; the digital device has no ports.",
                                          std::nullopt});
        } catch (const std::exception &error) {
            cell.ports = portsBackup;
            cell.nets = netsBackup;
            pinDiagnostics.push_back(
                {Diagnostic::Severity::warning, "implementation.pins",
                 std::string("Unable to import top-level pins from ") + def.filename().string() + ": " + error.what(),
                 std::nullopt});
        }
    } else {
        pinDiagnostics.push_back({Diagnostic::Severity::warning, "implementation.pins",
                                  "No DEF checkpoint was found; the digital device has no top-level ports.",
                                  std::nullopt});
    }

    run.resultLayout = cellId;
    run.updatedAt = nowUtc();
    const auto artifact = stream.lexically_relative(projectRoot).generic_string();
    if (!artifact.empty() && !artifact.starts_with("..") &&
        std::find(run.artifacts.begin(), run.artifacts.end(), artifact) == run.artifacts.end())
        run.artifacts.push_back(artifact);

    ImplementationLayoutImport result;
    result.layout = cellId;
    result.referencePath = stream;
    result.diagnostics = std::move(pinDiagnostics);
    return result;
}

} // namespace ice
