#include "layout_reference.hpp"

#include "icelib/interchange.hpp"

#include <algorithm>
#include <cctype>

namespace ice {
namespace {

std::string lowerCopy(std::string text) {
    for (auto &character : text)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return text;
}

} // namespace

LayoutReferencePreview loadLayoutReferencePreview(const LayoutReference &reference, const Project &styleProject,
                                                  const std::string &pdk, const std::string &stack,
                                                  const std::filesystem::path &projectRoot) {
    LayoutReferencePreview result;
    const auto source = std::filesystem::path(reference.path).is_absolute() ? std::filesystem::path(reference.path)
                                                                            : projectRoot / reference.path;
    std::error_code status;
    if (!std::filesystem::is_regular_file(source, status) || status) {
        result.error =
            QStringLiteral("Reference stream does not exist: %1").arg(QString::fromStdString(source.string()));
        return result;
    }

    try {
        LayoutImportOptions options;
        options.topCell = reference.topCell;
        auto project = lowerCopy(reference.format) == "oasis" ? LayoutInterchange::importOasis(source, options)
                                                              : LayoutInterchange::importGds(source, options);
        if (project.layoutCells.empty()) {
            result.error = QStringLiteral("Reference preview contains no layout cells.");
            return result;
        }
        const auto cellId = project.layoutCells.begin()->first;
        const auto &cell = project.layoutCells.at(cellId);
        result.model = buildLayoutCellPreviewModel(project, cell, true, true, false);
        if (result.model.empty()) {
            result.error = QStringLiteral("Reference preview contains no geometry.");
            return result;
        }

        const auto remapLayer = [&](LayoutPreviewLayer &layer) {
            ObjectId matched = 0;
            for (const auto &[id, candidate] : styleProject.layers) {
                if (candidate.number != layer.number || candidate.datatype != layer.datatype)
                    continue;
                if (!pdk.empty() && !candidate.pdk.empty() && candidate.pdk != pdk)
                    continue;
                if (!stack.empty() && !candidate.stack.empty() && candidate.stack != stack)
                    continue;
                matched = id;
                if (lowerCopy(candidate.name) == lowerCopy(layer.name.toStdString()))
                    break;
            }
            if (matched != 0) {
                const auto &candidate = styleProject.layers.at(matched);
                layer.id = matched;
                layer.name = QString::fromStdString(candidate.name);
            }
        };
        for (auto &shape : result.model.shapes)
            remapLayer(shape.layer);
        for (auto &port : result.model.ports)
            remapLayer(port.layer);
        return result;
    } catch (const std::exception &error) {
        result.error = QString::fromUtf8(error.what());
        return result;
    }
}

} // namespace ice
