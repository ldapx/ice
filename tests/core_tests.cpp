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
    auto layerId = project.nextId();
    project.layers.emplace(layerId, ice::Layer{layerId, "metal1", 1});
    auto schematicId = project.nextId();
    ice::SchematicCell schematic;
    schematic.id = schematicId;
    schematic.name = "top";
    auto symbolId = project.nextId();
    ice::Symbol symbol;
    symbol.id = symbolId;
    symbol.name = "resistor";
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

TEST_CASE("geometry transforms match Qt rotation convention") {
    const ice::Point point{10, 0};
    CHECK((ice::Transform{0, 0, ice::Orientation::r0}.apply(point) == ice::Point{10, 0}));
    CHECK((ice::Transform{0, 0, ice::Orientation::r90}.apply(point) == ice::Point{0, 10}));
    CHECK((ice::Transform{0, 0, ice::Orientation::r180}.apply(point) == ice::Point{-10, 0}));
    CHECK((ice::Transform{0, 0, ice::Orientation::r270}.apply(point) == ice::Point{0, -10}));
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
    request.netlist = {"V1 N14 N16 DC 0 PULSE(0 1 1n 1n 1n 100n 200n)",
                       "Resistor N14 N15 1k", "Capacitor N15 N16 1n"};
    request.probeSignals = {"N15"};
    ice::SimulationRunner runner;
    auto result = runner.run(request, std::chrono::seconds(5));
    CHECK(result.state == ice::JobState::succeeded);
    CHECK(!result.time.empty());
    CHECK(result.traces.size() == 1);
    if (!result.traces.empty()) {
        CHECK(result.traces[0].name == "N15");
        CHECK(result.traces[0].values.size() == result.time.size());
    }
}

TEST_CASE("a run with no resolvable signal names fails with a clear error") {
    ice::SimulationRequest request;
    request.analysis = "transient";
    request.step = 1e-9;
    request.stop = 200e-9;
    request.netlist = {"V1 N14 N16 DC 0 PULSE(0 1 1n 1n 1n 100n 200n)",
                       "Resistor N14 N15 1k", "Capacitor N15 N16 1n"};
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
    auto pinIdOf = [&](ice::ObjectId symbolId, std::size_t index) { return cell.symbols.at(symbolId).pins.at(index).id; };

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
    cell.nets.emplace(groundNetId,
                      ice::Net{groundNetId, "", {pinIdOf(sourceId, 1), pinIdOf(resistorId, 1), pinIdOf(groundId, 0),
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

TEST_CASE("package manifests and lockfiles are reproducible") {
    auto repositoryRoot = std::filesystem::temp_directory_path() / "ice-package-test";
    std::filesystem::remove_all(repositoryRoot);
    std::filesystem::create_directories(repositoryRoot / "example-lib");
    std::filesystem::create_directories(repositoryRoot / "example-pdk");
    std::ofstream(repositoryRoot / "example-pdk/package.toml")
        << "[package]\nname = \"example.pdk\"\nversion = \"1.0.0\"\n\n[provides]\npdk = true\n";
    std::ofstream(repositoryRoot / "example-lib/package.toml")
        << "[package]\nname = \"example.lib\"\nversion = \"1.0.0\"\n\n[dependencies]\n\"example.pdk\" = "
           "\">=1.0.0\"\n\n[provides]\nmodels = true\nsymbols = true\n";
    auto packageRoot = repositoryRoot / "example-lib";
    auto package = ice::PackageManager::readManifest(packageRoot);
    CHECK(package.name == "example.lib");
    CHECK(package.dependencies.at("example.pdk") == ">=1.0.0");
    CHECK((ice::PackageManager::capabilities(package) == std::vector<std::string>{"models", "symbols"}));
    auto lock = ice::PackageManager::resolve({repositoryRoot}, {{"example.lib", "*"}}, "0.1.0");
    CHECK(lock.packages.size() == 2);
    auto serialized = ice::serializeLockfile(lock);
    CHECK(serialized == ice::serializeLockfile(lock));
    CHECK(serialized.find("content_hash = \"sha256:") != std::string::npos);
    std::filesystem::remove_all(repositoryRoot);
}

TEST_CASE("file symbol library loads and validates") {
    const auto root = std::filesystem::temp_directory_path() / "ice-symbols-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    std::ofstream(root / "resistor.svg") << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 32 8\">"
                                            "<path d=\"M0 4h32\" stroke=\"black\"/></svg>";
    std::ofstream(root / "resistor.toml")
        << "[symbol]\nname = \"Resistor\"\nartwork = \"resistor.svg\"\ncategory = \"Passive\"\n"
           "\n[[pin]]\nname = \"1\"\nx = -16\ny = 0\ndirection = \"inout\"\n"
           "\n[[pin]]\nname = \"2\"\nx = 16\ny = 0\ndirection = \"inout\"\n";
    const auto symbols = ice::SymbolLibrary::load(root);
    CHECK(symbols.size() == 1);
    CHECK(symbols.front().name == "Resistor");
    CHECK(symbols.front().category == "Passive");
    CHECK(symbols.front().svg.find("<svg") != std::string::npos);
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

TEST_CASE("shipped file symbol library loads") {
    const auto libraryRoot = std::filesystem::path(ICE_SOURCE_DIR) / "symbols";
    const auto symbols = ice::SymbolLibrary::load(libraryRoot);
    CHECK(symbols.size() == 8);
    std::map<std::string, std::size_t> pinCounts;
    std::map<std::string, std::string> defaultValues;
    for (const auto &symbol : symbols) {
        CHECK(!symbol.svg.empty());
        CHECK(!symbol.pins.empty());
        pinCounts[symbol.name] = symbol.pins.size();
        for (const auto &parameter : symbol.defaultParameters)
            defaultValues[symbol.name] = parameter.name + "=" + parameter.value;
    }
    CHECK(pinCounts["Resistor"] == 2);
    CHECK(pinCounts["Capacitor"] == 2);
    CHECK(pinCounts["Diode"] == 2);
    CHECK(pinCounts["Voltage Source"] == 2);
    CHECK(pinCounts["Ground"] == 1);
    CHECK(pinCounts["Op Amp"] == 3);
    CHECK(pinCounts["NMOS"] == 4);
    CHECK(pinCounts["Inductor"] == 2);
    // Passive and source symbols seed a quantity-named default parameter.
    CHECK(defaultValues["Resistor"] == "resistance=1k");
    CHECK(defaultValues["Capacitor"] == "capacitance=1n");
    CHECK(defaultValues["Inductor"] == "inductance=1u");
    CHECK(defaultValues["Voltage Source"] == "voltage=1");
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
