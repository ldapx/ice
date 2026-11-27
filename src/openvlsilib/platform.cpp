#include "openvlsilib/platform.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string_view>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace openvlsi {
namespace {

bool isRegularFile(const std::filesystem::path &path) {
    std::error_code status;
    return std::filesystem::is_regular_file(path, status) && !status;
}

bool hasPathSeparator(const std::string &name) {
#ifdef _WIN32
    return name.find('/') != std::string::npos || name.find('\\') != std::string::npos ||
           (name.size() > 1 && name[1] == ':');
#else
    return name.find('/') != std::string::npos;
#endif
}

#ifdef _WIN32
std::string lowerAscii(std::string text) {
    for (auto &character : text)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return text;
}

std::vector<std::string> executableExtensions() {
    std::vector<std::string> extensions;
    if (const char *pathext = std::getenv("PATHEXT"); pathext && *pathext) {
        std::string value = pathext;
        std::size_t start = 0;
        while (start <= value.size()) {
            const auto end = value.find(';', start);
            auto entry = value.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (!entry.empty()) {
                if (entry.front() != '.')
                    entry.insert(entry.begin(), '.');
                const auto lowered = lowerAscii(std::move(entry));
                if (std::find(extensions.begin(), extensions.end(), lowered) == extensions.end())
                    extensions.push_back(lowered);
            }
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
    }
    for (const auto *extension : {".exe", ".com", ".bat", ".cmd"}) {
        if (std::find(extensions.begin(), extensions.end(), extension) == extensions.end())
            extensions.push_back(extension);
    }
    return extensions;
}
#endif

std::optional<std::filesystem::path> executableCandidate(const std::filesystem::path &directory,
                                                         const std::string &name) {
    const auto direct = directory / name;
    if (isRegularFile(direct))
        return direct;
#ifdef _WIN32
    if (direct.has_extension())
        return {};
    for (const auto &extension : executableExtensions()) {
        const auto candidate = directory / (name + extension);
        if (isRegularFile(candidate))
            return candidate;
    }
#endif
    return {};
}

} // namespace

std::filesystem::path currentExecutablePath() {
#ifdef _WIN32
    std::vector<wchar_t> buffer(32768);
    const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length > 0 && length < buffer.size())
        return std::filesystem::path(std::wstring(buffer.data(), length));
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    if (size > 0) {
        std::vector<char> buffer(size);
        if (_NSGetExecutablePath(buffer.data(), &size) == 0) {
            std::error_code status;
            auto path = std::filesystem::weakly_canonical(buffer.data(), status);
            return status ? std::filesystem::path(buffer.data()) : path;
        }
    }
#else
    std::error_code status;
    auto path = std::filesystem::read_symlink("/proc/self/exe", status);
    if (!status)
        return path;
#endif
    return {};
}

std::vector<std::filesystem::path> applicationResourceDirectories() {
    std::vector<std::filesystem::path> directories;
    const auto append = [&](const std::filesystem::path &candidate) {
        if (candidate.empty())
            return;
        std::error_code status;
        if (!std::filesystem::is_directory(candidate, status) || status)
            return;
        const auto normalized = candidate.lexically_normal();
        if (std::find(directories.begin(), directories.end(), normalized) == directories.end())
            directories.push_back(normalized);
    };

    const auto executable = currentExecutablePath();
    if (!executable.empty()) {
        const auto directory = executable.parent_path();
        append(directory);
        append(directory / ".." / "share" / "openvlsi");
        append(directory / ".." / "Resources");
        append(directory / ".." / ".." / "share" / "openvlsi");
        append(directory / ".." / ".." / ".." / "share" / "openvlsi");
    }

#ifdef OPENVLSI_SOURCE_DIR
    append(std::filesystem::path(OPENVLSI_SOURCE_DIR));
#endif

    std::error_code status;
    const auto current = std::filesystem::current_path(status);
    if (!status)
        append(current);

#ifndef _WIN32
    append("/usr/share/openvlsi");
    append("/usr/local/share/openvlsi");
#endif
    return directories;
}

std::filesystem::path homeDirectory() {
#ifdef _WIN32
    if (const char *profile = std::getenv("USERPROFILE"); profile && *profile)
        return std::filesystem::path(profile);
    if (const char *drive = std::getenv("HOMEDRIVE"); drive && *drive) {
        if (const char *path = std::getenv("HOMEPATH"); path && *path)
            return std::filesystem::path(std::string(drive) + path);
    }
#else
    if (const char *home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home);
#endif
    return {};
}

char pathListSeparator() {
#ifdef _WIN32
    return ';';
#else
    return ':';
#endif
}

std::vector<std::string> splitPathList(const std::string &value, bool keepEmpty) {
    std::vector<std::string> entries;
    const auto separator = pathListSeparator();
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto end = value.find(separator, start);
        auto entry = value.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (keepEmpty || !entry.empty())
            entries.push_back(std::move(entry));
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return entries;
}

std::optional<std::filesystem::path> findExecutableOnPath(const std::string &name) {
    if (name.empty())
        return std::nullopt;

    if (hasPathSeparator(name)) {
        const auto direct = std::filesystem::path(name);
        if (isRegularFile(direct))
            return direct;
        return executableCandidate(direct.parent_path(), direct.filename().string());
    }

    const char *path = std::getenv("PATH");
    if (!path)
        return executableCandidate(std::filesystem::current_path(), name);
    for (const auto &entry : splitPathList(path, true)) {
        const auto directory = entry.empty() ? std::filesystem::path(".") : std::filesystem::path(entry);
        if (const auto candidate = executableCandidate(directory, name))
            return candidate;
    }
    return std::nullopt;
}

std::string defaultPythonExecutable() {
    if (const char *python = std::getenv("OPENVLSI_PYTHON"); python && *python)
        return python;
#ifdef _WIN32
    return "python";
#else
    return "python3";
#endif
}

} // namespace openvlsi
