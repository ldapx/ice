#include "icelib/operations.hpp"
#include "icelib/pex.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

namespace {
bool near(double left, double right, double tolerance = 1.0e-9) { return std::abs(left - right) <= tolerance; }
} // namespace

TEST_CASE("PEX creates a text schematic with extracted net parasitics") {
    ice::Project project;
    project.identity = "pex-basic";
    project.name = "pex-basic";
    const auto cell = ice::createLayoutCell(project, "top");
    const auto metal = ice::addLayer(project, "Metal1", 8);
    const auto net = ice::createLayoutNet(project, cell, "VIN");
    const auto left = ice::addRectangle(project, cell, metal, {0, 0, 10000, 1000}, net);
    const auto right = ice::addRectangle(project, cell, metal, {10000, 0, 10000, 1000}, net);
    CHECK(left != 0);
    CHECK(right != 0);
    ice::addPort(project, cell, "P1", metal, {0, 0, 1000, 1000}, ice::PortDirection::input, ice::PortSide::left, net,
                 "P1");
    ice::addPort(project, cell, "P2", metal, {19000, 0, 1000, 1000}, ice::PortDirection::output, ice::PortSide::right,
                 net, "P2");

    const auto report = ice::runPex(project, cell);
    CHECK(report.nets.size() == 1);
    if (report.nets.size() != 1)
        return;
    const auto &extracted = report.nets.front();
    CHECK(extracted.name == "VIN");
    CHECK(near(extracted.area, 20.0));
    CHECK(near(extracted.perimeter, 44.0));
    // Two 10 x 1 um rectangles at 0.05 ohm/square.
    CHECK(near(extracted.resistance, 1.0));
    // 0.02 fF/um^2 area + 0.01 fF/um perimeter.
    CHECK(near(extracted.capacitance, 0.84));
    CHECK(near(report.totalResistance, 1.0));
    CHECK(near(report.totalCapacitance, 0.84));

    CHECK(report.schematicCell != 0);
    CHECK(project.schematicCells.contains(report.schematicCell));
    if (!project.schematicCells.contains(report.schematicCell))
        return;
    const auto &cellText = project.schematicCells.at(report.schematicCell);
    CHECK(cellText.textSchematic);
    CHECK(cellText.name == "top PEX");
    CHECK(cellText.spiceText.find("* ICE PEX layout-cell:") != std::string::npos);
    CHECK(cellText.spiceText.find(".title top PEX") != std::string::npos);
    CHECK(cellText.spiceText.find("RPEX1 P1 VIN 0.5") != std::string::npos);
    CHECK(cellText.spiceText.find("RPEX2 P2 VIN 0.5") != std::string::npos);
    CHECK(cellText.spiceText.find("CPEX1 VIN 0 0.84f") != std::string::npos);

    const auto second = ice::runPex(project, cell);
    CHECK(second.schematicCell == report.schematicCell);
    CHECK(project.schematicCells.size() == 1);

    const auto json = ice::serializePexReport(report);
    CHECK(json.find("\"resistance\"") != std::string::npos);
    CHECK(json.find("\"capacitance\"") != std::string::npos);
    CHECK(json.find("\"schematic\": \"top PEX\"") != std::string::npos);

    const auto roundTrip = ice::Project::deserialize(project.serialize());
    CHECK(roundTrip.schematicCells.contains(report.schematicCell));
    CHECK(roundTrip.schematicCells.at(report.schematicCell).textSchematic);
}

TEST_CASE("PEX ignores text and non-conducting layers and separates nets") {
    ice::Project project;
    project.identity = "pex-layers";
    project.name = "pex-layers";
    const auto cell = ice::createLayoutCell(project, "top");
    const auto metal = ice::addLayer(project, "Metal1", 8);
    const auto well = ice::addLayer(project, "NWell", 31);
    const auto firstNet = ice::createLayoutNet(project, cell, "A");
    const auto secondNet = ice::createLayoutNet(project, cell, "B");
    ice::addRectangle(project, cell, metal, {0, 0, 1000, 1000}, firstNet);
    ice::addRectangle(project, cell, metal, {5000, 0, 1000, 1000}, secondNet);
    ice::addRectangle(project, cell, well, {0, 0, 10000, 10000}, firstNet);
    ice::addText(project, cell, metal, 0, 0, "NOTE", 500, firstNet);

    const auto report = ice::runPex(project, cell);
    CHECK(report.nets.size() == 2);
    if (report.nets.size() != 2)
        return;
    CHECK(report.notes.size() >= 1);
    bool sawSkippedLayer = false;
    for (const auto &note : report.notes)
        sawSkippedLayer = sawSkippedLayer || note.find("NWell") != std::string::npos;
    CHECK(sawSkippedLayer);
    for (const auto &net : report.nets)
        CHECK(near(net.area, 1.0));
    const auto &text = project.schematicCells.at(report.schematicCell).spiceText;
    CHECK(text.find("RPEX1 A PEXRC1") != std::string::npos);
    CHECK(text.find("CPEX1 PEXRC1 0") != std::string::npos);
}

TEST_CASE("PEX connects pin-purpose layers to their drawing layer") {
    ice::Project project;
    project.identity = "pex-pin-layer";
    project.name = "pex-pin-layer";
    const auto cell = ice::createLayoutCell(project, "top");
    const auto metal = ice::addLayer(project, "Metal1", 8);
    const auto pinLayer = ice::addLayer(project, "Metal1.pin", 8, 0, {}, 2);
    const auto net = ice::createLayoutNet(project, cell, "VIN");
    ice::addRectangle(project, cell, metal, {0, 0, 10000, 1000});
    ice::addPort(project, cell, "VIN", pinLayer, {0, 0, 1000, 1000}, ice::PortDirection::input, ice::PortSide::left,
                 net, "VIN");

    const auto report = ice::runPex(project, cell);
    CHECK(report.nets.size() == 1);
    if (report.nets.size() != 1)
        return;
    const auto &extracted = report.nets.front();
    CHECK(extracted.name == "VIN");
    CHECK(near(extracted.area, 10.0));
    CHECK(near(extracted.resistance, 0.5));
    CHECK(near(extracted.capacitance, 0.42));
    CHECK(project.schematicCells.at(report.schematicCell).spiceText.find("RPEX1 VIN PEXRC1") != std::string::npos);
}

TEST_CASE("PEX recognizes polygon and via geometry") {
    ice::Project project;
    project.identity = "pex-polygon";
    project.name = "pex-polygon";
    const auto cell = ice::createLayoutCell(project, "top");
    const auto poly = ice::addLayer(project, "GatPoly", 5);
    const auto via = ice::addLayer(project, "Via1", 19);
    const auto net = ice::createLayoutNet(project, cell, "GATE");
    ice::addPolygon(project, cell, poly, {{0, 0}, {10000, 0}, {10000, 1000}, {0, 1000}}, net);
    ice::addRectangle(project, cell, via, {1000, 0, 200, 200}, net);

    const auto report = ice::runPex(project, cell);
    CHECK(report.nets.size() == 1);
    if (report.nets.size() != 1)
        return;
    const auto &extracted = report.nets.front();
    CHECK(extracted.viaCount == 1);
    CHECK(near(extracted.resistance, 50.5));
    CHECK(near(extracted.area, 10.04));
    CHECK(near(extracted.perimeter, 22.8));
    CHECK(near(extracted.capacitance, 0.4288));
}

TEST_CASE("PEX extracts placed MOS devices and prefers them over the schematic") {
    ice::Project project;
    project.identity = "pex-placed-device";
    project.name = "pex-placed-device";
    const auto metal = ice::addLayer(project, "Metal1", 8);
    const auto metalPin = ice::addLayer(project, "Metal1.pin", 8, 0, {}, 2);
    const auto poly = ice::addLayer(project, "GatPoly", 5);
    const auto polyPin = ice::addLayer(project, "GatPoly.pin", 5, 0, {}, 2);

    const auto child = ice::createLayoutCell(project, "nmos_dev", "ihp.sg13g2");
    auto &deviceCell = project.layoutCells.at(child);
    deviceCell.library = true;
    deviceCell.device = "nmos";
    deviceCell.parameters.push_back({project.nextId(), "model", "sg13_lv_nmos"});
    deviceCell.parameters.push_back({project.nextId(), "w", "1u"});
    deviceCell.parameters.push_back({project.nextId(), "l", "0.1u"});
    ice::addPort(project, child, "D", metalPin, {1000, 0, 200, 200}, ice::PortDirection::inout, ice::PortSide::right);
    ice::addPort(project, child, "G", polyPin, {500, 500, 200, 200}, ice::PortDirection::input, ice::PortSide::top);
    ice::addPort(project, child, "S", metalPin, {0, 0, 200, 200}, ice::PortDirection::inout, ice::PortSide::left);

    const auto parent = ice::createLayoutCell(project, "top", "ihp.sg13g2");
    ice::addLayoutInstance(project, parent, child, "M1", 10000, 0);
    const auto drainNet = ice::createLayoutNet(project, parent, "DRAIN");
    const auto gateNet = ice::createLayoutNet(project, parent, "GATE");
    const auto sourceNet = ice::createLayoutNet(project, parent, "SOURCE");
    ice::addRectangle(project, parent, metal, {11000, 0, 400, 400}, drainNet);
    ice::addRectangle(project, parent, poly, {10500, 500, 400, 400}, gateNet);
    ice::addRectangle(project, parent, metal, {10000, 0, 400, 400}, sourceNet);

    ice::createTextSchematicCell(project, "top", "ihp.sg13g2", "* linked schematic\nRSCH 1 2 1k\n.end\n");

    const auto report = ice::runPex(project, parent);
    CHECK(report.devicesFromLayout);
    CHECK(!report.devicesFromSchematic);
    CHECK(report.devices.size() == 1);
    if (report.devices.size() != 1)
        return;
    const auto &device = report.devices.front();
    CHECK(device.type == "mos");
    CHECK(device.model == "sg13_lv_nmos");
    CHECK((device.pins == std::vector<std::string>{"D", "G", "S", "B"}));
    CHECK((device.nodes == std::vector<std::string>{"DRAIN", "GATE", "SOURCE", "0"}));
    CHECK(device.spiceLine == "XM1 DRAIN GATE SOURCE 0 sg13_lv_nmos w=1u l=0.1u");

    const auto &text = project.schematicCells.at(report.schematicCell).spiceText;
    CHECK(text.find("* Devices extracted from placed layout instances.") != std::string::npos);
    CHECK(text.find("XM1 DRAIN GATE SOURCE 0 sg13_lv_nmos w=1u l=0.1u") != std::string::npos);
    CHECK(text.find("RSCH") == std::string::npos);

    const auto json = ice::serializePexReport(report);
    CHECK(json.find("\"devices_from_layout\": true") != std::string::npos);
    CHECK(json.find("\"devices_from_schematic\": false") != std::string::npos);
    CHECK(json.find("XM1 DRAIN GATE SOURCE 0 sg13_lv_nmos w=1u l=0.1u") != std::string::npos);
}

TEST_CASE("PEX extracts placed capacitor devices with their library values") {
    ice::Project project;
    project.identity = "pex-placed-cap";
    project.name = "pex-placed-cap";
    const auto metal = ice::addLayer(project, "Metal1", 8);
    const auto metalPin = ice::addLayer(project, "Metal1.pin", 8, 0, {}, 2);
    const auto child = ice::createLayoutCell(project, "cmim", "ihp.sg13g2");
    auto &deviceCell = project.layoutCells.at(child);
    deviceCell.library = true;
    deviceCell.device = "cmim";
    deviceCell.parameters.push_back({project.nextId(), "model", "cap_cmim"});
    deviceCell.parameters.push_back({project.nextId(), "C", "74.6f"});
    ice::addPort(project, child, "PLUS", metalPin, {0, 0, 200, 200}, ice::PortDirection::inout, ice::PortSide::left);
    ice::addPort(project, child, "MINUS", metalPin, {1000, 0, 200, 200}, ice::PortDirection::inout,
                 ice::PortSide::right);

    const auto parent = ice::createLayoutCell(project, "top", "ihp.sg13g2");
    ice::addLayoutInstance(project, parent, child, "C1", 5000, 0);
    const auto plus = ice::createLayoutNet(project, parent, "PLUS");
    const auto minus = ice::createLayoutNet(project, parent, "MINUS");
    ice::addRectangle(project, parent, metal, {5000, 0, 400, 400}, plus);
    ice::addRectangle(project, parent, metal, {6000, 0, 400, 400}, minus);

    const auto report = ice::runPex(project, parent);
    CHECK(report.devices.size() == 1);
    if (report.devices.size() != 1)
        return;
    const auto &device = report.devices.front();
    CHECK(device.type == "capacitor");
    CHECK(device.model == "cap_cmim");
    CHECK(device.spiceLine == "C1 PLUS MINUS 74.6f");
}

TEST_CASE("PEX extracts polygon MOS devices from PDK rules") {
    ice::Project project;
    project.identity = "pex-polygon-mos";
    project.name = "pex-polygon-mos";
    const auto active = ice::addLayer(project, "Activ", 1);
    const auto gateLayer = ice::addLayer(project, "GatPoly", 5);
    const auto contact = ice::addLayer(project, "Contact", 6);
    const auto metal = ice::addLayer(project, "Metal1", 8);
    const auto well = ice::addLayer(project, "NWell", 31);
    const auto cell = ice::createLayoutCell(project, "top", "test.pdk");
    const auto gateNet = ice::createLayoutNet(project, cell, "G");
    const auto sourceNet = ice::createLayoutNet(project, cell, "S");
    const auto drainNet = ice::createLayoutNet(project, cell, "D");
    const auto bodyNet = ice::createLayoutNet(project, cell, "B");
    ice::addRectangle(project, cell, active, {0, 0, 2000, 1000});
    ice::addRectangle(project, cell, gateLayer, {900, -200, 200, 1400}, gateNet);
    ice::addRectangle(project, cell, well, {-500, -500, 3000, 2000}, bodyNet);
    ice::addRectangle(project, cell, contact, {100, 100, 200, 200});
    ice::addRectangle(project, cell, contact, {1700, 100, 200, 200});
    ice::addRectangle(project, cell, metal, {0, 0, 500, 300}, sourceNet);
    ice::addRectangle(project, cell, metal, {1500, 0, 500, 300}, drainNet);

    const auto rulesPath = std::filesystem::temp_directory_path() / "ice-pex-polygon-rules.json";
    std::ofstream(rulesPath) << R"({
      "format": "ice-device-extraction",
      "schema_version": 1,
      "devices": [
        {
          "name": "nmos",
          "type": "mos",
          "model": "NMOS_TEST",
          "element": "X",
          "active": "Activ",
          "gate": "GatPoly",
          "body_layer": "NWell",
          "terminal_layers": ["Metal1"],
          "substrate": "0",
          "length_axis": "auto"
        }
      ]
    })";

    ice::PexOptions options;
    options.extractionRulesPath = rulesPath;
    options.includeLinkedSchematic = false;
    const auto report = ice::runPex(project, cell, options);
    std::filesystem::remove(rulesPath);

    CHECK(report.devicesFromLayout);
    CHECK(report.devices.size() == 1);
    if (report.devices.size() != 1)
        return;
    const auto &device = report.devices.front();
    CHECK(device.type == "mos");
    CHECK(device.model == "NMOS_TEST");
    CHECK(device.spiceLine == "X1 D G S B NMOS_TEST w=1u l=0.2u");
    const auto &text = project.schematicCells.at(report.schematicCell).spiceText;
    CHECK(text.find("X1 D G S B NMOS_TEST w=1u l=0.2u") != std::string::npos);
}

TEST_CASE("PEX extracts polygon capacitors from PDK rules") {
    ice::Project project;
    project.identity = "pex-polygon-cap";
    project.name = "pex-polygon-cap";
    const auto metal1 = ice::addLayer(project, "Metal1", 8);
    const auto metal2 = ice::addLayer(project, "Metal2", 10);
    const auto mim = ice::addLayer(project, "MIM", 36);
    const auto cell = ice::createLayoutCell(project, "top", "test.pdk");
    const auto firstNet = ice::createLayoutNet(project, cell, "A");
    const auto secondNet = ice::createLayoutNet(project, cell, "B");
    ice::addRectangle(project, cell, metal1, {0, 0, 1000, 1000}, firstNet);
    ice::addRectangle(project, cell, metal2, {0, 0, 1000, 1000}, secondNet);
    ice::addRectangle(project, cell, mim, {0, 0, 1000, 1000});

    const auto rulesPath = std::filesystem::temp_directory_path() / "ice-pex-polygon-cap-rules.json";
    std::ofstream(rulesPath) << R"({
      "format": "ice-device-extraction",
      "schema_version": 1,
      "devices": [
        {
          "name": "mim",
          "type": "capacitor",
          "model": "MIM_TEST",
          "element": "C",
          "plates": ["Metal1", "Metal2"],
          "require_layers": ["MIM"],
          "capacitance_per_area": 2.0
        }
      ]
    })";

    ice::PexOptions options;
    options.extractionRulesPath = rulesPath;
    options.includeLinkedSchematic = false;
    const auto report = ice::runPex(project, cell, options);
    std::filesystem::remove(rulesPath);

    CHECK(report.devices.size() == 1);
    if (report.devices.size() != 1)
        return;
    CHECK(report.devices.front().type == "capacitor");
    CHECK(report.devices.front().model == "MIM_TEST");
    CHECK(report.devices.front().spiceLine == "C1 A B 2f");
}

TEST_CASE("PEX extracts polygon resistors from PDK rules") {
    ice::Project project;
    project.identity = "pex-polygon-res";
    project.name = "pex-polygon-res";
    const auto body = ice::addLayer(project, "RES", 24);
    const auto metal = ice::addLayer(project, "Metal1", 8);
    const auto cell = ice::createLayoutCell(project, "top", "test.pdk");
    const auto firstNet = ice::createLayoutNet(project, cell, "A");
    const auto secondNet = ice::createLayoutNet(project, cell, "B");
    ice::addRectangle(project, cell, body, {0, 0, 2000, 500});
    ice::addRectangle(project, cell, metal, {0, 0, 300, 500}, firstNet);
    ice::addRectangle(project, cell, metal, {1700, 0, 300, 500}, secondNet);

    const auto rulesPath = std::filesystem::temp_directory_path() / "ice-pex-polygon-res-rules.json";
    std::ofstream(rulesPath) << R"({
      "format": "ice-device-extraction",
      "schema_version": 1,
      "devices": [
        {
          "name": "poly_res",
          "type": "resistor",
          "model": "RES_TEST",
          "element": "R",
          "body": "RES",
          "terminal_layers": ["Metal1"],
          "sheet_resistance": 1000.0
        }
      ]
    })";

    ice::PexOptions options;
    options.extractionRulesPath = rulesPath;
    options.includeLinkedSchematic = false;
    const auto report = ice::runPex(project, cell, options);
    std::filesystem::remove(rulesPath);

    CHECK(report.devices.size() == 1);
    if (report.devices.size() != 1)
        return;
    CHECK(report.devices.front().type == "resistor");
    CHECK(report.devices.front().model == "RES_TEST");
    CHECK(report.devices.front().spiceLine == "R1 A B 4000");
}

TEST_CASE("PEX copies the linked schematic device netlist") {
    ice::Project project;
    project.identity = "pex-linked";
    project.name = "pex-linked";
    const auto schematic = ice::createSchematicCell(project, "top");
    const auto symbol = ice::addSymbol(project, schematic, "resistor");
    const auto firstPin = project.nextId();
    const auto secondPin = project.nextId();
    auto &definition = project.schematicCells.at(schematic).symbols.at(symbol);
    definition.pins.push_back({firstPin, "A", 0, 0, ice::PinDirection::left});
    definition.pins.push_back({secondPin, "B", 0, 0, ice::PinDirection::right});
    ice::addSchematicInstance(project, schematic, symbol, "R1", 0, 0);
    ice::addNet(project, schematic, "VIN", {firstPin, secondPin});

    const auto cell = ice::createLayoutCell(project, "top");
    const auto metal = ice::addLayer(project, "Metal1", 8);
    const auto net = ice::createLayoutNet(project, cell, "VIN");
    ice::addRectangle(project, cell, metal, {0, 0, 2000, 1000}, net);

    const auto report = ice::runPex(project, cell);
    CHECK(report.devicesFromSchematic);
    CHECK(!report.devicesFromLayout);
    const auto &text = project.schematicCells.at(report.schematicCell).spiceText;
    CHECK(text.find("Devices from linked schematic 'top'") != std::string::npos);
    CHECK(text.find("R1 VIN VIN 1k") != std::string::npos);
}

TEST_CASE("PEX does not overwrite an unrelated text schematic with the same name") {
    ice::Project project;
    project.identity = "pex-name";
    project.name = "pex-name";
    const auto cell = ice::createLayoutCell(project, "top");
    const auto metal = ice::addLayer(project, "Metal1", 8);
    const auto net = ice::createLayoutNet(project, cell, "VIN");
    ice::addRectangle(project, cell, metal, {0, 0, 2000, 1000}, net);
    const auto existing = ice::createTextSchematicCell(project, "top PEX", ice::genericPdkId, "* user text\n.end\n");

    const auto report = ice::runPex(project, cell);
    CHECK(report.schematicCell != existing);
    CHECK(report.schematic == "top PEX 2");
    CHECK(project.schematicCells.at(existing).spiceText == "* user text\n.end\n");
}
