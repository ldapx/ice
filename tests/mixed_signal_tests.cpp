#include "icelib/agent_api.hpp"
#include "icelib/hdl.hpp"
#include "icelib/operations.hpp"
#include "icelib/simulation.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
const ice::SimulationTrace *findTrace(const std::vector<ice::SimulationTrace> &traces, const std::string &name) {
    for (const auto &trace : traces) {
        if (trace.name == name || trace.name.ends_with("." + name))
            return &trace;
    }
    return nullptr;
}

ice::ObjectId addPin(ice::Project &project, ice::ObjectId cellId, ice::ObjectId symbolId, std::string name,
                     ice::PinDirection direction = ice::PinDirection::right) {
    auto &cell = project.schematicCells.at(cellId);
    const auto pinId = project.nextId();
    cell.symbols.at(symbolId).pins.push_back({pinId, std::move(name), 0, 0, direction});
    return pinId;
}

ice::ObjectId pinNamed(const ice::Project &project, ice::ObjectId cellId, ice::ObjectId symbolId,
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
    const auto parsed = ice::parseSystemVerilogModule(ansi, "mixed_block");
    CHECK(parsed.ok);
    if (!parsed.ok)
        return;
    CHECK(parsed.module.name == "mixed_block");
    CHECK(parsed.module.ports.size() == 5);
    if (parsed.module.ports.size() != 5)
        return;
    CHECK(parsed.module.ports[0].name == "clk");
    CHECK(parsed.module.ports[0].direction == ice::HdlPortDirection::input);
    CHECK(parsed.module.ports[0].width == 1);
    CHECK(parsed.module.ports[2].name == "data");
    CHECK(parsed.module.ports[2].width == 4);
    CHECK(parsed.module.ports[3].name == "q");
    CHECK(parsed.module.ports[3].direction == ice::HdlPortDirection::output);
    CHECK(parsed.module.ports[3].width == 4);
    CHECK(parsed.module.ports[4].direction == ice::HdlPortDirection::inout);

    const std::string legacy = R"(
module legacy(a, b, c);
    input a;
    output [1:0] b;
    inout c;
endmodule
)";
    const auto legacyParsed = ice::parseSystemVerilogModule(legacy, "legacy");
    CHECK(legacyParsed.ok);
    if (!legacyParsed.ok)
        return;
    CHECK(legacyParsed.module.ports.size() == 3);
    if (legacyParsed.module.ports.size() != 3)
        return;
    CHECK(legacyParsed.module.ports[0].direction == ice::HdlPortDirection::input);
    CHECK(legacyParsed.module.ports[1].direction == ice::HdlPortDirection::output);
    CHECK(legacyParsed.module.ports[1].width == 2);
    CHECK(legacyParsed.module.ports[2].direction == ice::HdlPortDirection::inout);
}

TEST_CASE("SystemVerilog port parser inherits direction and width") {
    const std::string source = R"(
module inherit(input logic [7:0] a, b, output logic [1:0] c, d);
endmodule
)";
    const auto parsed = ice::parseSystemVerilogModule(source, "inherit");
    CHECK(parsed.ok);
    if (!parsed.ok)
        return;
    CHECK(parsed.module.ports.size() == 4);
    if (parsed.module.ports.size() != 4)
        return;
    CHECK(parsed.module.ports[0].name == "a");
    CHECK(parsed.module.ports[1].name == "b");
    CHECK(parsed.module.ports[0].direction == ice::HdlPortDirection::input);
    CHECK(parsed.module.ports[1].direction == ice::HdlPortDirection::input);
    CHECK(parsed.module.ports[0].width == 8);
    CHECK(parsed.module.ports[1].width == 8);
    CHECK(parsed.module.ports[2].direction == ice::HdlPortDirection::output);
    CHECK(parsed.module.ports[3].direction == ice::HdlPortDirection::output);
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
    const auto parsed = ice::parseSystemVerilogModule(source);
    CHECK(parsed.ok);
    if (!parsed.ok)
        return;
    CHECK(parsed.module.name == "dut");
    CHECK(parsed.module.ports.size() == 2);
}

TEST_CASE("SystemVerilog block instances get generated symbols and a mixed plan") {
    ice::Project project;
    project.identity = "mixed-plan";
    project.name = "mixed-plan";
    const auto block =
        ice::createSystemVerilogCell(project, "buffer", ice::genericPdkId,
                                     "module buffer(input logic in, output logic out);\nassign out = in;\nendmodule\n");
    const auto parent = ice::createSchematicCell(project, "top");

    ice::ObjectId hdlSymbol = 0;
    ice::addSystemVerilogInstance(project, parent, block, "U1", 0, 0, &hdlSymbol);
    const auto &cell = project.schematicCells.at(parent);
    CHECK(hdlSymbol != 0);
    CHECK(cell.symbols.at(hdlSymbol).sourceCell == block);
    CHECK(cell.symbols.at(hdlSymbol).pins.size() == 2);
    CHECK(pinNamed(project, parent, hdlSymbol, "in") != 0);
    CHECK(pinNamed(project, parent, hdlSymbol, "out") != 0);

    const auto resistorSymbol = ice::addSymbol(project, parent, "Resistor");
    addPin(project, parent, resistorSymbol, "1");
    addPin(project, parent, resistorSymbol, "2");
    const auto resistor = ice::addSchematicInstance(project, parent, resistorSymbol, "R1", 0, 0);
    project.schematicCells.at(parent).instances.at(resistor).parameters.push_back(
        {project.nextId(), "resistance", "1k"});

    auto &updated = project.schematicCells.at(parent);
    const auto inNet = project.nextId();
    updated.nets.emplace(
        inNet, ice::Net{inNet,
                        "in",
                        {pinNamed(project, parent, hdlSymbol, "in"), pinNamed(project, parent, resistorSymbol, "1")}});
    const auto outNet = project.nextId();
    updated.nets.emplace(outNet, ice::Net{outNet,
                                          "out",
                                          {pinNamed(project, parent, hdlSymbol, "out"),
                                           pinNamed(project, parent, resistorSymbol, "2")}});

    CHECK_THROWS_AS(ice::buildSchematicNetlist(project, parent), ice::ProjectError);
    const auto mixedNetlist = ice::buildSchematicNetlist(project, parent, nullptr, ice::SchematicNetlistMode::mixed);
    const auto joined = [&] {
        std::string text;
        for (const auto &line : mixedNetlist)
            text += line + "\n";
        return text;
    }();
    CHECK(joined.find("R1 in out 1k") != std::string::npos);
    CHECK(joined.find("SystemVerilog block: U1 buffer") != std::string::npos);

    const auto plan = ice::buildMixedSignalPlan(project, parent);
    CHECK(plan.ok());
    CHECK(plan.hdlInstances.size() == 1);
    if (!plan.ok() || plan.hdlInstances.size() != 1)
        return;
    CHECK(plan.hdlInstances.front().moduleName == "buffer");
    CHECK(plan.hdlInstances.front().portNodes.at("in") == "in");
    CHECK(plan.hdlInstances.front().portNodes.at("out") == "out");
    CHECK(plan.analogInputNodes == std::vector<std::string>{"in"});
    CHECK(plan.digitalOutputNodes == std::vector<std::string>{"out"});

    const auto diagnostics = ice::validateHdlHierarchy(project, parent);
    CHECK(std::none_of(diagnostics.begin(), diagnostics.end(), [](const ice::Diagnostic &diagnostic) {
        return diagnostic.severity == ice::Diagnostic::Severity::error;
    }));
}

TEST_CASE("Mixed-signal hierarchy rejects unconnected and unflattened bus ports") {
    ice::Project project;
    project.identity = "mixed-errors";
    project.name = "mixed-errors";
    const auto block = ice::createSystemVerilogCell(
        project, "busy", ice::genericPdkId,
        "module busy(input logic [3:0] data, output logic out);\nassign out = data[0];\nendmodule\n");
    const auto parent = ice::createSchematicCell(project, "top");
    ice::ObjectId symbol = 0;
    ice::addSystemVerilogInstance(project, parent, block, "U1", 0, 0, &symbol);

    auto plan = ice::buildMixedSignalPlan(project, parent);
    CHECK(!plan.ok());
    CHECK(plan.error.find("bus") != std::string::npos);

    const auto diagnostics = ice::validateHdlHierarchy(project, parent);
    CHECK(std::any_of(diagnostics.begin(), diagnostics.end(), [](const ice::Diagnostic &diagnostic) {
        return diagnostic.severity == ice::Diagnostic::Severity::error;
    }));
}

TEST_CASE("Mixed-signal PWM DAC settles near the duty-cycle average") {
    if (std::system("command -v iverilog >/dev/null 2>&1") != 0 ||
        std::system("command -v ngspice >/dev/null 2>&1") != 0)
        return;

    ice::Project project;
    project.identity = "mixed-pwm";
    project.name = "mixed-pwm";
    const auto block = ice::createSystemVerilogCell(project, "pwm", ice::genericPdkId,
                                                    "module pwm(input logic clk, output logic pwm_out);\n"
                                                    "    logic [2:0] counter = 3'd0;\n"
                                                    "    always_ff @(posedge clk) counter <= counter + 1'b1;\n"
                                                    "    assign pwm_out = counter[2];\n"
                                                    "endmodule\n");
    const auto parent = ice::createSchematicCell(project, "dac");
    ice::ObjectId hdlSymbol = 0;
    ice::addSystemVerilogInstance(project, parent, block, "U1", 0, 0, &hdlSymbol);

    ice::CircuitRequest circuit;
    circuit.components.push_back({"voltage source", {}, "V1", 0, 0, {{0, "value", "PULSE(0 1 5n 1n 1n 10n 20n)"}}});
    circuit.components.push_back({"resistor", {}, "R1", 100, 0, {{0, "resistance", "1k"}}});
    circuit.components.push_back({"capacitor", {}, "C1", 200, 0, {{0, "capacitance", "10n"}}});
    circuit.nets.push_back({"clk", {"U1.clk", "V1.+"}, 0, 0});
    circuit.nets.push_back({"pwm_out", {"U1.pwm_out", "R1.1"}, 0, 0});
    circuit.nets.push_back({"out", {"R1.2", "C1.1"}, 0, 0});
    circuit.nets.push_back({"0", {"V1.-", "C1.2"}, 0, 0});
    ice::addSchematicCircuit(project, parent, circuit);

    const auto plan = ice::buildMixedSignalPlan(project, parent);
    CHECK(plan.ok());
    if (!plan.ok())
        return;

    ice::SimulationRequest request;
    request.analysis = "transient";
    request.simulator = "mixed";
    request.stop = 100.0e-6;
    request.step = 10.0e-9;
    request.netlist = plan.analogNetlist;
    request.mixedSignal.blocks = plan.hdlInstances;
    request.mixedSignal.analogInputNodes = plan.analogInputNodes;
    request.mixedSignal.iterations = 3;
    request.probeSignals = {"v(out)"};

    ice::CancellationToken token;
    const auto result = ice::MixedSignalSimulator().run(request, token, {});
    if (result.state != ice::JobState::succeeded) {
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

    ice::Project project;
    project.identity = "mixed-bus";
    project.name = "mixed-bus";
    const auto block = ice::createSystemVerilogCell(
        project, "bus_buffer", ice::genericPdkId,
        "module bus_buffer(input logic [1:0] in, output logic [1:0] out);\nassign out = in;\nendmodule\n");
    const auto parent = ice::createSchematicCell(project, "top");
    ice::ObjectId hdlSymbol = 0;
    ice::addSystemVerilogInstance(project, parent, block, "U1", 0, 0, &hdlSymbol, true);

    const auto sourceSymbol = ice::addSymbol(project, parent, "Voltage Source");
    addPin(project, parent, sourceSymbol, "+");
    addPin(project, parent, sourceSymbol, "-");
    const auto source = ice::addSchematicInstance(project, parent, sourceSymbol, "V1", 0, 0);
    project.schematicCells.at(parent).instances.at(source).parameters.push_back({project.nextId(), "value", "1"});
    const auto r1Symbol = ice::addSymbol(project, parent, "Resistor");
    addPin(project, parent, r1Symbol, "1");
    addPin(project, parent, r1Symbol, "2");
    const auto r1 = ice::addSchematicInstance(project, parent, r1Symbol, "R1", 0, 0);
    project.schematicCells.at(parent).instances.at(r1).parameters.push_back({project.nextId(), "resistance", "1k"});
    const auto r2Symbol = ice::addSymbol(project, parent, "Resistor");
    addPin(project, parent, r2Symbol, "1");
    addPin(project, parent, r2Symbol, "2");
    const auto r2 = ice::addSchematicInstance(project, parent, r2Symbol, "R2", 0, 0);
    project.schematicCells.at(parent).instances.at(r2).parameters.push_back({project.nextId(), "resistance", "1k"});

    auto &cell = project.schematicCells.at(parent);
    const auto in0 = project.nextId();
    cell.nets.emplace(
        in0, ice::Net{in0,
                      "in0",
                      {pinNamed(project, parent, hdlSymbol, "in[0]"), pinNamed(project, parent, sourceSymbol, "+")}});
    const auto out0 = project.nextId();
    cell.nets.emplace(
        out0, ice::Net{out0,
                       "out0",
                       {pinNamed(project, parent, hdlSymbol, "out[0]"), pinNamed(project, parent, r1Symbol, "1")}});
    const auto out1 = project.nextId();
    cell.nets.emplace(
        out1, ice::Net{out1,
                       "out1",
                       {pinNamed(project, parent, hdlSymbol, "out[1]"), pinNamed(project, parent, r2Symbol, "1")}});
    const auto ground = project.nextId();
    cell.nets.emplace(
        ground, ice::Net{ground,
                         "0",
                         {pinNamed(project, parent, sourceSymbol, "-"), pinNamed(project, parent, hdlSymbol, "in[1]"),
                          pinNamed(project, parent, r1Symbol, "2"), pinNamed(project, parent, r2Symbol, "2")}});

    const auto plan = ice::buildMixedSignalPlan(project, parent);
    CHECK(plan.ok());
    if (!plan.ok())
        return;
    CHECK(plan.hdlInstances.size() == 1);
    if (plan.hdlInstances.size() != 1)
        return;
    CHECK(plan.hdlInstances.front().portBits.at("in").size() == 2);
    CHECK(plan.hdlInstances.front().portBits.at("out").size() == 2);

    ice::SimulationRequest request;
    request.analysis = "transient";
    request.simulator = "mixed";
    request.stop = 10.0e-9;
    request.step = 1.0e-9;
    request.netlist = plan.analogNetlist;
    request.mixedSignal.blocks = plan.hdlInstances;
    request.mixedSignal.analogInputNodes = plan.analogInputNodes;
    request.mixedSignal.iterations = 4;
    request.probeSignals = {"v(out0)", "v(out1)"};

    ice::CancellationToken token;
    const auto result = ice::MixedSignalSimulator().run(request, token, {});
    if (result.state != ice::JobState::succeeded) {
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

    ice::Project project;
    project.identity = "mixed-real";
    project.name = "mixed-real";
    const auto block = ice::createSystemVerilogCell(
        project, "real_buffer", ice::genericPdkId,
        "module real_buffer(input real in, output real out);\nassign out = in * 0.5;\nendmodule\n");
    const auto parent = ice::createSchematicCell(project, "top");
    ice::ObjectId hdlSymbol = 0;
    ice::addSystemVerilogInstance(project, parent, block, "U1", 0, 0, &hdlSymbol);

    const auto sourceSymbol = ice::addSymbol(project, parent, "Voltage Source");
    addPin(project, parent, sourceSymbol, "+");
    addPin(project, parent, sourceSymbol, "-");
    const auto source = ice::addSchematicInstance(project, parent, sourceSymbol, "V1", 0, 0);
    project.schematicCells.at(parent).instances.at(source).parameters.push_back({project.nextId(), "value", "1"});
    const auto resistorSymbol = ice::addSymbol(project, parent, "Resistor");
    addPin(project, parent, resistorSymbol, "1");
    addPin(project, parent, resistorSymbol, "2");
    const auto resistor = ice::addSchematicInstance(project, parent, resistorSymbol, "R1", 0, 0);
    project.schematicCells.at(parent).instances.at(resistor).parameters.push_back(
        {project.nextId(), "resistance", "1k"});

    auto &cell = project.schematicCells.at(parent);
    const auto inNet = project.nextId();
    cell.nets.emplace(
        inNet, ice::Net{inNet,
                        "in",
                        {pinNamed(project, parent, hdlSymbol, "in"), pinNamed(project, parent, sourceSymbol, "+")}});
    const auto outNet = project.nextId();
    cell.nets.emplace(outNet, ice::Net{outNet,
                                       "out",
                                       {pinNamed(project, parent, hdlSymbol, "out"),
                                        pinNamed(project, parent, resistorSymbol, "1")}});
    const auto groundNet = project.nextId();
    cell.nets.emplace(groundNet, ice::Net{groundNet,
                                          "0",
                                          {pinNamed(project, parent, sourceSymbol, "-"),
                                           pinNamed(project, parent, resistorSymbol, "2")}});

    const auto plan = ice::buildMixedSignalPlan(project, parent);
    CHECK(plan.ok());
    if (!plan.ok())
        return;

    ice::SimulationRequest request;
    request.analysis = "transient";
    request.simulator = "mixed";
    request.stop = 10.0e-9;
    request.step = 1.0e-9;
    request.netlist = plan.analogNetlist;
    request.mixedSignal.blocks = plan.hdlInstances;
    request.mixedSignal.analogInputNodes = plan.analogInputNodes;
    request.mixedSignal.iterations = 4;
    request.probeSignals = {"v(out)"};

    ice::CancellationToken token;
    const auto result = ice::MixedSignalSimulator().run(request, token, {});
    if (result.state != ice::JobState::succeeded) {
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

TEST_CASE("Mixed-signal co-simulation drives analog outputs from a digital block") {
    if (std::system("command -v iverilog >/dev/null 2>&1") != 0 ||
        std::system("command -v ngspice >/dev/null 2>&1") != 0)
        return;

    ice::Project project;
    project.identity = "mixed-sim";
    project.name = "mixed-sim";
    const auto block =
        ice::createSystemVerilogCell(project, "buffer", ice::genericPdkId,
                                     "module buffer(input logic in, output logic out);\nassign out = in;\nendmodule\n");
    const auto parent = ice::createSchematicCell(project, "top");
    ice::ObjectId hdlSymbol = 0;
    ice::addSystemVerilogInstance(project, parent, block, "U1", 0, 0, &hdlSymbol);

    const auto sourceSymbol = ice::addSymbol(project, parent, "Voltage Source");
    addPin(project, parent, sourceSymbol, "+");
    addPin(project, parent, sourceSymbol, "-");
    const auto source = ice::addSchematicInstance(project, parent, sourceSymbol, "V1", 0, 0);
    project.schematicCells.at(parent).instances.at(source).parameters.push_back(
        {project.nextId(), "value", "PULSE(0 1 5n 1n 1n 20n 40n)"});

    const auto resistorSymbol = ice::addSymbol(project, parent, "Resistor");
    addPin(project, parent, resistorSymbol, "1");
    addPin(project, parent, resistorSymbol, "2");
    const auto resistor = ice::addSchematicInstance(project, parent, resistorSymbol, "R1", 0, 0);
    project.schematicCells.at(parent).instances.at(resistor).parameters.push_back(
        {project.nextId(), "resistance", "1k"});

    const auto groundSymbol = ice::addSymbol(project, parent, "Ground");
    addPin(project, parent, groundSymbol, "G");
    ice::addSchematicInstance(project, parent, groundSymbol, "GND1", 0, 0);

    auto &cell = project.schematicCells.at(parent);
    const auto inNet = project.nextId();
    cell.nets.emplace(
        inNet, ice::Net{inNet,
                        "in",
                        {pinNamed(project, parent, hdlSymbol, "in"), pinNamed(project, parent, sourceSymbol, "+")}});
    const auto outNet = project.nextId();
    cell.nets.emplace(outNet, ice::Net{outNet,
                                       "out",
                                       {pinNamed(project, parent, hdlSymbol, "out"),
                                        pinNamed(project, parent, resistorSymbol, "1")}});
    const auto groundNet = project.nextId();
    cell.nets.emplace(groundNet, ice::Net{groundNet,
                                          "0",
                                          {pinNamed(project, parent, sourceSymbol, "-"),
                                           pinNamed(project, parent, resistorSymbol, "2"),
                                           pinNamed(project, parent, groundSymbol, "G")}});

    const auto plan = ice::buildMixedSignalPlan(project, parent);
    CHECK(plan.ok());
    if (!plan.ok())
        return;

    ice::SimulationRequest request;
    request.analysis = "transient";
    request.simulator = "mixed";
    request.stop = 20.0e-9;
    request.step = 1.0e-9;
    request.netlist = plan.analogNetlist;
    request.mixedSignal.blocks = plan.hdlInstances;
    request.mixedSignal.analogInputNodes = plan.analogInputNodes;
    request.mixedSignal.iterations = 4;
    request.probeSignals = {"v(out)"};

    ice::CancellationToken token;
    const auto result = ice::MixedSignalSimulator().run(request, token, {});
    if (result.state != ice::JobState::succeeded) {
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
