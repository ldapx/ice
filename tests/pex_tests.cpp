#include "openvlsilib/operations.hpp"
#include "openvlsilib/pex.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

namespace {
bool near(double left, double right, double tolerance = 1.0e-9) { return std::abs(left - right) <= tolerance; }
} // namespace

TEST_CASE("PEX creates a text schematic with extracted net parasitics") {
    openvlsi::Project project;
    project.identity = "pex-basic";
    project.name = "pex-basic";
    const auto cell = openvlsi::createLayoutCell(project, "top");
    const auto metal = openvlsi::addLayer(project, "Metal1", 8);
    const auto net = openvlsi::createLayoutNet(project, cell, "VIN");
    const auto left = openvlsi::addRectangle(project, cell, metal, {0, 0, 10000, 1000}, net);
    const auto right = openvlsi::addRectangle(project, cell, metal, {10000, 0, 10000, 1000}, net);
    CHECK(left != 0);
    CHECK(right != 0);
    openvlsi::addPort(project, cell, "P1", metal, {0, 0, 1000, 1000}, openvlsi::PortDirection::input, openvlsi::PortSide::left, net,
                 "P1");
    openvlsi::addPort(project, cell, "P2", metal, {19000, 0, 1000, 1000}, openvlsi::PortDirection::output, openvlsi::PortSide::right,
                 net, "P2");

    const auto report = openvlsi::runPex(project, cell);
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
    CHECK(cellText.spiceText.find("* OpenVLSI PEX layout-cell:") != std::string::npos);
    CHECK(cellText.spiceText.find(".title top PEX") != std::string::npos);
    CHECK(cellText.spiceText.find("RPEX1 P1 VIN 0.5") != std::string::npos);
    CHECK(cellText.spiceText.find("RPEX2 P2 VIN 0.5") != std::string::npos);
    CHECK(cellText.spiceText.find("CPEX1 VIN 0 0.84f") != std::string::npos);

    const auto second = openvlsi::runPex(project, cell);
    CHECK(second.schematicCell == report.schematicCell);
    CHECK(project.schematicCells.size() == 1);

    const auto json = openvlsi::serializePexReport(report);
    CHECK(json.find("\"resistance\"") != std::string::npos);
    CHECK(json.find("\"capacitance\"") != std::string::npos);
    CHECK(json.find("\"schematic\": \"top PEX\"") != std::string::npos);

    const auto roundTrip = openvlsi::Project::deserialize(project.serialize());
    CHECK(roundTrip.schematicCells.contains(report.schematicCell));
    CHECK(roundTrip.schematicCells.at(report.schematicCell).textSchematic);
}

TEST_CASE("PEX ignores text and non-conducting layers and separates nets") {
    openvlsi::Project project;
    project.identity = "pex-layers";
    project.name = "pex-layers";
    const auto cell = openvlsi::createLayoutCell(project, "top");
    const auto metal = openvlsi::addLayer(project, "Metal1", 8);
    const auto well = openvlsi::addLayer(project, "NWell", 31);
    const auto firstNet = openvlsi::createLayoutNet(project, cell, "A");
    const auto secondNet = openvlsi::createLayoutNet(project, cell, "B");
    openvlsi::addRectangle(project, cell, metal, {0, 0, 1000, 1000}, firstNet);
    openvlsi::addRectangle(project, cell, metal, {5000, 0, 1000, 1000}, secondNet);
    openvlsi::addRectangle(project, cell, well, {0, 0, 10000, 10000}, firstNet);
    openvlsi::addText(project, cell, metal, 0, 0, "NOTE", 500, firstNet);

    const auto report = openvlsi::runPex(project, cell);
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
    // Nets without a device pin or cell port are floating; their parasitic RC
    // is reported as a comment instead of unsolvable floating nodes.
    CHECK(text.find("RPEX1 A PEXRC1") == std::string::npos);
    CHECK(text.find("no device terminal or pin") != std::string::npos);
}

TEST_CASE("PEX connects pin-purpose layers to their drawing layer") {
    openvlsi::Project project;
    project.identity = "pex-pin-layer";
    project.name = "pex-pin-layer";
    const auto cell = openvlsi::createLayoutCell(project, "top");
    const auto metal = openvlsi::addLayer(project, "Metal1", 8);
    const auto pinLayer = openvlsi::addLayer(project, "Metal1.pin", 8, 0, {}, 2);
    const auto net = openvlsi::createLayoutNet(project, cell, "VIN");
    openvlsi::addRectangle(project, cell, metal, {0, 0, 10000, 1000});
    openvlsi::addPort(project, cell, "VIN", pinLayer, {0, 0, 1000, 1000}, openvlsi::PortDirection::input, openvlsi::PortSide::left,
                 net, "VIN");

    const auto report = openvlsi::runPex(project, cell);
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
    openvlsi::Project project;
    project.identity = "pex-polygon";
    project.name = "pex-polygon";
    const auto cell = openvlsi::createLayoutCell(project, "top");
    const auto poly = openvlsi::addLayer(project, "GatPoly", 5);
    const auto via = openvlsi::addLayer(project, "Via1", 19);
    const auto net = openvlsi::createLayoutNet(project, cell, "GATE");
    openvlsi::addPolygon(project, cell, poly, {{0, 0}, {10000, 0}, {10000, 1000}, {0, 1000}}, net);
    openvlsi::addRectangle(project, cell, via, {1000, 0, 200, 200}, net);

    const auto report = openvlsi::runPex(project, cell);
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

TEST_CASE("PEX combines parallel conductors instead of summing them") {
    openvlsi::Project project;
    project.identity = "pex-parallel";
    project.name = "pex-parallel";
    const auto cell = openvlsi::createLayoutCell(project, "top");
    const auto metal = openvlsi::addLayer(project, "Metal1", 8);
    const auto net = openvlsi::createLayoutNet(project, cell, "VIN");
    openvlsi::addRectangle(project, cell, metal, {0, 0, 10000, 1000}, net);
    openvlsi::addRectangle(project, cell, metal, {0, 2000, 10000, 1000}, net);
    openvlsi::addPort(project, cell, "P1", metal, {0, 0, 1000, 3000}, openvlsi::PortDirection::input, openvlsi::PortSide::left, net,
                 "P1");
    openvlsi::addPort(project, cell, "P2", metal, {10000, 0, 1000, 3000}, openvlsi::PortDirection::output, openvlsi::PortSide::right,
                 net, "P2");
    const auto report = openvlsi::runPex(project, cell);
    REQUIRE(report.nets.size() == 1);
    CHECK(near(report.nets.front().resistance, 0.25));
}

TEST_CASE("PEX combines via arrays in parallel") {
    openvlsi::Project project;
    project.identity = "pex-via-array";
    project.name = "pex-via-array";
    const auto cell = openvlsi::createLayoutCell(project, "top");
    const auto poly = openvlsi::addLayer(project, "GatPoly", 5);
    const auto via = openvlsi::addLayer(project, "Via1", 19);
    const auto net = openvlsi::createLayoutNet(project, cell, "GATE");
    openvlsi::addPolygon(project, cell, poly, {{0, 0}, {10000, 0}, {10000, 1000}, {0, 1000}}, net);
    for (int index = 0; index < 4; ++index)
        openvlsi::addRectangle(project, cell, via, {1000, 0, 200, 200}, net);
    const auto report = openvlsi::runPex(project, cell);
    REQUIRE(report.nets.size() == 1);
    CHECK(report.nets.front().viaCount == 4);
    CHECK(near(report.nets.front().resistance, 50.125));
}

TEST_CASE("PEX merges duplicate planned and placed devices without dropping distinct ones") {
    openvlsi::Project project;
    project.identity = "pex-mixed-devices";
    project.name = "pex-mixed-devices";
    const auto metal = openvlsi::addLayer(project, "Metal1", 8);
    const auto child = openvlsi::createLayoutCell(project, "res_dev");
    auto &deviceCell = project.layoutCells.at(child);
    deviceCell.library = true;
    deviceCell.device = "resistor";
    deviceCell.parameters.push_back({project.nextId(), "resistance", "1k"});
    openvlsi::addPort(project, child, "A", metal, {0, 0, 200, 200}, openvlsi::PortDirection::inout, openvlsi::PortSide::left);
    openvlsi::addPort(project, child, "B", metal, {1000, 0, 200, 200}, openvlsi::PortDirection::inout, openvlsi::PortSide::right);

    const auto layout = openvlsi::createLayoutCell(project, "top");
    openvlsi::addLayoutInstance(project, layout, child, "R1", 0, 0);
    openvlsi::addLayoutInstance(project, layout, child, "R2", 3000, 0);
    const auto netA = openvlsi::createLayoutNet(project, layout, "A");
    const auto netB = openvlsi::createLayoutNet(project, layout, "B");
    const auto netC = openvlsi::createLayoutNet(project, layout, "C");
    const auto netD = openvlsi::createLayoutNet(project, layout, "D");
    openvlsi::addRectangle(project, layout, metal, {0, 0, 200, 200}, netA);
    openvlsi::addRectangle(project, layout, metal, {1000, 0, 200, 200}, netB);
    openvlsi::addRectangle(project, layout, metal, {3000, 0, 200, 200}, netC);
    openvlsi::addRectangle(project, layout, metal, {4000, 0, 200, 200}, netD);

    openvlsi::PlannedDevice planned;
    planned.id = project.nextId();
    planned.name = "R1";
    planned.device = "resistor";
    planned.x = 0;
    planned.y = 0;
    planned.width = 1200;
    planned.height = 200;
    planned.parameters.push_back({project.nextId(), "resistance", "1k"});
    planned.terminals = {{"A", netA}, {"B", netB}};
    planned.placeholder = false;
    project.layoutCells.at(layout).plannedDevices.emplace(planned.id, planned);

    const auto report = openvlsi::runPex(project, layout);
    CHECK(report.devices.size() == 2);
    CHECK(std::any_of(report.notes.begin(), report.notes.end(), [](const std::string &note) {
        return note.find("duplicate placed device 'R1'") != std::string::npos;
    }));
}

TEST_CASE("PEX extracts placed MOS devices and prefers them over the schematic") {
    openvlsi::Project project;
    project.identity = "pex-placed-device";
    project.name = "pex-placed-device";
    const auto metal = openvlsi::addLayer(project, "Metal1", 8);
    const auto metalPin = openvlsi::addLayer(project, "Metal1.pin", 8, 0, {}, 2);
    const auto poly = openvlsi::addLayer(project, "GatPoly", 5);
    const auto polyPin = openvlsi::addLayer(project, "GatPoly.pin", 5, 0, {}, 2);

    const auto child = openvlsi::createLayoutCell(project, "nmos_dev", "ihp.sg13g2");
    auto &deviceCell = project.layoutCells.at(child);
    deviceCell.library = true;
    deviceCell.device = "nmos";
    deviceCell.parameters.push_back({project.nextId(), "model", "sg13_lv_nmos"});
    deviceCell.parameters.push_back({project.nextId(), "w", "1u"});
    deviceCell.parameters.push_back({project.nextId(), "l", "0.1u"});
    openvlsi::addPort(project, child, "D", metalPin, {1000, 0, 200, 200}, openvlsi::PortDirection::inout, openvlsi::PortSide::right);
    openvlsi::addPort(project, child, "G", polyPin, {500, 500, 200, 200}, openvlsi::PortDirection::input, openvlsi::PortSide::top);
    openvlsi::addPort(project, child, "S", metalPin, {0, 0, 200, 200}, openvlsi::PortDirection::inout, openvlsi::PortSide::left);

    const auto parent = openvlsi::createLayoutCell(project, "top", "ihp.sg13g2");
    openvlsi::addLayoutInstance(project, parent, child, "M1", 10000, 0);
    const auto drainNet = openvlsi::createLayoutNet(project, parent, "DRAIN");
    const auto gateNet = openvlsi::createLayoutNet(project, parent, "GATE");
    const auto sourceNet = openvlsi::createLayoutNet(project, parent, "SOURCE");
    openvlsi::addRectangle(project, parent, metal, {11000, 0, 400, 400}, drainNet);
    openvlsi::addRectangle(project, parent, poly, {10500, 500, 400, 400}, gateNet);
    openvlsi::addRectangle(project, parent, metal, {10000, 0, 400, 400}, sourceNet);

    openvlsi::createTextSchematicCell(project, "top", "ihp.sg13g2", "* linked schematic\nRSCH 1 2 1k\n.end\n");

    const auto report = openvlsi::runPex(project, parent);
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
    CHECK(text.find("* Devices extracted from layout metadata or geometry.") != std::string::npos);
    CHECK(text.find("XM1 DRAIN GATE SOURCE 0 sg13_lv_nmos w=1u l=0.1u") != std::string::npos);
    CHECK(text.find("RSCH") == std::string::npos);

    const auto json = openvlsi::serializePexReport(report);
    CHECK(json.find("\"devices_from_layout\": true") != std::string::npos);
    CHECK(json.find("\"devices_from_schematic\": false") != std::string::npos);
    CHECK(json.find("XM1 DRAIN GATE SOURCE 0 sg13_lv_nmos w=1u l=0.1u") != std::string::npos);
}

TEST_CASE("PEX extracts placed capacitor devices with their library values") {
    openvlsi::Project project;
    project.identity = "pex-placed-cap";
    project.name = "pex-placed-cap";
    const auto metal = openvlsi::addLayer(project, "Metal1", 8);
    const auto metalPin = openvlsi::addLayer(project, "Metal1.pin", 8, 0, {}, 2);
    const auto child = openvlsi::createLayoutCell(project, "cmim", "ihp.sg13g2");
    auto &deviceCell = project.layoutCells.at(child);
    deviceCell.library = true;
    deviceCell.device = "cmim";
    deviceCell.parameters.push_back({project.nextId(), "model", "cap_cmim"});
    deviceCell.parameters.push_back({project.nextId(), "C", "74.6f"});
    openvlsi::addPort(project, child, "PLUS", metalPin, {0, 0, 200, 200}, openvlsi::PortDirection::inout, openvlsi::PortSide::left);
    openvlsi::addPort(project, child, "MINUS", metalPin, {1000, 0, 200, 200}, openvlsi::PortDirection::inout,
                 openvlsi::PortSide::right);

    const auto parent = openvlsi::createLayoutCell(project, "top", "ihp.sg13g2");
    openvlsi::addLayoutInstance(project, parent, child, "C1", 5000, 0);
    const auto plus = openvlsi::createLayoutNet(project, parent, "PLUS");
    const auto minus = openvlsi::createLayoutNet(project, parent, "MINUS");
    openvlsi::addRectangle(project, parent, metal, {5000, 0, 400, 400}, plus);
    openvlsi::addRectangle(project, parent, metal, {6000, 0, 400, 400}, minus);

    const auto report = openvlsi::runPex(project, parent);
    CHECK(report.devices.size() == 1);
    if (report.devices.size() != 1)
        return;
    const auto &device = report.devices.front();
    CHECK(device.type == "capacitor");
    CHECK(device.model == "cap_cmim");
    CHECK(device.spiceLine == "C1 PLUS MINUS 74.6f");
}

TEST_CASE("PEX skips imported digital macros even when names match passives") {
    openvlsi::Project project;
    project.identity = "pex-digital-macro";
    project.name = "pex-digital-macro";
    const auto metal = openvlsi::addLayer(project, "Metal1", 8);
    const auto metalPin = openvlsi::addLayer(project, "Metal1.pin", 8, 0, {}, 2);
    const auto child = openvlsi::createLayoutCell(project, "digital_cap_macro", openvlsi::genericPdkId);
    auto &deviceCell = project.layoutCells.at(child);
    deviceCell.library = true;
    deviceCell.readOnly = true;
    deviceCell.device = "digital:cap_impl";
    openvlsi::addPort(project, child, "A", metalPin, {0, 0, 200, 200}, openvlsi::PortDirection::inout, openvlsi::PortSide::left);
    openvlsi::addPort(project, child, "B", metalPin, {1000, 0, 200, 200}, openvlsi::PortDirection::inout, openvlsi::PortSide::right);

    const auto parent = openvlsi::createLayoutCell(project, "top", openvlsi::genericPdkId);
    openvlsi::addLayoutInstance(project, parent, child, "D1", 5000, 0);
    const auto netA = openvlsi::createLayoutNet(project, parent, "A");
    const auto netB = openvlsi::createLayoutNet(project, parent, "B");
    openvlsi::addRectangle(project, parent, metal, {5000, 0, 400, 400}, netA);
    openvlsi::addRectangle(project, parent, metal, {6000, 0, 400, 400}, netB);

    const auto report = openvlsi::runPex(project, parent);
    CHECK(report.devices.empty());
    CHECK(std::any_of(report.notes.begin(), report.notes.end(), [](const std::string &note) {
        return note.find("not a recognized device") != std::string::npos;
    }));
}

TEST_CASE("PEX extracts polygon MOS devices from PDK rules") {
    openvlsi::Project project;
    project.identity = "pex-polygon-mos";
    project.name = "pex-polygon-mos";
    const auto active = openvlsi::addLayer(project, "Activ", 1);
    const auto gateLayer = openvlsi::addLayer(project, "GatPoly", 5);
    const auto contact = openvlsi::addLayer(project, "Contact", 6);
    const auto metal = openvlsi::addLayer(project, "Metal1", 8);
    const auto well = openvlsi::addLayer(project, "NWell", 31);
    const auto cell = openvlsi::createLayoutCell(project, "top", "test.pdk");
    const auto gateNet = openvlsi::createLayoutNet(project, cell, "G");
    const auto sourceNet = openvlsi::createLayoutNet(project, cell, "S");
    const auto drainNet = openvlsi::createLayoutNet(project, cell, "D");
    const auto bodyNet = openvlsi::createLayoutNet(project, cell, "B");
    openvlsi::addRectangle(project, cell, active, {0, 0, 2000, 1000});
    openvlsi::addRectangle(project, cell, gateLayer, {900, -200, 200, 1400}, gateNet);
    openvlsi::addRectangle(project, cell, well, {-500, -500, 3000, 2000}, bodyNet);
    openvlsi::addRectangle(project, cell, contact, {100, 100, 200, 200});
    openvlsi::addRectangle(project, cell, contact, {1700, 100, 200, 200});
    openvlsi::addRectangle(project, cell, metal, {0, 0, 500, 300}, sourceNet);
    openvlsi::addRectangle(project, cell, metal, {1500, 0, 500, 300}, drainNet);

    const auto rulesPath = std::filesystem::temp_directory_path() / "openvlsi-pex-polygon-rules.json";
    std::ofstream(rulesPath) << R"({
      "format": "openvlsi-device-extraction",
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

    openvlsi::PexOptions options;
    options.extractionRulesPath = rulesPath;
    options.includeLinkedSchematic = false;
    const auto report = openvlsi::runPex(project, cell, options);
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
    openvlsi::Project project;
    project.identity = "pex-polygon-cap";
    project.name = "pex-polygon-cap";
    const auto metal1 = openvlsi::addLayer(project, "Metal1", 8);
    const auto metal2 = openvlsi::addLayer(project, "Metal2", 10);
    const auto mim = openvlsi::addLayer(project, "MIM", 36);
    const auto cell = openvlsi::createLayoutCell(project, "top", "test.pdk");
    const auto firstNet = openvlsi::createLayoutNet(project, cell, "A");
    const auto secondNet = openvlsi::createLayoutNet(project, cell, "B");
    openvlsi::addRectangle(project, cell, metal1, {0, 0, 1000, 1000}, firstNet);
    openvlsi::addRectangle(project, cell, metal2, {0, 0, 1000, 1000}, secondNet);
    openvlsi::addRectangle(project, cell, mim, {0, 0, 1000, 1000});

    const auto rulesPath = std::filesystem::temp_directory_path() / "openvlsi-pex-polygon-cap-rules.json";
    std::ofstream(rulesPath) << R"({
      "format": "openvlsi-device-extraction",
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

    openvlsi::PexOptions options;
    options.extractionRulesPath = rulesPath;
    options.includeLinkedSchematic = false;
    const auto report = openvlsi::runPex(project, cell, options);
    std::filesystem::remove(rulesPath);

    CHECK(report.devices.size() == 1);
    if (report.devices.size() != 1)
        return;
    CHECK(report.devices.front().type == "capacitor");
    CHECK(report.devices.front().model == "MIM_TEST");
    CHECK(report.devices.front().spiceLine == "C1 A B 2f");
}

TEST_CASE("PEX keeps the layout ground net as SPICE node zero") {
    openvlsi::Project project;
    project.identity = "pex-ground";
    project.name = "pex-ground";
    const auto metal = openvlsi::addLayer(project, "Metal1", 8);
    const auto cell = openvlsi::createLayoutCell(project, "top");
    const auto ground = openvlsi::createLayoutNet(project, cell, "0");
    openvlsi::addRectangle(project, cell, metal, {0, 0, 10000, 1000}, ground);

    const auto report = openvlsi::runPex(project, cell);

    CHECK(report.spiceText.find("n_0") == std::string::npos);
    CHECK(report.spiceText.find("* Net 0") != std::string::npos);
}

TEST_CASE("PEX extracts polygon resistors from PDK rules") {
    openvlsi::Project project;
    project.identity = "pex-polygon-res";
    project.name = "pex-polygon-res";
    const auto body = openvlsi::addLayer(project, "RES", 24);
    const auto metal = openvlsi::addLayer(project, "Metal1", 8);
    const auto cell = openvlsi::createLayoutCell(project, "top", "test.pdk");
    const auto firstNet = openvlsi::createLayoutNet(project, cell, "A");
    const auto secondNet = openvlsi::createLayoutNet(project, cell, "B");
    openvlsi::addRectangle(project, cell, body, {0, 0, 2000, 500});
    openvlsi::addRectangle(project, cell, metal, {0, 0, 300, 500}, firstNet);
    openvlsi::addRectangle(project, cell, metal, {1700, 0, 300, 500}, secondNet);

    const auto rulesPath = std::filesystem::temp_directory_path() / "openvlsi-pex-polygon-res-rules.json";
    std::ofstream(rulesPath) << R"({
      "format": "openvlsi-device-extraction",
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

    openvlsi::PexOptions options;
    options.extractionRulesPath = rulesPath;
    options.includeLinkedSchematic = false;
    const auto report = openvlsi::runPex(project, cell, options);
    std::filesystem::remove(rulesPath);

    CHECK(report.devices.size() == 1);
    if (report.devices.size() != 1)
        return;
    CHECK(report.devices.front().type == "resistor");
    CHECK(report.devices.front().model == "RES_TEST");
    CHECK(report.devices.front().spiceLine == "R1 A B 4000");
}

TEST_CASE("PEX resistor rules can accept terminals that touch the body") {
    openvlsi::Project project;
    project.identity = "pex-touch-res";
    project.name = "pex-touch-res";
    const auto body = openvlsi::addLayer(project, "PolyRes", 128);
    const auto terminals = openvlsi::addLayer(project, "GatPoly", 5);
    const auto cell = openvlsi::createLayoutCell(project, "top", "test.pdk");
    const auto firstNet = openvlsi::createLayoutNet(project, cell, "A");
    const auto secondNet = openvlsi::createLayoutNet(project, cell, "B");
    openvlsi::addRectangle(project, cell, body, {0, 0, 2000, 500});
    openvlsi::addRectangle(project, cell, terminals, {-500, 0, 500, 500}, firstNet);
    openvlsi::addRectangle(project, cell, terminals, {2000, 0, 500, 500}, secondNet);

    const auto rulesPath = std::filesystem::temp_directory_path() / "openvlsi-pex-touch-res-rules.json";
    std::ofstream(rulesPath) << R"({
      "format": "openvlsi-device-extraction",
      "schema_version": 1,
      "devices": [
        {
          "name": "poly_res",
          "type": "resistor",
          "model": "RES_TEST",
          "element": "R",
          "body": "PolyRes",
          "terminal_layers": ["GatPoly"],
          "sheet_resistance": 1000.0,
          "terminal_overlap": "touch"
        }
      ]
    })";

    openvlsi::PexOptions options;
    options.extractionRulesPath = rulesPath;
    options.includeLinkedSchematic = false;
    const auto report = openvlsi::runPex(project, cell, options);
    std::filesystem::remove(rulesPath);

    CHECK(report.devices.size() == 1);
    if (report.devices.size() != 1)
        return;
    CHECK(report.devices.front().type == "resistor");
    CHECK(report.devices.front().model == "RES_TEST");
    CHECK(report.devices.front().spiceLine == "R1 A B 4000");
}

TEST_CASE("PEX extracts planned passive devices from procedural layouts") {
    openvlsi::Project project;
    project.identity = "pex-planned";
    project.name = "pex-planned";
    const auto cell = openvlsi::createLayoutCell(project, "top");
    const auto metal = openvlsi::addLayer(project, "met1.drawing", 68, 0, {}, 20);
    const auto netA = openvlsi::createLayoutNet(project, cell, "A");
    const auto netB = openvlsi::createLayoutNet(project, cell, "B");
    const auto netC = openvlsi::createLayoutNet(project, cell, "C");
    openvlsi::addRectangle(project, cell, metal, {0, 0, 1000, 1000}, netA);
    openvlsi::addRectangle(project, cell, metal, {5000, 0, 1000, 1000}, netB);
    openvlsi::addRectangle(project, cell, metal, {10000, 0, 1000, 1000}, netC);

    auto &layout = project.layoutCells.at(cell);
    openvlsi::PlannedDevice resistor;
    resistor.id = project.nextId();
    resistor.name = "RBIAS";
    resistor.device = "res_poly";
    resistor.model = "sky130_fd_pr__res_high_po";
    resistor.parameters.push_back({project.nextId(), "resistance", "55k"});
    resistor.terminals = {{"1", netA}, {"2", netB}};
    resistor.placeholder = false;
    layout.plannedDevices.emplace(resistor.id, resistor);

    openvlsi::PlannedDevice capacitor;
    capacitor.id = project.nextId();
    capacitor.name = "CC";
    capacitor.device = "mim_cap";
    capacitor.model = "sky130_fd_pr__cap_mim_m3_1";
    capacitor.parameters.push_back({project.nextId(), "capacitance", "3p"});
    capacitor.terminals = {{"1", netB}, {"2", netC}};
    capacitor.placeholder = false;
    layout.plannedDevices.emplace(capacitor.id, capacitor);

    const auto report = openvlsi::runPex(project, cell);
    CHECK(report.devices.size() == 2);
    if (report.devices.size() != 2)
        return;
    CHECK(report.devices[0].type == "resistor");
    CHECK(report.devices[0].spiceLine.find(" A B 55k") != std::string::npos);
    CHECK(report.devices[1].type == "capacitor");
    CHECK(report.devices[1].spiceLine.find(" B C 3p") != std::string::npos);
    CHECK(project.schematicCells.at(report.schematicCell).spiceText.find("planned layout device(s)") !=
          std::string::npos);
}

TEST_CASE("PEX keeps planned MOS terminal order without polygon extraction") {
    openvlsi::Project project;
    project.identity = "pex-planned-mos";
    project.name = "pex-planned-mos";
    const auto cell = openvlsi::createLayoutCell(project, "top");
    const auto metal = openvlsi::addLayer(project, "met1.drawing", 68, 0, {}, 20);
    const auto netD = openvlsi::createLayoutNet(project, cell, "DRAIN");
    const auto netG = openvlsi::createLayoutNet(project, cell, "GATE");
    const auto netS = openvlsi::createLayoutNet(project, cell, "SOURCE");
    const auto netB = openvlsi::createLayoutNet(project, cell, "BULK");
    openvlsi::addRectangle(project, cell, metal, {0, 0, 1000, 1000}, netD);
    openvlsi::addRectangle(project, cell, metal, {5000, 0, 1000, 1000}, netG);
    openvlsi::addRectangle(project, cell, metal, {10000, 0, 1000, 1000}, netS);
    openvlsi::addRectangle(project, cell, metal, {15000, 0, 1000, 1000}, netB);

    auto &layout = project.layoutCells.at(cell);
    openvlsi::PlannedDevice mos;
    mos.id = project.nextId();
    mos.name = "M1";
    mos.device = "nfet";
    mos.model = "sky130_fd_pr__nfet_01v8";
    mos.parameters.push_back({project.nextId(), "w", "14u"});
    mos.parameters.push_back({project.nextId(), "l", "1u"});
    mos.terminals = {{"D", netD}, {"G", netG}, {"S", netS}, {"B", netB}};
    mos.placeholder = false;
    layout.plannedDevices.emplace(mos.id, mos);

    // Polygon extraction is disabled: the planned metadata alone must provide
    // the device, and its D/G/S/B order must come from the pin metadata rather
    // than from the geometry, so mirrored and rotated instances stay correct.
    openvlsi::PexOptions options;
    options.extractPolygonDevices = false;
    const auto report = openvlsi::runPex(project, cell, options);
    CHECK(report.devices.size() == 1);
    if (report.devices.size() != 1)
        return;
    CHECK(report.devices[0].type == "mos");
    const std::vector<std::string> expectedNodes{"DRAIN", "GATE", "SOURCE", "BULK"};
    CHECK(report.devices[0].nodes == expectedNodes);
    CHECK(report.devices[0].spiceLine.find(" w=14u l=1u") != std::string::npos);
}

TEST_CASE("PEX copies the linked schematic device netlist") {
    openvlsi::Project project;
    project.identity = "pex-linked";
    project.name = "pex-linked";
    const auto schematic = openvlsi::createSchematicCell(project, "top");
    const auto symbol = openvlsi::addSymbol(project, schematic, "resistor");
    const auto firstPin = project.nextId();
    const auto secondPin = project.nextId();
    auto &definition = project.schematicCells.at(schematic).symbols.at(symbol);
    definition.pins.push_back({firstPin, "A", 0, 0, openvlsi::PinDirection::left});
    definition.pins.push_back({secondPin, "B", 0, 0, openvlsi::PinDirection::right});
    openvlsi::addSchematicInstance(project, schematic, symbol, "R1", 0, 0);
    openvlsi::addNet(project, schematic, "VIN", {firstPin, secondPin});

    const auto cell = openvlsi::createLayoutCell(project, "top");
    const auto metal = openvlsi::addLayer(project, "Metal1", 8);
    const auto net = openvlsi::createLayoutNet(project, cell, "VIN");
    openvlsi::addRectangle(project, cell, metal, {0, 0, 2000, 1000}, net);

    const auto report = openvlsi::runPex(project, cell);
    CHECK(report.devicesFromSchematic);
    CHECK(!report.devicesFromLayout);
    const auto &text = project.schematicCells.at(report.schematicCell).spiceText;
    CHECK(text.find("Devices from linked schematic 'top'") != std::string::npos);
    CHECK(text.find("R1 VIN VIN 1k") != std::string::npos);
}

TEST_CASE("PEX does not overwrite an unrelated text schematic with the same name") {
    openvlsi::Project project;
    project.identity = "pex-name";
    project.name = "pex-name";
    const auto cell = openvlsi::createLayoutCell(project, "top");
    const auto metal = openvlsi::addLayer(project, "Metal1", 8);
    const auto net = openvlsi::createLayoutNet(project, cell, "VIN");
    openvlsi::addRectangle(project, cell, metal, {0, 0, 2000, 1000}, net);
    const auto existing = openvlsi::createTextSchematicCell(project, "top PEX", openvlsi::genericPdkId, "* user text\n.end\n");

    const auto report = openvlsi::runPex(project, cell);
    CHECK(report.schematicCell != existing);
    CHECK(report.schematic == "top PEX 2");
    CHECK(project.schematicCells.at(existing).spiceText == "* user text\n.end\n");
}
