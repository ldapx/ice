#include "icelib/drc.hpp"

#include "icelib/interchange.hpp"
#include "icelib/pdk.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string_view>
#include <vector>

namespace ice {
namespace {

struct XmlEvent {
    enum class Kind { start, end, text, endOfFile };
    Kind kind = Kind::endOfFile;
    std::string name;
    std::string text;
    bool selfClosing = false;
};

std::string trim(std::string text) {
    const auto whitespace = [](unsigned char character) { return std::isspace(character) != 0; };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), [&](char character) {
                   return !whitespace(static_cast<unsigned char>(character));
               }));
    text.erase(std::find_if(text.rbegin(), text.rend(),
                            [&](char character) { return !whitespace(static_cast<unsigned char>(character)); })
                   .base(),
               text.end());
    return text;
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return text;
}

bool startsWith(const std::string &text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool startsAt(const std::string &text, std::size_t position, std::string_view prefix) {
    return position <= text.size() && text.size() - position >= prefix.size() &&
           text.compare(position, prefix.size(), prefix) == 0;
}

void appendUtf8(std::string &output, unsigned long value) {
    if (value <= 0x7f) {
        output.push_back(static_cast<char>(value));
    } else if (value <= 0x7ff) {
        output.push_back(static_cast<char>(0xc0 | (value >> 6)));
        output.push_back(static_cast<char>(0x80 | (value & 0x3f)));
    } else if (value <= 0xffff) {
        output.push_back(static_cast<char>(0xe0 | (value >> 12)));
        output.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (value & 0x3f)));
    } else if (value <= 0x10ffff) {
        output.push_back(static_cast<char>(0xf0 | (value >> 18)));
        output.push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (value & 0x3f)));
    }
}

std::string decodeEntities(const std::string &text) {
    std::string output;
    output.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] != '&') {
            output.push_back(text[index]);
            continue;
        }
        const auto semicolon = text.find(';', index + 1);
        if (semicolon == std::string::npos || semicolon - index > 16) {
            output.push_back(text[index]);
            continue;
        }
        const auto entity = text.substr(index + 1, semicolon - index - 1);
        if (entity == "amp") {
            output.push_back('&');
        } else if (entity == "lt") {
            output.push_back('<');
        } else if (entity == "gt") {
            output.push_back('>');
        } else if (entity == "apos") {
            output.push_back('\'');
        } else if (entity == "quot") {
            output.push_back('"');
        } else if (startsWith(entity, "#x") || startsWith(entity, "#X")) {
            char *end = nullptr;
            const auto value = std::strtoul(entity.c_str() + 2, &end, 16);
            if (end != entity.c_str() + 2)
                appendUtf8(output, value);
            else
                output.append(text, index, semicolon - index + 1);
        } else if (startsWith(entity, "#")) {
            char *end = nullptr;
            const auto value = std::strtoul(entity.c_str() + 1, &end, 10);
            if (end != entity.c_str() + 1)
                appendUtf8(output, value);
            else
                output.append(text, index, semicolon - index + 1);
        } else {
            output.append(text, index, semicolon - index + 1);
        }
        index = semicolon;
    }
    return output;
}

class XmlReader {
  public:
    explicit XmlReader(const std::string &text) : text_(text) {}

    XmlEvent next() {
        if (position_ >= text_.size())
            return {};
        if (text_[position_] != '<') {
            const auto end = text_.find('<', position_);
            const auto length = end == std::string::npos ? text_.size() - position_ : end - position_;
            const auto raw = text_.substr(position_, length);
            position_ += length;
            return {XmlEvent::Kind::text, {}, decodeEntities(raw), false};
        }

        if (startsAt(text_, position_, "<?")) {
            const auto end = text_.find("?>", position_ + 2);
            if (end == std::string::npos)
                throw ProjectError("drc.lyrdb", "Unterminated XML processing instruction");
            position_ = end + 2;
            return next();
        }
        if (startsAt(text_, position_, "<!--")) {
            const auto end = text_.find("-->", position_ + 4);
            if (end == std::string::npos)
                throw ProjectError("drc.lyrdb", "Unterminated XML comment");
            position_ = end + 3;
            return next();
        }
        if (startsAt(text_, position_, "<![CDATA[")) {
            const auto end = text_.find("]]>", position_ + 9);
            if (end == std::string::npos)
                throw ProjectError("drc.lyrdb", "Unterminated XML CDATA section");
            const auto content = text_.substr(position_ + 9, end - position_ - 9);
            position_ = end + 3;
            return {XmlEvent::Kind::text, {}, content, false};
        }
        if (startsAt(text_, position_, "<!")) {
            const auto end = text_.find('>', position_ + 2);
            if (end == std::string::npos)
                throw ProjectError("drc.lyrdb", "Unterminated XML declaration");
            position_ = end + 1;
            return next();
        }

        const bool closing = position_ + 1 < text_.size() && text_[position_ + 1] == '/';
        position_ += closing ? 2 : 1;
        const auto nameStart = position_;
        while (position_ < text_.size()) {
            const char character = text_[position_];
            if (std::isspace(static_cast<unsigned char>(character)) || character == '>' || character == '/')
                break;
            ++position_;
        }
        const auto name = text_.substr(nameStart, position_ - nameStart);
        bool selfClosing = false;
        bool inQuote = false;
        char quote = 0;
        while (position_ < text_.size()) {
            const char character = text_[position_++];
            if (inQuote) {
                if (character == quote)
                    inQuote = false;
                continue;
            }
            if (character == '\'' || character == '"') {
                inQuote = true;
                quote = character;
                continue;
            }
            if (character == '>') {
                const auto slash = text_.rfind('/', position_ - 2);
                selfClosing = slash != std::string::npos && slash >= nameStart &&
                              text_.find_first_not_of(" \t\r\n", slash + 1) == position_ - 1;
                break;
            }
        }
        if (position_ > text_.size() || name.empty())
            throw ProjectError("drc.lyrdb", "Malformed XML tag");
        return {closing ? XmlEvent::Kind::end : XmlEvent::Kind::start, name, {}, selfClosing};
    }

  private:
    const std::string &text_;
    std::size_t position_ = 0;
};

struct CategoryFrame {
    std::string name;
    std::string description;
};

struct Bounds {
    bool hasPoint = false;
    double minX = 0.0;
    double minY = 0.0;
    double maxX = 0.0;
    double maxY = 0.0;

    void add(double x, double y) {
        if (!hasPoint) {
            hasPoint = true;
            minX = maxX = x;
            minY = maxY = y;
            return;
        }
        minX = std::min(minX, x);
        maxX = std::max(maxX, x);
        minY = std::min(minY, y);
        maxY = std::max(maxY, y);
    }
};

std::vector<double> numbersIn(const std::string &value) {
    std::vector<double> numbers;
    const char *cursor = value.c_str();
    while (*cursor != '\0') {
        char *end = nullptr;
        const double number = std::strtod(cursor, &end);
        if (end == cursor) {
            ++cursor;
            continue;
        }
        if (std::isfinite(number))
            numbers.push_back(number);
        cursor = end;
    }
    return numbers;
}

std::string textValue(const std::string &value) {
    const auto colon = value.find(':');
    auto result = trim(colon == std::string::npos ? value : value.substr(colon + 1));
    if (result.size() >= 2 &&
        ((result.front() == '\'' && result.back() == '\'') || (result.front() == '"' && result.back() == '"')))
        result = result.substr(1, result.size() - 2);
    return result;
}

VerificationSeverity parseSeverity(const std::string &value) {
    const auto normalized = lower(trim(value));
    if (normalized == "warning" || normalized == "warn")
        return VerificationSeverity::warning;
    if (normalized == "info" || normalized == "information")
        return VerificationSeverity::info;
    return VerificationSeverity::error;
}

Coordinate toNanometers(double micrometers) {
    constexpr double nanometersPerMicrometer = 1000.0;
    return static_cast<Coordinate>(std::llround(micrometers * nanometersPerMicrometer));
}

std::string replaceAll(std::string text, const std::string &from, const std::string &to) {
    if (from.empty())
        return text;
    std::size_t position = 0;
    while ((position = text.find(from, position)) != std::string::npos) {
        text.replace(position, from.size(), to);
        position += to.size();
    }
    return text;
}

std::string expandEnvironment(std::string text) {
    if (text == "~" || text.starts_with("~/")) {
        if (const char *home = std::getenv("HOME"); home && *home)
            text = std::string(home) + text.substr(1);
    }
    std::string result;
    result.reserve(text.size());
    for (std::size_t index = 0; index < text.size();) {
        if (text[index] != '$') {
            result.push_back(text[index++]);
            continue;
        }
        std::size_t start = index + 1;
        std::size_t end = start;
        if (end < text.size() && text[end] == '{') {
            start = end + 1;
            end = text.find('}', start);
            if (end == std::string::npos) {
                result.push_back(text[index++]);
                continue;
            }
            const auto name = text.substr(start, end - start);
            if (const char *value = std::getenv(name.c_str()); value && *value)
                result += value;
            index = end + 1;
            continue;
        }
        while (end < text.size() && (std::isalnum(static_cast<unsigned char>(text[end])) != 0 || text[end] == '_'))
            ++end;
        if (end == start) {
            result.push_back(text[index++]);
            continue;
        }
        const auto name = text.substr(start, end - start);
        if (const char *value = std::getenv(name.c_str()); value && *value)
            result += value;
        index = end;
    }
    return result;
}

std::vector<std::filesystem::path> expandGlob(const std::filesystem::path &pattern) {
    const auto text = pattern.string();
    if (text.find('*') == std::string::npos)
        return {pattern};
    auto root = pattern.root_path();
    if (root.empty())
        root = std::filesystem::current_path();
    std::vector<std::filesystem::path> current{root};
    for (const auto &part : pattern.relative_path()) {
        std::vector<std::filesystem::path> next;
        if (part.string() == "*") {
            for (const auto &base : current) {
                std::error_code status;
                for (std::filesystem::directory_iterator it(base, status), end; !status && it != end;
                     it.increment(status)) {
                    if (it->is_directory())
                        next.push_back(it->path());
                }
            }
        } else {
            for (const auto &base : current)
                next.push_back(base / part);
        }
        current = std::move(next);
    }
    return current;
}

std::filesystem::path resolveDrcDeckPath(const std::filesystem::path &packageRoot, const std::string &deckName,
                                         const std::vector<std::string> &searchPaths) {
    if (deckName.empty())
        return {};
    const auto deck = std::filesystem::path(deckName);
    std::vector<std::filesystem::path> candidates;
    if (deck.is_absolute()) {
        candidates.push_back(deck);
    } else {
        candidates.push_back(packageRoot / "technology" / deck);
        candidates.push_back(packageRoot / deck);
    }
    for (const auto &raw : searchPaths) {
        const auto expanded = expandEnvironment(raw);
        if (expanded.empty())
            continue;
        for (const auto &base : expandGlob(expanded)) {
            std::error_code status;
            if (std::filesystem::is_directory(base, status) && !status) {
                candidates.push_back(base / deck.filename());
                candidates.push_back(base / deck);
            } else {
                candidates.push_back(base);
            }
        }
    }
    for (const auto &candidate : candidates) {
        std::error_code status;
        if (std::filesystem::is_regular_file(candidate, status) && !status)
            return candidate;
    }
    return {};
}

std::vector<std::string> stringArray(const nlohmann::json &value) {
    std::vector<std::string> result;
    if (!value.is_array())
        return result;
    for (const auto &entry : value) {
        if (entry.is_string())
            result.push_back(entry.get<std::string>());
    }
    return result;
}

} // namespace

std::optional<DrcDeck> drcDeckForPdk(const std::string &pdk, const std::string &suite, std::string *error) {
    if (error)
        error->clear();
    if (pdk.empty() || pdk == genericPdkId)
        return std::nullopt;
    std::string lastError;
    for (const auto &info : findPdks(pdk)) {
        const auto configPath = info.packageRoot / "technology" / "drc.json";
        std::error_code status;
        if (!std::filesystem::is_regular_file(configPath, status) || status)
            continue;
        std::ifstream stream(configPath);
        if (!stream) {
            lastError = "Unable to read DRC deck configuration: " + configPath.string();
            continue;
        }
        nlohmann::json config;
        try {
            stream >> config;
        } catch (const std::exception &exception) {
            lastError = "Invalid DRC deck configuration " + configPath.string() + ": " + exception.what();
            continue;
        }
        DrcDeck deck;
        deck.name = config.value("name", std::string{});
        deck.engine = config.value("engine", std::string{});
        deck.process = config.value("process", std::string{});
        deck.pdkPath = config.value("pdk_path", std::string{});
        std::string deckName = config.value("deck", std::string{});
        auto arguments = stringArray(config.value("arguments", nlohmann::json::array()));
        auto searchPaths = stringArray(config.value("search_paths", nlohmann::json::array()));
        if (!suite.empty()) {
            const auto suites = config.find("suites");
            if (suites != config.end() && suites->is_object() && suites->contains(suite)) {
                const auto &entry = suites->at(suite);
                if (entry.contains("engine"))
                    deck.engine = entry.value("engine", deck.engine);
                if (entry.contains("process"))
                    deck.process = entry.value("process", deck.process);
                if (entry.contains("pdk_path"))
                    deck.pdkPath = entry.value("pdk_path", deck.pdkPath);
                if (entry.contains("deck"))
                    deckName = entry.value("deck", deckName);
                if (entry.contains("arguments"))
                    arguments = stringArray(entry.at("arguments"));
            }
        }
        std::string key;
        for (const auto character : pdk) {
            if (std::isalnum(static_cast<unsigned char>(character)) != 0)
                key.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(character))));
        }
        if (!key.empty()) {
            if (const char *specific = std::getenv(("ICE_DRC_DECK_" + key).c_str()); specific && *specific)
                deckName = specific;
        }
        if (deckName.empty()) {
            if (const char *generic = std::getenv("ICE_DRC_DECK"); generic && *generic)
                deckName = generic;
        }
        if (deck.engine.empty()) {
            const auto extension = std::filesystem::path(deckName).extension().string();
            deck.engine = (extension == ".lydrc" || extension == ".drc" || extension == ".rb") ? "klayout" : "gdscheck";
        }
        deck.arguments = std::move(arguments);
        if (deck.engine == "klayout") {
            deck.path = resolveDrcDeckPath(info.packageRoot, deckName, searchPaths);
            if (deck.path.empty()) {
                if (error)
                    *error = "No KLayout DRC deck found for '" + pdk + "'" +
                             (deckName.empty() ? std::string{} : " (expected " + deckName + ")");
            }
        } else {
            if (deck.process.empty())
                deck.process = gdsCheckProcessForPdk(pdk);
            if (deck.pdkPath.empty())
                deck.pdkPath = deckName;
            std::error_code pathStatus;
            if (!deck.pdkPath.empty() && !std::filesystem::is_directory(deck.pdkPath, pathStatus))
                deck.pdkPath.clear();
        }
        return deck;
    }
    if (error)
        *error = std::move(lastError);
    return std::nullopt;
}

std::string gdsCheckProcessForPdk(const std::string &pdk) {
    if (pdk.empty() || pdk == genericPdkId)
        return {};
    // Packages normally declare the exact process in technology/drc.json. This
    // fallback keeps legacy projects working for identifiers that already look
    // like process tokens, and normalizes display-style ids such as
    // "IHP SG13G2" or "ihp.sg13g2" into "ihp-sg13g2".
    if (pdk.find_first_of(". _") == std::string::npos)
        return pdk;
    std::string process;
    bool pendingSeparator = false;
    for (const unsigned char character : pdk) {
        if (std::isalnum(character) != 0) {
            if (pendingSeparator && !process.empty())
                process.push_back('-');
            pendingSeparator = false;
            process.push_back(static_cast<char>(std::tolower(character)));
        } else {
            pendingSeparator = true;
        }
    }
    return process.empty() ? pdk : process;
}

std::vector<std::string> gdsCheckArguments(const GdsCheckOptions &options) {
    std::vector<std::string> arguments{"run",           "--input",       options.input.string(),
                                       "--process",     options.process, "--topcell",
                                       options.topCell, "--report",      options.report.string()};
    if (!options.suite.empty()) {
        arguments.push_back("--suite");
        arguments.push_back(options.suite);
    }
    return arguments;
}

LayoutDrcResult runLayoutDrc(const Project &project, ObjectId layoutCellId, const LayoutDrcOptions &options,
                             const ProcessCancellation *cancellation, const ProcessOutputCallback &onOutput) {
    LayoutDrcResult result;
    result.layout = layoutCellId;
    const auto cell = project.layoutCells.find(layoutCellId);
    if (cell == project.layoutCells.end()) {
        result.error = "Layout cell does not exist";
        return result;
    }
    result.layoutName = cell->second.name;

    std::optional<DrcDeck> configured;
    std::string deckError;
    if (!options.deck.empty()) {
        DrcDeck deck;
        deck.engine = options.engine.empty() ? "klayout" : options.engine;
        deck.path = options.deck;
        deck.name = options.deck.filename().string();
        configured = std::move(deck);
    } else {
        configured = drcDeckForPdk(cell->second.pdk, options.suite, &deckError);
    }
    std::string processName;
    if (configured) {
        if (!options.engine.empty())
            configured->engine = options.engine;
        result.engine = configured->engine;
        result.deckPath = configured->path;
        if (configured->engine == "klayout" && configured->path.empty()) {
            result.error = deckError.empty() ? "The layout PDK has no KLayout DRC deck" : deckError;
            return result;
        }
        if (configured->engine != "klayout") {
            result.engine = "gdscheck";
            processName = configured->process.empty() ? gdsCheckProcessForPdk(cell->second.pdk) : configured->process;
            if (processName.empty()) {
                result.error = "The layout PDK has no gdscheck process deck";
                return result;
            }
        }
    } else {
        processName = gdsCheckProcessForPdk(cell->second.pdk);
        if (processName.empty()) {
            result.error = deckError.empty() ? "The layout PDK has no DRC deck" : deckError;
            return result;
        }
        result.engine = "gdscheck";
    }

    auto workDirectory =
        options.workDirectory.empty()
            ? (std::filesystem::temp_directory_path() /
               ("ice-drc-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
            : options.workDirectory;
    std::error_code status;
    // The DRC process runs with workDirectory as its cwd, so a relative
    // workDirectory would make the exported GDS path relative to itself and
    // gdscheck would not find it.
    workDirectory = std::filesystem::absolute(workDirectory, status);
    if (status) {
        result.error = "Unable to resolve DRC work directory: " + status.message();
        return result;
    }
    std::filesystem::create_directories(workDirectory, status);
    if (status) {
        result.error = "Unable to create DRC work directory: " + status.message();
        return result;
    }
    result.gdsPath = workDirectory / "layout.gds";
    result.reportPath = workDirectory / "drc.lyrdb";
    result.logPath = workDirectory / "drc.log";
    try {
        if (options.flatten)
            LayoutInterchange::exportFlattenedGds(project, layoutCellId, result.gdsPath);
        else
            LayoutInterchange::exportGds(project, layoutCellId, result.gdsPath);
    } catch (const ProjectError &error) {
        result.error = error.what();
        return result;
    } catch (const std::exception &error) {
        result.error = error.what();
        return result;
    }

    std::filesystem::remove(result.reportPath, status);
    const auto topCell =
        !options.topCell.empty()
            ? options.topCell
            : (options.flatten ? std::string{"TOP"} : LayoutInterchange::gdsStructureName(cell->second.name));
    ProcessOptions processOptions;
    processOptions.workingDirectory = workDirectory;
    processOptions.logPath = result.logPath;
    processOptions.timeoutSeconds = options.timeoutSeconds;

    if (result.engine == "klayout") {
        auto arguments = configured->arguments;
        if (arguments.empty()) {
            arguments = {
                "-b", "-r", "{deck}", "-rd", "input={input}", "-rd", "report={report}", "-rd", "top_cell={top_cell}"};
        }
        bool hasDeck = false;
        bool hasInput = false;
        bool hasReport = false;
        for (const auto &argument : arguments) {
            hasDeck = hasDeck || argument.find("{deck}") != std::string::npos;
            hasInput = hasInput || argument.find("{input}") != std::string::npos;
            hasReport = hasReport || argument.find("{report}") != std::string::npos;
        }
        processOptions.arguments.push_back("klayout");
        if (!hasDeck) {
            processOptions.arguments.push_back("-b");
            processOptions.arguments.push_back("-r");
            processOptions.arguments.push_back(configured->path.string());
        }
        for (const auto &argument : arguments) {
            auto value = replaceAll(argument, "{deck}", configured->path.string());
            value = replaceAll(value, "{input}", result.gdsPath.string());
            value = replaceAll(value, "{report}", result.reportPath.string());
            value = replaceAll(value, "{top_cell}", topCell);
            value = replaceAll(value, "{suite}", options.suite);
            value = replaceAll(value, "{workdir}", workDirectory.string());
            processOptions.arguments.push_back(std::move(value));
        }
        if (!hasInput) {
            processOptions.arguments.push_back("-rd");
            processOptions.arguments.push_back("input=" + result.gdsPath.string());
        }
        if (!hasReport) {
            processOptions.arguments.push_back("-rd");
            processOptions.arguments.push_back("report=" + result.reportPath.string());
        }
    } else {
        GdsCheckOptions check;
        check.input = result.gdsPath;
        check.report = result.reportPath;
        check.process = processName;
        check.topCell = topCell;
        check.suite = options.suite;
        processOptions.arguments.push_back("gdscheck");
        if (configured && !configured->pdkPath.empty()) {
            processOptions.arguments.push_back("--pdk-path");
            processOptions.arguments.push_back(configured->pdkPath);
        }
        for (const auto &argument : gdsCheckArguments(check))
            processOptions.arguments.push_back(argument);
    }
    result.process = runProcess(processOptions, cancellation, onOutput);
    if (!result.process.started || result.process.cancelled || result.process.timedOut || result.process.signal != 0 ||
        result.process.exitCode < 0) {
        std::ostringstream message;
        message << (result.engine == "klayout" ? "klayout" : "gdscheck") << " DRC ";
        if (result.process.cancelled)
            message << "was cancelled";
        else if (result.process.timedOut)
            message << "timed out";
        else if (!result.process.started)
            message << "did not start";
        else if (result.process.signal != 0)
            message << "terminated on signal " << result.process.signal;
        else
            message << "failed";
        if (result.process.exitCode >= 0)
            message << " (exit " << result.process.exitCode << ")";
        if (!result.process.error.empty())
            message << ": " << result.process.error;
        if (!result.process.logTail.empty())
            message << ": " << result.process.logTail;
        result.error = message.str();
        return result;
    }

    if (!std::filesystem::is_regular_file(result.reportPath, status) || status) {
        std::ostringstream message;
        message << (result.engine == "klayout" ? "klayout" : "gdscheck") << " did not produce a report";
        if (result.process.timedOut)
            message << " (timeout)";
        else if (result.process.exitCode >= 0)
            message << " (exit " << result.process.exitCode << ")";
        if (!result.process.error.empty())
            message << ": " << result.process.error;
        if (!result.process.logTail.empty())
            message << ": " << result.process.logTail;
        result.error = message.str();
        return result;
    }
    try {
        result.report = parseGdsCheckLyrdb(result.reportPath);
    } catch (const ProjectError &error) {
        result.error = error.what();
        return result;
    }
    if (result.process.exitCode != 0 && result.report.drc.empty()) {
        std::ostringstream message;
        message << (result.engine == "klayout" ? "klayout" : "gdscheck") << " exited with status "
                << result.process.exitCode << " but produced no DRC violations";
        if (!result.process.logTail.empty())
            message << ": " << result.process.logTail;
        result.error = message.str();
        return result;
    }
    result.ok = true;
    return result;
}

VerificationReport parseGdsCheckLyrdbText(const std::string &text) {
    XmlReader reader(text);
    VerificationReport report;
    std::vector<std::string> stack;
    std::vector<CategoryFrame> categoryFrames;
    std::map<std::string, std::string> categoryDescriptions;
    int categoriesDepth = 0;
    bool inItem = false;
    bool sawReport = false;
    bool sawReportEnd = false;
    bool sawItems = false;
    bool sawItemsEnd = false;

    std::string itemCategory;
    std::string itemCell;
    std::string itemSeverity;
    std::string itemMultiplicity;
    std::string itemValue;
    std::string itemDescription;
    Bounds itemBounds;
    std::vector<std::string> itemMessages;

    const auto finishItem = [&] {
        if (!inItem)
            return;
        DrcViolation violation;
        violation.rule = trim(itemCategory);
        violation.cell = trim(itemCell);
        violation.severity = itemSeverity.empty() ? VerificationSeverity::error : parseSeverity(itemSeverity);
        const auto category = categoryDescriptions.find(violation.rule);
        const auto description =
            category == categoryDescriptions.end() ? trim(itemDescription) : trim(category->second);
        if (!itemSeverity.empty()) {
            violation.severity = parseSeverity(itemSeverity);
        } else {
            const auto combined = lower(description + " " + violation.rule);
            if (combined.find("recommended") != std::string::npos || combined.find("advisory") != std::string::npos ||
                combined.find("warning") != std::string::npos)
                violation.severity = VerificationSeverity::warning;
        }
        std::string message;
        for (const auto &entry : itemMessages) {
            if (entry.empty())
                continue;
            if (!message.empty())
                message += '\n';
            message += entry;
        }
        if (!description.empty())
            message = message.empty() ? description : description + ": " + message;
        if (message.empty())
            message = violation.rule;
        int multiplicity = 1;
        if (!itemMultiplicity.empty()) {
            char *end = nullptr;
            const auto parsed = std::strtol(itemMultiplicity.c_str(), &end, 10);
            if (end != itemMultiplicity.c_str() && parsed > 0)
                multiplicity = static_cast<int>(std::min<long>(parsed, std::numeric_limits<int>::max()));
        }
        if (multiplicity > 1)
            message += " (x" + std::to_string(multiplicity) + ")";
        violation.message = std::move(message);
        if (itemBounds.hasPoint) {
            violation.location.x = toNanometers(itemBounds.minX);
            violation.location.y = toNanometers(itemBounds.minY);
            violation.location.width = std::max(Coordinate{0}, toNanometers(itemBounds.maxX) - violation.location.x);
            violation.location.height = std::max(Coordinate{0}, toNanometers(itemBounds.maxY) - violation.location.y);
        }
        report.drc.push_back(std::move(violation));
        inItem = false;
    };

    while (true) {
        auto event = reader.next();
        if (event.kind == XmlEvent::Kind::endOfFile)
            break;
        if (event.kind == XmlEvent::Kind::start) {
            if (event.name == "report-database")
                sawReport = true;
            else if (event.name == "items")
                sawItems = true;
            else if (event.name == "categories")
                ++categoriesDepth;
            else if (event.name == "category" && categoriesDepth > 0)
                categoryFrames.push_back({});
            else if (event.name == "item") {
                inItem = true;
                itemCategory.clear();
                itemCell.clear();
                itemSeverity.clear();
                itemMultiplicity.clear();
                itemValue.clear();
                itemDescription.clear();
                itemMessages.clear();
                itemBounds = {};
            }
            if (!event.selfClosing)
                stack.push_back(event.name);
            if (event.selfClosing)
                stack.push_back(event.name);
            if (event.selfClosing) {
                // Treat self-closing elements as an immediate end event.
                if (event.name == "report-database")
                    sawReportEnd = true;
                else if (event.name == "items")
                    sawItemsEnd = true;
                if (event.name == "category" && categoriesDepth > 0 && !categoryFrames.empty()) {
                    std::string path;
                    for (const auto &frame : categoryFrames) {
                        if (!path.empty())
                            path += '.';
                        path += trim(frame.name);
                    }
                    if (!path.empty())
                        categoryDescriptions[path] = trim(categoryFrames.back().description);
                    categoryFrames.pop_back();
                } else if (event.name == "categories" && categoriesDepth > 0) {
                    --categoriesDepth;
                } else if (event.name == "item") {
                    finishItem();
                }
                if (!stack.empty())
                    stack.pop_back();
            }
            continue;
        }
        if (event.kind == XmlEvent::Kind::text) {
            if (stack.empty())
                continue;
            const auto &top = stack.back();
            if (categoriesDepth > 0 && !categoryFrames.empty()) {
                if (top == "name")
                    categoryFrames.back().name += event.text;
                else if (top == "description")
                    categoryFrames.back().description += event.text;
            }
            if (inItem) {
                if (top == "category")
                    itemCategory += event.text;
                else if (top == "cell")
                    itemCell += event.text;
                else if (top == "severity")
                    itemSeverity += event.text;
                else if (top == "multiplicity")
                    itemMultiplicity += event.text;
                else if (top == "value")
                    itemValue += event.text;
            }
            continue;
        }
        if (event.kind == XmlEvent::Kind::end) {
            if (event.name == "value" && inItem) {
                const auto value = trim(itemValue);
                itemValue.clear();
                if (!value.empty()) {
                    const auto normalized = lower(value);
                    if (startsWith(normalized, "text:")) {
                        const auto message = textValue(value);
                        if (!message.empty())
                            itemMessages.push_back(message);
                    } else {
                        const auto numbers = numbersIn(value);
                        for (std::size_t index = 0; index + 1 < numbers.size(); index += 2)
                            itemBounds.add(numbers[index], numbers[index + 1]);
                    }
                }
            } else if (event.name == "category" && categoriesDepth > 0 && !categoryFrames.empty()) {
                std::string path;
                for (const auto &frame : categoryFrames) {
                    if (!path.empty())
                        path += ".";
                    path += trim(frame.name);
                }
                if (!path.empty())
                    categoryDescriptions[path] = trim(categoryFrames.back().description);
                categoryFrames.pop_back();
            } else if (event.name == "categories" && categoriesDepth > 0) {
                --categoriesDepth;
            } else if (event.name == "item") {
                finishItem();
            } else if (event.name == "items") {
                sawItemsEnd = true;
            } else if (event.name == "report-database") {
                sawReportEnd = true;
            }
            if (!stack.empty())
                stack.pop_back();
        }
    }
    if (!sawReport || !sawItems)
        throw ProjectError("drc.lyrdb", "The report is not a KLayout report database");
    if (!sawReportEnd || !sawItemsEnd)
        throw ProjectError("drc.lyrdb",
                           "The report database is truncated; expected closing items/report-database tags");
    return report;
}

VerificationReport parseGdsCheckLyrdb(const std::filesystem::path &path) {
    std::ifstream input(path);
    if (!input)
        throw ProjectError("drc.lyrdb", "Unable to open DRC report: " + path.string());
    std::ostringstream text;
    text << input.rdbuf();
    return parseGdsCheckLyrdbText(text.str());
}

} // namespace ice
