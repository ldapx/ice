#pragma once

#include "icelib/core.hpp"
#include "layout_render.hpp"

#include <QString>

#include <filesystem>

namespace ice {

struct LayoutReferencePreview {
    LayoutPreviewModel model;
    QString error;
    QString warning;
};

// Loads a GDS/OASIS reference through KLayout, flattens it into one cell, and
// returns a render model whose layers are matched to the open project's layer
// definitions. The generated GDS cache lives under the project build directory.
LayoutReferencePreview loadLayoutReferencePreview(const LayoutReference &reference, const Project &styleProject,
                                                  const std::string &pdk, const std::string &stack,
                                                  const std::filesystem::path &projectRoot,
                                                  const std::filesystem::path &cacheRoot);

} // namespace ice
