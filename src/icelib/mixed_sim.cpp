#include "icelib/simulation.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>

namespace ice {
namespace {

constexpr double kPicosecondsPerSecond = 1.0e12;
constexpr int kMaximumMixedPoints = 200000;

std::string escapeVerilogString(const std::string &text) {
    std::string result;
    result.reserve(text.size());
    for (const char character : text) {
        if (character == '\\' || character == '"')
            result.push_back('\\');
        result.push_back(character);
    }
    return result;
}

std::string trimCopy(std::string text) {
    const auto notSpace = [](unsigned char character) { return !std::isspace(character); };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), notSpace));
    text.erase(std::find_if(text.rbegin(), text.rend(), notSpace).base(), text.end());
    return text;
}

std::string lowerCopy(std::string text) {
    for (auto &character : text)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return text;
}

double sampleLinear(const std::vector<double> &time, const std::vector<double> &values, double timePoint) {
    if (time.empty() || values.empty())
        return 0.0;
    if (timePoint <= time.front())
        return values.front();
    if (timePoint >= time.back())
        return values.back();
    const auto upper = std::upper_bound(time.begin(), time.end(), timePoint);
    const auto index = static_cast<std::size_t>(std::distance(time.begin(), upper));
    if (index == 0)
        return values.front();
    if (index >= time.size() || index >= values.size())
        return values.back();
    const auto leftTime = time[index - 1];
    const auto rightTime = time[index];
    if (rightTime <= leftTime)
        return values[index - 1];
    const auto fraction = (timePoint - leftTime) / (rightTime - leftTime);
    return values[index - 1] + fraction * (values[index] - values[index - 1]);
}

std::vector<double> sampleLinear(const std::vector<double> &time, const std::vector<double> &values,
                                 const std::vector<double> &grid) {
    std::vector<double> result;
    result.reserve(grid.size());
    for (const auto point : grid)
        result.push_back(sampleLinear(time, values, point));
    return result;
}

std::vector<double> sampleHold(const std::vector<double> &time, const std::vector<double> &values,
                               const std::vector<double> &grid) {
    std::vector<double> result;
    result.reserve(grid.size());
    if (time.empty() || values.empty()) {
        result.assign(grid.size(), 0.0);
        return result;
    }
    std::size_t index = 0;
    double current = values.front();
    for (const auto point : grid) {
        while (index < time.size() && time[index] <= point) {
            current = values[std::min(index, values.size() - 1)];
            ++index;
        }
        result.push_back(current);
    }
    return result;
}

const SimulationTrace *findTraceForPort(const SimulationResult &result, const std::string &tbInstance,
                                        const std::string &portName) {
    const auto wanted = lowerCopy(tbInstance + "." + portName);
    for (const auto &trace : result.traces) {
        const auto name = lowerCopy(trace.name);
        if (name == wanted || name.ends_with("." + wanted))
            return &trace;
    }
    return nullptr;
}

const SimulationTrace *findTraceEndingWith(const SimulationResult &result, const std::string &suffix) {
    const auto wanted = lowerCopy(suffix);
    for (const auto &trace : result.traces) {
        const auto name = lowerCopy(trace.name);
        if (name == wanted || name.ends_with("." + wanted))
            return &trace;
    }
    return nullptr;
}

std::string normalizedNode(std::string name) {
    name = lowerCopy(trimCopy(std::move(name)));
    if (name.starts_with("v(") && name.ends_with(")"))
        name = name.substr(2, name.size() - 3);
    return name;
}

bool requestContainsSignal(const std::vector<std::string> &signals, const std::string &signal) {
    const auto wanted = normalizedNode(signal);
    return std::any_of(signals.begin(), signals.end(),
                       [&](const std::string &existing) { return normalizedNode(existing) == wanted; });
}

const SimulationTrace *findAnalogTrace(const SimulationResult &result, const std::string &node) {
    const auto wanted = normalizedNode(node);
    for (const auto &trace : result.traces) {
        if (normalizedNode(trace.name) == wanted)
            return &trace;
    }
    return nullptr;
}

void appendWarnings(std::vector<std::string> &destination, const std::vector<std::string> &source) {
    for (const auto &warning : source) {
        if (std::find(destination.begin(), destination.end(), warning) == destination.end())
            destination.push_back(warning);
    }
}

std::string fingerprintMixedRequest(const SimulationRequest &request) {
    constexpr std::uint64_t kOffset = 1469598103934665603ULL;
    constexpr std::uint64_t kPrime = 1099511628211ULL;
    std::uint64_t hash = kOffset;
    const auto mix = [&](const void *data, std::size_t size) {
        const auto *bytes = static_cast<const unsigned char *>(data);
        for (std::size_t index = 0; index < size; ++index) {
            hash ^= bytes[index];
            hash *= kPrime;
        }
    };
    const auto mixString = [&](const std::string &text) {
        mix(text.data(), text.size());
        const char separator = '\0';
        mix(&separator, 1);
    };
    mixString("mixed");
    mixString(request.analysis);
    mix(reinterpret_cast<const char *>(&request.stop), sizeof(request.stop));
    mix(reinterpret_cast<const char *>(&request.step), sizeof(request.step));
    for (const auto &line : request.netlist)
        mixString(line);
    for (const auto &signal : request.probeSignals)
        mixString(signal);
    for (const auto &block : request.mixedSignal.blocks) {
        mixString(block.instanceName);
        mixString(block.moduleName);
        mixString(block.source);
        for (const auto &port : block.ports) {
            mixString(port.name);
            const auto node = block.portNodes.find(port.name);
            if (node != block.portNodes.end())
                mixString(node->second);
        }
    }
    std::ostringstream text;
    text << std::hex << std::setw(16) << std::setfill('0') << hash;
    return "fnv1a64:" + text.str();
}

struct TempDirectory {
    std::filesystem::path path;
    ~TempDirectory() {
        if (path.empty())
            return;
        std::error_code status;
        std::filesystem::remove_all(path, status);
    }
};

struct InputPortBinding {
    std::size_t block = 0;
    std::size_t port = 0;
    int bit = 0;
    std::string node;
    bool real = false;
    std::string signalName;
    std::string tbSignal;
};

struct OutputPortBinding {
    std::size_t block = 0;
    std::size_t port = 0;
    int bit = 0;
    std::string node;
    std::string tbInstance;
    bool real = false;
    std::string signalName;
    std::string tbWire;
};

const SimulationTrace *findDigitalOutputTrace(const SimulationResult &result, const OutputPortBinding &output) {
    if (const auto *trace = findTraceEndingWith(result, output.tbWire))
        return trace;
    if (const auto *trace = findTraceForPort(result, output.tbInstance, output.signalName))
        return trace;
    return findTraceForPort(result, output.tbInstance, output.signalName + "[0]");
}

std::string pwlValue(const std::vector<double> &time, const std::vector<double> &values, double high, double low,
                     bool realValues) {
    std::ostringstream output;
    output << std::setprecision(17);
    if (time.empty() || values.empty())
        return std::to_string(low);
    std::vector<std::pair<double, double>> points;
    double previous = realValues ? values.front() : (values.front() > 0.5 ? high : low);
    points.emplace_back(time.front(), previous);
    for (std::size_t index = 1; index < values.size() && index < time.size(); ++index) {
        const auto value = realValues ? values[index] : (values[index] > 0.5 ? high : low);
        if (value == previous)
            continue;
        points.emplace_back(time[index], value);
        previous = value;
    }
    if (points.size() == 1)
        return std::to_string(points.front().second);
    output << "PWL(";
    for (std::size_t index = 0; index < points.size(); ++index) {
        if (index)
            output << " ";
        output << points[index].first << " " << points[index].second;
    }
    output << ")";
    return output.str();
}

std::vector<std::string> splitLines(const std::string &text) {
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line))
        lines.push_back(std::move(line));
    return lines;
}

} // namespace

SimulationResult MixedSignalSimulator::run(const SimulationRequest &request, const CancellationToken &token,
                                           const ProgressCallback &progress) {
    SimulationResult result{1, JobState::running, "mixed", {}, {}, "mixed", "ngspice + Icarus Verilog", {}, {}};
    result.input_fingerprint = fingerprintMixedRequest(request);
    if (request.mixedSignal.blocks.empty()) {
        result.state = JobState::failed;
        result.error = "Mixed-signal simulation requires at least one SystemVerilog block";
        return result;
    }
    if (!(request.stop > 0.0) || !(request.step > 0.0)) {
        result.state = JobState::failed;
        result.error = "Mixed-signal transient analysis requires positive step and stop times";
        return result;
    }

    const auto requestedStep = std::min(request.step, request.stop);
    auto stepPs = std::max<long long>(1, std::llround(requestedStep * kPicosecondsPerSecond));
    auto step = static_cast<double>(stepPs) / kPicosecondsPerSecond;
    auto points = std::max(2, static_cast<int>(std::floor(request.stop / step)) + 1);
    if (points > kMaximumMixedPoints) {
        points = kMaximumMixedPoints;
        step = request.stop / static_cast<double>(points - 1);
        stepPs = std::max<long long>(1, std::llround(step * kPicosecondsPerSecond));
        step = static_cast<double>(stepPs) / kPicosecondsPerSecond;
        points = std::max(2, static_cast<int>(std::floor(request.stop / step)) + 1);
    }
    const auto effectiveStop = static_cast<double>(points - 1) * step;
    std::vector<double> grid;
    grid.reserve(static_cast<std::size_t>(points));
    for (int index = 0; index < points; ++index)
        grid.push_back(static_cast<double>(index) * step);

    std::vector<InputPortBinding> inputs;
    std::vector<OutputPortBinding> outputs;
    std::map<std::size_t, std::vector<std::size_t>> blockInputs;
    for (std::size_t blockIndex = 0; blockIndex < request.mixedSignal.blocks.size(); ++blockIndex) {
        const auto &block = request.mixedSignal.blocks[blockIndex];
        for (std::size_t portIndex = 0; portIndex < block.ports.size(); ++portIndex) {
            const auto &port = block.ports[portIndex];
            const auto bits = block.portBits.find(port.name);
            if (bits == block.portBits.end()) {
                result.state = JobState::failed;
                result.error =
                    "Mixed-signal block '" + block.instanceName + "' port '" + port.name + "' has no connected net";
                return result;
            }
            for (std::size_t bit = 0; bit < bits->second.size(); ++bit) {
                const auto signalName =
                    bits->second.size() == 1 ? port.name : port.name + "[" + std::to_string(bit) + "]";
                const auto tbSignal = "ice_b" + std::to_string(blockIndex) +
                                      (port.direction == HdlPortDirection::input ? "_in_" : "_out_") +
                                      std::to_string(portIndex) + "_" + std::to_string(bit);
                if (port.direction == HdlPortDirection::input) {
                    blockInputs[blockIndex].push_back(inputs.size());
                    inputs.push_back({blockIndex, portIndex, static_cast<int>(bit), bits->second[bit], port.real,
                                      signalName, tbSignal});
                } else if (port.direction == HdlPortDirection::output) {
                    outputs.push_back({blockIndex, portIndex, static_cast<int>(bit), bits->second[bit],
                                       "ice_blk_" + std::to_string(blockIndex), port.real, signalName, tbSignal});
                }
            }
        }
    }
    if (inputs.empty() && outputs.empty()) {
        result.state = JobState::failed;
        result.error = "Mixed-signal blocks have no scalar input or output ports";
        return result;
    }

    static std::atomic<std::uint64_t> counter{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    TempDirectory temp;
    temp.path = std::filesystem::temp_directory_path() /
                ("ice-mixed-" + std::to_string(stamp) + "-" + std::to_string(counter.fetch_add(1)));
    std::error_code status;
    std::filesystem::create_directories(temp.path, status);
    if (status) {
        result.state = JobState::failed;
        result.error = "Unable to create a mixed-signal directory: " + status.message();
        return result;
    }

    const auto inputPath = [&](std::size_t blockIndex) {
        return temp.path / ("inputs_" + std::to_string(blockIndex) + ".txt");
    };
    const auto vcdPath = temp.path / "ice_mixed.vcd";

    using InputStreams = std::vector<std::vector<double>>;
    const auto writeInputs = [&](const InputStreams &values) {
        for (std::size_t blockIndex = 0; blockIndex < request.mixedSignal.blocks.size(); ++blockIndex) {
            std::ofstream output(inputPath(blockIndex), std::ios::trunc);
            if (!output)
                return false;
            output << std::setprecision(17);
            const auto found = blockInputs.find(blockIndex);
            for (int point = 0; point < points; ++point) {
                if (found != blockInputs.end()) {
                    for (std::size_t index = 0; index < found->second.size(); ++index) {
                        if (index)
                            output << " ";
                        const auto inputIndex = found->second[index];
                        const auto value = values[inputIndex][static_cast<std::size_t>(point)];
                        if (inputs[inputIndex].real)
                            output << value;
                        else
                            output << (value > 0.5 ? 1 : 0);
                    }
                }
                output << "\n";
            }
            if (!output)
                return false;
        }
        return true;
    };

    const auto buildDigitalSource = [&] {
        std::ostringstream source;
        std::map<std::string, std::string> modules;
        for (const auto &block : request.mixedSignal.blocks) {
            if (modules.emplace(block.moduleName, block.source).second)
                source << block.source << "\n";
        }
        source << "`timescale 1ps/1ps\n";
        source << "module ice_mixed_tb;\n";
        source << "    localparam integer ICE_STEPS = " << points << ";\n";
        source << "    localparam integer ICE_STEP_PS = " << stepPs << ";\n";
        source << "    integer step;\n";
        for (std::size_t blockIndex = 0; blockIndex < request.mixedSignal.blocks.size(); ++blockIndex) {
            const auto &block = request.mixedSignal.blocks[blockIndex];
            for (std::size_t portIndex = 0; portIndex < block.ports.size(); ++portIndex) {
                const auto &port = block.ports[portIndex];
                const auto bits = block.portBits.at(port.name).size();
                const auto prefix = port.direction == HdlPortDirection::input ? "in" : "out";
                for (std::size_t bit = 0; bit < bits; ++bit) {
                    const auto type =
                        port.real ? "real" : (port.direction == HdlPortDirection::input ? "logic" : "wire");
                    source << "    " << type << " ice_b" << blockIndex << "_" << prefix << "_" << portIndex << "_"
                           << bit << ";\n";
                }
            }
            if (blockInputs.contains(blockIndex))
                source << "    integer fd_" << blockIndex << ";\n";
            for (const auto inputIndex : blockInputs[blockIndex])
                source << "    " << (inputs[inputIndex].real ? "real" : "integer") << " v_" << inputIndex << ";\n";
        }
        for (std::size_t blockIndex = 0; blockIndex < request.mixedSignal.blocks.size(); ++blockIndex) {
            const auto &block = request.mixedSignal.blocks[blockIndex];
            source << "    " << block.moduleName << " ice_blk_" << blockIndex << " (";
            for (std::size_t portIndex = 0; portIndex < block.ports.size(); ++portIndex) {
                if (portIndex)
                    source << ", ";
                const auto &port = block.ports[portIndex];
                const auto prefix = port.direction == HdlPortDirection::input ? "in" : "out";
                const auto bits = block.portBits.at(port.name).size();
                source << "." << port.name << "(";
                if (!port.real && bits > 1) {
                    source << "{";
                    for (std::size_t bit = bits; bit > 0; --bit) {
                        if (bit != bits)
                            source << ", ";
                        source << "ice_b" << blockIndex << "_" << prefix << "_" << portIndex << "_" << (bit - 1);
                    }
                    source << "}";
                } else {
                    source << "ice_b" << blockIndex << "_" << prefix << "_" << portIndex << "_0";
                }
                source << ")";
            }
            source << ");\n";
        }
        source << "    initial begin\n";
        source << "        $dumpfile(\"" << escapeVerilogString(vcdPath.string()) << "\");\n";
        source << "        $dumpvars(0, ice_mixed_tb);\n";
        for (std::size_t blockIndex = 0; blockIndex < request.mixedSignal.blocks.size(); ++blockIndex) {
            if (!blockInputs.contains(blockIndex))
                continue;
            source << "        fd_" << blockIndex << " = $fopen(\""
                   << escapeVerilogString(inputPath(blockIndex).string()) << "\", \"r\");\n";
            source << "        if (fd_" << blockIndex << " == 0) begin\n";
            source << "            $display(\"ICE mixed-signal input file could not be opened\");\n";
            source << "            $finish;\n";
            source << "        end\n";
        }
        source << "        for (step = 0; step < ICE_STEPS; step = step + 1) begin\n";
        for (std::size_t blockIndex = 0; blockIndex < request.mixedSignal.blocks.size(); ++blockIndex) {
            const auto found = blockInputs.find(blockIndex);
            if (found == blockInputs.end())
                continue;
            for (std::size_t index = 0; index < found->second.size(); ++index) {
                const auto inputIndex = found->second[index];
                source << "            if ($fscanf(fd_" << blockIndex << ", \""
                       << (inputs[inputIndex].real ? "%f" : "%d") << "\", v_" << inputIndex << ") != 1) begin\n";
                source << "                $display(\"ICE mixed-signal input underflow\");\n";
                source << "                $finish;\n";
                source << "            end\n";
            }
            for (std::size_t index = 0; index < found->second.size(); ++index) {
                const auto inputIndex = found->second[index];
                if (inputs[inputIndex].real)
                    source << "            " << inputs[inputIndex].tbSignal << " = v_" << inputIndex << ";\n";
                else
                    source << "            " << inputs[inputIndex].tbSignal << " = v_" << inputIndex << "[0];\n";
            }
        }
        source << "            #(ICE_STEP_PS);\n";
        source << "        end\n";
        source << "        $finish;\n";
        source << "    end\n";
        source << "endmodule\n";
        return source.str();
    };

    InputStreams currentInputs(inputs.size(), std::vector<double>(static_cast<std::size_t>(points), 0.0));
    InputStreams sampledInputs(inputs.size(), std::vector<double>(static_cast<std::size_t>(points), 0.0));
    SimulationResult finalDigital;
    SimulationResult finalAnalog;
    bool converged = false;
    const auto iterations = std::clamp(request.mixedSignal.iterations, 1, 20);

    for (int iteration = 0; iteration < iterations; ++iteration) {
        if (token.cancelled()) {
            result.state = JobState::cancelled;
            result.error = "simulation cancelled";
            return result;
        }
        if (!writeInputs(currentInputs)) {
            result.state = JobState::failed;
            result.error = "Unable to write mixed-signal digital input files";
            return result;
        }
        if (progress) {
            progress({0.05 + 0.7 * (static_cast<double>(iteration) / iterations),
                      "mixed-signal digital pass " + std::to_string(iteration + 1) + "/" + std::to_string(iterations)});
        }
        SimulationRequest digitalRequest;
        digitalRequest.analysis = "digital";
        digitalRequest.simulator = "iverilog";
        digitalRequest.stop = effectiveStop;
        digitalRequest.step = step;
        digitalRequest.workingDirectory = request.workingDirectory;
        digitalRequest.netlist = splitLines(buildDigitalSource());
        const auto digital = IcarusVerilogSimulator().run(digitalRequest, token, {});
        if (digital.state != JobState::succeeded) {
            result.state = digital.state;
            result.error = "mixed-signal digital phase failed: " + digital.error;
            appendWarnings(result.warnings, digital.warnings);
            return result;
        }
        finalDigital = digital;

        SimulationRequest analogRequest;
        analogRequest.analysis = "transient";
        analogRequest.simulator = "ngspice";
        analogRequest.stop = effectiveStop;
        analogRequest.step = step;
        analogRequest.use_initial_conditions = request.use_initial_conditions;
        analogRequest.initCommands = request.initCommands;
        analogRequest.workingDirectory = request.workingDirectory;
        analogRequest.netlist = request.netlist;

        std::set<std::string> inputNodes;
        for (const auto &input : inputs) {
            if (input.node != "0")
                inputNodes.insert(input.node);
        }
        std::size_t resistorIndex = 0;
        for (const auto &node : inputNodes) {
            analogRequest.netlist.push_back("RICE_HDL_IN_" + std::to_string(resistorIndex++) + " " + node + " 0 1G");
        }
        for (std::size_t outputIndex = 0; outputIndex < outputs.size(); ++outputIndex) {
            const auto &block = request.mixedSignal.blocks[outputs[outputIndex].block];
            const auto trace = findDigitalOutputTrace(finalDigital, outputs[outputIndex]);
            if (!trace) {
                result.state = JobState::failed;
                result.error = "Mixed-signal digital output '" + block.instanceName + "." +
                               outputs[outputIndex].signalName + "' was not found in the generated VCD";
                return result;
            }
            const auto values = sampleHold(finalDigital.time, trace->values, grid);
            const auto &sourcePort = block.ports[outputs[outputIndex].port];
            analogRequest.netlist.push_back(
                "VICE_HDL_OUT_" + std::to_string(outputIndex) + " " + outputs[outputIndex].node + " 0 " +
                pwlValue(grid, values, request.mixedSignal.outputHigh, request.mixedSignal.outputLow, sourcePort.real));
        }
        for (const auto &node : inputNodes) {
            if (!requestContainsSignal(analogRequest.probeSignals, node))
                analogRequest.probeSignals.push_back("v(" + node + ")");
        }
        std::set<std::string> outputNodes;
        for (const auto &output : outputs)
            outputNodes.insert(output.node);
        for (const auto &node : outputNodes) {
            if (!requestContainsSignal(analogRequest.probeSignals, node))
                analogRequest.probeSignals.push_back("v(" + node + ")");
        }
        for (const auto &signal : request.probeSignals) {
            if (!requestContainsSignal(analogRequest.probeSignals, signal))
                analogRequest.probeSignals.push_back(signal);
        }

        if (progress) {
            progress({0.05 + 0.7 * ((static_cast<double>(iteration) + 0.5) / iterations),
                      "mixed-signal analog pass " + std::to_string(iteration + 1) + "/" + std::to_string(iterations)});
        }
        const auto analog = NgspiceSimulator().run(analogRequest, token, {});
        if (analog.state != JobState::succeeded) {
            result.state = analog.state;
            result.error = "mixed-signal analog phase failed: " + analog.error;
            appendWarnings(result.warnings, analog.warnings);
            return result;
        }
        finalAnalog = analog;

        for (std::size_t index = 0; index < inputs.size(); ++index) {
            if (inputs[index].node == "0") {
                std::fill(sampledInputs[index].begin(), sampledInputs[index].end(), 0.0);
                continue;
            }
            const auto trace = findAnalogTrace(finalAnalog, inputs[index].node);
            if (!trace) {
                result.state = JobState::failed;
                result.error =
                    "Mixed-signal analog input node '" + inputs[index].node + "' was not returned by ngspice";
                return result;
            }
            const auto values = sampleLinear(finalAnalog.time, trace->values, grid);
            for (std::size_t point = 0; point < values.size(); ++point) {
                sampledInputs[index][point] = inputs[index].real
                                                  ? values[point]
                                                  : (values[point] > request.mixedSignal.logicThreshold ? 1.0 : 0.0);
            }
        }
        bool same = sampledInputs.size() == currentInputs.size();
        for (std::size_t index = 0; same && index < sampledInputs.size(); ++index) {
            if (sampledInputs[index].size() != currentInputs[index].size()) {
                same = false;
                break;
            }
            for (std::size_t point = 0; point < sampledInputs[index].size(); ++point) {
                const auto left = sampledInputs[index][point];
                const auto right = currentInputs[index][point];
                if (std::abs(left - right) > 1.0e-9 * std::max({1.0, std::abs(left), std::abs(right)})) {
                    same = false;
                    break;
                }
            }
        }
        if (same) {
            converged = true;
            break;
        }
        currentInputs = sampledInputs;
    }

    if (!converged)
        result.warnings.push_back("Mixed-signal co-simulation did not converge within " + std::to_string(iterations) +
                                  " iterations; the final pass is reported");
    if (finalDigital.state != JobState::succeeded || finalAnalog.state != JobState::succeeded) {
        result.state = JobState::failed;
        result.error = "Mixed-signal simulation did not complete a digital and analog pass";
        return result;
    }
    if (token.cancelled()) {
        result.state = JobState::cancelled;
        result.error = "simulation cancelled";
        return result;
    }

    result.time = grid;
    std::set<std::string> traceNames;
    for (const auto &trace : finalAnalog.traces) {
        if (trace.values.empty())
            continue;
        if (!traceNames.insert(trace.name).second)
            continue;
        result.traces.push_back({trace.name, sampleLinear(finalAnalog.time, trace.values, grid)});
    }
    for (std::size_t outputIndex = 0; outputIndex < outputs.size(); ++outputIndex) {
        const auto &block = request.mixedSignal.blocks[outputs[outputIndex].block];
        const auto trace = findDigitalOutputTrace(finalDigital, outputs[outputIndex]);
        if (!trace)
            continue;
        const auto name = block.instanceName + "." + outputs[outputIndex].signalName;
        if (!traceNames.insert(name).second)
            continue;
        result.traces.push_back({name, sampleHold(finalDigital.time, trace->values, grid)});
    }
    for (std::size_t inputIndex = 0; inputIndex < inputs.size(); ++inputIndex) {
        const auto &block = request.mixedSignal.blocks[inputs[inputIndex].block];
        const auto name = block.instanceName + "." + inputs[inputIndex].signalName;
        if (!traceNames.insert(name).second)
            continue;
        std::vector<double> values;
        values.reserve(static_cast<std::size_t>(points));
        for (const auto value : currentInputs[inputIndex])
            values.push_back(value);
        result.traces.push_back({name, std::move(values)});
    }
    appendWarnings(result.warnings, finalDigital.warnings);
    appendWarnings(result.warnings, finalAnalog.warnings);
    result.analysis = "mixed";
    result.simulator = "mixed";
    result.simulator_version = "ngspice + Icarus Verilog";
    if (progress)
        progress({1.0, "mixed-signal simulation complete"});
    result.state = JobState::succeeded;
    return result;
}

} // namespace ice
