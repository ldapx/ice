#pragma once

#include "icelib/geometry.hpp"

#include <QPointF>
#include <QString>

namespace ice {

// Converts text into filled layout polygons in layout coordinates. Glyph
// contours are simplified to 45/90-degree edges before holes are filled and
// split into independently fillable polygons.
MultiPolygon textPolygonGeometry(const QString &text, int pixelSize, QPointF lowerLeft);

} // namespace ice
