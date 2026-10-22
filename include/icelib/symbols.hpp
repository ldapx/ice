#pragma once

#include "icelib/core.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace ice {

// A schematic symbol loaded from a `<name>.toml` sidecar plus its sibling
// `<name>.svg` artwork file. Pin ids are always 0; callers assign stable ids
// from the owning project (see Project::nextId).
struct FileSymbol {
    std::string name;
    std::string category;
    std::string svg;
    std::vector<Pin> pins;
};

// Loads a file-based schematic symbol library from a directory containing
// `<name>.toml` sidecars and sibling `<name>.svg` artwork files:
//
// ```toml
// [symbol]
// name = "resistor"
// artwork = "resistor.svg"
// category = "Passive"
//
// [[pin]]
// name = "1"
// x = -16
// y = 0
// direction = "inout"
// ```
//
// Pin coordinates are integers in symbol units with the origin at the artwork
// center (Qt places the artwork centered on the symbol origin). `direction`
// accepts `input`, `output`, or `inout`; the physical pin side is derived
// from the pin position. Throws ProjectError when a sidecar is malformed,
// its artwork file is missing or unsafe (scripts or external references),
// pin names are empty or duplicated, coordinates are not integers, or the
// library contains no symbols or duplicate symbol names.
class SymbolLibrary {
  public:
    static std::vector<FileSymbol> load(const std::filesystem::path &directory);
};

// Derives the physical pin side from a symbol-relative position using the
// same rule as interactive placement: negative/positive x wins over y.
PinDirection pinSideFromPosition(Coordinate x, Coordinate y);

} // namespace ice
