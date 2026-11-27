#include "openvlsilib/core.hpp"
#include "openvlsilib/operations.hpp"
#include "openvlsilib/verification.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {
struct LvsFixture {
    openvlsi::Project project;
    openvlsi::ObjectId schematic = 0;
    openvlsi::ObjectId layout = 0;
    openvlsi::ObjectId resistor = 0;
    openvlsi::ObjectId capacitor = 0;
    std::vector<openvlsi::ObjectId> resistorPins;
    std::vector<openvlsi::ObjectId> capacitorPins;
};

openvlsi::ObjectId addTwoPinDevice(openvlsi::Project &project, openvlsi::ObjectId cell, const std::string &type,
                                   const std::string &name, std::vector<openvlsi::ObjectId> &pins) {
    const auto symbol = openvlsi::addSymbol(project, cell, type);
    auto &symbolEntry = project.schematicCells.at(cell).symbols.at(symbol);
    pins.clear();
    for (const char *pinName : {"A", "B"}) {
        const auto pin = project.nextId();
        symbolEntry.pins.push_back({pin, pinName, 0, 0});
        pins.push_back(pin);
    }
    return openvlsi::addSchematicInstance(project, cell, symbol, name, 0, 0);
}

LvsFixture makeFixture() {
    LvsFixture fixture;
    fixture.project.identity = "lvs-demo";
    fixture.project.name = "lvs-demo";
    fixture.schematic = openvlsi::createSchematicCell(fixture.project, "top");
    fixture.resistor = addTwoPinDevice(fixture.project, fixture.schematic, "resistor", "R1", fixture.resistorPins);
    fixture.capacitor = addTwoPinDevice(fixture.project, fixture.schematic, "capacitor", "C1", fixture.capacitorPins);
    fixture.project.schematicCells.at(fixture.schematic)
        .instances.at(fixture.resistor)
        .parameters.push_back({fixture.project.nextId(), "resistance", "1k"});
    fixture.project.schematicCells.at(fixture.schematic)
        .instances.at(fixture.capacitor)
        .parameters.push_back({fixture.project.nextId(), "capacitance", "1n"});
    openvlsi::addNet(fixture.project, fixture.schematic, "in", {fixture.resistorPins[0], fixture.capacitorPins[0]});
    openvlsi::addNet(fixture.project, fixture.schematic, "out", {fixture.resistorPins[1], fixture.capacitorPins[1]});
    fixture.layout = openvlsi::createLayoutFromSchematic(fixture.project, fixture.schematic);
    return fixture;
}

bool hasCategory(const openvlsi::LvsReport &report, const std::string &category) {
    return std::any_of(report.mismatches.begin(), report.mismatches.end(),
                       [&](const openvlsi::LvsMismatch &mismatch) { return mismatch.category == category; });
}

openvlsi::ObjectId plannedDeviceId(const openvlsi::Project &project, openvlsi::ObjectId layout,
                                   const std::string &name) {
    for (const auto &[id, device] : project.layoutCells.at(layout).plannedDevices) {
        if (device.name == name)
            return id;
    }
    return 0;
}
} // namespace

TEST_CASE("LVS matches a layout created from its schematic") {
    auto fixture = makeFixture();
    const auto report = openvlsi::runLvs(fixture.project, fixture.schematic, fixture.layout);
    CHECK(report.matched());
    CHECK(report.mismatches.empty());
    CHECK(report.schematic == "top");
    CHECK(report.layout == "top");
    CHECK(openvlsi::schematicForLayout(fixture.project, fixture.layout) == fixture.schematic);
    CHECK(openvlsi::serializeLvsReport(report).find("\"matched\": true") != std::string::npos);
}

TEST_CASE("LVS report exposes device net and port correspondence") {
    auto fixture = makeFixture();
    const auto report = openvlsi::runLvs(fixture.project, fixture.schematic, fixture.layout);
    REQUIRE(report.devices.size() == 2);
    for (const auto &match : report.devices) {
        REQUIRE(match.schematic.has_value());
        REQUIRE(match.layout.has_value());
        CHECK(match.method == openvlsi::LvsMatchMethod::linked);
        CHECK(match.matched);
        CHECK(match.pins.size() == 2);
        for (const auto &pin : match.pins)
            CHECK(pin.matched);
    }
    CHECK(report.summary.devicesMatched == 2);
    CHECK(report.summary.devicesMismatched == 0);
    REQUIRE(report.nets.size() == 2);
    for (const auto &match : report.nets) {
        REQUIRE(match.schematic.has_value());
        REQUIRE(match.layout.has_value());
        CHECK(match.matched);
        CHECK(match.layout->bounds.has_value());
        CHECK(!match.schematicConnections.empty());
    }
    CHECK(report.summary.netsMatched == 2);
    const auto serialized = openvlsi::serializeLvsReport(report);
    CHECK(serialized.find("\"schema_version\": 2") != std::string::npos);
    CHECK(serialized.find("\"devices\"") != std::string::npos);
    CHECK(serialized.find("\"method\": \"linked\"") != std::string::npos);
}

TEST_CASE("LVS correspondence reports name and topology match methods") {
    auto fixture = makeFixture();
    auto &layout = fixture.project.layoutCells.at(fixture.layout);
    for (auto &[_, device] : layout.plannedDevices)
        device.schematicInstance = 0;
    auto report = openvlsi::runLvs(fixture.project, fixture.schematic, fixture.layout);
    CHECK(report.matched());
    for (const auto &match : report.devices)
        CHECK(match.method == openvlsi::LvsMatchMethod::name);

    auto &renamed = fixture.project.layoutCells.at(fixture.layout);
    for (auto &[_, device] : renamed.plannedDevices)
        device.name = device.name == "R1" ? "RA" : "CA";
    report = openvlsi::runLvs(fixture.project, fixture.schematic, fixture.layout);
    CHECK(report.matched());
    for (const auto &match : report.devices)
        CHECK(match.method == openvlsi::LvsMatchMethod::topology);
}

TEST_CASE("LVS device mismatches carry typed refs and candidates") {
    auto fixture = makeFixture();
    auto &layout = fixture.project.layoutCells.at(fixture.layout);
    const auto extraId = fixture.project.nextId();
    layout.plannedDevices.emplace(extraId, openvlsi::PlannedDevice{extraId, "resistor", 0, 0, 0, 1200, 700});
    const auto report = openvlsi::runLvs(fixture.project, fixture.schematic, fixture.layout);
    const auto found = std::find_if(report.mismatches.begin(), report.mismatches.end(),
                                    [](const auto &mismatch) { return mismatch.category == "extra-device"; });
    REQUIRE(found != report.mismatches.end());
    REQUIRE(found->layoutRef.has_value());
    CHECK(found->layoutRef->kind == openvlsi::LvsObjectKind::plannedDevice);
    CHECK(found->layoutRef->name == "resistor");
    CHECK(found->layoutRef->bounds.has_value());
    CHECK(!found->candidates.empty());
}

TEST_CASE("layout device bindings can be authored queried and validated") {
    openvlsi::Project project;
    project.identity = "layout-bindings";
    project.name = "layout-bindings";
    const auto schematic = openvlsi::createSchematicCell(project, "top");
    std::vector<openvlsi::ObjectId> r1Pins;
    std::vector<openvlsi::ObjectId> r2Pins;
    const auto r1 = addTwoPinDevice(project, schematic, "resistor", "R1", r1Pins);
    const auto r2 = addTwoPinDevice(project, schematic, "resistor", "R2", r2Pins);
    const auto layout = openvlsi::createLayoutFromSchematic(project, schematic);
    auto bindings = openvlsi::layoutDeviceBindings(project, layout, schematic);
    REQUIRE(bindings.size() == 2);
    CHECK(std::none_of(bindings.begin(), bindings.end(),
                       [](const auto &binding) { return binding.stale || binding.duplicate; }));

    const auto r1Device = plannedDeviceId(project, layout, "R1");
    const auto r2Device = plannedDeviceId(project, layout, "R2");
    REQUIRE(r1Device != 0);
    REQUIRE(r2Device != 0);
    openvlsi::unbindLayoutDevice(project, layout, r1Device);
    CHECK(openvlsi::layoutDeviceBindings(project, layout, schematic).size() == 1);
    openvlsi::bindLayoutDevice(project, layout, r1Device, schematic, r2);
    bindings = openvlsi::layoutDeviceBindings(project, layout, schematic);
    REQUIRE(bindings.size() == 2);
    CHECK(std::all_of(bindings.begin(), bindings.end(), [](const auto &binding) { return binding.duplicate; }));
    const auto diagnostics = project.validate();
    CHECK(std::any_of(diagnostics.begin(), diagnostics.end(),
                      [](const auto &diagnostic) { return diagnostic.code == "layout.device_link.duplicate"; }));

    openvlsi::unbindLayoutDevice(project, layout, r1Device);
    project.schematicCells.at(schematic).instances.erase(r2);
    bindings = openvlsi::layoutDeviceBindings(project, layout);
    CHECK(std::any_of(bindings.begin(), bindings.end(), [](const auto &binding) { return binding.stale; }));
    CHECK_THROWS_AS(openvlsi::bindLayoutDevice(project, layout, r1Device, schematic, r2), openvlsi::ProjectError);
    (void)r1;
}

TEST_CASE("generated procedural layout cells accept device bindings") {
    openvlsi::Project project;
    project.identity = "layout-generated-bindings";
    project.name = "layout-generated-bindings";
    const auto schematic = openvlsi::createSchematicCell(project, "top");
    std::vector<openvlsi::ObjectId> pins;
    const auto instance = addTwoPinDevice(project, schematic, "resistor", "R1", pins);
    const auto procedural = openvlsi::createProceduralLayout(project, "top_pcell");
    const auto generated = project.proceduralLayouts.at(procedural).generatedCell;
    auto &cell = project.layoutCells.at(generated);
    CHECK(cell.readOnly);
    const auto device = project.nextId();
    cell.plannedDevices.emplace(device, openvlsi::PlannedDevice{device, "R1", 0, 0, 0, 1000, 1000});

    bool bound = true;
    try {
        openvlsi::bindLayoutDevice(project, generated, device, schematic, instance);
    } catch (const std::exception &) {
        bound = false;
    }
    CHECK(bound);
    CHECK(cell.plannedDevices.at(device).schematicInstance == instance);

    bool unbound = true;
    try {
        openvlsi::unbindLayoutDevice(project, generated, device);
    } catch (const std::exception &) {
        unbound = false;
    }
    CHECK(unbound);
    CHECK(cell.plannedDevices.at(device).schematicInstance == 0);
}

TEST_CASE("layout cells can link to existing schematic cells") {
    openvlsi::Project project;
    project.identity = "layout-schematic-link";
    project.name = "layout-schematic-link";
    const auto alpha = openvlsi::createSchematicCell(project, "alpha");
    const auto beta = openvlsi::createSchematicCell(project, "beta");
    const auto layout = openvlsi::createLayoutCell(project, "top");
    CHECK(project.layoutCells.at(layout).schematicCell == 0);
    CHECK(openvlsi::schematicForLayout(project, layout) == 0);
    openvlsi::linkLayoutToSchematic(project, layout, beta);
    CHECK(project.layoutCells.at(layout).schematicCell == beta);
    CHECK(openvlsi::schematicForLayout(project, layout) == beta);
    const auto roundTrip = openvlsi::Project::deserialize(project.serialize());
    CHECK(roundTrip.layoutCells.at(layout).schematicCell == beta);
    CHECK(openvlsi::schematicForLayout(roundTrip, layout) == beta);
    openvlsi::unlinkLayoutFromSchematic(project, layout);
    CHECK(project.layoutCells.at(layout).schematicCell == 0);
    CHECK(openvlsi::schematicForLayout(project, layout) == 0);
    CHECK_THROWS_AS(openvlsi::linkLayoutToSchematic(project, layout, project.nextId()), openvlsi::ProjectError);

    const auto projected = openvlsi::createLayoutFromSchematic(project, alpha);
    CHECK(project.layoutCells.at(projected).schematicCell == alpha);
}

TEST_CASE("LVS schematic inference prefers a DUT view") {
    auto fixture = makeFixture();
    const auto layout = openvlsi::createLayoutCell(fixture.project, "block");
    const auto dut = openvlsi::createSchematicCell(fixture.project, "block_lvs");
    openvlsi::createSchematicCell(fixture.project, "block");
    CHECK(openvlsi::schematicForLayout(fixture.project, layout) == dut);
}

TEST_CASE("LVS reports missing and extra devices") {
    auto fixture = makeFixture();
    auto &layout = fixture.project.layoutCells.at(fixture.layout);
    const auto extraId = fixture.project.nextId();
    layout.plannedDevices.emplace(extraId, openvlsi::PlannedDevice{extraId, "X1", 0, 0, 0, 1200, 700});

    const auto capacitorId = plannedDeviceId(fixture.project, fixture.layout, "C1");
    CHECK(capacitorId != 0);
    layout.plannedDevices.erase(capacitorId);
    std::erase_if(layout.plannedConnections, [capacitorId](const auto &entry) {
        return entry.second.from == capacitorId || entry.second.to == capacitorId;
    });

    const auto report = openvlsi::runLvs(fixture.project, fixture.schematic, fixture.layout);
    CHECK(!report.matched());
    CHECK(hasCategory(report, "missing-device"));
    CHECK(hasCategory(report, "extra-device"));
}

TEST_CASE("LVS compares device parameters on placed layout devices") {
    auto fixture = makeFixture();
    fixture.project.schematicCells.at(fixture.schematic)
        .instances.at(fixture.capacitor)
        .parameters.push_back({fixture.project.nextId(), "capacitance", "1u"});

    const auto deviceCell = openvlsi::createLayoutCell(fixture.project, "capacitor_cell");
    auto &device = fixture.project.layoutCells.at(deviceCell);
    device.library = true;
    device.device = "capacitor";
    device.parameters.push_back({fixture.project.nextId(), "capacitance", "2u"});
    openvlsi::addLayoutInstance(fixture.project, fixture.layout, deviceCell, "C1", 0, 0);

    const auto report = openvlsi::runLvs(fixture.project, fixture.schematic, fixture.layout);
    CHECK(!report.matched());
    CHECK(hasCategory(report, "parameter"));
    CHECK(!hasCategory(report, "missing-device"));
    CHECK(!hasCategory(report, "extra-device"));
}

TEST_CASE("LVS compares layout net topology") {
    auto fixture = makeFixture();
    auto &layout = fixture.project.layoutCells.at(fixture.layout);
    CHECK(!layout.plannedConnections.empty());
    for (auto &[_, device] : layout.plannedDevices)
        device.terminals.clear();
    layout.plannedConnections.erase(layout.plannedConnections.begin());

    const auto report = openvlsi::runLvs(fixture.project, fixture.schematic, fixture.layout);
    CHECK(!report.matched());
    CHECK(hasCategory(report, "missing-connection"));
}

TEST_CASE("LVS warns when layout connectivity metadata is missing") {
    auto fixture = makeFixture();
    const auto layout = openvlsi::createLayoutCell(fixture.project, "top_without_connections");
    for (const auto &[_, planned] : fixture.project.layoutCells.at(fixture.layout).plannedDevices) {
        const auto id = fixture.project.nextId();
        fixture.project.layoutCells.at(layout).plannedDevices.emplace(
            id, openvlsi::PlannedDevice{id, planned.name, planned.schematicInstance, planned.x, planned.y,
                                        planned.width, planned.height});
    }

    const auto report = openvlsi::runLvs(fixture.project, fixture.schematic, layout);
    CHECK(!report.matched());
    CHECK(hasCategory(report, "layout-netlist"));
    CHECK(!hasCategory(report, "missing-device"));
}

TEST_CASE("LVS uses planned device model and terminal metadata") {
    auto fixture = makeFixture();
    auto &layout = fixture.project.layoutCells.at(fixture.layout);
    layout.plannedConnections.clear();
    openvlsi::ObjectId inNet = 0;
    openvlsi::ObjectId outNet = 0;
    for (const auto &[id, net] : layout.nets) {
        if (net.name == "in")
            inNet = id;
        if (net.name == "out")
            outNet = id;
    }
    CHECK(inNet != 0);
    CHECK(outNet != 0);
    if (inNet == 0 || outNet == 0)
        return;
    for (auto &[_, device] : layout.plannedDevices) {
        device.model = device.name == "R1" ? "resistor" : "capacitor";
        device.terminals["A"] = inNet;
        device.terminals["B"] = outNet;
    }
    const auto report = openvlsi::runLvs(fixture.project, fixture.schematic, fixture.layout);
    CHECK(report.matched());
    CHECK(report.mismatches.empty());
}

TEST_CASE("LVS compares text SPICE netlists and rejects read-only layout cells") {
    auto fixture = makeFixture();
    const auto text = openvlsi::createTextSchematicCell(fixture.project, "top_text", openvlsi::genericPdkId,
                                                        "R1 in out 1k\nC1 in out 1n\n");
    const auto report = openvlsi::runLvs(fixture.project, text, fixture.layout);
    CHECK(report.matched());
    CHECK(report.mismatches.empty());

    const auto drawn = openvlsi::createSchematicCell(fixture.project, "drawn");
    fixture.project.layoutCells.at(fixture.layout).library = true;
    CHECK_THROWS_AS(openvlsi::runLvs(fixture.project, drawn, fixture.layout), openvlsi::ProjectError);
}

TEST_CASE("LVS matches text X MOSFETs and PDK passive parameter names") {
    openvlsi::Project project;
    project.identity = "lvs-text-pdk";
    project.name = "lvs-text-pdk";
    const auto layout = openvlsi::createLayoutCell(project, "top");
    const auto inNet = openvlsi::createLayoutNet(project, layout, "in");
    const auto outNet = openvlsi::createLayoutNet(project, layout, "out");
    const auto groundNet = openvlsi::createLayoutNet(project, layout, "0");
    auto &layoutCell = project.layoutCells.at(layout);

    openvlsi::PlannedDevice resistor;
    resistor.id = project.nextId();
    resistor.name = "R1";
    resistor.model = "res_rhigh";
    resistor.parameters.push_back({project.nextId(), "R", "1k"});
    resistor.terminals = {{"PLUS", inNet}, {"MINUS", outNet}};
    resistor.placeholder = false;
    layoutCell.plannedDevices.emplace(resistor.id, resistor);

    openvlsi::PlannedDevice capacitor;
    capacitor.id = project.nextId();
    capacitor.name = "C1";
    capacitor.model = "cap_cmim";
    capacitor.parameters.push_back({project.nextId(), "C", "1n"});
    capacitor.terminals = {{"PLUS", outNet}, {"MINUS", groundNet}};
    capacitor.placeholder = false;
    layoutCell.plannedDevices.emplace(capacitor.id, capacitor);

    openvlsi::PlannedDevice transistor;
    transistor.id = project.nextId();
    transistor.name = "X1";
    transistor.model = "sg13_lv_nmos";
    transistor.parameters.push_back({project.nextId(), "w", "1u"});
    transistor.parameters.push_back({project.nextId(), "l", "0.13u"});
    transistor.terminals = {{"D", outNet}, {"G", inNet}, {"S", groundNet}, {"B", groundNet}};
    transistor.placeholder = false;
    layoutCell.plannedDevices.emplace(transistor.id, transistor);

    const auto schematic =
        openvlsi::createTextSchematicCell(project, "text_top", openvlsi::genericPdkId,
                                          "R1 in out 1k\nC1 out 0 1n\nX1 out in 0 0 sg13_lv_nmos w=1u l=0.13u\n");
    const auto report = openvlsi::runLvs(project, schematic, layout);
    CHECK(report.matched());
    CHECK(report.mismatches.empty());
}

TEST_CASE("LVS compares device parameters from a text SPICE netlist") {
    auto fixture = makeFixture();
    for (auto &[_, device] : fixture.project.layoutCells.at(fixture.layout).plannedDevices) {
        if (device.name == "R1")
            device.parameters.push_back({fixture.project.nextId(), "resistance", "2k"});
    }
    const auto matching = openvlsi::createTextSchematicCell(fixture.project, "spice_params", openvlsi::genericPdkId,
                                                            "R1 in out 2k\nC1 in out 1n\n");
    CHECK(openvlsi::runLvs(fixture.project, matching, fixture.layout).matched());

    auto &text = fixture.project.schematicCells.at(matching);
    text.spiceText = "R1 in out 3k\nC1 in out 1n\n";
    const auto mismatch = openvlsi::runLvs(fixture.project, matching, fixture.layout);
    CHECK(!mismatch.matched());
    CHECK(hasCategory(mismatch, "parameter"));
}

TEST_CASE("LVS resolves numeric SPICE parameter expressions") {
    openvlsi::Project project;
    project.identity = "lvs-expression";
    project.name = "lvs-expression";
    const auto layout = openvlsi::createLayoutCell(project, "top");
    const auto inNet = openvlsi::createLayoutNet(project, layout, "in");
    const auto outNet = openvlsi::createLayoutNet(project, layout, "out");
    auto &layoutCell = project.layoutCells.at(layout);
    openvlsi::PlannedDevice resistor;
    resistor.id = project.nextId();
    resistor.name = "R1";
    resistor.device = "resistor";
    resistor.parameters.push_back({project.nextId(), "resistance", "2k"});
    resistor.terminals = {{"A", inNet}, {"B", outNet}};
    resistor.placeholder = false;
    layoutCell.plannedDevices.emplace(resistor.id, resistor);

    const auto schematic = openvlsi::createTextSchematicCell(project, "expression", openvlsi::genericPdkId,
                                                             ".param BASE=1k\nR1 IN OUT {BASE*2}\n");
    const auto report = openvlsi::runLvs(project, schematic, layout);
    CHECK(report.matched());
    CHECK(!hasCategory(report, "parameter"));
}

TEST_CASE("LVS distinguishes multi-terminal pin assignments") {
    openvlsi::Project project;
    project.identity = "lvs-terminal";
    project.name = "lvs-terminal";
    const auto layout = openvlsi::createLayoutCell(project, "top");
    const auto drainNet = openvlsi::createLayoutNet(project, layout, "d");
    const auto gateNet = openvlsi::createLayoutNet(project, layout, "g");
    const auto sourceNet = openvlsi::createLayoutNet(project, layout, "s");
    const auto bulkNet = openvlsi::createLayoutNet(project, layout, "b");
    auto &layoutCell = project.layoutCells.at(layout);
    openvlsi::PlannedDevice transistor;
    transistor.id = project.nextId();
    transistor.name = "M1";
    transistor.device = "nmos";
    transistor.terminals = {{"D", drainNet}, {"G", gateNet}, {"S", sourceNet}, {"B", bulkNet}};
    transistor.placeholder = false;
    layoutCell.plannedDevices.emplace(transistor.id, transistor);

    openvlsi::PlannedDevice second;
    second.id = project.nextId();
    second.name = "M2";
    second.device = "nmos";
    second.terminals = {{"D", drainNet}, {"G", gateNet}, {"S", sourceNet}, {"B", bulkNet}};
    second.placeholder = false;
    layoutCell.plannedDevices.emplace(second.id, second);

    const auto schematic = openvlsi::createTextSchematicCell(project, "terminal_top", openvlsi::genericPdkId,
                                                             "M1 D G S B nmos\nM2 D G S B nmos\n");
    CHECK(openvlsi::runLvs(project, schematic, layout).matched());

    auto &planned = project.layoutCells.at(layout).plannedDevices.at(transistor.id);
    std::swap(planned.terminals["D"], planned.terminals["G"]);
    const auto swapped = openvlsi::runLvs(project, schematic, layout);
    CHECK(!swapped.matched());
    CHECK(hasCategory(swapped, "missing-connection"));
    CHECK(hasCategory(swapped, "extra-connection"));
}

TEST_CASE("LVS reports schematic parameters missing from the layout") {
    auto fixture = makeFixture();
    for (auto &[_, device] : fixture.project.layoutCells.at(fixture.layout).plannedDevices)
        device.parameters.clear();
    const auto report = openvlsi::runLvs(fixture.project, fixture.schematic, fixture.layout);
    CHECK(!report.matched());
    CHECK(hasCategory(report, "parameter-unavailable"));
}

TEST_CASE("LVS expands multiplicity and matches renamed devices by topology") {
    openvlsi::Project project;
    project.identity = "lvs-multiplicity";
    project.name = "lvs-multiplicity";
    const auto layout = openvlsi::createLayoutCell(project, "top");
    const auto inNet = openvlsi::createLayoutNet(project, layout, "in");
    const auto outNet = openvlsi::createLayoutNet(project, layout, "out");
    auto &cell = project.layoutCells.at(layout);
    for (int index = 1; index <= 4; ++index) {
        openvlsi::PlannedDevice resistor;
        resistor.id = project.nextId();
        resistor.name = "R" + std::to_string(index);
        resistor.device = "resistor";
        resistor.parameters.push_back({project.nextId(), "resistance", "1k"});
        resistor.terminals = {{"A", inNet}, {"B", outNet}};
        resistor.placeholder = false;
        cell.plannedDevices.emplace(resistor.id, resistor);
    }
    const auto spice =
        openvlsi::createTextSchematicCell(project, "top_spice", openvlsi::genericPdkId, "R1 in out 1k m=4\n");
    const auto report = openvlsi::runLvs(project, spice, layout);
    CHECK(report.matched());
    CHECK(std::none_of(report.mismatches.begin(), report.mismatches.end(), [](const openvlsi::LvsMismatch &mismatch) {
        return mismatch.category == "missing-device" || mismatch.category == "extra-device";
    }));

    cell.plannedDevices.clear();
    openvlsi::PlannedDevice renamed;
    renamed.id = project.nextId();
    renamed.name = "RA";
    renamed.device = "resistor";
    renamed.parameters.push_back({project.nextId(), "resistance", "1k"});
    renamed.terminals = {{"A", inNet}, {"B", outNet}};
    renamed.placeholder = false;
    cell.plannedDevices.emplace(renamed.id, renamed);
    const auto simpleSpice =
        openvlsi::createTextSchematicCell(project, "renamed_spice", openvlsi::genericPdkId, "R1 in out 1k\n");
    const auto renamedReport = openvlsi::runLvs(project, simpleSpice, layout);
    CHECK(renamedReport.matched());
    CHECK(hasCategory(renamedReport, "device-name"));
    CHECK(!hasCategory(renamedReport, "missing-device"));
    CHECK(!hasCategory(renamedReport, "extra-device"));
}

TEST_CASE("LVS derives layout connectivity from geometry, not planned links") {
    openvlsi::Project project;
    project.identity = "lvs-geometry";
    project.name = "lvs-geometry";
    const auto layer = openvlsi::addLayer(project, "Metal1", 8);
    const auto child = openvlsi::createLayoutCell(project, "res_dev");
    auto &childCell = project.layoutCells.at(child);
    childCell.library = true;
    childCell.device = "resistor";
    openvlsi::addPort(project, child, "A", layer, {0, 0, 200, 200}, openvlsi::PortDirection::inout,
                      openvlsi::PortSide::left);
    openvlsi::addPort(project, child, "B", layer, {1000, 0, 200, 200}, openvlsi::PortDirection::inout,
                      openvlsi::PortSide::right);

    const auto layout = openvlsi::createLayoutCell(project, "top");
    openvlsi::addLayoutInstance(project, layout, child, "R1", 0, 0);
    openvlsi::addLayoutInstance(project, layout, child, "R2", 3000, 0);
    openvlsi::addRectangle(project, layout, layer, {1100, 0, 1900, 200});
    CHECK(project.layoutCells.at(layout).plannedConnections.empty());
    CHECK(project.layoutCells.at(layout).plannedDevices.empty());

    const auto schematic = openvlsi::createSchematicCell(project, "top");
    std::vector<openvlsi::ObjectId> r1Pins;
    std::vector<openvlsi::ObjectId> r2Pins;
    addTwoPinDevice(project, schematic, "resistor", "R1", r1Pins);
    addTwoPinDevice(project, schematic, "resistor", "R2", r2Pins);
    auto &drawn = project.schematicCells.at(schematic);
    const auto inputNet = project.nextId();
    drawn.nets.emplace(inputNet, openvlsi::Net{inputNet, "IN", {r1Pins[0]}});
    const auto middleNet = project.nextId();
    drawn.nets.emplace(middleNet, openvlsi::Net{middleNet, "MID", {r1Pins[1], r2Pins[0]}});
    const auto outputNet = project.nextId();
    drawn.nets.emplace(outputNet, openvlsi::Net{outputNet, "OUT", {r2Pins[1]}});

    const auto report = openvlsi::runLvs(project, schematic, layout);
    CHECK(report.matched());
}

TEST_CASE("LVS parses gate-level SystemVerilog and rejects behavioral modules") {
    openvlsi::Project project;
    project.identity = "lvs-gate-hdl";
    project.name = "lvs-gate-hdl";
    const auto source = openvlsi::createSystemVerilogCell(
        project, "gate", openvlsi::genericPdkId,
        "module gate(input logic a, input logic b, output logic y);\nand g1(y, a, b);\nendmodule\n");
    const auto layout = openvlsi::createLayoutCell(project, "gate_layout");
    const auto layer = openvlsi::addLayer(project, "Metal1", 10);
    const auto aNet = openvlsi::createLayoutNet(project, layout, "a");
    const auto bNet = openvlsi::createLayoutNet(project, layout, "b");
    const auto yNet = openvlsi::createLayoutNet(project, layout, "y");
    openvlsi::addPort(project, layout, "a", layer, {0, 0, 100, 100}, openvlsi::PortDirection::input,
                      openvlsi::PortSide::left, aNet, "a");
    openvlsi::addPort(project, layout, "b", layer, {0, 200, 100, 100}, openvlsi::PortDirection::input,
                      openvlsi::PortSide::left, bNet, "b");
    openvlsi::addPort(project, layout, "y", layer, {800, 100, 100, 100}, openvlsi::PortDirection::output,
                      openvlsi::PortSide::right, yNet, "y");
    auto &layoutCell = project.layoutCells.at(layout);
    openvlsi::PlannedDevice gate;
    gate.id = project.nextId();
    gate.name = "g1";
    gate.device = "and";
    gate.terminals = {{"Y", yNet}, {"A", aNet}, {"B", bNet}};
    gate.placeholder = false;
    layoutCell.plannedDevices.emplace(gate.id, gate);

    auto report = openvlsi::runLvs(project, source, layout);
    CHECK(report.matched());
    CHECK(report.portsMatched == 3);

    layoutCell.plannedDevices.at(gate.id).device = "or";
    report = openvlsi::runLvs(project, source, layout);
    CHECK(!report.matched());
    CHECK(hasCategory(report, "device-type"));

    const auto behavioral = openvlsi::createSystemVerilogCell(
        project, "behavioral_gate", openvlsi::genericPdkId,
        "module behavioral_gate(input logic a, input logic b, output logic y);\nalways_comb y = a & b;\nendmodule\n");
    CHECK_THROWS_AS(openvlsi::runLvs(project, behavioral, layout), openvlsi::ProjectError);
}

TEST_CASE("LVS flattens text SPICE subcircuits and compares top-level ports") {
    openvlsi::Project project;
    project.identity = "lvs-spice";
    project.name = "lvs-spice";
    const auto drawn = openvlsi::createSchematicCell(project, "filter_drawn");
    std::vector<openvlsi::ObjectId> rPins;
    std::vector<openvlsi::ObjectId> cPins;
    const auto r1 = addTwoPinDevice(project, drawn, "resistor", "R1", rPins);
    const auto c1 = addTwoPinDevice(project, drawn, "capacitor", "C1", cPins);
    project.schematicCells.at(drawn).instances.at(r1).parameters.push_back({project.nextId(), "resistance", "1k"});
    project.schematicCells.at(drawn).instances.at(c1).parameters.push_back({project.nextId(), "capacitance", "1n"});
    auto &drawnCell = project.schematicCells.at(drawn);
    const auto inNetId = project.nextId();
    drawnCell.nets.emplace(inNetId, openvlsi::Net{inNetId, "IN", {rPins[0]}});
    const auto midNetId = project.nextId();
    drawnCell.nets.emplace(midNetId, openvlsi::Net{midNetId, "MID", {rPins[1], cPins[0]}});
    const auto outNetId = project.nextId();
    drawnCell.nets.emplace(outNetId, openvlsi::Net{outNetId, "OUT", {cPins[1]}});
    const auto layout = openvlsi::createLayoutFromSchematic(project, drawn, "filter_layout");
    const auto layer = openvlsi::addLayer(project, "Metal1", 10);
    const auto inNet = openvlsi::createLayoutNet(project, layout, "IN");
    const auto outNet = openvlsi::createLayoutNet(project, layout, "OUT");
    openvlsi::addPort(project, layout, "IN", layer, {0, 0, 100, 100}, openvlsi::PortDirection::input,
                      openvlsi::PortSide::left, inNet, "IN");
    openvlsi::addPort(project, layout, "OUT", layer, {0, 0, 100, 100}, openvlsi::PortDirection::output,
                      openvlsi::PortSide::right, outNet, "OUT");

    const auto text =
        openvlsi::createTextSchematicCell(project, "filter", openvlsi::genericPdkId,
                                          ".subckt filter IN OUT\nR1 IN MID 1k\nC1 MID OUT 1n\n.ends filter\n");
    const auto report = openvlsi::runLvs(project, text, layout);
    CHECK(report.matched());
    CHECK(report.portsMatched == 2);
    CHECK(report.mismatches.empty());
}

TEST_CASE("LVS resolves SPICE includes against the source file") {
    openvlsi::Project project;
    project.identity = "lvs-spice-include";
    project.name = "lvs-spice-include";
    const auto drawn = openvlsi::createSchematicCell(project, "filter_drawn");
    std::vector<openvlsi::ObjectId> rPins;
    std::vector<openvlsi::ObjectId> cPins;
    const auto r1 = addTwoPinDevice(project, drawn, "resistor", "R1", rPins);
    const auto c1 = addTwoPinDevice(project, drawn, "capacitor", "C1", cPins);
    project.schematicCells.at(drawn).instances.at(r1).parameters.push_back({project.nextId(), "resistance", "1k"});
    project.schematicCells.at(drawn).instances.at(c1).parameters.push_back({project.nextId(), "capacitance", "1n"});
    auto &drawnCell = project.schematicCells.at(drawn);
    const auto inNetId = project.nextId();
    drawnCell.nets.emplace(inNetId, openvlsi::Net{inNetId, "IN", {rPins[0]}});
    const auto midNetId = project.nextId();
    drawnCell.nets.emplace(midNetId, openvlsi::Net{midNetId, "MID", {rPins[1], cPins[0]}});
    const auto outNetId = project.nextId();
    drawnCell.nets.emplace(outNetId, openvlsi::Net{outNetId, "OUT", {cPins[1]}});
    const auto layout = openvlsi::createLayoutFromSchematic(project, drawn, "filter_layout");
    const auto layer = openvlsi::addLayer(project, "Metal1", 10);
    const auto inNet = openvlsi::createLayoutNet(project, layout, "IN");
    const auto outNet = openvlsi::createLayoutNet(project, layout, "OUT");
    openvlsi::addPort(project, layout, "IN", layer, {0, 0, 100, 100}, openvlsi::PortDirection::input,
                      openvlsi::PortSide::left, inNet, "IN");
    openvlsi::addPort(project, layout, "OUT", layer, {0, 0, 100, 100}, openvlsi::PortDirection::output,
                      openvlsi::PortSide::right, outNet, "OUT");

    const auto root = std::filesystem::temp_directory_path() / "openvlsi-lvs-include-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    std::ofstream(root / "filter.inc") << ".subckt filter IN OUT\nR1 IN MID 1k\nC1 MID OUT 1n\n.ends filter\n";
    const auto text = openvlsi::createTextSchematicCell(project, "filter", openvlsi::genericPdkId,
                                                        ".include \"filter.inc\"\n", (root / "top.sp").string());
    auto report = openvlsi::runLvs(project, text, layout);
    CHECK(report.matched());
    CHECK(report.portsMatched == 2);

    std::filesystem::remove(root / "filter.inc");
    report = openvlsi::runLvs(project, text, layout);
    CHECK(!report.matched());
    CHECK(hasCategory(report, "spice-include"));
    std::filesystem::remove_all(root);
}

TEST_CASE("LVS flattens nested text SPICE subcircuit instances") {
    openvlsi::Project project;
    project.identity = "lvs-spice-nested";
    project.name = "lvs-spice-nested";
    const auto drawn = openvlsi::createSchematicCell(project, "nested_drawn");
    std::vector<openvlsi::ObjectId> r1Pins;
    std::vector<openvlsi::ObjectId> r2Pins;
    const auto r1 = addTwoPinDevice(project, drawn, "resistor", "R1", r1Pins);
    const auto r2 = addTwoPinDevice(project, drawn, "resistor", "R2", r2Pins);
    project.schematicCells.at(drawn).instances.at(r1).parameters.push_back({project.nextId(), "resistance", "1k"});
    project.schematicCells.at(drawn).instances.at(r2).parameters.push_back({project.nextId(), "resistance", "1k"});
    auto &drawnCell = project.schematicCells.at(drawn);
    const auto inNet = project.nextId();
    drawnCell.nets.emplace(inNet, openvlsi::Net{inNet, "IN", {r1Pins[0]}});
    const auto midNet = project.nextId();
    drawnCell.nets.emplace(midNet, openvlsi::Net{midNet, "MID", {r1Pins[1], r2Pins[0]}});
    const auto outNet = project.nextId();
    drawnCell.nets.emplace(outNet, openvlsi::Net{outNet, "OUT", {r2Pins[1]}});
    const auto layout = openvlsi::createLayoutFromSchematic(project, drawn, "nested_layout");

    const auto text = openvlsi::createTextSchematicCell(project, "nested_top", openvlsi::genericPdkId,
                                                        ".subckt leaf_a A B\nR1 A B 1k\n.ends leaf_a\n"
                                                        ".subckt leaf_b B C\nR2 B C 1k\n.ends leaf_b\n"
                                                        "X1 IN MID leaf_a\nX2 MID OUT leaf_b\n");
    const auto report = openvlsi::runLvs(project, text, layout);
    CHECK(report.matched());
    CHECK(report.mismatches.empty());
}

TEST_CASE("LVS merges SPICE global nets across subcircuit instances") {
    openvlsi::Project project;
    project.identity = "lvs-global";
    project.name = "lvs-global";
    const auto layer = openvlsi::addLayer(project, "Metal1", 12);
    const auto layout = openvlsi::createLayoutCell(project, "top");
    const auto inNet = openvlsi::createLayoutNet(project, layout, "IN");
    const auto midNet = openvlsi::createLayoutNet(project, layout, "MID");
    const auto outNet = openvlsi::createLayoutNet(project, layout, "OUT");
    const auto vddNet = openvlsi::createLayoutNet(project, layout, "VDD");
    openvlsi::addPort(project, layout, "IN", layer, {0, 0, 100, 100}, openvlsi::PortDirection::input,
                      openvlsi::PortSide::left, inNet, "IN");
    openvlsi::addPort(project, layout, "OUT", layer, {900, 0, 100, 100}, openvlsi::PortDirection::output,
                      openvlsi::PortSide::right, outNet, "OUT");
    openvlsi::addPort(project, layout, "MID", layer, {450, 0, 100, 100}, openvlsi::PortDirection::inout,
                      openvlsi::PortSide::left, midNet, "MID");
    struct Planned {
        const char *name;
        const char *value;
        openvlsi::ObjectId a;
        openvlsi::ObjectId b;
    };
    const Planned planned[] = {{"X1_R1", "1k", inNet, vddNet},
                               {"X1_R2", "2k", midNet, vddNet},
                               {"X2_R1", "1k", midNet, vddNet},
                               {"X2_R2", "2k", outNet, vddNet}};
    for (const auto &entry : planned) {
        openvlsi::PlannedDevice device;
        device.id = project.nextId();
        device.name = entry.name;
        device.device = "resistor";
        device.parameters.push_back({project.nextId(), "resistance", entry.value});
        device.terminals = {{"A", entry.a}, {"B", entry.b}};
        device.placeholder = false;
        project.layoutCells.at(layout).plannedDevices.emplace(device.id, device);
    }
    const auto schematic = openvlsi::createTextSchematicCell(project, "top", openvlsi::genericPdkId,
                                                             ".global VDD\n"
                                                             ".subckt leaf A B\n"
                                                             "R1 A VDD 1k\n"
                                                             "R2 B VDD 2k\n"
                                                             ".ends leaf\n"
                                                             ".subckt top IN MID OUT\n"
                                                             "X1 IN MID leaf\n"
                                                             "X2 MID OUT leaf\n"
                                                             ".ends top\n");
    const auto report = openvlsi::runLvs(project, schematic, layout);
    CHECK(report.matched());
}

TEST_CASE("LVS compares SystemVerilog module ports") {
    openvlsi::Project project;
    project.identity = "lvs-hdl";
    project.name = "lvs-hdl";
    const auto source = openvlsi::createSystemVerilogCell(
        project, "buffer", openvlsi::genericPdkId,
        "module buffer(input logic in, output logic out);\nassign out = in;\nendmodule\n");
    const auto layout = openvlsi::createLayoutCell(project, "buffer_layout");
    const auto layer = openvlsi::addLayer(project, "Metal1", 10);
    const auto inNet = openvlsi::createLayoutNet(project, layout, "in");
    const auto outNet = openvlsi::createLayoutNet(project, layout, "out");
    openvlsi::addPort(project, layout, "in", layer, {0, 0, 100, 100}, openvlsi::PortDirection::input,
                      openvlsi::PortSide::left, inNet, "in");
    openvlsi::addPort(project, layout, "out", layer, {0, 0, 100, 100}, openvlsi::PortDirection::output,
                      openvlsi::PortSide::right, outNet, "out");
    const auto report = openvlsi::runLvs(project, source, layout);
    CHECK(report.matched());
    CHECK(report.portsMatched == 2);
    CHECK(report.mismatches.empty());
}

namespace {
openvlsi::ObjectId addSchematicPort(openvlsi::Project &project, openvlsi::ObjectId cell, const std::string &type,
                                    const std::string &name, const std::string &netName) {
    const auto symbol = openvlsi::addSymbol(project, cell, type);
    auto &symbolEntry = project.schematicCells.at(cell).symbols.at(symbol);
    const auto pin = project.nextId();
    symbolEntry.pins.push_back({pin, "P", 0, 0});
    openvlsi::addSchematicInstance(project, cell, symbol, name, 0, 0);
    for (auto &[_, net] : project.schematicCells.at(cell).nets) {
        if (net.name == netName) {
            net.pins.push_back(pin);
            return pin;
        }
    }
    return 0;
}

openvlsi::ObjectId layoutNetNamed(const openvlsi::Project &project, openvlsi::ObjectId cell, const std::string &name) {
    for (const auto &[id, net] : project.layoutCells.at(cell).nets) {
        if (net.name == name)
            return id;
    }
    return 0;
}

openvlsi::ObjectId schematicInstanceNamed(const openvlsi::Project &project, openvlsi::ObjectId cell,
                                          const std::string &name) {
    for (const auto &[id, instance] : project.schematicCells.at(cell).instances) {
        if (instance.name == name)
            return id;
    }
    return 0;
}
} // namespace

TEST_CASE("LVS matches top-level schematic ports to layout ports") {
    auto fixture = makeFixture();
    const auto inputPin = addSchematicPort(fixture.project, fixture.schematic, "Input", "A", "in");
    const auto outputPin = addSchematicPort(fixture.project, fixture.schematic, "Output", "B", "out");
    CHECK(inputPin != 0);
    CHECK(outputPin != 0);

    const auto layer = openvlsi::addLayer(fixture.project, "Metal1", 8);
    const auto inNet = layoutNetNamed(fixture.project, fixture.layout, "in");
    const auto outNet = layoutNetNamed(fixture.project, fixture.layout, "out");
    CHECK(inNet != 0);
    CHECK(outNet != 0);
    if (inNet == 0 || outNet == 0)
        return;
    const auto input = openvlsi::addPort(fixture.project, fixture.layout, "A", layer, {0, 0, 400, 400},
                                         openvlsi::PortDirection::input, openvlsi::PortSide::left, inNet, "A");
    const auto output = openvlsi::addPort(fixture.project, fixture.layout, "B", layer, {800, 0, 400, 400},
                                          openvlsi::PortDirection::output, openvlsi::PortSide::right, outNet, "B");
    CHECK(input != 0);

    // Ports produced by Convert to Pin carry the schematic-instance link.
    const auto inputInstance = schematicInstanceNamed(fixture.project, fixture.schematic, "A");
    const auto outputInstance = schematicInstanceNamed(fixture.project, fixture.schematic, "B");
    CHECK(inputInstance != 0);
    CHECK(outputInstance != 0);
    fixture.project.layoutCells.at(fixture.layout).ports.at(input).schematicInstance = inputInstance;
    fixture.project.layoutCells.at(fixture.layout).ports.at(output).schematicInstance = outputInstance;

    auto report = openvlsi::runLvs(fixture.project, fixture.schematic, fixture.layout);
    CHECK(report.matched());
    CHECK(report.portsMatched == 2);
    CHECK(!hasCategory(report, "missing-port"));
    CHECK(!hasCategory(report, "extra-port"));
    CHECK(!hasCategory(report, "port-direction"));
    CHECK(!hasCategory(report, "port-net"));
    CHECK(openvlsi::serializeLvsReport(report).find("\"ports_matched\": 2") != std::string::npos);

    auto &outputPort = fixture.project.layoutCells.at(fixture.layout).ports.at(output);
    outputPort.net = inNet;
    report = openvlsi::runLvs(fixture.project, fixture.schematic, fixture.layout);
    CHECK(!report.matched());
    CHECK(hasCategory(report, "port-net"));

    outputPort.net = outNet;
    outputPort.direction = openvlsi::PortDirection::input;
    report = openvlsi::runLvs(fixture.project, fixture.schematic, fixture.layout);
    CHECK(!report.matched());
    CHECK(hasCategory(report, "port-direction"));

    outputPort.direction = openvlsi::PortDirection::output;
    fixture.project.layoutCells.at(fixture.layout).ports.erase(output);
    report = openvlsi::runLvs(fixture.project, fixture.schematic, fixture.layout);
    CHECK(!report.matched());
    CHECK(hasCategory(report, "missing-port"));

    const auto extra = openvlsi::addPort(fixture.project, fixture.layout, "C", layer, {0, 800, 400, 400},
                                         openvlsi::PortDirection::inout, openvlsi::PortSide::top, 0, "C");
    CHECK(extra != 0);
    report = openvlsi::runLvs(fixture.project, fixture.schematic, fixture.layout);
    CHECK(hasCategory(report, "missing-port"));
    CHECK(hasCategory(report, "extra-port"));
}

TEST_CASE("LVS reports port nets that cannot be attributed") {
    auto fixture = makeFixture();

    // Two ports that share one symbol cannot be told apart: their nets reference
    // the same symbol pin id.
    const auto shared = openvlsi::addSymbol(fixture.project, fixture.schematic, "Input");
    auto &cell = fixture.project.schematicCells.at(fixture.schematic);
    auto &symbol = cell.symbols.at(shared);
    const auto pin = fixture.project.nextId();
    symbol.pins.push_back({pin, "P", 0, 0});
    // Projects written before instances cloned their symbols on placement can
    // still contain two instances of one symbol; build that state directly.
    for (const auto *name : {"A", "B"}) {
        const auto instanceId = fixture.project.nextId();
        cell.instances.emplace(instanceId, openvlsi::SchematicInstance{instanceId, shared, name, 0, 0});
    }
    for (auto &[_, net] : cell.nets)
        net.pins.push_back(pin);

    auto report = openvlsi::runLvs(fixture.project, fixture.schematic, fixture.layout);
    CHECK(hasCategory(report, "port-net-ambiguous"));

    // Distinct symbols per port resolve cleanly.
    auto second = makeFixture();
    const auto firstSymbol = openvlsi::addSymbol(second.project, second.schematic, "Input");
    auto &firstEntry = second.project.schematicCells.at(second.schematic).symbols.at(firstSymbol);
    const auto firstPin = second.project.nextId();
    firstEntry.pins.push_back({firstPin, "P", 0, 0});
    openvlsi::addSchematicInstance(second.project, second.schematic, firstSymbol, "A", 0, 0);
    if (!second.project.schematicCells.at(second.schematic).nets.empty())
        second.project.schematicCells.at(second.schematic).nets.begin()->second.pins.push_back(firstPin);
    report = openvlsi::runLvs(second.project, second.schematic, second.layout);
    CHECK(!hasCategory(report, "port-net-ambiguous"));
}
