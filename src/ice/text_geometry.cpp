#include "text_geometry.hpp"

#include <QFont>
#include <QPainterPath>
#include <QPolygonF>
#include <QTransform>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <numbers>
#include <utility>
#include <vector>

namespace ice {
namespace {

constexpr double kPointEpsilon = 1.0e-6;

double cross(const QPointF &left, const QPointF &right) { return left.x() * right.y() - left.y() * right.x(); }

double length(const QPointF &point) { return std::hypot(point.x(), point.y()); }

QPointF snappedDirection(const QPointF &delta) {
    static const QPointF directions[8] = {{1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}};
    const auto angle = std::atan2(delta.y(), delta.x());
    auto index = static_cast<int>(std::llround(angle / (std::numbers::pi / 4.0)));
    index = (index % 8 + 8) % 8;
    return directions[index];
}

bool samePoint(const QPointF &left, const QPointF &right) {
    return std::fabs(left.x() - right.x()) <= kPointEpsilon && std::fabs(left.y() - right.y()) <= kPointEpsilon;
}

Point octilinearDirection(Coordinate dx, Coordinate dy) {
    if (dx == 0 && dy == 0)
        return {0, 0};
    const auto angle = std::atan2(static_cast<double>(dy), static_cast<double>(dx));
    auto index = static_cast<int>(std::llround(angle / (std::numbers::pi / 4.0)));
    index = (index % 8 + 8) % 8;
    static const Point directions[8] = {{1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}};
    return directions[index];
}

bool exactOctilinear(const Point &start, const Point &end) {
    const auto dx = end.x - start.x;
    const auto dy = end.y - start.y;
    return (dx == 0) != (dy == 0) || (dx != 0 && std::llabs(dx) == std::llabs(dy));
}

Polygon snapIntegerPolygon(const Polygon &source) {
    if (source.size() < 3)
        return {};
    Polygon result;
    result.reserve(source.size() + 2);
    result.push_back(source.front());
    auto current = source.front();
    for (std::size_t index = 1; index < source.size(); ++index) {
        const auto target = source[index];
        if (target.x == current.x && target.y == current.y)
            continue;
        const auto direction = octilinearDirection(target.x - current.x, target.y - current.y);
        if (direction.x == 0 && direction.y == 0)
            continue;
        const auto vx = target.x - current.x;
        const auto vy = target.y - current.y;
        const auto denominator = direction.x * direction.x + direction.y * direction.y;
        const auto step = std::max<Coordinate>(
            1, static_cast<Coordinate>(std::llround(static_cast<double>(vx * direction.x + vy * direction.y) /
                                                    static_cast<double>(denominator))));
        current = {current.x + step * direction.x, current.y + step * direction.y};
        if (current.x != result.back().x || current.y != result.back().y)
            result.push_back(current);
    }

    // The implicit closing edge must be octilinear too. If it is not, insert
    // one corner and finish with two allowed directions.
    const auto start = result.front();
    const auto dx = start.x - current.x;
    const auto dy = start.y - current.y;
    if ((dx != 0 || dy != 0) && !exactOctilinear(current, start)) {
        const auto sx = dx > 0 ? 1 : -1;
        const auto sy = dy > 0 ? 1 : -1;
        Point corner = current;
        if (std::llabs(dx) >= std::llabs(dy))
            corner.x += sx * (std::llabs(dx) - std::llabs(dy));
        else
            corner.y += sy * (std::llabs(dy) - std::llabs(dx));
        if ((corner.x != result.back().x || corner.y != result.back().y) &&
            (corner.x != start.x || corner.y != start.y))
            result.push_back(corner);
    }

    if (result.size() < 3)
        return {};
    Polygon cleaned;
    cleaned.reserve(result.size());
    for (const auto &point : result) {
        if (cleaned.empty() || point.x != cleaned.back().x || point.y != cleaned.back().y)
            cleaned.push_back(point);
    }
    if (cleaned.size() > 1 && cleaned.front().x == cleaned.back().x && cleaned.front().y == cleaned.back().y)
        cleaned.pop_back();
    if (cleaned.size() < 3)
        return {};
    for (std::size_t index = 0; index < cleaned.size(); ++index) {
        if (!exactOctilinear(cleaned[index], cleaned[(index + 1) % cleaned.size()]))
            return {};
    }
    return cleaned;
}

void simplifyStripe(const std::vector<QPointF> &points, int first, int last, double tolerance, std::vector<int> &keep) {
    if (last <= first + 1)
        return;
    const auto start = points[static_cast<std::size_t>(first)];
    const auto end = points[static_cast<std::size_t>(last)];
    const auto span = end - start;
    const auto spanLength = length(span);
    double maximum = -1.0;
    int selected = -1;
    for (int index = first + 1; index < last; ++index) {
        const auto offset = points[static_cast<std::size_t>(index)] - start;
        const auto distance = spanLength < kPointEpsilon ? length(offset) : std::fabs(cross(offset, span)) / spanLength;
        if (distance > maximum) {
            maximum = distance;
            selected = index;
        }
    }
    if (maximum <= tolerance || selected < 0)
        return;
    simplifyStripe(points, first, selected, tolerance, keep);
    keep.push_back(selected);
    simplifyStripe(points, selected, last, tolerance, keep);
}

struct DirectionRun {
    QPointF anchor;
    QPointF direction;
    double weight = 0.0;
};

std::vector<QPointF> octilinearContour(const std::vector<QPointF> &source, double tolerance) {
    std::vector<QPointF> points;
    points.reserve(source.size());
    for (const auto &point : source) {
        if (points.empty() || !samePoint(point, points.back()))
            points.push_back(point);
    }
    if (points.size() > 1 && samePoint(points.front(), points.back()))
        points.pop_back();
    if (points.size() < 3)
        return {};

    std::vector<int> keep{0};
    simplifyStripe(points, 0, static_cast<int>(points.size()) - 1, tolerance, keep);
    keep.push_back(static_cast<int>(points.size()) - 1);
    std::sort(keep.begin(), keep.end());
    keep.erase(std::unique(keep.begin(), keep.end()), keep.end());

    std::vector<QPointF> simplified;
    simplified.reserve(keep.size());
    for (const auto index : keep)
        simplified.push_back(points[static_cast<std::size_t>(index)]);
    if (simplified.size() < 3)
        return {};

    std::vector<DirectionRun> runs;
    for (std::size_t index = 0; index < simplified.size(); ++index) {
        const auto start = simplified[index];
        const auto end = simplified[(index + 1) % simplified.size()];
        const auto delta = end - start;
        const auto segmentLength = length(delta);
        if (segmentLength < kPointEpsilon)
            continue;
        const auto direction = snappedDirection(delta);
        if (!runs.empty() && samePoint(runs.back().direction, direction)) {
            auto &run = runs.back();
            run.anchor = (run.anchor * run.weight + (start + end) * 0.5 * segmentLength) / (run.weight + segmentLength);
            run.weight += segmentLength;
        } else {
            runs.push_back({(start + end) * 0.5, direction, segmentLength});
        }
    }
    if (runs.size() > 1 && samePoint(runs.front().direction, runs.back().direction)) {
        const auto last = runs.back();
        runs.pop_back();
        auto &first = runs.front();
        first.anchor = (first.anchor * first.weight + last.anchor * last.weight) / (first.weight + last.weight);
        first.weight += last.weight;
    }
    if (runs.size() < 3)
        return {};

    std::vector<QPointF> result;
    result.reserve(runs.size());
    for (std::size_t index = 0; index < runs.size(); ++index) {
        const auto &first = runs[index];
        const auto &second = runs[(index + 1) % runs.size()];
        const auto denominator = cross(first.direction, second.direction);
        QPointF corner;
        if (std::fabs(denominator) < kPointEpsilon) {
            // Opposite or coincident directions represent a retrace. Use the
            // midpoint of the two runs so the contour stays closed.
            corner = (first.anchor + second.anchor) * 0.5;
        } else {
            const auto parameter = cross(second.anchor - first.anchor, second.direction) / denominator;
            corner = first.anchor + first.direction * parameter;
        }
        corner.setX(std::round(corner.x()));
        corner.setY(std::round(corner.y()));
        result.push_back(corner);
    }

    std::vector<QPointF> cleaned;
    cleaned.reserve(result.size());
    for (const auto &point : result) {
        if (cleaned.empty() || !samePoint(point, cleaned.back()))
            cleaned.push_back(point);
    }
    if (cleaned.size() > 1 && samePoint(cleaned.front(), cleaned.back()))
        cleaned.pop_back();
    return cleaned;
}

} // namespace

MultiPolygon textPolygonGeometry(const QString &text, int pixelSize, QPointF lowerLeft) {
    if (text.isEmpty() || pixelSize <= 0)
        return {};

    QFont font;
    font.setPixelSize(pixelSize);
    QPainterPath glyphs;
    glyphs.addText(0, 0, font, text);
    if (glyphs.isEmpty())
        return {};

    QTransform toLayout(1, 0, 0, -1, 0, 0);
    auto placed = toLayout.map(glyphs);
    const auto bounds = placed.boundingRect();
    QTransform placement(1, 0, 0, 1, lowerLeft.x() - bounds.left(), lowerLeft.y() - bounds.top());
    placed = placement.map(placed);

    const auto tolerance = std::max(1.0, static_cast<double>(pixelSize) * 0.01);
    QPainterPath contours;
    contours.setFillRule(Qt::OddEvenFill);
    for (const auto &subpath : placed.toSubpathPolygons()) {
        std::vector<QPointF> source;
        source.reserve(static_cast<std::size_t>(subpath.size()));
        for (const auto &point : subpath)
            source.push_back(point);
        const auto contour = octilinearContour(source, tolerance);
        if (contour.size() < 3)
            continue;
        QPolygonF polygon;
        polygon.reserve(static_cast<int>(contour.size()));
        for (const auto &point : contour)
            polygon.push_back(QPointF(std::round(point.x()), std::round(point.y())));
        contours.addPolygon(polygon);
    }
    if (contours.isEmpty())
        return {};

    MultiPolygon result;
    for (const auto &filled : contours.toFillPolygons()) {
        Polygon polygon;
        polygon.reserve(static_cast<std::size_t>(filled.size()));
        for (const auto &point : filled)
            polygon.push_back(
                {static_cast<Coordinate>(std::llround(point.x())), static_cast<Coordinate>(std::llround(point.y()))});
        auto snapped = snapIntegerPolygon(polygon);
        if (GeometryKernel::valid(snapped))
            result.push_back(std::move(snapped));
    }
    return result;
}

} // namespace ice
