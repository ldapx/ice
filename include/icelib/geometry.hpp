#pragma once

#include "icelib/core.hpp"

#include <optional>
#include <vector>

namespace ice {

struct Point {
    Coordinate x = 0;
    Coordinate y = 0;
    bool operator==(const Point &) const = default;
};

using Polygon = std::vector<Point>;
using MultiPolygon = std::vector<Polygon>;

enum class BooleanOperation { union_, difference, intersection, xor_ };

class GeometryKernel {
  public:
    MultiPolygon booleanOp(const MultiPolygon &subject, const MultiPolygon &clip, BooleanOperation operation) const;
    MultiPolygon offset(const MultiPolygon &subject, Coordinate delta) const;
    MultiPolygon cleanup(const MultiPolygon &subject) const;
    static bool valid(const Polygon &polygon);
    static Rect bounds(const Polygon &polygon);
};

class Clipper2Adapter final : public GeometryKernel {
  public:
    MultiPolygon unionPolygons(const MultiPolygon &left, const MultiPolygon &right) const;
    MultiPolygon difference(const MultiPolygon &left, const MultiPolygon &right) const;
    MultiPolygon intersection(const MultiPolygon &left, const MultiPolygon &right) const;
    MultiPolygon xorPolygons(const MultiPolygon &left, const MultiPolygon &right) const;
};

struct Transform {
    Coordinate x = 0;
    Coordinate y = 0;
    Orientation orientation = Orientation::r0;
    Point apply(Point point) const;
};

struct ArraySpec {
    std::uint32_t columns = 1;
    std::uint32_t rows = 1;
    Coordinate columnStep = 0;
    Coordinate rowStep = 0;
};

class SpatialIndex {
  public:
    void clear();
    void insert(ObjectId id, Rect bounds);
    void remove(ObjectId id);
    std::vector<ObjectId> query(Rect bounds) const;

  private:
    struct Entry {
        ObjectId id;
        Rect bounds;
    };
    std::vector<Entry> entries_;
};

MultiPolygon transformArray(const MultiPolygon &geometry, const Transform &transform, const ArraySpec &array);
std::string booleanOperationName(BooleanOperation operation);

} // namespace ice
