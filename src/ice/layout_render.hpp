#pragma once

#include "icelib/core.hpp"
#include "icelib/layout_library.hpp"

#include "ui_helpers.hpp"

#include <QColor>
#include <QLineF>
#include <QPainter>
#include <QPointF>
#include <QPolygonF>
#include <QRectF>
#include <QString>

#include <map>
#include <utility>
#include <vector>

namespace ice {

struct LayoutPreviewLayer {
    ObjectId id = 0;
    int number = 0;
    int datatype = 0;
};

struct LayoutPreviewShape {
    enum class Kind { polygon, path, text };
    LayoutPreviewLayer layer;
    Kind kind = Kind::polygon;
    QPolygonF polygon;
    std::vector<QPointF> centerline;
    qreal width = 0.0;
    std::vector<ObjectId> segmentLayers;
    std::vector<std::vector<ObjectId>> pointVias;
    QRectF textRectangle;
    QString text;
};

struct LayoutPreviewPort {
    LayoutPreviewLayer layer;
    QRectF rectangle;
    QString label;
};

struct LayoutPreviewPlannedDevice {
    QRectF rectangle;
    QString name;
};

struct LayoutPreviewConnection {
    QLineF line;
    QString net;
};

struct LayoutPreviewModel {
    QRectF bounds;
    std::vector<LayoutPreviewShape> shapes;
    std::vector<LayoutPreviewPort> ports;
    std::vector<LayoutPreviewPlannedDevice> plannedDevices;
    std::vector<LayoutPreviewConnection> plannedConnections;

    bool empty() const;
};

// Builds a flattened, project-independent render model. Layer ids remain
// project ids so the caller can apply the same LayerStyle map used by the
// interactive canvas.
LayoutPreviewModel buildLayoutCellPreviewModel(const Project &project, const LayoutCell &cell,
                                               bool includeInstances = true, bool includePorts = true,
                                               bool includePlannedDevices = true,
                                               const std::vector<Transform> &rootTransform = {});
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

} // namespace ice
