#include "icelib/packages.hpp"

#include "icelib/core.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <openssl/sha.h>
#include <sstream>
#include <toml++/toml.h>

namespace ice {
namespace {
std::string readString(const toml::table &table, std::string_view key, std::string fallback = {}) {
    if (auto value = table[key].value<std::string>())
        return *value;
    return fallback;
}

std::vector<int> parseVersionParts(const std::string &version) {
    std::vector<int> parts;
    std::string current;
    for (char c : version) {
        if (c == '.') {
            try {
                parts.push_back(std::stoi(current));
            } catch (...) {
                return {};
            }
            current.clear();
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            current.push_back(c);
        } else {
            // Stop at pre-release/build metadata or any suffix.
            break;
        }
    }
    if (!current.empty()) {
        try {
            parts.push_back(std::stoi(current));
        } catch (...) {
            return {};
        }
    }
    return parts;
}

int compareVersions(const std::string &left, const std::string &right) {
    const auto l = parseVersionParts(left);
    const auto r = parseVersionParts(right);
    if (l.empty() || r.empty())
        return left.compare(right);
    const std::size_t count = std::max(l.size(), r.size());
    for (std::size_t i = 0; i < count; ++i) {
        const int lv = i < l.size() ? l[i] : 0;
        const int rv = i < r.size() ? r[i] : 0;
        if (lv != rv)
            return lv < rv ? -1 : 1;
    }
    return 0;
}

bool satisfies(const std::string &version, const std::string &constraint) {
    if (constraint.empty() || constraint == "*" || constraint == version)
        return true;
    if (constraint.starts_with('^')) {
        const std::string base = constraint.substr(1);
        if (compareVersions(version, base) < 0)
            return false;
        const auto baseParts = parseVersionParts(base);
        const auto versionParts = parseVersionParts(version);
        if (baseParts.empty() || versionParts.empty())
            return false;
        if (baseParts[0] != 0)
            return !versionParts.empty() && versionParts[0] == baseParts[0];
        if (baseParts.size() > 1 && baseParts[1] != 0)
            return versionParts.size() > 1 && versionParts[0] == 0 && versionParts[1] == baseParts[1];
        return compareVersions(version, base) == 0;
    }
    if (constraint.starts_with(">=")) {
        const std::string base = constraint.substr(2);
        // Trim leading whitespace after ">=".
        const auto first = base.find_first_not_of(" \t");
        const std::string trimmed = first == std::string::npos ? std::string{} : base.substr(first);
        if (trimmed.empty())
            return true;
        return compareVersions(version, trimmed) >= 0;
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

std::filesystem::path findPackage(const std::vector<std::filesystem::path> &roots, const std::string &name,
                                  const std::string &constraint) {
    std::filesystem::path selected;
    std::string selectedVersion;
    for (const auto &root : roots) {
        if (!std::filesystem::exists(root))
            continue;
        for (const auto &entry : std::filesystem::directory_iterator(root)) {
            if (!entry.is_directory())
                continue;
            try {
                auto spec = PackageManager::readManifest(entry.path());
                if (spec.name != name || !satisfies(spec.version, constraint))
                    continue;
                if (selected.empty() || compareVersions(spec.version, selectedVersion) > 0) {
                    selected = entry.path();
                    selectedVersion = spec.version;
                }
            } catch (const ProjectError &) {
                // Ignore unrelated or malformed package directories while
                // searching a repository root.
            }
        }
    }
    if (selected.empty())
        throw ProjectError("package.resolve", "Unable to resolve package: " + name);
    return selected;
}
} // namespace

PackageSpec PackageManager::readManifest(const std::filesystem::path &packageRoot) {
    try {
        auto table = toml::parse_file((packageRoot / "package.toml").string());
        const auto *package = table["package"].as_table();
        if (!package)
            throw ProjectError("package.manifest", "Missing [package] table");
        PackageSpec spec;
        spec.schemaVersion = static_cast<int>((*package)["schema_version"].value_or(1LL));
        spec.name = readString(*package, "name");
        spec.version = readString(*package, "version");
        spec.description = readString(*package, "description");
        spec.license = readString(*package, "license");
        spec.root = packageRoot;
        if (spec.name.empty() || spec.version.empty())
            throw ProjectError("package.manifest", "Package name and version are required");
        if (const auto *dependencies = table["dependencies"].as_table())
            for (const auto &[key, value] : *dependencies)
                if (auto text = value.value<std::string>())
                    spec.dependencies.emplace(std::string(key), *text);
        if (const auto *provides = table["provides"].as_table())
            for (const auto &[key, value] : *provides)
                if (auto enabled = value.value<bool>())
                    spec.capabilities.emplace(std::string(key), *enabled);
        if (const auto *trust = table["trust"].as_table())
            spec.trusted = (*trust)["trusted"].value_or(false);
        if (const auto *source = table["source"].as_table()) {
            spec.sourceType = readString(*source, "type");
            spec.sourceUrl = readString(*source, "url");
            spec.sourceRevision = readString(*source, "revision");
        }
        return spec;
    } catch (const ProjectError &) {
        throw;
    } catch (const std::exception &error) {
        throw ProjectError("package.manifest", error.what());
    }
}

void PackageManager::installGit(const std::filesystem::path &repository, const std::string &name,
                                const std::string &version, const std::string &url, const std::string &revision) {
    if (name.empty() || version.empty() || url.empty())
        throw ProjectError("package.install", "Package name, version, and URL are required");
    if (name.find_first_of("/\\") != std::string::npos || name == "." || name == "..")
        throw ProjectError("package.install", "Package name must not contain path separators");
    std::filesystem::create_directories(repository);
    const auto destination = repository / name;
    if (std::filesystem::exists(destination))
        throw ProjectError("package.install", "Destination already exists: " + destination.string());
    const auto cloneCommand = "git clone --depth 1 " +
                              (revision.empty() ? std::string{} : "--branch " + shellQuote(revision) + " ") + "-- " +
                              shellQuote(url) + " " + shellQuote(destination.string());
    if (std::system(cloneCommand.c_str()) != 0)
        throw ProjectError("package.install", "Git clone failed for: " + url);
    // Preserve upstream manifests; only generate one when the clone has none.
    if (std::filesystem::exists(destination / "package.toml"))
        return;
    std::ofstream manifest(destination / "package.toml");
    if (!manifest)
        throw ProjectError("package.install", "Unable to write generated package manifest");
    manifest << "[package]\n"
             << "schema_version = 1\n"
             << "name = \"" << name << "\"\n"
             << "version = \"" << version << "\"\n"
             << "description = \"Imported external package\"\n"
             << "license = \"See upstream repository\"\n\n"
             << "[source]\n"
             << "type = \"git\"\n"
             << "url = \"" << url << "\"\n";
    if (!revision.empty())
        manifest << "revision = \"" << revision << "\"\n";
    manifest << "\n[provides]\n"
             << "pdk = true\n"
             << "models = true\n"
             << "symbols = true\n"
             << "layout = true\n\n"
             << "[trust]\n"
             << "trusted = false\n";
}

std::string PackageManager::contentHash(const std::filesystem::path &packageRoot) {
    SHA256_CTX context;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    SHA256_Init(&context);
    std::vector<std::filesystem::path> files;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(packageRoot)) {
        if (!entry.is_regular_file())
            continue;
        const auto relative = std::filesystem::relative(entry.path(), packageRoot).generic_string();
        // Ignore VCS metadata so clones and exports hash identically.
        if (relative == ".git" || relative.starts_with(".git/"))
            continue;
        files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    std::array<unsigned char, 8192> buffer{};
    for (const auto &file : files) {
        auto relative = std::filesystem::relative(file, packageRoot).generic_string();
        SHA256_Update(&context, relative.data(), relative.size());
        std::ifstream input(file, std::ios::binary);
        while (input) {
            input.read(reinterpret_cast<char *>(buffer.data()), buffer.size());
            auto count = input.gcount();
            if (count > 0)
                SHA256_Update(&context, buffer.data(), static_cast<std::size_t>(count));
        }
    }
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256_Final(digest.data(), &context);
#pragma GCC diagnostic pop
    std::ostringstream result;
    result << "sha256:";
    for (auto byte : digest)
        result << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    return result.str();
}

Lockfile PackageManager::resolve(const std::vector<std::filesystem::path> &repositories,
                                 const std::map<std::string, std::string> &requested, std::string iceVersion) {
    Lockfile lock;
    lock.iceVersion = std::move(iceVersion);
    std::map<std::string, std::string> pending = requested;
    std::map<std::string, std::string> resolved;
    while (!pending.empty()) {
        auto [name, constraint] = *pending.begin();
        pending.erase(pending.begin());
        if (const auto existing = resolved.find(name); existing != resolved.end()) {
            if (!satisfies(existing->second, constraint)) {
                std::ostringstream message;
                message << "Conflicting constraints for package: " << name << " (" << existing->second
                        << " does not satisfy " << constraint << ")";
                throw ProjectError("package.resolve", message.str());
            }
            continue;
        }
        auto root = findPackage(repositories, name, constraint);
        auto spec = readManifest(root);
        if (!satisfies(spec.version, constraint))
            throw ProjectError("package.resolve", "Resolved version does not satisfy constraint: " + name);
        resolved[name] = spec.version;
        lock.packages.push_back(
            {spec.name, spec.version, root.generic_string(), contentHash(root), spec.trusted, spec.capabilities});
        for (const auto &dependency : spec.dependencies)
            pending.emplace(dependency.first, dependency.second);
    }
    std::sort(lock.packages.begin(), lock.packages.end(),
              [](const auto &left, const auto &right) { return left.name < right.name; });
    return lock;
}

std::string serializeLockfile(const Lockfile &lockfile) {
    const auto escape = [](const std::string &text) {
        std::string result;
        result.reserve(text.size());
        for (char c : text) {
            if (c == '"' || c == '\\')
                result.push_back('\\');
            result.push_back(c);
        }
        return result;
    };
    std::ostringstream output;
    output << "schema_version = " << lockfile.schemaVersion << "\nice_version = \"" << escape(lockfile.iceVersion)
           << "\"\n\n";
    for (const auto &package : lockfile.packages) {
        output << "[[package]]\nname = \"" << escape(package.name) << "\"\nversion = \"" << escape(package.version)
               << "\"\nsource = \"" << escape(package.source) << "\"\ncontent_hash = \"" << escape(package.contentHash)
               << "\"\ntrusted = " << (package.trusted ? "true" : "false") << "\n";
        for (const auto &[capability, enabled] : package.capabilities)
            output << "provides." << capability << " = " << (enabled ? "true" : "false") << "\n";
        output << "\n";
    }
    return output.str();
}

void PackageManager::writeLockfile(const std::filesystem::path &path, const Lockfile &lockfile) {
    std::ofstream output(path);
    if (!output)
        throw ProjectError("package.lockfile", "Unable to open lockfile for writing");
    output << serializeLockfile(lockfile);
}

Lockfile PackageManager::readLockfile(const std::filesystem::path &path) {
    try {
        auto table = toml::parse_file(path.string());
        Lockfile lock;
        lock.schemaVersion = static_cast<int>(table["schema_version"].value_or(1LL));
        lock.iceVersion = table["ice_version"].value_or(std::string{});
        if (const auto *packages = table["package"].as_array())
            for (const auto &item : *packages) {
                const auto *package = item.as_table();
                if (!package)
                    continue;
                std::map<std::string, bool> capabilities;
                if (const auto *provides = (*package)["provides"].as_table()) {
                    for (const auto &[key, value] : *provides) {
                        if (auto enabled = value.value<bool>())
                            capabilities.emplace(std::string(key), *enabled);
                    }
                }
                lock.packages.push_back({readString(*package, "name"), readString(*package, "version"),
                                         readString(*package, "source"), readString(*package, "content_hash"),
                                         (*package)["trusted"].value_or(false), std::move(capabilities)});
            }
        return lock;
    } catch (const std::exception &error) {
        throw ProjectError("package.lockfile", error.what());
    }
}

void PackageManager::verifyLockfile(const Lockfile &lockfile, const std::vector<std::filesystem::path> &repositories) {
    for (const auto &locked : lockfile.packages) {
        auto root = findPackage(repositories, locked.name, locked.version);
        if (contentHash(root) != locked.contentHash)
            throw ProjectError("package.integrity", "Content hash mismatch for package: " + locked.name);
    }
}

std::vector<std::string> PackageManager::capabilities(const PackageSpec &package) {
    std::vector<std::string> result;
    for (const auto &[name, enabled] : package.capabilities)
        if (enabled)
            result.push_back(name);
    return result;
}

std::vector<ProvenanceDiagnostic> PackageManager::provenance(const Lockfile &lockfile,
                                                             const std::vector<std::filesystem::path> &repositories) {
    std::vector<ProvenanceDiagnostic> result;
    for (const auto &package : lockfile.packages) {
        try {
            auto root = findPackage(repositories, package.name, package.version);
            if (contentHash(root) != package.contentHash)
                result.push_back(
                    {"package.integrity", "Package content hash does not match the lockfile", package.name});
            auto manifest = readManifest(root);
            if (manifest.trusted != package.trusted)
                result.push_back({"package.trust", "Package trust metadata changed since resolution", package.name});
        } catch (const ProjectError &error) {
            result.push_back({error.code(), error.what(), package.name});
        }
    }
    return result;
}
} // namespace ice
