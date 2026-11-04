#include "icelib/simulation.hpp"
#include "icelib/vcd.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {
const ice::SimulationTrace *findTrace(const ice::SimulationResult &result, const std::string &name) {
    for (const auto &trace : result.traces) {
        if (trace.name == name || trace.name.ends_with("." + name))
            return &trace;
    }
    return nullptr;
}

const ice::SimulationTrace *findTrace(const ice::VcdData &data, const std::string &name) {
    for (const auto &trace : data.traces) {
        if (trace.name == name)
            return &trace;
    }
    return nullptr;
}
} // namespace

TEST_CASE("VCD parser converts scalar and vector traces") {
    const std::string text = R"($timescale 1ns $end
$scope module tb $end
$var wire 1 ! clk $end
$var wire 4 " data [3:0] $end
$upscope $end
$enddefinitions $end
#0
$dumpvars
0!
b0000 "
$end
#5
1!
#10
b1010 "
0!
#15
1!)";

    const auto data = ice::parseVcdText(text, "test.vcd");
    CHECK(data.time.size() == 4);
    CHECK(std::abs(data.time[1] - 5.0e-9) < 1.0e-18);
    CHECK(std::abs(data.time[2] - 10.0e-9) < 1.0e-18);

    const auto *clock = findTrace(data, "tb.clk");
    const auto *bus = findTrace(data, "tb.data");
    CHECK(clock != nullptr);
    CHECK(bus != nullptr);
    if (!clock || !bus)
        return;
    CHECK((clock->values == std::vector<double>{0, 1, 0, 1}));
    CHECK((bus->values == std::vector<double>{0, 0, 10, 10}));
}

TEST_CASE("VCD traces populate empty simulation signals and graphs") {
    ice::SimulationItem simulation;
    ice::SimulationGraph graph;
    graph.id = 1;
    graph.name = "Graph 1";
    simulation.graphs.push_back(graph);
    const std::vector<ice::SimulationTrace> traces = {{"top.clk", {0, 1, 0}}, {"top.q", {0, 1, 1}}};

    CHECK(ice::populateSimulationSignalsFromTraces(simulation, traces));
    CHECK(simulation.captureSignals == "top.clk,top.q");
    CHECK((simulation.graphs.front().ySignals == std::vector<std::string>{"top.clk", "top.q"}));
    CHECK(simulation.graphs.front().xAxis == "time");
    CHECK(!ice::populateSimulationSignalsFromTraces(simulation, traces));
}

TEST_CASE("Icarus Verilog picks the testbench that owns the dump commands") {
    if (std::system("command -v iverilog >/dev/null 2>&1") != 0)
        return;
    const std::string source = R"(`timescale 1ns/1ps
module tb_first;
    logic clk = 1'b0;
    logic q = 1'b0;
    always #5 clk = ~clk;
    always_ff @(posedge clk) q <= ~q;
    initial begin
        $dumpfile("tb_first.vcd");
        $dumpvars(0, tb_first);
        #40 $finish;
    end
endmodule
module tb_first_dut(input logic clk, output logic q);
    assign q = clk;
endmodule
)";

    ice::SimulationRequest request;
    request.simulator = "iverilog";
    request.analysis = "digital";
    std::istringstream stream(source);
    std::string line;
    while (std::getline(stream, line))
        request.netlist.push_back(line);
    request.probeSignals = {"clk", "q"};
    ice::CancellationToken token;
    const auto result = ice::IcarusVerilogSimulator().run(request, token, {});
    CHECK(result.state == ice::JobState::succeeded);
    const auto *clock = findTrace(result, "clk");
    const auto *output = findTrace(result, "q");
    CHECK(clock != nullptr);
    CHECK(output != nullptr);
}

TEST_CASE("Icarus Verilog testbench produces VCD traces") {
    if (std::system("command -v iverilog >/dev/null 2>&1") != 0)
        return;
    const std::string source = R"(`timescale 1ns/1ps
module tb;
    logic clk = 1'b0;
    logic q = 1'b0;
    always #5 clk = ~clk;
    always_ff @(posedge clk) q <= ~q;
    initial begin
        #40 $finish;
    end
endmodule
)";

    ice::SimulationRequest request;
    request.simulator = "iverilog";
    request.analysis = "digital";
    std::istringstream stream(source);
    std::string line;
    while (std::getline(stream, line))
        request.netlist.push_back(line);
    request.probeSignals = {"clk", "q"};
    ice::CancellationToken token;
    ice::SimulationResult result;
    try {
        result = ice::IcarusVerilogSimulator().run(request, token, {});
    } catch (const std::exception &) {
        CHECK(false);
        return;
    }
    if (result.state != ice::JobState::succeeded) {
        std::cerr << "ICARUS DEBUG: " << result.error << "\n";
        for (const auto &warning : result.warnings)
            std::cerr << "ICARUS WARNING: " << warning << "\n";
    }
    CHECK(result.state == ice::JobState::succeeded);
    CHECK(!result.time.empty());
    CHECK(result.traces.size() == 2);
    const auto *clock = findTrace(result, "clk");
    const auto *output = findTrace(result, "q");
    CHECK(clock != nullptr);
    CHECK(output != nullptr);
    if (clock)
        CHECK(clock->values.size() == result.time.size());
    if (output)
        CHECK(output->values.size() == result.time.size());
}
