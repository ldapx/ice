#pragma once

#include "openvlsilib/core.hpp"

#include <QString>

inline QString schematicFileExtension(const openvlsi::SchematicCell &cell) {
    if (cell.systemVerilogSchematic)
        return QStringLiteral("sv");
    return cell.textSchematic ? QStringLiteral("sp") : QStringLiteral("ovsch");
}

inline QString schematicFileLabel(const openvlsi::SchematicCell &cell) {
    return QString::fromStdString(cell.name) + QLatin1Char('.') + schematicFileExtension(cell);
}

inline QString layoutFileLabel(const openvlsi::LayoutCell &cell) {
    return QString::fromStdString(cell.name) + QStringLiteral(".ovlay");
}

inline QString proceduralLayoutFileLabel(const openvlsi::ProceduralLayout &layout) {
    return QString::fromStdString(layout.name) + QStringLiteral(".py");
}
