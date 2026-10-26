#include "icelib/geometry.hpp"

#include <algorithm>

namespace ice {
namespace {
Polygon rectanglePolygon(Rect rectangle) {
    return {{rectangle.x, rectangle.y},
            {rectangle.x + rectangle.width, rectangle.y},
            {rectangle.x + rectangle.width, rectangle.y + rectangle.height},
            {rectangle.x, rectangle.y + rectangle.height}};
}

Rect unionBounds(const MultiPolygon &polygons) {
    Rect result{};
    bool first = true;
    for (const auto &polygon : polygons) {
        const Rect bounds = GeometryKernel::bounds(polygon);
        if (first) {
            result = bounds;
            first = false;
        } else {
            const Coordinate x0 = std::min(result.x, bounds.x);
            const Coordinate y0 = std::min(result.y, bounds.y);
            const Coordinate x1 = std::max(result.x + result.width, bounds.x + bounds.width);
            const Coordinate y1 = std::max(result.y + result.height, bounds.y + bounds.height);
            result = {x0, y0, x1 - x0, y1 - y0};
        }
    }
    return result;
}
} // namespace

// NOTE: Bounding-box approximation. Replace with the Clipper2 backend once the
// Clipper2 dependency is wired into the build; the public GeometryKernel API
// already isolates callers from the backend.
MultiPolygon GeometryKernel::booleanOp(const MultiPolygon &subject, const MultiPolygon &clip,
                                       BooleanOperation operation) const {
    if (subject.empty())
        return operation == BooleanOperation::union_ ? clip : MultiPolygon{};
    if (clip.empty())
        return operation == BooleanOperation::difference || operation == BooleanOperation::union_ ? subject
                                                                                                  : MultiPolygon{};
    const auto a = unionBounds(subject);
    const auto b = unionBounds(clip);
    Rect overlap{std::max(a.x, b.x), std::max(a.y, b.y), std::min(a.x + a.width, b.x + b.width) - std::max(a.x, b.x),
                 std::min(a.y + a.height, b.y + b.height) - std::max(a.y, b.y)};
    if (operation == BooleanOperation::intersection)
        return overlap.valid() ? MultiPolygon{rectanglePolygon(overlap)} : MultiPolygon{};
    if (operation == BooleanOperation::union_) {
        Rect united{std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x + a.width, b.x + b.width) - std::min(a.x, b.x),
                    std::max(a.y + a.height, b.y + b.height) - std::min(a.y, b.y)};
        return {rectanglePolygon(united)};
    }

    if (!overlap.valid())
        return subject;
    if (operation == BooleanOperation::xor_)
        return {};
    return {};
}

MultiPolygon Clipper2Adapter::unionPolygons(const MultiPolygon &left, const MultiPolygon &right) const {
    return booleanOp(left, right, BooleanOperation::union_);
}
MultiPolygon Clipper2Adapter::difference(const MultiPolygon &left, const MultiPolygon &right) const {
    return booleanOp(left, right, BooleanOperation::difference);
}
MultiPolygon Clipper2Adapter::intersection(const MultiPolygon &left, const MultiPolygon &right) const {
    return booleanOp(left, right, BooleanOperation::intersection);
}
MultiPolygon Clipper2Adapter::xorPolygons(const MultiPolygon &left, const MultiPolygon &right) const {
    return booleanOp(left, right, BooleanOperation::xor_);
}

MultiPolygon GeometryKernel::offset(const MultiPolygon &subject, Coordinate delta) const {
    MultiPolygon result;
    for (const auto &polygon : subject) {
        auto bounds = GeometryKernel::bounds(polygon);
        bounds.x -= delta;
        bounds.y -= delta;
        bounds.width += 2 * delta;
        bounds.height += 2 * delta;
        if (bounds.valid())
            result.push_back(rectanglePolygon(bounds));
    }
    return result;
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

std::string booleanOperationName(BooleanOperation operation) {
    switch (operation) {
    case BooleanOperation::union_:
        return "union";
    case BooleanOperation::difference:
        return "difference";
    case BooleanOperation::intersection:
        return "intersection";
    case BooleanOperation::xor_:
        return "xor";
    }
    return "unknown";
}
} // namespace ice
