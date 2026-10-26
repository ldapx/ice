#include "icelib/routing.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <queue>
#include <vector>

namespace ice {
namespace {
constexpr double kEpsilon = 1.0e-6;
constexpr double kLineHalfThickness = 0.5;
// Active extension walls are thicker than the conceptual line so a typical
// schematic wire stroke cannot be drawn visually on top of them.
constexpr double kActiveExtensionHalfThickness = 4.0;
constexpr double kExtensionPinPadding = 2.0;
constexpr double kOuterMargin = 10.0;
constexpr double kMaxCoordinate = 1.0e12;
constexpr std::size_t kMaxGridCells = 300000;

enum Direction : int { left = 0, right = 1, up = 2, down = 3, noDirection = 4 };

struct Obstacle {
    double left = 0.0;
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;
};

struct Cost {
    int turns = std::numeric_limits<int>::max();
    double length = std::numeric_limits<double>::infinity();
};

struct QueueEntry {
    int turns = 0;
    double length = 0.0;
    int node = 0;
    int direction = noDirection;
};

struct Later {
    bool operator()(const QueueEntry &first, const QueueEntry &second) const {
        if (first.turns != second.turns)
            return first.turns > second.turns;
        if (first.length != second.length)
            return first.length > second.length;
        return first.node > second.node;
    }
};

bool finite(double value) { return std::isfinite(value); }

bool bounded(double value) { return finite(value) && std::abs(value) <= kMaxCoordinate; }

bool finitePoint(const RoutePoint &point) { return bounded(point.x) && bounded(point.y); }

bool samePoint(const RoutePoint &first, const RoutePoint &second) {
    return std::abs(first.x - second.x) <= kEpsilon && std::abs(first.y - second.y) <= kEpsilon;
}

bool axisAligned(const RoutePoint &first, const RoutePoint &second) {
    return std::abs(first.x - second.x) <= kEpsilon || std::abs(first.y - second.y) <= kEpsilon;
}

int directionOf(const RoutePoint &from, const RoutePoint &to) {
    const double dx = to.x - from.x;
    const double dy = to.y - from.y;
    if (std::abs(dx) <= kEpsilon && std::abs(dy) <= kEpsilon)
        return noDirection;
    if (std::abs(dx) >= std::abs(dy))
        return dx < 0.0 ? left : right;
    return dy < 0.0 ? up : down;
}

int opposite(int direction) {
    switch (direction) {
    case left:
        return right;
    case right:
        return left;
    case up:
        return down;
    case down:
        return up;
    default:
        return noDirection;
    }
}

bool buildObstacle(double x0, double y0, double x1, double y1, double padding, Obstacle &obstacle) {
    if (!bounded(x0) || !bounded(y0) || !bounded(x1) || !bounded(y1) || !finite(padding))
        return false;
    obstacle = {std::min(x0, x1), std::min(y0, y1), std::max(x0, x1), std::max(y0, y1)};
    if (std::abs(obstacle.left) > kMaxCoordinate || std::abs(obstacle.right) > kMaxCoordinate ||
        std::abs(obstacle.top) > kMaxCoordinate || std::abs(obstacle.bottom) > kMaxCoordinate)
        return false;
    obstacle.left -= padding;
    obstacle.top -= padding;
    obstacle.right += padding;
    obstacle.bottom += padding;
    if (!finite(obstacle.left) || !finite(obstacle.right) || !finite(obstacle.top) || !finite(obstacle.bottom) ||
        std::abs(obstacle.left) > kMaxCoordinate || std::abs(obstacle.right) > kMaxCoordinate ||
        std::abs(obstacle.top) > kMaxCoordinate || std::abs(obstacle.bottom) > kMaxCoordinate)
        return false;
    // Degenerate symbol/line geometry still has to occupy some area so the
    // route cannot pass exactly through the obstacle center line.
    if (obstacle.right - obstacle.left < kLineHalfThickness * 2.0) {
        const double center = (obstacle.left + obstacle.right) * 0.5;
        obstacle.left = center - kLineHalfThickness;
        obstacle.right = center + kLineHalfThickness;
    }
    if (obstacle.bottom - obstacle.top < kLineHalfThickness * 2.0) {
        const double center = (obstacle.top + obstacle.bottom) * 0.5;
        obstacle.top = center - kLineHalfThickness;
        obstacle.bottom = center + kLineHalfThickness;
    }
    return true;
}

void appendExtensionObstacle(std::vector<Obstacle> &obstacles, const RoutePoint &pin, const RoutePoint &exit) {
    if (!finitePoint(pin) || !finitePoint(exit) || samePoint(pin, exit))
        return;
    Obstacle obstacle{std::min(pin.x, exit.x), std::min(pin.y, exit.y), std::max(pin.x, exit.x),
                      std::max(pin.y, exit.y)};
    // The active extension is a wall along its length. The exit end stays
    // exactly at the route endpoint so the wire can reach it, while the pin
    // end is extended slightly inward. Otherwise a route can travel along the
    // outside of the extension and terminate at the pin, which looks like it
    // is running over the destination extension.
    if (std::abs(pin.x - exit.x) <= kEpsilon) {
        obstacle.left -= kActiveExtensionHalfThickness;
        obstacle.right += kActiveExtensionHalfThickness;
        if (pin.y < exit.y)
            obstacle.top -= kExtensionPinPadding;
        else
            obstacle.bottom += kExtensionPinPadding;
    } else if (std::abs(pin.y - exit.y) <= kEpsilon) {
        obstacle.top -= kActiveExtensionHalfThickness;
        obstacle.bottom += kActiveExtensionHalfThickness;
        if (pin.x < exit.x)
            obstacle.left -= kExtensionPinPadding;
        else
            obstacle.right += kExtensionPinPadding;
    } else {
        obstacle.left -= kActiveExtensionHalfThickness;
        obstacle.top -= kActiveExtensionHalfThickness;
        obstacle.right += kActiveExtensionHalfThickness;
        obstacle.bottom += kActiveExtensionHalfThickness;
    }
    obstacles.push_back(obstacle);
}

bool containsInterior(const Obstacle &obstacle, const RoutePoint &point) {
    return point.x > obstacle.left + kEpsilon && point.x < obstacle.right - kEpsilon &&
           point.y > obstacle.top + kEpsilon && point.y < obstacle.bottom - kEpsilon;
}

bool containsAnyInterior(const std::vector<Obstacle> &obstacles, const RoutePoint &point) {
    return std::any_of(obstacles.begin(), obstacles.end(),
                       [&](const Obstacle &obstacle) { return containsInterior(obstacle, point); });
}

bool segmentBlocked(const Obstacle &obstacle, const RoutePoint &first, const RoutePoint &second) {
    if (std::abs(first.x - second.x) <= kEpsilon) {
        const double x = (first.x + second.x) * 0.5;
        if (x <= obstacle.left + kEpsilon || x >= obstacle.right - kEpsilon)
            return false;
        const double low = std::min(first.y, second.y);
        const double high = std::max(first.y, second.y);
        return std::min(high, obstacle.bottom) - std::max(low, obstacle.top) > kEpsilon;
    }
    if (std::abs(first.y - second.y) <= kEpsilon) {
        const double y = (first.y + second.y) * 0.5;
        if (y <= obstacle.top + kEpsilon || y >= obstacle.bottom - kEpsilon)
            return false;
        const double low = std::min(first.x, second.x);
        const double high = std::max(first.x, second.x);
        return std::min(high, obstacle.right) - std::max(low, obstacle.left) > kEpsilon;
    }
    // The router only creates orthogonal edges. Treat anything else as
    // blocked by the obstacle's bounding box rather than risking a crossing.
    return true;
}

bool segmentClear(const std::vector<Obstacle> &obstacles, const RoutePoint &first, const RoutePoint &second) {
    return std::none_of(obstacles.begin(), obstacles.end(),
                        [&](const Obstacle &obstacle) { return segmentBlocked(obstacle, first, second); });
}

void sortUnique(std::vector<double> &values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

void addMidpoints(std::vector<double> &values) {
    std::vector<double> result;
    result.reserve(values.size() * 2);
    for (std::size_t index = 0; index < values.size(); ++index) {
        result.push_back(values[index]);
        if (index + 1 < values.size()) {
            const double gap = values[index + 1] - values[index];
            if (gap > kEpsilon * 10.0)
                result.push_back(values[index] + gap * 0.5);
        }
    }
    values = std::move(result);
    sortUnique(values);
}

int coordinateIndex(const std::vector<double> &values, double value) {
    const auto found = std::lower_bound(values.begin(), values.end(), value);
    if (found == values.end() || *found != value)
        return -1;
    return static_cast<int>(found - values.begin());
}

bool between(double low, double middle, double high) {
    return middle >= std::min(low, high) - kEpsilon && middle <= std::max(low, high) + kEpsilon;
}

bool redundantPoint(const RoutePoint &first, const RoutePoint &middle, const RoutePoint &last) {
    const bool sameX = std::abs(first.x - middle.x) <= kEpsilon && std::abs(last.x - middle.x) <= kEpsilon;
    if (sameX)
        return between(first.y, middle.y, last.y);
    const bool sameY = std::abs(first.y - middle.y) <= kEpsilon && std::abs(last.y - middle.y) <= kEpsilon;
    if (sameY)
        return between(first.x, middle.x, last.x);
    return false;
}

void removeDuplicates(std::vector<RoutePoint> &points) {
    std::vector<RoutePoint> unique;
    unique.reserve(points.size());
    for (const auto &point : points)
        if (unique.empty() || !samePoint(unique.back(), point))
            unique.push_back(point);
    points = std::move(unique);
}

void cleanRoutePoints(std::vector<RoutePoint> &points) {
    removeDuplicates(points);
    if (points.size() < 3)
        return;
    std::vector<RoutePoint> simplified;
    simplified.reserve(points.size());
    simplified.push_back(points.front());
    for (std::size_t index = 1; index + 1 < points.size(); ++index) {
        if (redundantPoint(simplified.back(), points[index], points[index + 1]))
            continue;
        simplified.push_back(points[index]);
    }
    simplified.push_back(points.back());
    points = std::move(simplified);
}

void appendPoint(std::vector<RoutePoint> &points, const RoutePoint &point) {
    if (points.empty() || !samePoint(points.back(), point))
        points.push_back(point);
}

SchematicRouteResult fallback(const SchematicRouteRequest &request) {
    SchematicRouteResult result;
    result.points = {request.start, request.startExit, request.targetExit, request.target};
    removeDuplicates(result.points);
    return result;
}
} // namespace

SchematicRouteResult routeSchematicWire(const SchematicRouteRequest &request) {
    if (!finitePoint(request.start) || !finitePoint(request.startExit) || !finitePoint(request.target) ||
        !finitePoint(request.targetExit))
        return {};

    SchematicRouteResult result = fallback(request);
    if (samePoint(request.startExit, request.targetExit)) {
        result.routed = true;
        return result;
    }

    const double clearance = finite(request.clearance) ? std::clamp(request.clearance, 0.0, 1.0e6) : 0.0;

    std::vector<Obstacle> obstacles;
    obstacles.reserve(request.obstacleRects.size() + request.obstacleSegments.size());
    const auto addObstacle = [&](double x0, double y0, double x1, double y1, double padding) {
        Obstacle obstacle;
        if (!buildObstacle(x0, y0, x1, y1, padding, obstacle))
            return;
        // The route endpoints must remain reachable. Ignore an obstacle only
        // when its padded bounds would contain an endpoint; otherwise a pin
        // sitting just outside a symbol can make every route fail.
        if (containsInterior(obstacle, request.startExit) || containsInterior(obstacle, request.targetExit))
            return;
        obstacles.push_back(obstacle);
    };
    for (const auto &rect : request.obstacleRects)
        addObstacle(rect.left, rect.top, rect.right, rect.bottom, clearance);
    for (const auto &segment : request.obstacleSegments) {
        if (!finitePoint(segment.start) || !finitePoint(segment.end))
            continue;
        // Line obstacles are thin rectangles. The extra half thickness keeps
        // candidate route lines from lying exactly on the original segment.
        addObstacle(segment.start.x, segment.start.y, segment.end.x, segment.end.y, clearance + kLineHalfThickness);
    }
    // The two active extensions are not collision obstacles for their own
    // connection, but the routed part must not cross them either. Without this
    // the router can loop back across the start extension and overlap itself.
    appendExtensionObstacle(obstacles, request.start, request.startExit);
    appendExtensionObstacle(obstacles, request.target, request.targetExit);

    std::vector<double> xCoordinates{request.start.x, request.startExit.x, request.target.x, request.targetExit.x};
    std::vector<double> yCoordinates{request.start.y, request.startExit.y, request.target.y, request.targetExit.y};
    for (const auto &obstacle : obstacles) {
        xCoordinates.push_back(obstacle.left);
        xCoordinates.push_back(obstacle.right);
        yCoordinates.push_back(obstacle.top);
        yCoordinates.push_back(obstacle.bottom);
    }
    sortUnique(xCoordinates);
    sortUnique(yCoordinates);
    if (xCoordinates.empty() || yCoordinates.empty())
        return result;

    const double minX = xCoordinates.front();
    const double maxX = xCoordinates.back();
    const double minY = yCoordinates.front();
    const double maxY = yCoordinates.back();
    const double margin = std::max(kOuterMargin, clearance + kOuterMargin);
    xCoordinates.push_back(minX - margin);
    xCoordinates.push_back(maxX + margin);
    yCoordinates.push_back(minY - margin);
    yCoordinates.push_back(maxY + margin);
    sortUnique(xCoordinates);
    sortUnique(yCoordinates);
    addMidpoints(xCoordinates);
    addMidpoints(yCoordinates);

    const auto xCount = xCoordinates.size();
    const auto yCount = yCoordinates.size();
    if (xCount == 0 || yCount == 0 || xCount > kMaxGridCells / yCount)
        return result;
    const auto nodeCount = xCount * yCount;
    if (nodeCount == 0 || nodeCount > kMaxGridCells)
        return result;

    const int startX = coordinateIndex(xCoordinates, request.startExit.x);
    const int startY = coordinateIndex(yCoordinates, request.startExit.y);
    const int targetX = coordinateIndex(xCoordinates, request.targetExit.x);
    const int targetY = coordinateIndex(yCoordinates, request.targetExit.y);
    if (startX < 0 || startY < 0 || targetX < 0 || targetY < 0)
        return result;

    const auto xSize = static_cast<int>(xCount);
    const auto nodeOf = [xSize](int x, int y) { return static_cast<int>(y) * xSize + x; };
    const auto pointOf = [&](int node) {
        const int x = node % xSize;
        const int y = node / xSize;
        return RoutePoint{xCoordinates[static_cast<std::size_t>(x)], yCoordinates[static_cast<std::size_t>(y)]};
    };
    const int startNode = nodeOf(startX, startY);
    const int targetNode = nodeOf(targetX, targetY);

    std::vector<unsigned char> passable(nodeCount, 1);
    for (int y = 0; y < static_cast<int>(yCount); ++y) {
        for (int x = 0; x < xSize; ++x) {
            const RoutePoint point{xCoordinates[static_cast<std::size_t>(x)],
                                   yCoordinates[static_cast<std::size_t>(y)]};
            if (containsAnyInterior(obstacles, point))
                passable[static_cast<std::size_t>(nodeOf(x, y))] = 0;
        }
    }
    passable[static_cast<std::size_t>(startNode)] = 1;
    passable[static_cast<std::size_t>(targetNode)] = 1;

    const int startDirection =
        axisAligned(request.start, request.startExit) && !samePoint(request.start, request.startExit)
            ? directionOf(request.start, request.startExit)
            : noDirection;
    const int targetDirection =
        axisAligned(request.targetExit, request.target) && !samePoint(request.targetExit, request.target)
            ? directionOf(request.targetExit, request.target)
            : noDirection;

    const auto stateOf = [](int node, int direction) { return node * 4 + direction; };
    const auto stateCount = nodeCount * 4;
    std::vector<Cost> best(stateCount);
    std::vector<int> parent(stateCount, -1);
    std::priority_queue<QueueEntry, std::vector<QueueEntry>, Later> queue;
    const auto seed = [&](int direction) {
        const int state = stateOf(startNode, direction);
        best[static_cast<std::size_t>(state)] = Cost{0, 0.0};
        queue.push(QueueEntry{0, 0.0, startNode, direction});
    };
    if (startDirection == noDirection) {
        for (int direction = left; direction <= down; ++direction)
            seed(direction);
    } else {
        seed(startDirection);
    }

    const int stepX[] = {-1, 1, 0, 0};
    const int stepY[] = {0, 0, -1, 1};
    const int stepDirection[] = {left, right, up, down};
    int endState = -1;
    while (!queue.empty()) {
        const auto current = queue.top();
        queue.pop();
        const int currentState = stateOf(current.node, current.direction);
        const auto &recorded = best[static_cast<std::size_t>(currentState)];
        if (current.turns != recorded.turns || current.length > recorded.length + kEpsilon)
            continue;
        if (current.node == targetNode && (targetDirection == noDirection || current.direction == targetDirection)) {
            endState = currentState;
            break;
        }
        const int currentX = current.node % xSize;
        const int currentY = current.node / xSize;
        for (int step = 0; step < 4; ++step) {
            const int nextX = currentX + stepX[step];
            const int nextY = currentY + stepY[step];
            if (nextX < 0 || nextX >= xSize || nextY < 0 || nextY >= static_cast<int>(yCount))
                continue;
            const int nextNode = nodeOf(nextX, nextY);
            if (!passable[static_cast<std::size_t>(nextNode)])
                continue;
            const RoutePoint currentPoint = pointOf(current.node);
            const RoutePoint nextPoint = pointOf(nextNode);
            if (!segmentClear(obstacles, currentPoint, nextPoint))
                continue;
            const bool firstStep = parent[static_cast<std::size_t>(currentState)] == -1;
            const int direction = stepDirection[step];
            if (startDirection != noDirection && firstStep && direction == opposite(startDirection))
                continue;
            const bool turns = startDirection == noDirection && firstStep ? false : direction != current.direction;
            const int nextTurns = current.turns + (turns ? 1 : 0);
            const double nextLength =
                current.length + std::abs(nextPoint.x - currentPoint.x) + std::abs(nextPoint.y - currentPoint.y);
            const int nextState = stateOf(nextNode, direction);
            const Cost candidate{nextTurns, nextLength};
            const auto &previous = best[static_cast<std::size_t>(nextState)];
            if (candidate.turns > previous.turns ||
                (candidate.turns == previous.turns && candidate.length >= previous.length - kEpsilon))
                continue;
            best[static_cast<std::size_t>(nextState)] = candidate;
            parent[static_cast<std::size_t>(nextState)] = currentState;
            queue.push(QueueEntry{nextTurns, nextLength, nextNode, direction});
        }
    }

    if (endState < 0)
        return result;

    std::vector<int> nodePath;
    nodePath.reserve(32);
    std::size_t guard = 0;
    for (int state = endState; state >= 0 && guard <= stateCount; state = parent[static_cast<std::size_t>(state)]) {
        nodePath.push_back(state / 4);
        ++guard;
    }
    if (guard > stateCount)
        return result;
    std::reverse(nodePath.begin(), nodePath.end());

    std::vector<RoutePoint> routePoints;
    routePoints.reserve(nodePath.size());
    for (const int node : nodePath)
        routePoints.push_back(pointOf(node));
    cleanRoutePoints(routePoints);

    result.points.clear();
    result.points.reserve(routePoints.size() + 4);
    appendPoint(result.points, request.start);
    appendPoint(result.points, request.startExit);
    for (const auto &point : routePoints)
        appendPoint(result.points, point);
    appendPoint(result.points, request.targetExit);
    appendPoint(result.points, request.target);
    result.routed = true;
    return result;
}

} // namespace ice
