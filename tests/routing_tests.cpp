#include "icelib/routing.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace {
constexpr double epsilon = 1.0e-6;

bool samePoint(const ice::RoutePoint &first, const ice::RoutePoint &second) {
    return std::abs(first.x - second.x) <= epsilon && std::abs(first.y - second.y) <= epsilon;
}

int turnCount(const std::vector<ice::RoutePoint> &points) {
    int turns = 0;
    int previousDirection = -1;
    for (std::size_t index = 1; index < points.size(); ++index) {
        const double dx = points[index].x - points[index - 1].x;
        const double dy = points[index].y - points[index - 1].y;
        if (std::abs(dx) <= epsilon && std::abs(dy) <= epsilon)
            continue;
        int direction = 0;
        if (std::abs(dx) >= std::abs(dy))
            direction = dx < 0.0 ? 0 : 1;
        else
            direction = dy < 0.0 ? 2 : 3;
        if (previousDirection >= 0 && direction != previousDirection)
            ++turns;
        previousDirection = direction;
    }
    return turns;
}

bool orthogonal(const std::vector<ice::RoutePoint> &points) {
    for (std::size_t index = 1; index < points.size(); ++index)
        if (std::abs(points[index].x - points[index - 1].x) > epsilon &&
            std::abs(points[index].y - points[index - 1].y) > epsilon)
            return false;
    return true;
}

bool segmentCrossesRect(const ice::RoutePoint &first, const ice::RoutePoint &second, const ice::RouteRect &rect) {
    const double left = std::min(rect.left, rect.right);
    const double right = std::max(rect.left, rect.right);
    const double top = std::min(rect.top, rect.bottom);
    const double bottom = std::max(rect.top, rect.bottom);
    if (std::abs(first.x - second.x) <= epsilon) {
        const double x = (first.x + second.x) * 0.5;
        if (x <= left + epsilon || x >= right - epsilon)
            return false;
        return std::min(std::max(first.y, second.y), bottom) - std::max(std::min(first.y, second.y), top) > epsilon;
    }
    if (std::abs(first.y - second.y) <= epsilon) {
        const double y = (first.y + second.y) * 0.5;
        if (y <= top + epsilon || y >= bottom - epsilon)
            return false;
        return std::min(std::max(first.x, second.x), right) - std::max(std::min(first.x, second.x), left) > epsilon;
    }
    return true;
}

bool pathCrossesRect(const std::vector<ice::RoutePoint> &points, const ice::RouteRect &rect) {
    for (std::size_t index = 1; index < points.size(); ++index)
        if (segmentCrossesRect(points[index - 1], points[index], rect))
            return true;
    return false;
}

double cross(const ice::RoutePoint &first, const ice::RoutePoint &second, const ice::RoutePoint &third) {
    return (second.x - first.x) * (third.y - first.y) - (second.y - first.y) * (third.x - first.x);
}

bool segmentsIntersect(const ice::RoutePoint &a, const ice::RoutePoint &b, const ice::RoutePoint &c,
                       const ice::RoutePoint &d) {
    const double first = cross(a, b, c);
    const double second = cross(a, b, d);
    const double third = cross(c, d, a);
    const double fourth = cross(c, d, b);
    if (((first > epsilon && second < -epsilon) || (first < -epsilon && second > epsilon)) &&
        ((third > epsilon && fourth < -epsilon) || (third < -epsilon && fourth > epsilon)))
        return true;
    const auto onSegment = [](const ice::RoutePoint &start, const ice::RoutePoint &end, const ice::RoutePoint &point) {
        return point.x >= std::min(start.x, end.x) - epsilon && point.x <= std::max(start.x, end.x) + epsilon &&
               point.y >= std::min(start.y, end.y) - epsilon && point.y <= std::max(start.y, end.y) + epsilon;
    };
    if (std::abs(first) <= epsilon && onSegment(a, b, c))
        return true;
    if (std::abs(second) <= epsilon && onSegment(a, b, d))
        return true;
    if (std::abs(third) <= epsilon && onSegment(c, d, a))
        return true;
    return std::abs(fourth) <= epsilon && onSegment(c, d, b);
}

bool pathCrossesSegment(const std::vector<ice::RoutePoint> &points, const ice::RouteSegment &segment) {
    for (std::size_t index = 1; index < points.size(); ++index)
        if (segmentsIntersect(points[index - 1], points[index], segment.start, segment.end))
            return true;
    return false;
}

bool pathCrossesStartExtension(const std::vector<ice::RoutePoint> &points, const ice::RoutePoint &pin,
                               const ice::RoutePoint &exit) {
    // The first segment is the extension itself. Every later segment must stay
    // clear of its interior; touching the exit endpoint is expected.
    for (std::size_t index = 2; index < points.size(); ++index) {
        const auto &first = points[index - 1];
        const auto &second = points[index];
        if (std::abs(pin.y - exit.y) <= epsilon) {
            const double y = (pin.y + exit.y) * 0.5;
            const double left = std::min(pin.x, exit.x);
            const double right = std::max(pin.x, exit.x);
            if (std::abs(first.y - y) <= epsilon && std::abs(second.y - y) <= epsilon) {
                const double overlap =
                    std::min(std::max(first.x, second.x), right) - std::max(std::min(first.x, second.x), left);
                if (overlap > epsilon)
                    return true;
            } else if (std::abs(first.x - second.x) <= epsilon && first.x > left + epsilon &&
                       first.x < right - epsilon) {
                const double low = std::min(first.y, second.y);
                const double high = std::max(first.y, second.y);
                if (low < y - epsilon && high > y + epsilon)
                    return true;
            }
        } else if (std::abs(pin.x - exit.x) <= epsilon) {
            const double x = (pin.x + exit.x) * 0.5;
            const double top = std::min(pin.y, exit.y);
            const double bottom = std::max(pin.y, exit.y);
            if (std::abs(first.x - x) <= epsilon && std::abs(second.x - x) <= epsilon) {
                const double overlap =
                    std::min(std::max(first.y, second.y), bottom) - std::max(std::min(first.y, second.y), top);
                if (overlap > epsilon)
                    return true;
            } else if (std::abs(first.y - second.y) <= epsilon && first.y > top + epsilon &&
                       first.y < bottom - epsilon) {
                const double low = std::min(first.x, second.x);
                const double high = std::max(first.x, second.x);
                if (low < x - epsilon && high > x + epsilon)
                    return true;
            }
        }
    }
    return false;
}

ice::SchematicRouteRequest horizontalRequest() {
    ice::SchematicRouteRequest request;
    request.start = {0.0, 0.0};
    request.startExit = {30.0, 0.0};
    request.target = {100.0, 0.0};
    request.targetExit = {70.0, 0.0};
    return request;
}
} // namespace

TEST_CASE("schematic routing preserves pin extensions and uses right-angle segments") {
    auto request = horizontalRequest();
    request.start = {0.0, 0.0};
    request.startExit = {-30.0, 0.0};
    request.target = {100.0, 0.0};
    request.targetExit = {70.0, 0.0};

    const auto result = ice::routeSchematicWire(request);
    CHECK(result.routed);
    CHECK(result.points.size() >= 4);
    CHECK(samePoint(result.points.front(), request.start));
    CHECK(samePoint(result.points[1], request.startExit));
    CHECK(samePoint(result.points[result.points.size() - 2], request.targetExit));
    CHECK(samePoint(result.points.back(), request.target));
    CHECK(orthogonal(result.points));
    // The route leaves the start extension outward and enters the target
    // extension from the side opposite its outward direction.
    CHECK(result.points[2].x <= request.startExit.x + epsilon);
    CHECK(result.points[result.points.size() - 3].x <= request.targetExit.x + epsilon);
}

TEST_CASE("schematic routing uses a straight line when the extensions face each other") {
    const auto request = horizontalRequest();
    const auto result = ice::routeSchematicWire(request);
    CHECK(result.routed);
    CHECK(orthogonal(result.points));
    CHECK(turnCount(result.points) == 0);
    CHECK(samePoint(result.points.front(), request.start));
    CHECK(samePoint(result.points[1], request.startExit));
    CHECK(samePoint(result.points[result.points.size() - 2], request.targetExit));
    CHECK(samePoint(result.points.back(), request.target));
}

TEST_CASE("schematic routing avoids rectangular symbol bodies") {
    auto request = horizontalRequest();
    request.obstacleRects.push_back({40.0, -10.0, 60.0, 10.0});

    const auto result = ice::routeSchematicWire(request);
    CHECK(result.routed);
    CHECK(orthogonal(result.points));
    CHECK(!pathCrossesRect(result.points, request.obstacleRects.front()));
    // A four-turn path is required because the connection must leave the
    // start extension to the right and enter the target extension from the
    // left while the obstacle blocks the direct line.
    CHECK(turnCount(result.points) == 4);
}

TEST_CASE("schematic routing treats symbol extension lines as obstacles") {
    auto request = horizontalRequest();
    request.obstacleSegments.push_back({{50.0, -50.0}, {50.0, 50.0}});

    const auto result = ice::routeSchematicWire(request);
    CHECK(result.routed);
    CHECK(orthogonal(result.points));
    CHECK(!pathCrossesSegment(result.points, request.obstacleSegments.front()));
}

TEST_CASE("schematic routing falls back when the start extension is enclosed") {
    ice::SchematicRouteRequest request;
    request.start = {0.0, 0.0};
    request.startExit = {0.0, 0.0};
    request.target = {100.0, 0.0};
    request.targetExit = {70.0, 0.0};
    request.obstacleRects = {
        {-20.0, -20.0, -10.0, 20.0}, {10.0, -20.0, 20.0, 20.0}, {-20.0, 10.0, 20.0, 20.0}, {-20.0, -20.0, 20.0, -10.0}};

    const auto result = ice::routeSchematicWire(request);
    CHECK(!result.routed);
    CHECK(result.points.size() >= 2);
    CHECK(samePoint(result.points.front(), request.start));
    CHECK(samePoint(result.points.back(), request.target));
}

TEST_CASE("schematic routing does not cross its own start extension") {
    ice::SchematicRouteRequest request;
    request.start = {0.0, 0.0};
    request.startExit = {-30.0, 0.0};
    request.target = {-15.0, 100.0};
    request.targetExit = {-15.0, 70.0};
    // This obstacle blocks the direct way around the start extension, which
    // previously tempted the router to loop over the extension and cross it.
    request.obstacleRects.push_back({-40.0, 10.0, -20.0, 80.0});

    const auto result = ice::routeSchematicWire(request);
    CHECK(result.routed);
    CHECK(orthogonal(result.points));
    CHECK(!pathCrossesStartExtension(result.points, request.start, request.startExit));
}

TEST_CASE("schematic routing does not run along the destination extension to its pin") {
    ice::SchematicRouteRequest request;
    request.start = {84.0, 0.0};
    request.startExit = {54.0, 0.0};
    request.target = {-11.0, 0.0};
    request.targetExit = {-41.0, 0.0};

    const auto result = ice::routeSchematicWire(request);
    CHECK(result.routed);
    CHECK(orthogonal(result.points));
    // The route may pass behind the target pin, but it must not terminate on
    // the pin or overlap the destination's pin-to-exit extension.
    for (std::size_t index = 1; index + 1 < result.points.size(); ++index)
        CHECK(!samePoint(result.points[index], request.target));
}

TEST_CASE("schematic routing keeps an endpoint reachable near a symbol boundary") {
    ice::SchematicRouteRequest request;
    request.start = {0.0, -7.0};
    request.startExit = {0.0, -37.0};
    request.target = {-4.0, 36.0};
    request.targetExit = {-4.0, 66.0};
    request.clearance = 2.0;
    // The source body is only one unit away from the start extension point.
    // With clearance it would contain the endpoint unless the router treats
    // the endpoint as reachable.
    request.obstacleRects = {{-16.465, -15.41, 16.465, 15.41}, {-36.0, -36.0, 28.0, 44.0}};
    request.obstacleSegments = {{{-4.0, -28.0}, {-4.0, -58.0}}};

    const auto result = ice::routeSchematicWire(request);
    CHECK(result.routed);
    CHECK(orthogonal(result.points));
    CHECK(result.points[result.points.size() - 3].y > request.targetExit.y - epsilon);
}

TEST_CASE("schematic routing keeps clear of an extension longer than the pin separation") {
    ice::SchematicRouteRequest request;
    request.start = {0.0, -7.0};
    request.startExit = {0.0, -37.0};
    request.target = {0.0, 28.0};
    request.targetExit = {0.0, 58.0};

    const auto result = ice::routeSchematicWire(request);
    CHECK(result.routed);
    CHECK(orthogonal(result.points));
    // The extensions are longer than the vertical pin separation. The route
    // must not run parallel over the destination extension to get there.
    for (std::size_t index = 1; index + 1 < result.points.size(); ++index) {
        const auto &point = result.points[index];
        const bool alongsideExtension =
            point.y > request.target.y + epsilon && point.y < request.targetExit.y - epsilon;
        if (alongsideExtension)
            CHECK(std::abs(point.x - request.target.x) > 2.0);
    }
    CHECK(result.points[result.points.size() - 3].y > request.targetExit.y - epsilon);
}

TEST_CASE("schematic routing minimizes turns before distance") {
    ice::SchematicRouteRequest request;
    request.start = {-130.0, 0.0};
    request.startExit = {-100.0, 0.0};
    request.target = {100.0, 0.0};
    request.targetExit = {70.0, 0.0};
    // A short zig-zag between these staggered walls needs six turns. Going
    // around both walls needs four turns but travels much farther.
    request.obstacleRects = {{-10.0, -100.0, 10.0, 5.0}, {15.0, -5.0, 35.0, 100.0}};

    const auto result = ice::routeSchematicWire(request);
    CHECK(result.routed);
    CHECK(orthogonal(result.points));
    CHECK(turnCount(result.points) == 4);
    double maxAbsY = 0.0;
    for (const auto &point : result.points)
        maxAbsY = std::max(maxAbsY, std::abs(point.y));
    CHECK(maxAbsY >= 100.0 - epsilon);
    for (const auto &obstacle : request.obstacleRects)
        CHECK(!pathCrossesRect(result.points, obstacle));
}

TEST_CASE("schematic routing works for vertical pin directions") {
    ice::SchematicRouteRequest request;
    request.start = {0.0, 0.0};
    request.startExit = {0.0, -30.0};
    request.target = {200.0, 100.0};
    request.targetExit = {200.0, 70.0};

    const auto result = ice::routeSchematicWire(request);
    CHECK(result.routed);
    CHECK(orthogonal(result.points));
    CHECK(result.points.size() >= 4);
    CHECK(samePoint(result.points.front(), request.start));
    CHECK(samePoint(result.points[1], request.startExit));
    CHECK(samePoint(result.points[result.points.size() - 2], request.targetExit));
    CHECK(samePoint(result.points.back(), request.target));
    CHECK(result.points[2].y <= request.startExit.y + epsilon);
    CHECK(result.points[result.points.size() - 3].y <= request.targetExit.y + epsilon);
}

TEST_CASE("schematic routing rejects invalid coordinates without crashing") {
    ice::SchematicRouteRequest request = horizontalRequest();
    request.start.x = std::numeric_limits<double>::quiet_NaN();
    const auto invalid = ice::routeSchematicWire(request);
    CHECK(invalid.points.empty());
    CHECK(!invalid.routed);

    auto inverted = horizontalRequest();
    inverted.clearance = -5.0;
    inverted.obstacleRects.push_back({60.0, 10.0, 40.0, -10.0});
    inverted.obstacleSegments.push_back({{std::numeric_limits<double>::infinity(), 0.0}, {50.0, 50.0}});
    try {
        (void)ice::routeSchematicWire(inverted);
    } catch (...) {
        CHECK(false);
    }
}
