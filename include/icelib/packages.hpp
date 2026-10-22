#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace ice {

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

class PackageManager {
  public:
    static PackageSpec readManifest(const std::filesystem::path &packageRoot);
    static void installGit(const std::filesystem::path &repository, const std::string &name, const std::string &version,
                           const std::string &url, const std::string &revision = {});
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

} // namespace ice
