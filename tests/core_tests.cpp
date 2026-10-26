#include "icelib/core.hpp"
#include "icelib/geometry.hpp"
#include "icelib/interchange.hpp"
#include "icelib/operations.hpp"
#include "icelib/packages.hpp"
#include "icelib/plugins.hpp"
#include "icelib/simulation.hpp"
#include "icelib/symbols.hpp"
#include "icelib/verification.hpp"
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>

TEST_CASE("project serialization and transactions are deterministic") {
    ice::Project project;
    project.identity = "demo";
    project.name = "demo";
    project.packageDependencies = {"example.pdk"};
    auto layerId = project.nextId();
    project.layers.emplace(layerId, ice::Layer{layerId, "metal1", 1});
    auto schematicId = project.nextId();
    ice::SchematicCell schematic;
    schematic.id = schematicId;
    schematic.name = "top";
    schematic.pdk = "example.pdk";
    auto symbolId = project.nextId();
    ice::Symbol symbol;
    symbol.id = symbolId;
    symbol.name = "resistor";
    symbol.label = ice::SymbolLabel{3, 9};
    auto pinId = project.nextId();
    symbol.pins.push_back({pinId, "A", 0, 0});
    schematic.symbols.emplace(symbolId, symbol);
    auto instanceId = project.nextId();
    schematic.instances.emplace(instanceId, ice::SchematicInstance{instanceId, symbolId, "R1", 0, 0});
    auto netId = project.nextId();
    schematic.nets.emplace(netId, ice::Net{netId, "net1", {pinId}});
    project.schematicCells.emplace(schematicId, schematic);
    auto cellId = project.nextId();
    ice::LayoutCell cell;
    cell.id = cellId;
    cell.name = "top";
    auto shapeId = project.nextId();
    cell.shapes.emplace(shapeId, ice::Shape{shapeId, layerId, std::nullopt, {0, 0, 100, 50}});
    project.layoutCells.emplace(cellId, cell);
    CHECK(project.validate().empty());
    auto serialized = project.serialize();
    auto roundTrip = ice::Project::deserialize(serialized);
    CHECK(roundTrip.serialize() == serialized);
    CHECK(roundTrip.schematicCells.at(schematicId).symbols.at(symbolId).label.has_value());
    CHECK(roundTrip.schematicCells.at(schematicId).symbols.at(symbolId).label->x == 3);
    CHECK(roundTrip.schematicCells.at(schematicId).symbols.at(symbolId).label->y == 9);

    auto invalid = project;
    invalid.layoutCells.at(cellId).shapes.at(shapeId).layer = 9999;
    CHECK(!invalid.validate().empty());

    ice::ProjectSession session(roundTrip);
    bool notified = false;
    session.subscribe([&] { notified = true; });
    session.execute("rename", [](ice::Project &value) { value.name = "changed"; });
    CHECK(session.dirty());
    CHECK(notified);
    CHECK(session.project().name == "changed");
    CHECK(session.undo());
    CHECK(session.project().name == "demo");
    CHECK(session.redo());
    CHECK(session.project().name == "changed");
    CHECK_THROWS_AS(session.execute("invalid", [](ice::Project &value) { value.grid = 0; }), ice::ProjectError);
    CHECK(session.project().grid == 1);
}

TEST_CASE("geometry operations are deterministic and queryable") {
    ice::GeometryKernel kernel;
    ice::MultiPolygon first = {{{0, 0}, {10, 0}, {10, 10}, {0, 10}}};
    ice::MultiPolygon second = {{{5, 0}, {15, 0}, {15, 10}, {5, 10}}};
    auto intersection = kernel.booleanOp(first, second, ice::BooleanOperation::intersection);
    CHECK(intersection.size() == 1);
    CHECK(ice::GeometryKernel::bounds(intersection.front()).width == 5);
    auto expanded = kernel.offset(first, 2);
    CHECK(!expanded.empty());

    ice::SpatialIndex index;
    index.insert(10, {0, 0, 10, 10});
    index.insert(20, {20, 20, 5, 5});
    CHECK(index.query({5, 5, 2, 2}) == std::vector<ice::ObjectId>{10});
    CHECK(index.query({10, 10, 2, 2}).empty());

    ice::ArraySpec array{2, 1, 20, 0};
    auto transformed = ice::transformArray(first, ice::Transform{100, 50, ice::Orientation::r0}, array);
    CHECK(transformed.size() == 2);
    CHECK(transformed[1].front().x == 120);
}

TEST_CASE("selection, cross-probing, and grid snapping are stable") {
    ice::Project project;
    project.identity = "selection";
    project.name = "selection";
    project.grid = 10;
    CHECK(project.snap(14) == 10);
    CHECK(project.snap(16) == 20);
    CHECK(project.snap(-14) == -10);
    CHECK(project.snap(-16) == -20);
    CHECK(project.snap(0) == 0);
    CHECK(project.snap(5) == 10);
    CHECK(project.snap(-5) == 0);

    ice::SelectionModel selection;
    selection.select(42, {7, 8});
    CHECK(selection.primary() == std::optional<ice::ObjectId>{42});
    CHECK((selection.related() == std::vector<ice::ObjectId>{7, 8}));
    selection.clear();
    CHECK(!selection.primary().has_value());
    CHECK(selection.related().empty());
}

TEST_CASE("schematic instance parameters survive serialization") {
    ice::Project project;
    project.identity = "params";
    project.name = "params";
    const auto cellId = ice::createSchematicCell(project, "top");
    const auto symbolId = ice::addSymbol(project, cellId, "resistor");
    const auto instanceId = ice::addSchematicInstance(project, cellId, symbolId, "R1", 0, 0);
    project.schematicCells.at(cellId).instances.at(instanceId).parameters.push_back({project.nextId(), "r", "1k"});
    CHECK(project.validate().empty());
    const auto serialized = project.serialize();
    const auto roundTrip = ice::Project::deserialize(serialized);
    CHECK(roundTrip.serialize() == serialized);
    const auto &restored = roundTrip.schematicCells.at(cellId).instances.at(instanceId).parameters;
    CHECK(restored.size() == 1);
    CHECK(restored.front().name == "r");
    CHECK(restored.front().value == "1k");
}

TEST_CASE("hierarchical symbols keep their source schematic") {
    ice::Project project;
    project.identity = "hierarchy";
    project.name = "hierarchy";
    const auto childCellId = ice::createSchematicCell(project, "child");
    const auto parentCellId = ice::createSchematicCell(project, "parent");
    const auto symbolId = ice::addSymbol(project, parentCellId, "child", {}, childCellId);
    const auto instanceId = ice::addSchematicInstance(project, parentCellId, symbolId, "child1", 0, 0);
    CHECK(project.validate().empty());
    const auto roundTrip = ice::Project::deserialize(project.serialize());
    CHECK(roundTrip.schematicCells.at(parentCellId).symbols.at(symbolId).sourceCell == childCellId);
    CHECK(roundTrip.schematicCells.at(parentCellId).instances.contains(instanceId));
}

TEST_CASE("symbol default parameters are seeded without duplicates") {
    ice::Project project;
    project.identity = "defaults";
    project.name = "defaults";
    const auto cellId = ice::createSchematicCell(project, "top");
    const auto symbolId = ice::addSymbol(project, cellId, "Voltage Source");
    const auto instanceId = ice::addSchematicInstance(project, cellId, symbolId, "V1", 0, 0);
    auto &instance = project.schematicCells.at(cellId).instances.at(instanceId);

    const std::vector<ice::Parameter> defaults = {{0, "voltage", "1"}, {0, "gain", "2"}};
    CHECK(ice::seedDefaultParameters(project, instance, defaults) == 2);
    CHECK(instance.parameters.size() == 2);
    CHECK(instance.parameters[0].id != 0);
    CHECK(instance.parameters[1].id != 0);
    CHECK(instance.parameters[0].id != instance.parameters[1].id);
    CHECK(instance.parameters[0].name == "voltage");
    CHECK(instance.parameters[0].value == "1");
    // Re-seeding keeps existing values and does not duplicate names.
    CHECK(ice::seedDefaultParameters(project, instance, defaults) == 0);
    CHECK(instance.parameters.size() == 2);
}

TEST_CASE("simulation graph axis limits are optional and survive serialization") {
    ice::Project project;
    project.identity = "graphs";
    project.name = "graphs";
    const auto simId = project.nextId();
    ice::SimulationItem sim;
    sim.id = simId;
    sim.name = "tran";
    sim.use_initial_conditions = true;
    sim.captureSignals = "v(out), v(in)";
    ice::SimulationGraph automatic;
    automatic.id = project.nextId();
    automatic.name = "auto";
    automatic.ySignals = {"v(out)"};
    ice::SimulationGraph limited;
    limited.id = project.nextId();
    limited.name = "limited";
    limited.xAxis = "v(in)";
    limited.ySignals = {"v(out)", "v(in)"};
    limited.xMin = 1e-9;
    limited.xMax = 2e-9;
    limited.yMin = -0.5;
    limited.yMax = 1.5;
    sim.graphs = {automatic, limited};
    project.simulations.emplace(simId, sim);

    const auto serialized = project.serialize();
    const auto roundTrip = ice::Project::deserialize(serialized);
    CHECK(roundTrip.simulations.at(simId).use_initial_conditions);
    CHECK(roundTrip.simulations.at(simId).captureSignals == "v(out), v(in)");
    const auto &graphs = roundTrip.simulations.at(simId).graphs;
    CHECK(graphs[0].xAxis == "time");
    CHECK((graphs[0].ySignals == std::vector<std::string>{"v(out)"}));
    CHECK(graphs[1].xAxis == "v(in)");
    CHECK((graphs[1].ySignals == std::vector<std::string>{"v(out)", "v(in)"}));
    CHECK(graphs.size() == 2);
    CHECK(!graphs[0].xMin.has_value());
    CHECK(!graphs[0].xMax.has_value());
    CHECK(!graphs[0].yMin.has_value());
    CHECK(!graphs[0].yMax.has_value());
    CHECK(graphs[1].xMin == std::optional<double>{1e-9});
    CHECK(graphs[1].xMax == std::optional<double>{2e-9});
    CHECK(graphs[1].yMin == std::optional<double>{-0.5});
    CHECK(graphs[1].yMax == std::optional<double>{1.5});
    CHECK(roundTrip.serialize() == serialized);
}

TEST_CASE("geometry transforms match Qt rotation convention") {
    const ice::Point point{10, 0};
    CHECK((ice::Transform{0, 0, ice::Orientation::r0}.apply(point) == ice::Point{10, 0}));
    CHECK((ice::Transform{0, 0, ice::Orientation::r90}.apply(point) == ice::Point{0, 10}));
    CHECK((ice::Transform{0, 0, ice::Orientation::r180}.apply(point) == ice::Point{-10, 0}));
    CHECK((ice::Transform{0, 0, ice::Orientation::r270}.apply(point) == ice::Point{0, -10}));
    CHECK((ice::Transform{0, 0, ice::Orientation::m45}.apply(point) == ice::Point{0, 10}));
    CHECK((ice::Transform{0, 0, ice::Orientation::m135}.apply(point) == ice::Point{0, -10}));
}

TEST_CASE("ngspice simulation reports missing netlists explicitly") {
    ice::SimulationRequest request;
    ice::SimulationRunner runner;
    auto result = runner.run(request, std::chrono::seconds(1));
    CHECK(result.state == ice::JobState::failed);
    CHECK(result.error.find("netlist") != std::string::npos);

    ice::SimulationRequest unsupported = request;
    unsupported.netlist = {"* test", ".end"};
    unsupported.analysis = "ac";
    auto rejected = runner.run(unsupported, std::chrono::seconds(1));
    CHECK(rejected.state == ice::JobState::failed);
    CHECK(rejected.error.find("unsupported") != std::string::npos);
    CHECK(!rejected.input_fingerprint.empty());
}

TEST_CASE("ngspice fetches the vectors named by the request signals") {
    ice::SimulationRequest request;
    request.analysis = "transient";
    request.step = 1e-9;
    request.stop = 200e-9;
    request.netlist = {"* RC transient", "V1 N14 N16 DC 0 PULSE(0 1 1n 1n 1n 100n 200n)", "Resistor N14 N15 1k",
                       "Capacitor N15 N16 1n"};
    request.probeSignals = {"N15"};
    ice::SimulationRunner runner;
    auto result = runner.run(request, std::chrono::seconds(5));
    CHECK(result.state == ice::JobState::succeeded);
    CHECK(!result.time.empty());
    CHECK(result.traces.size() == 1);
    if (!result.traces.empty()) {
        CHECK(result.traces[0].name == "N15");
        CHECK(result.traces[0].values.size() == result.time.size());
        CHECK(*std::max_element(result.traces[0].values.begin(), result.traces[0].values.end()) > 0.01);
    }
}

TEST_CASE("ngspice adapter supplies a title line for raw netlists") {
    ice::SimulationRequest request;
    request.analysis = "transient";
    request.step = 1e-9;
    request.stop = 200e-9;
    request.netlist = {"V1 out 0 PULSE(0 1 1n 1n 1n 100n 200n)", "R1 out 0 1k"};
    request.probeSignals = {"out"};
    ice::SimulationRunner runner;
    const auto result = runner.run(request, std::chrono::seconds(5));
    CHECK(result.state == ice::JobState::succeeded);
    CHECK(result.traces.size() == 1);
    if (!result.traces.empty())
        CHECK(*std::max_element(result.traces.front().values.begin(), result.traces.front().values.end()) > 0.5);
}

TEST_CASE("a run with no resolvable signal names fails with a clear error") {
    ice::SimulationRequest request;
    request.analysis = "transient";
    request.step = 1e-9;
    request.stop = 200e-9;
    request.netlist = {"* RC transient", "V1 N14 N16 DC 0 PULSE(0 1 1n 1n 1n 100n 200n)", "Resistor N14 N15 1k",
                       "Capacitor N15 N16 1n"};
    request.probeSignals = {"nope"};
    ice::SimulationRunner runner;
    auto result = runner.run(request, std::chrono::seconds(5));
    CHECK(result.state == ice::JobState::failed);
    CHECK(result.error.find("nope") != std::string::npos);
}

TEST_CASE("schematic netlist maps symbols, nets, and values to SPICE") {
    ice::Project project;
    project.identity = "net";
    project.name = "net";
    const auto cellId = ice::createSchematicCell(project, "top");
    auto &cell = project.schematicCells.at(cellId);

    auto makeSymbol = [&](const std::string &name, std::vector<std::string> pinNames) {
        const auto symbolId = ice::addSymbol(project, cellId, name);
        auto &symbol = cell.symbols.at(symbolId);
        for (const auto &pinName : pinNames) {
            const auto pinId = project.nextId();
            symbol.pins.push_back({pinId, pinName, 0, 0});
        }
        return symbolId;
    };
    auto pinIdOf = [&](ice::ObjectId symbolId, std::size_t index) {
        return cell.symbols.at(symbolId).pins.at(index).id;
    };

    const auto sourceId = makeSymbol("Voltage Source", {"+", "-"});
    const auto resistorId = makeSymbol("Resistor", {"1", "2"});
    const auto groundId = makeSymbol("Ground", {"GND"});
    const auto weirdId = makeSymbol("Mystery Widget", {"a", "b"});

    const auto source = ice::addSchematicInstance(project, cellId, sourceId, "V1", 0, 0);
    const auto resistor = ice::addSchematicInstance(project, cellId, resistorId, "R1", 0, 0);
    ice::addSchematicInstance(project, cellId, groundId, "GND1", 0, 0);
    ice::addSchematicInstance(project, cellId, weirdId, "X1", 0, 0);
    cell.instances.at(source).parameters.push_back({project.nextId(), "value", "PULSE(0 1 1n 1n 1n 100n 200n)"});
    cell.instances.at(resistor).parameters.push_back({project.nextId(), "resistance", "2k"});

    const auto inductorId = makeSymbol("Inductor", {"1", "2"});
    const auto inductor = ice::addSchematicInstance(project, cellId, inductorId, "L1", 0, 0);
    cell.instances.at(inductor).parameters.push_back({project.nextId(), "inductance", "3u"});

    // V1: + -> out, - -> 0 ; R1: 1 -> out, 2 -> 0 ; X1: a -> out, b -> 0.
    const auto netId = project.nextId();
    cell.nets.emplace(netId,
                      ice::Net{netId, "out", {pinIdOf(sourceId, 0), pinIdOf(resistorId, 0), pinIdOf(weirdId, 0)}});
    const auto groundNetId = project.nextId();
    cell.nets.emplace(groundNetId, ice::Net{groundNetId,
                                            "",
                                            {pinIdOf(sourceId, 1), pinIdOf(resistorId, 1), pinIdOf(groundId, 0),
                                             pinIdOf(weirdId, 1)}});

    std::vector<std::string> notes;
    const auto netlist = ice::buildSchematicNetlist(project, cellId, &notes);
    const auto joined = [&] {
        std::string text;
        for (const auto &line : netlist)
            text += line + "\n";
        return text;
    }();

    // Voltage source and resistor are wired out/0 with their values.
    CHECK(joined.find("V1 out 0 PULSE") != std::string::npos);
    // Quantity-named parameters (resistance, inductance) feed the value slot.
    CHECK(joined.find("R1 out 0 2k") != std::string::npos);
    CHECK(joined.find("L1 0 0 3u") != std::string::npos);
    // The unsupported symbol is dropped and reported, not silently netlisted:
    // it appears only in a comment, never as a live X-element.
    CHECK(joined.find("\nX1") == std::string::npos);
    CHECK(joined.find("unsupported: X1 Mystery Widget") != std::string::npos);
    bool reportedUnsupported = false;
    for (const auto &noteText : notes)
        if (noteText.find("unsupported") != std::string::npos)
            reportedUnsupported = true;
    CHECK(reportedUnsupported);
    // No trailing .end: the simulator adapter appends it.
    CHECK(joined.find(".end") == std::string::npos);
}

TEST_CASE("waveform and current source symbols generate SPICE source expressions") {
    ice::Project project;
    project.identity = "sources";
    project.name = "sources";
    const auto cellId = ice::createSchematicCell(project, "top");
    auto &cell = project.schematicCells.at(cellId);

    auto addSource = [&](const std::string &name) {
        const auto symbolId = ice::addSymbol(project, cellId, name);
        auto &symbol = cell.symbols.at(symbolId);
        for (const auto *pinName : {"+", "-"}) {
            const auto pinId = project.nextId();
            symbol.pins.push_back({pinId, pinName, 0, 0});
        }
        return symbolId;
    };
    auto pinIdOf = [&](ice::ObjectId symbolId, std::size_t index) {
        return cell.symbols.at(symbolId).pins.at(index).id;
    };
    auto setParameters = [&](ice::ObjectId instanceId,
                             std::initializer_list<std::pair<std::string, std::string>> values) {
        for (const auto &[name, value] : values)
            cell.instances.at(instanceId).parameters.push_back({project.nextId(), name, value});
    };

    const auto pulseId = addSource("Pulse Voltage Source");
    const auto sineId = addSource("Sine Voltage Source");
    const auto currentId = addSource("Current Source");
    const auto pulseDefaultsId = addSource("Pulse Voltage Source");
    const auto sineDefaultsId = addSource("Sine Voltage Source");
    const auto groundId = addSource("Ground");

    ice::addSchematicInstance(project, cellId, groundId, "GND1", 0, 0);
    const auto pulse = ice::addSchematicInstance(project, cellId, pulseId, "V1", 0, 0);
    const auto sine = ice::addSchematicInstance(project, cellId, sineId, "V2", 0, 0);
    const auto current = ice::addSchematicInstance(project, cellId, currentId, "I1", 0, 0);
    ice::addSchematicInstance(project, cellId, pulseDefaultsId, "V3", 0, 0);
    ice::addSchematicInstance(project, cellId, sineDefaultsId, "V4", 0, 0);
    setParameters(pulse, {{"initial_voltage", "-1"},
                          {"pulsed_voltage", "2.5"},
                          {"delay_time", "1u"},
                          {"rise_time", "2u"},
                          {"fall_time", "3u"},
                          {"pulse_width", "4m"},
                          {"period", "5m"},
                          {"pulse_count", "3"}});
    setParameters(sine, {{"offset_voltage", "1"},
                         {"amplitude", "2"},
                         {"frequency", "3k"},
                         {"delay_time", "4u"},
                         {"damping_factor", "5"},
                         {"phase", "6"}});
    setParameters(current, {{"current", "7m"}});

    const auto outNet = project.nextId();
    cell.nets.emplace(outNet, ice::Net{outNet,
                                       "out",
                                       {pinIdOf(pulseId, 0), pinIdOf(sineId, 0), pinIdOf(currentId, 0),
                                        pinIdOf(pulseDefaultsId, 0), pinIdOf(sineDefaultsId, 0)}});
    const auto groundNet = project.nextId();
    cell.nets.emplace(groundNet,
                      ice::Net{groundNet,
                               "",
                               {pinIdOf(pulseId, 1), pinIdOf(sineId, 1), pinIdOf(currentId, 1),
                                pinIdOf(pulseDefaultsId, 1), pinIdOf(sineDefaultsId, 1), pinIdOf(groundId, 0)}});

    const auto netlist = ice::buildSchematicNetlist(project, cellId);
    const auto joined = [&] {
        std::string text;
        for (const auto &line : netlist)
            text += line + "\n";
        return text;
    }();
    CHECK(joined.find("V1 out 0 PULSE(-1 2.5 1u 2u 3u 4m 5m 3)") != std::string::npos);
    CHECK(joined.find("V2 out 0 SIN(1 2 3k 4u 5 6)") != std::string::npos);
    CHECK(joined.find("I1 out 0 7m") != std::string::npos);
    // Missing parameters fall back to the symbol's documented defaults.
    CHECK(joined.find("V3 out 0 PULSE(0 1 0 1u 1u 0.5 1 0)") != std::string::npos);
    CHECK(joined.find("V4 out 0 SIN(0 1 1 0 0 0)") != std::string::npos);
    CHECK(ice::spiceElementForSymbol("Current Source") == 'I');
    CHECK(ice::spiceElementForSymbol("Pulse Voltage Source") == 'V');
    CHECK(ice::spiceElementForSymbol("Sine Voltage Source") == 'V');
    CHECK(ice::spiceElementForSymbol("IHP NMOS 1.2V") == 0);
}

TEST_CASE("generated source expressions are accepted by ngspice") {
    auto run = [](const std::vector<std::string> &netlist, const std::string &probe) {
        ice::SimulationRequest request;
        request.analysis = "transient";
        request.step = 0.01;
        request.stop = 2.0;
        // ngspice treats the first netlist line as the circuit title, so make
        // it an explicit comment before the source netlist.
        request.netlist = {"* source expression test"};
        request.netlist.insert(request.netlist.end(), netlist.begin(), netlist.end());
        request.probeSignals = {probe};
        ice::SimulationRunner runner;
        return runner.run(request, std::chrono::seconds(5));
    };

    const auto pulse = run({"V1 out 0 DC 0 PULSE(0 1 0 1u 1u 0.5 1)", "R1 out 0 1k"}, "out");
    CHECK(pulse.state == ice::JobState::succeeded);
    CHECK(pulse.traces.size() == 1);
    if (!pulse.traces.empty()) {
        const auto &values = pulse.traces.front().values;
        CHECK(!values.empty());
        CHECK(*std::max_element(values.begin(), values.end()) > 0.9);
    }

    const auto sine = run({"V1 out 0 SIN(0 1 1 0 0 0)", "R1 out 0 1k"}, "out");
    CHECK(sine.state == ice::JobState::succeeded);
    CHECK(sine.traces.size() == 1);
    if (!sine.traces.empty()) {
        const auto &values = sine.traces.front().values;
        CHECK(!values.empty());
        CHECK(*std::max_element(values.begin(), values.end()) > 0.9);
        CHECK(*std::min_element(values.begin(), values.end()) < -0.9);
    }

    const auto current = run({"I1 0 out 1m", "R1 out 0 1k"}, "out");
    CHECK(current.state == ice::JobState::succeeded);
    CHECK(current.traces.size() == 1);
}

TEST_CASE("package manifests and lockfiles are reproducible") {
    auto repositoryRoot = std::filesystem::temp_directory_path() / "ice-package-test";
    std::filesystem::remove_all(repositoryRoot);
    std::filesystem::create_directories(repositoryRoot / "example-lib");
    std::filesystem::create_directories(repositoryRoot / "example-pdk");
    std::ofstream(repositoryRoot / "example-pdk/package.toml")
        << "[package]\nname = \"example.pdk\"\nversion = \"1.0.0\"\n\n[provides]\npdk = true\n"
           "\n[pdk]\ndisplay_name = \"Example PDK\"\ndefault_schematic = \"example_top\"\n"
           "\n[[pdk.layer]]\nname = \"diff\"\nnumber = 65\n"
           "\n[[pdk.layer]]\nname = \"metal1\"\nnumber = 68\n";
    std::ofstream(repositoryRoot / "example-lib/package.toml")
        << "[package]\nname = \"example.lib\"\nversion = \"1.0.0\"\n\n[dependencies]\n\"example.pdk\" = "
           "\">=1.0.0\"\n\n[provides]\nmodels = true\nsymbols = true\n";
    auto packageRoot = repositoryRoot / "example-lib";
    auto package = ice::PackageManager::readManifest(packageRoot);
    CHECK(package.name == "example.lib");
    CHECK(package.dependencies.at("example.pdk") == ">=1.0.0");
    CHECK((ice::PackageManager::capabilities(package) == std::vector<std::string>{"models", "symbols"}));
    const auto pdk = ice::PackageManager::readManifest(repositoryRoot / "example-pdk");
    CHECK(pdk.pdk.has_value());
    if (pdk.pdk) {
        CHECK(pdk.pdk->displayName == "Example PDK");
        CHECK(pdk.pdk->defaultSchematic == "example_top");
        CHECK(pdk.pdk->layers.size() == 2);
        CHECK(pdk.pdk->layers.front().name == "diff");
        CHECK(pdk.pdk->layers.front().number == 65);
    }
    auto lock = ice::PackageManager::resolve({repositoryRoot}, {{"example.lib", "*"}}, "0.1.0");
    CHECK(lock.packages.size() == 2);
    auto serialized = ice::serializeLockfile(lock);
    CHECK(serialized == ice::serializeLockfile(lock));
    CHECK(serialized.find("content_hash = \"sha256:") != std::string::npos);
    std::filesystem::remove_all(repositoryRoot);
}

TEST_CASE("package indexes discover submodules and logos") {
    const auto root = std::filesystem::temp_directory_path() / "ice-package-index-test";
    std::filesystem::remove_all(root);
    const auto packageRoot = root / "packages" / "example.lib";
    std::filesystem::create_directories(packageRoot / ".github");
    std::ofstream(root / ".gitmodules") << "[submodule \"example.lib\"]\n"
                                           "    path = packages/example.lib\n"
                                           "    url = https://example.invalid/example.lib.git\n"
                                           "    branch = dev\n";
    std::ofstream(packageRoot / "package.toml") << "[package]\n"
                                                   "schema_version = 1\n"
                                                   "name = \"example.lib\"\n"
                                                   "version = \"1.2.0\"\n"
                                                   "description = \"Example indexed package\"\n"
                                                   "\n[provides]\n"
                                                   "symbols = true\n";
    std::ofstream(packageRoot / ".github" / "logo.png") << "not a real image, only presence is tested";

    const auto entries = ice::PackageManager::scanPackageIndex(root);
    CHECK(entries.size() == 1);
    if (entries.size() == 1) {
        CHECK(entries.front().spec.name == "example.lib");
        CHECK(entries.front().repositoryUrl == "https://example.invalid/example.lib.git");
        CHECK(entries.front().repositoryRevision == "dev");
        CHECK(entries.front().logo == packageRoot / ".github" / "logo.png");
    }
    CHECK(ice::PackageManager::packageLogo(packageRoot) == packageRoot / ".github" / "logo.png");
    std::filesystem::remove_all(root);
}

TEST_CASE("installed packages can be uninstalled") {
    const auto root = std::filesystem::temp_directory_path() / "ice-package-uninstall-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "example.pkg");
    std::ofstream(root / "example.pkg" / "package.toml") << "[package]\nname = \"example.pkg\"\nversion = \"1.0.0\"\n";
    CHECK(std::filesystem::exists(root / "example.pkg"));
    ice::PackageManager::uninstallPackage(root, "example.pkg");
    CHECK(!std::filesystem::exists(root / "example.pkg"));
    CHECK_THROWS_AS(ice::PackageManager::uninstallPackage(root, "missing.pkg"), ice::ProjectError);
    CHECK_THROWS_AS(ice::PackageManager::uninstallPackage(root, "../escape"), ice::ProjectError);
    std::filesystem::remove_all(root);
}

TEST_CASE("schematics carry PDKs and projects carry package dependencies") {
    ice::Project project;
    project.identity = "pdk-project";
    project.name = "pdk-project";
    const auto generic = ice::createSchematicCell(project, "generic_top");
    const auto packaged = ice::createSchematicCell(project, "ihp_top", "ihp.sg13g2");
    CHECK(project.schematicCells.at(generic).pdk == ice::genericPdkId);
    CHECK(project.schematicCells.at(packaged).pdk == "ihp.sg13g2");
    project.packageDependencies = {"ihp.sg13g2"};
    CHECK(project.validate().empty());

    const auto roundTrip = ice::Project::deserialize(project.serialize());
    CHECK(roundTrip.packageDependencies == std::vector<std::string>{"ihp.sg13g2"});
    CHECK(roundTrip.schematicCells.at(generic).pdk == ice::genericPdkId);
    CHECK(roundTrip.schematicCells.at(packaged).pdk == "ihp.sg13g2");

    auto duplicated = project;
    duplicated.packageDependencies.push_back("ihp.sg13g2");
    CHECK(!duplicated.validate().empty());
    CHECK_THROWS_AS(ice::createSchematicCell(project, "empty_pdk", ""), ice::ProjectError);
    CHECK_THROWS_AS(ice::createSchematicCell(project, "bad_pdk", "../escape"), ice::ProjectError);
}

TEST_CASE("file symbol library loads and validates") {
    const auto root = std::filesystem::temp_directory_path() / "ice-symbols-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    std::ofstream(root / "resistor.svg") << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 32 8\">"
                                            "<path d=\"M0 4h32\" stroke=\"black\"/></svg>";
    std::ofstream(root / "resistor.toml")
        << "[symbol]\nname = \"Resistor\"\nartwork = \"resistor.svg\"\ncategory = \"Passive\"\n"
           "label_x = 0\nlabel_y = 12\n"
           "\n[[pin]]\nname = \"1\"\nx = -16\ny = 0\ndirection = \"inout\"\n"
           "\n[[pin]]\nname = \"2\"\nx = 16\ny = 0\ndirection = \"inout\"\n";
    const auto symbols = ice::SymbolLibrary::load(root);
    CHECK(symbols.size() == 1);
    CHECK(symbols.front().name == "Resistor");
    CHECK(symbols.front().category == "Passive");
    CHECK(symbols.front().svg.find("<svg") != std::string::npos);
    CHECK(symbols.front().label.has_value());
    CHECK(symbols.front().label->x == 0);
    CHECK(symbols.front().label->y == 12);
    CHECK(symbols.front().pins.size() == 2);
    CHECK(symbols.front().pins.front().direction == ice::PinDirection::left);
    CHECK(symbols.front().pins.back().direction == ice::PinDirection::right);

    const auto missing = root / "missing";
    std::filesystem::create_directories(missing);
    std::ofstream(missing / "broken.toml") << "[symbol]\nname = \"Broken\"\nartwork = \"absent.svg\"\n"
                                              "\n[[pin]]\nname = \"1\"\nx = 0\ny = 0\ndirection = \"inout\"\n";
    CHECK_THROWS_AS(ice::SymbolLibrary::load(missing), ice::ProjectError);

    const auto scripted = root / "scripted";
    std::filesystem::create_directories(scripted);
    std::ofstream(scripted / "evil.svg") << "<svg><script>alert(1)</script></svg>";
    std::ofstream(scripted / "evil.toml") << "[symbol]\nname = \"Evil\"\nartwork = \"evil.svg\"\n"
                                             "\n[[pin]]\nname = \"1\"\nx = 0\ny = 0\ndirection = \"inout\"\n";
    CHECK_THROWS_AS(ice::SymbolLibrary::load(scripted), ice::ProjectError);

    const auto fractional = root / "fractional";
    std::filesystem::create_directories(fractional);
    std::ofstream(fractional / "resistor.svg") << "<svg viewBox=\"0 0 32 8\"></svg>";
    std::ofstream(fractional / "resistor.toml") << "[symbol]\nname = \"Resistor\"\nartwork = \"resistor.svg\"\n"
                                                   "\n[[pin]]\nname = \"1\"\nx = -16.5\ny = 0\ndirection = \"inout\"\n";
    CHECK_THROWS_AS(ice::SymbolLibrary::load(fractional), ice::ProjectError);

    const auto duplicated = root / "duplicated";
    std::filesystem::create_directories(duplicated);
    std::ofstream(duplicated / "resistor.svg") << "<svg viewBox=\"0 0 32 8\"></svg>";
    std::ofstream(duplicated / "resistor.toml") << "[symbol]\nname = \"Resistor\"\nartwork = \"resistor.svg\"\n"
                                                   "\n[[pin]]\nname = \"1\"\nx = -16\ny = 0\ndirection = \"inout\"\n"
                                                   "\n[[pin]]\nname = \"1\"\nx = 16\ny = 0\ndirection = \"inout\"\n";
    CHECK_THROWS_AS(ice::SymbolLibrary::load(duplicated), ice::ProjectError);

    const auto nameless = root / "nameless";
    std::filesystem::create_directories(nameless);
    std::ofstream(nameless / "resistor.svg") << "<svg viewBox=\"0 0 32 8\"></svg>";
    std::ofstream(nameless / "resistor.toml")
        << "[symbol]\nname = \"Resistor\"\nartwork = \"resistor.svg\"\ncategory = \"Passive\"\n"
           "\n[[pin]]\nname = \"1\"\nx = -16\ny = 0\ndirection = \"inout\"\n"
           "\n[[pin]]\nname = \"2\"\nx = 16\ny = 0\ndirection = \"inout\"\n"
           "\n[[parameter]]\nname = \"\"\nvalue = \"1k\"\n";
    CHECK_THROWS_AS(ice::SymbolLibrary::load(nameless), ice::ProjectError);
    std::filesystem::remove_all(root);
}

TEST_CASE("symbols can reference built-in generic artwork") {
    const auto root = std::filesystem::temp_directory_path() / "ice-generic-artwork-test";
    std::filesystem::remove_all(root);
    const auto builtin = root / "builtin";
    const auto package = root / "package";
    const auto traversal = root / "traversal";
    std::filesystem::create_directories(builtin);
    std::filesystem::create_directories(package);
    std::filesystem::create_directories(traversal);
    std::ofstream(builtin / "shared.svg") << "<svg viewBox=\"0 0 10 10\"><path d=\"M0 0h10\"/></svg>";
    std::ofstream(package / "referenced.toml") << "[symbol]\nname = \"Referenced\"\nartwork = \"generic:shared.svg\"\n"
                                                  "\n[[pin]]\nname = \"1\"\nx = -5\ny = 0\ndirection = \"inout\"\n"
                                                  "\n[[pin]]\nname = \"2\"\nx = 5\ny = 0\ndirection = \"inout\"\n";
    const auto symbols = ice::SymbolLibrary::load(package, builtin);
    CHECK(symbols.size() == 1);
    CHECK(symbols.front().svg.find("M0 0h10") != std::string::npos);
    CHECK_THROWS_AS(ice::SymbolLibrary::load(package), ice::ProjectError);

    std::ofstream(traversal / "escape.toml") << "[symbol]\nname = \"Escape\"\nartwork = \"generic:../shared.svg\"\n"
                                                "\n[[pin]]\nname = \"1\"\nx = 0\ny = 0\ndirection = \"inout\"\n";
    CHECK_THROWS_AS(ice::SymbolLibrary::load(traversal, builtin), ice::ProjectError);
    std::filesystem::remove_all(root);
}

TEST_CASE("shipped file symbol library loads") {
    const auto libraryRoot = std::filesystem::path(ICE_SOURCE_DIR) / "symbols";
    const auto symbols = ice::SymbolLibrary::load(libraryRoot);
    CHECK(symbols.size() == 13);
    std::map<std::string, std::size_t> pinCounts;
    std::map<std::string, std::map<std::string, std::string>> defaults;
    for (const auto &symbol : symbols) {
        CHECK(!symbol.svg.empty());
        CHECK(!symbol.pins.empty());
        CHECK(symbol.label.has_value());
        pinCounts[symbol.name] = symbol.pins.size();
        for (const auto &parameter : symbol.defaultParameters)
            defaults[symbol.name][parameter.name] = parameter.value;
    }
    CHECK(pinCounts["Resistor"] == 2);
    CHECK(pinCounts["Capacitor"] == 2);
    CHECK(pinCounts["Diode"] == 2);
    CHECK(pinCounts["Voltage Source"] == 2);
    CHECK(pinCounts["Pulse Voltage Source"] == 2);
    CHECK(pinCounts["Sine Voltage Source"] == 2);
    CHECK(pinCounts["Current Source"] == 2);
    CHECK(pinCounts["Ground"] == 1);
    CHECK(pinCounts["Op Amp"] == 3);
    CHECK(pinCounts["NMOS"] == 4);
    CHECK(pinCounts["Inductor"] == 2);
    CHECK(pinCounts["Input"] == 1);
    CHECK(pinCounts["Output"] == 1);
    // Passive and source symbols seed their editable default parameters.
    CHECK(defaults["Resistor"]["resistance"] == "1k");
    CHECK(defaults["Capacitor"]["capacitance"] == "1n");
    CHECK(defaults["Inductor"]["inductance"] == "1u");
    CHECK(defaults["Voltage Source"]["voltage"] == "1");
    CHECK(defaults["Pulse Voltage Source"]["initial_voltage"] == "0");
    CHECK(defaults["Pulse Voltage Source"]["pulsed_voltage"] == "1");
    CHECK(defaults["Pulse Voltage Source"]["rise_time"] == "1u");
    CHECK(defaults["Pulse Voltage Source"]["pulse_width"] == "0.5");
    CHECK(defaults["Pulse Voltage Source"]["period"] == "1");
    CHECK(defaults["Pulse Voltage Source"]["pulse_count"] == "0");
    CHECK(defaults["Sine Voltage Source"]["offset_voltage"] == "0");
    CHECK(defaults["Sine Voltage Source"]["amplitude"] == "1");
    CHECK(defaults["Sine Voltage Source"]["frequency"] == "1");
    CHECK(defaults["Sine Voltage Source"]["phase"] == "0");
    CHECK(defaults["Current Source"]["current"] == "1m");
}

TEST_CASE("interchange, verification, and plugin lifecycle are explicit") {
    ice::Project project;
    project.identity = "design";
    project.name = "design";
    auto layer = project.nextId();
    project.layers.emplace(layer, ice::Layer{layer, "metal1", 1});
    auto cell = project.nextId();
    ice::LayoutCell layout{cell, "top"};
    auto shape = project.nextId();
    layout.shapes.emplace(shape, ice::Shape{shape, layer, std::nullopt, {0, 0, 10, 10}});
    project.layoutCells.emplace(cell, layout);
    auto path = std::filesystem::temp_directory_path() / "ice-layout.gds";
    ice::LayoutInterchange::exportGds(project, cell, path);
    auto imported = ice::LayoutInterchange::importGds(path);
    CHECK(imported.layoutCells.size() == 1);
    auto report = ice::LocalVerificationAdapter().run(project);
    CHECK(report.drc.empty());
    CHECK(ice::serializeVerificationReport(report).find("\"pex\"") != std::string::npos);
    auto future = ice::VerificationScheduler().schedule(project, std::make_shared<ice::LocalVerificationAdapter>());
    CHECK(future.get().schemaVersion == 1);
    std::filesystem::remove(path);

    auto pluginRoot = std::filesystem::temp_directory_path() / "ice-plugin";
    std::filesystem::create_directories(pluginRoot);
    std::ofstream(pluginRoot / "plugin.toml")
        << "[plugin]\nname=\"test-plugin\"\nversion=\"1.0.0\"\nice_target=\"0.1.0\"\n"
           "\n[permissions]\nread_project=true\nmutate_project=false\n";
    ice::PluginManager plugins;
    plugins.discover(pluginRoot);
    plugins.initialize("test-plugin");
    plugins.enable("test-plugin");
    CHECK(plugins.get("test-plugin").state == ice::PluginState::enabled);
    plugins.disable("test-plugin");
    CHECK(plugins.get("test-plugin").state == ice::PluginState::disabled);
    // Disabled plugins can be re-enabled without re-initialization.
    plugins.enable("test-plugin");
    CHECK(plugins.get("test-plugin").state == ice::PluginState::enabled);
    plugins.disable("test-plugin");
    plugins.unload("test-plugin");
    std::filesystem::remove_all(pluginRoot);
}
