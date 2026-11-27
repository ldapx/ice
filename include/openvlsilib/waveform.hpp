#pragma once

#include "openvlsilib/simulation.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace openvlsi {

// Presentation transform for a simulation graph. It only changes how traces
// are drawn; stored simulation samples are never modified.
enum class WaveformView { linear, decibel, phase };

const char *waveformViewName(WaveformView view);
std::optional<WaveformView> parseWaveformView(const std::string &text);

// Returns the display values for one trace under `view`. Linear returns the
// stored samples. Decibel converts magnitudes to 20*log10(|v|), clamped at
// -300 dB. Phase looks up the companion "<signal>.phase" trace and returns
// degrees; an empty result means the trace has no phase data.
std::vector<double> waveformViewValues(const SimulationTrace &trace, WaveformView view, const SimulationResult &result);

struct WaveformFftOptions {
    // none, hann, hamming, or blackman.
    std::string window = "hann";
    // Zero-pad or truncate to this length. Zero selects the next power of two
    // for the sample count, which keeps the FFT fast and the bins regular.
    std::size_t points = 0;
    bool oneSided = true;
};

struct WaveformSpectrum {
    std::vector<double> frequency;
    // Magnitude traces use the source name with a " FFT" suffix. Values are
    // amplitudes for `oneSided` spectra and phase is in degrees.
    std::vector<SimulationTrace> magnitude;
    std::vector<SimulationTrace> phase;
    std::vector<std::string> warnings;
};

// Computes the amplitude/phase spectrum of each trace over `x`. Non-uniform
// sample spacing is linearly resampled onto a uniform grid before the FFT so
// adaptive transient output can be analyzed directly.
WaveformSpectrum computeSpectrum(const std::vector<double> &x, const std::vector<SimulationTrace> &traces,
                                 const WaveformFftOptions &options = {});

// Evaluates one waveform expression. Supported syntax:
//   numbers, trace names (with or without v(...)/i(...)), + - * /, parentheses,
//   and the functions abs, sqrt, exp, ln, log10, db, mag, phase, derivative,
//   integral, min, and max. Trace names are matched case-insensitively and
//   ignore whitespace, so `v(Out)` and `out` resolve to the same vector.
// `error` receives an actionable message when evaluation fails.
bool evaluateDerivedTrace(const std::string &name, const std::string &expression, const SimulationResult &result,
                          SimulationTrace &trace, std::string *error = nullptr);

std::vector<SimulationTrace> evaluateDerivedTraces(const std::vector<SimulationDerivedTrace> &specs,
                                                   const SimulationResult &result,
                                                   std::vector<std::string> *errors = nullptr);

// Applies a graph-level transform to the selected traces. When `fft` is true
// the returned axes are frequencies and the traces are spectra; otherwise the
// input axis and traces are returned after evaluating derived expressions and
// applying `view`.
struct WaveformSeries {
    std::string name;
    std::string label;
    std::vector<double> x;
    std::vector<double> y;
    std::string xUnit;
    std::string yUnit;
};

std::vector<WaveformSeries> buildWaveformSeries(const SimulationResult &result,
                                                const std::vector<std::string> &signalNames,
                                                const std::vector<SimulationDerivedTrace> &derived, WaveformView view,
                                                bool fft, const WaveformFftOptions &options = {},
                                                std::vector<std::string> *warnings = nullptr);

} // namespace openvlsi
