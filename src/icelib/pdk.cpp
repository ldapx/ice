#include "icelib/pdk.hpp"

#include "icelib/core.hpp"
#include "icelib/simulation.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <string_view>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace ice {
namespace {

using json = nlohmann::json;
std::vector<std::filesystem::path> packageRootOverride;

std::string lowerCopy(std::string text) {
    for (auto &character : text)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return text;
}

std::string trimCopy(std::string text) {
    const auto notSpace = [](unsigned char character) { return !std::isspace(character); };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), notSpace));
    text.erase(std::find_if(text.rbegin(), text.rend(), notSpace).base(), text.end());
    return text;
}

std::string normalizeId(std::string value) {
    value = lowerCopy(std::move(value));
    std::replace(value.begin(), value.end(), '.', '-');
    std::replace(value.begin(), value.end(), '_', '-');
    return value;
}

std::string readPrefix(const std::filesystem::path &path, std::size_t limit = 512 * 1024) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    std::string text;
    text.resize(limit);
    input.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(input.gcount()));
    return text;
}

bool executableInPath(const std::string &name) {
    const auto *path = std::getenv("PATH");
    if (!path || !*path)
        return false;
    std::stringstream stream(path);
    std::string directory;
    while (std::getline(stream, directory, ':')) {
        if (directory.empty())
            continue;
        std::error_code status;
        const auto candidate = std::filesystem::path(directory) / name;
        if (std::filesystem::is_regular_file(candidate, status) && !status)
            return true;
    }
    return false;
}

std::filesystem::path homeDirectory() {
    const auto *home = std::getenv("HOME");
    return home && *home ? std::filesystem::path(home) : std::filesystem::path{};
}

std::filesystem::path cacheRoot() {
    if (const auto *value = std::getenv("ICE_PDK_CACHE"); value && *value)
        return value;
    if (const auto *xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg)
        return std::filesystem::path(xdg) / "ice" / "pdk";
    return homeDirectory() / ".ice" / "cache" / "pdk";
}

std::string safeName(std::string value) {
    for (auto &character : value) {
        const auto lower = static_cast<unsigned char>(character);
        if (!std::isalnum(lower) && character != '-' && character != '_' && character != '.')
            character = '-';
    }
    if (value.empty())
        value = "pdk";
    return value;
}

std::vector<std::string> sectionsInFile(const std::filesystem::path &path) {
    std::vector<std::string> sections;
    std::istringstream stream(readPrefix(path));
    std::string line;
    while (std::getline(stream, line)) {
        const auto trimmed = trimCopy(line);
        if (trimmed.size() < 5)
            continue;
        const auto prefix = lowerCopy(trimmed.substr(0, 4));
        if (prefix != ".lib")
            continue;
        if (trimmed.size() > 4 && !std::isspace(static_cast<unsigned char>(trimmed[4])))
            continue;
        auto rest = trimCopy(trimmed.substr(4));
        if (rest.empty() || rest.front() == '"' || rest.front() == '\'')
            continue;
        if (rest.find_first_of(" \t") != std::string::npos)
            continue;
        sections.push_back(std::move(rest));
    }
    return sections;
}

std::vector<std::filesystem::path> osdiFilesIn(const std::filesystem::path &directory) {
    std::vector<std::filesystem::path> files;
    std::error_code status;
    if (!std::filesystem::is_directory(directory, status) || status)
        return files;
    for (std::filesystem::directory_iterator it(directory, status), end; !status && it != end; it.increment(status)) {
        if (it->is_regular_file() && it->path().extension() == ".osdi")
            files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::string firstUsefulLogLine(const std::string &log) {
    std::istringstream stream(log);
    std::string line;
    std::string fallback;
    while (std::getline(stream, line)) {
        const auto trimmed = trimCopy(line);
        const auto lower = lowerCopy(trimmed);
        if (trimmed.empty())
            continue;
        if (lower.find("could not find") != std::string::npos || lower.find("not found") != std::string::npos ||
            lower.find("unknown model") != std::string::npos || lower.find("fatal") != std::string::npos ||
            lower.find("cannot compute") != std::string::npos)
            return trimmed;
        if (fallback.empty() && (lower.find("error") != std::string::npos || lower.find("cannot") != std::string::npos))
            fallback = trimmed;
    }
    return fallback;
}

bool logLooksLikeFailure(const std::string &log) {
    const auto lower = lowerCopy(log);
    for (const auto *marker : {"error on line", "fatal error", "simulation interrupted", "could not find",
                               "could not load", "unknown model", "cannot compute"}) {
        if (lower.find(marker) != std::string::npos)
            return true;
    }
    return false;
}

std::string shellQuote(const std::string &value) {
    std::string result = "'";
    for (const char character : value) {
        if (character == '\'')
            result += "'\\''";
        else
            result += character;
    }
    return result + "'";
}

class PdkBuildLock {
  public:
    explicit PdkBuildLock(const std::filesystem::path &directory) {
#ifndef _WIN32
        std::error_code status;
        std::filesystem::create_directories(directory, status);
        descriptor_ = ::open((directory / "build.lock").c_str(), O_CREAT | O_RDWR, 0666);
        if (descriptor_ >= 0) {
            while (::flock(descriptor_, LOCK_EX) != 0 && errno == EINTR) {
            }
        }
#else
        (void)directory;
#endif
    }
    ~PdkBuildLock() {
#ifndef _WIN32
        if (descriptor_ >= 0) {
            ::flock(descriptor_, LOCK_UN);
            ::close(descriptor_);
        }
#endif
    }

  private:
#ifndef _WIN32
    int descriptor_ = -1;
#endif
};

std::string setupLine(const std::filesystem::path &path) { return path.generic_string(); }

std::string deviceDimensions(const std::string &id) {
    if (id.find("sky130") != std::string::npos)
        return "w=1 l=0.15";
    if (id.find("gf180") != std::string::npos)
        return "w=1u l=0.28u";
    return "w=1u l=0.13u";
}

void addSupplyForPdk(const std::string &id, double &vdd, double &vd) {
    if (id.find("sky130") != std::string::npos) {
        vdd = 1.8;
        vd = 0.9;
    } else if (id.find("gf180") != std::string::npos) {
        vdd = 3.3;
        vd = 1.0;
    } else {
        vdd = 1.2;
        vd = 0.6;
    }
}

PdkInfo infoFromPackage(const PackageIndexEntry &entry) {
    const auto &spec = entry.spec;
    PdkInfo info;
    info.packageName = spec.name;
    info.packageVersion = spec.version;
    info.packageRoot = spec.root;
    info.versionRoot = spec.root;
    info.version = spec.version;
    info.source = "ice-package";
    info.id = spec.pdk ? spec.pdk->id : spec.name;
    info.exampleDevices.clear();
    info.hasOpenVaf = executableInPath("openvaf-r") || executableInPath("openvaf");

    if (!spec.pdk) {
        info.missing.push_back("package does not declare a [pdk] section");
        return info;
    }
    if (!spec.pdk->simulation) {
        info.root = spec.root;
        info.missing.push_back("package does not declare [pdk.simulation]");
        return info;
    }

    const auto &sim = *spec.pdk->simulation;
    info.packageSimulation = sim;
    info.root = spec.root / sim.root;
    info.ngspiceModelDir = info.root;
    info.modelFile = info.root / sim.modelFile;
    info.modelSection = sim.section;
    info.verilogA = sim.verilogA.empty() ? std::filesystem::path{} : spec.root / sim.verilogA;
    info.osdiSources = sim.osdiSources;
    info.needsOsdi = sim.requiresOsdi;
    info.exampleDevices = sim.devices;
    info.spiceInitPath = sim.spiceinit.empty() ? std::filesystem::path{} : spec.root / sim.spiceinit;
    info.hasSpiceInit = !info.spiceInitPath.empty() && std::filesystem::is_regular_file(info.spiceInitPath);

    const auto cache = cacheRoot() / (safeName(spec.name) + "-" + safeName(spec.version));
    info.osdiDir = cache / "osdi";
    info.osdiFiles = osdiFilesIn(info.osdiDir);

    info.modelFiles.push_back({info.modelFile, sectionsInFile(info.modelFile)});
    info.sections = info.modelFiles.front().sections;
    if (!std::filesystem::is_regular_file(info.modelFile))
        info.missing.push_back("model file not found: " + info.modelFile.string());
    if (!sim.section.empty() &&
        std::find(info.sections.begin(), info.sections.end(), sim.section) == info.sections.end())
        info.missing.push_back("model section '" + sim.section + "' not found in " + info.modelFile.string());
    for (const auto &include : sim.includes) {
        if (!std::filesystem::is_regular_file(info.root / include))
            info.missing.push_back("model include not found: " + (info.root / include).string());
    }

    if (info.needsOsdi && info.osdiFiles.empty()) {
        info.missing.push_back("OSDI models not built; run ice-pkg pdk-build " + spec.name);
    }
    if (info.needsOsdi && info.osdiSources.empty())
        info.missing.push_back("package has no [pdk.simulation.osdi] sources");

    for (const auto &include : sim.includes)
        info.recommendedSetup.push_back(".include \"" + setupLine(info.root / include) + "\"");
    for (const auto &option : sim.options)
        info.recommendedSetup.push_back(option);
    if (!info.modelFile.empty() && !sim.section.empty()) {
        info.recommendedLibLine = ".lib \"" + setupLine(info.modelFile) + "\" " + sim.section;
        info.recommendedSetup.push_back(info.recommendedLibLine);
    }
    for (const auto &osdi : info.osdiFiles)
        info.initCommands.push_back("osdi '" + setupLine(osdi) + "'");

    info.notes.push_back("ICE package: " + spec.name + " " + spec.version);
    if (!info.osdiDir.empty())
        info.notes.push_back("OSDI cache: " + info.osdiDir.string());
    if (info.needsOsdi && info.hasOpenVaf && info.osdiFiles.empty())
        info.notes.push_back("OSDI compiler is available; run ice-pkg pdk-build " + spec.name);

    info.simulationReady = info.missing.empty();
    return info;
}

} // namespace

void setPdkPackageRoots(std::vector<std::filesystem::path> roots) { packageRootOverride = std::move(roots); }

std::vector<std::filesystem::path> defaultPdkRoots() {
    if (!packageRootOverride.empty()) {
        std::vector<std::filesystem::path> unique;
        std::set<std::string> seen;
        for (const auto &root : packageRootOverride) {
            std::error_code status;
            if (!std::filesystem::is_directory(root, status) || status)
                continue;
            const auto canonical = std::filesystem::weakly_canonical(root, status);
            const auto key = status ? root.lexically_normal().string() : canonical.string();
            if (seen.insert(key).second)
                unique.push_back(status ? root : canonical);
        }
        return unique;
    }
    std::vector<std::filesystem::path> roots;
    const auto append = [&](const std::string &value) {
        if (value.empty())
            return;
        std::stringstream stream(value);
        std::string part;
        while (std::getline(stream, part, ':')) {
            if (!part.empty())
                roots.emplace_back(part);
        }
    };
    bool explicitRoots = false;
    if (const auto *value = std::getenv("ICE_PACKAGE_ROOT"); value && *value) {
        append(value);
        explicitRoots = true;
    }
    if (const auto *value = std::getenv("ICE_PACKAGE_ROOTS"); value && *value) {
        append(value);
        explicitRoots = true;
    }
    if (!explicitRoots) {
        const auto home = homeDirectory();
        if (!home.empty())
            roots.emplace_back(home / ".ice" / "packages");
        if (const auto *xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg)
            roots.emplace_back(std::filesystem::path(xdg) / "ice" / "packages");
        else if (!home.empty())
            roots.emplace_back(home / ".local" / "share" / "ice" / "packages");
    }

    std::vector<std::filesystem::path> unique;
    std::set<std::string> seen;
    for (const auto &root : roots) {
        std::error_code status;
        if (!std::filesystem::is_directory(root, status) || status)
            continue;
        const auto canonical = std::filesystem::weakly_canonical(root, status);
        const auto key = status ? root.lexically_normal().string() : canonical.string();
        if (!seen.insert(key).second)
            continue;
        unique.push_back(status ? root : canonical);
    }
    return unique;
}

std::vector<PdkInfo> discoverPdks(const std::vector<std::filesystem::path> &roots) {
    const auto searchRoots = roots.empty() ? defaultPdkRoots() : roots;
    std::vector<PdkInfo> result;
    std::set<std::string> seen;
    for (const auto &root : searchRoots) {
        std::error_code status;
        if (std::filesystem::is_regular_file(root / "package.toml", status) && !status) {
            try {
                const auto spec = PackageManager::readManifest(root);
                if (!spec.pdk)
                    continue;
                const auto key = std::filesystem::weakly_canonical(root, status).string();
                if (seen.insert(key).second)
                    result.push_back(infoFromPackage({spec, root, {}, {}, {}}));
            } catch (const ProjectError &) {
            }
            continue;
        }
        for (const auto &entry : PackageManager::scanPackageIndex(root)) {
            if (!entry.spec.pdk)
                continue;
            const auto key = std::filesystem::weakly_canonical(entry.root, status).string();
            if (!seen.insert(key).second)
                continue;
            result.push_back(infoFromPackage(entry));
        }
    }
    std::sort(result.begin(), result.end(), [](const PdkInfo &left, const PdkInfo &right) {
        if (left.id != right.id)
            return left.id < right.id;
        if (left.packageName != right.packageName)
            return left.packageName < right.packageName;
        return left.packageVersion < right.packageVersion;
    });
    return result;
}

std::vector<PdkInfo> findPdks(const std::string &idOrPath, const std::vector<std::filesystem::path> &roots) {
    std::vector<PdkInfo> result;
    const auto wanted = normalizeId(idOrPath);
    std::error_code status;
    const auto path = std::filesystem::weakly_canonical(idOrPath, status);
    const auto pathText = status ? idOrPath : path.string();
    for (auto info : discoverPdks(roots)) {
        bool match = normalizeId(info.id) == wanted || normalizeId(info.packageName) == wanted ||
                     std::filesystem::weakly_canonical(info.packageRoot, status).string() == pathText;
        if (!match && info.packageSimulation) {
            // package manifest aliases are not stored in PdkInfo; read them on demand.
            try {
                const auto spec = PackageManager::readManifest(info.packageRoot);
                if (spec.pdk) {
                    for (const auto &alias : spec.pdk->aliases)
                        if (normalizeId(alias) == wanted)
                            match = true;
                }
            } catch (const ProjectError &) {
            }
        }
        if (match)
            result.push_back(std::move(info));
    }
    return result;
}

PdkBuildResult buildPdkOsdi(const PdkInfo &info, std::chrono::milliseconds timeout) {
    PdkBuildResult result;
    if (!info.packageSimulation || info.packageSimulation->osdiSources.empty()) {
        result.summary = "Package does not declare OSDI sources.";
        return result;
    }
    const auto compiler = executableInPath("openvaf-r")
                              ? std::string("openvaf-r")
                              : (executableInPath("openvaf") ? std::string("openvaf") : std::string{});
    if (compiler.empty()) {
        result.summary = "openvaf-r or openvaf is not on PATH.";
        return result;
    }
    result.compiler = compiler;
    result.osdiDir = info.osdiDir;
    std::error_code status;
    std::filesystem::create_directories(info.osdiDir, status);
    if (status) {
        result.summary = "Unable to create OSDI cache: " + status.message();
        return result;
    }
    PdkBuildLock buildLock(info.osdiDir);
    result.ok = true;
    std::ostringstream log;
    for (const auto &source : info.packageSimulation->osdiSources) {
        const auto input = info.packageRoot / source.source;
        const auto output = info.osdiDir / source.output;
        if (!std::filesystem::is_regular_file(input)) {
            result.ok = false;
            log << "missing OSDI source: " << input << "\n";
            continue;
        }
        std::filesystem::remove(output, status);
        status.clear();
        for (std::filesystem::directory_iterator it(info.osdiDir, status), end; !status && it != end;
             it.increment(status)) {
            const auto name = it->path().filename().string();
            if (name.starts_with(output.filename().string() + "."))
                std::filesystem::remove(it->path(), status);
            status.clear();
        }
        std::string command = "cd " + shellQuote(input.parent_path().string()) + " && timeout " +
                              std::to_string(std::max<std::int64_t>(1, (timeout.count() + 999) / 1000)) + "s " +
                              shellQuote(compiler);
        for (const auto &flag : info.packageSimulation->compilerFlags)
            command += " " + shellQuote(flag);
        command += " " + shellQuote(input.filename().string()) + " --output " + shellQuote(output.string()) + " >> " +
                   shellQuote((info.osdiDir / "build.log").string()) + " 2>&1";
        const auto exitCode = std::system(command.c_str());
        if (exitCode != 0 || !std::filesystem::is_regular_file(output)) {
            result.ok = false;
            log << "failed: " << input << " (exit " << exitCode << ")\n";
        }
    }
    result.log = log.str();
    if (result.ok)
        result.summary = "OSDI build completed in " + info.osdiDir.string();
    else if (!result.log.empty())
        result.summary = "OSDI build failed; inspect " + (info.osdiDir / "build.log").string();
    else
        result.summary = "OSDI build failed.";
    return result;
}

PdkProbeResult probePdk(const PdkInfo &info, std::chrono::milliseconds timeout) {
    PdkProbeResult result;
    if (!info.packageSimulation) {
        result.summary = "PDK package does not declare [pdk.simulation].";
        return result;
    }
    if (info.modelFile.empty() || info.exampleDevices.find("nmos") == info.exampleDevices.end()) {
        result.summary = "PDK package has no model/device probe metadata.";
        return result;
    }
    result.attempted = true;

    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto directory = std::filesystem::temp_directory_path() / ("ice-pdk-probe-" + std::to_string(stamp));
    std::error_code status;
    std::filesystem::create_directories(directory, status);
    const auto netlist = directory / "probe.sp";
    const auto logPath = directory / "probe.log";
    const auto init = directory / ".spiceinit";
    double vdd = 1.2;
    double vd = 0.6;
    addSupplyForPdk(normalizeId(info.id), vdd, vd);
    {
        std::ofstream output(netlist);
        output << "* ICE PDK probe\n";
        for (const auto &line : info.recommendedSetup)
            output << line << "\n";
        output << ".temp 25\n"
               << "VDD vdd 0 " << vdd << "\n"
               << "VSS vss 0 0\n"
               << "VD d 0 " << vd << "\n"
               << "VG g 0 " << vd << "\n"
               << "X1 d g vss vss " << info.exampleDevices.at("nmos") << " " << deviceDimensions(normalizeId(info.id))
               << "\n"
               << ".print tran v(d)\n"
               << ".tran 1p 1n\n"
               << ".end\n";
    }
    if (!info.initCommands.empty()) {
        std::ofstream output(init);
        for (const auto &command : info.initCommands)
            output << command << "\n";
    }
    if (!std::filesystem::is_regular_file(netlist, status) || status) {
        result.summary = "Unable to write the PDK probe netlist.";
        return result;
    }

    const auto seconds = std::max<std::int64_t>(1, (timeout.count() + 999) / 1000);
    const auto command = "cd " + shellQuote(directory.string()) + " && HOME=" + shellQuote(directory.string()) +
                         " timeout " + std::to_string(seconds) + "s ngspice -b -o " + shellQuote(logPath.string()) +
                         " " + shellQuote(netlist.string()) + " >/dev/null 2>&1";
    const auto exitCode = std::system(command.c_str());
    const auto log = readPrefix(logPath, 64 * 1024);
    result.log = log.size() > 4000 ? "..." + log.substr(log.size() - 4000) : log;
    result.ok = exitCode == 0 && !logLooksLikeFailure(log);
    result.summary =
        result.ok ? "ngspice loaded the PDK model and biased a minimum-size NMOS." : firstUsefulLogLine(log);
    if (result.summary.empty())
        result.summary = result.ok ? "PDK probe succeeded." : "PDK probe failed; see the ngspice log.";
    std::filesystem::remove_all(directory, status);
    return result;
}

PdkSimulationSetup pdkSimulationSetupFor(const std::string &pdkId) {
    PdkSimulationSetup setup;
    if (pdkId.empty() || pdkId == genericPdkId)
        return setup;
    const auto pdks = findPdks(pdkId);
    if (pdks.empty())
        throw ProjectError("pdk.package", "No ICE PDK package provides '" + pdkId +
                                              "'. Install the package with ice-pkg before simulating.");
    const PdkInfo *selected = nullptr;
    for (const auto &pdk : pdks) {
        if (pdk.simulationReady) {
            selected = &pdk;
            break;
        }
    }
    if (!selected)
        selected = &pdks.front();
    if (!selected->packageSimulation)
        throw ProjectError("pdk.package", "ICE PDK package for '" + pdkId + "' does not declare [pdk.simulation].");
    if (!selected->simulationReady) {
        std::string missing;
        for (const auto &item : selected->missing) {
            if (!missing.empty())
                missing += "; ";
            missing += item;
        }
        throw ProjectError("pdk.not_ready", "PDK '" + pdkId + "' is not simulation-ready: " + missing);
    }
    setup.netlistLines = selected->recommendedSetup;
    setup.initCommands = selected->initCommands;
    return setup;
}

void applyPdkSimulationSetup(const std::string &pdkId, SimulationRequest &request) {
    const auto setup = pdkSimulationSetupFor(pdkId);
    if (setup.netlistLines.empty() && setup.initCommands.empty())
        return;
    std::vector<std::string> merged = setup.netlistLines;
    for (const auto &line : request.netlist) {
        if (std::find(merged.begin(), merged.end(), line) == merged.end())
            merged.push_back(line);
    }
    request.netlist = std::move(merged);
    for (const auto &command : setup.initCommands) {
        if (std::find(request.initCommands.begin(), request.initCommands.end(), command) == request.initCommands.end())
            request.initCommands.push_back(command);
    }
}

bool migrateProjectPdkIds(Project &project) {
    bool changed = false;
    const auto migrate = [&](std::string &value) {
        if (value.empty() || value == genericPdkId)
            return;
        const auto pdks = findPdks(value);
        if (pdks.empty())
            return;
        if (pdks.front().id != value) {
            value = pdks.front().id;
            changed = true;
        }
    };
    for (auto &[_, cell] : project.schematicCells)
        migrate(cell.pdk);
    for (auto &[_, cell] : project.layoutCells)
        migrate(cell.pdk);
    for (auto &[_, cell] : project.proceduralLayouts)
        migrate(cell.pdk);
    for (auto &dependency : project.packageDependencies) {
        const auto pdks = findPdks(dependency);
        if (!pdks.empty() && pdks.front().packageName != dependency) {
            dependency = pdks.front().packageName;
            changed = true;
        }
    }
    return changed;
}

nlohmann::json pdkInfoJson(const PdkInfo &info, bool includeFiles) {
    json result = {{"id", info.id},
                   {"source", info.source},
                   {"package", info.packageName},
                   {"package_version", info.packageVersion},
                   {"package_root", info.packageRoot.string()},
                   {"version_root", info.versionRoot.string()},
                   {"version", info.version},
                   {"ngspice_model_dir", info.root.empty() ? json(nullptr) : json(info.root.string())},
                   {"model_file", info.modelFile.empty() ? json(nullptr) : json(info.modelFile.string())},
                   {"model_section", info.modelSection},
                   {"sections", info.sections},
                   {"osdi_dir", info.osdiFiles.empty() ? json(nullptr) : json(info.osdiDir.string())},
                   {"osdi_file_count", info.osdiFiles.size()},
                   {"needs_osdi", info.needsOsdi},
                   {"spiceinit", info.hasSpiceInit ? json(info.spiceInitPath.string()) : json(nullptr)},
                   {"openvaf", info.hasOpenVaf},
                   {"simulation_ready", info.simulationReady},
                   {"missing", info.missing},
                   {"notes", info.notes},
                   {"recommended_lib_line", info.recommendedLibLine},
                   {"recommended_setup", info.recommendedSetup},
                   {"init_commands", info.initCommands},
                   {"example_devices", info.exampleDevices}};
    if (includeFiles) {
        json files = json::array();
        for (const auto &file : info.modelFiles)
            files.push_back({{"path", file.path.string()}, {"sections", file.sections}});
        result["model_files"] = std::move(files);
        result["osdi_files"] = json::array();
        for (const auto &file : info.osdiFiles)
            result["osdi_files"].push_back(file.string());
    }
    return result;
}

} // namespace ice
