#include "icelib/geometry.hpp"

#include "clipper2/clipper.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <set>

namespace ice {
namespace {
using ClipperPath = Clipper2Lib::Path64;
using ClipperPaths = Clipper2Lib::Paths64;

ClipperPath toClipperPath(const Polygon &polygon) {
    ClipperPath path;
    path.reserve(polygon.size());
    for (const auto &point : polygon)
        path.emplace_back(point.x, point.y);
    // Shape polygons are filled outlines, so normalize their winding. Mixed
    // windings would otherwise cancel each other under the NonZero fill rule.
    if (!path.empty() && !Clipper2Lib::IsPositive(path))
        std::reverse(path.begin(), path.end());
    return path;
}

ClipperPaths toClipperPaths(const MultiPolygon &polygons) {
    ClipperPaths paths;
    paths.reserve(polygons.size());
    for (const auto &polygon : polygons) {
        if (polygon.size() < 3)
            continue;
        auto path = toClipperPath(polygon);
        if (path.size() >= 3)
            paths.push_back(std::move(path));
    }
    return paths;
}

Polygon fromClipperPath(const ClipperPath &path) {
    Polygon polygon;
    polygon.reserve(path.size());
    for (const auto &point : path)
        polygon.push_back({point.x, point.y});
    return polygon;
}

void collectContours(const Clipper2Lib::PolyPath64 &node, ClipperPaths &paths) {
    for (const auto &child : node) {
        paths.push_back(child->Polygon());
        collectContours(*child, paths);
    }
}

// A shape stores one filled outline rather than an outer contour plus holes.
// When a boolean result contains holes, split the result into vertical slabs
// at every vertex x coordinate. Each fragment is then a simple, hole-free
// polygon and the fragments together reproduce the exact result.
MultiPolygon decomposeHoledResult(const Clipper2Lib::PolyTree64 &tree) {
    ClipperPaths contours;
    collectContours(tree, contours);
    if (contours.empty())
        return {};

    Coordinate minY = std::numeric_limits<Coordinate>::max();
    Coordinate maxY = std::numeric_limits<Coordinate>::lowest();
    std::set<Coordinate> columns;
    for (const auto &path : contours) {
        for (const auto &point : path) {
            minY = std::min(minY, point.y);
            maxY = std::max(maxY, point.y);
            columns.insert(point.x);
        }
    }
    if (columns.size() < 2 || maxY <= minY)
        return {};

    const auto lower = minY > std::numeric_limits<Coordinate>::lowest() ? minY - 1 : minY;
    const auto upper = maxY < std::numeric_limits<Coordinate>::max() ? maxY + 1 : maxY;
    MultiPolygon result;
    for (auto it = columns.begin(); std::next(it) != columns.end(); ++it) {
        const auto left = *it;
        const auto right = *std::next(it);
        if (right <= left)
            continue;
        ClipperPath slab{{left, lower}, {right, lower}, {right, upper}, {left, upper}};
        auto fragments = Clipper2Lib::BooleanOp(Clipper2Lib::ClipType::Intersection, Clipper2Lib::FillRule::NonZero,
                                                contours, ClipperPaths{slab});
        for (const auto &fragment : fragments) {
            if (fragment.size() < 3 || Clipper2Lib::Area(fragment) <= 0.0)
                continue;
            auto polygon = fromClipperPath(fragment);
            if (GeometryKernel::valid(polygon))
                result.push_back(std::move(polygon));
        }
    }
    GeometryKernel kernel;
    return kernel.cleanup(result);
}
} // namespace

MultiPolygon GeometryKernel::booleanOp(const MultiPolygon &subject, const MultiPolygon &clip,
                                       BooleanOperation operation) const {
    auto subjects = toClipperPaths(subject);
    auto clips = toClipperPaths(clip);
    if (subjects.empty() && clips.empty())
        return {};
    if (subjects.empty()) {
        if (operation != BooleanOperation::union_ && operation != BooleanOperation::xor_)
            return {};
        subjects = std::move(clips);
        clips.clear();
    }
    if (operation == BooleanOperation::intersection && clips.empty())
        return {};

    Clipper2Lib::Clipper64 clipper;
    clipper.AddSubject(subjects);
    if (!clips.empty())
        clipper.AddClip(clips);

    Clipper2Lib::ClipType clipType = Clipper2Lib::ClipType::Union;
    switch (operation) {
    case BooleanOperation::union_:
        clipType = Clipper2Lib::ClipType::Union;
        break;
    case BooleanOperation::difference:
        clipType = Clipper2Lib::ClipType::Difference;
        break;
    case BooleanOperation::intersection:
        clipType = Clipper2Lib::ClipType::Intersection;
        break;
    case BooleanOperation::xor_:
        clipType = Clipper2Lib::ClipType::Xor;
        break;
    }

    Clipper2Lib::PolyTree64 tree;
    clipper.Execute(clipType, Clipper2Lib::FillRule::NonZero, tree);

    bool hasHoles = false;
    for (const auto &child : tree) {
        if (child->Count() > 0) {
            hasHoles = true;
            break;
        }
    }
    if (hasHoles)
        return decomposeHoledResult(tree);

    MultiPolygon result;
    for (const auto &child : tree) {
        auto polygon = fromClipperPath(child->Polygon());
        if (valid(polygon))
            result.push_back(std::move(polygon));
    }
    return cleanup(result);
}

MultiPolygon Clipper2Adapter::difference(const MultiPolygon &left, const MultiPolygon &right) const {
    return booleanOp(left, right, BooleanOperation::difference);
}
MultiPolygon Clipper2Adapter::intersection(const MultiPolygon &left, const MultiPolygon &right) const {
    return booleanOp(left, right, BooleanOperation::intersection);
}

MultiPolygon GeometryKernel::offset(const MultiPolygon &subject, Coordinate delta) const {
    const auto paths = toClipperPaths(subject);
    if (paths.empty())
        return {};
    const auto inflated = Clipper2Lib::InflatePaths(paths, static_cast<double>(delta), Clipper2Lib::JoinType::Miter,
                                                    Clipper2Lib::EndType::Polygon);
    MultiPolygon result;
    result.reserve(inflated.size());
    for (const auto &path : inflated) {
        auto polygon = fromClipperPath(path);
        if (valid(polygon))
            result.push_back(std::move(polygon));
    }
    return cleanup(result);
}

MultiPolygon GeometryKernel::fillEvenOdd(const MultiPolygon &contours) const {
    const auto subjects = toClipperPaths(contours);
    if (subjects.empty())
        return {};
    Clipper2Lib::Clipper64 clipper;
    clipper.AddSubject(subjects);
    Clipper2Lib::PolyTree64 tree;
    clipper.Execute(Clipper2Lib::ClipType::Union, Clipper2Lib::FillRule::EvenOdd, tree);
    bool hasHoles = false;
    for (const auto &child : tree) {
        if (child->Count() > 0) {
            hasHoles = true;
            break;
        }
    }
    if (hasHoles)
        return decomposeHoledResult(tree);
    MultiPolygon result;
    for (const auto &child : tree) {
        auto polygon = fromClipperPath(child->Polygon());
        if (valid(polygon))
            result.push_back(std::move(polygon));
    }
    return cleanup(result);
}

MultiPolygon GeometryKernel::cleanup(const MultiPolygon &subject) const {
    MultiPolygon result;
    for (const auto &polygon : subject) {
        Polygon cleaned;
        for (const auto &point : polygon) {
            if (cleaned.empty() || cleaned.back() != point)
                cleaned.push_back(point);
        }
        if (cleaned.size() > 1 && cleaned.front() == cleaned.back())
            cleaned.pop_back();
        if (valid(cleaned))
            result.push_back(std::move(cleaned));
    }
    return result;
}

bool GeometryKernel::valid(const Polygon &polygon) {
    if (polygon.size() < 3)
        return false;
    // Use extended precision to avoid int64 overflow on large coordinates.
    long double area = 0.0L;
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        const auto &current = polygon[i];
        const auto &next = polygon[(i + 1) % polygon.size()];
        area += static_cast<long double>(current.x) * next.y - static_cast<long double>(next.x) * current.y;
    }
    return area != 0.0L;
}

Rect GeometryKernel::bounds(const Polygon &polygon) {
    if (polygon.empty())
        return {};
    Coordinate minX = polygon.front().x;
    Coordinate maxX = minX;
    Coordinate minY = polygon.front().y;
    Coordinate maxY = minY;
    for (const auto &point : polygon) {
        minX = std::min(minX, point.x);
        maxX = std::max(maxX, point.x);
        minY = std::min(minY, point.y);
        maxY = std::max(maxY, point.y);
    }
    return {minX, minY, maxX - minX, maxY - minY};
}

Point Transform::apply(Point point) const {
    Coordinate x = point.x;
    Coordinate y = point.y;
    switch (orientation) {
    case Orientation::r0:
        break;
    case Orientation::r90:
        std::swap(x, y);
        x = -x;
        break;
    case Orientation::r180:
        x = -x;
        y = -y;
        break;
    case Orientation::r270:
        std::swap(x, y);
        y = -y;
        break;
    case Orientation::mx:
        y = -y;
        break;
    case Orientation::my:
        x = -x;
        break;
    case Orientation::m45:
        std::swap(x, y);
        break;
    case Orientation::m135:
        std::swap(x, y);
        x = -x;
        y = -y;
        break;
    }
    return {x + this->x, y + this->y};
}

MultiPolygon transformArray(const MultiPolygon &geometry, const Transform &transform, const ArraySpec &array) {
    MultiPolygon result;
    for (std::uint32_t row = 0; row < std::max(1u, array.rows); ++row) {
        for (std::uint32_t column = 0; column < std::max(1u, array.columns); ++column) {
            Transform item = transform;
            item.x += static_cast<Coordinate>(column) * array.columnStep;
            item.y += static_cast<Coordinate>(row) * array.rowStep;
            for (const auto &polygon : geometry) {
                Polygon transformed;
                for (const auto &point : polygon)
                    transformed.push_back(item.apply(point));
                result.push_back(std::move(transformed));
            }
        }
    }
    return result;
}

void SpatialIndex::clear() { entries_.clear(); }
void SpatialIndex::insert(ObjectId id, Rect bounds) {
    remove(id);
    entries_.push_back({id, bounds});
}
void SpatialIndex::remove(ObjectId id) {
    entries_.erase(
        std::remove_if(entries_.begin(), entries_.end(), [id](const Entry &entry) { return entry.id == id; }),
        entries_.end());
}
std::vector<ObjectId> SpatialIndex::query(Rect bounds) const {
    std::vector<ObjectId> result;
    for (const auto &entry : entries_) {
        const bool separated =
            entry.bounds.x + entry.bounds.width <= bounds.x || bounds.x + bounds.width <= entry.bounds.x ||
            entry.bounds.y + entry.bounds.height <= bounds.y || bounds.y + bounds.height <= entry.bounds.y;
        if (!separated)
            result.push_back(entry.id);
    }
    return result;
}

} // namespace ice
