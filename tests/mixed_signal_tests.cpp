#include "openvlsilib/agent_api.hpp"
#include "openvlsilib/hdl.hpp"
#include "openvlsilib/operations.hpp"
#include "openvlsilib/simulation.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
const openvlsi::SimulationTrace *findTrace(const std::vector<openvlsi::SimulationTrace> &traces, const std::string &name) {
    for (const auto &trace : traces) {
        if (trace.name == name || trace.name.ends_with("." + name))
            return &trace;
    }
    return nullptr;
}

openvlsi::ObjectId addPin(openvlsi::Project &project, openvlsi::ObjectId cellId, openvlsi::ObjectId symbolId, std::string name,
                     openvlsi::PinDirection direction = openvlsi::PinDirection::right) {
    auto &cell = project.schematicCells.at(cellId);
    const auto pinId = project.nextId();
    cell.symbols.at(symbolId).pins.push_back({pinId, std::move(name), 0, 0, direction});
    return pinId;
}

openvlsi::ObjectId pinNamed(const openvlsi::Project &project, openvlsi::ObjectId cellId, openvlsi::ObjectId symbolId,
                       const std::string &name) {
    const auto &cell = project.schematicCells.at(cellId);
    for (const auto &pin : cell.symbols.at(symbolId).pins) {
        if (pin.name == name)
            return pin.id;
    }
    return 0;
}
} // namespace

TEST_CASE("SystemVerilog port parser handles ANSI and legacy declarations") {
    const std::string ansi = R"(
module mixed_block (
    input logic clk,
    input logic rst_n,
    input logic [3:0] data,
    output logic [3:0] q,
    inout wire io
);
endmodule
)";
    const auto parsed = openvlsi::parseSystemVerilogModule(ansi, "mixed_block");
    CHECK(parsed.ok);
    if (!parsed.ok)
        return;
    CHECK(parsed.module.name == "mixed_block");
    CHECK(parsed.module.ports.size() == 5);
    if (parsed.module.ports.size() != 5)
        return;
    CHECK(parsed.module.ports[0].name == "clk");
    CHECK(parsed.module.ports[0].direction == openvlsi::HdlPortDirection::input);
    CHECK(parsed.module.ports[0].width == 1);
    CHECK(parsed.module.ports[2].name == "data");
    CHECK(parsed.module.ports[2].width == 4);
    CHECK(parsed.module.ports[3].name == "q");
    CHECK(parsed.module.ports[3].direction == openvlsi::HdlPortDirection::output);
    CHECK(parsed.module.ports[3].width == 4);
    CHECK(parsed.module.ports[4].direction == openvlsi::HdlPortDirection::inout);

    const std::string legacy = R"(
module legacy(a, b, c);
    input a;
    output [1:0] b;
    inout c;
endmodule
)";
    const auto legacyParsed = openvlsi::parseSystemVerilogModule(legacy, "legacy");
    CHECK(legacyParsed.ok);
    if (!legacyParsed.ok)
        return;
    CHECK(legacyParsed.module.ports.size() == 3);
    if (legacyParsed.module.ports.size() != 3)
        return;
    CHECK(legacyParsed.module.ports[0].direction == openvlsi::HdlPortDirection::input);
    CHECK(legacyParsed.module.ports[1].direction == openvlsi::HdlPortDirection::output);
    CHECK(legacyParsed.module.ports[1].width == 2);
    CHECK(legacyParsed.module.ports[2].direction == openvlsi::HdlPortDirection::inout);
}

TEST_CASE("SystemVerilog port parser inherits direction and width") {
    const std::string source = R"(
module inherit(input logic [7:0] a, b, output logic [1:0] c, d);
endmodule
)";
    const auto parsed = openvlsi::parseSystemVerilogModule(source, "inherit");
    CHECK(parsed.ok);
    if (!parsed.ok)
        return;
    CHECK(parsed.module.ports.size() == 4);
    if (parsed.module.ports.size() != 4)
        return;
    CHECK(parsed.module.ports[0].name == "a");
    CHECK(parsed.module.ports[1].name == "b");
    CHECK(parsed.module.ports[0].direction == openvlsi::HdlPortDirection::input);
    CHECK(parsed.module.ports[1].direction == openvlsi::HdlPortDirection::input);
    CHECK(parsed.module.ports[0].width == 8);
    CHECK(parsed.module.ports[1].width == 8);
    CHECK(parsed.module.ports[2].direction == openvlsi::HdlPortDirection::output);
    CHECK(parsed.module.ports[3].direction == openvlsi::HdlPortDirection::output);
    CHECK(parsed.module.ports[2].width == 2);
    CHECK(parsed.module.ports[3].width == 2);
}

TEST_CASE("SystemVerilog parser prefers a block over its testbench") {
    const std::string source = R"(
module dut(input logic a, output logic y);
    assign y = a;
endmodule
module dut_tb;
    logic a;
    logic y;
    dut u(.a(a), .y(y));
endmodule
)";
    const auto parsed = openvlsi::parseSystemVerilogModule(source);
    CHECK(parsed.ok);
    if (!parsed.ok)
        return;
    CHECK(parsed.module.name == "dut");
    CHECK(parsed.module.ports.size() == 2);
}

TEST_CASE("SystemVerilog block instances get generated symbols and a mixed plan") {
    openvlsi::Project project;
    project.identity = "mixed-plan";
    project.name = "mixed-plan";
    const auto block =
        openvlsi::createSystemVerilogCell(project, "buffer", openvlsi::genericPdkId,
                                     "module buffer(input logic in, output logic out);\nassign out = in;\nendmodule\n");
    const auto parent = openvlsi::createSchematicCell(project, "top");

    openvlsi::ObjectId hdlSymbol = 0;
    openvlsi::addSystemVerilogInstance(project, parent, block, "U1", 0, 0, &hdlSymbol);
    const auto &cell = project.schematicCells.at(parent);
    CHECK(hdlSymbol != 0);
    CHECK(cell.symbols.at(hdlSymbol).sourceCell == block);
    CHECK(cell.symbols.at(hdlSymbol).pins.size() == 2);
    CHECK(pinNamed(project, parent, hdlSymbol, "in") != 0);
    CHECK(pinNamed(project, parent, hdlSymbol, "out") != 0);

    const auto resistorSymbol = openvlsi::addSymbol(project, parent, "Resistor");
    addPin(project, parent, resistorSymbol, "1");
    addPin(project, parent, resistorSymbol, "2");
    const auto resistor = openvlsi::addSchematicInstance(project, parent, resistorSymbol, "R1", 0, 0);
    project.schematicCells.at(parent).instances.at(resistor).parameters.push_back(
        {project.nextId(), "resistance", "1k"});

    auto &updated = project.schematicCells.at(parent);
    const auto inNet = project.nextId();
    updated.nets.emplace(
        inNet, openvlsi::Net{inNet,
                        "in",
                        {pinNamed(project, parent, hdlSymbol, "in"), pinNamed(project, parent, resistorSymbol, "1")}});
    const auto outNet = project.nextId();
    updated.nets.emplace(outNet, openvlsi::Net{outNet,
                                          "out",
                                          {pinNamed(project, parent, hdlSymbol, "out"),
                                           pinNamed(project, parent, resistorSymbol, "2")}});

    CHECK_THROWS_AS(openvlsi::buildSchematicNetlist(project, parent), openvlsi::ProjectError);
    const auto mixedNetlist = openvlsi::buildSchematicNetlist(project, parent, nullptr, openvlsi::SchematicNetlistMode::mixed);
    const auto joined = [&] {
        std::string text;
        for (const auto &line : mixedNetlist)
            text += line + "\n";
        return text;
    }();
    CHECK(joined.find("R1 in out 1k") != std::string::npos);
    CHECK(joined.find("SystemVerilog block: U1 buffer") != std::string::npos);

    const auto plan = openvlsi::buildMixedSignalPlan(project, parent);
    CHECK(plan.ok());
    CHECK(plan.hdlInstances.size() == 1);
    if (!plan.ok() || plan.hdlInstances.size() != 1)
        return;
    CHECK(plan.hdlInstances.front().moduleName == "buffer");
    CHECK(plan.hdlInstances.front().portNodes.at("in") == "in");
    CHECK(plan.hdlInstances.front().portNodes.at("out") == "out");
    CHECK(plan.analogInputNodes == std::vector<std::string>{"in"});
    CHECK(plan.digitalOutputNodes == std::vector<std::string>{"out"});

    const auto diagnostics = openvlsi::validateHdlHierarchy(project, parent);
    CHECK(std::none_of(diagnostics.begin(), diagnostics.end(), [](const openvlsi::Diagnostic &diagnostic) {
        return diagnostic.severity == openvlsi::Diagnostic::Severity::error;
    }));
}

TEST_CASE("Mixed-signal planning flattens HDL blocks inside drawn hierarchy") {
    openvlsi::Project project;
    project.identity = "mixed-nested";
    project.name = "mixed-nested";
    const auto block =
        openvlsi::createSystemVerilogCell(project, "buffer", openvlsi::genericPdkId,
                                     "module buffer(input logic in, output logic out);\nassign out = in;\nendmodule\n");

    const auto channel = openvlsi::createSchematicCell(project, "channel");
    openvlsi::ObjectId hdlSymbol = 0;
    const auto hdlInstance = openvlsi::addSystemVerilogInstance(project, channel, block, "U2", 0, 0, &hdlSymbol);
    const auto inputSymbol = openvlsi::addSymbol(project, channel, "Input");
    addPin(project, channel, inputSymbol, "IN", openvlsi::PinDirection::left);
    const auto inputInstance = openvlsi::addSchematicInstance(project, channel, inputSymbol, "IN", -200, 0);
    const auto outputSymbol = openvlsi::addSymbol(project, channel, "Output");
    addPin(project, channel, outputSymbol, "OUT");
    const auto outputInstance = openvlsi::addSchematicInstance(project, channel, outputSymbol, "OUT", 200, 0);
    auto &channelCell = project.schematicCells.at(channel);
    const auto inputNet = project.nextId();
    channelCell.nets.emplace(inputNet, openvlsi::Net{inputNet,
                                                "in",
                                                {pinNamed(project, channel, hdlSymbol, "in"),
                                                 pinNamed(project, channel, inputSymbol, "IN")}});
    const auto outputNet = project.nextId();
    channelCell.nets.emplace(outputNet, openvlsi::Net{outputNet,
                                                 "out",
                                                 {pinNamed(project, channel, hdlSymbol, "out"),
                                                  pinNamed(project, channel, outputSymbol, "OUT")}});
    CHECK(hdlInstance != 0);
    CHECK(inputInstance != 0);
    CHECK(outputInstance != 0);

    const auto top = openvlsi::createSchematicCell(project, "top");
    const auto channelSymbol = openvlsi::addHierarchicalSymbol(project, top, channel, "channel");
    openvlsi::addSchematicInstance(project, top, channelSymbol, "U1", 0, 0);
    const auto source =
        openvlsi::addSchematicComponent(project, top, {"voltage source", {}, "V1", 0, 0, {{0, "value", "1"}}});
    const auto resistor =
        openvlsi::addSchematicComponent(project, top, {"resistor", {}, "R1", 0, 0, {{0, "resistance", "1k"}}});
    (void)source;
    (void)resistor;
    const auto pin = [&](const std::string &name) {
        return openvlsi::resolveSchematicPin(project.schematicCells.at(top), name);
    };
    openvlsi::addNet(project, top, "in", {pin("U1.IN"), pin("V1.+")});
    openvlsi::addNet(project, top, "out", {pin("U1.OUT"), pin("R1.1")});
    openvlsi::addNet(project, top, "0", {pin("V1.-"), pin("R1.2")});

    CHECK(openvlsi::schematicContainsHdl(project, top));
    const auto plan = openvlsi::buildMixedSignalPlan(project, top);
    CHECK(plan.ok());
    if (!plan.ok())
        return;
    REQUIRE(plan.hdlInstances.size() == 1);
    CHECK(plan.hdlInstances.front().instanceName == "U1_U2");
    CHECK(plan.hdlInstances.front().portNodes.at("in") == "in");
    CHECK(plan.hdlInstances.front().portNodes.at("out") == "out");
    CHECK(plan.analogInputNodes == std::vector<std::string>{"in"});
    CHECK(plan.digitalOutputNodes == std::vector<std::string>{"out"});
    const auto joined = [&] {
        std::string text;
        for (const auto &line : plan.analogNetlist)
            text += line + "\n";
        return text;
    }();
    CHECK(joined.find(".subckt channel") == std::string::npos);
    CHECK(joined.find("V1 in 0 1") != std::string::npos);
    CHECK(joined.find("R1 out 0 1k") != std::string::npos);

    openvlsi::SimulationRequest request;
    request.analysis = "transient";
    request.simulator = "mixed";
    request.stop = 4.0e-9;
    request.step = 1.0e-9;
    request.netlist = plan.analogNetlist;
    request.mixedSignal.blocks = plan.hdlInstances;
    request.mixedSignal.analogInputNodes = plan.analogInputNodes;
    request.mixedSignal.iterations = 2;
    request.probeSignals = {"v(out)"};
    openvlsi::CancellationToken token;
    const auto result = openvlsi::MixedSignalSimulator().run(request, token, {});
    if (result.state != openvlsi::JobState::succeeded) {
        CHECK(result.error.empty());
        return;
    }
    CHECK(findTrace(result.traces, "U1_U2.out") != nullptr);
    CHECK(findTrace(result.traces, "v(out)") != nullptr);
}

TEST_CASE("Mixed-signal hierarchy rejects unconnected and unflattened bus ports") {
    openvlsi::Project project;
    project.identity = "mixed-errors";
    project.name = "mixed-errors";
    const auto block = openvlsi::createSystemVerilogCell(
        project, "busy", openvlsi::genericPdkId,
        "module busy(input logic [3:0] data, output logic out);\nassign out = data[0];\nendmodule\n");
    const auto parent = openvlsi::createSchematicCell(project, "top");
    openvlsi::ObjectId symbol = 0;
    openvlsi::addSystemVerilogInstance(project, parent, block, "U1", 0, 0, &symbol);

    auto plan = openvlsi::buildMixedSignalPlan(project, parent);
    CHECK(!plan.ok());
    CHECK(plan.error.find("bus") != std::string::npos);

    const auto diagnostics = openvlsi::validateHdlHierarchy(project, parent);
    CHECK(std::any_of(diagnostics.begin(), diagnostics.end(), [](const openvlsi::Diagnostic &diagnostic) {
        return diagnostic.severity == openvlsi::Diagnostic::Severity::error;
    }));
}

TEST_CASE("Mixed-signal PWM DAC settles near the duty-cycle average") {
    if (std::system("command -v iverilog >/dev/null 2>&1") != 0 ||
        std::system("command -v ngspice >/dev/null 2>&1") != 0)
        return;

    openvlsi::Project project;
    project.identity = "mixed-pwm";
    project.name = "mixed-pwm";
    const auto block = openvlsi::createSystemVerilogCell(project, "pwm", openvlsi::genericPdkId,
                                                    "module pwm(input logic clk, output logic pwm_out);\n"
                                                    "    logic [2:0] counter = 3'd0;\n"
                                                    "    always_ff @(posedge clk) counter <= counter + 1'b1;\n"
                                                    "    assign pwm_out = counter[2];\n"
                                                    "endmodule\n");
    const auto parent = openvlsi::createSchematicCell(project, "dac");
    openvlsi::ObjectId hdlSymbol = 0;
    openvlsi::addSystemVerilogInstance(project, parent, block, "U1", 0, 0, &hdlSymbol);

    openvlsi::CircuitRequest circuit;
    circuit.components.push_back({"voltage source", {}, "V1", 0, 0, {{0, "value", "PULSE(0 1 5n 1n 1n 10n 20n)"}}});
    circuit.components.push_back({"resistor", {}, "R1", 100, 0, {{0, "resistance", "1k"}}});
    circuit.components.push_back({"capacitor", {}, "C1", 200, 0, {{0, "capacitance", "10n"}}});
    circuit.nets.push_back({"clk", {"U1.clk", "V1.+"}, 0, 0});
    circuit.nets.push_back({"pwm_out", {"U1.pwm_out", "R1.1"}, 0, 0});
    circuit.nets.push_back({"out", {"R1.2", "C1.1"}, 0, 0});
    circuit.nets.push_back({"0", {"V1.-", "C1.2"}, 0, 0});
    openvlsi::addSchematicCircuit(project, parent, circuit);

    const auto plan = openvlsi::buildMixedSignalPlan(project, parent);
    CHECK(plan.ok());
    if (!plan.ok())
        return;

    openvlsi::SimulationRequest request;
    request.analysis = "transient";
    request.simulator = "mixed";
    request.stop = 100.0e-6;
    request.step = 10.0e-9;
    request.netlist = plan.analogNetlist;
    request.mixedSignal.blocks = plan.hdlInstances;
    request.mixedSignal.analogInputNodes = plan.analogInputNodes;
    request.mixedSignal.iterations = 3;
    request.probeSignals = {"v(out)"};

    openvlsi::CancellationToken token;
    const auto result = openvlsi::MixedSignalSimulator().run(request, token, {});
    if (result.state != openvlsi::JobState::succeeded) {
        CHECK(result.error.empty());
        return;
    }
    const auto *output = findTrace(result.traces, "v(out)");
    const auto *pwm = findTrace(result.traces, "U1.pwm_out");
    CHECK(output != nullptr);
    CHECK(pwm != nullptr);
    if (output && pwm && !output->values.empty() && !pwm->values.empty()) {
        CHECK(std::abs(output->values.back() - 0.5) < 0.08);
        const auto minPwm = *std::min_element(pwm->values.begin(), pwm->values.end());
        const auto maxPwm = *std::max_element(pwm->values.begin(), pwm->values.end());
        CHECK(minPwm < 0.1);
        CHECK(maxPwm > 0.9);
    }
}

TEST_CASE("Mixed-signal co-simulation supports flattened buses") {
    if (std::system("command -v iverilog >/dev/null 2>&1") != 0 ||
        std::system("command -v ngspice >/dev/null 2>&1") != 0)
        return;

    openvlsi::Project project;
    project.identity = "mixed-bus";
    project.name = "mixed-bus";
    const auto block = openvlsi::createSystemVerilogCell(
        project, "bus_buffer", openvlsi::genericPdkId,
        "module bus_buffer(input logic [1:0] in, output logic [1:0] out);\nassign out = in;\nendmodule\n");
    const auto parent = openvlsi::createSchematicCell(project, "top");
    openvlsi::ObjectId hdlSymbol = 0;
    openvlsi::addSystemVerilogInstance(project, parent, block, "U1", 0, 0, &hdlSymbol, true);

    const auto sourceSymbol = openvlsi::addSymbol(project, parent, "Voltage Source");
    addPin(project, parent, sourceSymbol, "+");
    addPin(project, parent, sourceSymbol, "-");
    const auto source = openvlsi::addSchematicInstance(project, parent, sourceSymbol, "V1", 0, 0);
    project.schematicCells.at(parent).instances.at(source).parameters.push_back({project.nextId(), "value", "1"});
    const auto r1Symbol = openvlsi::addSymbol(project, parent, "Resistor");
    addPin(project, parent, r1Symbol, "1");
    addPin(project, parent, r1Symbol, "2");
    const auto r1 = openvlsi::addSchematicInstance(project, parent, r1Symbol, "R1", 0, 0);
    project.schematicCells.at(parent).instances.at(r1).parameters.push_back({project.nextId(), "resistance", "1k"});
    const auto r2Symbol = openvlsi::addSymbol(project, parent, "Resistor");
    addPin(project, parent, r2Symbol, "1");
    addPin(project, parent, r2Symbol, "2");
    const auto r2 = openvlsi::addSchematicInstance(project, parent, r2Symbol, "R2", 0, 0);
    project.schematicCells.at(parent).instances.at(r2).parameters.push_back({project.nextId(), "resistance", "1k"});

    auto &cell = project.schematicCells.at(parent);
    const auto in0 = project.nextId();
    cell.nets.emplace(
        in0, openvlsi::Net{in0,
                      "in0",
                      {pinNamed(project, parent, hdlSymbol, "in[0]"), pinNamed(project, parent, sourceSymbol, "+")}});
    const auto out0 = project.nextId();
    cell.nets.emplace(
        out0, openvlsi::Net{out0,
                       "out0",
                       {pinNamed(project, parent, hdlSymbol, "out[0]"), pinNamed(project, parent, r1Symbol, "1")}});
    const auto out1 = project.nextId();
    cell.nets.emplace(
        out1, openvlsi::Net{out1,
                       "out1",
                       {pinNamed(project, parent, hdlSymbol, "out[1]"), pinNamed(project, parent, r2Symbol, "1")}});
    const auto ground = project.nextId();
    cell.nets.emplace(
        ground, openvlsi::Net{ground,
                         "0",
                         {pinNamed(project, parent, sourceSymbol, "-"), pinNamed(project, parent, hdlSymbol, "in[1]"),
                          pinNamed(project, parent, r1Symbol, "2"), pinNamed(project, parent, r2Symbol, "2")}});

    const auto plan = openvlsi::buildMixedSignalPlan(project, parent);
    CHECK(plan.ok());
    if (!plan.ok())
        return;
    CHECK(plan.hdlInstances.size() == 1);
    if (plan.hdlInstances.size() != 1)
        return;
    CHECK(plan.hdlInstances.front().portBits.at("in").size() == 2);
    CHECK(plan.hdlInstances.front().portBits.at("out").size() == 2);

    openvlsi::SimulationRequest request;
    request.analysis = "transient";
    request.simulator = "mixed";
    request.stop = 10.0e-9;
    request.step = 1.0e-9;
    request.netlist = plan.analogNetlist;
    request.mixedSignal.blocks = plan.hdlInstances;
    request.mixedSignal.analogInputNodes = plan.analogInputNodes;
    request.mixedSignal.iterations = 4;
    request.probeSignals = {"v(out0)", "v(out1)"};

    openvlsi::CancellationToken token;
    const auto result = openvlsi::MixedSignalSimulator().run(request, token, {});
    if (result.state != openvlsi::JobState::succeeded) {
        CHECK(result.error.empty());
        return;
    }
    const auto *bit0 = findTrace(result.traces, "U1.out[0]");
    const auto *bit1 = findTrace(result.traces, "U1.out[1]");
    const auto *out0Trace = findTrace(result.traces, "v(out0)");
    const auto *out1Trace = findTrace(result.traces, "v(out1)");
    CHECK(bit0 != nullptr);
    CHECK(bit1 != nullptr);
    CHECK(out0Trace != nullptr);
    CHECK(out1Trace != nullptr);
    if (bit0 && bit1 && out0Trace && out1Trace && !bit0->values.empty() && !bit1->values.empty()) {
        CHECK(bit0->values.back() > 0.5);
        CHECK(bit1->values.back() < 0.5);
        CHECK(out0Trace->values.back() > 0.5);
        CHECK(out1Trace->values.back() < 0.5);
    }
}

TEST_CASE("Mixed-signal co-simulation supports real ports") {
    if (std::system("command -v iverilog >/dev/null 2>&1") != 0 ||
        std::system("command -v ngspice >/dev/null 2>&1") != 0)
        return;

    openvlsi::Project project;
    project.identity = "mixed-real";
    project.name = "mixed-real";
    const auto block = openvlsi::createSystemVerilogCell(
        project, "real_buffer", openvlsi::genericPdkId,
        "module real_buffer(input real in, output real out);\nassign out = in * 0.5;\nendmodule\n");
    const auto parent = openvlsi::createSchematicCell(project, "top");
    openvlsi::ObjectId hdlSymbol = 0;
    openvlsi::addSystemVerilogInstance(project, parent, block, "U1", 0, 0, &hdlSymbol);

    const auto sourceSymbol = openvlsi::addSymbol(project, parent, "Voltage Source");
    addPin(project, parent, sourceSymbol, "+");
    addPin(project, parent, sourceSymbol, "-");
    const auto source = openvlsi::addSchematicInstance(project, parent, sourceSymbol, "V1", 0, 0);
    project.schematicCells.at(parent).instances.at(source).parameters.push_back({project.nextId(), "value", "1"});
    const auto resistorSymbol = openvlsi::addSymbol(project, parent, "Resistor");
    addPin(project, parent, resistorSymbol, "1");
    addPin(project, parent, resistorSymbol, "2");
    const auto resistor = openvlsi::addSchematicInstance(project, parent, resistorSymbol, "R1", 0, 0);
    project.schematicCells.at(parent).instances.at(resistor).parameters.push_back(
        {project.nextId(), "resistance", "1k"});

    auto &cell = project.schematicCells.at(parent);
    const auto inNet = project.nextId();
    cell.nets.emplace(
        inNet, openvlsi::Net{inNet,
                        "in",
                        {pinNamed(project, parent, hdlSymbol, "in"), pinNamed(project, parent, sourceSymbol, "+")}});
    const auto outNet = project.nextId();
    cell.nets.emplace(outNet, openvlsi::Net{outNet,
                                       "out",
                                       {pinNamed(project, parent, hdlSymbol, "out"),
                                        pinNamed(project, parent, resistorSymbol, "1")}});
    const auto groundNet = project.nextId();
    cell.nets.emplace(groundNet, openvlsi::Net{groundNet,
                                          "0",
                                          {pinNamed(project, parent, sourceSymbol, "-"),
                                           pinNamed(project, parent, resistorSymbol, "2")}});

    const auto plan = openvlsi::buildMixedSignalPlan(project, parent);
    CHECK(plan.ok());
    if (!plan.ok())
        return;

    openvlsi::SimulationRequest request;
    request.analysis = "transient";
    request.simulator = "mixed";
    request.stop = 10.0e-9;
    request.step = 1.0e-9;
    request.netlist = plan.analogNetlist;
    request.mixedSignal.blocks = plan.hdlInstances;
    request.mixedSignal.analogInputNodes = plan.analogInputNodes;
    request.mixedSignal.iterations = 4;
    request.probeSignals = {"v(out)"};

    openvlsi::CancellationToken token;
    const auto result = openvlsi::MixedSignalSimulator().run(request, token, {});
    if (result.state != openvlsi::JobState::succeeded) {
        CHECK(result.error.empty());
        return;
    }
    const auto *digitalOut = findTrace(result.traces, "U1.out");
    const auto *analogOut = findTrace(result.traces, "v(out)");
    CHECK(digitalOut != nullptr);
    CHECK(analogOut != nullptr);
    if (digitalOut && analogOut && !digitalOut->values.empty() && !analogOut->values.empty()) {
        CHECK(std::abs(digitalOut->values.back() - 0.5) < 1.0e-6);
        CHECK(std::abs(analogOut->values.back() - 0.5) < 1.0e-3);
    }
}

TEST_CASE("Digital simulation source includes referenced project source cells") {
    openvlsi::Project project;
    project.identity = "digital-source";
    project.name = "digital-source";
    const auto design = openvlsi::createSystemVerilogCell(project, "counter", openvlsi::genericPdkId,
                                                     "module counter(input logic clk, output logic q);\n"
                                                     "    always_ff @(posedge clk) q <= ~q;\n"
                                                     "endmodule\n");
    const auto testbench = openvlsi::createSystemVerilogCell(project, "counter_tb", openvlsi::genericPdkId,
                                                        "`timescale 1ns/1ps\n"
                                                        "module counter_tb;\n"
                                                        "    logic clk = 0;\n"
                                                        "    logic q;\n"
                                                        "    counter dut(clk, q);\n"
                                                        "    always #5 clk = ~clk;\n"
                                                        "    initial #40 $finish;\n"
                                                        "endmodule\n");
    CHECK(design != 0);
    CHECK(testbench != 0);
    const auto source = openvlsi::buildSystemVerilogSimulationSource(project, testbench);
    CHECK(source.find("module counter_tb") != std::string::npos);
    CHECK(source.find("module counter(") != std::string::npos);
    CHECK(source.find("// counter") != std::string::npos);
}

TEST_CASE("Mixed-signal co-simulation drives analog outputs from a digital block") {
    if (std::system("command -v iverilog >/dev/null 2>&1") != 0 ||
        std::system("command -v ngspice >/dev/null 2>&1") != 0)
        return;

    openvlsi::Project project;
    project.identity = "mixed-sim";
    project.name = "mixed-sim";
    const auto block =
        openvlsi::createSystemVerilogCell(project, "buffer", openvlsi::genericPdkId,
                                     "module buffer(input logic in, output logic out);\nassign out = in;\nendmodule\n");
    const auto parent = openvlsi::createSchematicCell(project, "top");
    openvlsi::ObjectId hdlSymbol = 0;
    openvlsi::addSystemVerilogInstance(project, parent, block, "U1", 0, 0, &hdlSymbol);

    const auto sourceSymbol = openvlsi::addSymbol(project, parent, "Voltage Source");
    addPin(project, parent, sourceSymbol, "+");
    addPin(project, parent, sourceSymbol, "-");
    const auto source = openvlsi::addSchematicInstance(project, parent, sourceSymbol, "V1", 0, 0);
    project.schematicCells.at(parent).instances.at(source).parameters.push_back(
        {project.nextId(), "value", "PULSE(0 1 5n 1n 1n 20n 40n)"});

    const auto resistorSymbol = openvlsi::addSymbol(project, parent, "Resistor");
    addPin(project, parent, resistorSymbol, "1");
    addPin(project, parent, resistorSymbol, "2");
    const auto resistor = openvlsi::addSchematicInstance(project, parent, resistorSymbol, "R1", 0, 0);
    project.schematicCells.at(parent).instances.at(resistor).parameters.push_back(
        {project.nextId(), "resistance", "1k"});

    const auto groundSymbol = openvlsi::addSymbol(project, parent, "Ground");
    addPin(project, parent, groundSymbol, "G");
    openvlsi::addSchematicInstance(project, parent, groundSymbol, "GND1", 0, 0);

    auto &cell = project.schematicCells.at(parent);
    const auto inNet = project.nextId();
    cell.nets.emplace(
        inNet, openvlsi::Net{inNet,
                        "in",
                        {pinNamed(project, parent, hdlSymbol, "in"), pinNamed(project, parent, sourceSymbol, "+")}});
    const auto outNet = project.nextId();
    cell.nets.emplace(outNet, openvlsi::Net{outNet,
                                       "out",
                                       {pinNamed(project, parent, hdlSymbol, "out"),
                                        pinNamed(project, parent, resistorSymbol, "1")}});
    const auto groundNet = project.nextId();
    cell.nets.emplace(groundNet, openvlsi::Net{groundNet,
                                          "0",
                                          {pinNamed(project, parent, sourceSymbol, "-"),
                                           pinNamed(project, parent, resistorSymbol, "2"),
                                           pinNamed(project, parent, groundSymbol, "G")}});

    const auto plan = openvlsi::buildMixedSignalPlan(project, parent);
    CHECK(plan.ok());
    if (!plan.ok())
        return;

    openvlsi::SimulationRequest request;
    request.analysis = "transient";
    request.simulator = "mixed";
    request.stop = 20.0e-9;
    request.step = 1.0e-9;
    request.netlist = plan.analogNetlist;
    request.mixedSignal.blocks = plan.hdlInstances;
    request.mixedSignal.analogInputNodes = plan.analogInputNodes;
    request.mixedSignal.iterations = 4;
    request.probeSignals = {"v(out)"};

    openvlsi::CancellationToken token;
    const auto result = openvlsi::MixedSignalSimulator().run(request, token, {});
    if (result.state != openvlsi::JobState::succeeded) {
        CHECK(result.error.empty());
        return;
    }
    CHECK(result.analysis == "mixed");
    CHECK(!result.time.empty());
    const auto *digitalOut = findTrace(result.traces, "U1.out");
    const auto *analogOut = findTrace(result.traces, "v(out)");
    CHECK(digitalOut != nullptr);
    CHECK(analogOut != nullptr);
    if (digitalOut && analogOut) {
        CHECK(digitalOut->values.size() == result.time.size());
        CHECK(analogOut->values.size() == result.time.size());
        const auto highIndex = static_cast<std::size_t>(std::llround(10.0e-9 / request.step));
        CHECK(highIndex < result.time.size());
        if (highIndex < result.time.size() && highIndex < digitalOut->values.size() &&
            highIndex < analogOut->values.size()) {
            std::cerr << "MIXED HIGH: t=" << result.time[highIndex] << " digital=" << digitalOut->values[highIndex]
                      << " analog=" << analogOut->values[highIndex] << "\n";
            CHECK(digitalOut->values[highIndex] > 0.5);
            CHECK(analogOut->values[highIndex] > 0.5);
        }
    }
}
