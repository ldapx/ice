#pragma once

#include "icelib/core.hpp"
#include "symbol_ui.hpp"

#include <QRectF>

SymbolDefinition generatedSchematicSymbol(const ice::SchematicCell &cell);

class QGraphicsItem;
QRectF symbolHighlightRect(QGraphicsItem *symbol);
