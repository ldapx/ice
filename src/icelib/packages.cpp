#include "icelib/packages.hpp"

#include "icelib/core.hpp"

#include <openssl/sha.h>
#include <fstream>
#include <algorithm>
#include <array>
#include <iomanip>
#include <sstream>
#include <toml++/toml.h>
#include <cstdlib>

namespace ice {
namespace {
std::string readString(const toml::table& table, std::string_view key, std::string fallback = {}) {
    if (auto value = table[key].value<std::string>()) return *value;
    return fallback;
}

bool satisfies(const std::string& version, const std::string& constraint) {
    if (constraint.empty() || constraint == "*" || constraint == version) return true;
    if (constraint.starts_with("^")) return version.starts_with(constraint.substr(1, constraint.find('.', 1) - 1));
    if (constraint.starts_with(">=")) return version >= constraint.substr(2);
    return false;
}

std::string shellQuote(const std::string& value) {
    std::string result = "'";
    for (const char character : value) {
        if (character == '\'') result += "'\\''";
        else result += character;
    }
    return result + "'";
}

std::filesystem::path findPackage(const std::vector<std::filesystem::path>& roots, const std::string& name,
                                  const std::string& constraint) {
    std::filesystem::path selected;
    std::string selectedVersion;
    for (const auto& root : roots) {
        if (!std::filesystem::exists(root)) continue;
        for (const auto& entry : std::filesystem::directory_iterator(root)) {
            if (!entry.is_directory()) continue;
            try {
                auto spec = PackageManager::readManifest(entry.path());
                if (spec.name != name || !satisfies(spec.version, constraint)) continue;
                if (selected.empty() || spec.version > selectedVersion) {
                    selected = entry.path();
                    selectedVersion = spec.version;
                }
            } catch (const ProjectError&) {
            }
        }
    }
    if (selected.empty()) throw ProjectError("package.resolve", "Unable to resolve package: " + name);
    return selected;
}
} // namespace

PackageSpec PackageManager::readManifest(const std::filesystem::path& packageRoot) {
    try {
        auto table = toml::parse_file((packageRoot / "package.toml").string());
        const auto* package = table["package"].as_table();
        if (!package) throw ProjectError("package.manifest", "Missing [package] table");
        PackageSpec spec;
        spec.schemaVersion = static_cast<int>((*package)["schema_version"].value_or(1LL));
        spec.name = readString(*package, "name");
        spec.version = readString(*package, "version");
        spec.description = readString(*package, "description");
        spec.license = readString(*package, "license");
        spec.root = packageRoot;
        if (spec.name.empty() || spec.version.empty())
            throw ProjectError("package.manifest", "Package name and version are required");
        if (const auto* dependencies = table["dependencies"].as_table())
            for (const auto& [key, value] : *dependencies)
                if (auto text = value.value<std::string>()) spec.dependencies.emplace(std::string(key), *text);
        if (const auto* provides = table["provides"].as_table())
            for (const auto& [key, value] : *provides)
                if (auto enabled = value.value<bool>()) spec.capabilities.emplace(std::string(key), *enabled);
        if (const auto* trust = table["trust"].as_table())
            spec.trusted = (*trust)["trusted"].value_or(false);
        if (const auto* source = table["source"].as_table()) {
            spec.sourceType = readString(*source, "type");
            spec.sourceUrl = readString(*source, "url");
            spec.sourceRevision = readString(*source, "revision");
        }
        return spec;
    } catch (const ProjectError&) {
        throw;
    } catch (const std::exception& error) {
        throw ProjectError("package.manifest", error.what());
    }
}

void PackageManager::installGit(const std::filesystem::path& repository, const std::string& name,
                                const std::string& version, const std::string& url,
                                const std::string& revision) {
        if (name.empty() || version.empty() || url.empty())
            throw ProjectError("package.install", "Package name, version, and URL are required");
        if (name.find_first_of("/\\") != std::string::npos || name == "." || name == "..")
            throw ProjectError("package.install", "Package name must not contain path separators");
        std::filesystem::create_directories(repository);
        const auto destination = repository / name;
        if (std::filesystem::exists(destination))
            throw ProjectError("package.install", "Destination already exists: " + destination.string());
        const auto cloneCommand = "git clone --depth 1 " + (revision.empty() ? std::string{} : "--branch " + shellQuote(revision) + " ")
                                  + "-- " + shellQuote(url) + " " + shellQuote(destination.string());
        if (std::system(cloneCommand.c_str()) != 0)
            throw ProjectError("package.install", "Git clone failed for: " + url);
        std::ofstream manifest(destination / "package.toml");
        if (!manifest) throw ProjectError("package.install", "Unable to write generated package manifest");
        manifest << "[package]\n"
                 << "schema_version = 1\n"
                 << "name = \"" << name << "\"\n"
                 << "version = \"" << version << "\"\n"
                 << "description = \"Imported external package\"\n"
                 << "license = \"See upstream repository\"\n\n"
                 << "[source]\n"
                 << "type = \"git\"\n"
                 << "url = \"" << url << "\"\n";
        if (!revision.empty()) manifest << "revision = \"" << revision << "\"\n";
        manifest << "\n[provides]\n"
                 << "pdk = true\n"
                 << "models = true\n"
                 << "symbols = true\n"
                 << "layout = true\n\n"
                 << "[trust]\n"
                 << "trusted = false\n";
}

std::string PackageManager::contentHash(const std::filesystem::path& packageRoot) {
    SHA256_CTX context;
    SHA256_Init(&context);
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(packageRoot))
        if (entry.is_regular_file()) files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    std::array<unsigned char, 8192> buffer{};
    for (const auto& file : files) {
        auto relative = std::filesystem::relative(file, packageRoot).generic_string();
        SHA256_Update(&context, relative.data(), relative.size());
        std::ifstream input(file, std::ios::binary);
        while (input) {
            input.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
            auto count = input.gcount();
            if (count > 0) SHA256_Update(&context, buffer.data(), static_cast<std::size_t>(count));
        }
    }
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256_Final(digest.data(), &context);
    std::ostringstream result;
    result << "sha256:";
    for (auto byte : digest) result << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    return result.str();
}

Lockfile PackageManager::resolve(const std::vector<std::filesystem::path>& repositories,
                                 const std::map<std::string, std::string>& requested, std::string iceVersion) {
    Lockfile lock;
    lock.iceVersion = std::move(iceVersion);
    std::map<std::string, std::string> pending = requested;
    std::map<std::string, bool> resolved;
    while (!pending.empty()) {
        auto [name, constraint] = *pending.begin();
        pending.erase(pending.begin());
        if (resolved.contains(name)) continue;
        auto root = findPackage(repositories, name, constraint);
        auto spec = readManifest(root);
        resolved[name] = true;
        lock.packages.push_back({spec.name, spec.version, root.generic_string(), contentHash(root), spec.trusted, spec.capabilities});
        for (const auto& dependency : spec.dependencies) pending.emplace(dependency.first, dependency.second);
    }
    std::sort(lock.packages.begin(), lock.packages.end(), [](const auto& left, const auto& right) {
        return left.name < right.name;
    });
    return lock;
}

std::string serializeLockfile(const Lockfile& lockfile) {
    std::ostringstream output;
    output << "schema_version = " << lockfile.schemaVersion << "\nice_version = \"" << lockfile.iceVersion << "\"\n\n";
    for (const auto& package : lockfile.packages) {
        output << "[[package]]\nname = \"" << package.name << "\"\nversion = \"" << package.version
               << "\"\nsource = \"" << package.source << "\"\ncontent_hash = \"" << package.contentHash
               << "\"\ntrusted = " << (package.trusted ? "true" : "false") << "\n";
        for (const auto& [capability, enabled] : package.capabilities)
            output << "provides." << capability << " = " << (enabled ? "true" : "false") << "\n";
        output << "\n";
    }
    return output.str();
}

void PackageManager::writeLockfile(const std::filesystem::path& path, const Lockfile& lockfile) {
    std::ofstream output(path);
    if (!output) throw ProjectError("package.lockfile", "Unable to open lockfile for writing");
    output << serializeLockfile(lockfile);
}

Lockfile PackageManager::readLockfile(const std::filesystem::path& path) {
    try {
        auto table = toml::parse_file(path.string());
        Lockfile lock;
        lock.schemaVersion = static_cast<int>(table["schema_version"].value_or(1LL));
        lock.iceVersion = table["ice_version"].value_or(std::string{});
        if (const auto* packages = table["package"].as_array())
            for (const auto& item : *packages) {
                const auto* package = item.as_table();
                if (!package) continue;
                lock.packages.push_back({readString(*package, "name"), readString(*package, "version"),
                                         readString(*package, "source"), readString(*package, "content_hash"),
                                         package->at("trusted").value_or(false), {}});
            }
        return lock;
    } catch (const std::exception& error) {
        throw ProjectError("package.lockfile", error.what());
    }
}

void PackageManager::verifyLockfile(const Lockfile& lockfile, const std::vector<std::filesystem::path>& repositories) {
    for (const auto& locked : lockfile.packages) {
        auto root = findPackage(repositories, locked.name, locked.version);
        if (contentHash(root) != locked.contentHash)
            throw ProjectError("package.integrity", "Content hash mismatch for package: " + locked.name);
    }
}

std::vector<std::string> PackageManager::capabilities(const PackageSpec& package) {
    std::vector<std::string> result;
    for (const auto& [name, enabled] : package.capabilities)
        if (enabled) result.push_back(name);
    return result;
}

std::vector<ProvenanceDiagnostic> PackageManager::provenance(
    const Lockfile& lockfile, const std::vector<std::filesystem::path>& repositories) {
    std::vector<ProvenanceDiagnostic> result;
    for (const auto& package : lockfile.packages) {
        try {
            auto root = findPackage(repositories, package.name, package.version);
            if (contentHash(root) != package.contentHash)
                result.push_back({"package.integrity", "Package content hash does not match the lockfile", package.name});
            auto manifest = readManifest(root);
            if (manifest.trusted != package.trusted)
                result.push_back({"package.trust", "Package trust metadata changed since resolution", package.name});
        } catch (const ProjectError& error) {
            result.push_back({error.code(), error.what(), package.name});
        }
    }
    return result;
}
} // namespace ice
