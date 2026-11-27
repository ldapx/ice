#include "openvlsilib/pdk.hpp"

#include "openvlsilib/platform.hpp"
#include "openvlsilib/process.hpp"

#include "openvlsilib/core.hpp"
#include "openvlsilib/implementation.hpp"
#include "openvlsilib/simulation.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#else
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace openvlsi {
namespace {

using json = nlohmann::json;
std::vector<std::filesystem::path> packageRootOverride;

namespace {
std::mutex connectivityMutex;
std::map<std::string, std::optional<PdkConnectivity>> connectivityCache;
} // namespace

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

// Parses the numeric part of a SPICE quantity such as "1u", "1e-6" or "0.5meg"
// into a plain double. Returns nullopt for expressions the simple parser cannot
// evaluate (for example `{scale_param}`).
std::optional<double> parseSpiceQuantity(const std::string &text) {
    std::string trimmed = trimCopy(text);
    if (trimmed.empty())
        return std::nullopt;
    char *end = nullptr;
    const auto number = std::strtod(trimmed.c_str(), &end);
    if (end == trimmed.c_str())
        return std::nullopt;
    std::string suffix = lowerCopy(trimCopy(std::string(end)));
    double multiplier = 1.0;
    if (suffix.empty())
        multiplier = 1.0;
    else if (suffix == "t")
        multiplier = 1e12;
    else if (suffix == "g")
        multiplier = 1e9;
    else if (suffix == "meg")
        multiplier = 1e6;
    else if (suffix == "k")
        multiplier = 1e3;
    else if (suffix == "m")
        multiplier = 1e-3;
    else if (suffix == "u")
        multiplier = 1e-6;
    else if (suffix == "n")
        multiplier = 1e-9;
    else if (suffix == "p")
        multiplier = 1e-12;
    else if (suffix == "f")
        multiplier = 1e-15;
    else
        return std::nullopt;
    return number * multiplier;
}

// Extracts the geometry scale from a set of netlist preamble lines, accepting
// ".option scale=1u", ".options scale = 1u" and several comma separated options.
std::optional<double> geometryScaleFromLines(const std::vector<std::string> &lines) {
    for (const auto &line : lines) {
        const auto trimmed = lowerCopy(trimCopy(line));
        if (!trimmed.starts_with(".option"))
            continue;
        auto body = trimmed.substr(trimmed.find_first_of(" \t") == std::string::npos ? trimmed.size()
                                                                                     : trimmed.find_first_of(" \t"));
        std::size_t position = 0;
        while ((position = body.find("scale", position)) != std::string::npos) {
            const auto equals = body.find('=', position);
            if (equals == std::string::npos)
                break;
            const auto following = body.find_first_of(", \t", equals + 1);
            const auto value = parseSpiceQuantity(body.substr(equals + 1, following - equals - 1));
            if (value && *value > 0.0)
                return value;
            position = equals + 1;
        }
    }
    return std::nullopt;
}

std::string normalizeId(std::string value) {
    value = lowerCopy(std::move(value));
    std::replace(value.begin(), value.end(), '.', '-');
    std::replace(value.begin(), value.end(), '_', '-');
    return value;
}

std::string readPrefix(const std::filesystem::path &path, std::size_t limit = std::size_t{512} * 1024) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    std::string text;
    text.resize(limit);
    input.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(input.gcount()));
    return text;
}

bool executableInPath(const std::string &name) { return findExecutableOnPath(name).has_value(); }

std::filesystem::path cacheRoot() {
    if (const auto *value = std::getenv("OPENVLSI_PDK_CACHE"); value && *value)
        return value;
    if (const auto *xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg)
        return std::filesystem::path(xdg) / "openvlsi" / "pdk";
    return homeDirectory() / ".openvlsi" / "cache" / "pdk";
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

class PdkBuildLock {
  public:
    explicit PdkBuildLock(const std::filesystem::path &directory) {
        std::error_code status;
        std::filesystem::create_directories(directory, status);
        const auto path = directory / "build.lock";
#ifndef _WIN32
        descriptor_ = ::open(path.c_str(), O_CREAT | O_RDWR, 0666);
        if (descriptor_ >= 0) {
            while (::flock(descriptor_, LOCK_EX) != 0 && errno == EINTR) {
            }
        }
#else
        handle_ = CreateFileW(path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ != INVALID_HANDLE_VALUE) {
            OVERLAPPED overlapped{};
            LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD, &overlapped);
        }
#endif
    }
    ~PdkBuildLock() {
#ifndef _WIN32
        if (descriptor_ >= 0) {
            ::flock(descriptor_, LOCK_UN);
            ::close(descriptor_);
        }
#else
        if (handle_ != INVALID_HANDLE_VALUE) {
            OVERLAPPED overlapped{};
            UnlockFileEx(handle_, 0, MAXDWORD, MAXDWORD, &overlapped);
            CloseHandle(handle_);
        }
#endif
    }

  private:
#ifndef _WIN32
    int descriptor_ = -1;
#else
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#endif
};

std::string setupLine(const std::filesystem::path &path) { return path.generic_string(); }

std::string layerBaseName(std::string name) {
    if (const auto dot = name.rfind('.'); dot != std::string::npos)
        name = name.substr(0, dot);
    return lowerCopy(std::move(name));
}

std::vector<std::string> stringOrArray(const json &value) {
    std::vector<std::string> result;
    if (value.is_string()) {
        result.push_back(value.get<std::string>());
        return result;
    }
    if (value.is_array()) {
        for (const auto &entry : value) {
            if (entry.is_string())
                result.push_back(entry.get<std::string>());
        }
    }
    return result;
}

PdkInfo infoFromPackage(const PackageIndexEntry &entry) {
    const auto &spec = entry.spec;
    PdkInfo info;
    info.packageName = spec.name;
    info.packageVersion = spec.version;
    info.packageRoot = spec.root;
    info.versionRoot = spec.root;
    info.version = spec.version;
    info.source = "openvlsi-package";
    info.id = spec.pdk ? spec.pdk->id : spec.name;
    info.exampleDevices.clear();
    info.hasOpenVaf = executableInPath("openvaf-r") || executableInPath("openvaf");

    if (!spec.pdk) {
        info.missing.push_back("package does not declare a [pdk] section");
        return info;
    }
    info.manufacturingGrid = spec.pdk->manufacturingGrid;
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
    // Like model_file/includes/verilog_a, spiceinit is relative to the model
    // root ([pdk.simulation].root), not to the package root.
    info.spiceInitPath = sim.spiceinit.empty() ? std::filesystem::path{} : info.root / sim.spiceinit;
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
        info.missing.push_back("OSDI models not built; run openvlsi-pkg pdk-build " + spec.name);
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

    // ngspice reads its start-up commands from ~/.spiceinit, which is not
    // guaranteed to exist (or to belong to the selected PDK). Packages that
    // ship one - SKY130's `spinit` sets `ngbehavior=hsa`, required for the
    // binned subcircuit models - get their commands replayed before the circuit
    // is loaded, so a run does not depend on the user's home directory.
    if (info.hasSpiceInit) {
        std::ifstream initFile(info.spiceInitPath);
        std::string initLine;
        while (std::getline(initFile, initLine)) {
            const auto command = trimCopy(initLine);
            if (command.empty() || command.front() == '*' || command.front() == ';' || command.front() == '.')
                continue;
            if (std::find(info.initCommands.begin(), info.initCommands.end(), command) == info.initCommands.end())
                info.initCommands.push_back(command);
        }
        info.notes.push_back("ngspice init: " + info.spiceInitPath.string());
    }

    info.notes.push_back("OpenVLSI package: " + spec.name + " " + spec.version);
    if (!info.osdiDir.empty())
        info.notes.push_back("OSDI cache: " + info.osdiDir.string());
    if (info.needsOsdi && info.hasOpenVaf && info.osdiFiles.empty())
        info.notes.push_back("OSDI compiler is available; run openvlsi-pkg pdk-build " + spec.name);

    info.simulationReady = info.missing.empty();
    if (spec.pdk->implementation) {
        std::vector<Diagnostic> diagnostics;
        try {
            const auto stack = resolvePdkStack(*spec.pdk, {});
            const auto platform = resolveImplementationPlatform(spec, stack, diagnostics);
            info.implementationEngine = spec.pdk->implementation->engine;
            info.implementationStack = stack;
            info.implementationReady = platform.techLef.generic_string().size() > 0 &&
                                       platform.cellLef.generic_string().size() > 0 && !platform.liberty.empty() &&
                                       platform.site.size() > 0;
            for (const auto &diagnostic : diagnostics) {
                if (diagnostic.severity == Diagnostic::Severity::error)
                    info.implementationMissing.push_back(diagnostic.message);
                else
                    info.implementationNotes.push_back(diagnostic.message);
            }
            if (info.implementationReady)
                info.implementationNotes.push_back("Implementation platform ready for stack '" + stack + "'");
        } catch (const ProjectError &error) {
            info.implementationMissing.push_back(error.what());
        }
    }
    return info;
}

} // namespace

PdkConnectivity PdkConnectivity::load(const std::filesystem::path &path) {
    std::ifstream input(path);
    if (!input)
        throw ProjectError("pdk.connectivity", "Unable to read PDK connectivity: " + path.string());
    json document;
    try {
        input >> document;
    } catch (const std::exception &error) {
        throw ProjectError("pdk.connectivity", "Invalid PDK connectivity " + path.string() + ": " + error.what());
    }
    if (!document.is_object())
        throw ProjectError("pdk.connectivity", "PDK connectivity must be a JSON object: " + path.string());
    if (document.value("schema_version", 1) != 1)
        throw ProjectError("pdk.connectivity", "PDK connectivity must use schema_version 1: " + path.string());

    PdkConnectivity connectivity;
    connectivity.declared = true;
    if (const auto layers = document.find("layers"); layers != document.end()) {
        if (!layers->is_array())
            throw ProjectError("pdk.connectivity", "PDK connectivity 'layers' must be an array");
        for (const auto &item : *layers) {
            if (!item.is_object())
                throw ProjectError("pdk.connectivity", "PDK connectivity layer entries must be objects");
            PdkLayerRole entry;
            entry.name = lowerCopy(item.value("name", std::string{}));
            entry.role = lowerCopy(item.value("role", std::string{}));
            entry.sheetResistance = item.value("sheet_resistance", 0.0);
            if (entry.name.empty() || entry.role.empty())
                throw ProjectError("pdk.connectivity", "PDK connectivity layers require name and role");
            static const std::set<std::string> roles = {
                "conductor", "metal",   "active", "diffusion", "gate",  "poly", "via",     "pad",
                "marker",    "implant", "well",   "boundary",  "label", "text", "ignored", "insulator"};
            if (!roles.contains(entry.role))
                throw ProjectError("pdk.connectivity", "Unknown PDK connectivity role: " + entry.role);
            if (entry.sheetResistance < 0.0)
                throw ProjectError("pdk.connectivity", "PDK connectivity sheet_resistance must not be negative");
            connectivity.layers.push_back(std::move(entry));
        }
    }
    if (const auto vias = document.find("vias"); vias != document.end()) {
        if (!vias->is_array())
            throw ProjectError("pdk.connectivity", "PDK connectivity 'vias' must be an array");
        for (const auto &item : *vias) {
            if (!item.is_object())
                throw ProjectError("pdk.connectivity", "PDK connectivity via entries must be objects");
            PdkVia entry;
            entry.name = lowerCopy(item.value("name", std::string{}));
            if (const auto lower = item.find("lower"); lower != item.end())
                for (const auto &name : stringOrArray(*lower))
                    entry.lower.push_back(layerBaseName(name));
            if (const auto upper = item.find("upper"); upper != item.end())
                for (const auto &name : stringOrArray(*upper))
                    entry.upper.push_back(layerBaseName(name));
            entry.size = item.value("size", 0.0);
            entry.enclosure = item.value("enclosure", 0.0);
            if (entry.name.empty() || entry.lower.empty() || entry.upper.empty())
                throw ProjectError("pdk.connectivity", "PDK connectivity vias require name, lower, and upper");
            if (entry.size < 0.0 || entry.enclosure < 0.0)
                throw ProjectError("pdk.connectivity", "PDK connectivity via size/enclosure must not be negative");
            connectivity.vias.push_back(std::move(entry));
        }
    }
    if (const auto routing = document.find("routing"); routing != document.end() && routing->is_object()) {
        if (const auto stack = routing->find("stack"); stack != routing->end()) {
            if (!stack->is_array())
                throw ProjectError("pdk.connectivity", "PDK connectivity routing.stack must be an array");
            for (const auto &entry : *stack)
                if (entry.is_string())
                    connectivity.routingStack.push_back(entry.get<std::string>());
        }
    } else if (const auto stack = document.find("routing_stack"); stack != document.end()) {
        if (!stack->is_array())
            throw ProjectError("pdk.connectivity", "PDK connectivity routing_stack must be an array");
        for (const auto &entry : *stack)
            if (entry.is_string())
                connectivity.routingStack.push_back(entry.get<std::string>());
    }
    return connectivity;
}

std::string PdkConnectivity::roleFor(const std::string &layerName) const {
    const auto lower = lowerCopy(layerName);
    for (const auto &layer : layers) {
        if (layer.name == lower)
            return layer.role;
    }
    if (const auto dot = lower.rfind('.'); dot != std::string::npos) {
        // Purpose variants that annotate or block a conductor are not part of
        // the electrical layer even when the package declares the base layer
        // as a conductor.
        static const std::set<std::string> nonConductingPurposes = {
            "label", "text",    "boundary", "blockage", "block", "dummy", "slot",  "nofill",
            "noqrc", "keepout", "cut",      "res",      "mask",  "net",   "probe", "short"};
        if (nonConductingPurposes.contains(lower.substr(dot + 1)))
            return "marker";
    }
    const auto base = layerBaseName(layerName);
    for (const auto &layer : layers) {
        if (layer.name == base)
            return layer.role;
    }
    return {};
}

const PdkVia *PdkConnectivity::viaFor(const std::string &layerName) const {
    const auto base = layerBaseName(layerName);
    for (const auto &via : vias) {
        if (via.name == base)
            return &via;
    }
    return nullptr;
}

const PdkVia *PdkConnectivity::viaBetween(const std::string &lower, const std::string &upper) const {
    const auto lowerBase = layerBaseName(lower);
    const auto upperBase = layerBaseName(upper);
    const auto contains = [](const std::vector<std::string> &names, const std::string &name) {
        return std::find(names.begin(), names.end(), name) != names.end();
    };
    for (const auto &via : vias) {
        if ((contains(via.lower, lowerBase) && contains(via.upper, upperBase)) ||
            (contains(via.lower, upperBase) && contains(via.upper, lowerBase)))
            return &via;
    }
    return nullptr;
}

void setPdkPackageRoots(std::vector<std::filesystem::path> roots) {
    packageRootOverride = std::move(roots);
    std::lock_guard<std::mutex> lock(connectivityMutex);
    connectivityCache.clear();
}

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
        for (const auto &part : splitPathList(value)) {
            if (!part.empty())
                roots.emplace_back(part);
        }
    };
    bool explicitRoots = false;
    if (const auto *value = std::getenv("OPENVLSI_PACKAGE_ROOT"); value && *value) {
        append(value);
        explicitRoots = true;
    }
    if (const auto *value = std::getenv("OPENVLSI_PACKAGE_ROOTS"); value && *value) {
        append(value);
        explicitRoots = true;
    }
    if (!explicitRoots) {
        const auto home = homeDirectory();
        if (!home.empty())
            roots.emplace_back(home / ".openvlsi" / "packages");
        if (const auto *xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg)
            roots.emplace_back(std::filesystem::path(xdg) / "openvlsi" / "packages");
        else if (!home.empty())
            roots.emplace_back(home / ".local" / "share" / "openvlsi" / "packages");
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

Coordinate pdkManufacturingGrid(const std::string &idOrPath) {
    const auto pdks = findPdks(idOrPath);
    return pdks.empty() ? 0 : pdks.front().manufacturingGrid;
}

std::optional<PdkConnectivity> pdkConnectivityFor(const std::string &pdkId) {
    if (pdkId.empty() || pdkId == genericPdkId)
        return std::nullopt;
    const auto key = normalizeId(pdkId);
    {
        std::lock_guard<std::mutex> lock(connectivityMutex);
        if (const auto cached = connectivityCache.find(key); cached != connectivityCache.end())
            return cached->second;
    }
    std::optional<PdkConnectivity> result;
    for (const auto &info : findPdks(pdkId)) {
        const auto path = info.packageRoot / "technology" / "connectivity.json";
        std::error_code status;
        if (!std::filesystem::is_regular_file(path, status) || status)
            continue;
        try {
            result = PdkConnectivity::load(path);
        } catch (const ProjectError &) {
            result.reset();
        }
        break;
    }
    {
        std::lock_guard<std::mutex> lock(connectivityMutex);
        connectivityCache[key] = result;
    }
    return result;
}

void applyPdkDefaultGrid(Project &project, const std::string &pdkId) {
    const auto grid = pdkManufacturingGrid(pdkId);
    if (grid > 0 && project.grid == 1)
        project.grid = grid;
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
        ProcessOptions options;
        options.arguments.push_back(compiler);
        for (const auto &flag : info.packageSimulation->compilerFlags)
            options.arguments.push_back(flag);
        options.arguments.push_back(input.filename().string());
        options.arguments.push_back("--output");
        options.arguments.push_back(output.string());
        options.workingDirectory = input.parent_path();
        options.logPath = info.osdiDir / "build.log";
        options.timeoutSeconds = static_cast<int>(std::max<std::int64_t>(1, (timeout.count() + 999) / 1000));
        const auto process = runProcess(options);
        const auto exitCode = process.started ? process.exitCode : -1;
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
    const auto directory = std::filesystem::temp_directory_path() / ("openvlsi-pdk-probe-" + std::to_string(stamp));
    std::error_code status;
    std::filesystem::create_directories(directory, status);
    const auto netlist = directory / "probe.sp";
    const auto logPath = directory / "probe.log";
    const auto init = directory / ".spiceinit";
    const auto &probe = info.packageSimulation->probe;
    const auto width = probe.declared ? probe.width : std::string{"1u"};
    const auto length = probe.declared ? probe.length : std::string{"0.13u"};
    const auto vdd = probe.declared ? probe.vdd : 1.2;
    const auto vd = probe.declared ? probe.vd : 0.6;
    {
        std::ofstream output(netlist);
        output << "* OpenVLSI PDK probe\n";
        for (const auto &line : info.recommendedSetup)
            output << line << "\n";
        output << ".temp 25\n"
               << "VDD vdd 0 " << vdd << "\n"
               << "VSS vss 0 0\n"
               << "VD d 0 " << vd << "\n"
               << "VG g 0 " << vd << "\n"
               << "X1 d g vss vss " << info.exampleDevices.at("nmos") << " w=" << width << " l=" << length << "\n"
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

    ProcessOptions options;
    options.arguments = {"ngspice", "-b", "-o", logPath.string(), netlist.string()};
    options.workingDirectory = directory;
    options.environment["HOME"] = directory.string();
    options.environment["USERPROFILE"] = directory.string();
    options.timeoutSeconds = static_cast<int>(std::max<std::int64_t>(1, (timeout.count() + 999) / 1000));
    options.logTailBytes = 64 * 1024;
    const auto process = runProcess(options);
    const auto log = readPrefix(logPath, std::size_t{64} * 1024);
    result.log = log.size() > 4000 ? "..." + log.substr(log.size() - 4000) : log;
    result.ok = process.succeeded() && !logLooksLikeFailure(log);
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
        throw ProjectError("pdk.package", "No OpenVLSI PDK package provides '" + pdkId +
                                              "'. Install the package with openvlsi-pkg before simulating.");
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
        throw ProjectError("pdk.package", "OpenVLSI PDK package for '" + pdkId + "' does not declare [pdk.simulation].");
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
    if (const auto scale = geometryScaleFromLines(setup.netlistLines))
        setup.geometryScale = *scale;
    return setup;
}

double pdkNetlistGeometryScale(const std::string &pdkId) {
    try {
        return pdkSimulationSetupFor(pdkId).geometryScale;
    } catch (const std::exception &) {
        return 1.0;
    }
}

std::optional<double> netlistLengthNumber(const std::string &text, double geometryScale) {
    const auto trimmed = trimCopy(text);
    if (trimmed.empty())
        return std::nullopt;
    char *end = nullptr;
    const auto number = std::strtod(trimmed.c_str(), &end);
    if (end == trimmed.c_str())
        return std::nullopt;
    const auto suffix = lowerCopy(trimCopy(std::string(end)));
    if (suffix.empty())
        return std::nullopt;
    double multiplier = 1.0;
    if (suffix == "t")
        multiplier = 1e12;
    else if (suffix == "g")
        multiplier = 1e9;
    else if (suffix == "meg")
        multiplier = 1e6;
    else if (suffix == "k")
        multiplier = 1e3;
    else if (suffix == "m")
        multiplier = 1e-3;
    else if (suffix == "u")
        multiplier = 1e-6;
    else if (suffix == "n")
        multiplier = 1e-9;
    else if (suffix == "p")
        multiplier = 1e-12;
    else if (suffix == "f")
        multiplier = 1e-15;
    else
        return std::nullopt;
    if (!(geometryScale > 0.0))
        return std::nullopt;
    return number * multiplier / geometryScale;
}

std::string netlistLengthText(const std::string &text, double geometryScale) {
    // Without a declared scale the netlist reads plain SPICE lengths, so an SI
    // suffix is already correct and the text is left alone.
    if (!(geometryScale > 0.0) || geometryScale == 1.0)
        return text;
    const auto value = netlistLengthNumber(text, geometryScale);
    if (!value)
        return text;
    std::ostringstream formatted;
    formatted << std::setprecision(12) << *value;
    return formatted.str();
}

namespace {

struct SubcircuitIndex {
    std::map<std::string, std::vector<std::string>> nodes;
    std::once_flag loaded;
};

std::mutex subcircuitIndexMutex;
std::map<std::string, SubcircuitIndex> subcircuitIndexes;

// Reads `.subckt <name> <nodes...>` definitions (with `+` continuations) from
// every SPICE file in a directory tree.
void loadSubcircuits(const std::filesystem::path &directory, SubcircuitIndex &index) {
    std::error_code status;
    if (!std::filesystem::is_directory(directory, status) || status)
        return;
    for (std::filesystem::recursive_directory_iterator it(directory, status), end; it != end; it.increment(status)) {
        if (status)
            break;
        if (!it->is_regular_file(status) || status)
            continue;
        const auto extension = lowerCopy(it->path().extension().string());
        if (extension != ".spice" && extension != ".lib" && extension != ".cir" && extension != ".mod")
            continue;
        std::ifstream input(it->path());
        if (!input)
            continue;
        std::string line;
        while (std::getline(input, line)) {
            const auto trimmed = trimCopy(line);
            if (trimmed.empty() || trimmed.front() == '*')
                continue;
            if (lowerCopy(trimmed).starts_with(".subckt")) {
                std::istringstream stream(trimmed);
                std::string token;
                stream >> token; // .subckt
                std::string name;
                if (!(stream >> name))
                    continue;
                std::vector<std::string> nodes;
                while (stream >> token) {
                    if (token.front() == '+' || token.front() == '(')
                        break;
                    nodes.push_back(token);
                }
                // Continuation lines for long node lists.
                std::streampos position = input.tellg();
                while (std::getline(input, line)) {
                    const auto more = trimCopy(line);
                    if (more.empty() || more.front() != '+') {
                        input.seekg(position);
                        break;
                    }
                    std::istringstream extra(more.substr(1));
                    while (extra >> token)
                        nodes.push_back(token);
                    position = input.tellg();
                }
                index.nodes.emplace(lowerCopy(name), std::move(nodes));
            }
        }
    }
}

} // namespace

std::optional<std::vector<std::string>> pdkSubcircuitNodes(const std::string &pdkId, const std::string &model) {
    if (pdkId.empty() || model.empty() || pdkId == genericPdkId)
        return std::nullopt;
    std::filesystem::path modelDirectory;
    try {
        const auto pdks = findPdks(pdkId);
        if (pdks.empty())
            return std::nullopt;
        const auto *selected = &pdks.front();
        for (const auto &pdk : pdks) {
            if (pdk.simulationReady) {
                selected = &pdk;
                break;
            }
        }
        modelDirectory = selected->ngspiceModelDir;
        if (modelDirectory.empty())
            modelDirectory = selected->root;
    } catch (const std::exception &) {
        return std::nullopt;
    }
    const auto key = pdkId + "|" + modelDirectory.string();
    SubcircuitIndex *index = nullptr;
    {
        std::lock_guard lock(subcircuitIndexMutex);
        index = &subcircuitIndexes[key];
    }
    std::call_once(index->loaded, [&] { loadSubcircuits(modelDirectory, *index); });
    const auto entry = index->nodes.find(lowerCopy(model));
    if (entry == index->nodes.end() || entry->second.empty())
        return std::nullopt;
    return entry->second;
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

nlohmann::json pdkInfoJson(const PdkInfo &info, bool includeFiles) {
    json result = {{"id", info.id},
                   {"source", info.source},
                   {"package", info.packageName},
                   {"package_version", info.packageVersion},
                   {"package_root", info.packageRoot.string()},
                   {"manufacturing_grid", info.manufacturingGrid},
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
                   {"implementation_ready", info.implementationReady},
                   {"implementation_engine", info.implementationEngine},
                   {"implementation_stack", info.implementationStack},
                   {"implementation_missing", info.implementationMissing},
                   {"implementation_notes", info.implementationNotes},
                   {"missing", info.missing},
                   {"notes", info.notes},
                   {"recommended_lib_line", info.recommendedLibLine},
                   {"recommended_setup", info.recommendedSetup},
                   {"init_commands", info.initCommands},
                   {"example_devices", info.exampleDevices}};
    const auto layoutLibrary = info.packageRoot / "layout" / "devices" / "index.json";
    const auto layoutGenerator = info.packageRoot / "layout" / "devices" / "generator.py";
    const auto layoutExtraction = info.packageRoot / "technology" / "extraction.json";
    const auto layoutConnectivity = info.packageRoot / "technology" / "connectivity.json";
    const auto drcConfig = info.packageRoot / "technology" / "drc.json";
    result["layout_library"] =
        std::filesystem::is_regular_file(layoutLibrary) ? json(layoutLibrary.string()) : json(nullptr);
    result["layout_generator"] =
        std::filesystem::is_regular_file(layoutGenerator) ? json(layoutGenerator.string()) : json(nullptr);
    result["layout_extraction"] =
        std::filesystem::is_regular_file(layoutExtraction) ? json(layoutExtraction.string()) : json(nullptr);
    result["layout_connectivity"] =
        std::filesystem::is_regular_file(layoutConnectivity) ? json(layoutConnectivity.string()) : json(nullptr);
    result["drc_config"] = std::filesystem::is_regular_file(drcConfig) ? json(drcConfig.string()) : json(nullptr);
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

} // namespace openvlsi
