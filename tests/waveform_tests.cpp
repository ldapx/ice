#include "icelib/operations.hpp"
#include "icelib/simulation_store.hpp"
#include "icelib/waveform.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <numbers>
#include <string>
#include <vector>

namespace {

ice::SimulationResult rcResult() {
    ice::SimulationResult result;
    result.analysis = "transient";
    result.xAxis = "time";
    result.xUnit = "s";
    result.time = {0.0, 1e-6, 2e-6, 3e-6};
    result.traces = {
        {"v(out)", {0.0, 1.0, 4.0, 9.0}},
        {"v(in)", {0.0, 0.5, 1.0, 1.5}},
        {"i(v1)", {-1e-3, -2e-3, -3e-3, -4e-3}},
        {"out.phase", {0.0, 30.0, 60.0, 90.0}},
    };
    return result;
}

bool close(double actual, double expected, double tolerance) { return std::abs(actual - expected) <= tolerance; }

} // namespace

TEST_CASE("waveform expressions evaluate arithmetic, functions, and derivatives") {
    const auto result = rcResult();
    ice::SimulationTrace trace;
    std::string error;

    REQUIRE(ice::evaluateDerivedTrace("difference", "out - in", result, trace, &error));
    REQUIRE(trace.values.size() == result.time.size());
    CHECK(close(trace.values[1], 0.5, 1e-12));
    CHECK(close(trace.values[3], 7.5, 1e-12));

    REQUIRE(ice::evaluateDerivedTrace("power", "i(v1) * v(out)", result, trace, &error));
    CHECK(close(trace.values[2], -0.012, 1e-12));

    REQUIRE(ice::evaluateDerivedTrace("db", "db(out)", result, trace, &error));
    CHECK(close(trace.values[1], 0.0, 1e-12));
    CHECK(close(trace.values[2], 12.041199826559248, 1e-9));

    REQUIRE(ice::evaluateDerivedTrace("phase", "phase(out)", result, trace, &error));
    CHECK(close(trace.values[2], 60.0, 1e-12));

    REQUIRE(ice::evaluateDerivedTrace("slope", "derivative(out)", result, trace, &error));
    // out[i] = i^2 in microseconds: the analytic slope is (2i) * 1e6.
    CHECK(close(trace.values[1], 2e6, 1.0));
    CHECK(close(trace.values[2], 4e6, 1.0));

    REQUIRE(ice::evaluateDerivedTrace("area", "integral(out)", result, trace, &error));
    CHECK(close(trace.values[1], 0.5e-6, 1e-12));

    REQUIRE(ice::evaluateDerivedTrace("minmax", "max(abs(out), 2)", result, trace, &error));
    CHECK(close(trace.values[0], 2.0, 1e-12));
    CHECK(close(trace.values[2], 4.0, 1e-12));

    REQUIRE(ice::evaluateDerivedTrace("scaled", "out * 1e-3 + 5e-1", result, trace, &error));
    CHECK(close(trace.values[2], 0.504, 1e-12));

    CHECK(!ice::evaluateDerivedTrace("bad", "missing + 1", result, trace, &error));
    CHECK(error.find("unknown signal") != std::string::npos);

    CHECK(!ice::evaluateDerivedTrace("bad", "out +", result, trace, &error));
    CHECK(!error.empty());

    // Division by zero produces NaN rather than infinity so plots break the
    // line instead of drawing a vertical spike.
    REQUIRE(ice::evaluateDerivedTrace("divzero", "out / (out - out)", result, trace, &error));
    CHECK(std::isnan(trace.values[1]));
}

TEST_CASE("FFT finds the tone frequency, amplitude, and phase") {
    constexpr double sampleRate = 100000.0;
    constexpr std::size_t count = 1024;
    // Choose a tone on an exact FFT bin so window leakage cannot skew the
    // amplitude or phase assertions.
    constexpr double frequency = sampleRate * 10.0 / static_cast<double>(count);
    std::vector<double> x(count);
    ice::SimulationTrace sine;
    sine.name = "v(out)";
    sine.values.resize(count);
    for (std::size_t index = 0; index < count; ++index) {
        x[index] = static_cast<double>(index) / sampleRate;
        sine.values[index] = std::sin(2.0 * std::numbers::pi * frequency * x[index] + 0.5);
    }
    ice::WaveformFftOptions options;
    options.window = "hann";
    const auto spectrum = ice::computeSpectrum(x, {sine}, options);
    REQUIRE(spectrum.frequency.size() > 10);
    REQUIRE(spectrum.magnitude.size() == 1);
    REQUIRE(spectrum.phase.size() == 1);
    std::size_t peak = 0;
    for (std::size_t index = 1; index < spectrum.magnitude.front().values.size(); ++index) {
        if (spectrum.magnitude.front().values[index] > spectrum.magnitude.front().values[peak])
            peak = index;
    }
    CHECK(close(spectrum.frequency[peak], frequency, sampleRate / static_cast<double>(count) * 1.5));
    CHECK(close(spectrum.magnitude.front().values[peak], 1.0, 0.05));
    // The FFT returns the phase of the positive-frequency component, which is
    // the sine phase shifted by -90 degrees (cosine reference).
    CHECK(close(spectrum.phase.front().values[peak], 0.5 * 180.0 / std::numbers::pi - 90.0, 8.0));

    // Non-uniform transient output is resampled and still finds the tone.
    auto jittered = x;
    for (std::size_t index = 1; index < jittered.size(); ++index)
        jittered[index] += (index % 3) * 1e-7;
    const auto resampled = ice::computeSpectrum(jittered, {sine}, options);
    REQUIRE(!resampled.magnitude.empty());
    CHECK(!resampled.warnings.empty());

    // An explicitly smaller FFT truncates instead of writing past the buffer.
    auto truncatedOptions = options;
    truncatedOptions.points = 128;
    const auto truncated = ice::computeSpectrum(x, {sine}, truncatedOptions);
    REQUIRE(truncated.frequency.size() == 65);
    REQUIRE(truncated.magnitude.size() == 1);
    CHECK(truncated.magnitude.front().values.size() == 65);
}

TEST_CASE("waveform views and series builders apply dB, phase, FFT, and derived traces") {
    auto result = rcResult();
    const std::vector<ice::SimulationDerivedTrace> derived = {{"diff", "out - in"}};
    std::vector<std::string> warnings;

    const auto linear =
        ice::buildWaveformSeries(result, {"out", "in"}, derived, ice::WaveformView::linear, false, {}, &warnings);
    REQUIRE(linear.size() == 3);
    CHECK(linear.back().name == "diff");
    CHECK(linear.back().y.size() == 4);
    CHECK(linear.front().xUnit == "s");

    const auto decibels =
        ice::buildWaveformSeries(result, {"out"}, {}, ice::WaveformView::decibel, false, {}, &warnings);
    REQUIRE(decibels.size() == 1);
    CHECK(decibels.front().yUnit == "dB");
    CHECK(close(decibels.front().y[1], 0.0, 1e-12));

    const auto phase = ice::buildWaveformSeries(result, {"out"}, {}, ice::WaveformView::phase, false, {}, &warnings);
    REQUIRE(phase.size() == 1);
    CHECK(phase.front().yUnit == "deg");
    CHECK(close(phase.front().y[2], 60.0, 1e-12));

    warnings.clear();
    const auto noPhase = ice::buildWaveformSeries(result, {"in"}, {}, ice::WaveformView::phase, false, {}, &warnings);
    CHECK(noPhase.empty());
    CHECK(!warnings.empty());

    // A 1 kHz tone in the result produces a labelled FFT series.
    auto tone = result;
    tone.time.resize(512);
    tone.traces.clear();
    ice::SimulationTrace sine;
    sine.name = "v(out)";
    sine.values.resize(512);
    for (std::size_t index = 0; index < 512; ++index) {
        tone.time[index] = static_cast<double>(index) * 1e-5;
        sine.values[index] = std::sin(2.0 * std::numbers::pi * 1000.0 * tone.time[index]);
    }
    tone.traces.push_back(sine);
    ice::WaveformFftOptions options;
    const auto spectrum =
        ice::buildWaveformSeries(tone, {"out"}, {}, ice::WaveformView::linear, true, options, &warnings);
    REQUIRE(spectrum.size() == 1);
    CHECK(spectrum.front().xUnit == "Hz");
    CHECK(spectrum.front().name == "v(out) FFT");
    CHECK(spectrum.front().x.size() == spectrum.front().y.size());
}

TEST_CASE("graph waveform settings round-trip and stored runs are discoverable") {
    ice::Project project;
    project.identity = "waveform-settings";
    project.name = "waveform-settings";
    const auto cell = ice::createTextSchematicCell(project, "rc", "generic", "* RC\nV1 in 0 DC 1\nR1 in out 1k\n");
    ice::SimulationItem simulation;
    simulation.id = project.nextId();
    simulation.name = "ac";
    simulation.schematic = cell;
    simulation.analysis = "ac";
    ice::SimulationGraph graph;
    graph.id = project.nextId();
    graph.name = "Bode";
    graph.xAxis = "time";
    graph.ySignals = {"out"};
    graph.view = "db";
    graph.fft = true;
    graph.fftWindow = "blackman";
    graph.fftPoints = 2048;
    graph.showMeasurements = false;
    graph.derivedTraces.push_back({"gain", "db(out)"});
    graph.overlays.push_back({"run-1", "ff"});
    graph.markers.push_back({"M1", "out", 1.5, 2.5, true});
    simulation.graphs.push_back(graph);
    project.simulations.emplace(simulation.id, simulation);

    // updateSimulationGraph must carry every presentation field, including the
    // waveform-analysis additions.
    ice::SimulationGraph updated = graph;
    updated.view = "phase";
    updated.fft = false;
    updated.showMeasurements = true;
    updated.markers.front().label = "M2";
    REQUIRE(ice::updateSimulationGraph(project, simulation.id, graph.id, updated));
    const auto &storedGraph = project.simulations.at(simulation.id).graphs.front();
    CHECK(storedGraph.view == "phase");
    CHECK(!storedGraph.fft);
    CHECK(storedGraph.showMeasurements);
    REQUIRE(storedGraph.markers.size() == 1);
    CHECK(storedGraph.markers.front().label == "M2");
    CHECK(storedGraph.overlays.size() == 1);

    const auto restored = ice::Project::deserialize(project.serialize());
    const auto &loaded = restored.simulations.at(simulation.id).graphs.front();
    CHECK(loaded.view == "phase");
    CHECK(!loaded.fft);
    CHECK(loaded.fftWindow == "blackman");
    CHECK(loaded.fftPoints == 2048);
    CHECK(loaded.showMeasurements);
    REQUIRE(loaded.derivedTraces.size() == 1);
    CHECK(loaded.derivedTraces.front().expression == "db(out)");
    REQUIRE(loaded.overlays.size() == 1);
    CHECK(loaded.overlays.front().runId == "run-1");
    REQUIRE(loaded.markers.size() == 1);
    CHECK(loaded.markers.front().hasY);

    const auto root = std::filesystem::temp_directory_path() / "ice-waveform-store-test";
    std::filesystem::remove_all(root);
    ice::SimulationRunContext context;
    context.projectRoot = root;
    context.projectIdentity = project.identity;
    context.projectName = project.name;
    context.simulationId = simulation.id;
    context.simulationName = simulation.name;
    context.variant = "ff";
    context.schematicId = cell;
    context.schematicName = "rc";
    ice::SimulationResult storedResult;
    storedResult.state = ice::JobState::succeeded;
    storedResult.analysis = "ac";
    storedResult.time = {1.0, 10.0};
    storedResult.traces = {{"v(out)", {1.0, 0.5}}};
    const auto artifact = ice::writeSimulationRun(context, storedResult);
    const auto found = ice::findSimulationRun(root, artifact.runId);
    REQUIRE(found.has_value());
    CHECK(found->variant == "ff");
    const auto summaries = ice::listSimulationRunSummaries(root, simulation.id);
    REQUIRE(summaries.size() == 1);
    CHECK(summaries.front().runId == artifact.runId);
    CHECK(summaries.front().simulationName == simulation.name);
    std::filesystem::remove_all(root);
}
