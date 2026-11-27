#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace openvlsi {

// Absolute path to the running executable, or an empty path when the
// platform does not expose it.
std::filesystem::path currentExecutablePath();

// Candidate roots that contain OpenVLSI's installed/share resources (`symbols/`,
// `assets/`, `python/`). Includes source-tree and install-tree locations.
std::vector<std::filesystem::path> applicationResourceDirectories();

// The current user's home directory. Windows uses USERPROFILE; POSIX uses
// HOME.
std::filesystem::path homeDirectory();

// `;` on Windows and `:` on POSIX.
char pathListSeparator();

// Splits a PATH/PYTHONPATH-style list, preserving empty entries only when the
// caller asks for them.
std::vector<std::string> splitPathList(const std::string &value, bool keepEmpty = false);

// Finds an executable on PATH. On Windows this also probes PATHEXT extensions
// such as .exe, .cmd, and .bat.
std::optional<std::filesystem::path> findExecutableOnPath(const std::string &name);

// OPENVLSI_PYTHON when set, otherwise the conventional interpreter name for the
// platform.
std::string defaultPythonExecutable();

} // namespace openvlsi
