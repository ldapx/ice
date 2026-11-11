#include "icelib/implementation.hpp"

#include "icelib/interchange.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iomanip>
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

void addDiagnostic(std::vector<Diagnostic> &diagnostics, Diagnostic::Severity severity, std::string code,
                   std::string message) {
    diagnostics.push_back({severity, std::move(code), std::move(message), std::nullopt});
}

} // namespace

ImplementationLayoutImport importImplementationLayout(Project &project, ImplementationRun &run,
                                                      const PackageSpec &package,
                                                      const std::filesystem::path &projectRoot,
                                                      const std::string &layoutName, bool replace) {
    ImplementationLayoutImport result;
    if (!package.pdk) {
        addDiagnostic(result.diagnostics, Diagnostic::Severity::error, "implementation.pdk",
                      "Package does not declare a PDK");
        return result;
    }
    const auto runDirectory = projectRoot / run.directory;
    for (const auto *candidate :
         {"def/6_final.def", "def/5_route.def", "def/4_cts.def", "def/3_place.def", "def/2_floorplan.def"}) {
        const auto path = runDirectory / candidate;
        std::error_code status;
        if (std::filesystem::is_regular_file(path, status) && !status) {
            result.defPath = path;
            break;
        }
    }
    if (result.defPath.empty()) {
        addDiagnostic(result.diagnostics, Diagnostic::Severity::error, "implementation.import",
                      "No DEF checkpoint exists for run '" + run.name + "'. Run at least the floorplan stage first.");
        return result;
    }

    std::vector<Diagnostic> platformDiagnostics;
    const auto platform = resolveImplementationPlatform(package, run.config.stack, platformDiagnostics);
    for (const auto &diagnostic : platformDiagnostics) {
        if (diagnostic.severity == Diagnostic::Severity::error)
            result.diagnostics.push_back(diagnostic);
    }
    if (!result.diagnostics.empty())
        return result;

    DefImportOptions options;
    options.techLef = platform.techLef;
    options.cellLef = platform.cellLef;
    options.pdk = package.pdk->id.empty() ? package.name : package.pdk->id;
    options.stack = platform.stack.empty() ? run.config.stack : platform.stack;
    for (const auto &layer : package.pdk->layers)
        options.layers[lowercase(layer.name)] = {layer.number, layer.datatype};
    options.cellName = layoutName.empty() ? run.name + " layout" : layoutName;
    if (replace) {
        if (!layoutName.empty()) {
            for (const auto &[id, cell] : project.layoutCells) {
                if (cell.name == layoutName) {
                    options.replaceCell = id;
                    break;
                }
            }
        }
        if (options.replaceCell == 0 && run.resultLayout != 0 && project.layoutCells.contains(run.resultLayout))
            options.replaceCell = run.resultLayout;
    }

    result.layout = LayoutInterchange::importDefIntoProject(project, result.defPath, options);
    run.resultLayout = result.layout;
    run.updatedAt = nowUtc();
    const auto artifact = result.defPath.lexically_relative(projectRoot).generic_string();
    if (std::find(run.artifacts.begin(), run.artifacts.end(), artifact) == run.artifacts.end())
        run.artifacts.push_back(artifact);
    return result;
}

} // namespace ice
