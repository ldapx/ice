#include "icelib/measure.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <utility>

namespace ice {
namespace {
constexpr double epsilon = 1e-30;

std::string lower(std::string text) {
    for (auto &character : text)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return text;
}

const SimulationTrace *findTrace(const SimulationResult &result, const std::string &name) {
    if (name.empty())
        return nullptr;
    const auto wanted = lower(name);
    for (const auto &trace : result.traces) {
        if (lower(trace.name) == wanted)
            return &trace;
    }
    return nullptr;
}

const SimulationTrace *findPhaseTrace(const SimulationResult &result, const std::string &name) {
    return findTrace(result, name + ".phase");
}

double interpolateAt(const std::vector<double> &x, const std::vector<double> &y, double target) {
    if (x.size() < 2 || y.size() < 2)
        return std::numeric_limits<double>::quiet_NaN();
    const auto count = std::min(x.size(), y.size());
    for (std::size_t index = 1; index < count; ++index) {
        const auto first = y[index - 1];
        const auto second = y[index];
        if ((first <= target && second >= target) || (first >= target && second <= target)) {
            const auto denominator = second - first;
            if (std::abs(denominator) <= epsilon)
                return x[index];
            const auto fraction = (target - first) / denominator;
            return x[index - 1] + (x[index] - x[index - 1]) * fraction;
        }
    }
    return std::numeric_limits<double>::quiet_NaN();
}

double interpolateValueAt(const std::vector<double> &x, const std::vector<double> &y, double target) {
    if (x.size() < 2 || y.size() < 2)
        return std::numeric_limits<double>::quiet_NaN();
    const auto count = std::min(x.size(), y.size());
    for (std::size_t index = 1; index < count; ++index) {
        const auto firstX = x[index - 1];
        const auto secondX = x[index];
        if ((firstX <= target && secondX >= target) || (firstX >= target && secondX <= target)) {
            const auto denominator = secondX - firstX;
            if (std::abs(denominator) <= epsilon)
                return y[index];
            const auto fraction = (target - firstX) / denominator;
            return y[index - 1] + (y[index] - y[index - 1]) * fraction;
        }
    }
    return std::numeric_limits<double>::quiet_NaN();
}

struct MeasurementContext {
    const SimulationResult &result;
    const std::vector<double> &x() const { return result.time; }
    bool ac() const { return result.analysis == "ac"; }
};

SimulationMeasurementResult failure(const SimulationMeasurementSpec &spec, std::string error) {
    SimulationMeasurementResult result;
    result.name = spec.name;
    result.metric = spec.metric;
    result.valid = false;
    result.error = std::move(error);
    return result;
}

SimulationMeasurementResult measurementResult(const SimulationMeasurementSpec &spec, double value, std::string unit) {
    SimulationMeasurementResult result;
    result.name = spec.name;
    result.metric = spec.metric;
    result.value = value;
    result.unit = std::move(unit);
    result.valid = std::isfinite(value);
    if (!result.valid)
        result.error = "measurement did not resolve to a finite value";
    return result;
}

SimulationMeasurementResult measureGain(const SimulationMeasurementSpec &spec, const MeasurementContext &context) {
    const auto *output = findTrace(context.result, spec.signal);
    if (!output || output->values.empty())
        return failure(spec, "gain requires a captured output signal: " + spec.signal);
    const auto *input = findTrace(context.result, spec.input);
    if (context.ac()) {
        const auto inputMagnitude = input && !input->values.empty() ? std::abs(input->values.front()) : 1.0;
        if (inputMagnitude <= epsilon)
            return failure(spec, "gain input signal is zero: " + spec.input);
        const auto ratio = std::abs(output->values.front()) / inputMagnitude;
        return measurementResult(spec, 20.0 * std::log10(std::max(ratio, epsilon)), "dB");
    }
    const auto outputAmplitude = traceStatistics(*output).amplitude();
    if (!input)
        return measurementResult(spec, outputAmplitude, "V");
    const auto inputAmplitude = traceStatistics(*input).amplitude();
    if (inputAmplitude <= epsilon)
        return failure(spec, "gain input amplitude is zero: " + spec.input);
    return measurementResult(spec, outputAmplitude / inputAmplitude, "V/V");
}

SimulationMeasurementResult measureBandwidth(const SimulationMeasurementSpec &spec, const MeasurementContext &context) {
    if (!context.ac())
        return failure(spec, "bandwidth requires an AC analysis");
    const auto *output = findTrace(context.result, spec.signal);
    if (!output || output->values.size() < 2)
        return failure(spec, "bandwidth requires a captured AC output signal: " + spec.signal);
    const auto *input = findTrace(context.result, spec.input);
    const auto referenceCount = std::min<std::size_t>(5, output->values.size());
    double reference = 0.0;
    for (std::size_t index = 0; index < referenceCount; ++index) {
        const auto inputMagnitude = input && index < input->values.size() ? std::abs(input->values[index]) : 1.0;
        if (inputMagnitude > epsilon)
            reference += std::abs(output->values[index]) / inputMagnitude;
    }
    reference /= static_cast<double>(referenceCount);
    if (reference <= epsilon)
        return failure(spec, "bandwidth could not determine a low-frequency gain");
    const auto target = reference * std::sqrt(0.5);
    std::vector<double> magnitudes;
    magnitudes.reserve(output->values.size());
    for (std::size_t index = 0; index < output->values.size(); ++index) {
        const auto inputMagnitude = input && index < input->values.size() ? std::abs(input->values[index]) : 1.0;
        magnitudes.push_back(std::abs(output->values[index]) / std::max(inputMagnitude, epsilon));
    }
    const auto frequency = interpolateAt(context.x(), magnitudes, target);
    if (!std::isfinite(frequency))
        return failure(spec, "bandwidth did not cross the -3 dB point in the AC sweep");
    return measurementResult(spec, frequency, "Hz");
}

SimulationMeasurementResult measurePhaseMargin(const SimulationMeasurementSpec &spec,
                                               const MeasurementContext &context) {
    if (!context.ac())
        return failure(spec, "phase margin requires an AC analysis");
    const auto *output = findTrace(context.result, spec.signal);
    const auto *input = findTrace(context.result, spec.input);
    const auto *outputPhase = findPhaseTrace(context.result, spec.signal);
    const auto *inputPhase = findPhaseTrace(context.result, spec.input);
    if (!output || !outputPhase)
        return failure(spec, "phase margin requires magnitude and phase traces for: " + spec.signal);
    std::vector<double> loopGain;
    std::vector<double> phase;
    loopGain.reserve(output->values.size());
    phase.reserve(output->values.size());
    for (std::size_t index = 0; index < output->values.size(); ++index) {
        const auto inputMagnitude = input && index < input->values.size() ? std::abs(input->values[index]) : 1.0;
        loopGain.push_back(std::abs(output->values[index]) / std::max(inputMagnitude, epsilon));
        const auto outputDegrees = index < outputPhase->values.size() ? outputPhase->values[index] : 0.0;
        const auto inputDegrees = inputPhase && index < inputPhase->values.size() ? inputPhase->values[index] : 0.0;
        phase.push_back(outputDegrees - inputDegrees);
    }
    const auto unityFrequency = interpolateAt(context.x(), loopGain, 1.0);
    if (!std::isfinite(unityFrequency))
        return failure(spec, "phase margin did not find a unity-gain crossing");
    const auto phaseAtUnity = interpolateValueAt(context.x(), phase, unityFrequency);
    if (!std::isfinite(phaseAtUnity))
        return failure(spec, "phase margin could not interpolate the phase at unity gain");
    return measurementResult(spec, 180.0 + phaseAtUnity, "deg");
}

SimulationMeasurementResult measureSlewRate(const SimulationMeasurementSpec &spec, const MeasurementContext &context) {
    const auto *trace = findTrace(context.result, spec.signal);
    if (!trace || trace->values.size() < 2 || context.x().size() < 2)
        return failure(spec, "slew rate requires a transient signal: " + spec.signal);
    double maximum = 0.0;
    const auto count = std::min(trace->values.size(), context.x().size());
    for (std::size_t index = 1; index < count; ++index) {
        const auto dt = context.x()[index] - context.x()[index - 1];
        if (dt <= epsilon)
            continue;
        maximum = std::max(maximum, std::abs(trace->values[index] - trace->values[index - 1]) / dt);
    }
    return measurementResult(spec, maximum, "V/s");
}

SimulationMeasurementResult measureSettling(const SimulationMeasurementSpec &spec, const MeasurementContext &context) {
    const auto *trace = findTrace(context.result, spec.signal);
    if (!trace || trace->values.empty())
        return failure(spec, "settling time requires a captured signal: " + spec.signal);
    const auto threshold = spec.threshold.value_or(std::numeric_limits<double>::quiet_NaN());
    const auto result = measureSignal(*trace, context.x(), threshold, 0.0, spec.tolerance);
    if (!result.settlingTime)
        return failure(spec, "settling time could not be determined");
    return measurementResult(spec, *result.settlingTime, "s");
}

SimulationMeasurementResult measurePower(const SimulationMeasurementSpec &spec, const MeasurementContext &context) {
    const auto *trace = findTrace(context.result, spec.signal);
    if (!trace || trace->values.empty())
        return failure(spec, "power requires a captured current or power signal: " + spec.signal);
    const auto statistics = traceStatistics(*trace);
    auto power = statistics.mean;
    const auto normalized = lower(spec.signal);
    if (normalized.starts_with("i(") || normalized.find("branch") != std::string::npos) {
        power = statistics.mean * (spec.voltage == 0.0 ? 1.0 : std::abs(spec.voltage));
        // Supply currents are normally negative; report consumed power.
        power = std::abs(power);
    }
    return measurementResult(spec, power, "W");
}
} // namespace

std::vector<SimulationMeasurementResult> evaluateMeasurements(const std::vector<SimulationMeasurementSpec> &specs,
                                                              const SimulationResult &result) {
    std::vector<SimulationMeasurementResult> measurements;
    measurements.reserve(specs.size());
    const MeasurementContext context{result};
    for (const auto &spec : specs) {
        const auto metric = lower(spec.metric);
        if (metric == "gain" || metric == "gain_db")
            measurements.push_back(measureGain(spec, context));
        else if (metric == "bandwidth" || metric == "bw" || metric == "unity_gain_frequency")
            measurements.push_back(measureBandwidth(spec, context));
        else if (metric == "phase_margin" || metric == "phase-margin" || metric == "pm")
            measurements.push_back(measurePhaseMargin(spec, context));
        else if (metric == "slew_rate" || metric == "slew" || metric == "slew-rate")
            measurements.push_back(measureSlewRate(spec, context));
        else if (metric == "settling_time" || metric == "settling" || metric == "settling-time")
            measurements.push_back(measureSettling(spec, context));
        else if (metric == "power" || metric == "average_power")
            measurements.push_back(measurePower(spec, context));
        else
            measurements.push_back(failure(spec, "unsupported measurement metric: " + spec.metric));
    }
    return measurements;
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
