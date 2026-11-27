#include "openvlsilib/simulation.hpp"

#include "openvlsilib/measure.hpp"
#include "openvlsilib/operations.hpp"
#include "openvlsilib/pdk.hpp"
#include "openvlsilib/platform.hpp"
#include "openvlsilib/process.hpp"
#include "openvlsilib/vcd.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <clocale>
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
#include <numbers>
#include <random>
#include <sstream>

#ifndef _WIN32
#include <locale.h>
#endif

namespace openvlsi {
using json = nlohmann::ordered_json;

namespace {
std::mutex ngspiceMutex;

// Qt installs the user's LC_NUMERIC locale (for example de_DE with a comma
// decimal separator). ngspice parses netlists and model parameters with the C
// library's strtod(), so a decimal comma makes values such as 0.5510 parse as
// two tokens. Force the C numeric locale on the calling thread while ngspice
// runs, without changing the locale seen by the GUI or other threads.
class ScopedNumericLocale {
  public:
    ScopedNumericLocale() {
#ifdef _WIN32
        if (const auto *current = std::setlocale(LC_NUMERIC, nullptr))
            previous_ = current;
        std::setlocale(LC_NUMERIC, "C");
#else
        locale_ = newlocale(LC_NUMERIC_MASK, "C", nullptr);
        if (locale_)
            previous_ = uselocale(locale_);
#endif
    }
    ~ScopedNumericLocale() {
#ifdef _WIN32
        if (!previous_.empty())
            std::setlocale(LC_NUMERIC, previous_.c_str());
#else
        if (locale_) {
            uselocale(previous_);
            freelocale(locale_);
        }
#endif
    }
    ScopedNumericLocale(const ScopedNumericLocale &) = delete;
    ScopedNumericLocale &operator=(const ScopedNumericLocale &) = delete;

  private:
#ifdef _WIN32
    std::string previous_;
#else
    locale_t locale_ = nullptr;
    locale_t previous_ = nullptr;
#endif
};

struct NgspiceLog {
    std::string text;
    std::mutex mutex;

    void clear() {
        std::lock_guard lock(mutex);
        text.clear();
    }

    void append(const char *message) {
        if (!message || !*message)
            return;
        std::lock_guard lock(mutex);
        text.append(message);
        if (!text.empty() && text.back() != '\n')
            text.push_back('\n');
        constexpr std::size_t limit = std::size_t{32} * 1024;
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
        return "A model include path is missing. Run openvlsi_doctor with the schematic or simulation and inspect "
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
    const auto mixDouble = [&](double value) { mix(reinterpret_cast<const char *>(&value), sizeof(value)); };
    const auto mixInt = [&](int value) { mix(reinterpret_cast<const char *>(&value), sizeof(value)); };
    mixDouble(request.start);
    mixDouble(request.stop);
    mixDouble(request.step);
    mixDouble(request.amplitude);
    mixDouble(request.time_constant);
    mixDouble(request.temperature);
    mixString(request.modelSection);
    mixDouble(request.dcStart);
    mixDouble(request.dcStop);
    mixDouble(request.dcStep);
    mixString(request.dcSource);
    mixString(request.acVariation);
    mixInt(request.acPoints);
    mixDouble(request.acStart);
    mixDouble(request.acStop);
    mixString(request.noiseOutput);
    mixString(request.noiseInput);
    mixString(request.noiseVariation);
    mixInt(request.noisePoints);
    mixDouble(request.noiseStart);
    mixDouble(request.noiseStop);
    mixString(request.tfOutput);
    mixString(request.tfSource);
    mix(reinterpret_cast<const char *>(&request.use_initial_conditions), sizeof(request.use_initial_conditions));
    for (const auto &[name, value] : request.parameters) {
        mixString(name);
        mixDouble(value);
    }
    for (const auto &[name, value] : request.sourceOverrides) {
        mixString(name);
        mixDouble(value);
    }
    for (const auto &line : request.netlist)
        mixString(line);
    for (const auto &signal : request.probeSignals)
        mixString(signal);
    for (const auto &command : request.initCommands)
        mixString(command);
    for (const auto &measurement : request.measurements) {
        mixString(measurement.name);
        mixString(measurement.metric);
        mixString(measurement.signal);
        mixString(measurement.input);
        mixDouble(measurement.voltage);
        mixDouble(measurement.tolerance);
    }
    std::ostringstream text;
    text << std::hex << std::setw(16) << std::setfill('0') << hash;
    return "fnv1a64:" + text.str();
}

std::string readTextFile(const std::filesystem::path &path, std::size_t limit = std::size_t{2} * 1024 * 1024) {
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

std::string lowerCopy(std::string text) {
    for (auto &character : text)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return text;
}

std::string trimCopy(std::string text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.erase(text.begin());
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.pop_back();
    return text;
}

std::string formatDouble(double value) {
    std::ostringstream stream;
    stream << std::setprecision(17) << value;
    return stream.str();
}

bool isSpiceComment(const std::string &line) {
    const auto trimmed = trimCopy(line);
    return !trimmed.empty() && (trimmed.front() == '*' || trimmed.front() == ';');
}

std::string firstSourceName(const std::vector<std::string> &netlist) {
    for (const auto &line : netlist) {
        const auto trimmed = trimCopy(line);
        if (trimmed.empty() || isSpiceComment(trimmed) || trimmed.front() == '.' || trimmed.front() == '+')
            continue;
        const auto first = trimmed.find_first_of(" \t");
        const auto name = lowerCopy(trimmed.substr(0, first));
        if (!name.empty() && (name.front() == 'v' || name.front() == 'i') && name.size() > 1)
            return name;
    }
    return {};
}

void applyNetlistOverrides(std::vector<std::string> &lines, const SimulationRequest &request) {
    const auto section = trimCopy(request.modelSection);
    for (auto &line : lines) {
        if (section.empty())
            break;
        const auto trimmed = trimCopy(line);
        const auto lowered = lowerCopy(trimmed);
        if (!lowered.starts_with(".lib") || lowered.starts_with(".library"))
            continue;
        // Replace the final section token while preserving the quoted model
        // file. Comments after the section are preserved.
        std::size_t comment = trimmed.find_first_of("*;");
        const auto body = trimCopy(trimmed.substr(0, comment));
        const auto lastSpace = body.find_last_of(" \t");
        if (lastSpace == std::string::npos || lastSpace == 0)
            continue;
        const auto before = body.substr(0, lastSpace + 1);
        std::string suffix;
        if (comment != std::string::npos)
            suffix = " " + trimmed.substr(comment);
        line = before + section + suffix;
    }
    for (const auto &[name, value] : request.parameters) {
        if (name.empty())
            continue;
        lines.push_back(".param " + name + "=" + formatDouble(value));
    }
    if (std::abs(request.temperature - 27.0) > 1e-9)
        lines.push_back(".temp " + formatDouble(request.temperature));
    std::erase_if(lines, [](const std::string &line) { return lowerCopy(trimCopy(line)) == ".end"; });
}

std::string stripLogPrefix(std::string line) {
    const auto trimmed = trimCopy(line);
    if (trimmed.starts_with("stdout "))
        return trimCopy(trimmed.substr(7));
    if (trimmed.starts_with("stderr "))
        return trimCopy(trimmed.substr(7));
    return trimmed;
}

bool ngspiceLogHasError(const NgspiceLog &log) {
    std::string text;
    {
        std::lock_guard lock(const_cast<NgspiceLog &>(log).mutex);
        text = log.text;
    }
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        const auto body = stripLogPrefix(line);
        if (body.starts_with("Error:") || body.starts_with("ERROR:"))
            return true;
    }
    return false;
}

std::vector<std::string> displayVectorNames(const NgspiceLog &log) {
    std::vector<std::string> names;
    std::string text;
    {
        std::lock_guard lock(const_cast<NgspiceLog &>(log).mutex);
        text = log.text;
    }
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        line = stripLogPrefix(line);
        const auto marker = line.find(" : ");
        if (marker == std::string::npos)
            continue;
        const auto name = trimCopy(line.substr(0, marker));
        if (name.empty() || name.find(' ') != std::string::npos || name.starts_with("Title") ||
            name.starts_with("Name"))
            continue;
        names.push_back(name);
    }
    return names;
}

std::string plotNameContaining(const NgspiceLog &log, const std::string &needle) {
    std::string text;
    {
        std::lock_guard lock(const_cast<NgspiceLog &>(log).mutex);
        text = log.text;
    }
    std::istringstream stream(text);
    std::string line;
    const auto loweredNeedle = lowerCopy(needle);
    while (std::getline(stream, line)) {
        line = stripLogPrefix(line);
        const auto lowered = lowerCopy(line);
        if (lowered.find(loweredNeedle) == std::string::npos)
            continue;
        std::istringstream tokens(line);
        std::string first;
        tokens >> first;
        if (!first.empty() && first != "Current" && first != "*")
            return first;
    }
    return {};
}

bool fetchRealVector(const std::string &name, std::vector<double> &values) {
    auto *vector = ngGet_Vec_Info(const_cast<char *>(name.c_str()));
    if (!vector || vector->v_length <= 0)
        return false;
    values.clear();
    values.reserve(static_cast<std::size_t>(vector->v_length));
    if (vector->v_realdata) {
        for (int index = 0; index < vector->v_length; ++index)
            values.push_back(vector->v_realdata[index]);
        return true;
    }
    if (vector->v_compdata) {
        for (int index = 0; index < vector->v_length; ++index)
            values.push_back(vector->v_compdata[index].cx_real);
        return true;
    }
    return false;
}

bool fetchComplexVector(const std::string &name, std::vector<double> &magnitude, std::vector<double> &phaseDegrees) {
    auto *vector = ngGet_Vec_Info(const_cast<char *>(name.c_str()));
    if (!vector || vector->v_length <= 0)
        return false;
    magnitude.clear();
    phaseDegrees.clear();
    magnitude.reserve(static_cast<std::size_t>(vector->v_length));
    phaseDegrees.reserve(static_cast<std::size_t>(vector->v_length));
    if (vector->v_compdata) {
        for (int index = 0; index < vector->v_length; ++index) {
            const auto real = vector->v_compdata[index].cx_real;
            const auto imaginary = vector->v_compdata[index].cx_imag;
            magnitude.push_back(std::hypot(real, imaginary));
            phaseDegrees.push_back(std::atan2(imaginary, real) * 180.0 / std::numbers::pi);
        }
        return true;
    }
    if (vector->v_realdata) {
        for (int index = 0; index < vector->v_length; ++index) {
            magnitude.push_back(std::abs(vector->v_realdata[index]));
            phaseDegrees.push_back(vector->v_realdata[index] < 0.0 ? 180.0 : 0.0);
        }
        return true;
    }
    return false;
}

std::string transferOutputNode(const std::string &expression) {
    const auto text = trimCopy(expression);
    const auto lowered = lowerCopy(text);
    if (lowered.starts_with("v(") && text.ends_with(')') && text.size() > 3)
        return trimCopy(text.substr(2, text.size() - 3));
    return text;
}

std::string vectorExpressionForSignal(const std::string &signal) {
    std::string name = trimCopy(signal);
    const auto lowered = lowerCopy(name);
    if (lowered.empty())
        return {};
    if ((lowered.starts_with("v(") || lowered.starts_with("i(") || lowered.starts_with('@')) && name.size() >= 3)
        return name;
    if (lowered.find("#branch") != std::string::npos || lowered.find("#input_impedance") != std::string::npos)
        return name;
    return "v(" + name + ")";
}

bool isAnalysisDirective(const std::string &line) {
    const auto lowered = lowerCopy(trimCopy(line));
    return lowered.starts_with(".tran") || lowered.starts_with(".dc") || lowered.starts_with(".ac") ||
           lowered.starts_with(".op") || lowered.starts_with(".noise") || lowered.starts_with(".tf") ||
           lowered.starts_with(".end");
}

std::vector<std::string> splitSignalList(const std::string &text) {
    std::vector<std::string> signals;
    std::size_t start = 0;
    while (start < text.size()) {
        const auto end = text.find_first_of(", \t\r\n", start);
        const auto token = trimCopy(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (!token.empty() && std::find(signals.begin(), signals.end(), token) == signals.end())
            signals.push_back(token);
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return signals;
}

std::string normalizedAnalysis(std::string analysis) {
    analysis = lowerCopy(trimCopy(analysis));
    if (analysis == "tran")
        return "transient";
    if (analysis == "dcsweep")
        return "dc";
    if (analysis == "acsweep")
        return "ac";
    if (analysis == "operating" || analysis == "operatingpoint")
        return "op";
    if (analysis == "transfer")
        return "tf";
    return analysis.empty() ? "transient" : analysis;
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
                capture += ',';
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
    ScopedNumericLocale numericLocale;
    SimulationResult result;
    result.state = JobState::running;
    result.analysis = lowerCopy(trimCopy(request.analysis));
    if (result.analysis == "tran")
        result.analysis = "transient";
    else if (result.analysis == "transfer")
        result.analysis = "tf";
    else if (result.analysis == "dcsweep")
        result.analysis = "dc";
    else if (result.analysis == "acsweep")
        result.analysis = "ac";
    else if (result.analysis == "operating" || result.analysis == "operatingpoint")
        result.analysis = "op";
    result.simulator = "ngspice";
    result.simulator_version = NGSPICE_PACKAGE_VERSION;
    result.input_fingerprint = fingerprintRequest(request);
    if (request.netlist.empty()) {
        result.state = JobState::failed;
        result.error = "ngspice requires a netlist";
        return result;
    }
    const auto analysis = result.analysis;
    const bool transient = analysis == "transient";
    const bool dc = analysis == "dc" || analysis == "dcsweep";
    const bool ac = analysis == "ac" || analysis == "acsweep";
    const bool op = analysis == "op" || analysis == "operating" || analysis == "operatingpoint";
    const bool noise = analysis == "noise";
    const bool tf = analysis == "tf";
    if (!transient && !dc && !ac && !op && !noise && !tf) {
        result.state = JobState::failed;
        result.error = "unsupported ngspice analysis: " + request.analysis;
        return result;
    }
    if (transient && (!(request.step > 0.0) || !(request.stop > 0.0))) {
        result.state = JobState::failed;
        result.error = "transient analysis requires positive step and stop times";
        return result;
    }
    if (dc && (!(request.dcStep > 0.0) || request.dcStop < request.dcStart)) {
        result.state = JobState::failed;
        result.error = "DC sweep requires a positive step and stop >= start";
        return result;
    }
    if (ac && (!(request.acPoints > 0) || !(request.acStart > 0.0) || !(request.acStop >= request.acStart))) {
        result.state = JobState::failed;
        result.error = "AC sweep requires positive points and start, with stop >= start";
        return result;
    }
    if (noise &&
        (!(request.noisePoints > 0) || !(request.noiseStart > 0.0) || !(request.noiseStop >= request.noiseStart))) {
        result.state = JobState::failed;
        result.error = "noise analysis requires positive points and start, with stop >= start";
        return result;
    }

    std::vector<std::string> lines = request.netlist;
    if (lines.empty() || !isSpiceComment(lines.front()))
        lines.insert(lines.begin(), "* OpenVLSI simulation");
    applyNetlistOverrides(lines, request);
    lines.push_back(".end");
    std::vector<char *> circuit;
    circuit.reserve(lines.size() + 1);
    for (auto &line : lines)
        circuit.push_back(line.data());
    circuit.push_back(nullptr);

    // ngspice's shared library keeps callbacks, OSDI models, circuits, and
    // plots in process-wide state. Initialize it once with a persistent
    // callback log; then clear the previous circuit/plots before each run while
    // keeping the loaded PDK models (ngSpice_Reset would unload the OSDI models
    // and break the next MOSFET load).
    static NgspiceLog ngspiceLog;
    static bool ngspiceInitialized = false;
    auto &log = ngspiceLog;
    log.clear();
    if (!ngspiceInitialized) {
        if (ngSpice_Init(collectOutput, collectStatus, collectExit, nullptr, nullptr, nullptr, &log) != 0) {
            result.state = JobState::failed;
            result.error = "ngspice could not initialize";
            appendFailureDetails(result.error, compactNgspiceLog(log));
            return result;
        }
        ngspiceInitialized = true;
    } else {
        char destroy[] = "destroy all";
        char remcirc[] = "remcirc";
        ngSpice_Command(destroy);
        // Remove accumulated circuits, stopping when ngspice reports that
        // there is nothing left to remove.
        for (int attempt = 0; attempt < 32; ++attempt) {
            if (ngSpice_Command(remcirc) != 0)
                break;
        }
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
    // ngspice 42 returns success from ngSpice_Circ even when a .include file
    // is missing (it only logs the error and leaves any previous circuit
    // loaded). Treat captured error diagnostics as a load failure so a stale
    // circuit is never simulated.
    if (ngSpice_Circ(circuit.data()) != 0 || ngspiceLogHasError(log)) {
        result.state = JobState::failed;
        result.error = "ngspice could not load the circuit";
        appendFailureDetails(result.error, compactNgspiceLog(log));
        return result;
    }
    for (const auto &[source, value] : request.sourceOverrides) {
        if (source.empty())
            continue;
        const auto command = "alter " + lowerCopy(source) + " = " + formatDouble(value);
        std::vector<char> buffer(command.begin(), command.end());
        buffer.push_back('\0');
        if (ngSpice_Command(buffer.data()) != 0) {
            result.state = JobState::failed;
            result.error = "ngspice could not apply source override: " + command;
            appendFailureDetails(result.error, compactNgspiceLog(log));
            return result;
        }
    }

    std::string command;
    std::string xVector;
    std::string xAxis = "time";
    std::string xUnit = "s";
    bool complexAnalysis = false;
    bool scalarAnalysis = false;
    std::string analysisOutput;
    std::string source;
    if (dc)
        source = trimCopy(request.dcSource);
    else if (noise)
        source = trimCopy(request.noiseInput);
    else if (tf)
        source = trimCopy(request.tfSource);
    if (source.empty())
        source = firstSourceName(request.netlist);
    if (transient) {
        command = "tran " + formatDouble(request.step) + " " + formatDouble(request.stop);
        if (request.start > 0.0)
            command += " " + formatDouble(request.start);
        if (request.use_initial_conditions)
            command += " uic";
        xVector = "time";
    } else if (dc) {
        if (source.empty()) {
            result.state = JobState::failed;
            result.error = "DC sweep requires a source name (for example V1)";
            return result;
        }
        command = "dc " + lowerCopy(source) + " " + formatDouble(request.dcStart) + " " + formatDouble(request.dcStop) +
                  " " + formatDouble(request.dcStep);
        xVector = "v-sweep";
        xAxis = lowerCopy(source) + " sweep";
        xUnit = "V";
    } else if (ac) {
        command = "ac " + request.acVariation + " " + std::to_string(request.acPoints) + " " +
                  formatDouble(request.acStart) + " " + formatDouble(request.acStop);
        xVector = "frequency";
        xAxis = "frequency";
        xUnit = "Hz";
        complexAnalysis = true;
    } else if (op) {
        command = "op";
        xAxis = "operating point";
        xUnit = "";
        scalarAnalysis = true;
    } else if (noise) {
        auto output = trimCopy(request.noiseOutput);
        if (output.empty() && !request.probeSignals.empty())
            output = request.probeSignals.front();
        if (output.empty()) {
            result.state = JobState::failed;
            result.error = "noise analysis requires an output node or signal";
            return result;
        }
        if (source.empty()) {
            result.state = JobState::failed;
            result.error = "noise analysis requires an input source name (for example V1)";
            return result;
        }
        const auto outputExpression = output.find('(') != std::string::npos ? output : "v(" + output + ")";
        command = "noise " + outputExpression + " " + lowerCopy(source) + " " + request.noiseVariation + " " +
                  std::to_string(request.noisePoints) + " " + formatDouble(request.noiseStart) + " " +
                  formatDouble(request.noiseStop);
        xVector = "frequency";
        xAxis = "frequency";
        xUnit = "Hz";
    } else {
        auto output = trimCopy(request.tfOutput);
        if (output.empty() && !request.probeSignals.empty())
            output = request.probeSignals.front();
        if (output.empty()) {
            result.state = JobState::failed;
            result.error = "transfer-function analysis requires an output node or signal";
            return result;
        }
        if (source.empty()) {
            result.state = JobState::failed;
            result.error = "transfer-function analysis requires an input source name (for example V1)";
            return result;
        }
        const auto outputExpression = output.find('(') != std::string::npos ? output : "v(" + output + ")";
        command = "tf " + outputExpression + " " + lowerCopy(source);
        xAxis = "transfer function";
        xUnit = "";
        scalarAnalysis = true;
        analysisOutput = transferOutputNode(output);
    }

    if (token.cancelled()) {
        result.state = JobState::cancelled;
        result.error = "simulation cancelled";
        return result;
    }
    auto commandBuffer = command;
    auto *commandText = commandBuffer.data();
    if (ngSpice_Command(commandText) != 0) {
        result.state = JobState::failed;
        result.error = "ngspice " + analysis + " analysis failed";
        appendFailureDetails(result.error, compactNgspiceLog(log));
        return result;
    }
    const auto analysisLog = compactNgspiceLog(log);
    if (analysisLog.find("aborted") != std::string::npos || analysisLog.find("not found") != std::string::npos ||
        analysisLog.find("no such device") != std::string::npos) {
        result.state = JobState::failed;
        result.error = "ngspice " + analysis + " analysis failed";
        appendFailureDetails(result.error, analysisLog);
        return result;
    }

    if (noise) {
        char listPlots[] = "setplot";
        ngSpice_Command(listPlots);
        const auto spectrumPlot = plotNameContaining(log, "Noise Spectral Density");
        if (!spectrumPlot.empty()) {
            auto setplot = std::string("setplot ") + spectrumPlot;
            ngSpice_Command(setplot.data());
        }
    }

    const auto fetchX = [&]() {
        if (xVector.empty()) {
            result.time = {0.0};
            return true;
        }
        return fetchRealVector(xVector, result.time) && !result.time.empty();
    };
    if (!fetchX()) {
        result.state = JobState::failed;
        result.error = "ngspice " + analysis + " did not produce the expected x axis (" + xVector + ")";
        appendFailureDetails(result.error, compactNgspiceLog(log));
        return result;
    }

    std::vector<std::string> requested;
    for (const auto &signal : request.probeSignals) {
        const auto trimmed = trimCopy(signal);
        if (!trimmed.empty() && std::find(requested.begin(), requested.end(), trimmed) == requested.end())
            requested.push_back(trimmed);
    }
    if (scalarAnalysis && requested.empty() && op) {
        char display[] = "display";
        ngSpice_Command(display);
        requested = displayVectorNames(log);
    }
    if (noise) {
        std::vector<std::string> noiseSignals = {"onoise_spectrum", "inoise_spectrum"};
        for (const auto &signal : requested) {
            const auto lowered = lowerCopy(signal);
            if (lowered.find("noise") != std::string::npos &&
                std::find(noiseSignals.begin(), noiseSignals.end(), signal) == noiseSignals.end())
                noiseSignals.push_back(signal);
        }
        requested = std::move(noiseSignals);
    } else if (tf) {
        std::vector<std::string> tfSignals = {"transfer_function", "output_impedance", "input_impedance"};
        for (const auto &signal : requested) {
            if (std::find(tfSignals.begin(), tfSignals.end(), signal) == tfSignals.end())
                tfSignals.push_back(signal);
        }
        requested = std::move(tfSignals);
    } else if (requested.empty()) {
        requested = {"out"};
    }

    const auto addScalar = [&](const std::string &name, double value, bool keepTrace) {
        result.scalars[name] = value;
        if (keepTrace) {
            SimulationTrace trace{name, {value}};
            result.traces.push_back(std::move(trace));
        }
    };
    const auto fetchNamedScalar = [&](const std::string &name, const std::vector<std::string> &candidates,
                                      bool keepTrace) {
        for (const auto &candidate : candidates) {
            std::vector<double> values;
            if (fetchRealVector(candidate, values) && values.size() == 1) {
                addScalar(name, values.front(), keepTrace);
                return true;
            }
        }
        return false;
    };

    if (op) {
        for (const auto &signal : requested) {
            const auto expression = vectorExpressionForSignal(signal);
            std::vector<double> values;
            if (fetchRealVector(expression, values) && values.size() == 1)
                addScalar(signal, values.front(), true);
        }
    } else if (tf) {
        const auto output = analysisOutput.empty() ? "out" : analysisOutput;
        const auto sourceName = lowerCopy(source);
        for (const auto &signal : requested) {
            const auto normalized = lowerCopy(signal);
            std::vector<std::string> candidates;
            if (normalized == "transfer_function" || normalized == "transfer-function" || normalized == "tf")
                candidates = {"Transfer_function", "transfer_function"};
            else if (normalized == "output_impedance" || normalized == "output-impedance")
                candidates = {"output_impedance_at_V(" + output + ")", "output_impedance_at_v(" + output + ")"};
            else if (normalized == "input_impedance" || normalized == "input-impedance")
                candidates = {sourceName + "#Input_impedance", sourceName + "#input_impedance"};
            else
                candidates = {signal, vectorExpressionForSignal(signal)};
            fetchNamedScalar(signal, candidates, true);
        }
    } else if (noise) {
        for (const auto &signal : requested) {
            const auto expression = signal.find('(') != std::string::npos ? signal : signal;
            std::vector<double> values;
            if (fetchRealVector(expression, values) && !values.empty()) {
                result.traces.push_back({signal, std::move(values)});
            }
        }
        char listPlots[] = "setplot";
        ngSpice_Command(listPlots);
        const auto integratedPlot = plotNameContaining(log, "Integrated Noise");
        if (!integratedPlot.empty()) {
            auto setplot = std::string("setplot ") + integratedPlot;
            ngSpice_Command(setplot.data());
            fetchNamedScalar("onoise_total", {"onoise_total"}, false);
            fetchNamedScalar("inoise_total", {"inoise_total"}, false);
        }
    } else {
        for (const auto &signal : requested) {
            if (complexAnalysis) {
                std::vector<double> magnitude;
                std::vector<double> phase;
                const auto expression = vectorExpressionForSignal(signal);
                if (!fetchComplexVector(expression, magnitude, phase))
                    continue;
                result.traces.push_back({signal, std::move(magnitude)});
                result.traces.push_back({signal + ".phase", std::move(phase)});
            } else {
                std::vector<double> values;
                if (signal == xVector) {
                    values = result.time;
                } else {
                    const auto expression = vectorExpressionForSignal(signal);
                    if (!fetchRealVector(expression, values))
                        continue;
                }
                result.traces.push_back({signal, std::move(values)});
            }
        }
    }

    if (result.traces.empty() && result.scalars.empty()) {
        result.state = JobState::failed;
        std::string listed;
        for (const auto &signal : requested) {
            if (!listed.empty())
                listed += ", ";
            listed += signal;
        }
        result.error = "ngspice did not produce the requested vectors (" + listed +
                       ") - check the Signals to capture list matches the schematic's nets";
        return result;
    }
    if (xVector.empty())
        result.time = {0.0};
    result.xAxis = xAxis;
    result.xUnit = xUnit;
    if (progress)
        progress({1.0, "ngspice " + analysis + " analysis complete"});
    result.state = JobState::succeeded;
    return result;
}

SimulationResult IcarusVerilogSimulator::run(const SimulationRequest &request, const CancellationToken &token,
                                             const ProgressCallback &progress) {
    SimulationResult result;
    result.state = JobState::running;
    result.analysis = "digital";
    result.simulator = "iverilog";
    result.simulator_version = "Icarus Verilog";
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
                      ("openvlsi-hdl-" + std::to_string(stamp) + "-" + std::to_string(counter.fetch_add(1)));
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
    const auto wrapperPath = temp / "openvlsi_wrapper.sv";
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
    std::vector<std::string> sourcePaths{sourcePath.string()};
    std::string compileTop = sourceTop;
    if (!sourceHasDump) {
        vcdPath = temp / "openvlsi_trace.vcd";
        std::ofstream wrapper(wrapperPath);
        wrapper << "`timescale 1ns/1ps\n"
                << "module openvlsi_vcd_top;\n"
                << "  " << sourceTop << " openvlsi_dut();\n"
                << "  initial begin\n"
                << "    $dumpfile(\"" << verilogStringLiteral(vcdPath) << "\");\n"
                << "    $dumpvars(0, openvlsi_vcd_top);\n"
                << "  end\n"
                << "endmodule\n";
        if (!wrapper) {
            cleanup();
            result.state = JobState::failed;
            result.error = "Unable to write the SystemVerilog VCD wrapper";
            return result;
        }
        sourcePaths.push_back(wrapperPath.string());
        compileTop = "openvlsi_vcd_top";
    }

    auto compileDirectory = temp;
    if (!request.workingDirectory.empty()) {
        std::error_code workingStatus;
        if (std::filesystem::is_directory(request.workingDirectory, workingStatus) && !workingStatus)
            compileDirectory = request.workingDirectory;
    }
    if (progress)
        progress({0.1, "compiling SystemVerilog testbench"});
    ProcessOptions compileOptions;
    compileOptions.arguments = {"iverilog", "-g2012", "-s", compileTop, "-o", executablePath.string()};
    for (const auto &sourcePath : sourcePaths)
        compileOptions.arguments.push_back(sourcePath);
    compileOptions.workingDirectory = compileDirectory;
    compileOptions.logPath = compileLogPath;
    compileOptions.timeoutSeconds = std::max(30, request.wallTimeoutSeconds > 0 ? request.wallTimeoutSeconds : 60);
    const auto compileResult = runProcess(compileOptions);
    if (token.cancelled()) {
        cleanup();
        result.state = JobState::cancelled;
        result.error = "simulation cancelled";
        return result;
    }
    if (!compileResult.succeeded()) {
        auto details = tailText(compileResult.logTail.empty() ? readTextFile(compileLogPath) : compileResult.logTail);
        cleanup();
        result.state = compileResult.timedOut ? JobState::timed_out : JobState::failed;
        result.error = compileResult.timedOut ? "iverilog compilation timed out"
                                              : "iverilog could not compile the SystemVerilog source";
        if (!details.empty())
            result.error += ": " + details;
        return result;
    }

    if (progress)
        progress({0.45, "running SystemVerilog testbench"});
    ProcessOptions runOptions;
    runOptions.arguments = {"vvp", executablePath.string()};
    runOptions.workingDirectory = temp;
    runOptions.logPath = runLogPath;
    runOptions.timeoutSeconds = request.wallTimeoutSeconds > 0 ? request.wallTimeoutSeconds : 10;
    const auto runResult = runProcess(runOptions);
    if (token.cancelled()) {
        cleanup();
        result.state = JobState::cancelled;
        result.error = "simulation cancelled";
        return result;
    }
    const bool timedOut = runResult.timedOut;
    if (!runResult.succeeded() && !timedOut) {
        auto details = tailText(runResult.logTail.empty() ? readTextFile(runLogPath) : runResult.logTail);
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

namespace {

SimulationResult runAdapterWithTimeout(const std::shared_ptr<SimulatorAdapter> &adapter,
                                       const SimulationRequest &request, std::chrono::milliseconds timeout,
                                       const std::shared_ptr<CancellationToken> &token,
                                       const ProgressCallback &progress) {
    auto effective = request;
    if (timeout.count() > 0) {
        const auto seconds = static_cast<int>(std::clamp<long long>((timeout.count() + 999) / 1000, 5, 3600));
        effective.wallTimeoutSeconds = seconds;
    }
    auto future = std::async(std::launch::async, [adapter, effective, token, progress] {
        return adapter->run(effective, *token, progress);
    });
    if (future.wait_for(timeout) == std::future_status::timeout) {
        token->cancel();
        // NOTE: std::future from std::async blocks on destruction, so this still
        // waits for the worker. Adapters with process-based execution react to
        // the token; in-process ngspice can only stop at its next boundary.
        auto result = future.get();
        if (result.state == JobState::succeeded || result.state == JobState::running) {
            result.state = JobState::timed_out;
            result.error = "simulation timed out";
            result.traces.clear();
            result.scalars.clear();
            result.measurements.clear();
        }
        return result;
    }
    auto result = future.get();
    if (result.state == JobState::succeeded && !request.measurements.empty())
        result.measurements = evaluateMeasurements(request.measurements, result);
    return result;
}

} // namespace

SimulationRunner::SimulationRunner(std::shared_ptr<SimulatorAdapter> adapter) : adapter_(std::move(adapter)) {}

SimulationResult SimulationRunner::run(const SimulationRequest &request, std::chrono::milliseconds timeout,
                                       const ProgressCallback &progress) {
    auto token = std::make_shared<CancellationToken>();
    {
        std::lock_guard lock(tokenMutex_);
        activeToken_ = token;
    }
    auto result = runAdapterWithTimeout(adapter_, request, timeout, token, progress);
    {
        std::lock_guard lock(tokenMutex_);
        if (activeToken_ == token)
            activeToken_.reset();
    }
    return result;
}

std::future<SimulationResult> SimulationRunner::runAsync(SimulationRequest request, std::chrono::milliseconds timeout,
                                                         ProgressCallback progress) {
    auto token = std::make_shared<CancellationToken>();
    {
        std::lock_guard lock(tokenMutex_);
        activeToken_ = token;
    }
    auto adapter = adapter_;
    return std::async(std::launch::async,
                      [adapter, request = std::move(request), timeout, progress = std::move(progress), token] {
                          return runAdapterWithTimeout(adapter, request, timeout, token, progress);
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

void Experiment::addCorner(SimulationCorner corner) {
    if (corner.name.empty())
        throw ProjectError("experiment.corner", "Simulation corners require a name");
    corners_.push_back(std::move(corner));
}

void Experiment::setMonteCarlo(SimulationMonteCarloOptions options) {
    if (options.enabled && options.samples <= 0)
        throw ProjectError("experiment.monte_carlo", "Monte Carlo requires at least one sample");
    monteCarlo_ = std::move(options);
}

std::vector<ExperimentPoint> Experiment::run(const SimulationRequest &base, std::chrono::milliseconds timeout,
                                             const ProgressCallback &progress) const {
    std::vector<ExperimentPoint> points;
    const auto applyValue = [](SimulationRequest &request, const std::string &name, double value) {
        if (name == "temperature") {
            request.temperature = value;
        } else if (name == "amplitude") {
            request.amplitude = value;
        } else if (name == "time_constant") {
            request.time_constant = value;
        } else if (name == "supply") {
            const auto source =
                request.sourceOverrides.empty() ? std::string{"V1"} : request.sourceOverrides.begin()->first;
            request.sourceOverrides[source] = value;
        } else if (name.starts_with("source.") || name.starts_with("source:")) {
            const auto source = name.substr(name.find_first_of(".:") + 1);
            if (!source.empty())
                request.sourceOverrides[source] = value;
        } else {
            request.parameters[name] = value;
        }
    };
    const auto applyCorner = [&](SimulationRequest &request, const SimulationCorner &corner) {
        if (!corner.modelSection.empty())
            request.modelSection = corner.modelSection;
        request.temperature = corner.temperature;
        for (const auto &[source, value] : corner.sourceOverrides)
            request.sourceOverrides[source] = value;
        for (const auto &[name, value] : corner.parameters)
            request.parameters[name] = value;
    };

    std::vector<SimulationCorner> corners = corners_;
    if (corners.empty()) {
        SimulationCorner nominal;
        nominal.name = "nominal";
        nominal.temperature = base.temperature;
        corners.push_back(std::move(nominal));
    }
    std::vector<std::map<std::string, double>> sweeps(1);
    for (const auto &axis : parameters_) {
        std::vector<std::map<std::string, double>> expanded;
        for (const auto &combination : sweeps) {
            for (const double value : axis.values) {
                auto next = combination;
                next[axis.name] = value;
                expanded.push_back(std::move(next));
            }
        }
        sweeps = std::move(expanded);
    }

    std::size_t total = 0;
    for (const auto &corner : corners) {
        (void)corner;
        total += sweeps.size() * static_cast<std::size_t>(monteCarlo_.enabled ? std::max(1, monteCarlo_.samples) : 1);
    }
    std::size_t completed = 0;
    SimulationRunner runner;
    for (std::size_t cornerIndex = 0; cornerIndex < corners.size(); ++cornerIndex) {
        for (const auto &sweep : sweeps) {
            const auto samples = monteCarlo_.enabled ? std::max(1, monteCarlo_.samples) : 1;
            for (int sample = 0; sample < samples; ++sample) {
                auto request = base;
                applyCorner(request, corners[cornerIndex]);
                for (const auto &[name, value] : sweep)
                    applyValue(request, name, value);
                std::map<std::string, double> applied = sweep;
                if (monteCarlo_.enabled) {
                    std::mt19937 generator(monteCarlo_.seed + static_cast<std::uint32_t>(cornerIndex * 100003) +
                                           static_cast<std::uint32_t>(sample) * 7919);
                    std::normal_distribution<double> normal(0.0, 1.0);
                    if (monteCarlo_.temperatureSigma > 0.0) {
                        request.temperature += normal(generator) * monteCarlo_.temperatureSigma;
                        applied["temperature"] = request.temperature;
                    }
                    if (monteCarlo_.supplySigmaPercent > 0.0 && !monteCarlo_.supplySource.empty()) {
                        const auto nominal = monteCarlo_.supplyNominal;
                        const auto varied =
                            nominal * (1.0 + normal(generator) * monteCarlo_.supplySigmaPercent / 100.0);
                        request.sourceOverrides[lowerCopy(monteCarlo_.supplySource)] = varied;
                        applied["supply"] = varied;
                    }
                    for (const auto &[name, sigma] : monteCarlo_.parameterSigmaPercent) {
                        if (!(sigma > 0.0))
                            continue;
                        const auto found = request.parameters.find(name);
                        const auto nominal = found == request.parameters.end() ? 0.0 : found->second;
                        const auto varied = nominal * (1.0 + normal(generator) * sigma / 100.0);
                        request.parameters[name] = varied;
                        applied[name] = varied;
                    }
                }
                ExperimentPoint point;
                point.corner = corners[cornerIndex].name;
                point.sample = monteCarlo_.enabled ? sample + 1 : 0;
                point.variant = point.corner;
                for (const auto &[name, value] : sweep)
                    point.variant += " " + name + "=" + formatDouble(value);
                if (monteCarlo_.enabled)
                    point.variant += "/mc" + std::to_string(point.sample);
                point.parameters = std::move(applied);
                point.result = runner.run(request, timeout, [&](const SimulationProgress &update) {
                    SimulationProgress scaled = update;
                    if (progress && total > 0)
                        scaled.fraction =
                            (static_cast<double>(completed) + update.fraction) / static_cast<double>(total);
                    if (progress)
                        progress(scaled);
                });
                points.push_back(std::move(point));
                ++completed;
            }
        }
    }
    return points;
}

SimulationRequest simulationRequestForItem(const Project &project, const SimulationItem &item,
                                           const std::filesystem::path &projectRoot, std::vector<std::string> *notes) {
    SimulationRequest request;
    request.analysis = normalizedAnalysis(item.analysis);
    request.simulator = item.simulator.empty() ? "ngspice" : item.simulator;
    request.start = item.start;
    request.stop = item.stop;
    request.step = item.step;
    request.use_initial_conditions = item.use_initial_conditions;
    request.temperature = item.temperature;
    request.modelSection = item.modelSection;
    request.parameters = item.parameters;
    request.sourceOverrides = item.sourceOverrides;
    request.dcSource = item.dcSource;
    request.dcStart = item.dcStart;
    request.dcStop = item.dcStop;
    request.dcStep = item.dcStep;
    request.acVariation = item.acVariation;
    request.acPoints = item.acPoints;
    request.acStart = item.acStart;
    request.acStop = item.acStop;
    request.noiseOutput = item.noiseOutput;
    request.noiseInput = item.noiseInput;
    request.noiseVariation = item.noiseVariation;
    request.noisePoints = item.noisePoints;
    request.noiseStart = item.noiseStart;
    request.noiseStop = item.noiseStop;
    request.tfOutput = item.tfOutput;
    request.tfSource = item.tfSource;
    request.measurements = item.measurements;
    request.probeSignals = splitSignalList(item.captureSignals);
    if (!projectRoot.empty())
        request.workingDirectory = projectRoot;

    const auto cellIt = project.schematicCells.find(item.schematic);
    if (cellIt == project.schematicCells.end())
        throw ProjectError("simulation.schematic", "Simulation '" + item.name + "' references a missing schematic");
    const auto &cell = cellIt->second;
    if (cell.systemVerilogSchematic) {
        request.simulator = "iverilog";
        request.analysis = "digital";
        const auto source = buildSystemVerilogSimulationSource(project, cell.id);
        std::istringstream stream(source);
        std::string line;
        while (std::getline(stream, line))
            request.netlist.push_back(line);
        if (notes)
            notes->push_back("SystemVerilog source cell");
    } else if (schematicContainsHdl(project, cell.id)) {
        std::string error;
        request.simulator = "mixed";
        request.analysis = "transient";
        if (!prepareMixedSignalRequest(request, project, cell.id, &error))
            throw ProjectError("simulation.mixed", error);
        applyPdkSimulationSetup(cell.pdk, request);
        if (notes)
            notes->push_back("Mixed-signal co-simulation (ngspice + Icarus Verilog)");
    } else {
        request.simulator = "ngspice";
        std::vector<std::string> netlistNotes;
        request.netlist = buildSimulationNetlist(project, cell.id, &netlistNotes);
        applyPdkSimulationSetup(cell.pdk, request);
        if (notes) {
            for (const auto &note : netlistNotes)
                notes->push_back(note);
        }
    }
    if (request.simulator == "ngspice" || request.simulator == "mixed") {
        std::istringstream directives(item.directives);
        std::string line;
        while (std::getline(directives, line)) {
            const auto trimmed = trimCopy(line);
            if (trimmed.empty() || isAnalysisDirective(trimmed))
                continue;
            request.netlist.push_back(trimmed);
        }
    }
    return request;
}

std::vector<SimulationCorner> pdkSimulationCorners(const std::string &pdkId) {
    std::vector<SimulationCorner> corners;
    if (pdkId.empty() || pdkId == genericPdkId)
        return corners;
    std::vector<PdkInfo> pdks;
    try {
        pdks = findPdks(pdkId);
    } catch (const std::exception &) {
        return corners;
    }
    if (pdks.empty())
        return corners;
    const PdkInfo *selected = &pdks.front();
    for (const auto &pdk : pdks) {
        if (pdk.simulationReady) {
            selected = &pdk;
            break;
        }
    }
    for (const auto &section : selected->sections) {
        const auto lowered = lowerCopy(section);
        const bool standard = lowered.ends_with("tt") || lowered.ends_with("ff") || lowered.ends_with("ss") ||
                              lowered.find("_tt") != std::string::npos || lowered.find("_ff") != std::string::npos ||
                              lowered.find("_ss") != std::string::npos;
        if (!standard)
            continue;
        SimulationCorner corner;
        corner.name = section;
        corner.modelSection = section;
        corners.push_back(std::move(corner));
    }
    return corners;
}

std::vector<ExperimentPoint> runSimulationExperiment(const Project &project, const SimulationItem &item,
                                                     std::chrono::milliseconds timeout,
                                                     const ProgressCallback &progress,
                                                     const std::filesystem::path &projectRoot) {
    const auto request = simulationRequestForItem(project, item, projectRoot);
    Experiment experiment;
    for (const auto &sweep : item.sweeps)
        experiment.addParameter(sweep.parameter, sweep.values);
    for (const auto &corner : item.corners)
        experiment.addCorner(corner);
    if (item.monteCarlo.enabled)
        experiment.setMonteCarlo(item.monteCarlo);
    return experiment.run(request, timeout, progress);
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
                {"x_axis", result.xAxis},
                {"x_unit", result.xUnit},
                {"time", result.time},
                {"scalars", result.scalars},
                {"simulator", result.simulator},
                {"simulator_version", result.simulator_version},
                {"input_fingerprint", result.input_fingerprint},
                {"error", result.error},
                {"warnings", result.warnings},
                {"traces", json::array()}};
    for (const auto &trace : result.traces)
        output["traces"].push_back({{"name", trace.name}, {"values", trace.values}});
    output["measurements"] = json::array();
    for (const auto &measurement : result.measurements) {
        json entry = {{"name", measurement.name},
                      {"metric", measurement.metric},
                      {"value", measurement.value},
                      {"unit", measurement.unit},
                      {"valid", measurement.valid}};
        if (!measurement.error.empty())
            entry["error"] = measurement.error;
        output["measurements"].push_back(std::move(entry));
    }
    return output.dump(2) + "\n";
}
bool ngspiceLibraryAvailable() {
    const auto check = [](const std::filesystem::path &path) {
        std::error_code status;
        return !path.empty() && std::filesystem::is_regular_file(path, status) && !status;
    };
    std::vector<std::filesystem::path> candidates;
    const auto executable = currentExecutablePath();
    if (!executable.empty()) {
#ifdef _WIN32
        candidates.push_back(executable.parent_path() / "ngspice.dll");
        candidates.push_back(executable.parent_path() / "libngspice.dll");
#elif defined(__APPLE__)
        candidates.push_back(executable.parent_path() / "libngspice.dylib");
#else
        candidates.push_back(executable.parent_path() / "libngspice.so");
#endif
    }
#ifdef _WIN32
    if (const auto found = findExecutableOnPath("ngspice.dll"))
        candidates.push_back(*found);
    if (const auto found = findExecutableOnPath("libngspice.dll"))
        candidates.push_back(*found);
#elif defined(__APPLE__)
    candidates.insert(candidates.end(), {"/opt/homebrew/lib/libngspice.dylib", "/usr/local/lib/libngspice.dylib",
                                         "/usr/lib/libngspice.dylib"});
    if (const auto found = findExecutableOnPath("libngspice.dylib"))
        candidates.push_back(*found);
#else
    candidates.insert(candidates.end(), {"/usr/lib/libngspice.so.0", "/usr/lib64/libngspice.so.0",
                                         "/usr/lib/x86_64-linux-gnu/libngspice.so.0", "/usr/lib/libngspice.so"});
#endif
    return std::any_of(candidates.begin(), candidates.end(), check);
}

} // namespace openvlsi
