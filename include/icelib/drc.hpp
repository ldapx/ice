#pragma once

#include "icelib/verification.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace ice {

// Options for an external gdscheck invocation. gdscheck writes a KLayout
// report database (.lyrdb) that can be converted into the normalized
// VerificationReport model.
struct GdsCheckOptions {
    std::filesystem::path input;
    std::filesystem::path report;
    std::string process;
    std::string topCell = "TOP";
    std::string suite = "main";
};

// Maps an ICE layout PDK identifier to a gdscheck process name. Returns an
// empty string for the generic PDK, which has no process deck.
std::string gdsCheckProcessForPdk(const std::string &pdk);

// Builds the argument list after the executable name, e.g.
// {"run", "--input", ..., "--process", ..., ...}.
std::vector<std::string> gdsCheckArguments(const GdsCheckOptions &options);

// Parses a gdscheck/KLayout .lyrdb report. Geometry coordinates in the report
// are in micrometres; DrcViolation::location is converted to nanometres.
VerificationReport parseGdsCheckLyrdb(const std::filesystem::path &path);
VerificationReport parseGdsCheckLyrdbText(const std::string &text);

} // namespace ice
