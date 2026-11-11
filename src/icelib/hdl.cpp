#include "icelib/hdl.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

namespace ice {
namespace {

std::string lowerCopy(std::string text) {
    for (auto &character : text)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return text;
}

std::string trimCopy(std::string text) {
    const auto notSpace = [](unsigned char character) { return !std::isspace(character); };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), notSpace));
    text.erase(std::find_if(text.rbegin(), text.rend(), notSpace).base(), text.end());
    return text;
}

bool isIdentifierChar(char character) {
    const auto byte = static_cast<unsigned char>(character);
    return std::isalnum(byte) || character == '_' || character == '$';
}

std::string stripComments(const std::string &source) {
    std::string result;
    result.reserve(source.size());
    bool lineComment = false;
    bool blockComment = false;
    bool stringLiteral = false;
    for (std::size_t index = 0; index < source.size(); ++index) {
        const char character = source[index];
        const char next = index + 1 < source.size() ? source[index + 1] : '\0';
        if (lineComment) {
            if (character == '\n') {
                lineComment = false;
                result.push_back(character);
            } else {
                result.push_back(' ');
            }
            continue;
        }
        if (blockComment) {
            if (character == '*' && next == '/') {
                result += "  ";
                ++index;
                blockComment = false;
            } else {
                result.push_back(character == '\n' ? '\n' : ' ');
            }
            continue;
        }
        if (stringLiteral) {
            result.push_back(character);
            if (character == '\\' && index + 1 < source.size()) {
                result.push_back(source[++index]);
            } else if (character == '"') {
                stringLiteral = false;
            }
            continue;
        }
        if (character == '/' && next == '/') {
            result += "  ";
            ++index;
            lineComment = true;
        } else if (character == '/' && next == '*') {
            result += "  ";
            ++index;
            blockComment = true;
        } else if (character == '"') {
            result.push_back(character);
            stringLiteral = true;
        } else {
            result.push_back(character);
        }
    }
    return result;
}

std::size_t findMatching(const std::string &text, std::size_t open, char openCharacter, char closeCharacter) {
    if (open >= text.size() || text[open] != openCharacter)
        return std::string::npos;
    int depth = 0;
    for (std::size_t index = open; index < text.size(); ++index) {
        if (text[index] == openCharacter) {
            ++depth;
        } else if (text[index] == closeCharacter) {
            --depth;
            if (depth == 0)
                return index;
        }
    }
    return std::string::npos;
}

std::vector<std::string> splitTopLevel(const std::string &text, char separator) {
    std::vector<std::string> result;
    int paren = 0;
    int bracket = 0;
    int brace = 0;
    std::size_t start = 0;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char character = text[index];
        if (character == '(')
            ++paren;
        else if (character == ')')
            --paren;
        else if (character == '[')
            ++bracket;
        else if (character == ']')
            --bracket;
        else if (character == '{')
            ++brace;
        else if (character == '}')
            --brace;
        else if (character == separator && paren == 0 && bracket == 0 && brace == 0) {
            result.push_back(trimCopy(text.substr(start, index - start)));
            start = index + 1;
        }
    }
    result.push_back(trimCopy(text.substr(start)));
    return result;
}

bool containsWord(const std::string &text, std::string_view word) {
    const auto lowered = lowerCopy(text);
    const auto target = lowerCopy(std::string(word));
    std::size_t position = 0;
    while ((position = lowered.find(target, position)) != std::string::npos) {
        const bool left = position == 0 || !isIdentifierChar(lowered[position - 1]);
        const auto after = position + target.size();
        const bool right = after >= lowered.size() || !isIdentifierChar(lowered[after]);
        if (left && right)
            return true;
        position = after;
    }
    return false;
}

bool isTypeKeyword(const std::string &word) {
    static const std::set<std::string> keywords = {
        "input",    "output",   "inout",   "wire",      "reg",    "logic",   "bit",       "byte",
        "shortint", "int",      "longint", "integer",   "time",   "real",    "realtime",  "shortreal",
        "signed",   "unsigned", "var",     "tri",       "tri0",   "tri1",    "wand",      "wor",
        "uwire",    "supply0",  "supply1", "automatic", "static", "const",   "parameter", "localparam",
        "genvar",   "typedef",  "struct",  "union",     "enum",   "packed",  "interface", "modport",
        "clocking", "return",   "default", "virtual",   "class",  "program", "primitive"};
    return keywords.contains(word);
}

std::optional<long long> parseIntegerLiteral(std::string text) {
    text = trimCopy(std::move(text));
    if (text.empty())
        return std::nullopt;
    if (text.front() == '+')
        text.erase(text.begin());
    bool negative = false;
    if (!text.empty() && text.front() == '-') {
        negative = true;
        text.erase(text.begin());
    }
    std::string digits;
    for (const char character : text) {
        if (character == '_')
            continue;
        if (!std::isdigit(static_cast<unsigned char>(character)))
            return std::nullopt;
        digits.push_back(character);
    }
    if (digits.empty())
        return std::nullopt;
    try {
        const auto value = std::stoll(digits);
        return negative ? -value : value;
    } catch (const std::exception &) {
        return std::nullopt;
    }
}

struct WidthResult {
    int width = 1;
    bool known = true;
};

WidthResult widthForSegment(const std::string &segment) {
    WidthResult result;
    int length = 1;
    for (std::size_t index = 0; index < segment.size(); ++index) {
        if (segment[index] != '[')
            continue;
        const auto end = findMatching(segment, index, '[', ']');
        if (end == std::string::npos)
            break;
        const auto content = trimCopy(segment.substr(index + 1, end - index - 1));
        const auto colon = content.find(':');
        if (colon == std::string::npos) {
            length = std::max(length, 1);
        } else {
            const auto left = parseIntegerLiteral(content.substr(0, colon));
            const auto right = parseIntegerLiteral(content.substr(colon + 1));
            if (!left || !right) {
                result.known = false;
            } else {
                const auto width = std::abs(static_cast<long long>(*left - *right)) + 1;
                if (width <= 0 || width > 65536) {
                    result.known = false;
                } else {
                    length *= static_cast<int>(width);
                    if (length > 65536) {
                        length = 65536;
                        result.known = false;
                    }
                }
            }
        }
        index = end;
    }
    result.width = std::max(1, length);
    return result;
}

std::string segmentWithoutDimensions(std::string segment) {
    // Remove attributes and default values before looking for the port name.
    std::string result;
    result.reserve(segment.size());
    int attributeDepth = 0;
    for (std::size_t index = 0; index < segment.size(); ++index) {
        if (segment[index] == '(' && index + 1 < segment.size() && segment[index + 1] == '*') {
            ++attributeDepth;
            ++index;
            continue;
        }
        if (attributeDepth > 0 && segment[index] == '*' && index + 1 < segment.size() && segment[index + 1] == ')') {
            --attributeDepth;
            ++index;
            continue;
        }
        if (attributeDepth > 0)
            continue;
        if (segment[index] == '=') {
            break;
        }
        if (segment[index] == '[') {
            const auto end = findMatching(segment, index, '[', ']');
            if (end != std::string::npos) {
                index = end;
                continue;
            }
        }
        result.push_back(segment[index]);
    }
    return result;
}

std::string portNameForSegment(const std::string &segment) {
    const auto cleaned = segmentWithoutDimensions(segment);
    std::string last;
    std::string current;
    for (const char character : cleaned) {
        if (isIdentifierChar(character)) {
            current.push_back(character);
        } else {
            if (!current.empty()) {
                if (!isTypeKeyword(lowerCopy(current)) && !current.starts_with('\''))
                    last = current;
                current.clear();
            }
        }
    }
    if (!current.empty() && !isTypeKeyword(lowerCopy(current)))
        last = current;
    return last;
}

std::string escapeXml(const std::string &text) {
    std::string result;
    result.reserve(text.size());
    for (const char character : text) {
        switch (character) {
        case '&':
            result += "&amp;";
            break;
        case '<':
            result += "&lt;";
            break;
        case '>':
            result += "&gt;";
            break;
        case '\'':
            result += "&apos;";
            break;
        case '"':
            result += "&quot;";
            break;
        default:
            result.push_back(character);
            break;
        }
    }
    return result;
}

struct RawModule {
    std::string name;
    std::string header;
    std::string body;
};

std::vector<RawModule> rawModules(const std::string &source) {
    std::vector<RawModule> modules;
    std::size_t position = 0;
    while ((position = source.find("module", position)) != std::string::npos) {
        const bool left = position == 0 || !isIdentifierChar(source[position - 1]);
        const auto afterKeyword = position + 6;
        const bool right = afterKeyword >= source.size() || !isIdentifierChar(source[afterKeyword]);
        if (!left || !right) {
            position = afterKeyword;
            continue;
        }
        auto cursor = afterKeyword;
        while (cursor < source.size() && std::isspace(static_cast<unsigned char>(source[cursor])))
            ++cursor;
        std::string name;
        while (cursor < source.size() && isIdentifierChar(source[cursor]))
            name.push_back(source[cursor++]);
        if (name.empty()) {
            position = afterKeyword;
            continue;
        }
        while (cursor < source.size() && std::isspace(static_cast<unsigned char>(source[cursor])))
            ++cursor;
        if (cursor < source.size() && source[cursor] == '#') {
            auto parameterOpen = cursor + 1;
            while (parameterOpen < source.size() && std::isspace(static_cast<unsigned char>(source[parameterOpen])))
                ++parameterOpen;
            if (parameterOpen < source.size() && source[parameterOpen] == '(') {
                const auto parameterEnd = findMatching(source, parameterOpen, '(', ')');
                if (parameterEnd != std::string::npos)
                    cursor = parameterEnd + 1;
            }
        }
        while (cursor < source.size() && std::isspace(static_cast<unsigned char>(source[cursor])))
            ++cursor;
        std::string header;
        if (cursor < source.size() && source[cursor] == '(') {
            const auto headerEnd = findMatching(source, cursor, '(', ')');
            if (headerEnd != std::string::npos) {
                header = source.substr(cursor + 1, headerEnd - cursor - 1);
                cursor = headerEnd + 1;
            }
        }
        const auto semicolon = source.find(';', cursor);
        if (semicolon == std::string::npos) {
            position = cursor;
            continue;
        }
        const auto endModule = source.find("endmodule", semicolon + 1);
        const auto bodyEnd = endModule == std::string::npos ? source.size() : endModule;
        modules.push_back({std::move(name), std::move(header), source.substr(semicolon + 1, bodyEnd - semicolon - 1)});
        position = bodyEnd + (endModule == std::string::npos ? 0 : 9);
    }
    return modules;
}

std::vector<HdlPort> parsePortsFromHeader(const std::string &header, bool &hasDirections) {
    std::vector<HdlPort> ports;
    hasDirections = containsWord(header, "input") || containsWord(header, "output") || containsWord(header, "inout");
    HdlPortDirection direction = HdlPortDirection::input;
    WidthResult inheritedWidth;
    bool sawDirection = false;

    for (const auto &segment : splitTopLevel(header, ',')) {
        if (segment.empty())
            continue;
        const auto lowered = lowerCopy(segment);
        bool segmentHasDirection = false;
        if (containsWord(lowered, "input")) {
            direction = HdlPortDirection::input;
            sawDirection = true;
            segmentHasDirection = true;
        } else if (containsWord(lowered, "output")) {
            direction = HdlPortDirection::output;
            sawDirection = true;
            segmentHasDirection = true;
        } else if (containsWord(lowered, "inout")) {
            direction = HdlPortDirection::inout;
            sawDirection = true;
            segmentHasDirection = true;
        }
        const auto segmentWidth = widthForSegment(segment);
        if (segmentHasDirection || !sawDirection)
            inheritedWidth = segmentWidth;
        const auto name = portNameForSegment(segment);
        if (name.empty())
            continue;
        HdlPort port;
        port.name = name;
        port.direction = direction;
        port.width = segmentHasDirection ? segmentWidth.width : inheritedWidth.width;
        port.widthKnown = segmentHasDirection ? segmentWidth.known : inheritedWidth.known;
        port.real =
            containsWord(lowered, "real") || containsWord(lowered, "realtime") || containsWord(lowered, "shortreal");
        ports.push_back(std::move(port));
    }
    return ports;
}

void applyBodyDeclarations(const std::string &body, std::vector<HdlPort> &ports) {
    for (const auto &statement : splitTopLevel(body, ';')) {
        const auto trimmed = trimCopy(statement);
        const auto lowered = lowerCopy(trimmed);
        std::optional<HdlPortDirection> direction;
        if (lowered.starts_with("input") && (lowered.size() == 5 || !isIdentifierChar(lowered[5])))
            direction = HdlPortDirection::input;
        else if (lowered.starts_with("output") && (lowered.size() == 6 || !isIdentifierChar(lowered[6])))
            direction = HdlPortDirection::output;
        else if (lowered.starts_with("inout") && (lowered.size() == 5 || !isIdentifierChar(lowered[5])))
            direction = HdlPortDirection::inout;
        if (!direction)
            continue;
        const auto afterDirection = trimmed.substr(
            trimmed.find_first_of(" \t\n") == std::string::npos ? trimmed.size() : trimmed.find_first_of(" \t\n"));
        WidthResult inheritedWidth;
        bool hasWidth = false;
        for (const auto &part : splitTopLevel(trimCopy(afterDirection), ',')) {
            if (part.empty())
                continue;
            const auto width = widthForSegment(part);
            if (!hasWidth || width.width > 1 || !width.known) {
                inheritedWidth = width;
                hasWidth = true;
            }
            const auto name = portNameForSegment(part);
            if (name.empty())
                continue;
            auto existing = std::find_if(ports.begin(), ports.end(),
                                         [&](const HdlPort &port) { return lowerCopy(port.name) == lowerCopy(name); });
            const auto loweredPart = lowerCopy(part);
            const bool realPort = containsWord(loweredPart, "real") || containsWord(loweredPart, "realtime") ||
                                  containsWord(loweredPart, "shortreal");
            if (existing != ports.end()) {
                existing->direction = *direction;
                existing->width = inheritedWidth.width;
                existing->widthKnown = inheritedWidth.known;
                existing->real = realPort;
            } else {
                ports.push_back({name, *direction, inheritedWidth.width, inheritedWidth.known, realPort});
            }
        }
    }
}

HdlParseResult parseRawModule(const RawModule &raw, const std::string &source) {
    HdlParseResult result;
    result.module.name = raw.name;
    bool hasDirections = false;
    result.module.ports = parsePortsFromHeader(raw.header, hasDirections);
    if (!hasDirections || result.module.ports.empty())
        applyBodyDeclarations(raw.body, result.module.ports);
    if (result.module.ports.empty()) {
        result.error = "module '" + raw.name + "' has no ports";
        return result;
    }
    std::set<std::string> names;
    for (const auto &port : result.module.ports) {
        if (!names.insert(lowerCopy(port.name)).second) {
            result.error = "module '" + raw.name + "' declares port '" + port.name + "' more than once";
            return result;
        }
        if (port.name.empty()) {
            result.error = "module '" + raw.name + "' has an unnamed port";
            return result;
        }
    }
    result.ok = true;
    (void)source;
    return result;
}

} // namespace

std::string systemVerilogIdentifier(const std::string &text) {
    std::string result;
    result.reserve(text.size());
    for (const char character : text) {
        const auto byte = static_cast<unsigned char>(character);
        if (std::isalnum(byte) || character == '_') {
            result.push_back(character);
        } else if (!result.empty() && result.back() != '_') {
            result.push_back('_');
        }
    }
    while (!result.empty() && result.back() == '_')
        result.pop_back();
    if (result.empty())
        result = "digital_block";
    if (std::isdigit(static_cast<unsigned char>(result.front())))
        result.insert(result.begin(), '_');
    return result;
}

HdlParseResult parseSystemVerilogModule(const std::string &source, const std::string &preferredModule) {
    HdlParseResult result;
    const auto cleaned = stripComments(source);
    const auto raw = rawModules(cleaned);
    if (raw.empty()) {
        result.error = "SystemVerilog source contains no module declaration";
        return result;
    }
    std::vector<HdlParseResult> parsed;
    parsed.reserve(raw.size());
    for (const auto &module : raw) {
        auto moduleResult = parseRawModule(module, cleaned);
        if (!moduleResult.ok) {
            result.warnings.push_back("Skipping module '" + module.name + "': " + moduleResult.error);
            continue;
        }
        parsed.push_back(std::move(moduleResult));
    }
    if (parsed.empty()) {
        result.error = "SystemVerilog source has no module with usable ports";
        return result;
    }
    if (!preferredModule.empty()) {
        const auto preferred = systemVerilogIdentifier(preferredModule);
        for (auto &module : parsed) {
            if (lowerCopy(module.module.name) == lowerCopy(preferred)) {
                result = std::move(module);
                return result;
            }
        }
    }
    if (parsed.size() == 1) {
        result = std::move(parsed.front());
        return result;
    }
    std::sort(parsed.begin(), parsed.end(), [&](const HdlParseResult &left, const HdlParseResult &right) {
        const bool leftTb = lowerCopy(left.module.name).ends_with("_tb") ||
                            lowerCopy(left.module.name).ends_with("_testbench") || lowerCopy(left.module.name) == "tb";
        const bool rightTb = lowerCopy(right.module.name).ends_with("_tb") ||
                             lowerCopy(right.module.name).ends_with("_testbench") ||
                             lowerCopy(right.module.name) == "tb";
        if (leftTb != rightTb)
            return !leftTb;
        if (left.module.ports.size() != right.module.ports.size())
            return left.module.ports.size() > right.module.ports.size();
        return lowerCopy(left.module.name) < lowerCopy(right.module.name);
    });
    result = std::move(parsed.front());
    if (parsed.size() > 1)
        result.warnings.push_back("Source contains several modules; using '" + result.module.name + "'");
    return result;
}

HdlParseResult parseSystemVerilogModule(const SchematicCell &cell) {
    if (!cell.systemVerilogSchematic)
        return {false, {}, {}, "Schematic cell is not a SystemVerilog source cell"};
    return parseSystemVerilogModule(cell.systemVerilogText, cell.name);
}

HdlSymbolTemplate makeHdlSymbolTemplate(const HdlModule &module, bool flattenBuses) {
    HdlModule flattened;
    if (flattenBuses) {
        flattened.name = module.name;
        for (const auto &port : module.ports) {
            if (port.width > 1 && port.widthKnown && !port.real) {
                for (int bit = 0; bit < port.width; ++bit)
                    flattened.ports.push_back(
                        {port.name + "[" + std::to_string(bit) + "]", port.direction, 1, true, false});
            } else {
                flattened.ports.push_back(port);
            }
        }
    }
    const auto &symbolModule = flattenBuses ? flattened : module;

    std::vector<HdlPort> inputs;
    std::vector<HdlPort> outputs;
    for (const auto &port : symbolModule.ports) {
        if (port.direction == HdlPortDirection::input)
            inputs.push_back(port);
        else
            outputs.push_back(port);
    }
    const int rows = static_cast<int>(std::max(inputs.size(), outputs.size()));
    const int width = 240;
    const int height = std::max(100, 60 + 30 * std::max(0, rows - 1));
    const int boxLeft = 30;
    const int boxRight = width - 30;
    const int boxTop = 20;
    const int boxBottom = height - 20;
    const int inputPinX = 10;
    const int outputPinX = width - 10;
    const double centerX = width / 2.0;
    const double centerY = height / 2.0;
    const auto rowY = [&](std::size_t index, std::size_t count) {
        if (count <= 1)
            return centerY;
        const auto spacing = (boxBottom - boxTop) / static_cast<double>(count + 1);
        return boxTop + spacing * static_cast<double>(index + 1);
    };

    std::ostringstream svg;
    svg << "<svg xmlns='http://www.w3.org/2000/svg' width='" << width << "' height='" << height << "' viewBox='0 0 "
        << width << " " << height << "'>";
    svg << "<rect x='" << boxLeft << "' y='" << boxTop << "' width='" << (boxRight - boxLeft) << "' height='"
        << (boxBottom - boxTop) << "' fill='none' stroke='black' stroke-width='3'/>";
    const auto title = escapeXml(module.name);
    svg << "<text x='" << centerX << "' y='" << (centerY + 5)
        << "' text-anchor='middle' font-family='sans-serif' font-size='12' fill='black'>" << title << "</text>";

    HdlSymbolTemplate symbol;
    const auto portLabel = [](const HdlPort &port) {
        auto label = port.name;
        if (port.width > 1 || !port.widthKnown)
            label += "[" + std::to_string(port.width) + "]";
        return label;
    };
    for (std::size_t index = 0; index < inputs.size(); ++index) {
        const auto y = rowY(index, inputs.size());
        svg << "<path d='M" << boxLeft << " " << y << " L" << inputPinX << " " << y
            << "' fill='none' stroke='black' stroke-width='2'/>";
        svg << "<circle cx='" << inputPinX << "' cy='" << y << "' r='4' fill='black'/>";
        svg << "<text x='" << (boxLeft + 5) << "' y='" << (y + 4)
            << "' text-anchor='start' font-family='sans-serif' font-size='10' fill='black'>"
            << escapeXml(portLabel(inputs[index])) << "</text>";
        Pin pin;
        pin.name = inputs[index].name;
        pin.x = static_cast<Coordinate>(std::lround(inputPinX - centerX));
        pin.y = static_cast<Coordinate>(std::lround(centerY - y));
        pin.direction = PinDirection::left;
        pin.width = inputs[index].width;
        pin.widthKnown = inputs[index].widthKnown;
        symbol.pins.push_back(std::move(pin));
    }
    for (std::size_t index = 0; index < outputs.size(); ++index) {
        const auto y = rowY(index, outputs.size());
        svg << "<path d='M" << boxRight << " " << y << " L" << outputPinX << " " << y
            << "' fill='none' stroke='black' stroke-width='2'/>";
        svg << "<circle cx='" << outputPinX << "' cy='" << y << "' r='4' fill='black'/>";
        svg << "<text x='" << (boxRight - 5) << "' y='" << (y + 4)
            << "' text-anchor='end' font-family='sans-serif' font-size='10' fill='black'>"
            << escapeXml(portLabel(outputs[index])) << "</text>";
        Pin pin;
        pin.name = outputs[index].name;
        pin.x = static_cast<Coordinate>(std::lround(outputPinX - centerX));
        pin.y = static_cast<Coordinate>(std::lround(centerY - y));
        pin.direction = outputs[index].direction == HdlPortDirection::output ? PinDirection::right : PinDirection::left;
        symbol.pins.push_back(std::move(pin));
    }
    svg << "</svg>";
    symbol.artwork = svg.str();
    symbol.labelY = static_cast<Coordinate>(height) / 2 + 12;
    return symbol;
}

} // namespace ice
