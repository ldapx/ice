#include "icelib/agent_api.hpp"
#include "icelib/operations.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <vector>

TEST_CASE("agent component helpers create a complete RC circuit") {
    ice::Project project;
    project.identity = "agent-rc";
    project.name = "agent-rc";
    const auto cell = ice::createSchematicCell(project, "rc");

    ice::CircuitRequest request;
    request.components.push_back({"resistor", {}, "R1", 0, 0, {{0, "resistance", "10k"}}});
    request.components.push_back({"capacitor", {}, "C1", 0, 0, {{0, "capacitance", "1n"}}});
    request.components.push_back({"voltage source", {}, "V1", 0, 0, {{0, "value", "PULSE(0 1 1n 1n 1n 10n 20n)"}}});
    request.nets.push_back({"out", {"R1.1", "C1.1", "V1.+"}, 0, 0});
    request.nets.push_back({"0", {"R1.2", "C1.2", "V1.-"}, 0, 0});

    const auto placement = ice::addSchematicCircuit(project, cell, request);
    CHECK(placement.components.size() == 3);
    CHECK(placement.nets.size() == 2);
    CHECK(placement.components[0].pins.size() == 2);

    const auto diagnostics = ice::validateSchematic(project, cell);
    CHECK(std::none_of(diagnostics.begin(), diagnostics.end(), [](const ice::Diagnostic &diagnostic) {
        return diagnostic.severity == ice::Diagnostic::Severity::error;
    }));

    const auto netlist = ice::buildSchematicNetlist(project, cell);
    std::string joined;
    for (const auto &line : netlist)
        joined += line + "\n";
    CHECK(joined.find("R1 out 0 10k") != std::string::npos);
    CHECK(joined.find("C1 out 0 1n") != std::string::npos);
    CHECK(joined.find("V1 out 0 PULSE(0 1 1n 1n 1n 10n 20n)") != std::string::npos);
}

TEST_CASE("agent validation reports unconnected pins") {
    ice::Project project;
    project.identity = "agent-validation";
    project.name = "agent-validation";
    const auto cell = ice::createSchematicCell(project, "top");
    ice::ComponentRequest request;
    request.type = "resistor";
    request.name = "R1";
    request.parameters.push_back({0, "resistance", "1k"});
    ice::addSchematicComponent(project, cell, request);

    const auto diagnostics = ice::validateSchematic(project, cell);
    CHECK(std::any_of(diagnostics.begin(), diagnostics.end(), [](const ice::Diagnostic &diagnostic) {
        return diagnostic.code == "schematic.pin.unconnected";
    }));
}

TEST_CASE("agent SPICE import can create a drawn RC circuit") {
    const std::string source = R"(* RC low-pass
V1 in 0 PULSE(0 1 1n 1n 1n 10n 20n)
R1 in out 1k
C1 out 0 1n
.end
)";
    CHECK(ice::spiceNetlistSupportsDrawn(source));

    ice::Project project;
    project.identity = "agent-import";
    project.name = "agent-import";
    const auto imported = ice::createSchematicFromSpice(project, "rc_import", ice::genericPdkId, source, true);
    CHECK(imported.cell != 0);
    if (imported.cell == 0)
        return;
    const auto &cell = project.schematicCells.at(imported.cell);
    CHECK(cell.instances.size() == 3);
    CHECK(cell.nets.size() == 3);

    const auto netlist = ice::buildSchematicNetlist(project, imported.cell);
    std::string joined;
    for (const auto &line : netlist)
        joined += line + "\n";
    CHECK(joined.find("V1 in 0 PULSE(0 1 1n 1n 1n 10n 20n)") != std::string::npos);
    CHECK(joined.find("R1 in out 1k") != std::string::npos);
    CHECK(joined.find("C1 out 0 1n") != std::string::npos);

    CHECK(!ice::spiceNetlistSupportsDrawn("X1 a b subckt\n"));
}

TEST_CASE("agent symbol library exposes primitive components") {
    const auto symbols = ice::availableSymbols(ice::genericPdkId);
    const auto has = [&](const std::string &name) {
        return std::any_of(symbols.begin(), symbols.end(),
                           [&](const ice::FileSymbol &symbol) { return symbol.name == name; });
    };
    CHECK(has("Resistor"));
    CHECK(has("Capacitor"));
    CHECK(has("Voltage Source"));
    CHECK(has("Ground"));
}

TEST_CASE("agent deletion plans expose dependencies and cascade safely") {
    ice::Project project;
    project.identity = "agent-delete";
    project.name = "agent-delete";
    const auto child = ice::createSchematicCell(project, "child");
    const auto parent = ice::createSchematicCell(project, "parent");
    const auto symbol = ice::addSymbol(project, parent, "child", {}, child);
    const auto pin = project.nextId();
    project.schematicCells.at(parent).symbols.at(symbol).pins.push_back({pin, "P", 0, 0});
    const auto instance = ice::addSchematicInstance(project, parent, symbol, "U1", 0, 0);
    ice::SimulationItem simulation;
    simulation.id = project.nextId();
    simulation.name = "child_sim";
    simulation.schematic = child;
    project.simulations.emplace(simulation.id, simulation);

    const auto plan = ice::planSchematicDeletion(project, child);
    CHECK(plan.blocked());
    CHECK(plan.simulations.size() == 1);
    CHECK(plan.symbols.size() == 1);
    CHECK(plan.symbols.front().instances == std::vector<ice::ObjectId>{instance});

    ice::removeSchematicCascade(project, child);
    CHECK(!project.schematicCells.contains(child));
    CHECK(project.simulations.empty());
    CHECK(project.schematicCells.at(parent).symbols.empty());
    CHECK(project.schematicCells.at(parent).instances.empty());
}

TEST_CASE("agent pin resolver supports instance pin names") {
    ice::Project project;
    project.identity = "agent-pins";
    project.name = "agent-pins";
    const auto cell = ice::createSchematicCell(project, "top");
    ice::ComponentRequest request;
    request.type = "resistor";
    request.name = "R1";
    const auto placement = ice::addSchematicComponent(project, cell, request);
    const auto resolved = ice::resolveSchematicPin(project.schematicCells.at(cell), "R1.1");
    CHECK(resolved == placement.pins.front().id);
}
