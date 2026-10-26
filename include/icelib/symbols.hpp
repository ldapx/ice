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
    // Optional instance-label anchor in symbol coordinates (origin at the
    // artwork center, positive Y upward).
    std::optional<SymbolLabel> label;
    std::vector<Pin> pins;
    // Optional default parameters the sidecar seeds into every instance placed
    // from this symbol (ids are assigned by the project at placement time).
    std::vector<Parameter> defaultParameters;
};

// Loads a file-based schematic symbol library from a directory containing
// `<name>.toml` sidecars and sibling `<name>.svg` artwork files. Package
// libraries can reference ICE's built-in artwork with `artwork =
// "generic:name.svg"`:
//
// ```toml
// [symbol]
// name = "resistor"
// artwork = "resistor.svg"          # sibling artwork
// # artwork = "generic:resistor.svg" # built-in ICE artwork
// category = "Passive"
// label_x = 0                        # instance label anchor
// label_y = 20
//
// [[pin]]
// name = "1"
// x = -16
// y = 0
// direction = "inout"
//
// [[parameter]]
// name = "resistance"
// value = "1k"
// ```
//
// Optional `[[parameter]]` entries seed default instance parameters whenever a
// symbol is placed; users can edit them later in the properties panel.
//
// Pin coordinates are integers in symbol units with the origin at the artwork
// center (Qt places the artwork centered on the symbol origin). `direction`
// accepts `input`, `output`, or `inout`; the physical pin side is derived
// from the pin position. Optional `label_x` and `label_y` place the centered
// instance label in the same coordinate system; both must be present together.
// Throws ProjectError when a sidecar is malformed,
// its artwork file is missing or unsafe (scripts or external references),
// pin names are empty or duplicated, coordinates are not integers, or the
// library contains no symbols or duplicate symbol names.
class SymbolLibrary {
  public:
    static std::vector<FileSymbol> load(const std::filesystem::path &directory);
    // `genericArtworkDirectory` is the built-in symbols directory used to
    // resolve `generic:<file>.svg` artwork references. Only plain SVG file
    // names are accepted for generic references.
    static std::vector<FileSymbol> load(const std::filesystem::path &directory,
                                        const std::filesystem::path &genericArtworkDirectory);
};

// Derives the physical pin side from a symbol-relative position using the
// same rule as interactive placement: negative/positive x wins over y.
PinDirection pinSideFromPosition(Coordinate x, Coordinate y);

} // namespace ice
