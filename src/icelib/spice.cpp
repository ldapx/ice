#include "icelib/spice.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>

namespace ice {
namespace {

std::string trimCopy(std::string text) {
    const auto isSpace = [](unsigned char character) { return std::isspace(character) != 0; };
    while (!text.empty() && isSpace(static_cast<unsigned char>(text.front())))
        text.erase(text.begin());
    while (!text.empty() && isSpace(static_cast<unsigned char>(text.back())))
        text.pop_back();
    return text;
}

std::string lowerCopy(std::string text) {
    for (auto &character : text)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return text;
}

std::string stripInlineComment(const std::string &line) {
    bool quoted = false;
    int bracketDepth = 0;
    int braceDepth = 0;
    int parenDepth = 0;
    for (std::size_t index = 0; index < line.size(); ++index) {
        const auto character = line[index];
        if (character == '"' && (index == 0 || line[index - 1] != '\\')) {
            quoted = !quoted;
            continue;
        }
        if (quoted)
            continue;
        if (character == '[')
            ++bracketDepth;
        else if (character == ']' && bracketDepth > 0)
            --bracketDepth;
        else if (character == '{')
            ++braceDepth;
        else if (character == '}' && braceDepth > 0)
            --braceDepth;
        else if (character == '(')
            ++parenDepth;
        else if (character == ')' && parenDepth > 0)
            --parenDepth;
        else if (character == ';' && bracketDepth == 0 && braceDepth == 0 && parenDepth == 0)
            return line.substr(0, index);
    }
    return line;
}

struct LogicalLine {
    int line = 0;
    std::string text;
};

std::vector<LogicalLine> logicalLines(const std::string &source) {
    std::vector<LogicalLine> result;
    std::istringstream stream(source);
    std::string physical;
    int lineNumber = 0;
    while (std::getline(stream, physical)) {
        ++lineNumber;
        if (!physical.empty() && physical.back() == '\r')
            physical.pop_back();
        const auto trimmed = trimCopy(stripInlineComment(physical));
        if (trimmed.empty() || trimmed.front() == '*' || trimmed.front() == ';')
            continue;
        if (trimmed.front() == '+') {
            if (!result.empty())
                result.back().text += " " + trimCopy(trimmed.substr(1));
            continue;
        }
        result.push_back({lineNumber, trimmed});
    }
    return result;
}

std::vector<std::string> splitTokens(const std::string &line) {
    std::vector<std::string> tokens;
    std::string current;
    int bracketDepth = 0;
    int braceDepth = 0;
    int parenDepth = 0;
    bool quoted = false;
    const auto flush = [&] {
        auto token = trimCopy(current);
        while (!token.empty() && (token.front() == ',' || token.front() == ';')) {
            token.erase(token.begin());
            token = trimCopy(std::move(token));
        }
        while (!token.empty() && (token.back() == ',' || token.back() == ';'))
            token.pop_back();
        if (!token.empty())
            tokens.push_back(std::move(token));
        current.clear();
    };
    for (std::size_t index = 0; index < line.size(); ++index) {
        const auto character = line[index];
        if (character == '"' && (index == 0 || line[index - 1] != '\\')) {
            quoted = !quoted;
            current.push_back(character);
            continue;
        }
        if (quoted) {
            current.push_back(character);
            continue;
        }
        switch (character) {
        case '[':
            ++bracketDepth;
            break;
        case ']':
            if (bracketDepth > 0)
                --bracketDepth;
            break;
        case '{':
            ++braceDepth;
            break;
        case '}':
            if (braceDepth > 0)
                --braceDepth;
            break;
        case '(':
            ++parenDepth;
            break;
        case ')':
            if (parenDepth > 0)
                --parenDepth;
            break;
        default:
            break;
        }
        if (std::isspace(static_cast<unsigned char>(character)) && bracketDepth == 0 && braceDepth == 0 &&
            parenDepth == 0) {
            flush();
            continue;
        }
        current.push_back(character);
    }
    flush();
    return tokens;
}

bool isAssignmentToken(const std::string &token) {
    bool quoted = false;
    int bracketDepth = 0;
    int braceDepth = 0;
    int parenDepth = 0;
    for (std::size_t index = 0; index < token.size(); ++index) {
        const auto character = token[index];
        if (character == '"' && (index == 0 || token[index - 1] != '\\')) {
            quoted = !quoted;
            continue;
        }
        if (quoted)
            continue;
        switch (character) {
        case '[':
            ++bracketDepth;
            break;
        case ']':
            if (bracketDepth > 0)
                --bracketDepth;
            break;
        case '{':
            ++braceDepth;
            break;
        case '}':
            if (braceDepth > 0)
                --braceDepth;
            break;
        case '(':
            ++parenDepth;
            break;
        case ')':
            if (parenDepth > 0)
                --parenDepth;
            break;
        case '=':
            if (bracketDepth == 0 && braceDepth == 0 && parenDepth == 0)
                return true;
            break;
        default:
            break;
        }
    }
    return false;
}

std::string trimParentheses(std::string text) {
    text = trimCopy(std::move(text));
    while (text.size() >= 2 && text.front() == '(' && text.back() == ')') {
        bool balanced = true;
        int depth = 0;
        for (std::size_t index = 0; index < text.size(); ++index) {
            if (text[index] == '(')
                ++depth;
            else if (text[index] == ')') {
                --depth;
                if (depth == 0 && index + 1 < text.size()) {
                    balanced = false;
                    break;
                }
            }
        }
        if (!balanced)
            break;
        text = trimCopy(text.substr(1, text.size() - 2));
    }
    return text;
}

void parseAssignmentText(const std::string &text, std::map<std::string, std::string> &values,
                         std::vector<SpiceParameter> *ordered = nullptr) {
    std::size_t position = 0;
    const auto skipSeparators = [&] {
        while (position < text.size() &&
               (std::isspace(static_cast<unsigned char>(text[position])) || text[position] == ','))
            ++position;
    };
    while (position < text.size()) {
        skipSeparators();
        if (position >= text.size())
            break;
        if (lowerCopy(text.substr(position, 7)) == "params:") {
            position += 7;
            continue;
        }
        if (text[position] == '(' || text[position] == ')') {
            ++position;
            continue;
        }
        const auto nameStart = position;
        while (position < text.size() && text[position] != '=' && text[position] != ',' &&
               !std::isspace(static_cast<unsigned char>(text[position])))
            ++position;
        const auto name = trimCopy(text.substr(nameStart, position - nameStart));
        if (name.empty()) {
            ++position;
            continue;
        }
        while (position < text.size() && std::isspace(static_cast<unsigned char>(text[position])))
            ++position;
        std::string value;
        if (position < text.size() && text[position] == '=') {
            ++position;
            while (position < text.size() && std::isspace(static_cast<unsigned char>(text[position])))
                ++position;
            const auto valueStart = position;
            int bracketDepth = 0;
            int braceDepth = 0;
            int parenDepth = 0;
            bool quoted = false;
            while (position < text.size()) {
                const auto character = text[position];
                if (character == '"' && (position == 0 || text[position - 1] != '\\')) {
                    quoted = !quoted;
                    ++position;
                    continue;
                }
                if (quoted) {
                    ++position;
                    continue;
                }
                if (character == '[')
                    ++bracketDepth;
                else if (character == ']' && bracketDepth > 0)
                    --bracketDepth;
                else if (character == '{')
                    ++braceDepth;
                else if (character == '}' && braceDepth > 0)
                    --braceDepth;
                else if (character == '(')
                    ++parenDepth;
                else if (character == ')' && parenDepth > 0)
                    --parenDepth;
                else if ((std::isspace(static_cast<unsigned char>(character)) || character == ',') &&
                         bracketDepth == 0 && braceDepth == 0 && parenDepth == 0)
                    break;
                ++position;
            }
            value = trimCopy(text.substr(valueStart, position - valueStart));
        }
        const auto key = lowerCopy(name);
        values[key] = value;
        if (ordered)
            ordered->push_back({name, value});
    }
}

std::string joinTokens(const std::vector<std::string> &tokens, std::size_t start) {
    std::ostringstream output;
    for (std::size_t index = start; index < tokens.size(); ++index) {
        if (index > start)
            output << " ";
        output << tokens[index];
    }
    return output.str();
}

std::size_t firstAssignment(const std::vector<std::string> &tokens, std::size_t start) {
    for (std::size_t index = start; index < tokens.size(); ++index) {
        if (lowerCopy(tokens[index]) == "params:" || isAssignmentToken(tokens[index]))
            return index;
    }
    return tokens.size();
}

std::string unquoteSpiceToken(std::string token) {
    token = trimCopy(std::move(token));
    if (token.size() >= 2 && token.front() == '"' && token.back() == '"')
        token = token.substr(1, token.size() - 2);
    return token;
}

std::string quoteSpiceToken(const std::string &token) {
    if (token.find_first_of(" \t") == std::string::npos)
        return token;
    return "\"" + token + "\"";
}

std::filesystem::path resolveSpiceIncludePath(const std::string &raw, const std::filesystem::path &sourcePath) {
    const auto value = unquoteSpiceToken(raw);
    if (value.empty())
        return {};
    std::filesystem::path requested(value);
    std::error_code status;
    if (requested.is_absolute()) {
        if (std::filesystem::is_regular_file(requested, status) && !status)
            return std::filesystem::weakly_canonical(requested, status);
        return {};
    }

    auto base = sourcePath;
    if (!base.empty()) {
        if (!std::filesystem::is_directory(base, status) || status)
            base = base.parent_path();
        status.clear();
        const auto candidate = base / requested;
        if (std::filesystem::is_regular_file(candidate, status) && !status)
            return std::filesystem::weakly_canonical(candidate, status);
    }
    status.clear();
    const auto candidate = std::filesystem::current_path(status) / requested;
    if (!status && std::filesystem::is_regular_file(candidate, status) && !status)
        return std::filesystem::weakly_canonical(candidate, status);
    return {};
}

std::optional<std::string> readSpiceFile(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return std::nullopt;
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

std::string extractSpiceLibSection(const std::string &source, const std::string &section) {
    const auto wanted = lowerCopy(unquoteSpiceToken(section));
    std::ostringstream output;
    std::istringstream stream(source);
    std::string line;
    bool inSection = false;
    int depth = 0;
    while (std::getline(stream, line)) {
        const auto trimmed = trimCopy(stripInlineComment(line));
        const auto tokens = splitTokens(trimmed);
        if (!tokens.empty()) {
            const auto directive = lowerCopy(tokens.front());
            if (directive == ".lib" && tokens.size() >= 2) {
                if (!inSection) {
                    if (lowerCopy(unquoteSpiceToken(tokens[1])) == wanted) {
                        inSection = true;
                        depth = 1;
                    }
                } else {
                    ++depth;
                    output << line << "\n";
                }
                continue;
            }
            if (directive == ".endl" && inSection) {
                --depth;
                if (depth == 0)
                    break;
                output << line << "\n";
                continue;
            }
        }
        if (inSection)
            output << line << "\n";
    }
    return inSection ? output.str() : std::string{};
}

std::string inlineSpiceSource(const std::string &source, const std::filesystem::path &sourcePath,
                              std::set<std::filesystem::path> &stack, int depth, std::vector<std::string> *warnings) {
    if (depth > 32) {
        if (warnings)
            warnings->push_back("SPICE include nesting exceeds 32 levels; remaining includes were left unresolved");
        return source;
    }
    std::ostringstream output;
    std::istringstream stream(source);
    std::string line;
    while (std::getline(stream, line)) {
        const auto trimmed = trimCopy(stripInlineComment(line));
        const auto tokens = splitTokens(trimmed);
        if (!tokens.empty()) {
            const auto directive = lowerCopy(tokens.front());
            if ((directive == ".include" || directive == ".inc" || directive == ".lib") && tokens.size() >= 2) {
                const auto resolved = resolveSpiceIncludePath(tokens[1], sourcePath);
                if (resolved.empty()) {
                    if (warnings)
                        warnings->push_back("SPICE include file not found: " + unquoteSpiceToken(tokens[1]));
                    output << line << "\n";
                    continue;
                }
                if (stack.contains(resolved)) {
                    if (warnings)
                        warnings->push_back("SPICE include cycle skipped: " + resolved.string());
                    output << line << "\n";
                    continue;
                }
                auto contents = readSpiceFile(resolved);
                if (!contents) {
                    if (warnings)
                        warnings->push_back("Unable to read SPICE include file: " + resolved.string());
                    output << line << "\n";
                    continue;
                }
                auto body = std::move(*contents);
                if (directive == ".lib" && tokens.size() >= 3) {
                    const auto section = unquoteSpiceToken(tokens[2]);
                    auto extracted = extractSpiceLibSection(body, section);
                    if (extracted.empty()) {
                        if (warnings)
                            warnings->push_back("SPICE library section '" + section + "' not found in " +
                                                resolved.string());
                        output << line << "\n";
                        continue;
                    }
                    body = std::move(extracted);
                }
                stack.insert(resolved);
                output << "* ICE include begin: " << resolved.string() << "\n";
                output << inlineSpiceSource(body, resolved, stack, depth + 1, warnings);
                if (!body.empty() && body.back() != '\n')
                    output << "\n";
                output << "* ICE include end: " << resolved.string() << "\n";
                stack.erase(resolved);
                continue;
            }
        }
        output << line << "\n";
    }
    return output.str();
}

std::string rewriteSpiceIncludes(const std::string &source, const std::filesystem::path &sourcePath,
                                 std::vector<std::string> *warnings) {
    std::ostringstream output;
    std::istringstream stream(source);
    std::string line;
    while (std::getline(stream, line)) {
        const auto trimmed = trimCopy(stripInlineComment(line));
        const auto tokens = splitTokens(trimmed);
        if (!tokens.empty()) {
            const auto directive = lowerCopy(tokens.front());
            if ((directive == ".include" || directive == ".inc" || directive == ".lib") && tokens.size() >= 2) {
                const auto resolved = resolveSpiceIncludePath(tokens[1], sourcePath);
                if (!resolved.empty()) {
                    output << tokens.front() << " " << quoteSpiceToken(resolved.string());
                    for (std::size_t index = 2; index < tokens.size(); ++index)
                        output << " " << tokens[index];
                    output << "\n";
                    continue;
                }
                if (warnings)
                    warnings->push_back("SPICE include file not found: " + unquoteSpiceToken(tokens[1]));
            }
        }
        output << line << "\n";
    }
    return output.str();
}

SpiceElement parseElement(const LogicalLine &logical, SpiceDocument &document) {
    SpiceElement element;
    element.line = logical.line;
    element.raw = logical.text;
    const auto tokens = splitTokens(logical.text);
    if (tokens.empty())
        return element;
    element.name = tokens.front();
    element.kind = static_cast<char>(std::tolower(static_cast<unsigned char>(element.name.front())));

    const auto parsedKind = std::string("rclvidmqxegfhbjksw");
    if (parsedKind.find(element.kind) == std::string::npos) {
        document.warnings.push_back("Unsupported SPICE element '" + element.name + "' on line " +
                                    std::to_string(logical.line) + " is preserved but ignored");
        return element;
    }

    const auto assignmentStart = firstAssignment(tokens, 1);
    const auto parameterText = assignmentStart < tokens.size() ? joinTokens(tokens, assignmentStart) : std::string{};
    if (!parameterText.empty())
        parseAssignmentText(parameterText, element.parameters);

    switch (element.kind) {
    case 'r':
    case 'c':
    case 'l': {
        if (tokens.size() < 3) {
            document.warnings.push_back("Malformed SPICE element '" + element.name + "' on line " +
                                        std::to_string(logical.line) + " was skipped");
            element.kind = 0;
            return element;
        }
        element.nodes = {tokens[1], tokens[2]};
        if (assignmentStart > 3)
            element.value = tokens[3];
        break;
    }
    case 'v':
    case 'i':
    case 'b': {
        if (tokens.size() < 3) {
            document.warnings.push_back("Malformed SPICE source '" + element.name + "' on line " +
                                        std::to_string(logical.line) + " was skipped");
            element.kind = 0;
            return element;
        }
        element.nodes = {tokens[1], tokens[2]};
        if (tokens.size() > 3)
            element.value = joinTokens(tokens, 3);
        break;
    }
    case 'd': {
        if (tokens.size() < 4) {
            document.warnings.push_back("Malformed SPICE diode '" + element.name + "' on line " +
                                        std::to_string(logical.line) + " was skipped");
            element.kind = 0;
            return element;
        }
        element.nodes = {tokens[1], tokens[2]};
        element.model = tokens[3];
        break;
    }
    case 'm': {
        if (tokens.size() < 6) {
            document.warnings.push_back("Malformed SPICE MOSFET '" + element.name + "' on line " +
                                        std::to_string(logical.line) + " was skipped");
            element.kind = 0;
            return element;
        }
        element.nodes = {tokens[1], tokens[2], tokens[3], tokens[4]};
        element.model = tokens[5];
        break;
    }
    case 'q': {
        if (assignmentStart < 5) {
            document.warnings.push_back("Malformed SPICE BJT '" + element.name + "' on line " +
                                        std::to_string(logical.line) + " was skipped");
            element.kind = 0;
            return element;
        }
        if (assignmentStart < tokens.size()) {
            for (std::size_t index = 1; index + 1 < assignmentStart; ++index)
                element.nodes.push_back(tokens[index]);
            element.model = tokens[assignmentStart - 1];
        } else {
            for (std::size_t index = 1; index + 1 < tokens.size(); ++index)
                element.nodes.push_back(tokens[index]);
            element.model = tokens.back();
        }
        break;
    }
    case 'j': {
        if (tokens.size() < 5) {
            document.warnings.push_back("Malformed SPICE JFET '" + element.name + "' on line " +
                                        std::to_string(logical.line) + " was skipped");
            element.kind = 0;
            return element;
        }
        element.nodes = {tokens[1], tokens[2], tokens[3]};
        element.model = tokens[4];
        break;
    }
    case 'x': {
        const auto end = assignmentStart;
        if (end < 3) {
            document.warnings.push_back("Malformed subcircuit instance '" + element.name + "' on line " +
                                        std::to_string(logical.line) + " was skipped");
            element.kind = 0;
            return element;
        }
        for (std::size_t index = 1; index + 1 < end; ++index)
            element.nodes.push_back(tokens[index]);
        element.model = tokens[end - 1];
        break;
    }
    case 'e':
    case 'g': {
        if (tokens.size() < 6) {
            document.warnings.push_back("Malformed controlled source '" + element.name + "' on line " +
                                        std::to_string(logical.line) + " was skipped");
            element.kind = 0;
            return element;
        }
        element.nodes = {tokens[1], tokens[2], tokens[3], tokens[4]};
        element.value = joinTokens(tokens, 5);
        break;
    }
    case 'f':
    case 'h': {
        if (tokens.size() < 5) {
            document.warnings.push_back("Malformed controlled source '" + element.name + "' on line " +
                                        std::to_string(logical.line) + " was skipped");
            element.kind = 0;
            return element;
        }
        element.nodes = {tokens[1], tokens[2]};
        element.model = tokens[3];
        element.value = joinTokens(tokens, 4);
        break;
    }
    case 's':
    case 'w': {
        if (tokens.size() < 6) {
            document.warnings.push_back("Malformed SPICE switch '" + element.name + "' on line " +
                                        std::to_string(logical.line) + " was skipped");
            element.kind = 0;
            return element;
        }
        element.nodes = {tokens[1], tokens[2], tokens[3], tokens[4]};
        element.model = tokens[5];
        break;
    }
    default:
        element.value = joinTokens(tokens, 1);
        break;
    }
    return element;
}

} // namespace

SpiceDocument parseSpiceDocument(const std::string &source) {
    SpiceDocument document;
    SpiceSubcircuit *current = nullptr;
    for (const auto &logical : logicalLines(source)) {
        if (logical.text.empty())
            continue;
        if (logical.text.front() != '.') {
            auto element = parseElement(logical, document);
            if (element.kind == 0)
                continue;
            if (current)
                current->elements.push_back(std::move(element));
            else
                document.elements.push_back(std::move(element));
            continue;
        }

        const auto tokens = splitTokens(logical.text);
        if (tokens.empty())
            continue;
        const auto directive = lowerCopy(tokens.front());
        if (directive == ".subckt") {
            if (current) {
                document.warnings.push_back("Nested .subckt on line " + std::to_string(logical.line) +
                                            "; the previous definition was left unterminated");
            }
            if (tokens.size() < 2) {
                document.warnings.push_back(".subckt without a name on line " + std::to_string(logical.line));
                continue;
            }
            SpiceSubcircuit subcircuit;
            subcircuit.name = tokens[1];
            auto parameterStart = tokens.size();
            for (std::size_t index = 2; index < tokens.size(); ++index) {
                if (lowerCopy(tokens[index]) == "params:" || isAssignmentToken(tokens[index])) {
                    parameterStart = index;
                    break;
                }
                subcircuit.ports.push_back(tokens[index]);
            }
            if (parameterStart < tokens.size()) {
                std::map<std::string, std::string> ignored;
                parseAssignmentText(joinTokens(tokens, parameterStart), ignored, &subcircuit.parameters);
            }
            document.subcircuits.push_back(std::move(subcircuit));
            current = &document.subcircuits.back();
            continue;
        }
        if (directive == ".ends") {
            if (current && tokens.size() > 1 && !tokens[1].empty() &&
                lowerCopy(tokens[1]) != lowerCopy(current->name)) {
                document.warnings.push_back(".ends name '" + tokens[1] + "' does not match .subckt '" + current->name +
                                            "' on line " + std::to_string(logical.line));
            }
            if (!current)
                document.warnings.push_back(".ends without an open .subckt on line " + std::to_string(logical.line));
            current = nullptr;
            continue;
        }
        if (directive == ".param") {
            std::vector<SpiceParameter> parsed;
            std::map<std::string, std::string> ignored;
            parseAssignmentText(joinTokens(tokens, 1), ignored, &parsed);
            if (current)
                current->parameters.insert(current->parameters.end(), parsed.begin(), parsed.end());
            else
                document.parameters.insert(document.parameters.end(), parsed.begin(), parsed.end());
            continue;
        }
        if (directive == ".model") {
            if (tokens.size() < 3) {
                document.warnings.push_back("Malformed .model on line " + std::to_string(logical.line));
                continue;
            }
            SpiceModel model;
            model.name = tokens[1];
            model.type = static_cast<char>(std::tolower(static_cast<unsigned char>(tokens[2].front())));
            parseAssignmentText(trimParentheses(joinTokens(tokens, 3)), model.parameters);
            document.models.push_back(std::move(model));
            continue;
        }
        if (directive == ".include" || directive == ".inc" || directive == ".lib") {
            const auto path = joinTokens(tokens, 1);
            if (path.empty())
                document.warnings.push_back(directive + " without a path on line " + std::to_string(logical.line));
            else if (directive == ".lib")
                document.libraries.push_back(path);
            else
                document.includes.push_back(path);
            continue;
        }
        if (directive == ".title") {
            if (!document.title.empty())
                document.title += " ";
            document.title += joinTokens(tokens, 1);
            continue;
        }
        if (directive == ".end")
            break;
        if (directive == ".global" || directive == ".options" || directive == ".option" || directive == ".temp" ||
            directive == ".save" || directive == ".ic" || directive == ".nodeset" || directive == ".width" ||
            directive == ".protect" || directive == ".unprotect" || directive == ".four" || directive == ".plot" ||
            directive == ".print" || directive == ".probe" || directive == ".meas" || directive == ".measure")
            continue;
        if (directive == ".noise" || directive == ".tf" || directive == ".tran" || directive == ".dc" ||
            directive == ".ac" || directive == ".op")
            continue;
        document.warnings.push_back("Unrecognized SPICE directive '" + tokens.front() + "' on line " +
                                    std::to_string(logical.line));
    }
    return document;
}

SpiceDocument parseSpiceDocumentResolved(const std::string &source, const std::filesystem::path &sourcePath,
                                         std::vector<std::string> *warnings) {
    std::set<std::filesystem::path> stack;
    if (!sourcePath.empty()) {
        std::error_code status;
        if (std::filesystem::is_regular_file(sourcePath, status) && !status) {
            const auto canonical = std::filesystem::weakly_canonical(sourcePath, status);
            if (!status)
                stack.insert(canonical);
        }
    }
    return parseSpiceDocument(inlineSpiceSource(source, sourcePath, stack, 0, warnings));
}

std::string rewriteSpiceIncludePaths(const std::string &source, const std::filesystem::path &sourcePath,
                                     std::vector<std::string> *warnings) {
    return rewriteSpiceIncludes(source, sourcePath, warnings);
}

const SpiceSubcircuit *findSpiceSubcircuit(const SpiceDocument &document, const std::string &name) {
    const auto target = lowerCopy(name);
    for (const auto &subcircuit : document.subcircuits) {
        if (lowerCopy(subcircuit.name) == target)
            return &subcircuit;
    }
    return nullptr;
}

const SpiceSubcircuit *preferredSpiceSubcircuit(const SpiceDocument &document, const std::string &preferredName,
                                                std::string *error) {
    if (!preferredName.empty()) {
        if (const auto *found = findSpiceSubcircuit(document, preferredName))
            return found;
    }
    if (document.subcircuits.size() == 1)
        return &document.subcircuits.front();
    if (document.subcircuits.empty()) {
        if (error)
            *error = "SPICE source contains no .subckt definition";
        return nullptr;
    }
    if (error) {
        std::string names;
        for (const auto &subcircuit : document.subcircuits) {
            if (!names.empty())
                names += ", ";
            names += subcircuit.name;
        }
        *error = "SPICE source defines several subcircuits (" + names +
                 "); name the schematic cell after the one it represents";
    }
    return nullptr;
}

std::vector<std::string> expandSpiceBus(const std::string &name) {
    if (name.size() < 4 || name.back() != ']')
        return {name};
    const auto open = name.find_last_of('[');
    if (open == std::string::npos || open == 0 || open + 1 >= name.size())
        return {name};
    const auto range = name.substr(open + 1, name.size() - open - 2);
    const auto colon = range.find(':');
    if (colon == std::string::npos)
        return {name};
    const auto parseIndex = [](const std::string &text) -> std::optional<int> {
        try {
            std::size_t consumed = 0;
            const auto value = std::stoi(text, &consumed);
            if (consumed != text.size())
                return std::nullopt;
            return value;
        } catch (const std::exception &) {
            return std::nullopt;
        }
    };
    const auto left = parseIndex(trimCopy(range.substr(0, colon)));
    const auto right = parseIndex(trimCopy(range.substr(colon + 1)));
    if (!left || !right)
        return {name};
    const auto width = std::abs(*left - *right) + 1;
    if (width <= 1 || width > 65536)
        return {name};
    const auto base = name.substr(0, open);
    std::vector<std::string> bits;
    bits.reserve(static_cast<std::size_t>(width));
    for (int bit = 0; bit < width; ++bit)
        bits.push_back(base + "[" + std::to_string(bit) + "]");
    return bits;
}

std::string spiceElementType(const SpiceElement &element) {
    switch (element.kind) {
    case 'r':
        return "resistor";
    case 'c':
        return "capacitor";
    case 'l':
        return "inductor";
    case 'd':
    case 'm':
    case 'q':
    case 'j':
    case 'x':
        return element.model;
    default:
        return {};
    }
}

std::map<std::string, std::string> spiceElementParameters(const SpiceElement &element) {
    auto parameters = element.parameters;
    const auto value = element.value.empty() ? std::string{} : element.value;
    switch (element.kind) {
    case 'r':
        if (!value.empty()) {
            parameters["resistance"] = value;
            parameters["value"] = value;
        }
        break;
    case 'c':
        if (!value.empty()) {
            parameters["capacitance"] = value;
            parameters["value"] = value;
        }
        break;
    case 'l':
        if (!value.empty()) {
            parameters["inductance"] = value;
            parameters["value"] = value;
        }
        break;
    default:
        break;
    }
    return parameters;
}

SpiceInterface spiceInterfaceForText(const std::string &source, const std::string &preferredName,
                                     const std::filesystem::path &sourcePath) {
    SpiceInterface interface;
    std::vector<std::string> includeWarnings;
    auto document = sourcePath.empty() ? parseSpiceDocument(source)
                                       : parseSpiceDocumentResolved(source, sourcePath, &includeWarnings);
    interface.warnings = std::move(includeWarnings);
    interface.warnings.insert(interface.warnings.end(), document.warnings.begin(), document.warnings.end());
    std::string error;
    const auto *subcircuit = preferredSpiceSubcircuit(document, preferredName, &error);
    if (!subcircuit) {
        interface.error = std::move(error);
        return interface;
    }
    interface.subcircuitName = subcircuit->name;
    interface.declaredPorts = subcircuit->ports;
    for (const auto &port : subcircuit->ports) {
        for (auto &bit : expandSpiceBus(port))
            interface.ports.push_back(std::move(bit));
    }
    interface.parameters = subcircuit->parameters;
    interface.ok = true;
    return interface;
}

SpiceInterface spiceInterfaceForCell(const SchematicCell &cell) {
    if (!cell.textSchematic)
        return {false, "Schematic cell is not a text SPICE cell", {}, {}, {}, {}, {}};
    return spiceInterfaceForText(cell.spiceText, cell.name, cell.spiceSourcePath);
}

std::vector<Pin> spiceSubcircuitPins(const SpiceInterface &interface) {
    const auto count = static_cast<int>(interface.ports.size());
    const auto spacing = 40;
    std::vector<Pin> pins;
    pins.reserve(interface.ports.size());
    int row = 0;
    for (const auto &port : interface.ports) {
        pins.push_back({0, port, -70, count * spacing / 2 - row * spacing, PinDirection::left, 1, true});
        ++row;
    }
    return pins;
}

std::vector<std::string> spiceSourceLinesForHierarchy(const std::string &source,
                                                      const std::filesystem::path &sourcePath,
                                                      std::vector<std::string> *warnings) {
    const auto rewritten = sourcePath.empty() ? source : rewriteSpiceIncludes(source, sourcePath, warnings);
    std::vector<std::string> lines;
    for (const auto &logical : logicalLines(rewritten)) {
        if (logical.text.empty() || logical.text.front() != '.') {
            lines.push_back(logical.text);
            continue;
        }
        const auto tokens = splitTokens(logical.text);
        if (tokens.empty())
            continue;
        const auto directive = lowerCopy(tokens.front());
        if (directive == ".end" || directive == ".title" || directive == ".tran" || directive == ".dc" ||
            directive == ".ac" || directive == ".op" || directive == ".noise" || directive == ".tf")
            continue;
        lines.push_back(logical.text);
    }
    return lines;
}

} // namespace ice
