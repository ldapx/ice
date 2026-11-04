#include "icelib/extraction.hpp"

#include "icelib/core.hpp"
#include "icelib/pdk.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <set>
#include <sstream>

namespace ice {
namespace {

using json = nlohmann::ordered_json;

std::string readString(const json &object, const char *key, const std::string &fallback = {}) {
    const auto it = object.find(key);
    if (it == object.end() || it->is_null())
        return fallback;
    if (!it->is_string())
        throw ProjectError("extraction.rule", std::string("Rule field must be a string: ") + key);
    return it->get<std::string>();
}

std::vector<std::string> readStringArray(const json &object, const char *key) {
    const auto it = object.find(key);
    if (it == object.end() || it->is_null())
        return {};
    if (!it->is_array())
        throw ProjectError("extraction.rule", std::string("Rule field must be an array: ") + key);
    std::vector<std::string> result;
    for (const auto &entry : *it) {
        if (!entry.is_string())
            throw ProjectError("extraction.rule", std::string("Rule array must contain strings: ") + key);
        result.push_back(entry.get<std::string>());
    }
    return result;
}

double readNumber(const json &object, const char *key, double fallback = 0.0) {
    const auto it = object.find(key);
    if (it == object.end() || it->is_null())
        return fallback;
    if (!it->is_number())
        throw ProjectError("extraction.rule", std::string("Rule field must be a number: ") + key);
    return it->get<double>();
}

DeviceExtractionRule parseRule(const json &entry) {
    if (!entry.is_object())
        throw ProjectError("extraction.rule", "Each extraction device must be an object");
    DeviceExtractionRule rule;
    rule.name = readString(entry, "name");
    rule.type = readString(entry, "type");
    rule.model = readString(entry, "model");
    rule.element = readString(entry, "element");
    rule.active = readString(entry, "active");
    rule.gate = readString(entry, "gate");
    rule.bodyLayer = readString(entry, "body_layer");
    rule.requireLayers = readStringArray(entry, "require_layers");
    rule.excludeLayers = readStringArray(entry, "exclude_layers");
    rule.terminalLayers = readStringArray(entry, "terminal_layers");
    rule.substrate = readString(entry, "substrate", "0");
    rule.lengthAxis = readString(entry, "length_axis", "auto");
    rule.body = readString(entry, "body");
    rule.plates = readStringArray(entry, "plates");
    rule.sheetResistance = readNumber(entry, "sheet_resistance");
    rule.capacitancePerArea = readNumber(entry, "capacitance_per_area");
    rule.value = readString(entry, "value");

    if (rule.name.empty() || rule.type.empty())
        throw ProjectError("extraction.rule", "Extraction rules require name and type");
    if (rule.type == "mos") {
        if (rule.active.empty() || rule.gate.empty() || rule.terminalLayers.empty())
            throw ProjectError("extraction.rule",
                               "MOS extraction rule '" + rule.name + "' requires active, gate, and terminal layers");
        if (rule.active == rule.gate)
            throw ProjectError("extraction.rule",
                               "MOS extraction rule '" + rule.name + "' must use distinct active and gate layers");
        if (rule.element.empty())
            rule.element = "X";
    } else if (rule.type == "resistor") {
        if (rule.body.empty() || rule.sheetResistance <= 0.0 || rule.terminalLayers.empty())
            throw ProjectError("extraction.rule", "Resistor extraction rule '" + rule.name +
                                                      "' requires body, sheet_resistance, and terminal layers");
        if (rule.element.empty())
            rule.element = "R";
    } else if (rule.type == "capacitor") {
        if (rule.plates.size() < 2 || rule.capacitancePerArea <= 0.0)
            throw ProjectError("extraction.rule", "Capacitor extraction rule '" + rule.name +
                                                      "' requires two plates and capacitance_per_area");
        if (rule.element.empty())
            rule.element = "C";
    } else {
        throw ProjectError("extraction.rule", "Unsupported extraction rule type: " + rule.type);
    }
    return rule;
}

} // namespace

std::optional<DeviceExtractionRules> loadDeviceExtractionRules(const std::filesystem::path &path, std::string *error) {
    if (error)
        error->clear();
    std::error_code status;
    if (!std::filesystem::is_regular_file(path, status) || status)
        return std::nullopt;
    try {
        std::ifstream input(path);
        if (!input)
            throw ProjectError("extraction.file", "Unable to open extraction rules: " + path.string());
        std::stringstream buffer;
        buffer << input.rdbuf();
        const auto document = json::parse(buffer.str());
        DeviceExtractionRules rules;
        rules.schemaVersion = document.value("schema_version", 1);
        rules.format = document.value("format", std::string("ice-device-extraction"));
        rules.pdk = document.value("pdk", std::string{});
        if (rules.schemaVersion != 1)
            throw ProjectError("extraction.schema", "Unsupported extraction rule schema version");
        if (rules.format != "ice-device-extraction")
            throw ProjectError("extraction.format", "Unsupported extraction rule format: " + rules.format);
        const auto devices = document.find("devices");
        if (devices == document.end() || !devices->is_array())
            throw ProjectError("extraction.rule", "Extraction rules require a 'devices' array");
        std::set<std::string> names;
        for (const auto &entry : *devices) {
            auto rule = parseRule(entry);
            auto key = rule.name;
            for (auto &character : key)
                character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
            if (!names.insert(key).second)
                throw ProjectError("extraction.rule", "Duplicate extraction rule name: " + rule.name);
            rules.rules.push_back(std::move(rule));
        }
        return rules;
    } catch (const ProjectError &exception) {
        if (error)
            *error = std::string(exception.what()) + " (" + path.string() + ")";
        return std::nullopt;
    } catch (const std::exception &exception) {
        if (error)
            *error = std::string(exception.what()) + " (" + path.string() + ")";
        return std::nullopt;
    }
}

std::optional<DeviceExtractionRules> loadDeviceExtractionRulesForPdk(const std::string &pdkId, std::string *error) {
    if (error)
        error->clear();
    if (pdkId.empty() || pdkId == genericPdkId)
        return std::nullopt;
    const auto pdks = findPdks(pdkId);
    for (const auto &pdk : pdks) {
        const auto path = pdk.packageRoot / "layout" / "extraction.json";
        std::error_code status;
        if (!std::filesystem::is_regular_file(path, status) || status)
            continue;
        if (auto rules = loadDeviceExtractionRules(path, error))
            return rules;
        return std::nullopt;
    }
    return std::nullopt;
}

} // namespace ice
