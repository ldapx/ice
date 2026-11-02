#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ice {

struct PackageLayer {
    std::string name;
    int number = 0;
};

struct PackagePdkOsdiSource {
    std::filesystem::path source;
    std::string output;
};

struct PackagePdkSimulation {
    std::string engine = "ngspice";
    // Model directory relative to the package root.
    std::filesystem::path root;
    // Model file and includes relative to `root`.
    std::filesystem::path modelFile;
    std::string section;
    std::vector<std::filesystem::path> includes;
    std::vector<std::string> options;
    bool requiresOsdi = false;
    // Verilog-A root and spiceinit are relative to the package root.
    std::filesystem::path verilogA;
    std::filesystem::path spiceinit;
    std::vector<std::string> compilerFlags;
    std::vector<PackagePdkOsdiSource> osdiSources;
    std::map<std::string, std::string> devices;
};

struct PackagePdk {
    // Canonical PDK id. Package names are separate because a package may be
    // named `ihp.sg13g2` while schematics store `ihp-sg13g2`.
    std::string id;
    std::vector<std::string> aliases;
    std::string displayName;
    std::string defaultSchematic;
    std::vector<PackageLayer> layers;
    std::optional<PackagePdkSimulation> simulation;
};

struct PackageSpec {
    int schemaVersion = 1;
    std::string name;
    std::string version;
    std::string description;
    std::string license;
    std::map<std::string, std::string> dependencies;
    std::map<std::string, bool> capabilities;
    bool trusted = false;
    std::string sourceType;
    std::string sourceUrl;
    std::string sourceRevision;
    std::optional<PackagePdk> pdk;
    std::filesystem::path root;
};

struct LockedPackage {
    std::string name;
    std::string version;
    std::string source;
    std::string contentHash;
    bool trusted = false;
    std::map<std::string, bool> capabilities;
};

struct Lockfile {
    int schemaVersion = 1;
    std::string iceVersion;
    std::vector<LockedPackage> packages;
};

struct ProvenanceDiagnostic {
    std::string code;
    std::string message;
    std::string package;
};

struct PackageIndexEntry {
    PackageSpec spec;
    std::filesystem::path root;
    std::filesystem::path logo;
    std::string repositoryUrl;
    std::string repositoryRevision;
};

class PackageManager {
  public:
    static PackageSpec readManifest(const std::filesystem::path &packageRoot);
    static void installGit(const std::filesystem::path &repository, const std::string &name, const std::string &version,
                           const std::string &url, const std::string &revision = {});
    static PackageSpec installGitPackage(const std::filesystem::path &repository, const std::string &url,
                                         const std::string &revision = {});
    static void uninstallPackage(const std::filesystem::path &repository, const std::string &name);
    static std::vector<PackageIndexEntry> scanPackageIndex(const std::filesystem::path &indexRoot);
    static std::filesystem::path packageLogo(const std::filesystem::path &packageRoot);
    static void cloneIndex(const std::filesystem::path &indexRoot, const std::string &url,
                           const std::string &revision = {});
    static void updateIndex(const std::filesystem::path &indexRoot);
    static std::string contentHash(const std::filesystem::path &packageRoot);
    static Lockfile resolve(const std::vector<std::filesystem::path> &repositories,
                            const std::map<std::string, std::string> &requested, std::string iceVersion);
    static void writeLockfile(const std::filesystem::path &path, const Lockfile &lockfile);
    static Lockfile readLockfile(const std::filesystem::path &path);
    static void verifyLockfile(const Lockfile &lockfile, const std::vector<std::filesystem::path> &repositories);
    static std::vector<std::string> capabilities(const PackageSpec &package);
    static std::vector<ProvenanceDiagnostic> provenance(const Lockfile &lockfile,
                                                        const std::vector<std::filesystem::path> &repositories);
};

std::string serializeLockfile(const Lockfile &lockfile);

// True when the package exposes the requested PDK id, name, or alias.
bool packageMatchesPdk(const PackageSpec &package, const std::string &pdkId);

} // namespace ice
