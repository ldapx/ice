#pragma once

#include "icelib/process.hpp"
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

struct LayoutDrcOptions {
    // Directory used for layout.gds, drc.lyrdb, and drc.log. Created when
    // missing. Prefer a project build directory so the report is inspectable.
    std::filesystem::path workDirectory;
    // Flatten instances before exporting. Normal DRC usually needs this for
    // hierarchical layouts; the default matches the GUI action.
    bool flatten = true;
    // Empty uses TOP, the cell name written by ICE's GDS exporter.
    std::string topCell;
    std::string suite = "main";
    int timeoutSeconds = 0;
};

struct LayoutDrcResult {
    bool ok = false;
    ObjectId layout = 0;
    std::string layoutName;
    std::filesystem::path gdsPath;
    std::filesystem::path reportPath;
    std::filesystem::path logPath;
    ProcessResult process;
    VerificationReport report;
    std::string error;
};

// Exports a layout, runs gdscheck with the process deck derived from its PDK,
// and parses the resulting .lyrdb report. `ok` means the report was produced
// and parsed successfully; violations are reported separately and do not by
// themselves turn a completed DRC run into a process failure.
LayoutDrcResult runLayoutDrc(const Project &project, ObjectId layoutCellId, const LayoutDrcOptions &options = {},
                             const ProcessCancellation *cancellation = nullptr,
                             const ProcessOutputCallback &onOutput = {});

// Parses a gdscheck/KLayout .lyrdb report. Geometry coordinates in the report
// are in micrometres; DrcViolation::location is converted to nanometres.
VerificationReport parseGdsCheckLyrdb(const std::filesystem::path &path);
VerificationReport parseGdsCheckLyrdbText(const std::string &text);

} // namespace ice
