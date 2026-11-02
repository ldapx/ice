#include "icelib/measure.hpp"

#include <algorithm>
#include <cmath>

namespace ice {
namespace {
constexpr double epsilon = 1e-30;
}

TraceStatistics traceStatistics(const SimulationTrace &trace) {
    TraceStatistics result;
    result.points = trace.values.size();
    if (trace.values.empty())
        return result;
    result.min = trace.values.front();
    result.max = trace.values.front();
    result.first = trace.values.front();
    result.last = trace.values.back();
    double sum = 0.0;
    double sumSquares = 0.0;
    for (const double value : trace.values) {
        result.min = std::min(result.min, value);
        result.max = std::max(result.max, value);
        sum += value;
        sumSquares += value * value;
    }
    result.mean = sum / static_cast<double>(trace.values.size());
    result.rms = std::sqrt(sumSquares / static_cast<double>(trace.values.size()));
    return result;
}

SignalMeasurement measureSignal(const SimulationTrace &trace, const std::vector<double> &time, double threshold,
                                double startTime, double settleTolerance) {
    SignalMeasurement result;
    result.name = trace.name;
    result.statistics = traceStatistics(trace);
    const auto count = std::min(trace.values.size(), time.size());
    if (count == 0)
        return result;

    if (std::isnan(threshold))
        threshold = (result.statistics.min + result.statistics.max) * 0.5;
    result.threshold = threshold;

    std::vector<double> risingTimes;
    for (std::size_t index = 1; index < count; ++index) {
        if (time[index] < startTime)
            continue;
        const auto previous = trace.values[index - 1] - threshold;
        const auto current = trace.values[index] - threshold;
        if ((previous < 0.0 && current >= 0.0) || (previous > 0.0 && current <= 0.0))
            ++result.zeroCrossings;
        if (previous < 0.0 && current >= 0.0)
            risingTimes.push_back(time[index]);
    }
    result.risingCrossings = risingTimes.size();
    if (risingTimes.size() >= 2) {
        const auto period = (risingTimes.back() - risingTimes.front()) / static_cast<double>(risingTimes.size() - 1);
        if (period > epsilon) {
            result.period = period;
            result.frequency = 1.0 / period;
        }
    }

    const auto amplitude = result.statistics.amplitude();
    const auto tolerance = std::max(std::abs(amplitude) * settleTolerance, epsilon);
    const auto finalValue = result.statistics.last;
    bool found = false;
    std::size_t lastOutside = 0;
    for (std::size_t index = 0; index < count; ++index) {
        if (std::abs(trace.values[index] - finalValue) > tolerance) {
            lastOutside = index;
            found = true;
        }
    }
    if (found)
        result.settlingTime = time[lastOutside] - time.front();
    else
        result.settlingTime = 0.0;
    return result;
}

} // namespace ice
