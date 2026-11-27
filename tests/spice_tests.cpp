#include "openvlsilib/agent_api.hpp"
#include "openvlsilib/operations.hpp"
#include "openvlsilib/spice.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

TEST_CASE("SPICE parser builds subcircuit hierarchy and parameter defaults") {
    const std::string source = R"(
* hierarchical filter
.title rc filter
.include "models/rc.lib"
.lib "models/transistors.lib" tt
.param VDD=1.8 TEMP=27
.model dmod D (is=1e-14 n=1.05)
.subckt filter IN OUT params: R=1k C=1n
R1 IN MID {R}
C1 MID OUT {C}
D1 MID 0 dmod
E1 OUT 0 MID 0 2
F1 OUT 0 V1 0.5
Xstage IN MID dmod W=2u L=0.18u
.ends filter
)";
    const auto document = openvlsi::parseSpiceDocument(source);
    CHECK(document.title == "rc filter");
    CHECK(document.includes.size() == 1);
    CHECK(document.libraries.size() == 1);
    CHECK(document.parameters.size() == 2);
    CHECK(document.models.size() == 1);
    REQUIRE(document.subcircuits.size() == 1);

    const auto *filter = openvlsi::findSpiceSubcircuit(document, "FILTER");
    REQUIRE(filter != nullptr);
    CHECK((filter->ports == std::vector<std::string>{"IN", "OUT"}));
    REQUIRE(filter->parameters.size() == 2);
    CHECK(filter->parameters[0].name == "R");
    CHECK(filter->parameters[0].value == "1k");
    CHECK(filter->parameters[1].name == "C");
    CHECK(filter->parameters[1].value == "1n");
    REQUIRE(filter->elements.size() == 6);
    CHECK(filter->elements[0].kind == 'r');
    CHECK(filter->elements[0].value == "{R}");
    CHECK(filter->elements[1].kind == 'c');
    CHECK(filter->elements[2].kind == 'd');
    CHECK(filter->elements[2].model == "dmod");
    CHECK(filter->elements[3].kind == 'e');
    CHECK(filter->elements[3].nodes.size() == 4);
    CHECK(filter->elements[4].kind == 'f');
    CHECK(filter->elements[4].model == "V1");
    CHECK(filter->elements[5].kind == 'x');
    CHECK((filter->elements[5].nodes == std::vector<std::string>{"IN", "MID"}));
    CHECK(filter->elements[5].model == "dmod");
    CHECK(filter->elements[5].parameters.at("w") == "2u");
}

TEST_CASE("SPICE parser expands bus ports and chooses the cell subcircuit") {
    const std::string source = R"(
.subckt other A B
R1 A B 1k
.ends other
.subckt data_block data[7:0] out[1:0] params: WIDTH=8
R1 data[0] out[0] 1k
.ends data_block
)";
    const auto interface = openvlsi::spiceInterfaceForText(source, "data_block");
    REQUIRE(interface.ok);
    CHECK(interface.subcircuitName == "data_block");
    REQUIRE(interface.ports.size() == 10);
    CHECK(interface.ports[0] == "data[0]");
    CHECK(interface.ports[7] == "data[7]");
    CHECK(interface.ports[8] == "out[0]");
    CHECK(interface.ports[9] == "out[1]");
    REQUIRE(interface.parameters.size() == 1);
    CHECK(interface.parameters[0].name == "WIDTH");

    const auto ambiguous = openvlsi::spiceInterfaceForText(source, "missing");
    CHECK(!ambiguous.ok);
    CHECK(ambiguous.error.find("several subcircuits") != std::string::npos);
}

TEST_CASE("SPICE hierarchy strips analysis directives but keeps definitions") {
    const std::string source = R"(
.tran 1n 10n
.ac dec 10 1 1G
.title ignored
.param A=1
.subckt block IN OUT
R1 IN OUT {A}
.ends block
.end
)";
    const auto lines = openvlsi::spiceSourceLinesForHierarchy(source);
    CHECK(std::find(lines.begin(), lines.end(), ".subckt block IN OUT") != lines.end());
    CHECK(std::find(lines.begin(), lines.end(), "R1 IN OUT {A}") != lines.end());
    CHECK(std::find(lines.begin(), lines.end(), ".param A=1") != lines.end());
    CHECK(std::find(lines.begin(), lines.end(), ".tran 1n 10n") == lines.end());
    CHECK(std::find(lines.begin(), lines.end(), ".ac dec 10 1 1G") == lines.end());
    CHECK(std::find(lines.begin(), lines.end(), ".end") == lines.end());
}

TEST_CASE("Text SPICE import retains subcircuit parameter defaults") {
    openvlsi::Project project;
    project.identity = "spice-import";
    project.name = "spice-import";
    const auto imported = openvlsi::createSchematicFromSpice(
        project, "filter", openvlsi::genericPdkId,
        ".subckt filter IN OUT params: R=1k C=1n\nR1 IN OUT {R}\nC1 OUT 0 {C}\n.ends filter\n", false);
    REQUIRE(imported.cell != 0);
    const auto &cell = project.schematicCells.at(imported.cell);
    CHECK(cell.textSchematic);
    REQUIRE(cell.parameters.size() == 2);
    CHECK(cell.parameters[0].name == "R");
    CHECK(cell.parameters[0].value == "1k");
}

TEST_CASE("Text SPICE subcircuits become hierarchical blocks with parameter overrides") {
    openvlsi::Project project;
    project.identity = "spice-hierarchy";
    project.name = "spice-hierarchy";
    const auto filter = openvlsi::createTextSchematicCell(
        project, "filter", openvlsi::genericPdkId,
        ".subckt filter IN OUT params: R=1k C=1n\nR1 IN MID {R}\nC1 MID OUT {C}\n.ends filter\n");
    const auto parent = openvlsi::createSchematicCell(project, "top");

    openvlsi::ObjectId symbol = 0;
    const auto block = openvlsi::addTextSpiceInstance(project, parent, filter, "X1", 0, 0, &symbol);
    CHECK(symbol != 0);
    const auto &cell = project.schematicCells.at(parent);
    CHECK(cell.symbols.at(symbol).sourceCell == filter);
    CHECK(cell.symbols.at(symbol).pins.size() == 2);
    CHECK(cell.symbols.at(symbol).pins[0].name == "IN");
    CHECK(cell.symbols.at(symbol).pins[1].name == "OUT");
    CHECK(!cell.symbols.at(symbol).artwork.empty());
    CHECK(cell.symbols.at(symbol).artwork.find("<svg") != std::string::npos);
    CHECK(cell.symbols.at(symbol).artwork.find("filter") != std::string::npos);
    project.schematicCells.at(parent).symbols.at(symbol).artwork.clear();
    CHECK(openvlsi::repairHierarchicalSymbolArtwork(project) >= 1);
    CHECK(!project.schematicCells.at(parent).symbols.at(symbol).artwork.empty());
    CHECK(project.schematicCells.at(filter).parameters.size() == 2);
    openvlsi::setSchematicParameter(project, parent, block, "R", "2k");

    const auto source =
        openvlsi::addSchematicComponent(project, parent, {"voltage source", {}, "V1", 0, 0, {{0, "value", "1"}}});
    const auto resistor =
        openvlsi::addSchematicComponent(project, parent, {"resistor", {}, "R1", 0, 0, {{0, "resistance", "10k"}}});
    const auto pin = [&](const std::string &name) {
        return openvlsi::resolveSchematicPin(project.schematicCells.at(parent), name);
    };
    openvlsi::addNet(project, parent, "in", {pin("X1.IN"), pin("V1.+")});
    openvlsi::addNet(project, parent, "out", {pin("X1.OUT"), pin("R1.1")});
    openvlsi::addNet(project, parent, "0", {pin("V1.-"), pin("R1.2")});

    const auto lines = openvlsi::buildSchematicNetlist(project, parent);
    const auto joined = [&] {
        std::string text;
        for (const auto &line : lines)
            text += line + "\n";
        return text;
    }();
    CHECK(joined.find(".subckt filter IN OUT params: R=1k C=1n") != std::string::npos);
    CHECK(joined.find("X1 in out filter R=2k C=1n") != std::string::npos);
    CHECK(joined.find("R1 IN MID {R}") != std::string::npos);

    const auto secondParent = openvlsi::createSchematicCell(project, "top2");
    const auto placed = openvlsi::addSchematicComponent(project, secondParent, {"filter", {}, "X2", 0, 0, {}});
    CHECK(placed.instance != 0);
    CHECK(placed.pins.size() == 2);
    CHECK(!project.schematicCells.at(secondParent).symbols.at(placed.symbol).artwork.empty());
}

TEST_CASE("Text SPICE bus ports expand into scalar block pins") {
    openvlsi::Project project;
    project.identity = "spice-bus";
    project.name = "spice-bus";
    const auto source = openvlsi::createTextSchematicCell(
        project, "bus_block", openvlsi::genericPdkId,
        ".subckt bus_block data[1:0] out[1:0]\nR1 data[0] out[0] 1k\nR2 data[1] out[1] 1k\n.ends bus_block\n");
    const auto parent = openvlsi::createSchematicCell(project, "top");
    openvlsi::ObjectId symbol = 0;
    openvlsi::addTextSpiceInstance(project, parent, source, "X1", 0, 0, &symbol);
    const auto &pins = project.schematicCells.at(parent).symbols.at(symbol).pins;
    REQUIRE(pins.size() == 4);
    CHECK(pins[0].name == "data[0]");
    CHECK(pins[1].name == "data[1]");
    CHECK(pins[2].name == "out[0]");
    CHECK(pins[3].name == "out[1]");
    const auto lines = openvlsi::buildSchematicNetlist(project, parent);
    CHECK(std::find(lines.begin(), lines.end(), ".subckt bus_block data[0] data[1] out[0] out[1]") != lines.end());
    CHECK(std::find(lines.begin(), lines.end(), "X1 0 0 0 0 bus_block") != lines.end());
}

TEST_CASE("SPICE parser accepts behavioral controlled sources") {
    const auto document = openvlsi::parseSpiceDocument(
        ".subckt amp IN OUT\nE1 OUT 0 VALUE={V(IN)*2}\nG1 0 OUT TABLE {V(IN)}=(0,0) (1,1)\n.ends amp\n");
    CHECK(document.warnings.empty());
    REQUIRE(document.subcircuits.size() == 1);
    REQUIRE(document.subcircuits.front().elements.size() == 2);
    CHECK((document.subcircuits.front().elements[0].nodes == std::vector<std::string>{"OUT", "0"}));
    CHECK(document.subcircuits.front().elements[0].value.find("VALUE") != std::string::npos);
    CHECK((document.subcircuits.front().elements[1].nodes == std::vector<std::string>{"0", "OUT"}));
}

TEST_CASE("SPICE parser records global nets") {
    const auto document = openvlsi::parseSpiceDocument(".global VDD VSS\n.subckt block A\nR1 A VDD 1k\n.ends block\n");
    REQUIRE(document.globals.size() == 2);
    CHECK(document.globals[0] == "VDD");
    CHECK(document.globals[1] == "VSS");
}

TEST_CASE("Editing text SPICE source reconciles parent symbol pins") {
    openvlsi::Project project;
    project.identity = "spice-edit";
    project.name = "spice-edit";
    const auto source = openvlsi::createTextSchematicCell(project, "block", openvlsi::genericPdkId,
                                                     ".subckt block A B\nR1 A B 1k\n.ends block\n");
    const auto parent = openvlsi::createSchematicCell(project, "top");
    openvlsi::ObjectId symbolId = 0;
    openvlsi::addTextSpiceInstance(project, parent, source, "X1", 0, 0, &symbolId);
    const auto changed =
        openvlsi::setSchematicSourceText(project, source, false, ".subckt block C D\nR1 C D 2k\n.ends block\n");
    CHECK(changed.addedPins == 2);
    CHECK(changed.removedPins == 2);
    const auto &pins = project.schematicCells.at(parent).symbols.at(symbolId).pins;
    REQUIRE(pins.size() == 2);
    CHECK(pins[0].name == "C");
    CHECK(pins[1].name == "D");
    const auto &symbol = project.schematicCells.at(parent).symbols.at(symbolId);
    CHECK(symbol.artwork.find(">C<") != std::string::npos);
    CHECK(symbol.artwork.find(">D<") != std::string::npos);
    const auto netlist = openvlsi::buildSchematicNetlist(project, parent);
    CHECK(std::find(netlist.begin(), netlist.end(), "X1 0 0 block") != netlist.end());
}

TEST_CASE("Editing text SPICE source rejects removing connected pins") {
    openvlsi::Project project;
    project.identity = "spice-edit-connected";
    project.name = "spice-edit-connected";
    const auto source = openvlsi::createTextSchematicCell(project, "block", openvlsi::genericPdkId,
                                                     ".subckt block A B\nR1 A B 1k\n.ends block\n");
    const auto parent = openvlsi::createSchematicCell(project, "top");
    openvlsi::ObjectId symbolId = 0;
    openvlsi::addTextSpiceInstance(project, parent, source, "X1", 0, 0, &symbolId);
    auto &parentCell = project.schematicCells.at(parent);
    const auto other = openvlsi::addSymbol(project, parent, "resistor");
    const auto otherPin = project.nextId();
    parentCell.symbols.at(other).pins.push_back({otherPin, "A", 0, 0});
    const auto otherPinB = project.nextId();
    parentCell.symbols.at(other).pins.push_back({otherPinB, "B", 0, 0});
    const auto blockPin = parentCell.symbols.at(symbolId).pins.front().id;
    const auto netId = project.nextId();
    parentCell.nets.emplace(netId, openvlsi::Net{netId, "net", {blockPin, otherPin}});
    CHECK_THROWS_AS(openvlsi::setSchematicSourceText(project, source, false, ".subckt block C D\nR1 C D 2k\n.ends block\n"),
                    openvlsi::ProjectError);
}

TEST_CASE("Text SPICE resolves includes and library sections") {
    const auto root = std::filesystem::temp_directory_path() / "openvlsi-spice-include-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "models");
    std::ofstream(root / "models" / "devices.inc") << ".subckt inc_block A B\nR1 A B 2k\n.ends inc_block\n";
    std::ofstream(root / "models" / "devices.lib")
        << ".lib tt\n.model nch nmos (vto=0.5)\n.subckt lib_block X Y\nR2 X Y 3k\n.ends lib_block\n.endl tt\n";
    const std::string source = ".include \"models/devices.inc\"\n"
                               ".lib \"models/devices.lib\" tt\n"
                               ".subckt top IN OUT\n"
                               "X1 IN OUT inc_block\n"
                               ".ends top\n";
    const auto sourcePath = root / "top.sp";
    std::vector<std::string> warnings;
    const auto document = openvlsi::parseSpiceDocumentResolved(source, sourcePath, &warnings);
    CHECK(warnings.empty());
    CHECK(openvlsi::findSpiceSubcircuit(document, "inc_block") != nullptr);
    CHECK(openvlsi::findSpiceSubcircuit(document, "lib_block") != nullptr);
    CHECK(document.models.size() == 1);
    CHECK(openvlsi::findSpiceSubcircuit(document, "top") != nullptr);

    const auto interface = openvlsi::spiceInterfaceForText(source, "top", sourcePath);
    REQUIRE(interface.ok);
    CHECK((interface.ports == std::vector<std::string>{"IN", "OUT"}));
    CHECK(interface.warnings.empty());

    const auto lines = openvlsi::spiceSourceLinesForHierarchy(source, sourcePath);
    const auto hasAbsoluteInclude = std::any_of(lines.begin(), lines.end(), [&](const std::string &line) {
        return line.find("devices.inc") != std::string::npos && line.find(root.string()) != std::string::npos;
    });
    CHECK(hasAbsoluteInclude);

    const auto missing = openvlsi::spiceInterfaceForText(".include \"missing.inc\"\n.subckt top A B\nR1 A B 1\n.ends top\n",
                                                    "top", sourcePath);
    CHECK(missing.ok);
    CHECK(!missing.warnings.empty());
    CHECK(missing.warnings.front().find("not found") != std::string::npos);

    openvlsi::Project project;
    project.identity = "spice-includes";
    project.name = "spice-includes";
    const auto cell = openvlsi::createTextSchematicCell(project, "top", openvlsi::genericPdkId, source, sourcePath.string());
    const auto cellInterface = openvlsi::spiceInterfaceForCell(project.schematicCells.at(cell));
    CHECK(cellInterface.ok);
    CHECK(cellInterface.subcircuitName == "top");
    const auto serialized = project.serialize();
    CHECK(serialized.find("spice_source_path") != std::string::npos);
    std::filesystem::remove_all(root);
}

TEST_CASE("SPICE element helpers expose LVS-facing types and values") {
    openvlsi::SpiceDocument document;
    const auto parsed = openvlsi::parseSpiceDocument("R1 IN OUT 2k\nM1 D G S B nch W=2u L=0.15u M=2\nX1 A B cell param=3\n");
    REQUIRE(parsed.elements.size() == 3);
    CHECK(openvlsi::spiceElementType(parsed.elements[0]) == "resistor");
    CHECK(openvlsi::spiceElementParameters(parsed.elements[0]).at("resistance") == "2k");
    CHECK(openvlsi::spiceElementType(parsed.elements[1]) == "nch");
    CHECK(openvlsi::spiceElementParameters(parsed.elements[1]).at("w") == "2u");
    CHECK(openvlsi::spiceElementType(parsed.elements[2]) == "cell");
    CHECK(openvlsi::spiceElementParameters(parsed.elements[2]).at("param") == "3");
}
