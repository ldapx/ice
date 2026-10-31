#include "icelib/core.hpp"
#include "icelib/operations.hpp"
#include "icelib/verification.hpp"

#include <doctest/doctest.h>

#include <algorithm>
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

TEST_CASE("LVS rejects text SPICE schematics and read-only layout cells") {
    ice::Project project;
    project.identity = "lvs-errors";
    project.name = "lvs-errors";
    const auto schematic = ice::createTextSchematicCell(project, "top");
    const auto layout = ice::createLayoutCell(project, "top_layout");
    CHECK_THROWS_AS(ice::runLvs(project, schematic, layout), ice::ProjectError);

    const auto drawn = ice::createSchematicCell(project, "drawn");
    project.layoutCells.at(layout).library = true;
    CHECK_THROWS_AS(ice::runLvs(project, drawn, layout), ice::ProjectError);
}
