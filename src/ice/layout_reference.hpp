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

// Loads a GDSII or OASIS reference with the native interchange reader and
// returns a render model whose layers are matched to the open project's layer
// definitions.
LayoutReferencePreview loadLayoutReferencePreview(const LayoutReference &reference, const Project &styleProject,
                                                  const std::string &pdk, const std::string &stack,
                                                  const std::filesystem::path &projectRoot);

} // namespace ice
