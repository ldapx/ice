#include "icelib/measure.hpp"
#include "icelib/operations.hpp"
#include "icelib/pdk.hpp"
#include "icelib/simulation.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

ice::SimulationRequest rcRequest(const std::string &analysis) {
    ice::SimulationRequest request;
    request.analysis = analysis;
    request.simulator = "ngspice";
    request.netlist = {"* RC analysis", "V1 in 0 DC 1 AC 1", "R1 in out 1k", "C1 out 0 1u"};
    return request;
}

bool hasTrace(const ice::SimulationResult &result, const std::string &name) {
    return std::any_of(result.traces.begin(), result.traces.end(),
                       [&](const ice::SimulationTrace &trace) { return trace.name == name; });
}

const ice::SimulationTrace *traceNamed(const ice::SimulationResult &result, const std::string &name) {
    for (const auto &trace : result.traces) {
        if (trace.name == name)
            return &trace;
    }
    return nullptr;
}

void checkClose(double actual, double expected, double tolerance) { CHECK(std::abs(actual - expected) <= tolerance); }

} // namespace

TEST_CASE("ngspice adapter supports DC sweep analysis") {
    auto request = rcRequest("dc");
    request.dcSource = "V1";
    request.dcStart = 0.0;
    request.dcStop = 1.0;
    request.dcStep = 0.25;
    request.probeSignals = {"out"};
    const auto result = ice::SimulationRunner().run(request, std::chrono::seconds(5));
    CHECK(result.state == ice::JobState::succeeded);
    CHECK(result.xAxis == "v1 sweep");
    CHECK(result.time.size() == 5);
    CHECK(hasTrace(result, "out"));
}

TEST_CASE("ngspice adapter supports AC sweep analysis") {
    auto request = rcRequest("ac");
    request.acVariation = "dec";
    request.acPoints = 5;
    request.acStart = 1.0;
    request.acStop = 1e6;
    request.probeSignals = {"out", "in"};
    const auto result = ice::SimulationRunner().run(request, std::chrono::seconds(5));
    CHECK(result.state == ice::JobState::succeeded);
    CHECK(result.xAxis == "frequency");
    CHECK(result.time.size() == 31);
    CHECK(hasTrace(result, "out"));
    CHECK(hasTrace(result, "out.phase"));
    CHECK(hasTrace(result, "in"));
    if (const auto *out = traceNamed(result, "out"))
        checkClose(out->values.front(), 1.0, 0.01);
}

TEST_CASE("ngspice adapter reloads a new circuit for repeated AC analyses") {
    auto first = rcRequest("ac");
    first.netlist = {"* first divider", "V1 in 0 DC 1 AC 1", "R1 in out 1k", "R2 out 0 1k"};
    first.acVariation = "dec";
    first.acPoints = 5;
    first.acStart = 1.0;
    first.acStop = 1e6;
    first.probeSignals = {"out"};
    const auto firstResult = ice::SimulationRunner().run(first, std::chrono::seconds(5));
    REQUIRE(firstResult.state == ice::JobState::succeeded);
    const auto *firstOut = traceNamed(firstResult, "out");
    REQUIRE(firstOut != nullptr);
    checkClose(firstOut->values.front(), 0.5, 0.02);

    auto second = first;
    second.netlist = {"* second divider", "V1 in 0 DC 1 AC 1", "R1 in out 1k", "R2 out 0 2k"};
    const auto secondResult = ice::SimulationRunner().run(second, std::chrono::seconds(5));
    REQUIRE(secondResult.state == ice::JobState::succeeded);
    const auto *secondOut = traceNamed(secondResult, "out");
    REQUIRE(secondOut != nullptr);
    checkClose(secondOut->values.front(), 2.0 / 3.0, 0.02);
}

TEST_CASE("ngspice adapter does not reuse a circuit after a failed load") {
    auto first = rcRequest("ac");
    first.netlist = {"* valid divider", "V1 in 0 DC 1 AC 1", "R1 in out 1k", "R2 out 0 1k"};
    first.acVariation = "dec";
    first.acPoints = 5;
    first.acStart = 1.0;
    first.acStop = 1e6;
    first.probeSignals = {"out"};
    const auto firstResult = ice::SimulationRunner().run(first, std::chrono::seconds(5));
    REQUIRE(firstResult.state == ice::JobState::succeeded);

    auto broken = first;
    broken.netlist = {"* invalid circuit", "V1 in 0 DC 1 AC 1", "R1 in out 1k",
                      "X1 out 0 missing_subcircuit"};
    const auto brokenResult = ice::SimulationRunner().run(broken, std::chrono::seconds(5));
    CHECK(brokenResult.state == ice::JobState::failed);
}

TEST_CASE("ngspice adapter reloads OSDI circuits between runs") {
    if (ice::findPdks("ihp-sg13g2").empty())
        return;
    ice::PdkSimulationSetup setup;
    try {
        setup = ice::pdkSimulationSetupFor("ihp-sg13g2");
    } catch (const std::exception &) {
        return;
    }

    auto divider = rcRequest("ac");
    divider.netlist = setup.netlistLines;
    divider.netlist.insert(divider.netlist.end(),
                           {"* divider", "V1 in 0 DC 1 AC 1", "R1 in out 1k", "R2 out 0 1k"});
    divider.initCommands = setup.initCommands;
    divider.acVariation = "dec";
    divider.acPoints = 5;
    divider.acStart = 1.0;
    divider.acStop = 1e6;
    divider.probeSignals = {"out"};
    const auto dividerResult = ice::SimulationRunner().run(divider, std::chrono::seconds(20));
    REQUIRE(dividerResult.state == ice::JobState::succeeded);
    const auto *dividerOut = traceNamed(dividerResult, "out");
    REQUIRE(dividerOut != nullptr);
    checkClose(dividerOut->values.front(), 0.5, 0.02);

    auto mosfet = divider;
    mosfet.netlist = setup.netlistLines;
    mosfet.netlist.insert(
        mosfet.netlist.end(),
        {"* follower", "vdd vdd 0 1.2", "vin in 0 DC 0.7 AC 1",
         "x1 vdd in out out sg13_lv_nmos w=40u l=0.5u", "rs out 0 2k"});
    const auto mosfetResult = ice::SimulationRunner().run(mosfet, std::chrono::seconds(20));
    REQUIRE(mosfetResult.state == ice::JobState::succeeded);
    const auto *mosfetOut = traceNamed(mosfetResult, "out");
    REQUIRE(mosfetOut != nullptr);
    CHECK(std::abs(mosfetOut->values.front() - 0.5) > 0.05);
}

TEST_CASE("ngspice adapter supports operating point analysis") {
    auto request = rcRequest("op");
    request.probeSignals = {"out"};
    const auto result = ice::SimulationRunner().run(request, std::chrono::seconds(5));
    CHECK(result.state == ice::JobState::succeeded);
    CHECK(result.xAxis == "operating point");
    CHECK(result.time.size() == 1);
    CHECK(result.scalars.contains("out"));
    if (const auto value = result.scalars.find("out"); value != result.scalars.end())
        checkClose(value->second, 1.0, 0.01);
}

TEST_CASE("ngspice adapter supports transfer-function analysis") {
    auto request = rcRequest("tf");
    request.tfOutput = "out";
    request.tfSource = "V1";
    const auto result = ice::SimulationRunner().run(request, std::chrono::seconds(5));
    CHECK(result.state == ice::JobState::succeeded);
    CHECK(result.xAxis == "transfer function");
    CHECK(result.scalars.contains("transfer_function"));
    CHECK(result.scalars.contains("output_impedance"));
    if (const auto value = result.scalars.find("transfer_function"); value != result.scalars.end())
        checkClose(value->second, 1.0, 0.01);
    if (const auto value = result.scalars.find("output_impedance"); value != result.scalars.end())
        checkClose(value->second, 1000.0, 1.0);
}

TEST_CASE("transfer-function output impedance accepts a v(node) output expression") {
    auto request = rcRequest("tf");
    request.tfOutput = "v(out)";
    request.tfSource = "V1";
    const auto result = ice::SimulationRunner().run(request, std::chrono::seconds(5));
    CHECK(result.state == ice::JobState::succeeded);
    CHECK(result.scalars.contains("output_impedance"));
    if (const auto value = result.scalars.find("output_impedance"); value != result.scalars.end())
        checkClose(value->second, 1000.0, 1.0);
}

TEST_CASE("ngspice adapter supports noise analysis") {
    auto request = rcRequest("noise");
    request.noiseOutput = "out";
    request.noiseInput = "V1";
    request.noiseVariation = "dec";
    request.noisePoints = 5;
    request.noiseStart = 1.0;
    request.noiseStop = 1e6;
    const auto result = ice::SimulationRunner().run(request, std::chrono::seconds(5));
    CHECK(result.state == ice::JobState::succeeded);
    CHECK(result.xAxis == "frequency");
    CHECK(hasTrace(result, "onoise_spectrum"));
    CHECK(hasTrace(result, "inoise_spectrum"));
    CHECK(result.scalars.contains("onoise_total"));
    CHECK(result.scalars.contains("inoise_total"));
}

TEST_CASE("declarative AC measurements cover gain, bandwidth, and phase margin") {
    ice::SimulationResult ac;
    ac.analysis = "ac";
    ac.xAxis = "frequency";
    ac.xUnit = "Hz";
    ac.time = {1.0, 10.0, 100.0, 1e3, 1e4, 1e5, 1e6};
    ac.traces = {
        {"out", {1.0, 1.0, 1.0, 0.99, 0.9, 0.1, 0.001}},
        {"out.phase", {0.0, -1.0, -5.0, -45.0, -80.0, -170.0, -179.0}},
        {"in", {1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0}},
        {"in.phase", {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}},
    };
    const std::vector<ice::SimulationMeasurementSpec> specs = {
        {"gain", "gain", "out", "in", 0.0, std::nullopt, 0.02},
        {"bandwidth", "bandwidth", "out", "in", 0.0, std::nullopt, 0.02},
        {"phase_margin", "phase_margin", "out", "in", 0.0, std::nullopt, 0.02},
    };
    const auto measurements = ice::evaluateMeasurements(specs, ac);
    REQUIRE(measurements.size() == 3);
    CHECK(measurements[0].valid);
    CHECK(std::abs(measurements[0].value) < 0.2);
    CHECK(measurements[1].valid);
    CHECK(measurements[1].value > 1e3);
    CHECK(measurements[1].value < 1e6);
    CHECK(measurements[2].valid);
    CHECK(measurements[2].value > 0.0);
}

TEST_CASE("declarative transient measurements cover slew rate, settling, and power") {
    ice::SimulationResult transient;
    transient.analysis = "transient";
    transient.time = {0.0, 1e-6, 2e-6, 3e-6, 4e-6, 5e-6};
    transient.traces = {
        {"out", {0.0, 1.0, 1.1, 1.0, 1.0, 1.0}},
        {"i(v1)", {-0.001, -0.002, -0.002, -0.002, -0.002, -0.002}},
    };
    const std::vector<ice::SimulationMeasurementSpec> specs = {
        {"slew", "slew_rate", "out", "", 0.0, std::nullopt, 0.02},
        {"settling", "settling_time", "out", "", 0.0, std::nullopt, 0.02},
        {"power", "power", "i(v1)", "", 1.0, std::nullopt, 0.02},
    };
    const auto measurements = ice::evaluateMeasurements(specs, transient);
    REQUIRE(measurements.size() == 3);
    CHECK(measurements[0].valid);
    checkClose(measurements[0].value, 1e6, 5e4);
    CHECK(measurements[1].valid);
    CHECK(measurements[1].value >= 0.0);
    CHECK(measurements[2].valid);
    checkClose(measurements[2].value, 0.0018333, 0.0001);
}

TEST_CASE("Experiment expands parameter sets, corners, and Monte Carlo runs deterministically") {
    ice::SimulationRequest request;
    request.analysis = "op";
    request.netlist = {"* experiment", "V1 in 0 DC 1", "R1 in out 1k", "C1 out 0 1u"};
    request.probeSignals = {"out"};
    ice::Experiment experiment;
    experiment.addParameter("temperature", {27.0, 85.0});
    experiment.addCorner({"tt", "", 27.0, {}, {}});
    experiment.addCorner({"ff", "", 85.0, {{"v1", 1.1}}, {}});
    const auto points = experiment.run(request, std::chrono::seconds(5));
    REQUIRE(points.size() == 4);
    CHECK(points.front().corner == "tt");
    CHECK(points.back().corner == "ff");
    CHECK(points.front().result.state == ice::JobState::succeeded);

    ice::SimulationMonteCarloOptions monteCarlo;
    monteCarlo.enabled = true;
    monteCarlo.samples = 3;
    monteCarlo.temperatureSigma = 5.0;
    monteCarlo.supplySigmaPercent = 5.0;
    monteCarlo.supplyNominal = 1.0;
    monteCarlo.seed = 7;
    ice::Experiment varied;
    varied.setMonteCarlo(monteCarlo);
    const auto first = varied.run(request, std::chrono::seconds(5));
    const auto second = varied.run(request, std::chrono::seconds(5));
    REQUIRE(first.size() == 3);
    REQUIRE(second.size() == 3);
    for (std::size_t index = 0; index < first.size(); ++index) {
        CHECK(first[index].variant == second[index].variant);
        CHECK(first[index].parameters == second[index].parameters);
    }
}

TEST_CASE("simulation analysis and experiment settings round-trip through the project") {
    ice::Project project;
    project.identity = "sim-settings";
    project.name = "sim-settings";
    const auto cell =
        ice::createTextSchematicCell(project, "rc", "generic", "* RC\nV1 in 0 DC 1 AC 1\nR1 in out 1k\nC1 out 0 1u\n");
    ice::SimulationItem simulation;
    simulation.id = project.nextId();
    simulation.name = "corner_sim";
    simulation.schematic = cell;
    simulation.analysis = "ac";
    simulation.temperature = 85.0;
    simulation.modelSection = "ff";
    simulation.acVariation = "oct";
    simulation.acPoints = 7;
    simulation.acStart = 10.0;
    simulation.acStop = 1e8;
    simulation.noiseOutput = "out";
    simulation.noiseInput = "V1";
    simulation.tfOutput = "out";
    simulation.tfSource = "V1";
    simulation.parameters["w"] = 2.0;
    simulation.sourceOverrides["v1"] = 1.1;
    simulation.corners.push_back({"tt", "mos_tt", 27.0, {{"v1", 1.0}}, {{"w", 1.0}}});
    simulation.sweeps.push_back({"temperature", {27.0, 85.0}});
    simulation.monteCarlo.enabled = true;
    simulation.monteCarlo.samples = 5;
    simulation.monteCarlo.temperatureSigma = 2.0;
    simulation.monteCarlo.supplySigmaPercent = 5.0;
    simulation.monteCarlo.supplySource = "V1";
    simulation.monteCarlo.supplyNominal = 1.0;
    simulation.monteCarlo.parameterSigmaPercent["w"] = 3.0;
    simulation.monteCarlo.seed = 11;
    simulation.measurements.push_back({"gain", "gain", "out", "in", 0.0, std::nullopt, 0.02});
    project.simulations.emplace(simulation.id, simulation);

    const auto restored = ice::Project::deserialize(project.serialize());
    const auto &loaded = restored.simulations.at(simulation.id);
    CHECK(loaded.analysis == "ac");
    CHECK(loaded.temperature == 85.0);
    CHECK(loaded.modelSection == "ff");
    CHECK(loaded.acVariation == "oct");
    CHECK(loaded.acPoints == 7);
    CHECK(loaded.acStart == 10.0);
    CHECK(loaded.acStop == 1e8);
    CHECK(loaded.noiseOutput == "out");
    CHECK(loaded.tfSource == "V1");
    CHECK(loaded.parameters.at("w") == 2.0);
    CHECK(loaded.sourceOverrides.at("v1") == 1.1);
    REQUIRE(loaded.corners.size() == 1);
    CHECK(loaded.corners.front().modelSection == "mos_tt");
    REQUIRE(loaded.sweeps.size() == 1);
    CHECK(loaded.sweeps.front().values.size() == 2);
    CHECK(loaded.monteCarlo.enabled);
    CHECK(loaded.monteCarlo.samples == 5);
    CHECK(loaded.monteCarlo.parameterSigmaPercent.at("w") == 3.0);
    REQUIRE(loaded.measurements.size() == 1);
    CHECK(loaded.measurements.front().metric == "gain");
}

TEST_CASE("SimulationRunner async cancellation reaches the active adapter") {
    class BlockingAdapter final : public ice::SimulatorAdapter {
      public:
        ice::SimulationResult run(const ice::SimulationRequest &, const ice::CancellationToken &token,
                                  const ice::ProgressCallback &) override {
            for (int index = 0; index < 500 && !token.cancelled(); ++index)
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            ice::SimulationResult result;
            result.state = token.cancelled() ? ice::JobState::cancelled : ice::JobState::succeeded;
            result.error = token.cancelled() ? "simulation cancelled" : "";
            return result;
        }
    };

    auto adapter = std::make_shared<BlockingAdapter>();
    ice::SimulationRunner runner(adapter);
    auto future = runner.runAsync(ice::SimulationRequest{}, std::chrono::seconds(5));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    runner.cancel();
    const auto result = future.get();
    CHECK(result.state == ice::JobState::cancelled);
}
