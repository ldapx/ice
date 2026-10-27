#pragma once

#include "icelib/core.hpp"
#include "icelib/layout_library.hpp"
#include "ui_helpers.hpp"

#include <QGraphicsView>
#include <QPointF>
#include <QPolygonF>

#include <functional>
#include <map>
#include <string>
#include <vector>

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

class LayoutCanvas : public QGraphicsView {
  public:
    enum class Tool { select, path, rectangle, placeDevice };

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
                              std::function<void(std::vector<ice::ObjectId>)> deleted,
                              std::function<void()> changed) = 0;
    virtual void setLayerStyles(const std::map<ice::ObjectId, LayerStyle> &styles) = 0;
    virtual void fitToContent() = 0;

    virtual void setPathConfig(const LayoutPathConfig &config) = 0;
    virtual void setPathWidth(int width) = 0;
    virtual int pathWidth() const = 0;
    virtual bool pathAvailable() const = 0;
    virtual std::string pathLayerName() const = 0;
    virtual void setPathSegmentCallback(LayoutPathSegmentCallback callback) = 0;
    virtual void setPathLayerChangedCallback(LayoutPathLayerChangedCallback callback) = 0;
};

// Builds the rounded-rectangle polygon for one path segment. Points are in
// scene coordinates (positive Y downwards).
QPolygonF layoutPathPolygon(QPointF start, QPointF end, qreal width);

LayoutCanvas *createLayoutCanvas(QWidget *parent = nullptr);
