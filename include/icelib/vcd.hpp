#pragma once

#include "icelib/simulation.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace ice {

struct VcdSignal {
    std::string name;
    std::string type;
    int width = 1;
};

struct VcdData {
    double timescaleSeconds = 1.0e-9;
    std::vector<double> time;
    std::vector<SimulationTrace> traces;
    // Named `signalDeclarations` because Qt defines `signals` as a macro and
    // this header is included from Qt translation units.
    std::vector<VcdSignal> signalDeclarations;
    std::vector<std::string> warnings;
};

// Parses a Value Change Dump. Time values are converted to seconds using the
// VCD $timescale. Scalar x/z values and binary x/z bits are represented as 0 so
// digital waveforms remain graphable.
VcdData parseVcdText(const std::string &text, const std::string &sourceName = {});
VcdData parseVcdFile(const std::filesystem::path &path);

} // namespace ice
