#include "openvlsilib/agent_api.hpp"
#include "openvlsilib/operations.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <vector>

TEST_CASE("agent component helpers create a complete RC circuit") {
    openvlsi::Project project;
    project.identity = "agent-rc";
    project.name = "agent-rc";
    const auto cell = openvlsi::createSchematicCell(project, "rc");

    openvlsi::CircuitRequest request;
    request.components.push_back({"resistor", {}, "R1", 0, 0, {{0, "resistance", "10k"}}});
    request.components.push_back({"capacitor", {}, "C1", 0, 0, {{0, "capacitance", "1n"}}});
    request.components.push_back({"voltage source", {}, "V1", 0, 0, {{0, "value", "PULSE(0 1 1n 1n 1n 10n 20n)"}}});
    request.nets.push_back({"out", {"R1.1", "C1.1", "V1.+"}, 0, 0});
    request.nets.push_back({"0", {"R1.2", "C1.2", "V1.-"}, 0, 0});

    const auto placement = openvlsi::addSchematicCircuit(project, cell, request);
    CHECK(placement.components.size() == 3);
    CHECK(placement.nets.size() == 2);
    CHECK(placement.components[0].pins.size() == 2);

    const auto diagnostics = openvlsi::validateSchematic(project, cell);
    CHECK(std::none_of(diagnostics.begin(), diagnostics.end(), [](const openvlsi::Diagnostic &diagnostic) {
        return diagnostic.severity == openvlsi::Diagnostic::Severity::error;
    }));

    const auto netlist = openvlsi::buildSchematicNetlist(project, cell);
    std::string joined;
    for (const auto &line : netlist)
        joined += line + "\n";
    CHECK(joined.find("R1 out 0 10k") != std::string::npos);
    CHECK(joined.find("C1 out 0 1n") != std::string::npos);
    CHECK(joined.find("V1 out 0 PULSE(0 1 1n 1n 1n 10n 20n)") != std::string::npos);
}

TEST_CASE("agent validation reports unconnected pins") {
    openvlsi::Project project;
    project.identity = "agent-validation";
    project.name = "agent-validation";
    const auto cell = openvlsi::createSchematicCell(project, "top");
    openvlsi::ComponentRequest request;
    request.type = "resistor";
    request.name = "R1";
    request.parameters.push_back({0, "resistance", "1k"});
    openvlsi::addSchematicComponent(project, cell, request);

    const auto diagnostics = openvlsi::validateSchematic(project, cell);
    CHECK(std::any_of(diagnostics.begin(), diagnostics.end(), [](const openvlsi::Diagnostic &diagnostic) {
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
    CHECK(openvlsi::spiceNetlistSupportsDrawn(source));

    openvlsi::Project project;
    project.identity = "agent-import";
    project.name = "agent-import";
    const auto imported = openvlsi::createSchematicFromSpice(project, "rc_import", openvlsi::genericPdkId, source, true);
    CHECK(imported.cell != 0);
    if (imported.cell == 0)
        return;
    const auto &cell = project.schematicCells.at(imported.cell);
    CHECK(cell.instances.size() == 3);
    CHECK(cell.nets.size() == 3);

    const auto netlist = openvlsi::buildSchematicNetlist(project, imported.cell);
    std::string joined;
    for (const auto &line : netlist)
        joined += line + "\n";
    CHECK(joined.find("V1 in 0 PULSE(0 1 1n 1n 1n 10n 20n)") != std::string::npos);
    CHECK(joined.find("R1 in out 1k") != std::string::npos);
    CHECK(joined.find("C1 out 0 1n") != std::string::npos);

    CHECK(!openvlsi::spiceNetlistSupportsDrawn("X1 a b subckt\n"));
}

TEST_CASE("agent symbol library exposes primitive components") {
    const auto symbols = openvlsi::availableSymbols(openvlsi::genericPdkId);
    const auto has = [&](const std::string &name) {
        return std::any_of(symbols.begin(), symbols.end(),
                           [&](const openvlsi::FileSymbol &symbol) { return symbol.name == name; });
    };
    CHECK(has("Resistor"));
    CHECK(has("Capacitor"));
    CHECK(has("Voltage Source"));
    CHECK(has("Ground"));
    CHECK(has("Pin"));
}

TEST_CASE("agent deletion plans expose dependencies and cascade safely") {
    openvlsi::Project project;
    project.identity = "agent-delete";
    project.name = "agent-delete";
    const auto child = openvlsi::createSchematicCell(project, "child");
    const auto parent = openvlsi::createSchematicCell(project, "parent");
    const auto symbol = openvlsi::addSymbol(project, parent, "child", {}, child);
    const auto pin = project.nextId();
    project.schematicCells.at(parent).symbols.at(symbol).pins.push_back({pin, "P", 0, 0});
    const auto instance = openvlsi::addSchematicInstance(project, parent, symbol, "U1", 0, 0);
    openvlsi::SimulationItem simulation;
    simulation.id = project.nextId();
    simulation.name = "child_sim";
    simulation.schematic = child;
    project.simulations.emplace(simulation.id, simulation);

    const auto plan = openvlsi::planSchematicDeletion(project, child);
    CHECK(plan.blocked());
    CHECK(plan.simulations.size() == 1);
    CHECK(plan.symbols.size() == 1);
    CHECK(plan.symbols.front().instances == std::vector<openvlsi::ObjectId>{instance});

    openvlsi::removeSchematicCascade(project, child);
    CHECK(!project.schematicCells.contains(child));
    CHECK(project.simulations.empty());
    CHECK(project.schematicCells.at(parent).symbols.empty());
    CHECK(project.schematicCells.at(parent).instances.empty());
}

TEST_CASE("agent pin resolver supports instance pin names") {
    openvlsi::Project project;
    project.identity = "agent-pins";
    project.name = "agent-pins";
    const auto cell = openvlsi::createSchematicCell(project, "top");
    openvlsi::ComponentRequest request;
    request.type = "resistor";
    request.name = "R1";
    const auto placement = openvlsi::addSchematicComponent(project, cell, request);
    const auto resolved = openvlsi::resolveSchematicPin(project.schematicCells.at(cell), "R1.1");
    CHECK(resolved == placement.pins.front().id);
}

TEST_CASE("repeated component placement keeps unique pins and symbol defaults") {
    openvlsi::Project project;
    project.identity = "agent-repeat";
    project.name = "agent-repeat";
    const auto cell = openvlsi::createSchematicCell(project, "pulses");

    openvlsi::ComponentRequest first;
    first.type = "pulse voltage source";
    first.name = "V1";
    first.parameters.push_back({0, "delay_time", "5n"});
    const auto v1 = openvlsi::addSchematicComponent(project, cell, first);

    // Adding the same library symbol again copies it; the copy must get its own
    // pin ids and keep the defaults that were seeded on the first placement.
    openvlsi::ComponentRequest second;
    second.type = "pulse voltage source";
    second.name = "V2";
    const auto v2 = openvlsi::addSchematicComponent(project, cell, second);

    const auto &placed = project.schematicCells.at(cell);
    CHECK(v1.instance != v2.instance);
    CHECK(v1.symbol != v2.symbol);

    std::vector<openvlsi::ObjectId> pinIds;
    for (const auto &[_, symbol] : placed.symbols)
        for (const auto &pin : symbol.pins)
            pinIds.push_back(pin.id);
    CHECK(pinIds.size() == 4);
    std::sort(pinIds.begin(), pinIds.end());
    CHECK(std::adjacent_find(pinIds.begin(), pinIds.end()) == pinIds.end());

    const auto parameterValue = [](const openvlsi::SchematicInstance &instance, const std::string &name) {
        for (const auto &parameter : instance.parameters) {
            if (parameter.name == name)
                return parameter.value;
        }
        return std::string{};
    };
    const auto &firstInstance = placed.instances.at(v1.instance);
    const auto &secondInstance = placed.instances.at(v2.instance);
    CHECK(parameterValue(firstInstance, "delay_time") == "5n");
    CHECK(parameterValue(secondInstance, "delay_time") == "5n");
    CHECK(parameterValue(secondInstance, "period") == "1");
    CHECK(parameterValue(secondInstance, "rise_time") == "1u");

    // Both sources netlist with a pulse expression, so the copy kept its defaults.
    const auto netlist = openvlsi::buildSchematicNetlist(project, cell);
    const auto pulseLines = std::count_if(netlist.begin(), netlist.end(), [](const std::string &line) {
        return line.find("PULSE(") != std::string::npos;
    });
    CHECK(pulseLines == 2);
}
