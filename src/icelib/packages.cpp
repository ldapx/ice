#include "icelib/packages.hpp"

#include "icelib/core.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
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

std::string trimText(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string unquoteText(std::string value) {
    value = trimText(std::move(value));
    if (value.size() >= 2 &&
        ((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\'')))
        return value.substr(1, value.size() - 2);
    return value;
}

std::string relativeGeneric(const std::filesystem::path &path, const std::filesystem::path &root) {
    std::error_code error;
    const auto relative = std::filesystem::relative(path, root, error);
    if (error)
        return {};
    return relative.lexically_normal().generic_string();
}

struct SubmoduleEntry {
    std::string path;
    std::string url;
    std::string revision;
};

std::vector<SubmoduleEntry> readGitmodules(const std::filesystem::path &indexRoot) {
    std::ifstream input(indexRoot / ".gitmodules");
    if (!input)
        return {};
    std::vector<SubmoduleEntry> entries;
    SubmoduleEntry current;
    bool inSubmodule = false;
    const auto flush = [&] {
        if (inSubmodule && !current.path.empty())
            entries.push_back(current);
        current = {};
    };
    std::string line;
    while (std::getline(input, line)) {
        line = trimText(std::move(line));
        if (line.empty() || line.starts_with('#'))
            continue;
        if (line.front() == '[') {
            flush();
            inSubmodule = line.find("submodule") != std::string::npos;
            continue;
        }
        if (!inSubmodule)
            continue;
        const auto separator = line.find('=');
        if (separator == std::string::npos)
            continue;
        const auto key = trimText(line.substr(0, separator));
        const auto value = unquoteText(line.substr(separator + 1));
        if (key == "path")
            current.path = value;
        else if (key == "url")
            current.url = value;
        else if (key == "branch")
            current.revision = value;
    }
    flush();
    return entries;
}

const SubmoduleEntry *findSubmodule(const std::vector<SubmoduleEntry> &entries, const std::string &relativePath) {
    const SubmoduleEntry *selected = nullptr;
    for (const auto &entry : entries) {
        if (relativePath != entry.path && !relativePath.starts_with(entry.path + "/"))
            continue;
        if (!selected || entry.path.size() > selected->path.size())
            selected = &entry;
    }
    return selected;
}

std::filesystem::path temporaryInstallPath(const std::filesystem::path &repository) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto path = repository / (".ice-install-" + std::to_string(stamp));
    for (unsigned int suffix = 1; std::filesystem::exists(path); ++suffix)
        path = repository / (".ice-install-" + std::to_string(stamp) + "-" + std::to_string(suffix));
    return path;
}

void removeQuietly(const std::filesystem::path &path) {
    std::error_code error;
    std::filesystem::remove_all(path, error);
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

PackageSpec PackageManager::installGitPackage(const std::filesystem::path &repository, const std::string &url,
                                              const std::string &revision) {
    if (url.empty())
        throw ProjectError("package.install", "Package repository URL is required");
    std::filesystem::create_directories(repository);
    const auto temporary = temporaryInstallPath(repository);
    const auto cloneCommand = "GIT_TERMINAL_PROMPT=0 git clone --depth 1 " +
                              (revision.empty() ? std::string{} : "--branch " + shellQuote(revision) + " ") + "-- " +
                              shellQuote(url) + " " + shellQuote(temporary.string());
    if (std::system(cloneCommand.c_str()) != 0) {
        removeQuietly(temporary);
        throw ProjectError("package.install", "Git clone failed for: " + url);
    }

    PackageSpec spec;
    try {
        spec = readManifest(temporary);
    } catch (const std::exception &error) {
        removeQuietly(temporary);
        throw ProjectError("package.install",
                           std::string("Package repository does not contain a valid package.toml: ") + error.what());
    }
    if (spec.name.find_first_of("/\\") != std::string::npos || spec.name == "." || spec.name == "..") {
        removeQuietly(temporary);
        throw ProjectError("package.install", "Package name must not contain path separators");
    }
    if (spec.version.empty()) {
        removeQuietly(temporary);
        throw ProjectError("package.install", "Package manifest is missing a version");
    }
    const auto destination = repository / spec.name;
    if (std::filesystem::exists(destination)) {
        removeQuietly(temporary);
        throw ProjectError("package.install", "Destination already exists: " + destination.string());
    }
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        std::filesystem::copy(temporary, destination,
                              std::filesystem::copy_options::recursive | std::filesystem::copy_options::copy_symlinks,
                              error);
        if (error) {
            removeQuietly(temporary);
            removeQuietly(destination);
            throw ProjectError("package.install", "Unable to move installed package: " + error.message());
        }
        removeQuietly(temporary);
    }
    return readManifest(destination);
}

void PackageManager::uninstallPackage(const std::filesystem::path &repository, const std::string &name) {
    if (name.empty() || name.find_first_of("/\\") != std::string::npos || name == "." || name == "..")
        throw ProjectError("package.uninstall", "Invalid package name");
    const auto root = repository / name;
    std::error_code error;
    if (!std::filesystem::is_directory(root, error) || error)
        throw ProjectError("package.uninstall", "Package is not installed: " + name);
    if (std::filesystem::is_regular_file(root / "package.toml", error) && !error) {
        const auto spec = readManifest(root);
        if (spec.name != name)
            throw ProjectError("package.uninstall", "Package manifest name does not match the install directory");
    }
    std::filesystem::remove_all(root, error);
    if (error)
        throw ProjectError("package.uninstall", "Unable to remove package: " + error.message());
}

std::filesystem::path PackageManager::packageLogo(const std::filesystem::path &packageRoot) {
    const auto logo = packageRoot / ".github" / "logo.png";
    std::error_code error;
    if (std::filesystem::is_regular_file(logo, error) && !error)
        return logo;
    return {};
}

std::vector<PackageIndexEntry> PackageManager::scanPackageIndex(const std::filesystem::path &indexRoot) {
    std::vector<PackageIndexEntry> result;
    std::error_code error;
    if (!std::filesystem::is_directory(indexRoot, error) || error)
        return result;

    const auto submodules = readGitmodules(indexRoot);
    std::vector<std::filesystem::path> packageRoots;
    if (std::filesystem::is_regular_file(indexRoot / "package.toml", error) && !error)
        packageRoots.push_back(indexRoot);

    for (std::filesystem::recursive_directory_iterator
             iterator(indexRoot, std::filesystem::directory_options::skip_permission_denied, error),
         end;
         iterator != end; iterator.increment(error)) {
        if (error) {
            error.clear();
            continue;
        }
        if (!iterator->is_directory(error)) {
            error.clear();
            continue;
        }
        const auto name = iterator->path().filename().string();
        if (name.empty() || name.front() == '.' || name == "build" || name == "node_modules") {
            iterator.disable_recursion_pending();
            continue;
        }
        if (std::filesystem::is_regular_file(iterator->path() / "package.toml", error) && !error) {
            packageRoots.push_back(iterator->path());
            iterator.disable_recursion_pending();
            continue;
        }
        error.clear();
        if (iterator.depth() >= 4)
            iterator.disable_recursion_pending();
    }

    std::sort(packageRoots.begin(), packageRoots.end());
    std::map<std::string, PackageIndexEntry> selected;
    for (const auto &root : packageRoots) {
        try {
            PackageIndexEntry entry;
            entry.spec = readManifest(root);
            entry.root = root;
            entry.logo = packageLogo(root);
            const auto relative = relativeGeneric(root, indexRoot);
            if (const auto *submodule = findSubmodule(submodules, relative)) {
                entry.repositoryUrl = submodule->url;
                entry.repositoryRevision = submodule->revision;
            }
            if (const auto existing = selected.find(entry.spec.name); existing != selected.end()) {
                if (compareVersions(entry.spec.version, existing->second.spec.version) <= 0)
                    continue;
            }
            selected[entry.spec.name] = std::move(entry);
        } catch (const ProjectError &) {
            // Ignore malformed or unrelated directories in the index.
        }
    }
    for (auto &[_, entry] : selected)
        result.push_back(std::move(entry));
    return result;
}

void PackageManager::cloneIndex(const std::filesystem::path &indexRoot, const std::string &url,
                                const std::string &revision) {
    if (url.empty())
        throw ProjectError("package.index", "Package index URL is required");
    std::error_code error;
    if (std::filesystem::exists(indexRoot, error) && !error) {
        const bool empty = std::filesystem::is_empty(indexRoot, error);
        if (error || !empty)
            throw ProjectError("package.index", "Package index destination already exists: " + indexRoot.string());
    } else if (error) {
        throw ProjectError("package.index", error.message());
    }
    if (!indexRoot.parent_path().empty())
        std::filesystem::create_directories(indexRoot.parent_path());
    const auto cloneCommand = "GIT_TERMINAL_PROMPT=0 git clone --depth 1 --recurse-submodules "
                              "--shallow-submodules " +
                              (revision.empty() ? std::string{} : "--branch " + shellQuote(revision) + " ") + "-- " +
                              shellQuote(url) + " " + shellQuote(indexRoot.string());
    if (std::system(cloneCommand.c_str()) != 0) {
        removeQuietly(indexRoot);
        throw ProjectError("package.index", "Unable to clone package index: " + url);
    }
}

void PackageManager::updateIndex(const std::filesystem::path &indexRoot) {
    std::error_code error;
    if (!std::filesystem::is_directory(indexRoot, error) || error)
        throw ProjectError("package.index", "Package index not found: " + indexRoot.string());
    if (!std::filesystem::exists(indexRoot / ".git", error) || error)
        return;
    // Best-effort fast-forward. A local checkout may intentionally have no
    // upstream, local commits, or offline remotes; submodule initialization and
    // scanning still work in those cases.
    const auto pullCommand =
        "GIT_TERMINAL_PROMPT=0 git -C " + shellQuote(indexRoot.string()) + " pull --ff-only --quiet >/dev/null 2>&1";
    std::system(pullCommand.c_str());
    const auto submoduleCommand =
        "GIT_TERMINAL_PROMPT=0 git -C " + shellQuote(indexRoot.string()) + " submodule update --init --recursive";
    if (std::system(submoduleCommand.c_str()) != 0)
        throw ProjectError("package.index", "Unable to update package index submodules");
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
