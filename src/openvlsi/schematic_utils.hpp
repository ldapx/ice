#pragma once

#include "openvlsilib/core.hpp"
#include "symbol_ui.hpp"

#include <QRectF>

SymbolDefinition generatedSchematicSymbol(const openvlsi::SchematicCell &cell);

class QGraphicsItem;
QRectF symbolHighlightRect(QGraphicsItem *symbol);
