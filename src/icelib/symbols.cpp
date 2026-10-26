#include "icelib/symbols.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string_view>

#include <toml++/toml.h>

namespace ice {
namespace {
bool hasInteger(toml::node_view<const toml::node> node, std::int64_t &value) {
    if (node.type() != toml::node_type::integer)
        return false;
    if (auto number = node.value<std::int64_t>())
        value = *number;
    else
        return false;
    return true;
}

std::string readTextFile(const std::filesystem::path &path, const char *code, const std::string &what) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        throw ProjectError(code, what + ": " + path.string());
    std::ostringstream content;
    content << stream.rdbuf();
    if (stream.bad())
        throw ProjectError(code, what + ": " + path.string());
    return content.str();
}

void rejectUnsafeArtwork(const std::string &svg, const std::filesystem::path &path) {
    if (svg.find("<svg") == std::string::npos)
        throw ProjectError("symbol.artwork", "Symbol artwork is not an SVG document: " + path.string());
    if (svg.find("<script") != std::string::npos)
        throw ProjectError("symbol.artwork", "Symbol artwork must not contain scripts: " + path.string());
    // Namespace declarations legitimately contain http(s) URIs, so ignore
    // them when looking for external resource references.
    std::string scrubbed = svg;
    for (std::string::size_type at = scrubbed.find("xmlns"); at != std::string::npos; at = scrubbed.find("xmlns")) {
        const auto quote = scrubbed.find('"', at);
        if (quote == std::string::npos)
            break;
        const auto end = scrubbed.find('"', quote + 1);
        if (end == std::string::npos)
            break;
        scrubbed.erase(at, end - at + 1);
    }
    for (const char *marker : {"http://", "https://"})
        if (scrubbed.find(marker) != std::string::npos)
            throw ProjectError("symbol.artwork",
                               "Symbol artwork must not reference external resources: " + path.string());
}
} // namespace

PinDirection pinSideFromPosition(Coordinate x, Coordinate y) {
    if (x < 0)
        return PinDirection::left;
    if (x > 0)
        return PinDirection::right;
    return y > 0 ? PinDirection::up : PinDirection::down;
}

namespace {
constexpr std::string_view genericArtworkPrefix = "generic:";

std::vector<FileSymbol> loadSymbols(const std::filesystem::path &directory,
                                    const std::filesystem::path &genericArtworkDirectory) {
    std::error_code status;
    if (!std::filesystem::is_directory(directory, status) || status)
        throw ProjectError("symbol.library", "Symbol library directory does not exist: " + directory.string());
    std::vector<std::filesystem::path> sidecars;
    for (const auto &entry : std::filesystem::directory_iterator(directory, status)) {
        if (status)
            throw ProjectError("symbol.library", "Unable to list symbol library: " + directory.string());
        if (entry.is_regular_file() && entry.path().extension() == ".toml")
            sidecars.push_back(entry.path());
    }
    std::sort(sidecars.begin(), sidecars.end());
    if (sidecars.empty())
        throw ProjectError("symbol.library", "Symbol library has no symbol definitions: " + directory.string());
    std::vector<FileSymbol> symbols;
    for (const auto &sidecar : sidecars) {
        FileSymbol symbol;
        std::string artworkFile;
        std::optional<SymbolLabel> label;
        std::vector<Pin> pins;
        std::vector<Parameter> defaultParameters;
        try {
            auto table = toml::parse_file(sidecar.string());
            const auto *header = table["symbol"].as_table();
            if (!header)
                throw ProjectError("symbol.manifest", "Missing [symbol] table: " + sidecar.string());
            symbol.name = (*header)["name"].value_or(std::string{});
            artworkFile = (*header)["artwork"].value_or(std::string{});
            symbol.category = (*header)["category"].value_or(std::string{});
            const auto labelX = (*header)["label_x"];
            const auto labelY = (*header)["label_y"];
            const bool hasLabelX = labelX.type() != toml::node_type::none;
            const bool hasLabelY = labelY.type() != toml::node_type::none;
            if (hasLabelX != hasLabelY)
                throw ProjectError("symbol.label",
                                   "label_x and label_y must be specified together: " + sidecar.string());
            if (hasLabelX) {
                std::int64_t x = 0;
                std::int64_t y = 0;
                if (!hasInteger(labelX, x) || !hasInteger(labelY, y))
                    throw ProjectError("symbol.label", "Label coordinates must be integers: " + sidecar.string());
                label = SymbolLabel{x, y};
            }
            if (symbol.name.empty())
                throw ProjectError("symbol.manifest", "Symbol name is required: " + sidecar.string());
            if (artworkFile.empty())
                throw ProjectError("symbol.manifest", "Symbol artwork is required: " + sidecar.string());
            const auto *pinList = table["pin"].as_array();
            if (!pinList || pinList->empty())
                throw ProjectError("symbol.pin", "Symbol defines no pins: " + sidecar.string());
            for (const auto &item : *pinList) {
                const auto *pinTable = item.as_table();
                if (!pinTable)
                    throw ProjectError("symbol.pin", "Pin entry must be a table: " + sidecar.string());
                Pin pin;
                pin.name = (*pinTable)["name"].value_or(std::string{});
                std::int64_t x = 0;
                std::int64_t y = 0;
                if (!hasInteger((*pinTable)["x"], x) || !hasInteger((*pinTable)["y"], y))
                    throw ProjectError("symbol.pin", "Pin coordinates must be integers: " + sidecar.string());
                const auto direction = (*pinTable)["direction"].value_or(std::string{});
                if (direction != "input" && direction != "output" && direction != "inout")
                    throw ProjectError("symbol.pin",
                                       "Pin direction must be input, output, or inout: " + sidecar.string());
                if (pin.name.empty())
                    throw ProjectError("symbol.pin", "Pin name is required: " + sidecar.string());
                pin.x = x;
                pin.y = y;
                pin.direction = pinSideFromPosition(pin.x, pin.y);
                pins.push_back(std::move(pin));
            }
            const auto *parameterList = table["parameter"].as_array();
            if (parameterList) {
                for (const auto &item : *parameterList) {
                    const auto *parameterTable = item.as_table();
                    if (!parameterTable)
                        throw ProjectError("symbol.parameter", "Parameter entry must be a table: " + sidecar.string());
                    Parameter parameter;
                    parameter.name = (*parameterTable)["name"].value_or(std::string{});
                    parameter.value = (*parameterTable)["value"].value_or(std::string{});
                    if (parameter.name.empty())
                        throw ProjectError("symbol.parameter", "Parameter name is required: " + sidecar.string());
                    for (const auto &existing : defaultParameters)
                        if (existing.name == parameter.name)
                            throw ProjectError("symbol.parameter", "Duplicate parameter name '" + parameter.name +
                                                                       "': " + sidecar.string());
                    defaultParameters.push_back(std::move(parameter));
                }
            }
        } catch (const ProjectError &) {
            throw;
        } catch (const std::exception &error) {
            throw ProjectError("symbol.manifest", sidecar.string() + ": " + error.what());
        }
        for (std::size_t i = 0; i < pins.size(); ++i)
            for (std::size_t j = i + 1; j < pins.size(); ++j)
                if (pins[i].name == pins[j].name)
                    throw ProjectError("symbol.pin", "Duplicate pin name '" + pins[i].name + "': " + sidecar.string());
        std::filesystem::path artworkPath;
        if (artworkFile.starts_with(genericArtworkPrefix)) {
            if (genericArtworkDirectory.empty())
                throw ProjectError("symbol.artwork",
                                   "Generic artwork reference requires a built-in artwork directory: " +
                                       sidecar.string());
            const auto name = artworkFile.substr(genericArtworkPrefix.size());
            const std::filesystem::path reference(name);
            if (name.empty() || reference.is_absolute() || reference != reference.filename() ||
                reference.extension() != ".svg")
                throw ProjectError("symbol.artwork",
                                   "Generic artwork reference must be a plain SVG file name: " + sidecar.string());
            artworkPath = genericArtworkDirectory / reference;
        } else {
            artworkPath = sidecar.parent_path() / artworkFile;
        }
        symbol.svg = readTextFile(artworkPath, "symbol.artwork", "Symbol artwork file is missing");
        rejectUnsafeArtwork(symbol.svg, artworkPath);
        symbol.label = std::move(label);
        symbol.pins = std::move(pins);
        symbol.defaultParameters = std::move(defaultParameters);
        symbols.push_back(std::move(symbol));
    }
    for (std::size_t i = 0; i < symbols.size(); ++i)
        for (std::size_t j = i + 1; j < symbols.size(); ++j)
            if (symbols[i].name == symbols[j].name)
                throw ProjectError("symbol.library", "Duplicate symbol name '" + symbols[i].name + "'");
    return symbols;
}
} // namespace

std::vector<FileSymbol> SymbolLibrary::load(const std::filesystem::path &directory) {
    return loadSymbols(directory, {});
}

std::vector<FileSymbol> SymbolLibrary::load(const std::filesystem::path &directory,
                                            const std::filesystem::path &genericArtworkDirectory) {
    return loadSymbols(directory, genericArtworkDirectory);
}
} // namespace ice
