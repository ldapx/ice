#pragma once

#include "openvlsilib/core.hpp"
#include "openvlsilib/layout_library.hpp"

#include "ui_helpers.hpp"

#include <QColor>
#include <QLineF>
#include <QPainter>
#include <QPointF>
#include <QPolygonF>
#include <QRectF>
#include <QString>

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace openvlsi {

struct LayoutPreviewLayer {
    ObjectId id = 0;
    int number = 0;
    int datatype = 0;
    QString name;
};

// Stable key used by preview display controls. ODB layers use their layer name;
// project-only layers fall back to object/number/datatype identity.
QString layoutPreviewLayerKey(const LayoutPreviewLayer &layer);

struct LayoutPreviewShape {
    enum class Kind { polygon, path, text };
    LayoutPreviewLayer layer;
    Kind kind = Kind::polygon;
    QPolygonF polygon;
    std::vector<QPointF> centerline;
    qreal width = 0.0;
    std::vector<qreal> segmentWidths;
    std::vector<ObjectId> segmentLayers;
    std::vector<std::vector<ObjectId>> pointVias;
    QRectF textRectangle;
    QString text;
    QString net;
    int netId = 0;
    bool special = false;
};

struct LayoutPreviewPort {
    LayoutPreviewLayer layer;
    QRectF rectangle;
    QString label;
    QString net;
    int netId = 0;
    // True for intrinsic OpenVLSI ports with no process-layer association.
    bool intrinsic = false;
};

struct LayoutPreviewPlannedDevice {
    QRectF rectangle;
    QString name;
    int id = 0;
    QString master;
    QString category;
    QString module;
    QString orientation;
};

struct LayoutPreviewNet {
    int id = 0;
    QString name;
    QString type;
    bool special = false;
    quint64 termCount = 0;
    QStringList terms;
};

struct LayoutPreviewModule {
    int id = 0;
    QString name;
    int parentId = 0;
    quint64 instanceCount = 0;
};

struct LayoutPreviewMarker {
    QString type;
    QString layer;
    QRectF rectangle;
    QString sources;
    QString comment;
};

struct LayoutPreviewGuide {
    QString net;
    QString layer;
    QRectF rectangle;
    bool congested = false;
};

struct LayoutPreviewRow {
    QRectF rectangle;
    QString site;
};

struct LayoutPreviewBlockage {
    QRectF rectangle;
    QString type;
};

struct LayoutPreviewConnection {
    QLineF line;
    QString net;
};

enum class LayoutHeatMapKind { none, placement_density, routing_congestion };

struct LayoutPreviewHeatMap {
    QRectF bounds;
    int columns = 0;
    int rows = 0;
    std::vector<double> values;
    double minimum = 0.0;
    double maximum = 0.0;
};

struct LayoutPreviewModel {
    QRectF bounds;
    std::vector<LayoutPreviewShape> shapes;
    std::vector<LayoutPreviewPort> ports;
    std::vector<LayoutPreviewPlannedDevice> plannedDevices;
    std::vector<LayoutPreviewConnection> plannedConnections;
    std::vector<LayoutPreviewNet> nets;
    std::vector<LayoutPreviewModule> modules;
    std::vector<LayoutPreviewMarker> markers;
    std::vector<LayoutPreviewGuide> guides;
    std::vector<LayoutPreviewRow> rows;
    std::vector<LayoutPreviewBlockage> blockages;
    std::optional<LayoutPreviewHeatMap> placementDensity;
    std::optional<LayoutPreviewHeatMap> routingCongestion;
    // The ODB reader may cap the number of emitted shapes.
    bool truncated = false;

    bool empty() const;
};

// Resolves a referenced GDS/OASIS child into a flattened preview model. The
// UI supplies this because reference loading is a GUI concern.
using LayoutReferencePreviewResolver =
    std::function<std::optional<LayoutPreviewModel>(const LayoutCell &referenceCell)>;

// Builds a flattened, project-independent render model. Layer ids remain
// project ids so the caller can apply the same LayerStyle map used by the
// interactive canvas.
// When includeInstanceDevices is set, each top-level placed instance is also
// emitted as a preview device with its master name and category. This lets
// previews that were built from an imported DEF populate the same instance
// display controls as an ODB preview. includeNets copies the root cell's nets
// into the model and tags shapes with their net ids.
LayoutPreviewModel buildLayoutCellPreviewModel(const Project &project, const LayoutCell &cell,
                                               bool includeInstances = true, bool includePorts = true,
                                               bool includePlannedDevices = true,
                                               const std::vector<Transform> &rootTransform = {},
                                               const LayoutReferencePreviewResolver &referenceResolver = {},
                                               bool includeInstanceDevices = false, bool includeNets = false);
LayoutPreviewModel buildLayoutDevicePreviewModel(const LayoutDeviceCell &device);

struct LayoutPaintOptions {
    bool showPatterns = true;
    bool showText = true;
    bool showPaths = true;
    bool showPorts = true;
    bool showPlannedDevices = true;
    bool showLabels = true;
    qreal shapeAlpha = 110.0;
    qreal portAlpha = 45.0;
    qreal outlineWidth = 0.8;
    // Keys produced by layoutPreviewLayerKey(). Empty means every layer visible.
    std::set<QString> hiddenLayers;
    std::set<QString> hiddenInstanceCategories;
    std::set<QString> hiddenNetTypes;
    bool showSpecialNets = true;
    bool showGuides = false;
    bool showRows = false;
    bool showBlockages = false;
};

class LayoutRenderer {
  public:
    using LayerStyleMap = std::map<ObjectId, LayerStyle>;
    using DeviceStyleMap = std::map<std::pair<int, int>, LayerStyle>;

    using PaintOptions = LayoutPaintOptions;

    // Resolves device-library layers to the same styles their placed project
    // layers use, falling back to the stable palette for unplaced layers.
    static DeviceStyleMap deviceStylesFor(const Project &project, const LayoutDeviceLibrary &library,
                                          const LayerStyleMap &styles);
    static void paintFitted(QPainter &painter, const LayoutPreviewModel &model, const LayerStyleMap &styles,
                            const DeviceStyleMap &deviceStyles, const QRectF &target,
                            const PaintOptions &options = LayoutPaintOptions{});
    static void paintAt(QPainter &painter, const LayoutPreviewModel &model, const LayerStyleMap &styles,
                        const DeviceStyleMap &deviceStyles, QPointF center,
                        const PaintOptions &options = LayoutPaintOptions{});
    static void paintAtOrigin(QPainter &painter, const LayoutPreviewModel &model, const LayerStyleMap &styles,
                              const DeviceStyleMap &deviceStyles, QPointF origin,
                              const PaintOptions &options = LayoutPaintOptions{});
};

} // namespace openvlsi
