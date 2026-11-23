#include "icelib/core.hpp"
#include "icelib/operations.hpp"
#include "icelib/verification.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {
struct LvsFixture {
    ice::Project project;
    ice::ObjectId schematic = 0;
    ice::ObjectId layout = 0;
    ice::ObjectId resistor = 0;
    ice::ObjectId capacitor = 0;
    std::vector<ice::ObjectId> resistorPins;
    std::vector<ice::ObjectId> capacitorPins;
};

ice::ObjectId addTwoPinDevice(ice::Project &project, ice::ObjectId cell, const std::string &type,
                              const std::string &name, std::vector<ice::ObjectId> &pins) {
    const auto symbol = ice::addSymbol(project, cell, type);
    auto &symbolEntry = project.schematicCells.at(cell).symbols.at(symbol);
    pins.clear();
    for (const char *pinName : {"A", "B"}) {
        const auto pin = project.nextId();
        symbolEntry.pins.push_back({pin, pinName, 0, 0});
        pins.push_back(pin);
    }
    return ice::addSchematicInstance(project, cell, symbol, name, 0, 0);
}

LvsFixture makeFixture() {
    LvsFixture fixture;
    fixture.project.identity = "lvs-demo";
    fixture.project.name = "lvs-demo";
    fixture.schematic = ice::createSchematicCell(fixture.project, "top");
    fixture.resistor = addTwoPinDevice(fixture.project, fixture.schematic, "resistor", "R1", fixture.resistorPins);
    fixture.capacitor = addTwoPinDevice(fixture.project, fixture.schematic, "capacitor", "C1", fixture.capacitorPins);
    fixture.project.schematicCells.at(fixture.schematic)
        .instances.at(fixture.resistor)
        .parameters.push_back({fixture.project.nextId(), "resistance", "1k"});
    fixture.project.schematicCells.at(fixture.schematic)
        .instances.at(fixture.capacitor)
        .parameters.push_back({fixture.project.nextId(), "capacitance", "1n"});
    ice::addNet(fixture.project, fixture.schematic, "in", {fixture.resistorPins[0], fixture.capacitorPins[0]});
    ice::addNet(fixture.project, fixture.schematic, "out", {fixture.resistorPins[1], fixture.capacitorPins[1]});
    fixture.layout = ice::createLayoutFromSchematic(fixture.project, fixture.schematic);
    return fixture;
}

bool hasCategory(const ice::LvsReport &report, const std::string &category) {
    return std::any_of(report.mismatches.begin(), report.mismatches.end(),
                       [&](const ice::LvsMismatch &mismatch) { return mismatch.category == category; });
}

ice::ObjectId plannedDeviceId(const ice::Project &project, ice::ObjectId layout, const std::string &name) {
    for (const auto &[id, device] : project.layoutCells.at(layout).plannedDevices) {
        if (device.name == name)
            return id;
    }
    return 0;
}
} // namespace

TEST_CASE("LVS matches a layout created from its schematic") {
    auto fixture = makeFixture();
    const auto report = ice::runLvs(fixture.project, fixture.schematic, fixture.layout);
    CHECK(report.matched());
    CHECK(report.mismatches.empty());
    CHECK(report.schematic == "top");
    CHECK(report.layout == "top");
    CHECK(ice::schematicForLayout(fixture.project, fixture.layout) == fixture.schematic);
    CHECK(ice::serializeLvsReport(report).find("\"matched\": true") != std::string::npos);
}

TEST_CASE("LVS schematic inference prefers a DUT view") {
    auto fixture = makeFixture();
    const auto layout = ice::createLayoutCell(fixture.project, "block");
    const auto dut = ice::createSchematicCell(fixture.project, "block_lvs");
    ice::createSchematicCell(fixture.project, "block");
    CHECK(ice::schematicForLayout(fixture.project, layout) == dut);
}

TEST_CASE("LVS reports missing and extra devices") {
    auto fixture = makeFixture();
    auto &layout = fixture.project.layoutCells.at(fixture.layout);
    const auto extraId = fixture.project.nextId();
    layout.plannedDevices.emplace(extraId, ice::PlannedDevice{extraId, "X1", 0, 0, 0, 1200, 700});

    const auto capacitorId = plannedDeviceId(fixture.project, fixture.layout, "C1");
    CHECK(capacitorId != 0);
    layout.plannedDevices.erase(capacitorId);
    std::erase_if(layout.plannedConnections, [capacitorId](const auto &entry) {
        return entry.second.from == capacitorId || entry.second.to == capacitorId;
    });

    const auto report = ice::runLvs(fixture.project, fixture.schematic, fixture.layout);
    CHECK(!report.matched());
    CHECK(hasCategory(report, "missing-device"));
    CHECK(hasCategory(report, "extra-device"));
}

TEST_CASE("LVS compares device parameters on placed layout devices") {
    auto fixture = makeFixture();
    fixture.project.schematicCells.at(fixture.schematic)
        .instances.at(fixture.capacitor)
        .parameters.push_back({fixture.project.nextId(), "capacitance", "1u"});

    const auto deviceCell = ice::createLayoutCell(fixture.project, "capacitor_cell");
    auto &device = fixture.project.layoutCells.at(deviceCell);
    device.library = true;
    device.device = "capacitor";
    device.parameters.push_back({fixture.project.nextId(), "capacitance", "2u"});
    ice::addLayoutInstance(fixture.project, fixture.layout, deviceCell, "C1", 0, 0);

    const auto report = ice::runLvs(fixture.project, fixture.schematic, fixture.layout);
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

    const auto report = ice::runLvs(fixture.project, fixture.schematic, fixture.layout);
    CHECK(!report.matched());
    CHECK(hasCategory(report, "missing-connection"));
}

TEST_CASE("LVS warns when layout connectivity metadata is missing") {
    auto fixture = makeFixture();
    const auto layout = ice::createLayoutCell(fixture.project, "top_without_connections");
    for (const auto &[_, planned] : fixture.project.layoutCells.at(fixture.layout).plannedDevices) {
        const auto id = fixture.project.nextId();
        fixture.project.layoutCells.at(layout).plannedDevices.emplace(
            id, ice::PlannedDevice{id, planned.name, planned.schematicInstance, planned.x, planned.y, planned.width,
                                   planned.height});
    }

    const auto report = ice::runLvs(fixture.project, fixture.schematic, layout);
    CHECK(!report.matched());
    CHECK(hasCategory(report, "layout-netlist"));
    CHECK(!hasCategory(report, "missing-device"));
}

TEST_CASE("LVS uses planned device model and terminal metadata") {
    auto fixture = makeFixture();
    auto &layout = fixture.project.layoutCells.at(fixture.layout);
    layout.plannedConnections.clear();
    ice::ObjectId inNet = 0;
    ice::ObjectId outNet = 0;
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
    const auto report = ice::runLvs(fixture.project, fixture.schematic, fixture.layout);
    CHECK(report.matched());
    CHECK(report.mismatches.empty());
}

TEST_CASE("LVS compares text SPICE netlists and rejects read-only layout cells") {
    auto fixture = makeFixture();
    const auto text =
        ice::createTextSchematicCell(fixture.project, "top_text", ice::genericPdkId, "R1 in out 1k\nC1 in out 1n\n");
    const auto report = ice::runLvs(fixture.project, text, fixture.layout);
    CHECK(report.matched());
    CHECK(report.mismatches.empty());

    const auto drawn = ice::createSchematicCell(fixture.project, "drawn");
    fixture.project.layoutCells.at(fixture.layout).library = true;
    CHECK_THROWS_AS(ice::runLvs(fixture.project, drawn, fixture.layout), ice::ProjectError);
}

TEST_CASE("LVS compares device parameters from a text SPICE netlist") {
    auto fixture = makeFixture();
    for (auto &[_, device] : fixture.project.layoutCells.at(fixture.layout).plannedDevices) {
        if (device.name == "R1")
            device.parameters.push_back({fixture.project.nextId(), "resistance", "2k"});
    }
    const auto matching = ice::createTextSchematicCell(fixture.project, "spice_params", ice::genericPdkId,
                                                       "R1 in out 2k\nC1 in out 1n\n");
    CHECK(ice::runLvs(fixture.project, matching, fixture.layout).matched());

    auto &text = fixture.project.schematicCells.at(matching);
    text.spiceText = "R1 in out 3k\nC1 in out 1n\n";
    const auto mismatch = ice::runLvs(fixture.project, matching, fixture.layout);
    CHECK(!mismatch.matched());
    CHECK(hasCategory(mismatch, "parameter"));
}

TEST_CASE("LVS resolves numeric SPICE parameter expressions") {
    ice::Project project;
    project.identity = "lvs-expression";
    project.name = "lvs-expression";
    const auto layout = ice::createLayoutCell(project, "top");
    const auto inNet = ice::createLayoutNet(project, layout, "in");
    const auto outNet = ice::createLayoutNet(project, layout, "out");
    auto &layoutCell = project.layoutCells.at(layout);
    ice::PlannedDevice resistor;
    resistor.id = project.nextId();
    resistor.name = "R1";
    resistor.device = "resistor";
    resistor.parameters.push_back({project.nextId(), "resistance", "2k"});
    resistor.terminals = {{"A", inNet}, {"B", outNet}};
    resistor.placeholder = false;
    layoutCell.plannedDevices.emplace(resistor.id, resistor);

    const auto schematic =
        ice::createTextSchematicCell(project, "expression", ice::genericPdkId, ".param BASE=1k\nR1 IN OUT {BASE*2}\n");
    const auto report = ice::runLvs(project, schematic, layout);
    CHECK(report.matched());
    CHECK(!hasCategory(report, "parameter"));
}

TEST_CASE("LVS distinguishes multi-terminal pin assignments") {
    ice::Project project;
    project.identity = "lvs-terminal";
    project.name = "lvs-terminal";
    const auto layout = ice::createLayoutCell(project, "top");
    const auto drainNet = ice::createLayoutNet(project, layout, "d");
    const auto gateNet = ice::createLayoutNet(project, layout, "g");
    const auto sourceNet = ice::createLayoutNet(project, layout, "s");
    const auto bulkNet = ice::createLayoutNet(project, layout, "b");
    auto &layoutCell = project.layoutCells.at(layout);
    ice::PlannedDevice transistor;
    transistor.id = project.nextId();
    transistor.name = "M1";
    transistor.device = "nmos";
    transistor.terminals = {{"D", drainNet}, {"G", gateNet}, {"S", sourceNet}, {"B", bulkNet}};
    transistor.placeholder = false;
    layoutCell.plannedDevices.emplace(transistor.id, transistor);

    ice::PlannedDevice second;
    second.id = project.nextId();
    second.name = "M2";
    second.device = "nmos";
    second.terminals = {{"D", drainNet}, {"G", gateNet}, {"S", sourceNet}, {"B", bulkNet}};
    second.placeholder = false;
    layoutCell.plannedDevices.emplace(second.id, second);

    const auto schematic =
        ice::createTextSchematicCell(project, "terminal_top", ice::genericPdkId, "M1 D G S B nmos\nM2 D G S B nmos\n");
    CHECK(ice::runLvs(project, schematic, layout).matched());

    auto &planned = project.layoutCells.at(layout).plannedDevices.at(transistor.id);
    std::swap(planned.terminals["D"], planned.terminals["G"]);
    const auto swapped = ice::runLvs(project, schematic, layout);
    CHECK(!swapped.matched());
    CHECK(hasCategory(swapped, "missing-connection"));
    CHECK(hasCategory(swapped, "extra-connection"));
}

TEST_CASE("LVS reports schematic parameters missing from the layout") {
    auto fixture = makeFixture();
    for (auto &[_, device] : fixture.project.layoutCells.at(fixture.layout).plannedDevices)
        device.parameters.clear();
    const auto report = ice::runLvs(fixture.project, fixture.schematic, fixture.layout);
    CHECK(!report.matched());
    CHECK(hasCategory(report, "parameter-unavailable"));
}

TEST_CASE("LVS expands multiplicity and matches renamed devices by topology") {
    ice::Project project;
    project.identity = "lvs-multiplicity";
    project.name = "lvs-multiplicity";
    const auto layout = ice::createLayoutCell(project, "top");
    const auto inNet = ice::createLayoutNet(project, layout, "in");
    const auto outNet = ice::createLayoutNet(project, layout, "out");
    auto &cell = project.layoutCells.at(layout);
    for (int index = 1; index <= 4; ++index) {
        ice::PlannedDevice resistor;
        resistor.id = project.nextId();
        resistor.name = "R" + std::to_string(index);
        resistor.device = "resistor";
        resistor.parameters.push_back({project.nextId(), "resistance", "1k"});
        resistor.terminals = {{"A", inNet}, {"B", outNet}};
        resistor.placeholder = false;
        cell.plannedDevices.emplace(resistor.id, resistor);
    }
    const auto spice = ice::createTextSchematicCell(project, "top_spice", ice::genericPdkId, "R1 in out 1k m=4\n");
    const auto report = ice::runLvs(project, spice, layout);
    CHECK(report.matched());
    CHECK(std::none_of(report.mismatches.begin(), report.mismatches.end(), [](const ice::LvsMismatch &mismatch) {
        return mismatch.category == "missing-device" || mismatch.category == "extra-device";
    }));

    cell.plannedDevices.clear();
    ice::PlannedDevice renamed;
    renamed.id = project.nextId();
    renamed.name = "RA";
    renamed.device = "resistor";
    renamed.parameters.push_back({project.nextId(), "resistance", "1k"});
    renamed.terminals = {{"A", inNet}, {"B", outNet}};
    renamed.placeholder = false;
    cell.plannedDevices.emplace(renamed.id, renamed);
    const auto simpleSpice =
        ice::createTextSchematicCell(project, "renamed_spice", ice::genericPdkId, "R1 in out 1k\n");
    const auto renamedReport = ice::runLvs(project, simpleSpice, layout);
    CHECK(renamedReport.matched());
    CHECK(hasCategory(renamedReport, "device-name"));
    CHECK(!hasCategory(renamedReport, "missing-device"));
    CHECK(!hasCategory(renamedReport, "extra-device"));
}

TEST_CASE("LVS derives layout connectivity from geometry, not planned links") {
    ice::Project project;
    project.identity = "lvs-geometry";
    project.name = "lvs-geometry";
    const auto layer = ice::addLayer(project, "Metal1", 8);
    const auto child = ice::createLayoutCell(project, "res_dev");
    auto &childCell = project.layoutCells.at(child);
    childCell.library = true;
    childCell.device = "resistor";
    ice::addPort(project, child, "A", layer, {0, 0, 200, 200}, ice::PortDirection::inout, ice::PortSide::left);
    ice::addPort(project, child, "B", layer, {1000, 0, 200, 200}, ice::PortDirection::inout, ice::PortSide::right);

    const auto layout = ice::createLayoutCell(project, "top");
    ice::addLayoutInstance(project, layout, child, "R1", 0, 0);
    ice::addLayoutInstance(project, layout, child, "R2", 3000, 0);
    ice::addRectangle(project, layout, layer, {1100, 0, 1900, 200});
    CHECK(project.layoutCells.at(layout).plannedConnections.empty());
    CHECK(project.layoutCells.at(layout).plannedDevices.empty());

    const auto schematic = ice::createSchematicCell(project, "top");
    std::vector<ice::ObjectId> r1Pins;
    std::vector<ice::ObjectId> r2Pins;
    addTwoPinDevice(project, schematic, "resistor", "R1", r1Pins);
    addTwoPinDevice(project, schematic, "resistor", "R2", r2Pins);
    auto &drawn = project.schematicCells.at(schematic);
    const auto inputNet = project.nextId();
    drawn.nets.emplace(inputNet, ice::Net{inputNet, "IN", {r1Pins[0]}});
    const auto middleNet = project.nextId();
    drawn.nets.emplace(middleNet, ice::Net{middleNet, "MID", {r1Pins[1], r2Pins[0]}});
    const auto outputNet = project.nextId();
    drawn.nets.emplace(outputNet, ice::Net{outputNet, "OUT", {r2Pins[1]}});

    const auto report = ice::runLvs(project, schematic, layout);
    CHECK(report.matched());
}

TEST_CASE("LVS parses gate-level SystemVerilog and rejects behavioral modules") {
    ice::Project project;
    project.identity = "lvs-gate-hdl";
    project.name = "lvs-gate-hdl";
    const auto source = ice::createSystemVerilogCell(
        project, "gate", ice::genericPdkId,
        "module gate(input logic a, input logic b, output logic y);\nand g1(y, a, b);\nendmodule\n");
    const auto layout = ice::createLayoutCell(project, "gate_layout");
    const auto layer = ice::addLayer(project, "Metal1", 10);
    const auto aNet = ice::createLayoutNet(project, layout, "a");
    const auto bNet = ice::createLayoutNet(project, layout, "b");
    const auto yNet = ice::createLayoutNet(project, layout, "y");
    ice::addPort(project, layout, "a", layer, {0, 0, 100, 100}, ice::PortDirection::input, ice::PortSide::left, aNet,
                 "a");
    ice::addPort(project, layout, "b", layer, {0, 200, 100, 100}, ice::PortDirection::input, ice::PortSide::left, bNet,
                 "b");
    ice::addPort(project, layout, "y", layer, {800, 100, 100, 100}, ice::PortDirection::output, ice::PortSide::right,
                 yNet, "y");
    auto &layoutCell = project.layoutCells.at(layout);
    ice::PlannedDevice gate;
    gate.id = project.nextId();
    gate.name = "g1";
    gate.device = "and";
    gate.terminals = {{"Y", yNet}, {"A", aNet}, {"B", bNet}};
    gate.placeholder = false;
    layoutCell.plannedDevices.emplace(gate.id, gate);

    auto report = ice::runLvs(project, source, layout);
    CHECK(report.matched());
    CHECK(report.portsMatched == 3);

    layoutCell.plannedDevices.at(gate.id).device = "or";
    report = ice::runLvs(project, source, layout);
    CHECK(!report.matched());
    CHECK(hasCategory(report, "device-type"));

    const auto behavioral = ice::createSystemVerilogCell(
        project, "behavioral_gate", ice::genericPdkId,
        "module behavioral_gate(input logic a, input logic b, output logic y);\nalways_comb y = a & b;\nendmodule\n");
    CHECK_THROWS_AS(ice::runLvs(project, behavioral, layout), ice::ProjectError);
}

TEST_CASE("LVS flattens text SPICE subcircuits and compares top-level ports") {
    ice::Project project;
    project.identity = "lvs-spice";
    project.name = "lvs-spice";
    const auto drawn = ice::createSchematicCell(project, "filter_drawn");
    std::vector<ice::ObjectId> rPins;
    std::vector<ice::ObjectId> cPins;
    const auto r1 = addTwoPinDevice(project, drawn, "resistor", "R1", rPins);
    const auto c1 = addTwoPinDevice(project, drawn, "capacitor", "C1", cPins);
    project.schematicCells.at(drawn).instances.at(r1).parameters.push_back({project.nextId(), "resistance", "1k"});
    project.schematicCells.at(drawn).instances.at(c1).parameters.push_back({project.nextId(), "capacitance", "1n"});
    auto &drawnCell = project.schematicCells.at(drawn);
    const auto inNetId = project.nextId();
    drawnCell.nets.emplace(inNetId, ice::Net{inNetId, "IN", {rPins[0]}});
    const auto midNetId = project.nextId();
    drawnCell.nets.emplace(midNetId, ice::Net{midNetId, "MID", {rPins[1], cPins[0]}});
    const auto outNetId = project.nextId();
    drawnCell.nets.emplace(outNetId, ice::Net{outNetId, "OUT", {cPins[1]}});
    const auto layout = ice::createLayoutFromSchematic(project, drawn, "filter_layout");
    const auto layer = ice::addLayer(project, "Metal1", 10);
    const auto inNet = ice::createLayoutNet(project, layout, "IN");
    const auto outNet = ice::createLayoutNet(project, layout, "OUT");
    ice::addPort(project, layout, "IN", layer, {0, 0, 100, 100}, ice::PortDirection::input, ice::PortSide::left, inNet,
                 "IN");
    ice::addPort(project, layout, "OUT", layer, {0, 0, 100, 100}, ice::PortDirection::output, ice::PortSide::right,
                 outNet, "OUT");

    const auto text = ice::createTextSchematicCell(
        project, "filter", ice::genericPdkId, ".subckt filter IN OUT\nR1 IN MID 1k\nC1 MID OUT 1n\n.ends filter\n");
    const auto report = ice::runLvs(project, text, layout);
    CHECK(report.matched());
    CHECK(report.portsMatched == 2);
    CHECK(report.mismatches.empty());
}

TEST_CASE("LVS resolves SPICE includes against the source file") {
    ice::Project project;
    project.identity = "lvs-spice-include";
    project.name = "lvs-spice-include";
    const auto drawn = ice::createSchematicCell(project, "filter_drawn");
    std::vector<ice::ObjectId> rPins;
    std::vector<ice::ObjectId> cPins;
    const auto r1 = addTwoPinDevice(project, drawn, "resistor", "R1", rPins);
    const auto c1 = addTwoPinDevice(project, drawn, "capacitor", "C1", cPins);
    project.schematicCells.at(drawn).instances.at(r1).parameters.push_back({project.nextId(), "resistance", "1k"});
    project.schematicCells.at(drawn).instances.at(c1).parameters.push_back({project.nextId(), "capacitance", "1n"});
    auto &drawnCell = project.schematicCells.at(drawn);
    const auto inNetId = project.nextId();
    drawnCell.nets.emplace(inNetId, ice::Net{inNetId, "IN", {rPins[0]}});
    const auto midNetId = project.nextId();
    drawnCell.nets.emplace(midNetId, ice::Net{midNetId, "MID", {rPins[1], cPins[0]}});
    const auto outNetId = project.nextId();
    drawnCell.nets.emplace(outNetId, ice::Net{outNetId, "OUT", {cPins[1]}});
    const auto layout = ice::createLayoutFromSchematic(project, drawn, "filter_layout");
    const auto layer = ice::addLayer(project, "Metal1", 10);
    const auto inNet = ice::createLayoutNet(project, layout, "IN");
    const auto outNet = ice::createLayoutNet(project, layout, "OUT");
    ice::addPort(project, layout, "IN", layer, {0, 0, 100, 100}, ice::PortDirection::input, ice::PortSide::left, inNet,
                 "IN");
    ice::addPort(project, layout, "OUT", layer, {0, 0, 100, 100}, ice::PortDirection::output, ice::PortSide::right,
                 outNet, "OUT");

    const auto root = std::filesystem::temp_directory_path() / "ice-lvs-include-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    std::ofstream(root / "filter.inc") << ".subckt filter IN OUT\nR1 IN MID 1k\nC1 MID OUT 1n\n.ends filter\n";
    const auto text = ice::createTextSchematicCell(project, "filter", ice::genericPdkId, ".include \"filter.inc\"\n",
                                                   (root / "top.sp").string());
    auto report = ice::runLvs(project, text, layout);
    CHECK(report.matched());
    CHECK(report.portsMatched == 2);

    std::filesystem::remove(root / "filter.inc");
    report = ice::runLvs(project, text, layout);
    CHECK(!report.matched());
    CHECK(hasCategory(report, "spice-include"));
    std::filesystem::remove_all(root);
}

TEST_CASE("LVS flattens nested text SPICE subcircuit instances") {
    ice::Project project;
    project.identity = "lvs-spice-nested";
    project.name = "lvs-spice-nested";
    const auto drawn = ice::createSchematicCell(project, "nested_drawn");
    std::vector<ice::ObjectId> r1Pins;
    std::vector<ice::ObjectId> r2Pins;
    const auto r1 = addTwoPinDevice(project, drawn, "resistor", "R1", r1Pins);
    const auto r2 = addTwoPinDevice(project, drawn, "resistor", "R2", r2Pins);
    project.schematicCells.at(drawn).instances.at(r1).parameters.push_back({project.nextId(), "resistance", "1k"});
    project.schematicCells.at(drawn).instances.at(r2).parameters.push_back({project.nextId(), "resistance", "1k"});
    auto &drawnCell = project.schematicCells.at(drawn);
    const auto inNet = project.nextId();
    drawnCell.nets.emplace(inNet, ice::Net{inNet, "IN", {r1Pins[0]}});
    const auto midNet = project.nextId();
    drawnCell.nets.emplace(midNet, ice::Net{midNet, "MID", {r1Pins[1], r2Pins[0]}});
    const auto outNet = project.nextId();
    drawnCell.nets.emplace(outNet, ice::Net{outNet, "OUT", {r2Pins[1]}});
    const auto layout = ice::createLayoutFromSchematic(project, drawn, "nested_layout");

    const auto text = ice::createTextSchematicCell(project, "nested_top", ice::genericPdkId,
                                                   ".subckt leaf_a A B\nR1 A B 1k\n.ends leaf_a\n"
                                                   ".subckt leaf_b B C\nR2 B C 1k\n.ends leaf_b\n"
                                                   "X1 IN MID leaf_a\nX2 MID OUT leaf_b\n");
    const auto report = ice::runLvs(project, text, layout);
    CHECK(report.matched());
    CHECK(report.mismatches.empty());
}

TEST_CASE("LVS merges SPICE global nets across subcircuit instances") {
    ice::Project project;
    project.identity = "lvs-global";
    project.name = "lvs-global";
    const auto layer = ice::addLayer(project, "Metal1", 12);
    const auto layout = ice::createLayoutCell(project, "top");
    const auto inNet = ice::createLayoutNet(project, layout, "IN");
    const auto midNet = ice::createLayoutNet(project, layout, "MID");
    const auto outNet = ice::createLayoutNet(project, layout, "OUT");
    const auto vddNet = ice::createLayoutNet(project, layout, "VDD");
    ice::addPort(project, layout, "IN", layer, {0, 0, 100, 100}, ice::PortDirection::input, ice::PortSide::left, inNet,
                 "IN");
    ice::addPort(project, layout, "OUT", layer, {900, 0, 100, 100}, ice::PortDirection::output, ice::PortSide::right,
                 outNet, "OUT");
    ice::addPort(project, layout, "MID", layer, {450, 0, 100, 100}, ice::PortDirection::inout, ice::PortSide::left,
                 midNet, "MID");
    struct Planned {
        const char *name;
        const char *value;
        ice::ObjectId a;
        ice::ObjectId b;
    };
    const Planned planned[] = {{"X1_R1", "1k", inNet, vddNet},
                               {"X1_R2", "2k", midNet, vddNet},
                               {"X2_R1", "1k", midNet, vddNet},
                               {"X2_R2", "2k", outNet, vddNet}};
    for (const auto &entry : planned) {
        ice::PlannedDevice device;
        device.id = project.nextId();
        device.name = entry.name;
        device.device = "resistor";
        device.parameters.push_back({project.nextId(), "resistance", entry.value});
        device.terminals = {{"A", entry.a}, {"B", entry.b}};
        device.placeholder = false;
        project.layoutCells.at(layout).plannedDevices.emplace(device.id, device);
    }
    const auto schematic = ice::createTextSchematicCell(project, "top", ice::genericPdkId,
                                                        ".global VDD\n"
                                                        ".subckt leaf A B\n"
                                                        "R1 A VDD 1k\n"
                                                        "R2 B VDD 2k\n"
                                                        ".ends leaf\n"
                                                        ".subckt top IN MID OUT\n"
                                                        "X1 IN MID leaf\n"
                                                        "X2 MID OUT leaf\n"
                                                        ".ends top\n");
    const auto report = ice::runLvs(project, schematic, layout);
    CHECK(report.matched());
}

TEST_CASE("LVS compares SystemVerilog module ports") {
    ice::Project project;
    project.identity = "lvs-hdl";
    project.name = "lvs-hdl";
    const auto source =
        ice::createSystemVerilogCell(project, "buffer", ice::genericPdkId,
                                     "module buffer(input logic in, output logic out);\nassign out = in;\nendmodule\n");
    const auto layout = ice::createLayoutCell(project, "buffer_layout");
    const auto layer = ice::addLayer(project, "Metal1", 10);
    const auto inNet = ice::createLayoutNet(project, layout, "in");
    const auto outNet = ice::createLayoutNet(project, layout, "out");
    ice::addPort(project, layout, "in", layer, {0, 0, 100, 100}, ice::PortDirection::input, ice::PortSide::left, inNet,
                 "in");
    ice::addPort(project, layout, "out", layer, {0, 0, 100, 100}, ice::PortDirection::output, ice::PortSide::right,
                 outNet, "out");
    const auto report = ice::runLvs(project, source, layout);
    CHECK(report.matched());
    CHECK(report.portsMatched == 2);
    CHECK(report.mismatches.empty());
}

namespace {
ice::ObjectId addSchematicPort(ice::Project &project, ice::ObjectId cell, const std::string &type,
                               const std::string &name, const std::string &netName) {
    const auto symbol = ice::addSymbol(project, cell, type);
    auto &symbolEntry = project.schematicCells.at(cell).symbols.at(symbol);
    const auto pin = project.nextId();
    symbolEntry.pins.push_back({pin, "P", 0, 0});
    ice::addSchematicInstance(project, cell, symbol, name, 0, 0);
    for (auto &[_, net] : project.schematicCells.at(cell).nets) {
        if (net.name == netName) {
            net.pins.push_back(pin);
            return pin;
        }
    }
    return 0;
}

ice::ObjectId layoutNetNamed(const ice::Project &project, ice::ObjectId cell, const std::string &name) {
    for (const auto &[id, net] : project.layoutCells.at(cell).nets) {
        if (net.name == name)
            return id;
    }
    return 0;
}

ice::ObjectId schematicInstanceNamed(const ice::Project &project, ice::ObjectId cell, const std::string &name) {
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

    const auto layer = ice::addLayer(fixture.project, "Metal1", 8);
    const auto inNet = layoutNetNamed(fixture.project, fixture.layout, "in");
    const auto outNet = layoutNetNamed(fixture.project, fixture.layout, "out");
    CHECK(inNet != 0);
    CHECK(outNet != 0);
    if (inNet == 0 || outNet == 0)
        return;
    const auto input = ice::addPort(fixture.project, fixture.layout, "A", layer, {0, 0, 400, 400},
                                    ice::PortDirection::input, ice::PortSide::left, inNet, "A");
    const auto output = ice::addPort(fixture.project, fixture.layout, "B", layer, {800, 0, 400, 400},
                                     ice::PortDirection::output, ice::PortSide::right, outNet, "B");
    CHECK(input != 0);

    // Ports produced by Convert to Pin carry the schematic-instance link.
    const auto inputInstance = schematicInstanceNamed(fixture.project, fixture.schematic, "A");
    const auto outputInstance = schematicInstanceNamed(fixture.project, fixture.schematic, "B");
    CHECK(inputInstance != 0);
    CHECK(outputInstance != 0);
    fixture.project.layoutCells.at(fixture.layout).ports.at(input).schematicInstance = inputInstance;
    fixture.project.layoutCells.at(fixture.layout).ports.at(output).schematicInstance = outputInstance;

    auto report = ice::runLvs(fixture.project, fixture.schematic, fixture.layout);
    CHECK(report.matched());
    CHECK(report.portsMatched == 2);
    CHECK(!hasCategory(report, "missing-port"));
    CHECK(!hasCategory(report, "extra-port"));
    CHECK(!hasCategory(report, "port-direction"));
    CHECK(!hasCategory(report, "port-net"));
    CHECK(ice::serializeLvsReport(report).find("\"ports_matched\": 2") != std::string::npos);

    auto &outputPort = fixture.project.layoutCells.at(fixture.layout).ports.at(output);
    outputPort.net = inNet;
    report = ice::runLvs(fixture.project, fixture.schematic, fixture.layout);
    CHECK(!report.matched());
    CHECK(hasCategory(report, "port-net"));

    outputPort.net = outNet;
    outputPort.direction = ice::PortDirection::input;
    report = ice::runLvs(fixture.project, fixture.schematic, fixture.layout);
    CHECK(!report.matched());
    CHECK(hasCategory(report, "port-direction"));

    outputPort.direction = ice::PortDirection::output;
    fixture.project.layoutCells.at(fixture.layout).ports.erase(output);
    report = ice::runLvs(fixture.project, fixture.schematic, fixture.layout);
    CHECK(!report.matched());
    CHECK(hasCategory(report, "missing-port"));

    const auto extra = ice::addPort(fixture.project, fixture.layout, "C", layer, {0, 800, 400, 400},
                                    ice::PortDirection::inout, ice::PortSide::top, 0, "C");
    CHECK(extra != 0);
    report = ice::runLvs(fixture.project, fixture.schematic, fixture.layout);
    CHECK(hasCategory(report, "missing-port"));
    CHECK(hasCategory(report, "extra-port"));
}

TEST_CASE("LVS reports port nets that cannot be attributed") {
    auto fixture = makeFixture();

    // Two ports that share one symbol cannot be told apart: their nets reference
    // the same symbol pin id.
    const auto shared = ice::addSymbol(fixture.project, fixture.schematic, "Input");
    auto &cell = fixture.project.schematicCells.at(fixture.schematic);
    auto &symbol = cell.symbols.at(shared);
    const auto pin = fixture.project.nextId();
    symbol.pins.push_back({pin, "P", 0, 0});
    // Projects written before instances cloned their symbols on placement can
    // still contain two instances of one symbol; build that state directly.
    for (const auto *name : {"A", "B"}) {
        const auto instanceId = fixture.project.nextId();
        cell.instances.emplace(instanceId, ice::SchematicInstance{instanceId, shared, name, 0, 0});
    }
    for (auto &[_, net] : cell.nets)
        net.pins.push_back(pin);

    auto report = ice::runLvs(fixture.project, fixture.schematic, fixture.layout);
    CHECK(hasCategory(report, "port-net-ambiguous"));

    // Distinct symbols per port resolve cleanly.
    auto second = makeFixture();
    const auto firstSymbol = ice::addSymbol(second.project, second.schematic, "Input");
    auto &firstEntry = second.project.schematicCells.at(second.schematic).symbols.at(firstSymbol);
    const auto firstPin = second.project.nextId();
    firstEntry.pins.push_back({firstPin, "P", 0, 0});
    ice::addSchematicInstance(second.project, second.schematic, firstSymbol, "A", 0, 0);
    if (!second.project.schematicCells.at(second.schematic).nets.empty())
        second.project.schematicCells.at(second.schematic).nets.begin()->second.pins.push_back(firstPin);
    report = ice::runLvs(second.project, second.schematic, second.layout);
    CHECK(!hasCategory(report, "port-net-ambiguous"));
}
