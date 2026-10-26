#pragma once

#include <vector>

namespace ice {

// The schematic router works on plain double-precision coordinates because the
// Qt canvas projects the integer domain model into scene units. Callers supply
// the pin positions and their precomputed minimum-extension points, so the
// router does not depend on Qt or on a particular coordinate orientation.
struct RoutePoint {
    double x = 0.0;
    double y = 0.0;
    bool operator==(const RoutePoint &) const = default;
};

struct RouteRect {
    double left = 0.0;
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;
};

struct RouteSegment {
    RoutePoint start;
    RoutePoint end;
};

struct SchematicRouteRequest {
    // Pin centers and the outermost points of their minimum-extension lines.
    // The route is only computed between startExit and targetExit; the
    // straight pin-to-exit segments are always part of the returned path.
    RoutePoint start;
    RoutePoint startExit;
    RoutePoint target;
    RoutePoint targetExit;

    // Extra spacing kept around every obstacle. The router also adds a small
    // internal thickness to line obstacles so a route cannot run along them.
    double clearance = 0.0;

    // Symbols are rectangle obstacles. Extension lines are segment obstacles.
    std::vector<RouteRect> obstacleRects;
    std::vector<RouteSegment> obstacleSegments;
};

struct SchematicRouteResult {
    // Ordered path from start through startExit, the routed path, targetExit,
    // and finally target. Empty only when the input coordinates are invalid.
    std::vector<RoutePoint> points;

    // True when an obstacle-free orthogonal route was found. When false the
    // path falls back to a direct line between startExit and targetExit.
    bool routed = false;
};

// Routes a schematic connection between two pin extension points. The route is
// rectilinear (only right-angle turns), avoids every supplied obstacle, and
// minimizes the number of turns before minimizing length. The first routed
// segment leaves startExit along the extension direction and the final routed
// segment enters targetExit from the opposite direction. If no valid route
// exists the result contains the straight pin-extension-pin fallback path.
SchematicRouteResult routeSchematicWire(const SchematicRouteRequest &request);

} // namespace ice
