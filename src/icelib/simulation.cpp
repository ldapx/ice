#include "icelib/simulation.hpp"

#include "icelib/vcd.hpp"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <mutex>
#include <ngspice/sharedspice.h>
#include <nlohmann/json.hpp>
#include <sstream>

#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace ice {
using json = nlohmann::ordered_json;

namespace {
std::mutex ngspiceMutex;

struct NgspiceLog {
    std::string text;
    std::mutex mutex;

    void append(const char *message) {
        if (!message || !*message)
            return;
        std::lock_guard lock(mutex);
        text.append(message);
        if (!text.empty() && text.back() != '\n')
            text.push_back('\n');
        constexpr std::size_t limit = 32 * 1024;
        if (text.size() > limit)
            text.erase(0, text.size() - limit);
    }
};

int collectOutput(char *output, int, void *userData) {
    if (userData)
        static_cast<NgspiceLog *>(userData)->append(output);
    return 0;
}

int collectStatus(char *status, int, void *userData) {
    if (userData)
        static_cast<NgspiceLog *>(userData)->append(status);
    return 0;
}

int collectExit(int status, NG_BOOL immediate, NG_BOOL quit, int, void *userData) {
    if (!userData)
        return 0;
    std::ostringstream message;
    message << "ngspice exit status " << status;
    if (immediate)
        message << " (immediate unload)";
    if (quit)
        message << " (requested by circuit)";
    static_cast<NgspiceLog *>(userData)->append(message.str().c_str());
    return 0;
}

std::string compactNgspiceLog(const NgspiceLog &log) {
    std::string text;
    {
        std::lock_guard lock(const_cast<NgspiceLog &>(log).mutex);
        text = log.text;
    }
    std::string compact;
    compact.reserve(text.size());
    for (const char character : text) {
        if (character != '\r')
            compact.push_back(character);
    }
    while (!compact.empty() && (compact.back() == '\n' || compact.back() == ' '))
        compact.pop_back();
    constexpr std::size_t limit = 4000;
    if (compact.size() > limit)
        compact = "..." + compact.substr(compact.size() - limit);
    return compact;
}

std::string ngspiceFailureHint(const std::string &details) {
    std::string lower = details;
    for (auto &character : lower)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    if (lower.find("could not find a valid modelname") != std::string::npos ||
        lower.find("unknown model") != std::string::npos)
        return "The netlist references a model that ngspice could not load. Check the PDK OSDI/PSP setup, the "
               ".lib section, and the device model names.";
    if (lower.find("section definition") != std::string::npos && lower.find("not found") != std::string::npos)
        return "The model file exists but the requested section is missing. Check the corner/section name in the "
               ".lib directive.";
    if (lower.find("could not find include file") != std::string::npos)
        return "A model include path is missing. Run ice_doctor with the schematic or simulation and inspect "
               "model_checks.";
    if (lower.find("cannot compute substitute") != std::string::npos)
        return "ngspice could not evaluate a model parameter. The PDK model stack may be incomplete; check the "
               "OSDI/PSP models and the selected corner.";
    if (lower.find("singular matrix") != std::string::npos)
        return "The circuit has a singular operating point. Check for floating nodes, missing DC paths, or "
               "conflicting sources.";
    return {};
}

void appendFailureDetails(std::string &error, const std::string &details) {
    if (!details.empty())
        error += ": " + details;
    const auto hint = ngspiceFailureHint(details);
    if (!hint.empty())
        error += "\nhint: " + hint;
}

std::string fingerprintRequest(const SimulationRequest &request) {
    // FNV-1a 64-bit over the analysis parameters and netlist for provenance.
    constexpr std::uint64_t kOffset = 1469598103934665603ULL;
    constexpr std::uint64_t kPrime = 1099511628211ULL;
    std::uint64_t hash = kOffset;
    const auto mix = [&](const void *data, std::size_t size) {
        const auto *bytes = static_cast<const unsigned char *>(data);
        for (std::size_t i = 0; i < size; ++i) {
            hash ^= bytes[i];
            hash *= kPrime;
        }
    };
    const auto mixString = [&](const std::string &text) {
        mix(text.data(), text.size());
        const char separator = '\0';
        mix(&separator, 1);
    };
    mixString(request.analysis);
    mixString(request.simulator);
    mixString(request.simulator_version);
    mix(reinterpret_cast<const char *>(&request.stop), sizeof(request.stop));
    mix(reinterpret_cast<const char *>(&request.step), sizeof(request.step));
    mix(reinterpret_cast<const char *>(&request.use_initial_conditions), sizeof(request.use_initial_conditions));
    for (const auto &line : request.netlist)
        mixString(line);
    for (const auto &signal : request.probeSignals)
        mixString(signal);
    for (const auto &command : request.initCommands)
        mixString(command);
    std::ostringstream text;
    text << std::hex << std::setw(16) << std::setfill('0') << hash;
    return "fnv1a64:" + text.str();
}

std::string shellQuote(const std::string &value) {
    std::string result = "'";
    for (const char character : value) {
        if (character == '\'')
            result += "'\\''";
        else
            result += character;
    }
    return result + "'";
}

std::string readTextFile(const std::filesystem::path &path, std::size_t limit = 2 * 1024 * 1024) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    std::string text;
    text.resize(limit);
    input.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(input.gcount()));
    return text;
}

std::string tailText(std::string text, std::size_t limit = 4000) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
        text.pop_back();
    if (text.size() > limit)
        text = "..." + text.substr(text.size() - limit);
    return text;
}

bool identifierCharacter(char character) {
    return std::isalnum(static_cast<unsigned char>(character)) || character == '_' || character == '$';
}

std::vector<std::string> verilogModules(const std::string &source) {
    std::vector<std::string> modules;
    std::size_t position = 0;
    while ((position = source.find("module", position)) != std::string::npos) {
        const bool leftBoundary = position == 0 || !identifierCharacter(source[position - 1]);
        const auto after = position + 6;
        const bool rightBoundary = after >= source.size() || !identifierCharacter(source[after]);
        if (!leftBoundary || !rightBoundary) {
            position = after;
            continue;
        }
        auto cursor = after;
        while (cursor < source.size() && std::isspace(static_cast<unsigned char>(source[cursor])))
            ++cursor;
        std::string name;
        while (cursor < source.size() && identifierCharacter(source[cursor]))
            name.push_back(source[cursor++]);
        if (!name.empty())
            modules.push_back(std::move(name));
        position = cursor;
    }
    return modules;
}

std::size_t identifierOccurrences(const std::string &source, const std::string &identifier) {
    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = source.find(identifier, position)) != std::string::npos) {
        const bool leftBoundary = position == 0 || !identifierCharacter(source[position - 1]);
        const auto after = position + identifier.size();
        const bool rightBoundary = after >= source.size() || !identifierCharacter(source[after]);
        if (leftBoundary && rightBoundary)
            ++count;
        position = after;
    }
    return count;
}

std::string verilogModuleWithDump(const std::string &source) {
    for (const auto &moduleName : verilogModules(source)) {
        std::size_t position = 0;
        while ((position = source.find(moduleName, position)) != std::string::npos) {
            auto keywordEnd = position;
            while (keywordEnd > 0 && std::isspace(static_cast<unsigned char>(source[keywordEnd - 1])))
                --keywordEnd;
            const bool precededByModule = keywordEnd >= 6 && source.compare(keywordEnd - 6, 6, "module") == 0;
            const bool keywordBoundary = keywordEnd == 6 || !identifierCharacter(source[keywordEnd - 7]);
            const auto afterName = position + moduleName.size();
            const bool nameBoundary = afterName >= source.size() || !identifierCharacter(source[afterName]);
            if (precededByModule && keywordBoundary && nameBoundary) {
                const auto end = source.find("endmodule", position + moduleName.size());
                const auto bodyEnd = end == std::string::npos ? source.size() : end;
                const auto body = source.substr(position + moduleName.size(), bodyEnd - position - moduleName.size());
                if (body.find("$dumpfile") != std::string::npos || body.find("$dumpvars") != std::string::npos)
                    return moduleName;
                break;
            }
            position += moduleName.size();
        }
    }
    return {};
}

std::string inferVerilogTop(const std::string &source, bool preferLast) {
    const auto modules = verilogModules(source);
    if (modules.empty())
        return {};
    if (preferLast) {
        // A source cell may hold both a design and its testbench. Prefer the
        // module that actually contains the dump commands; otherwise keep the
        // historical last-module fallback.
        const auto dumpTop = verilogModuleWithDump(source);
        return dumpTop.empty() ? modules.back() : dumpTop;
    }
    std::string best;
    for (const auto &moduleName : modules) {
        if (identifierOccurrences(source, moduleName) == 1)
            best = moduleName;
    }
    return best.empty() ? modules.back() : best;
}

std::vector<std::filesystem::path> vcdFiles(const std::filesystem::path &directory) {
    std::vector<std::filesystem::path> files;
    std::error_code status;
    for (std::filesystem::recursive_directory_iterator it(directory, status), end; !status && it != end;
         it.increment(status)) {
        if (!it->is_regular_file())
            continue;
        if (it->path().extension() == ".vcd")
            files.push_back(it->path());
    }
    return files;
}

std::filesystem::path parseVerilogDumpFile(const std::string &source, const std::filesystem::path &relativeBase) {
    auto position = source.find("$dumpfile");
    if (position == std::string::npos)
        return {};
    position = source.find('"', position);
    if (position == std::string::npos)
        return {};
    const auto end = source.find('"', position + 1);
    if (end == std::string::npos)
        return {};
    const std::filesystem::path path = source.substr(position + 1, end - position - 1);
    if (path.empty())
        return {};
    return path.is_absolute() ? path : relativeBase / path;
}

std::filesystem::path newestVcd(const std::filesystem::path &directory) {
    const auto files = vcdFiles(directory);
    if (files.empty())
        return {};
    auto newest = files.front();
    for (const auto &file : files) {
        std::error_code status;
        if (std::filesystem::last_write_time(file, status) > std::filesystem::last_write_time(newest, status) &&
            !status)
            newest = file;
    }
    return newest;
}

std::vector<SimulationTrace> filterVcdTraces(const VcdData &data, const std::vector<std::string> &wanted,
                                             std::vector<std::string> &warnings) {
    if (wanted.empty())
        return data.traces;
    const auto normalize = [](std::string text) {
        for (auto &character : text)
            character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        return text;
    };
    std::vector<SimulationTrace> traces;
    std::vector<std::string> missing;
    for (const auto &name : wanted) {
        const auto normalized = normalize(name);
        bool found = false;
        for (const auto &trace : data.traces) {
            const auto full = normalize(trace.name);
            if (full == normalized || full.ends_with("." + normalized) || normalized.ends_with("." + full)) {
                if (std::none_of(traces.begin(), traces.end(),
                                 [&](const auto &entry) { return entry.name == trace.name; }))
                    traces.push_back(trace);
                found = true;
            }
        }
        if (!found)
            missing.push_back(name);
    }
    if (traces.empty()) {
        warnings.push_back("Requested VCD signals were not found; returning all VCD traces");
        return data.traces;
    }
    for (const auto &name : missing)
        warnings.push_back("VCD signal not found: " + name);
    return traces;
}

int processExitCode(int status) {
#ifndef _WIN32
    if (status != -1 && WIFEXITED(status))
        return WEXITSTATUS(status);
#endif
    return status;
}

std::string verilogStringLiteral(const std::filesystem::path &path) {
    auto text = path.string();
    std::string result;
    result.reserve(text.size() + 2);
    for (const char character : text) {
        if (character == '\\' || character == '"')
            result.push_back('\\');
        result.push_back(character);
    }
    return result;
}

} // namespace

bool populateSimulationSignalsFromTraces(SimulationItem &simulation, const std::vector<SimulationTrace> &traces) {
    if (traces.empty())
        return false;
    std::vector<std::string> traceNames;
    traceNames.reserve(traces.size());
    for (const auto &trace : traces)
        traceNames.push_back(trace.name);
    bool changed = false;
    if (simulation.captureSignals.empty()) {
        std::string capture;
        for (std::size_t index = 0; index < traceNames.size(); ++index) {
            if (index)
                capture += ",";
            capture += traceNames[index];
        }
        simulation.captureSignals = std::move(capture);
        changed = true;
    }
    for (auto &graph : simulation.graphs) {
        if (graph.ySignals.empty()) {
            const auto count = std::min<std::size_t>(4, traceNames.size());
            graph.ySignals.assign(traceNames.begin(), traceNames.begin() + static_cast<std::ptrdiff_t>(count));
            changed = true;
        }
        if (graph.xAxis.empty()) {
            graph.xAxis = "time";
            changed = true;
        }
    }
    return changed;
}

void CancellationToken::cancel() noexcept { cancelled_.store(true); }
bool CancellationToken::cancelled() const noexcept { return cancelled_.load(); }

SimulationResult NgspiceSimulator::run(const SimulationRequest &request, const CancellationToken &token,
                                       const ProgressCallback &progress) {
    std::lock_guard lock(ngspiceMutex);
    SimulationResult result{1, JobState::running, request.analysis, {}, {}, "ngspice", NGSPICE_PACKAGE_VERSION, {}, {}};
    result.input_fingerprint = fingerprintRequest(request);
    if (request.netlist.empty()) {
        result.state = JobState::failed;
        result.error = "ngspice requires a netlist";
        return result;
    }
    if (request.analysis != "transient" && request.analysis != "tran") {
        result.state = JobState::failed;
        result.error = "unsupported ngspice analysis: " + request.analysis;
        return result;
    }
    if (!(request.step > 0.0) || !(request.stop > 0.0)) {
        result.state = JobState::failed;
        result.error = "transient analysis requires positive step and stop times";
        return result;
    }
    std::vector<std::string> lines = request.netlist;
    const auto firstLineIsComment = [](const std::string &line) {
        for (const char character : line) {
            if (std::isspace(static_cast<unsigned char>(character)))
                continue;
            return character == '*' || character == ';';
        }
        return false;
    };
    if (lines.empty() || !firstLineIsComment(lines.front()))
        lines.insert(lines.begin(), "* ICE simulation");
    lines.push_back(".end");
    std::vector<char *> circuit;
    circuit.reserve(lines.size() + 1);
    for (auto &line : lines)
        circuit.push_back(line.data());
    circuit.push_back(nullptr);
    NgspiceLog log;
    if (ngSpice_Init(collectOutput, collectStatus, collectExit, nullptr, nullptr, nullptr, &log) != 0) {
        result.state = JobState::failed;
        result.error = "ngspice could not initialize";
        appendFailureDetails(result.error, compactNgspiceLog(log));
        return result;
    }
    for (const auto &command : request.initCommands) {
        std::vector<char> buffer(command.begin(), command.end());
        buffer.push_back('\0');
        if (ngSpice_Command(buffer.data()) != 0) {
            result.state = JobState::failed;
            result.error = "ngspice simulator setup command failed: " + command;
            appendFailureDetails(result.error, compactNgspiceLog(log));
            return result;
        }
    }
    if (ngSpice_Circ(circuit.data()) != 0) {
        result.state = JobState::failed;
        result.error = "ngspice could not load the circuit";
        appendFailureDetails(result.error, compactNgspiceLog(log));
        return result;
    }
    std::ostringstream command;
    command << "tran " << request.step << " " << request.stop;
    if (request.use_initial_conditions)
        command << " uic";
    if (token.cancelled()) {
        result.state = JobState::cancelled;
        result.error = "simulation cancelled";
        return result;
    }
    auto commandText = command.str();
    if (ngSpice_Command(commandText.data()) != 0) {
        result.state = JobState::failed;
        result.error = "ngspice transient analysis failed";
        appendFailureDetails(result.error, compactNgspiceLog(log));
        return result;
    }
    auto *time = ngGet_Vec_Info(const_cast<char *>("time"));
    if (time == nullptr || time->v_realdata == nullptr || time->v_length <= 0) {
        result.state = JobState::failed;
        result.error = "ngspice did not produce a time vector";
        appendFailureDetails(result.error, compactNgspiceLog(log));
        return result;
    }

    // Bare names are node voltages. Explicit ngspice vector expressions are
    // passed through so current probes such as i(v1) and @l1[i] work too.
    const auto vectorName = [](const std::string &signal) {
        std::string name = signal;
        for (auto &character : name)
            character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        if ((name.rfind("v(", 0) == 0 || name.rfind("i(", 0) == 0 || name.rfind("@", 0) == 0) && name.size() >= 3)
            return name;
        return "v(" + name + ")";
    };

    std::vector<std::string> wanted = request.probeSignals;
    if (wanted.empty())
        wanted.push_back("out");

    std::vector<std::string> missing;
    const int timeCount = time->v_length;
    result.time.reserve(static_cast<std::size_t>(timeCount));
    for (int index = 0; index < timeCount; ++index) {
        if (token.cancelled()) {
            result.state = JobState::cancelled;
            result.error = "simulation cancelled";
            return result;
        }
        result.time.push_back(time->v_realdata[index]);
    }

    for (const auto &signal : wanted) {
        if (signal.empty())
            continue;
        const auto normalizedName = vectorName(signal);
        auto *vector = ngGet_Vec_Info(const_cast<char *>(normalizedName.c_str()));
        if (vector == nullptr || vector->v_realdata == nullptr || vector->v_length <= 0) {
            missing.push_back(signal);
            continue;
        }
        const int count = std::min(timeCount, vector->v_length);
        auto trace = SimulationTrace{signal, {}};
        trace.values.reserve(static_cast<std::size_t>(count));
        for (int index = 0; index < count; ++index) {
            if (token.cancelled()) {
                result.state = JobState::cancelled;
                result.error = "simulation cancelled";
                return result;
            }
            trace.values.push_back(vector->v_realdata[index]);
        }
        result.traces.push_back(std::move(trace));
    }

    if (result.traces.empty()) {
        result.state = JobState::failed;
        std::string listed;
        for (const auto &signal : wanted) {
            if (!listed.empty())
                listed += ", ";
            listed += signal;
        }
        result.error = "ngspice did not produce the requested vectors (" + listed +
                       ") — check the Signals to capture list matches the schematic's nets";
        return result;
    }
    for (const auto &signal : missing)
        result.warnings.push_back(signal + ": not captured by ngspice");
    if (progress)
        progress({1.0, "ngspice transient analysis complete"});
    result.state = JobState::succeeded;
    return result;
}

SimulationResult IcarusVerilogSimulator::run(const SimulationRequest &request, const CancellationToken &token,
                                             const ProgressCallback &progress) {
    SimulationResult result{1, JobState::running, "digital", {}, {}, "iverilog", "Icarus Verilog", {}, {}};
    result.input_fingerprint = fingerprintRequest(request);
    std::ostringstream sourceStream;
    for (std::size_t index = 0; index < request.netlist.size(); ++index) {
        if (index)
            sourceStream << "\n";
        sourceStream << request.netlist[index];
    }
    const auto source = sourceStream.str();
    if (source.find_first_not_of(" \t\r\n") == std::string::npos) {
        result.state = JobState::failed;
        result.error = "SystemVerilog simulation requires source text";
        return result;
    }
    const auto modules = verilogModules(source);
    if (modules.empty()) {
        result.state = JobState::failed;
        result.error = "SystemVerilog source contains no module declaration";
        return result;
    }
    if (token.cancelled()) {
        result.state = JobState::cancelled;
        result.error = "simulation cancelled";
        return result;
    }

    static std::atomic<std::uint64_t> counter{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto temp = std::filesystem::temp_directory_path() /
                      ("ice-hdl-" + std::to_string(stamp) + "-" + std::to_string(counter.fetch_add(1)));
    std::error_code status;
    std::filesystem::create_directories(temp, status);
    if (status) {
        result.state = JobState::failed;
        result.error = "Unable to create an HDL simulation directory: " + status.message();
        return result;
    }
    const auto cleanup = [&] {
        std::error_code ignored;
        std::filesystem::remove_all(temp, ignored);
    };

    const auto sourcePath = temp / "design.sv";
    const auto wrapperPath = temp / "ice_wrapper.sv";
    const auto executablePath = temp / "sim.vvp";
    const auto compileLogPath = temp / "iverilog.log";
    const auto runLogPath = temp / "vvp.log";
    {
        std::ofstream output(sourcePath);
        output << source;
        if (!output) {
            cleanup();
            result.state = JobState::failed;
            result.error = "Unable to write the SystemVerilog source file";
            return result;
        }
    }

    const bool sourceHasDump =
        source.find("$dumpfile") != std::string::npos || source.find("$dumpvars") != std::string::npos;
    const auto sourceTop = inferVerilogTop(source, sourceHasDump);
    if (sourceTop.empty()) {
        cleanup();
        result.state = JobState::failed;
        result.error = "Unable to determine the SystemVerilog top module";
        return result;
    }
    std::filesystem::path vcdPath;
    if (sourceHasDump)
        vcdPath = parseVerilogDumpFile(source, temp);
    std::string sources = " " + shellQuote(sourcePath.string());
    std::string compileTop = sourceTop;
    if (!sourceHasDump) {
        vcdPath = temp / "ice_trace.vcd";
        std::ofstream wrapper(wrapperPath);
        wrapper << "`timescale 1ns/1ps\n"
                << "module ice_vcd_top;\n"
                << "  " << sourceTop << " ice_dut();\n"
                << "  initial begin\n"
                << "    $dumpfile(\"" << verilogStringLiteral(vcdPath) << "\");\n"
                << "    $dumpvars(0, ice_vcd_top);\n"
                << "  end\n"
                << "endmodule\n";
        if (!wrapper) {
            cleanup();
            result.state = JobState::failed;
            result.error = "Unable to write the SystemVerilog VCD wrapper";
            return result;
        }
        sources += " " + shellQuote(wrapperPath.string());
        compileTop = "ice_vcd_top";
    }

    auto compileDirectory = temp;
    if (!request.workingDirectory.empty()) {
        std::error_code workingStatus;
        if (std::filesystem::is_directory(request.workingDirectory, workingStatus) && !workingStatus)
            compileDirectory = request.workingDirectory;
    }
    if (progress)
        progress({0.1, "compiling SystemVerilog testbench"});
    std::string compileCommand;
#ifdef _WIN32
    compileCommand = "cd /d " + shellQuote(compileDirectory.string()) + " && ";
#else
    compileCommand = "cd " + shellQuote(compileDirectory.string()) + " && ";
#endif
    compileCommand += "iverilog -g2012 -s " + shellQuote(compileTop) + " -o " + shellQuote(executablePath.string()) +
                      sources + " >" + shellQuote(compileLogPath.string()) + " 2>&1";
    const auto compileStatus = processExitCode(std::system(compileCommand.c_str()));
    if (token.cancelled()) {
        cleanup();
        result.state = JobState::cancelled;
        result.error = "simulation cancelled";
        return result;
    }
    if (compileStatus != 0) {
        auto details = tailText(readTextFile(compileLogPath));
        cleanup();
        result.state = JobState::failed;
        result.error = "iverilog could not compile the SystemVerilog source";
        if (!details.empty())
            result.error += ": " + details;
        return result;
    }

    if (progress)
        progress({0.45, "running SystemVerilog testbench"});
    std::string runCommand;
#ifdef _WIN32
    runCommand = "cd /d " + shellQuote(temp.string()) + " && vvp " + shellQuote(executablePath.string());
#else
    runCommand = "cd " + shellQuote(temp.string()) + " && timeout 10s vvp " + shellQuote(executablePath.string());
#endif
    runCommand += " >" + shellQuote(runLogPath.string()) + " 2>&1";
    const auto runStatus = processExitCode(std::system(runCommand.c_str()));
    if (token.cancelled()) {
        cleanup();
        result.state = JobState::cancelled;
        result.error = "simulation cancelled";
        return result;
    }
    const bool timedOut = runStatus == 124;
    if (runStatus != 0 && !timedOut) {
        auto details = tailText(readTextFile(runLogPath));
        cleanup();
        result.state = JobState::failed;
        result.error = "vvp failed while running the SystemVerilog testbench";
        if (!details.empty())
            result.error += ": " + details;
        return result;
    }

    auto actualVcd = vcdPath;
    if (actualVcd.empty() || !std::filesystem::is_regular_file(actualVcd, status) || status)
        actualVcd = newestVcd(temp);
    if (actualVcd.empty()) {
        auto details = tailText(readTextFile(runLogPath));
        cleanup();
        result.state = timedOut ? JobState::timed_out : JobState::failed;
        result.error = timedOut ? "SystemVerilog testbench timed out without producing a VCD"
                                : "SystemVerilog testbench produced no VCD file";
        if (!details.empty())
            result.error += ": " + details;
        return result;
    }

    auto vcd = parseVcdFile(actualVcd);
    cleanup();
    if (vcd.traces.empty()) {
        result.state = JobState::failed;
        result.error = "The generated VCD contains no graphable traces";
        for (const auto &warning : vcd.warnings)
            result.warnings.push_back(warning);
        return result;
    }
    result.time = std::move(vcd.time);
    result.traces = filterVcdTraces(vcd, request.probeSignals, result.warnings);
    for (const auto &warning : vcd.warnings) {
        if (std::find(result.warnings.begin(), result.warnings.end(), warning) == result.warnings.end())
            result.warnings.push_back(warning);
    }
    if (timedOut)
        result.warnings.push_back("vvp timed out; using the partial VCD produced before termination");
    if (result.traces.empty()) {
        result.state = JobState::failed;
        result.error = "No VCD traces matched the requested signals";
        return result;
    }
    if (progress)
        progress({1.0, "SystemVerilog simulation complete"});
    result.state = JobState::succeeded;
    return result;
}

SimulationResult AutoSimulator::run(const SimulationRequest &request, const CancellationToken &token,
                                    const ProgressCallback &progress) {
    if (request.simulator == "mixed" || request.simulator == "mixed-signal" || request.simulator == "ngspice+iverilog")
        return MixedSignalSimulator().run(request, token, progress);
    if (request.simulator == "iverilog" || request.simulator == "icarus" || request.simulator == "verilog")
        return IcarusVerilogSimulator().run(request, token, progress);
    return NgspiceSimulator().run(request, token, progress);
}

SimulationRunner::SimulationRunner(std::shared_ptr<SimulatorAdapter> adapter) : adapter_(std::move(adapter)) {}

SimulationResult SimulationRunner::run(const SimulationRequest &request, std::chrono::milliseconds timeout,
                                       const ProgressCallback &progress) {
    auto token = std::make_shared<CancellationToken>();
    {
        std::lock_guard lock(tokenMutex_);
        activeToken_ = token;
    }
    // Copy the adapter so the worker does not depend on `this` outliving the call.
    auto adapter = adapter_;
    auto future = std::async(std::launch::async,
                             [adapter, request, token, progress] { return adapter->run(request, *token, progress); });
    if (future.wait_for(timeout) == std::future_status::timeout) {
        token->cancel();
        // NOTE: std::future from std::async blocks on destruction, so this still
        // waits for the worker. ngspice has no async abort here; the timeout
        // marks the result while cancellation propagates cooperatively.
        auto result = future.get();
        if (result.state == JobState::succeeded || result.state == JobState::running)
            result = {1,
                      JobState::timed_out,
                      request.analysis,
                      {},
                      {},
                      request.simulator,
                      request.simulator_version,
                      result.input_fingerprint,
                      "simulation timed out"};
        {
            std::lock_guard lock(tokenMutex_);
            if (activeToken_ == token)
                activeToken_.reset();
        }
        return result;
    }
    auto result = future.get();
    {
        std::lock_guard lock(tokenMutex_);
        if (activeToken_ == token)
            activeToken_.reset();
    }
    return result;
}

std::future<SimulationResult> SimulationRunner::runAsync(SimulationRequest request, std::chrono::milliseconds timeout,
                                                         ProgressCallback progress) {
    auto adapter = adapter_;
    return std::async(std::launch::async,
                      [adapter, request = std::move(request), timeout, progress = std::move(progress)] {
                          SimulationRunner runner(adapter);
                          return runner.run(request, timeout, progress);
                      });
}

void SimulationRunner::cancel() noexcept {
    std::lock_guard lock(tokenMutex_);
    if (activeToken_)
        activeToken_->cancel();
}

void Experiment::addParameter(std::string name, std::vector<double> values) {
    if (name.empty() || values.empty())
        throw ProjectError("experiment.parameter", "Experiment parameters require a name and values");
    parameters_.push_back({std::move(name), std::move(values)});
}

std::vector<ExperimentPoint> Experiment::run(const SimulationRequest &base, std::chrono::milliseconds timeout,
                                             const ProgressCallback &progress) const {
    std::vector<ExperimentPoint> points;
    std::function<void(std::size_t, std::map<std::string, double>)> visit;
    visit = [&](std::size_t index, std::map<std::string, double> values) {
        if (index == parameters_.size()) {
            auto request = base;
            if (auto it = values.find("amplitude"); it != values.end())
                request.amplitude = it->second;
            if (auto it = values.find("time_constant"); it != values.end())
                request.time_constant = it->second;
            points.push_back({values, SimulationRunner().run(request, timeout, progress)});
            return;
        }
        for (double value : parameters_[index].values) {
            auto next = values;
            next[parameters_[index].name] = value;
            visit(index + 1, std::move(next));
        }
    };
    visit(0, {});
    return points;
}

std::string jobStateName(JobState state) {
    switch (state) {
    case JobState::queued:
        return "queued";
    case JobState::running:
        return "running";
    case JobState::succeeded:
        return "succeeded";
    case JobState::failed:
        return "failed";
    case JobState::cancelled:
        return "cancelled";
    case JobState::timed_out:
        return "timed_out";
    }
    return "unknown";
}

std::string serializeSimulationResult(const SimulationResult &result) {
    json output{{"schema_version", result.schema_version},
                {"state", jobStateName(result.state)},
                {"analysis", result.analysis},
                {"time", result.time},
                {"simulator", result.simulator},
                {"simulator_version", result.simulator_version},
                {"input_fingerprint", result.input_fingerprint},
                {"error", result.error},
                {"warnings", result.warnings},
                {"traces", json::array()}};
    for (const auto &trace : result.traces)
        output["traces"].push_back({{"name", trace.name}, {"values", trace.values}});
    return output.dump(2) + "\n";
}
} // namespace ice
