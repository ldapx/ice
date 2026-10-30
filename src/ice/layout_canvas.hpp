#pragma once

#include "icelib/core.hpp"
#include "icelib/layout_library.hpp"
#include "ui_helpers.hpp"

#include <QColor>
#include <QGraphicsView>
#include <QPointF>
#include <QPolygonF>

#include <functional>
#include <map>
#include <string>
#include <vector>

class QGraphicsItem;
class QPainter;
class QRectF;
class QLineEdit;

struct LayoutPathLayer {
    ice::ObjectId layer = 0;
    ice::ObjectId viaToNext = 0;
    std::string name;
};

struct LayoutPathConfig {
    std::vector<LayoutPathLayer> layers;
    int width = 200;
    ice::ObjectId cell = 0;
};

using LayoutPathSegmentCallback = std::function<void(QPointF start, QPointF end, int width, ice::ObjectId layer,
                                                     const std::vector<ice::ObjectId> &viaLayers)>;
using LayoutPathLayerChangedCallback = std::function<void(ice::ObjectId layer, const std::string &name)>;
using LayoutPathPointMovedCallback = std::function<void(ice::ObjectId id, int index, QPointF position)>;
using LayoutPathPointInsertedCallback = std::function<void(ice::ObjectId id, int index, QPointF position)>;
using LayoutPathPointDeletedCallback = std::function<void(ice::ObjectId id, int index)>;
using LayoutPathFinishedCallback = std::function<void()>;

class LayoutCanvas : public QGraphicsView {
  public:
    enum class Tool { select, path, rectangle, placeDevice, ruler };

    explicit LayoutCanvas(QWidget *parent = nullptr);
    ~LayoutCanvas() override;

    virtual void setTool(Tool tool) = 0;
    virtual void setReadOnly(bool readOnly) = 0;
    virtual bool isReadOnly() const = 0;
    virtual Tool currentTool() const = 0;
    virtual void setGridVisible(bool visible) = 0;
    virtual void setSnapToGrid(bool enabled) = 0;
    virtual void setGridSpacing(qreal spacing) = 0;
    virtual void setDevicePreview(const ice::LayoutDeviceCell *cell) = 0;
    virtual void restoreLayoutInstanceSelection() = 0;
    virtual void setDevicePlacementCallback(std::function<void(QPointF)> callback) = 0;
    virtual void setInstanceActivatedCallback(std::function<void(ice::ObjectId)> callback) = 0;
    virtual void setInstanceSelectionCallback(std::function<void(ice::ObjectId)> callback) = 0;
    virtual ice::ObjectId selectedLayoutInstance() const = 0;
    virtual void setToolEscapeCallback(std::function<void()> callback) = 0;
    virtual void deleteSelectedShapes() = 0;
    virtual void setCallbacks(std::function<void(QPointF, QPointF)> rectangleCreated,
                              std::function<void(ice::ObjectId, QPointF)> shapeMoved,
                              std::function<void(ice::ObjectId, QRectF)> shapeResized,
                              std::function<void(std::vector<ice::ObjectId>)> deleted,
                              std::function<void()> changed) = 0;
    virtual void setLayerStyles(const std::map<ice::ObjectId, LayerStyle> &styles) = 0;
    virtual void setRectangleStyle(const LayerStyle &style) = 0;
    virtual void fitToContent() = 0;

    virtual void setPathConfig(const LayoutPathConfig &config) = 0;
    virtual void setPathWidth(int width) = 0;
    virtual int pathWidth() const = 0;
    virtual bool pathAvailable() const = 0;
    virtual std::string pathLayerName() const = 0;
    virtual void setPathSegmentCallback(LayoutPathSegmentCallback callback) = 0;
    virtual void setPathLayerChangedCallback(LayoutPathLayerChangedCallback callback) = 0;
    virtual void setPathFinishedCallback(LayoutPathFinishedCallback callback) = 0;
};

// Builds the rounded-rectangle polygon for one path segment. Points are in
// scene coordinates (positive Y downwards).
QPolygonF layoutPathPolygon(QPointF start, QPointF end, qreal width);

// Creates the single selectable item used for a routed path. Points are in
// scene coordinates. Handles become visible while selected; dragging a handle
// calls pointMoved, double-clicking a segment calls pointInserted, and
// right-clicking a handle offers point deletion.
QGraphicsItem *createLayoutPathItem(ice::ObjectId id, const std::vector<QPointF> &points, qreal width,
                                    const std::vector<QColor> &segmentColors,
                                    const std::vector<std::vector<QColor>> &pointViaColors, bool readOnly,
                                    LayoutPathPointMovedCallback pointMoved,
                                    LayoutPathPointInsertedCallback pointInserted,
                                    LayoutPathPointDeletedCallback pointDeleted);

// Creates the selectable/movable canvas box used for a planned device. The
// rectangle is given in scene coordinates and the item is positioned at its
// top-left corner so drag deltas map directly onto layout coordinates.
QGraphicsItem *createLayoutPlannedDeviceItem(ice::ObjectId id, const QString &name, const QRectF &rect, bool readOnly);

LayoutCanvas *createLayoutCanvas(QWidget *parent = nullptr);
