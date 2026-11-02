#pragma once

#include "icelib/simulation.hpp"

#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace ice {

struct TraceStatistics {
    std::size_t points = 0;
    double min = 0.0;
    double max = 0.0;
    double mean = 0.0;
    double rms = 0.0;
    double first = 0.0;
    double last = 0.0;
    double amplitude() const { return max - min; }
};

TraceStatistics traceStatistics(const SimulationTrace &trace);

struct SignalMeasurement {
    std::string name;
    TraceStatistics statistics;
    double threshold = 0.0;
    std::size_t zeroCrossings = 0;
    std::size_t risingCrossings = 0;
    std::optional<double> period;
    std::optional<double> frequency;
    // Time from the first sample until the signal last leaves a band around
    // its final value. For a sustained oscillator this is the whole run; callers
    // should pair it with the crossing and frequency fields.
    std::optional<double> settlingTime;
};

// Measures a signal over `time`. A NaN threshold selects the midpoint of the
// observed min/max. `startTime` excludes startup crossings from the frequency
// estimate. `settleTolerance` is relative to the observed amplitude.
SignalMeasurement measureSignal(const SimulationTrace &trace, const std::vector<double> &time,
                                double threshold = std::numeric_limits<double>::quiet_NaN(), double startTime = 0.0,
                                double settleTolerance = 0.02);

} // namespace ice
