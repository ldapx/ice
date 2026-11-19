#include "icelib/agent_api.hpp"
#include "icelib/operations.hpp"
#include "icelib/spice.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <string>

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
    const auto document = ice::parseSpiceDocument(source);
    CHECK(document.title == "rc filter");
    CHECK(document.includes.size() == 1);
    CHECK(document.libraries.size() == 1);
    CHECK(document.parameters.size() == 2);
    CHECK(document.models.size() == 1);
    REQUIRE(document.subcircuits.size() == 1);

    const auto *filter = ice::findSpiceSubcircuit(document, "FILTER");
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
    const auto interface = ice::spiceInterfaceForText(source, "data_block");
    REQUIRE(interface.ok);
    CHECK(interface.subcircuitName == "data_block");
    REQUIRE(interface.ports.size() == 10);
    CHECK(interface.ports[0] == "data[0]");
    CHECK(interface.ports[7] == "data[7]");
    CHECK(interface.ports[8] == "out[0]");
    CHECK(interface.ports[9] == "out[1]");
    REQUIRE(interface.parameters.size() == 1);
    CHECK(interface.parameters[0].name == "WIDTH");

    const auto ambiguous = ice::spiceInterfaceForText(source, "missing");
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
    const auto lines = ice::spiceSourceLinesForHierarchy(source);
    CHECK(std::find(lines.begin(), lines.end(), ".subckt block IN OUT") != lines.end());
    CHECK(std::find(lines.begin(), lines.end(), "R1 IN OUT {A}") != lines.end());
    CHECK(std::find(lines.begin(), lines.end(), ".param A=1") != lines.end());
    CHECK(std::find(lines.begin(), lines.end(), ".tran 1n 10n") == lines.end());
    CHECK(std::find(lines.begin(), lines.end(), ".ac dec 10 1 1G") == lines.end());
    CHECK(std::find(lines.begin(), lines.end(), ".end") == lines.end());
}

TEST_CASE("Text SPICE import retains subcircuit parameter defaults") {
    ice::Project project;
    project.identity = "spice-import";
    project.name = "spice-import";
    const auto imported = ice::createSchematicFromSpice(
        project, "filter", ice::genericPdkId,
        ".subckt filter IN OUT params: R=1k C=1n\nR1 IN OUT {R}\nC1 OUT 0 {C}\n.ends filter\n", false);
    REQUIRE(imported.cell != 0);
    const auto &cell = project.schematicCells.at(imported.cell);
    CHECK(cell.textSchematic);
    REQUIRE(cell.parameters.size() == 2);
    CHECK(cell.parameters[0].name == "R");
    CHECK(cell.parameters[0].value == "1k");
}

TEST_CASE("Text SPICE subcircuits become hierarchical blocks with parameter overrides") {
    ice::Project project;
    project.identity = "spice-hierarchy";
    project.name = "spice-hierarchy";
    const auto filter = ice::createTextSchematicCell(
        project, "filter", ice::genericPdkId,
        ".subckt filter IN OUT params: R=1k C=1n\nR1 IN MID {R}\nC1 MID OUT {C}\n.ends filter\n");
    const auto parent = ice::createSchematicCell(project, "top");

    ice::ObjectId symbol = 0;
    const auto block = ice::addTextSpiceInstance(project, parent, filter, "X1", 0, 0, &symbol);
    CHECK(symbol != 0);
    const auto &cell = project.schematicCells.at(parent);
    CHECK(cell.symbols.at(symbol).sourceCell == filter);
    CHECK(cell.symbols.at(symbol).pins.size() == 2);
    CHECK(cell.symbols.at(symbol).pins[0].name == "IN");
    CHECK(cell.symbols.at(symbol).pins[1].name == "OUT");
    CHECK(project.schematicCells.at(filter).parameters.size() == 2);
    ice::setSchematicParameter(project, parent, block, "R", "2k");

    const auto source =
        ice::addSchematicComponent(project, parent, {"voltage source", {}, "V1", 0, 0, {{0, "value", "1"}}});
    const auto resistor =
        ice::addSchematicComponent(project, parent, {"resistor", {}, "R1", 0, 0, {{0, "resistance", "10k"}}});
    const auto pin = [&](const std::string &name) {
        return ice::resolveSchematicPin(project.schematicCells.at(parent), name);
    };
    ice::addNet(project, parent, "in", {pin("X1.IN"), pin("V1.+")});
    ice::addNet(project, parent, "out", {pin("X1.OUT"), pin("R1.1")});
    ice::addNet(project, parent, "0", {pin("V1.-"), pin("R1.2")});

    const auto lines = ice::buildSchematicNetlist(project, parent);
    const auto joined = [&] {
        std::string text;
        for (const auto &line : lines)
            text += line + "\n";
        return text;
    }();
    CHECK(joined.find(".subckt filter IN OUT params: R=1k C=1n") != std::string::npos);
    CHECK(joined.find("X1 in out filter R=2k C=1n") != std::string::npos);
    CHECK(joined.find("R1 IN MID {R}") != std::string::npos);

    const auto secondParent = ice::createSchematicCell(project, "top2");
    const auto placed = ice::addSchematicComponent(project, secondParent, {"filter", {}, "X2", 0, 0, {}});
    CHECK(placed.instance != 0);
    CHECK(placed.pins.size() == 2);
}

TEST_CASE("Text SPICE bus ports expand into scalar block pins") {
    ice::Project project;
    project.identity = "spice-bus";
    project.name = "spice-bus";
    const auto source = ice::createTextSchematicCell(
        project, "bus_block", ice::genericPdkId,
        ".subckt bus_block data[1:0] out[1:0]\nR1 data[0] out[0] 1k\nR2 data[1] out[1] 1k\n.ends bus_block\n");
    const auto parent = ice::createSchematicCell(project, "top");
    ice::ObjectId symbol = 0;
    ice::addTextSpiceInstance(project, parent, source, "X1", 0, 0, &symbol);
    const auto &pins = project.schematicCells.at(parent).symbols.at(symbol).pins;
    REQUIRE(pins.size() == 4);
    CHECK(pins[0].name == "data[0]");
    CHECK(pins[1].name == "data[1]");
    CHECK(pins[2].name == "out[0]");
    CHECK(pins[3].name == "out[1]");
    const auto lines = ice::buildSchematicNetlist(project, parent);
    CHECK(std::find(lines.begin(), lines.end(), "X1 0 0 0 0 bus_block") != lines.end());
}

TEST_CASE("SPICE element helpers expose LVS-facing types and values") {
    ice::SpiceDocument document;
    const auto parsed = ice::parseSpiceDocument("R1 IN OUT 2k\nM1 D G S B nch W=2u L=0.15u M=2\nX1 A B cell param=3\n");
    REQUIRE(parsed.elements.size() == 3);
    CHECK(ice::spiceElementType(parsed.elements[0]) == "resistor");
    CHECK(ice::spiceElementParameters(parsed.elements[0]).at("resistance") == "2k");
    CHECK(ice::spiceElementType(parsed.elements[1]) == "nch");
    CHECK(ice::spiceElementParameters(parsed.elements[1]).at("w") == "2u");
    CHECK(ice::spiceElementType(parsed.elements[2]) == "cell");
    CHECK(ice::spiceElementParameters(parsed.elements[2]).at("param") == "3");
}
