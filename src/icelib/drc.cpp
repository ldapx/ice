#include "icelib/drc.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
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

} // namespace

std::string gdsCheckProcessForPdk(const std::string &pdk) {
    std::string compact;
    for (const unsigned char character : pdk) {
        if (std::isalnum(character) != 0)
            compact.push_back(static_cast<char>(std::tolower(character)));
    }
    if (compact.empty() || compact == std::string(genericPdkId))
        return {};
    if (compact.find("ihpsg13g2") != std::string::npos)
        return "ihp-sg13g2";
    if (compact.find("ihpsg13cmos5l") != std::string::npos)
        return "ihp-sg13cmos5l";
    if (compact.find("gf180mcua") != std::string::npos)
        return "gf180mcuA";
    if (compact.find("gf180mcud") != std::string::npos)
        return "gf180mcuD";
    if (compact.find("gf180mcu") != std::string::npos)
        return "gf180mcuA";
    if (compact.find("icsprout55") != std::string::npos)
        return "icsprout55";
    // Unknown PDKs may still be valid gdscheck process names. Preserve the
    // original identifier rather than rejecting it here.
    return pdk;
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

VerificationReport parseGdsCheckLyrdbText(const std::string &text) {
    XmlReader reader(text);
    VerificationReport report;
    std::vector<std::string> stack;
    std::vector<CategoryFrame> categoryFrames;
    std::map<std::string, std::string> categoryDescriptions;
    int categoriesDepth = 0;
    bool inItem = false;
    bool sawReport = false;
    bool sawItems = false;

    std::string itemCategory;
    std::string itemCell;
    std::string itemSeverity;
    std::string itemMultiplicity;
    std::string itemValue;
    std::string itemText;
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
                message += "\n";
            message += entry;
        }
        if (!description.empty())
            message = message.empty() ? description : description + ": " + message;
        if (message.empty())
            message = violation.rule;
        const auto multiplicity = itemMultiplicity.empty() ? 1 : std::max(1, std::atoi(itemMultiplicity.c_str()));
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
                if (event.name == "category" && categoriesDepth > 0 && !categoryFrames.empty()) {
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
            }
            if (!stack.empty())
                stack.pop_back();
        }
    }
    if (!sawReport || !sawItems)
        throw ProjectError("drc.lyrdb", "The report is not a KLayout report database");
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
