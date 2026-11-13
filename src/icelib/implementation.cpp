#include "icelib/implementation.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <system_error>

namespace ice {
namespace {

using json = nlohmann::ordered_json;

constexpr const char *kImplementationFormat = "ice-implementation";

const std::vector<ImplementationStage> kStages = {
    ImplementationStage::created,   ImplementationStage::elaboration, ImplementationStage::synthesis,
    ImplementationStage::floorplan, ImplementationStage::placement,   ImplementationStage::cts,
    ImplementationStage::routing,   ImplementationStage::final,
};

const char *stageStatusName(ImplementationStageStatus status) {
    switch (status) {
    case ImplementationStageStatus::pending:
        return "pending";
    case ImplementationStageStatus::running:
        return "running";
    case ImplementationStageStatus::succeeded:
        return "succeeded";
    case ImplementationStageStatus::failed:
        return "failed";
    case ImplementationStageStatus::skipped:
        return "skipped";
    case ImplementationStageStatus::cancelled:
        return "cancelled";
    }
    return "pending";
}

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

std::string hashText(const std::string &text) {
    constexpr std::uint64_t offset = 1469598103934665603ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    std::uint64_t hash = offset;
    for (const unsigned char character : text) {
        hash ^= character;
        hash *= prime;
    }
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}

std::string safeStem(const std::string &name) {
    std::string stem;
    stem.reserve(name.size());
    for (const unsigned char character : name) {
        if (std::isalnum(character) || character == '-' || character == '_' || character == ' ' || character == '.')
            stem.push_back(static_cast<char>(character));
        else
            stem.push_back('_');
    }
    while (!stem.empty() && stem.front() == '.')
        stem.erase(stem.begin());
    if (stem.empty())
        stem = "implementation";
    return stem;
}

bool validIdentifier(const std::string &text) {
    if (text.empty())
        return false;
    const auto first = static_cast<unsigned char>(text.front());
    if (!std::isalpha(first) && first != '_')
        return false;
    for (const unsigned char character : text) {
        if (!std::isalnum(character) && character != '_' && character != '$')
            return false;
    }
    return true;
}

json rectJson(const Rect &rect) {
    return {{"x", rect.x}, {"y", rect.y}, {"width", rect.width}, {"height", rect.height}};
}

std::optional<Rect> parseRect(const json &value) {
    if (!value.is_object())
        return std::nullopt;
    Rect rect{value.value("x", Coordinate{0}), value.value("y", Coordinate{0}), value.value("width", Coordinate{0}),
              value.value("height", Coordinate{0})};
    if (!rect.valid())
        return std::nullopt;
    return rect;
}

json configJson(const ImplementationConfig &config) {
    json sources = json::array();
    for (const auto &source : config.sources)
        sources.push_back(source.generic_string());
    json sourceCells = json::array();
    for (const auto sourceCell : config.sourceCells)
        sourceCells.push_back(sourceCell);
    json macros = json::array();
    for (const auto &macro : config.macros)
        macros.push_back({{"cell", macro.cell}, {"x", macro.x}, {"y", macro.y}, {"orientation", macro.orientation}});

    json result = {
        {"name", config.name},
        {"top", config.top},
        {"pdk", config.pdk},
        {"stack", config.stack},
        {"standard_cell_library", config.standardCellLibrary},
        {"corner", config.corner},
        {"sources", std::move(sources)},
        {"source_cells", std::move(sourceCells)},
        {"sdc", config.sdc.generic_string()},
        {"utilization", config.utilization},
        {"aspect_ratio", config.aspectRatio},
        {"site", config.site},
        {"io_horizontal_layers", config.ioHorizontalLayers},
        {"io_vertical_layers", config.ioVerticalLayers},
        {"macros", std::move(macros)},
        {"placement_density", config.placementDensity},
        {"timing_driven", config.timingDriven},
        {"routability_driven", config.routabilityDriven},
        {"cts_enabled", config.ctsEnabled},
        {"cts_root_buffer", config.ctsRootBuffer},
        {"cts_buffers", config.ctsBuffers},
        {"routing_min_layer", config.routingMinLayer},
        {"routing_max_layer", config.routingMaxLayer},
        {"antenna_repair", config.antennaRepair},
        {"backend", config.backend},
        {"threads", config.threads},
        {"seed", config.seed},
        {"timeout_seconds", config.timeoutSeconds},
    };
    if (config.dieArea)
        result["die_area"] = rectJson(*config.dieArea);
    if (config.coreArea)
        result["core_area"] = rectJson(*config.coreArea);
    return result;
}

ImplementationConfig configFromJson(const json &value) {
    ImplementationConfig config;
    config.name = value.value("name", std::string{});
    config.top = value.value("top", std::string{});
    config.pdk = value.value("pdk", std::string{genericPdkId});
    if (config.pdk.empty())
        config.pdk = genericPdkId;
    config.stack = value.value("stack", std::string{});
    config.standardCellLibrary = value.value("standard_cell_library", std::string{});
    config.corner = value.value("corner", std::string{"tt"});
    if (value.contains("sources")) {
        for (const auto &source : value.at("sources")) {
            if (source.is_string())
                config.sources.emplace_back(source.get<std::string>());
        }
    }
    if (value.contains("source_cells")) {
        for (const auto &sourceCell : value.at("source_cells")) {
            if (sourceCell.is_number_unsigned() || sourceCell.is_number_integer())
                config.sourceCells.push_back(sourceCell.get<ObjectId>());
        }
    }
    config.sdc = value.value("sdc", std::string{});
    if (value.contains("die_area"))
        config.dieArea = parseRect(value.at("die_area"));
    if (value.contains("core_area"))
        config.coreArea = parseRect(value.at("core_area"));
    config.utilization = value.value("utilization", 0.5);
    config.aspectRatio = value.value("aspect_ratio", 1.0);
    config.site = value.value("site", std::string{});
    if (value.contains("io_horizontal_layers")) {
        for (const auto &layer : value.at("io_horizontal_layers"))
            if (layer.is_string())
                config.ioHorizontalLayers.push_back(layer.get<std::string>());
    }
    if (value.contains("io_vertical_layers")) {
        for (const auto &layer : value.at("io_vertical_layers"))
            if (layer.is_string())
                config.ioVerticalLayers.push_back(layer.get<std::string>());
    }
    if (value.contains("macros")) {
        for (const auto &macro : value.at("macros")) {
            if (!macro.is_object())
                continue;
            config.macros.push_back({macro.value("cell", std::string{}), macro.value("x", Coordinate{0}),
                                     macro.value("y", Coordinate{0}), macro.value("orientation", std::string{"r0"})});
        }
    }
    config.placementDensity = value.value("placement_density", 0.0);
    config.timingDriven = value.value("timing_driven", false);
    config.routabilityDriven = value.value("routability_driven", false);
    config.ctsEnabled = value.value("cts_enabled", true);
    config.ctsRootBuffer = value.value("cts_root_buffer", std::string{});
    if (value.contains("cts_buffers")) {
        for (const auto &buffer : value.at("cts_buffers"))
            if (buffer.is_string())
                config.ctsBuffers.push_back(buffer.get<std::string>());
    }
    config.routingMinLayer = value.value("routing_min_layer", std::string{});
    config.routingMaxLayer = value.value("routing_max_layer", std::string{});
    config.antennaRepair = value.value("antenna_repair", true);
    config.backend = value.value("backend", std::string{"openroad"});
    config.threads = value.value("threads", 1);
    config.seed = value.value("seed", std::uint32_t{1});
    config.timeoutSeconds = value.value("timeout_seconds", 3600);
    return config;
}

json stageJson(const ImplementationStageState &stage) {
    json result = {{"stage", implementationStageName(stage.stage)},
                   {"status", stageStatusName(stage.status)},
                   {"artifacts", stage.artifacts}};
    if (!stage.startedAt.empty())
        result["started_at"] = stage.startedAt;
    if (!stage.finishedAt.empty())
        result["finished_at"] = stage.finishedAt;
    if (!stage.error.empty())
        result["error"] = stage.error;
    return result;
}

ImplementationStageState stageFromJson(const json &value) {
    ImplementationStageState stage;
    if (const auto parsed = parseImplementationStage(value.value("stage", std::string{})))
        stage.stage = *parsed;
    if (const auto parsed = parseImplementationStageStatus(value.value("status", std::string{})))
        stage.status = *parsed;
    stage.startedAt = value.value("started_at", std::string{});
    stage.finishedAt = value.value("finished_at", std::string{});
    stage.error = value.value("error", std::string{});
    if (value.contains("artifacts")) {
        for (const auto &artifact : value.at("artifacts"))
            if (artifact.is_string())
                stage.artifacts.push_back(artifact.get<std::string>());
    }
    return stage;
}

std::string tclQuote(const std::string &value) {
    std::string result = "\"";
    for (const char character : value) {
        if (character == '\\' || character == '"' || character == '$' || character == '[' || character == ']')
            result.push_back('\\');
        result.push_back(character);
    }
    result.push_back('"');
    return result;
}

std::string tclPath(const std::filesystem::path &path) { return tclQuote(path.generic_string()); }

std::string tclList(const std::vector<std::string> &values) {
    std::string result = "[list";
    for (const auto &value : values)
        result += " " + tclQuote(value);
    result += "]";
    return result;
}

void writeText(const std::filesystem::path &path, const std::string &text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        throw ProjectError("implementation.write", "Unable to write " + path.string());
    output << text;
}

std::filesystem::path uniqueDestination(const std::filesystem::path &directory, const std::string &stem,
                                        const std::string &extension, std::set<std::string> &used) {
    auto candidate = stem;
    int suffix = 1;
    while (!used.insert(candidate).second)
        candidate = stem + "-" + std::to_string(suffix++);
    return directory / (candidate + extension);
}

std::optional<std::filesystem::path> findExecutable(const std::string &name) {
    const auto check = [](const std::filesystem::path &path) -> std::optional<std::filesystem::path> {
        std::error_code status;
        if (std::filesystem::is_regular_file(path, status) && !status)
            return path;
        return std::nullopt;
    };
    std::string environmentName = "ICE_";
    for (const char character : name)
        environmentName.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(character))));
    if (const auto *overridePath = std::getenv(environmentName.c_str()); overridePath && *overridePath) {
        if (const auto found = check(overridePath))
            return found;
    }
    if (const auto *path = std::getenv("PATH"); path && *path) {
        std::stringstream entries(path);
        std::string entry;
        while (std::getline(entries, entry, ':')) {
            if (entry.empty())
                entry = ".";
            const auto candidate = std::filesystem::path(entry) / name;
            if (const auto found = check(candidate))
                return found;
        }
    }
    return std::nullopt;
}

std::string lowercase(std::string value) {
    for (auto &character : value)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return value;
}

std::filesystem::path implementationOrfsRoot() {
    for (const auto *name : {"ICE_OPENROAD_FLOW_SCRIPTS", "OPENROAD_FLOW_SCRIPTS", "ORFS_ROOT"}) {
        if (const auto *value = std::getenv(name); value && *value) {
            std::error_code status;
            if (std::filesystem::is_directory(value, status) && !status)
                return value;
        }
    }
    std::vector<std::filesystem::path> candidates = {
        "/opt/OpenROAD-flow-scripts",
        "/opt/openroad-flow-scripts",
        "/usr/local/OpenROAD-flow-scripts",
        "/usr/share/openroad-flow-scripts",
    };
    if (const auto *home = std::getenv("HOME"); home && *home) {
        const auto root = std::filesystem::path(home);
        candidates.push_back(root / "OpenROAD-flow-scripts");
        candidates.push_back(root / "repositories" / "OpenROAD-flow-scripts");
        candidates.push_back(root / "src" / "OpenROAD-flow-scripts");
        candidates.push_back(root / ".local" / "share" / "OpenROAD-flow-scripts");
    }
    for (const auto &candidate : candidates) {
        std::error_code status;
        if (std::filesystem::is_directory(candidate, status) && !status)
            return candidate;
    }
    return {};
}

std::optional<std::filesystem::path> findFirstFile(const std::filesystem::path &directory,
                                                   const std::function<bool(const std::string &)> &predicate) {
    std::error_code status;
    if (!std::filesystem::is_directory(directory, status) || status)
        return std::nullopt;
    std::vector<std::filesystem::path> matches;
    for (std::filesystem::directory_iterator iterator(directory, status), end; !status && iterator != end;
         iterator.increment(status)) {
        if (!iterator->is_regular_file())
            continue;
        if (predicate(iterator->path().filename().string()))
            matches.push_back(iterator->path());
    }
    std::sort(matches.begin(), matches.end());
    if (matches.empty())
        return std::nullopt;
    return matches.front();
}

std::optional<std::filesystem::path> findOrfsExecutable(const std::string &tool, const std::string &name) {
    const auto *root = std::getenv("OPENROAD_FLOW_SCRIPTS");
    const auto *iceRoot = std::getenv("ICE_OPENROAD_FLOW_SCRIPTS");
    const std::filesystem::path base = iceRoot && *iceRoot ? iceRoot : (root && *root ? root : "");
    if (base.empty())
        return std::nullopt;
    const std::vector<std::filesystem::path> candidates = {
        base / "tools" / "install" / "OpenROAD" / "bin" / name,
        base / "tools" / "install" / tool / "bin" / name,
        base / "tools" / tool / "bin" / name,
    };
    for (const auto &candidate : candidates) {
        std::error_code status;
        if (std::filesystem::is_regular_file(candidate, status) && !status)
            return candidate;
    }
    return std::nullopt;
}

void addDiagnostic(std::vector<Diagnostic> &diagnostics, Diagnostic::Severity severity, std::string code,
                   std::string message) {
    diagnostics.push_back({severity, std::move(code), std::move(message), std::nullopt});
}

const PackagePdkStack *findStack(const PackagePdk &pdk, const std::string &id) {
    for (const auto &stack : pdk.stacks)
        if (stack.id == id)
            return &stack;
    return nullptr;
}

const PackagePdkStackImplementation *stackFiles(const PackagePdk &pdk, const PackagePdkStack *stack) {
    if (stack && stack->implementation)
        return &*stack->implementation;
    if (pdk.implementation && pdk.implementation->files)
        return &*pdk.implementation->files;
    return nullptr;
}

} // namespace

const char *implementationStageName(ImplementationStage stage) {
    switch (stage) {
    case ImplementationStage::created:
        return "created";
    case ImplementationStage::elaboration:
        return "elaboration";
    case ImplementationStage::synthesis:
        return "synthesis";
    case ImplementationStage::floorplan:
        return "floorplan";
    case ImplementationStage::placement:
        return "placement";
    case ImplementationStage::cts:
        return "cts";
    case ImplementationStage::routing:
        return "routing";
    case ImplementationStage::final:
        return "final";
    }
    return "created";
}

std::optional<ImplementationStage> parseImplementationStage(const std::string &text) {
    for (const auto stage : kStages)
        if (text == implementationStageName(stage))
            return stage;
    return std::nullopt;
}

const char *implementationStatusName(ImplementationStatus status) {
    switch (status) {
    case ImplementationStatus::pending:
        return "pending";
    case ImplementationStatus::running:
        return "running";
    case ImplementationStatus::succeeded:
        return "succeeded";
    case ImplementationStatus::failed:
        return "failed";
    case ImplementationStatus::cancelled:
        return "cancelled";
    }
    return "pending";
}

std::optional<ImplementationStatus> parseImplementationStatus(const std::string &text) {
    for (const auto status :
         {ImplementationStatus::pending, ImplementationStatus::running, ImplementationStatus::succeeded,
          ImplementationStatus::failed, ImplementationStatus::cancelled})
        if (text == implementationStatusName(status))
            return status;
    return std::nullopt;
}

const char *implementationStageStatusName(ImplementationStageStatus status) { return stageStatusName(status); }

std::optional<ImplementationStageStatus> parseImplementationStageStatus(const std::string &text) {
    for (const auto status :
         {ImplementationStageStatus::pending, ImplementationStageStatus::running, ImplementationStageStatus::succeeded,
          ImplementationStageStatus::failed, ImplementationStageStatus::skipped, ImplementationStageStatus::cancelled})
        if (text == implementationStageStatusName(status))
            return status;
    return std::nullopt;
}

std::vector<ImplementationStage> implementationStages() { return kStages; }

json implementationRunJson(const ImplementationRun &run) {
    json stages = json::array();
    for (const auto stage : kStages) {
        const auto found = run.stages.find(stage);
        if (found != run.stages.end())
            stages.push_back(stageJson(found->second));
        else
            stages.push_back(
                {{"stage", implementationStageName(stage)}, {"status", "pending"}, {"artifacts", json::array()}});
    }
    json toolVersions = json::object();
    for (const auto &[name, version] : run.toolVersions)
        toolVersions[name] = version;
    json metrics = json::object();
    for (const auto &[name, value] : run.metrics)
        metrics[name] = value;
    return {{"id", run.id},
            {"name", run.name},
            {"config", configJson(run.config)},
            {"current_stage", implementationStageName(run.currentStage)},
            {"status", implementationStatusName(run.status)},
            {"stages", std::move(stages)},
            {"directory", run.directory},
            {"config_hash", run.configHash},
            {"tool_versions", std::move(toolVersions)},
            {"metrics", std::move(metrics)},
            {"artifacts", run.artifacts},
            {"result_layout", run.resultLayout},
            {"last_error", run.lastError},
            {"created_at", run.createdAt},
            {"updated_at", run.updatedAt}};
}

ImplementationRun implementationRunFromJson(const json &document) {
    ImplementationRun run;
    run.id = document.value("id", ObjectId{0});
    run.name = document.value("name", std::string{});
    if (document.contains("config"))
        run.config = configFromJson(document.at("config"));
    if (const auto current = parseImplementationStage(document.value("current_stage", std::string{})))
        run.currentStage = *current;
    if (const auto status = parseImplementationStatus(document.value("status", std::string{})))
        run.status = *status;
    if (document.contains("stages")) {
        for (const auto &stage : document.at("stages")) {
            auto parsed = stageFromJson(stage);
            run.stages[parsed.stage] = std::move(parsed);
        }
    }
    for (const auto stage : kStages) {
        if (!run.stages.contains(stage)) {
            ImplementationStageState state;
            state.stage = stage;
            run.stages.emplace(stage, std::move(state));
        }
    }
    run.directory = document.value("directory", std::string{});
    run.configHash = document.value("config_hash", std::string{});
    if (document.contains("tool_versions")) {
        for (const auto &[name, value] : document.at("tool_versions").items()) {
            if (value.is_string())
                run.toolVersions[name] = value.get<std::string>();
        }
    }
    if (document.contains("metrics")) {
        for (const auto &[name, value] : document.at("metrics").items()) {
            if (value.is_number())
                run.metrics[name] = value.get<double>();
        }
    }
    if (document.contains("artifacts")) {
        for (const auto &artifact : document.at("artifacts"))
            if (artifact.is_string())
                run.artifacts.push_back(artifact.get<std::string>());
    }
    run.resultLayout = document.value("result_layout", ObjectId{0});
    run.lastError = document.value("last_error", std::string{});
    run.createdAt = document.value("created_at", std::string{});
    run.updatedAt = document.value("updated_at", std::string{});
    if (run.config.pdk.empty())
        run.config.pdk = genericPdkId;
    return run;
}

std::string serializeImplementationRun(const ImplementationRun &run) {
    return json{{"format", kImplementationFormat}, {"schema_version", 1}, {"run", implementationRunJson(run)}}.dump(2) +
           "\n";
}

ImplementationRun deserializeImplementationRun(const std::string &text) {
    const auto document = json::parse(text);
    if (document.value("format", std::string{}) != kImplementationFormat)
        throw ProjectError("implementation.format", "Invalid implementation file format");
    if (document.value("schema_version", 0) != 1)
        throw ProjectError("implementation.schema", "Unsupported implementation schema version");
    if (!document.contains("run"))
        throw ProjectError("implementation.format", "Implementation file has no run object");
    return implementationRunFromJson(document.at("run"));
}

std::string implementationRunStem(const ImplementationRun &run) {
    return safeStem(run.name) + "-" + std::to_string(run.id);
}

bool ImplementationToolchain::ready() const { return !openroad.path.empty() && !yosys.path.empty(); }

std::vector<std::string> ImplementationToolchain::missing() const {
    std::vector<std::string> result;
    if (openroad.path.empty())
        result.push_back("openroad");
    if (yosys.path.empty())
        result.push_back("yosys");
    if (klayout.path.empty())
        result.push_back("klayout");
    return result;
}

ImplementationToolchain discoverImplementationToolchain() {
    ImplementationToolchain toolchain;
    const auto findTool = [](const std::string &name) {
        if (const auto found = findExecutable(name))
            return std::pair{*found, std::string{"PATH"}};
        if (const auto found = findOrfsExecutable(name, name))
            return std::pair{*found, std::string{"OpenROAD-flow-scripts"}};
        return std::pair{std::filesystem::path{}, std::string{"missing"}};
    };
    auto [openroad, openroadSource] = findTool("openroad");
    toolchain.openroad = {"openroad", std::move(openroad), std::move(openroadSource)};
    auto [yosys, yosysSource] = findTool("yosys");
    toolchain.yosys = {"yosys", std::move(yosys), std::move(yosysSource)};
    auto [klayout, klayoutSource] = findTool("klayout");
    toolchain.klayout = {"klayout", std::move(klayout), std::move(klayoutSource)};
    return toolchain;
}

json implementationToolchainJson(const ImplementationToolchain &toolchain) {
    const auto tool = [](const ImplementationTool &value) {
        return json{{"name", value.name},
                    {"path", value.path.empty() ? json(nullptr) : json(value.path.generic_string())},
                    {"source", value.source}};
    };
    return {{"ready", toolchain.ready()},
            {"missing", toolchain.missing()},
            {"openroad", tool(toolchain.openroad)},
            {"yosys", tool(toolchain.yosys)},
            {"klayout", tool(toolchain.klayout)}};
}

std::string implementationConfigHash(const ImplementationConfig &config) { return hashText(configJson(config).dump()); }

ImplementationPlatform resolveImplementationPlatform(const PackageSpec &package, const std::string &stack,
                                                     std::vector<Diagnostic> &diagnostics) {
    ImplementationPlatform platform;
    if (!package.pdk) {
        addDiagnostic(diagnostics, Diagnostic::Severity::error, "implementation.pdk", "Package does not declare a PDK");
        return platform;
    }
    platform.pdk = package.pdk->id.empty() ? package.name : package.pdk->id;
    std::string stackId;
    try {
        stackId = resolvePdkStack(*package.pdk, stack);
    } catch (const ProjectError &error) {
        addDiagnostic(diagnostics, Diagnostic::Severity::error, "implementation.stack", error.what());
        return platform;
    }
    platform.stack = stackId;
    if (!package.pdk->implementation) {
        addDiagnostic(diagnostics, Diagnostic::Severity::error, "implementation.platform",
                      "Package " + package.name + " does not declare [pdk.implementation]");
        return platform;
    }
    platform.engine = package.pdk->implementation->engine;
    platform.synthesis = package.pdk->implementation->synthesis;
    const PackagePdkStack *selectedStack = nullptr;
    if (!stackId.empty()) {
        selectedStack = findStack(*package.pdk, stackId);
        if (!selectedStack) {
            addDiagnostic(diagnostics, Diagnostic::Severity::error, "implementation.stack",
                          "Stack '" + stackId + "' is not declared by PDK '" + platform.pdk + "'");
            return platform;
        }
    }
    const auto *files = stackFiles(*package.pdk, selectedStack);
    if (!files) {
        addDiagnostic(diagnostics, Diagnostic::Severity::error, "implementation.platform",
                      "PDK '" + platform.pdk + "' has no implementation data for stack '" + stackId + "'");
        return platform;
    }
    const auto resolve = [&package](const std::filesystem::path &path) {
        if (path.empty())
            return std::filesystem::path{};
        return path.is_absolute() ? path : (package.root / path).lexically_normal();
    };
    platform.site = files->site;
    platform.techLef = resolve(files->techLef);
    platform.cellLef = resolve(files->cellLef);
    platform.gds = resolve(files->gds);
    for (const auto &[corner, paths] : files->liberty) {
        auto &resolved = platform.liberty[corner];
        for (const auto &path : paths)
            resolved.push_back(resolve(path));
    }
    platform.ioHorizontalLayers = files->ioHorizontalLayers;
    platform.ioVerticalLayers = files->ioVerticalLayers;
    platform.routingMinLayer = files->routingMinLayer;
    platform.routingMaxLayer = files->routingMaxLayer;
    platform.tapcellScript = resolve(files->tapcellScript);
    platform.pdnScript = resolve(files->pdnScript);
    platform.rcScript = resolve(files->rcScript);
    platform.rcxRules = resolve(files->rcxRules);
    platform.fillCells = files->fillCells;

    // Optional OpenROAD-flow-scripts platform fallback. This lets a package
    // point at an ORFS platform directory while the physical files are still
    // managed by that installation.
    const auto &implementation = *package.pdk->implementation;
    if (!implementation.orfsPlatform.empty()) {
        const auto orfsRoot = implementationOrfsRoot();
        if (orfsRoot.empty()) {
            addDiagnostic(diagnostics, Diagnostic::Severity::error, "implementation.orfs",
                          "Set OPENROAD_FLOW_SCRIPTS or ICE_OPENROAD_FLOW_SCRIPTS to use platform '" +
                              implementation.orfsPlatform + "'");
        } else {
            const auto platformDirectory = orfsRoot / "flow" / "platforms" / implementation.orfsPlatform;
            std::error_code status;
            if (!std::filesystem::is_directory(platformDirectory, status) || status) {
                addDiagnostic(diagnostics, Diagnostic::Severity::error, "implementation.orfs",
                              "OpenROAD-flow-scripts platform does not exist: " + platformDirectory.string());
            } else {
                const auto metal =
                    lowercase(implementation.orfsMetalOption.empty() ? stackId : implementation.orfsMetalOption);
                const auto track = lowercase(implementation.orfsTrackOption);
                const auto matches = [&](const std::string &filename, const std::string &suffix) {
                    const auto lower = lowercase(filename);
                    return lower.ends_with(suffix) && (metal.empty() || lower.find(metal) != std::string::npos) &&
                           (track.empty() || lower.find(track) != std::string::npos);
                };
                if (platform.techLef.empty()) {
                    if (const auto found = findFirstFile(platformDirectory / "lef", [&](const std::string &name) {
                            return matches(name, "_tech.lef");
                        }))
                        platform.techLef = *found;
                    else if (const auto found = findFirstFile(platformDirectory / "lef", [&](const std::string &name) {
                                 const auto lower = lowercase(name);
                                 return lower.ends_with(".tlef") &&
                                        (metal.empty() || lower.find(metal) != std::string::npos);
                             }))
                        platform.techLef = *found;
                }
                if (platform.cellLef.empty()) {
                    if (const auto found = findFirstFile(platformDirectory / "lef", [&](const std::string &name) {
                            return matches(name, "_sc.lef");
                        }))
                        platform.cellLef = *found;
                    else if (const auto found = findFirstFile(platformDirectory / "lef", [&](const std::string &name) {
                                 const auto lower = lowercase(name);
                                 return lower.ends_with(".lef") && !lower.ends_with(".tlef") &&
                                        (metal.empty() || lower.find(metal) != std::string::npos);
                             }))
                        platform.cellLef = *found;
                }
                if (platform.site.empty()) {
                    if (implementation.orfsPlatform == "gf180")
                        platform.site = track == "7t" ? "GF018hv5v_mcu_sc7" : "GF018hv5v_green_sc9";
                    else if (implementation.orfsPlatform == "sky130hd")
                        platform.site = "unithd";
                    else if (implementation.orfsPlatform == "sky130hs")
                        platform.site = "uniths";
                }
                if (platform.ioHorizontalLayers.empty() && platform.ioVerticalLayers.empty()) {
                    if (implementation.orfsPlatform == "gf180") {
                        platform.ioHorizontalLayers = {"Metal3"};
                        platform.ioVerticalLayers = {"Metal4"};
                    } else if (implementation.orfsPlatform.rfind("sky130", 0) == 0) {
                        platform.ioHorizontalLayers = {"met3"};
                        platform.ioVerticalLayers = {"met2"};
                    }
                }
                if (platform.routingMinLayer.empty() && implementation.orfsPlatform == "gf180")
                    platform.routingMinLayer = "Metal1";
                if (platform.routingMaxLayer.empty() && implementation.orfsPlatform == "gf180") {
                    if (stackId == "3LM_1TM")
                        platform.routingMaxLayer = "Metal3";
                    else if (stackId == "4LM_1TM")
                        platform.routingMaxLayer = "Metal4";
                    else if (stackId == "6LM_1TM")
                        platform.routingMaxLayer = "MetalTop";
                    else
                        platform.routingMaxLayer = "Metal5";
                }
                if (platform.routingMinLayer.empty() && implementation.orfsPlatform.rfind("sky130", 0) == 0)
                    platform.routingMinLayer = "met1";
                if (platform.routingMaxLayer.empty() && implementation.orfsPlatform.rfind("sky130", 0) == 0)
                    platform.routingMaxLayer = "met5";
                if (platform.liberty.empty()) {
                    auto cornerToken = lowercase(implementation.orfsLibertyPattern);
                    if (cornerToken.empty()) {
                        cornerToken = lowercase(implementation.orfsCorner);
                        if (cornerToken == "bc")
                            cornerToken = "ff";
                        else if (cornerToken == "tc")
                            cornerToken = "tt";
                        else if (cornerToken == "wc")
                            cornerToken = "ss";
                    }
                    const auto libertyFile = findFirstFile(platformDirectory / "lib", [&](const std::string &name) {
                        const auto lower = lowercase(name);
                        if (!lower.ends_with(".lib") && !lower.ends_with(".lib.gz"))
                            return false;
                        return cornerToken.empty() || lower.find(cornerToken) != std::string::npos;
                    });
                    if (libertyFile)
                        platform.liberty["tt"] = {*libertyFile};
                    else if (const auto anyLiberty =
                                 findFirstFile(platformDirectory / "lib", [](const std::string &name) {
                                     const auto lower = lowercase(name);
                                     return lower.ends_with(".lib") || lower.ends_with(".lib.gz");
                                 }))
                        platform.liberty["tt"] = {*anyLiberty};
                }
                if (platform.rcxRules.empty()) {
                    if (const auto rules = platformDirectory / "rcx_patterns.rules";
                        std::filesystem::is_regular_file(rules, status) && !status) {
                        platform.rcxRules = rules;
                    } else if (implementation.orfsPlatform == "gf180") {
                        const auto rulesDirectory = platformDirectory / "openROAD" / "rcx";
                        auto metalToken = std::string{"1p"};
                        if (!stackId.empty() && std::isdigit(static_cast<unsigned char>(stackId.front())))
                            metalToken.push_back(stackId.front());
                        metalToken += "m_1tm";
                        auto trackToken = lowercase(implementation.orfsTrackOption);
                        if (trackToken.ends_with("t"))
                            trackToken.back() = 'k';
                        auto cornerToken = lowercase(implementation.orfsCorner);
                        std::string cornerSuffix;
                        if (cornerToken == "bc")
                            cornerSuffix = "bst";
                        else if (cornerToken == "wc")
                            cornerSuffix = "wst";
                        else if (cornerToken == "tc")
                            cornerSuffix = "typ";
                        else
                            cornerSuffix = cornerToken;
                        const auto preferToken = lowercase(implementation.orfsPowerOption).starts_with("5v0")
                                                     ? std::string{"optb"}
                                                     : std::string{};
                        const auto matchRules = [&](bool requirePreferred) {
                            return findFirstFile(rulesDirectory, [&](const std::string &name) {
                                const auto lower = lowercase(name);
                                if (!lower.ends_with(".rules"))
                                    return false;
                                if (!metalToken.empty() && lower.find(metalToken) == std::string::npos)
                                    return false;
                                if (!trackToken.empty() && lower.find(trackToken) == std::string::npos)
                                    return false;
                                if (!cornerSuffix.empty() && lower.find(cornerSuffix) == std::string::npos)
                                    return false;
                                return !requirePreferred || lower.find(preferToken) != std::string::npos;
                            });
                        };
                        if (const auto found = matchRules(!preferToken.empty()))
                            platform.rcxRules = *found;
                        else if (const auto found = matchRules(false))
                            platform.rcxRules = *found;
                    }
                }
            }
        }
    }

    const auto requireFile = [&](const std::filesystem::path &path, const std::string &label, bool required) {
        if (path.empty()) {
            if (required)
                addDiagnostic(diagnostics, Diagnostic::Severity::error, "implementation.file",
                              "Missing implementation " + label + " for stack '" + stackId + "'");
            return;
        }
        std::error_code status;
        if (!std::filesystem::is_regular_file(path, status) || status)
            addDiagnostic(diagnostics, required ? Diagnostic::Severity::error : Diagnostic::Severity::warning,
                          "implementation.file", "Implementation " + label + " does not exist: " + path.string());
    };
    requireFile(platform.techLef, "tech LEF", true);
    requireFile(platform.cellLef, "cell LEF", true);
    if (platform.site.empty())
        addDiagnostic(diagnostics, Diagnostic::Severity::error, "implementation.site",
                      "Missing placement site for stack '" + stackId + "'");
    if (platform.liberty.empty())
        addDiagnostic(diagnostics, Diagnostic::Severity::error, "implementation.liberty",
                      "Missing Liberty files for stack '" + stackId + "'");
    for (const auto &[corner, filesForCorner] : platform.liberty)
        for (const auto &path : filesForCorner)
            requireFile(path, "Liberty (" + corner + ")", true);
    requireFile(platform.gds, "GDS", false);
    requireFile(platform.tapcellScript, "tapcell script", false);
    requireFile(platform.pdnScript, "PDN script", false);
    requireFile(platform.rcScript, "RC script", false);
    requireFile(platform.rcxRules, "RCX rules", false);
    return platform;
}

json implementationPlatformJson(const ImplementationPlatform &platform) {
    json liberty = json::object();
    for (const auto &[corner, paths] : platform.liberty) {
        json files = json::array();
        for (const auto &path : paths)
            files.push_back(path.generic_string());
        liberty[corner] = std::move(files);
    }
    return {{"pdk", platform.pdk},
            {"stack", platform.stack},
            {"engine", platform.engine},
            {"synthesis", platform.synthesis},
            {"site", platform.site},
            {"tech_lef", platform.techLef.generic_string()},
            {"cell_lef", platform.cellLef.generic_string()},
            {"gds", platform.gds.generic_string()},
            {"liberty", std::move(liberty)},
            {"io_horizontal_layers", platform.ioHorizontalLayers},
            {"io_vertical_layers", platform.ioVerticalLayers},
            {"routing_min_layer", platform.routingMinLayer},
            {"routing_max_layer", platform.routingMaxLayer},
            {"tapcell_script", platform.tapcellScript.generic_string()},
            {"pdn_script", platform.pdnScript.generic_string()},
            {"rc_script", platform.rcScript.generic_string()},
            {"rcx_rules", platform.rcxRules.generic_string()},
            {"fill_cells", platform.fillCells}};
}

std::vector<Diagnostic> validateImplementationConfig(const Project &project, const ImplementationConfig &config) {
    std::vector<Diagnostic> result;
    if (config.name.empty())
        addDiagnostic(result, Diagnostic::Severity::error, "implementation.name",
                      "Implementation run name must not be empty");
    if (!validIdentifier(config.top))
        addDiagnostic(result, Diagnostic::Severity::error, "implementation.top",
                      "Top module must be a valid identifier");
    if (config.pdk.empty() || config.pdk.find_first_of("/\\") != std::string::npos)
        addDiagnostic(result, Diagnostic::Severity::error, "implementation.pdk", "Implementation PDK is invalid");
    if (config.stack.find_first_of("/\\") != std::string::npos)
        addDiagnostic(result, Diagnostic::Severity::error, "implementation.stack", "Implementation stack is invalid");
    if (config.sources.empty() && config.sourceCells.empty())
        addDiagnostic(result, Diagnostic::Severity::error, "implementation.sources",
                      "Implementation run needs at least one RTL source");
    for (const auto &source : config.sources) {
        if (source.empty())
            addDiagnostic(result, Diagnostic::Severity::error, "implementation.source",
                          "RTL source path must not be empty");
    }
    for (const auto sourceCell : config.sourceCells) {
        const auto cell = project.schematicCells.find(sourceCell);
        if (cell == project.schematicCells.end()) {
            addDiagnostic(result, Diagnostic::Severity::error, "implementation.source_cell",
                          "RTL source cell does not exist");
            continue;
        }
        if (!cell->second.systemVerilogSchematic)
            addDiagnostic(result, Diagnostic::Severity::error, "implementation.source_cell",
                          "RTL source cell must be a SystemVerilog source cell");
    }
    if (config.utilization <= 0.0 || config.utilization > 1.0)
        addDiagnostic(result, Diagnostic::Severity::error, "implementation.utilization",
                      "Utilization must be in the range (0, 1]");
    if (config.placementDensity < 0.0 || config.placementDensity > 1.0)
        addDiagnostic(result, Diagnostic::Severity::error, "implementation.placement_density",
                      "Placement density must be in the range [0, 1]");
    if (config.aspectRatio <= 0.0)
        addDiagnostic(result, Diagnostic::Severity::error, "implementation.aspect_ratio",
                      "Aspect ratio must be positive");
    if (config.threads < 1)
        addDiagnostic(result, Diagnostic::Severity::error, "implementation.threads",
                      "Thread count must be at least one");
    if (config.timeoutSeconds < 1)
        addDiagnostic(result, Diagnostic::Severity::error, "implementation.timeout", "Timeout must be positive");
    if (config.dieArea && !config.dieArea->valid())
        addDiagnostic(result, Diagnostic::Severity::error, "implementation.die_area",
                      "Die area must have positive width and height");
    if (config.coreArea && !config.coreArea->valid())
        addDiagnostic(result, Diagnostic::Severity::error, "implementation.core_area",
                      "Core area must have positive width and height");
    for (const auto &macro : config.macros) {
        if (macro.cell.empty())
            addDiagnostic(result, Diagnostic::Severity::error, "implementation.macro",
                          "Macro placement requires a cell name");
    }
    return result;
}

ObjectId createImplementationRun(Project &project, ImplementationConfig config) {
    const auto diagnostics = validateImplementationConfig(project, config);
    if (!diagnostics.empty())
        throw ProjectError(diagnostics.front().code, diagnostics.front().message);
    const auto name = config.name;
    if (std::any_of(project.implementationRuns.begin(), project.implementationRuns.end(),
                    [&](const auto &entry) { return entry.second.name == name; }))
        throw ProjectError("implementation.name", "An implementation run named '" + name + "' already exists");

    ImplementationRun run;
    run.id = project.nextId();
    run.name = name;
    run.config = std::move(config);
    run.currentStage = ImplementationStage::created;
    run.status = ImplementationStatus::pending;
    run.createdAt = nowUtc();
    run.updatedAt = run.createdAt;
    run.configHash = implementationConfigHash(run.config);
    run.directory = "build/impl/" + implementationRunStem(run);
    for (const auto stage : implementationStages()) {
        ImplementationStageState state;
        state.stage = stage;
        run.stages.emplace(stage, std::move(state));
    }
    const auto id = run.id;
    project.implementationRuns.emplace(id, std::move(run));
    return id;
}

void removeImplementationRun(Project &project, ObjectId runId) { project.implementationRuns.erase(runId); }

ImplementationRun *findImplementationRun(Project &project, ObjectId runId) {
    const auto found = project.implementationRuns.find(runId);
    return found == project.implementationRuns.end() ? nullptr : &found->second;
}

const ImplementationRun *findImplementationRun(const Project &project, ObjectId runId) {
    const auto found = project.implementationRuns.find(runId);
    return found == project.implementationRuns.end() ? nullptr : &found->second;
}

ImplementationRun *findImplementationRun(Project &project, const std::string &name) {
    for (auto &[_, run] : project.implementationRuns)
        if (run.name == name)
            return &run;
    return nullptr;
}

const ImplementationRun *findImplementationRun(const Project &project, const std::string &name) {
    for (const auto &[_, run] : project.implementationRuns)
        if (run.name == name)
            return &run;
    return nullptr;
}

ImplementationPreparation prepareImplementationRun(Project &project, ImplementationRun &run,
                                                   const ImplementationPlatform &platform,
                                                   const std::filesystem::path &projectRoot) {
    ImplementationPreparation result;
    const auto stem = implementationRunStem(run);
    const auto runDirectory = projectRoot / "build" / "impl" / stem;
    run.directory = "build/impl/" + stem;
    run.configHash = implementationConfigHash(run.config);
    run.updatedAt = nowUtc();
    if (platform.techLef.empty())
        addDiagnostic(result.diagnostics, Diagnostic::Severity::error, "implementation.file",
                      "Platform has no technology LEF");
    if (platform.cellLef.empty())
        addDiagnostic(result.diagnostics, Diagnostic::Severity::error, "implementation.file",
                      "Platform has no cell LEF");
    if (platform.site.empty())
        addDiagnostic(result.diagnostics, Diagnostic::Severity::error, "implementation.site",
                      "Platform has no placement site");

    for (const auto &directory :
         {"config", "rtl", "constraints", "scripts", "logs", "synth", "db", "def", "final", "reports"})
        std::filesystem::create_directories(runDirectory / directory);

    std::vector<std::filesystem::path> rtlFiles;
    std::set<std::string> usedStems;
    for (const auto sourceCell : run.config.sourceCells) {
        const auto cell = project.schematicCells.find(sourceCell);
        if (cell == project.schematicCells.end()) {
            addDiagnostic(result.diagnostics, Diagnostic::Severity::error, "implementation.source_cell",
                          "RTL source cell does not exist");
            continue;
        }
        if (!cell->second.systemVerilogSchematic) {
            addDiagnostic(result.diagnostics, Diagnostic::Severity::error, "implementation.source_cell",
                          "RTL source cell is not a SystemVerilog source cell");
            continue;
        }
        const auto destination = uniqueDestination(runDirectory / "rtl", safeStem(cell->second.name), ".sv", usedStems);
        writeText(destination, cell->second.systemVerilogText);
        rtlFiles.push_back(destination);
    }
    for (const auto &source : run.config.sources) {
        auto sourcePath = source.is_absolute() ? source : (projectRoot / source).lexically_normal();
        std::error_code status;
        if (!std::filesystem::is_regular_file(sourcePath, status) || status) {
            addDiagnostic(result.diagnostics, Diagnostic::Severity::error, "implementation.source",
                          "RTL source does not exist: " + sourcePath.string());
            continue;
        }
        const auto extension = sourcePath.extension().empty() ? std::string{".v"} : sourcePath.extension().string();
        const auto destination =
            uniqueDestination(runDirectory / "rtl", safeStem(sourcePath.stem().string()), extension, usedStems);
        std::filesystem::copy_file(sourcePath, destination, std::filesystem::copy_options::overwrite_existing, status);
        if (status) {
            addDiagnostic(result.diagnostics, Diagnostic::Severity::error, "implementation.source",
                          "Unable to copy RTL source: " + sourcePath.string());
            continue;
        }
        rtlFiles.push_back(destination);
    }
    if (rtlFiles.empty())
        addDiagnostic(result.diagnostics, Diagnostic::Severity::error, "implementation.sources",
                      "No RTL sources were prepared");

    std::filesystem::path sdcFile;
    if (!run.config.sdc.empty()) {
        const auto sourcePath = run.config.sdc.is_absolute() ? run.config.sdc : (projectRoot / run.config.sdc);
        std::error_code status;
        if (std::filesystem::is_regular_file(sourcePath, status) && !status) {
            sdcFile = runDirectory / "constraints" / "top.sdc";
            std::filesystem::copy_file(sourcePath, sdcFile, std::filesystem::copy_options::overwrite_existing, status);
            if (status) {
                addDiagnostic(result.diagnostics, Diagnostic::Severity::error, "implementation.sdc",
                              "Unable to copy SDC file: " + sourcePath.string());
                sdcFile.clear();
            }
        } else {
            addDiagnostic(result.diagnostics, Diagnostic::Severity::error, "implementation.sdc",
                          "SDC file does not exist: " + sourcePath.string());
        }
    }

    std::vector<std::filesystem::path> liberty;
    if (const auto found = platform.liberty.find(run.config.corner); found != platform.liberty.end()) {
        liberty = found->second;
    } else if (platform.liberty.size() == 1) {
        liberty = platform.liberty.begin()->second;
    } else if (!platform.liberty.empty()) {
        addDiagnostic(result.diagnostics, Diagnostic::Severity::error, "implementation.corner",
                      "Liberty corner '" + run.config.corner + "' is not declared by the platform");
    } else {
        addDiagnostic(result.diagnostics, Diagnostic::Severity::error, "implementation.liberty",
                      "The platform has no Liberty files");
    }

    const auto synthNetlist = runDirectory / "synth" / "1_synth.v";
    const auto techMapNetlist = runDirectory / implementationMappedNetlistRelativePath();
    const bool graphviz = findExecutable("dot").has_value();
    std::ostringstream yosys;
    yosys << "# Generated by ICE. Do not edit by hand.\n";
    yosys << "read_verilog -sv";
    for (const auto &file : rtlFiles)
        yosys << " " << tclQuote(file.generic_string());
    yosys << "\n";
    yosys << "hierarchy -check -top " << run.config.top << "\n";
    yosys << "proc\n";
    yosys << "opt\n";
    if (graphviz) {
        // Generate a compact pre-mapping graph first. It stays small enough for
        // Graphviz to lay out even when the final gate-level design has
        // thousands of cells; the stage runner falls back to it for large
        // designs. Continue with the remaining synthesis passes afterwards.
        yosys << "synth -top " << run.config.top << " -flatten -run begin:fine\n";
        yosys << "show -format dot -prefix reports/synthesis-overview -notitle\n";
        yosys << "synth -top " << run.config.top << " -flatten -run fine:check\n";
    } else {
        yosys << "synth -top " << run.config.top << " -flatten\n";
    }
    yosys << "opt_clean\n";
    if (graphviz) {
        // Generate the detailed DOT here, but leave SVG rendering to the stage
        // runner so large graphs can be skipped or timed out without blocking
        // synthesis itself.
        yosys << "show -format dot -prefix reports/synthesis -notitle\n";
    }
    yosys << "write_verilog -noattr -noexpr -nohex -nodec " << tclQuote(synthNetlist.generic_string()) << "\n";
    yosys << "stat\n";
    const auto yosysScript = runDirectory / "scripts" / "00-synth.ys";
    writeText(yosysScript, yosys.str());
    result.scripts.push_back(yosysScript);

    if (!liberty.empty()) {
        std::ostringstream techmap;
        techmap << "# Generated by ICE. Do not edit by hand.\n";
        techmap << "read_verilog -sv";
        for (const auto &file : rtlFiles)
            techmap << " " << tclQuote(file.generic_string());
        techmap << "\n";
        techmap << "hierarchy -check -top " << run.config.top << "\n";
        techmap << "proc\n";
        techmap << "opt\n";
        techmap << "synth -top " << run.config.top << " -flatten\n";
        techmap << "dfflibmap -liberty " << tclQuote(liberty.front().generic_string()) << "\n";
        techmap << "abc -liberty " << tclQuote(liberty.front().generic_string()) << "\n";
        techmap << "opt_clean\n";
        techmap << "write_verilog -noattr -noexpr -nohex -nodec " << tclPath(techMapNetlist) << "\n";
        techmap << "stat\n";
        const auto techmapScript = runDirectory / implementationTechnologyMapScriptRelativePath();
        writeText(techmapScript, techmap.str());
        result.scripts.push_back(techmapScript);
    }

    std::ostringstream elaborate;
    elaborate << "# Generated by ICE. Do not edit by hand.\n";
    elaborate << "read_verilog -sv";
    for (const auto &file : rtlFiles)
        elaborate << " " << tclQuote(file.generic_string());
    elaborate << "\n";
    elaborate << "hierarchy -check -top " << run.config.top << "\n";
    elaborate << "proc\n";
    elaborate << "opt\n";
    if (graphviz)
        elaborate << "show -format dot -prefix reports/elaboration -notitle\n";
    elaborate << "stat\n";
    const auto elaborateScript = runDirectory / "scripts" / "00-elaborate.ys";
    writeText(elaborateScript, elaborate.str());
    result.scripts.push_back(elaborateScript);

    const auto libertyLines = [&] {
        std::ostringstream output;
        for (const auto &file : liberty)
            output << "read_liberty " << tclPath(file) << "\n";
        return output.str();
    }();
    const auto sdcLine = [&] {
        if (sdcFile.empty())
            return std::string{};
        return "read_sdc " + tclPath(sdcFile) + "\n";
    }();
    const auto micro = [&](Coordinate value) {
        return static_cast<double>(project.units.toNanometers(value)) / 1000.0;
    };
    const auto floorplanArgs = [&] {
        std::ostringstream output;
        if (run.config.dieArea) {
            const auto &die = *run.config.dieArea;
            output << " -die_area [list " << micro(die.x) << " " << micro(die.y) << " " << micro(die.x + die.width)
                   << " " << micro(die.y + die.height) << "]";
            if (run.config.coreArea) {
                const auto &core = *run.config.coreArea;
                output << " -core_area [list " << micro(core.x) << " " << micro(core.y) << " "
                       << micro(core.x + core.width) << " " << micro(core.y + core.height) << "]";
            } else {
                output << " -core_space 2";
            }
        } else {
            // OpenROAD's initialize_floorplan expects a percentage while the
            // ICE configuration stores utilization as a fraction.
            output << " -utilization " << (run.config.utilization * 100.0) << " -aspect_ratio "
                   << run.config.aspectRatio << " -core_space 2";
        }
        const auto site = run.config.site.empty() ? platform.site : run.config.site;
        if (!site.empty())
            output << " -site " << tclQuote(site);
        return output.str();
    }();

    const auto writeStage = [&](const std::string &name, const std::string &text) {
        const auto path = runDirectory / "scripts" / name;
        writeText(path, "# Generated by ICE. Do not edit by hand.\n" + text);
        result.scripts.push_back(path);
    };

    std::ostringstream floorplan;
    floorplan << "read_lef " << tclPath(platform.techLef) << "\n";
    floorplan << "read_lef " << tclPath(platform.cellLef) << "\n";
    floorplan << libertyLines;
    floorplan << "read_verilog " << tclPath(techMapNetlist) << "\n";
    floorplan << "link_design " << run.config.top << "\n";
    floorplan << sdcLine;
    floorplan << "initialize_floorplan" << floorplanArgs << "\n";
    floorplan << "make_tracks\n";
    const auto horizontal =
        !run.config.ioHorizontalLayers.empty() ? run.config.ioHorizontalLayers : platform.ioHorizontalLayers;
    const auto vertical =
        !run.config.ioVerticalLayers.empty() ? run.config.ioVerticalLayers : platform.ioVerticalLayers;
    if (!horizontal.empty() && !vertical.empty()) {
        floorplan << "place_pins -hor_layer " << tclQuote(horizontal.front()) << " -ver_layer "
                  << tclQuote(vertical.front()) << "\n";
    }
    for (const auto &macro : run.config.macros) {
        floorplan << "place_macro -macro_name " << tclQuote(macro.cell) << " -location [list " << micro(macro.x) << " "
                  << micro(macro.y) << "] -orientation " << tclQuote(macro.orientation) << "\n";
    }
    if (!platform.tapcellScript.empty())
        floorplan << "source " << tclPath(platform.tapcellScript) << "\n";
    if (!platform.pdnScript.empty())
        floorplan << "source " << tclPath(platform.pdnScript) << "\n";
    floorplan << "write_db " << tclPath(runDirectory / "db" / "2_floorplan.odb") << "\n";
    floorplan << "write_def " << tclPath(runDirectory / "def" / "2_floorplan.def") << "\n";
    floorplan << "write_sdc " << tclPath(runDirectory / "final" / "2_floorplan.sdc") << "\n";
    floorplan << "report_design_area_metrics\n";
    writeStage("01-floorplan.tcl", floorplan.str());

    std::ostringstream placement;
    placement << "read_db " << tclPath(runDirectory / "db" / "2_floorplan.odb") << "\n";
    placement << libertyLines;
    placement << "read_sdc " << tclPath(runDirectory / "final" / "2_floorplan.sdc") << "\n";
    const auto density = run.config.placementDensity > 0.0 ? run.config.placementDensity : 0.5;
    placement << "global_placement -density " << density;
    if (run.config.timingDriven)
        placement << " -timing_driven";
    if (run.config.routabilityDriven)
        placement << " -routability_driven";
    placement << "\n";
    placement << "detailed_placement\n";
    placement << "repair_design\n";
    placement << "estimate_parasitics -placement\n";
    placement << "write_db " << tclPath(runDirectory / "db" / "3_place.odb") << "\n";
    placement << "write_def " << tclPath(runDirectory / "def" / "3_place.def") << "\n";
    placement << "write_sdc " << tclPath(runDirectory / "final" / "3_place.sdc") << "\n";
    placement << "report_checks\nreport_tns\nreport_design_area_metrics\n";
    writeStage("02-place.tcl", placement.str());

    std::ostringstream cts;
    cts << "read_db " << tclPath(runDirectory / "db" / "3_place.odb") << "\n";
    cts << libertyLines;
    cts << "read_sdc " << tclPath(runDirectory / "final" / "3_place.sdc") << "\n";
    cts << "clock_tree_synthesis";
    if (!run.config.ctsRootBuffer.empty())
        cts << " -root_buf " << tclQuote(run.config.ctsRootBuffer);
    if (!run.config.ctsBuffers.empty())
        cts << " -buf_list " << tclList(run.config.ctsBuffers);
    cts << "\n";
    cts << "set_propagated_clock [all_clocks]\n";
    cts << "repair_clock_nets\n";
    cts << "detailed_placement\n";
    cts << "write_db " << tclPath(runDirectory / "db" / "4_cts.odb") << "\n";
    cts << "write_def " << tclPath(runDirectory / "def" / "4_cts.def") << "\n";
    cts << "write_sdc " << tclPath(runDirectory / "final" / "4_cts.sdc") << "\n";
    cts << "report_cts\nreport_clock_skew\nreport_checks\n";
    writeStage("03-cts.tcl", cts.str());

    std::ostringstream routing;
    const auto routeDatabase = run.config.ctsEnabled ? "4_cts.odb" : "3_place.odb";
    const auto routeSdc = run.config.ctsEnabled ? "4_cts.sdc" : "3_place.sdc";
    routing << "read_db " << tclPath(runDirectory / "db" / routeDatabase) << "\n";
    routing << libertyLines;
    routing << "read_sdc " << tclPath(runDirectory / "final" / routeSdc) << "\n";
    // Yosys leaves constant outputs on nets named zero_/one_. OpenROAD marks
    // them as GROUND, which TritonRoute refuses to route. Treat them as
    // ordinary signal nets instead of special power nets.
    routing << "set block [ord::get_db_block]\n";
    routing << "foreach constantNet {zero_ one_} {\n";
    routing << "  set constantDbNet [$block findNet $constantNet]\n";
    routing << "  if {$constantDbNet != \"NULL\" && [$constantDbNet getSigType] != \"SIGNAL\"} {\n";
    routing << "    $constantDbNet setSigType SIGNAL\n";
    routing << "  }\n";
    routing << "}\n";
    const auto routingMin = !run.config.routingMinLayer.empty() ? run.config.routingMinLayer : platform.routingMinLayer;
    const auto routingMax = !run.config.routingMaxLayer.empty() ? run.config.routingMaxLayer : platform.routingMaxLayer;
    if (!routingMin.empty() && !routingMax.empty())
        routing << "set_routing_layers -signal " << tclQuote(routingMin + "-" + routingMax) << "\n";
    // OpenROAD's guide writer cannot open absolute paths containing spaces
    // (run directories derived from HDL cell names may contain them). The
    // process working directory is the run directory, so use relative paths
    // for the routing reports.
    routing << "global_route -guide_file reports/route.guide\n";
    if (run.config.antennaRepair) {
        // Antenna checking is advisory. Some OpenROAD builds cannot see the
        // global-routing database and would otherwise abort the stage.
        routing << "if {[catch {check_antennas -report_file reports/antenna.rpt} antennaError]} {\n";
        routing << "  puts \"warning: antenna check skipped: $antennaError\"\n";
        routing << "}\n";
    }
    routing << "detailed_route -output_drc reports/route_drc.rpt\n";
    if (!platform.fillCells.empty())
        routing << "filler_placement " << tclList(platform.fillCells) << "\n";
    routing << "check_placement\n";
    routing << "global_connect\n";
    routing << "write_db " << tclPath(runDirectory / "db" / "5_route.odb") << "\n";
    routing << "write_def " << tclPath(runDirectory / "def" / "5_route.def") << "\n";
    routing << "write_sdc " << tclPath(runDirectory / "final" / "5_route.sdc") << "\n";
    // Congestion reporting is advisory. Some OpenROAD releases no longer
    // provide report_congestion (removed from GRT in 26Q2), so keep it
    // optional instead of failing the stage after routing has finished.
    routing << "if {[catch {report_congestion} congestionError]} {\n";
    routing << "  puts \"warning: congestion report skipped: $congestionError\"\n";
    routing << "}\n";
    writeStage("04-route.tcl", routing.str());

    std::ostringstream finalStage;
    finalStage << "read_db " << tclPath(runDirectory / "db" / "5_route.odb") << "\n";
    finalStage << libertyLines;
    finalStage << "read_sdc " << tclPath(runDirectory / "final" / "5_route.sdc") << "\n";
    const auto extractionModel = !platform.rcxRules.empty() ? platform.rcxRules : platform.rcScript;
    if (!extractionModel.empty()) {
        finalStage << "define_process_corner -ext_model_index 0 X\n";
        finalStage << "extract_parasitics -ext_model_file " << tclPath(extractionModel) << "\n";
        // write_spef/read_spef do not cope with absolute paths containing
        // spaces (they brace the path and then try to open it literally). The
        // process working directory is the run directory, so use the relative
        // path here.
        finalStage << "write_spef \"final/6_final.spef\"\n";
        finalStage << "read_spef \"final/6_final.spef\"\n";
    }
    finalStage << "write_db " << tclPath(runDirectory / "db" / "6_final.odb") << "\n";
    finalStage << "write_def " << tclPath(runDirectory / "def" / "6_final.def") << "\n";
    finalStage << "write_verilog " << tclPath(runDirectory / "final" / "6_final.v") << "\n";
    finalStage << "write_sdc " << tclPath(runDirectory / "final" / "6_final.sdc") << "\n";
    finalStage << "report_checks\nreport_tns\nreport_power\nreport_design_area_metrics\nreport_erc_metrics\n";
    finalStage << "# Final GDS is generated by the KLayout backend.\n";
    writeStage("05-final.tcl", finalStage.str());

    writeText(runDirectory / "config" / "run.json", implementationRunJson(run).dump(2) + "\n");
    writeText(runDirectory / "config" / "platform.json", implementationPlatformJson(platform).dump(2) + "\n");

    result.runDirectory = runDirectory;
    result.manifestPath = runDirectory / "config" / "run.json";
    result.rtlFiles.reserve(rtlFiles.size());
    for (const auto &file : rtlFiles)
        result.rtlFiles.push_back(file.lexically_relative(runDirectory).generic_string());
    const auto addArtifact = [&run](const std::string &artifact) {
        if (std::find(run.artifacts.begin(), run.artifacts.end(), artifact) == run.artifacts.end())
            run.artifacts.push_back(artifact);
    };
    addArtifact(result.manifestPath.lexically_relative(projectRoot).generic_string());
    for (const auto &script : result.scripts)
        addArtifact(script.lexically_relative(projectRoot).generic_string());
    result.ready =
        std::none_of(result.diagnostics.begin(), result.diagnostics.end(),
                     [](const Diagnostic &diagnostic) { return diagnostic.severity == Diagnostic::Severity::error; }) &&
        rtlFiles.size() > 0 && !liberty.empty();
    return result;
}

std::filesystem::path writeTechnologyMapScript(const ImplementationRun &run, const ImplementationPlatform &platform,
                                               const std::filesystem::path &projectRoot) {
    if (run.directory.empty())
        return {};
    const auto runDirectory = projectRoot / run.directory;
    std::vector<std::filesystem::path> rtlFiles;
    std::error_code status;
    for (std::filesystem::directory_iterator iterator(runDirectory / "rtl", status), end; !status && iterator != end;
         iterator.increment(status)) {
        if (!iterator->is_regular_file())
            continue;
        const auto extension = iterator->path().extension();
        if (extension == ".sv" || extension == ".v")
            rtlFiles.push_back(iterator->path());
    }
    std::sort(rtlFiles.begin(), rtlFiles.end());
    if (rtlFiles.empty())
        return {};
    std::vector<std::filesystem::path> liberty;
    if (const auto found = platform.liberty.find(run.config.corner); found != platform.liberty.end())
        liberty = found->second;
    else if (platform.liberty.size() == 1)
        liberty = platform.liberty.begin()->second;
    if (liberty.empty())
        return {};
    std::ostringstream yosys;
    yosys << "# Generated by ICE. Do not edit by hand.\n";
    yosys << "read_verilog -sv";
    for (const auto &file : rtlFiles)
        yosys << " " << tclQuote(file.generic_string());
    yosys << "\n";
    yosys << "hierarchy -check -top " << run.config.top << "\n";
    yosys << "proc\n";
    yosys << "opt\n";
    yosys << "synth -top " << run.config.top << " -flatten\n";
    yosys << "dfflibmap -liberty " << tclQuote(liberty.front().generic_string()) << "\n";
    yosys << "abc -liberty " << tclQuote(liberty.front().generic_string()) << "\n";
    yosys << "opt_clean\n";
    yosys << "write_verilog -noattr -noexpr -nohex -nodec "
          << tclQuote((runDirectory / implementationMappedNetlistRelativePath()).generic_string()) << "\n";
    yosys << "stat\n";
    const auto script = runDirectory / implementationTechnologyMapScriptRelativePath();
    writeText(script, yosys.str());
    return script;
}

} // namespace ice
