#include "icelib/vcd.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>

namespace ice {
namespace {

std::string trimCopy(std::string text) {
    const auto notSpace = [](unsigned char character) { return !std::isspace(character); };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), notSpace));
    text.erase(std::find_if(text.rbegin(), text.rend(), notSpace).base(), text.end());
    return text;
}

std::vector<std::string> splitTokens(const std::string &line) {
    std::istringstream stream(line);
    std::vector<std::string> tokens;
    std::string token;
    while (stream >> token)
        tokens.push_back(token);
    return tokens;
}

double parseTimeValue(const std::string &text) {
    try {
        return std::stod(text);
    } catch (const std::exception &) {
        return 0.0;
    }
}

double timescaleMultiplier(std::string unit) {
    for (auto &character : unit)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    if (unit == "s")
        return 1.0;
    if (unit == "ms")
        return 1.0e-3;
    if (unit == "us")
        return 1.0e-6;
    if (unit == "ns")
        return 1.0e-9;
    if (unit == "ps")
        return 1.0e-12;
    if (unit == "fs")
        return 1.0e-15;
    return 1.0e-9;
}

double parseTimescale(const std::vector<std::string> &tokens, bool &found) {
    found = false;
    for (const auto &token : tokens) {
        if (token.empty() || token.front() == '$')
            continue;
        std::string number;
        std::string unit;
        for (const auto character : token) {
            if (std::isdigit(static_cast<unsigned char>(character)) || character == '.' || character == '+' ||
                character == '-' || character == 'e' || character == 'E') {
                number += character;
            } else {
                unit += character;
            }
        }
        if (number.empty() || unit.empty())
            continue;
        const auto magnitude = parseTimeValue(number);
        if (magnitude <= 0.0)
            continue;
        found = true;
        return magnitude * timescaleMultiplier(unit);
    }
    return 1.0e-9;
}

double valueForChange(const VcdSignal &signal, const std::string &value, bool &ok) {
    ok = true;
    if (value.empty()) {
        ok = false;
        return 0.0;
    }
    if (signal.type == "real" || value.front() == 'r' || value.front() == 'R') {
        const auto text = value.front() == 'r' || value.front() == 'R' ? value.substr(1) : value;
        try {
            return std::stod(text);
        } catch (const std::exception &) {
            ok = false;
            return 0.0;
        }
    }
    if (value.front() == 'b' || value.front() == 'B') {
        double result = 0.0;
        for (std::size_t index = 1; index < value.size(); ++index) {
            const auto bit = value[index];
            result *= 2.0;
            if (bit == '1')
                result += 1.0;
            else if (bit != '0' && bit != 'x' && bit != 'X' && bit != 'z' && bit != 'Z')
                ok = false;
        }
        return result;
    }
    if (value.size() == 1) {
        if (value.front() == '1')
            return 1.0;
        if (value.front() == '0')
            return 0.0;
        if (value.front() == 'x' || value.front() == 'X' || value.front() == 'z' || value.front() == 'Z')
            return 0.0;
    }
    ok = false;
    return 0.0;
}

} // namespace

VcdData parseVcdText(const std::string &text, const std::string &sourceName) {
    VcdData data;
    std::vector<std::string> scope;
    std::map<std::string, std::size_t> signalById;
    std::map<std::string, std::vector<std::pair<double, std::string>>> events;
    std::set<double> times;
    std::set<double> eventTimes;
    bool sawTimescale = false;
    bool inDefinitions = true;
    double currentTime = 0.0;

    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        line = trimCopy(line);
        if (line.empty())
            continue;
        if (line.starts_with("$var")) {
            const auto tokens = splitTokens(line);
            if (tokens.size() < 5) {
                data.warnings.push_back("Malformed VCD $var declaration");
                continue;
            }
            VcdSignal signal;
            signal.type = tokens[1];
            try {
                signal.width = std::max(1, std::stoi(tokens[2]));
            } catch (const std::exception &) {
                signal.width = 1;
            }
            const auto id = tokens[3];
            std::string reference = tokens[4];
            if (!scope.empty()) {
                std::ostringstream full;
                for (std::size_t index = 0; index < scope.size(); ++index) {
                    if (index)
                        full << ".";
                    full << scope[index];
                }
                full << "." << reference;
                reference = full.str();
            }
            signal.name = reference;
            if (signalById.contains(id)) {
                data.warnings.push_back("VCD signal id declared more than once: " + id);
                continue;
            }
            signalById.emplace(id, data.signalDeclarations.size());
            data.signalDeclarations.push_back(std::move(signal));
        } else if (line.starts_with("$scope")) {
            const auto tokens = splitTokens(line);
            if (tokens.size() >= 3)
                scope.push_back(tokens[2]);
        } else if (line.starts_with("$upscope")) {
            if (!scope.empty())
                scope.pop_back();
        } else if (line.starts_with("$timescale")) {
            auto tokens = splitTokens(line);
            if (tokens.size() <= 2) {
                // Multi-line form: read until $end.
                std::string more;
                while (std::getline(stream, more)) {
                    more = trimCopy(more);
                    if (more == "$end")
                        break;
                    for (auto &token : splitTokens(more))
                        tokens.push_back(std::move(token));
                }
            }
            data.timescaleSeconds = parseTimescale(tokens, sawTimescale);
        } else if (line.starts_with("$enddefinitions")) {
            inDefinitions = false;
        } else if (line.starts_with("#")) {
            inDefinitions = false;
            currentTime = parseTimeValue(line.substr(1));
            times.insert(currentTime);
        } else if (line.starts_with("$")) {
            // $dumpvars, $dumpall, $dumpon, $dumpoff, $comment, $date,
            // $version, $end, ...
            continue;
        } else if (!inDefinitions) {
            const auto tokens = splitTokens(line);
            for (std::size_t index = 0; index < tokens.size();) {
                const auto &token = tokens[index];
                if (token.size() >= 2 && (token.front() == '0' || token.front() == '1' || token.front() == 'x' ||
                                          token.front() == 'X' || token.front() == 'z' || token.front() == 'Z')) {
                    const auto id = token.substr(1);
                    if (signalById.contains(id)) {
                        events[id].emplace_back(currentTime, token.substr(0, 1));
                        eventTimes.insert(currentTime);
                    }
                    ++index;
                } else if ((token.front() == 'b' || token.front() == 'B' || token.front() == 'r' ||
                            token.front() == 'R') &&
                           index + 1 < tokens.size()) {
                    const auto id = tokens[index + 1];
                    if (signalById.contains(id)) {
                        events[id].emplace_back(currentTime, token);
                        eventTimes.insert(currentTime);
                    }
                    index += 2;
                } else {
                    ++index;
                }
            }
        }
    }

    if (!sawTimescale)
        data.warnings.push_back("VCD has no $timescale; assuming 1 ns");
    if (data.signalDeclarations.empty())
        data.warnings.push_back("VCD contains no $var declarations");
    if (times.empty())
        times.insert(0.0);
    times.insert(eventTimes.begin(), eventTimes.end());

    data.time.reserve(times.size());
    for (const auto time : times)
        data.time.push_back(time * data.timescaleSeconds);

    // Rebuild events by id and replay them in time order.
    for (const auto &[id, signalIndex] : signalById) {
        const auto &signal = data.signalDeclarations[signalIndex];
        auto event = events.find(id);
        SimulationTrace trace;
        trace.name = signal.name;
        trace.values.reserve(times.size());
        double currentValue = 0.0;
        std::size_t eventIndex = 0;
        if (event != events.end()) {
            std::sort(event->second.begin(), event->second.end(),
                      [](const auto &left, const auto &right) { return left.first < right.first; });
        }
        for (const auto time : times) {
            if (event != events.end()) {
                while (eventIndex < event->second.size() && event->second[eventIndex].first <= time + 1.0e-15) {
                    bool ok = true;
                    currentValue = valueForChange(signal, event->second[eventIndex].second, ok);
                    if (!ok)
                        data.warnings.push_back("Unsupported VCD value for signal " + signal.name);
                    ++eventIndex;
                }
            }
            trace.values.push_back(currentValue);
        }
        data.traces.push_back(std::move(trace));
    }
    if (!sourceName.empty() && data.traces.empty())
        data.warnings.push_back("No traces found in " + sourceName);
    return data;
}

VcdData parseVcdFile(const std::filesystem::path &path) {
    std::ifstream input(path);
    if (!input) {
        VcdData data;
        data.warnings.push_back("Unable to open VCD file: " + path.string());
        return data;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return parseVcdText(buffer.str(), path.filename().string());
}

} // namespace ice
