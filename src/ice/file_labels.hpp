#pragma once

#include "icelib/core.hpp"

#include <QString>

inline QString schematicFileExtension(const ice::SchematicCell &cell) {
    if (cell.systemVerilogSchematic)
        return QStringLiteral("sv");
    return cell.textSchematic ? QStringLiteral("sp") : QStringLiteral("icesch");
}

inline QString schematicFileLabel(const ice::SchematicCell &cell) {
    return QString::fromStdString(cell.name) + QLatin1Char('.') + schematicFileExtension(cell);
}

inline QString layoutFileLabel(const ice::LayoutCell &cell) {
    return QString::fromStdString(cell.name) + QStringLiteral(".icelay");
}

inline QString proceduralLayoutFileLabel(const ice::ProceduralLayout &layout) {
    return QString::fromStdString(layout.name) + QStringLiteral(".py");
}
