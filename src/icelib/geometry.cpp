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
} // namespace

MultiPolygon GeometryKernel::booleanOp(const MultiPolygon& subject, const MultiPolygon& clip,
                                       BooleanOperation operation) const {
    if (subject.empty()) return operation == BooleanOperation::union_ ? clip : MultiPolygon{};
    if (clip.empty()) return operation == BooleanOperation::difference || operation == BooleanOperation::union_
                                 ? subject
                                 : MultiPolygon{};
    auto a = GeometryKernel::bounds(subject.front());
    auto b = GeometryKernel::bounds(clip.front());
    Rect overlap{std::max(a.x, b.x), std::max(a.y, b.y),
                 std::min(a.x + a.width, b.x + b.width) - std::max(a.x, b.x),
                 std::min(a.y + a.height, b.y + b.height) - std::max(a.y, b.y)};
    if (operation == BooleanOperation::intersection) return overlap.valid() ? MultiPolygon{rectanglePolygon(overlap)} : MultiPolygon{};
    if (operation == BooleanOperation::union_) {
        Rect united{std::min(a.x, b.x), std::min(a.y, b.y),
                    std::max(a.x + a.width, b.x + b.width) - std::min(a.x, b.x),
                    std::max(a.y + a.height, b.y + b.height) - std::min(a.y, b.y)};
        return {rectanglePolygon(united)};
    }

    if (!overlap.valid()) return subject;
    if (operation == BooleanOperation::xor_) return {};
    return {};
}

MultiPolygon Clipper2Adapter::unionPolygons(const MultiPolygon& left, const MultiPolygon& right) const {
    return booleanOp(left, right, BooleanOperation::union_);
}
MultiPolygon Clipper2Adapter::difference(const MultiPolygon& left, const MultiPolygon& right) const {
    return booleanOp(left, right, BooleanOperation::difference);
}
MultiPolygon Clipper2Adapter::intersection(const MultiPolygon& left, const MultiPolygon& right) const {
    return booleanOp(left, right, BooleanOperation::intersection);
}
MultiPolygon Clipper2Adapter::xorPolygons(const MultiPolygon& left, const MultiPolygon& right) const {
    return booleanOp(left, right, BooleanOperation::xor_);
}

MultiPolygon GeometryKernel::offset(const MultiPolygon& subject, Coordinate delta) const {
    MultiPolygon result;
    for (const auto& polygon : subject) {
        auto bounds = GeometryKernel::bounds(polygon);
        bounds.x -= delta;
        bounds.y -= delta;
        bounds.width += 2 * delta;
        bounds.height += 2 * delta;
        if (bounds.valid()) result.push_back(rectanglePolygon(bounds));
    }
    return result;
}

MultiPolygon GeometryKernel::cleanup(const MultiPolygon& subject) const {
    MultiPolygon result;
    for (const auto& polygon : subject) {
        Polygon cleaned;
        for (const auto& point : polygon) {
            if (cleaned.empty() || cleaned.back() != point) cleaned.push_back(point);
        }
        if (cleaned.size() > 1 && cleaned.front() == cleaned.back()) cleaned.pop_back();
        if (valid(cleaned)) result.push_back(std::move(cleaned));
    }
    return result;
}

bool GeometryKernel::valid(const Polygon& polygon) {
    if (polygon.size() < 3) return false;
    Coordinate area = 0;
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        const auto& current = polygon[i];
        const auto& next = polygon[(i + 1) % polygon.size()];
        area += current.x * next.y - next.x * current.y;
    }
    return area != 0;
}

Rect GeometryKernel::bounds(const Polygon& polygon) {
    if (polygon.empty()) return {};
    Coordinate minX = polygon.front().x;
    Coordinate maxX = minX;
    Coordinate minY = polygon.front().y;
    Coordinate maxY = minY;
    for (const auto& point : polygon) {
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
    case Orientation::r0: break;
    case Orientation::r90: std::swap(x, y); y = -point.x; break;
    case Orientation::r180: x = -x; y = -y; break;
    case Orientation::r270: std::swap(x, y); x = -point.y; break;
    case Orientation::mx: y = -y; break;
    case Orientation::my: x = -x; break;
    }
    return {x + this->x, y + this->y};
}

MultiPolygon transformArray(const MultiPolygon& geometry, const Transform& transform, const ArraySpec& array) {
    MultiPolygon result;
    for (std::uint32_t row = 0; row < std::max(1u, array.rows); ++row) {
        for (std::uint32_t column = 0; column < std::max(1u, array.columns); ++column) {
            Transform item = transform;
            item.x += static_cast<Coordinate>(column) * array.columnStep;
            item.y += static_cast<Coordinate>(row) * array.rowStep;
            for (const auto& polygon : geometry) {
                Polygon transformed;
                for (const auto& point : polygon) transformed.push_back(item.apply(point));
                result.push_back(std::move(transformed));
            }
        }
    }
    return result;
}

void SpatialIndex::clear() { entries_.clear(); }
void SpatialIndex::insert(ObjectId id, Rect bounds) { remove(id); entries_.push_back({id, bounds}); }
void SpatialIndex::remove(ObjectId id) {
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [id](const Entry& entry) { return entry.id == id; }),
                   entries_.end());
}
std::vector<ObjectId> SpatialIndex::query(Rect bounds) const {
    std::vector<ObjectId> result;
    for (const auto& entry : entries_) {
        const bool separated = entry.bounds.x + entry.bounds.width <= bounds.x ||
                               bounds.x + bounds.width <= entry.bounds.x ||
                               entry.bounds.y + entry.bounds.height <= bounds.y ||
                               bounds.y + bounds.height <= entry.bounds.y;
        if (!separated) result.push_back(entry.id);
    }
    return result;
}

std::string booleanOperationName(BooleanOperation operation) {
    switch (operation) {
    case BooleanOperation::union_: return "union";
    case BooleanOperation::difference: return "difference";
    case BooleanOperation::intersection: return "intersection";
    case BooleanOperation::xor_: return "xor";
    }
    return "unknown";
}
} // namespace ice
